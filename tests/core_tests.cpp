#include "test.h"
#include "math/math.h"
#include "core/image.h"
#include "core/ini.h"
#include "net/crypto.h"
#include "net/net_sys.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

TEST(math_quat_roundtrip) {
    m::quat q = m::axisAngle(m::vec3(0.3f, 1.0f, -0.2f), 1.1f);
    m::quat r = m::fromMat3(m::toMat3(q));
    CHECK(std::fabs(std::fabs(m::dot(q, r)) - 1.0f) < 1e-5f);
}

TEST(math_inverse) {
    m::mat4 a = m::translate(m::vec3(1, 2, 3)) * m::rotateY(0.7f) * m::scale(m::vec3(2, 1, 0.5f));
    m::mat4 i = m::inverse(a) * a;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) CHECK(std::fabs(i.c[c][r] - (c == r ? 1.0f : 0.0f)) < 1e-4f);
}

TEST(ini_roundtrip) {
    IniFile a;
    a.setInt("display.width", 1920);
    a.setBool("audio.ambience", false);
    CHECK(a.save("/tmp/scacelith_ini_test.ini"));
    IniFile b;
    CHECK(b.load("/tmp/scacelith_ini_test.ini"));
    CHECK_EQ(b.getInt("display.width"), 1920);
    CHECK_EQ(b.getBool("audio.ambience", true), false);
}

// A CR or LF in a value (a custom server's category id) stays on its line: it cannot add keys
// that override earlier ones when the file is read back.
TEST(ini_value_newlines_stay_on_one_line) {
    IniFile a;
    a.set("online.host", "h");
    a.set("online.category", "5+3\nhost = attacker.example\r\n[display]\nwidth = 7");
    CHECK(a.save("/tmp/scacelith_ini_lines.ini"));
    IniFile b;
    CHECK(b.load("/tmp/scacelith_ini_lines.ini"));
    CHECK_EQ(b.getString("online.host"), std::string("h"));
    CHECK_EQ(b.getString("online.category"), std::string("5+3 host = attacker.example  [display] width = 7"));
    CHECK(!b.has("display.width"));
    CHECK_EQ(a.getString("online.category"), std::string("5+3\nhost = attacker.example\r\n[display]\nwidth = 7"));
    std::remove("/tmp/scacelith_ini_lines.ini");
}

#ifndef _WIN32
// save() writes a new file and renames it over the old one, which is never truncated in place (a
// crash midway would leave it empty): a reader that opened the old file still reads it whole.
TEST(ini_save_replaces_the_file) {
    const std::string path = "/tmp/scacelith_ini_replace.ini", tmp = path + ".tmp";
    rmdir(tmp.c_str());
    IniFile a;
    a.setInt("display.width", 1920);
    CHECK(a.save(path));
    std::ifstream before(path);
    a.setInt("display.width", 1280);
    CHECK(a.save(path));
    std::string old((std::istreambuf_iterator<char>(before)), std::istreambuf_iterator<char>());
    CHECK(old.find("width = 1920") != std::string::npos);
    CHECK(!net::sys::fileExists(tmp));
    IniFile b;
    CHECK(b.load(path));
    CHECK_EQ(b.getInt("display.width"), 1280);
    // No new file can be made there (here a folder named path.tmp): written in place, as before.
    CHECK(mkdir(tmp.c_str(), 0755) == 0);
    a.setInt("display.width", 800);
    CHECK(a.save(path));
    CHECK(b.load(path));
    CHECK_EQ(b.getInt("display.width"), 800);
    CHECK(rmdir(tmp.c_str()) == 0);   // left as it was
    std::remove(path.c_str());
}
#endif

// Every byte of the PNG writer's output. Its deflate blocks hold 65535 bytes: 28x771 RGB fills
// exactly one, 1x16384 RGB overflows it by one byte; all-0xFF pixels are Adler-32's worst case.
TEST(image_png_bytes) {
    struct Case { int w, h, ch, fill; const char* sha; };
    const Case cases[] = {
        {7, 3, 3, -1, "6dbb0a77308d3aebdc6514d0ba87fa6934fbb73075e08cde0d29de2ac53c8818"},
        {300, 200, 3, -1, "73e9a79701068da41448e4d0a99b78fb77758599c21439ef6a9974c2a40d816b"},
        {129, 77, 4, -1, "6443f018d226c8da5f08543d9d45cd09c706d598687e5b5b33af1510632d144d"},
        {0, 0, 3, -1, "906dd47a9e7e9d78e9d45b4d527a0b6d75d57d5913293408c34ae208b2259899"},
        {28, 771, 3, -1, "5288d0ec1034bf9d581caff5beb5d6ee0687536cb925d927f372de580f64ef7c"},
        {1, 16384, 3, -1, "0ed95e73673cce70ce2cd99eead5b66f61d6e3d6bc913bc5f611dbee018eb98c"},
        {1000, 200, 4, 255, "fcc5134c68170922b63c383aedcb29ce6486492807f5e07d0370f3b6e2f3052c"},
    };
    const std::string path = "/tmp/scacelith_png_test.png";
    for (const Case& c : cases) {
        std::vector<uint8_t> px(size_t(c.w) * size_t(c.h) * size_t(c.ch) + 1);
        for (size_t i = 0; i < px.size(); ++i) px[i] = c.fill >= 0 ? uint8_t(c.fill) : uint8_t(i * 31 + (i >> 7) * 7);
        CHECK(image::writePNG(path, c.w, c.h, c.ch, px.data()));
        std::string bytes;
        CHECK(net::sys::readFile(path, bytes, size_t(1) << 24));
        CHECK_EQ(net::crypto::hex(net::crypto::sha256(bytes)), std::string(c.sha));
    }
    std::remove(path.c_str());
}

#ifndef _WIN32
// A failed write (a full disk: /dev/full) is reported and leaves no file behind.
TEST(image_png_write_error) {
    if (access("/dev/full", W_OK) != 0) return;
    const char* link = "/tmp/scacelith_png_full.png";
    std::vector<uint8_t> px(300 * 200 * 3, 7);
    unlink(link);
    CHECK(symlink("/dev/full", link) == 0);
    CHECK(!image::writePNG(link, 7, 3, 3, px.data()));       // buffered: the error comes at fclose
    CHECK(access(link, F_OK) != 0);                          // removed (the link, not the device)
    unlink(link);
    CHECK(symlink("/dev/full", link) == 0);
    CHECK(!image::writePNG(link, 300, 200, 3, px.data()));   // the IDAT write itself fails
    CHECK(access(link, F_OK) != 0);
    unlink(link);
}
#endif
