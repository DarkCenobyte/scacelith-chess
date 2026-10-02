#include "test.h"
#include "math/math.h"
#include "core/image.h"
#include "core/ini.h"
#include "net/crypto.h"
#include "net/net_sys.h"

#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

// A file under /tmp named after this process too: several test runs can share /tmp.
std::string tmpFile(const char* name, const char* ext) {
#ifdef _WIN32
    const unsigned pid = unsigned(_getpid());
#else
    const unsigned pid = unsigned(getpid());
#endif
    return std::string("/tmp/") + name + "_" + std::to_string(pid) + ext;
}

}  // namespace

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
    const std::string path = tmpFile("scacelith_ini_lines", ".ini");
    CHECK(a.save(path));
    IniFile b;
    CHECK(b.load(path));
    CHECK_EQ(b.getString("online.host"), std::string("h"));
    CHECK_EQ(b.getString("online.category"), std::string("5+3 host = attacker.example  [display] width = 7"));
    CHECK(!b.has("display.width"));
    CHECK_EQ(a.getString("online.category"), std::string("5+3\nhost = attacker.example\r\n[display]\nwidth = 7"));
    std::remove(path.c_str());
}

// Numbers that are not finite read as the default (a NaN render scale passes std::clamp).
TEST(ini_floats_not_finite) {
    IniFile a;
    a.set("a.nan", "nan");
    a.set("a.inf", "-inf");
    a.set("a.huge", "1e999");
    a.set("a.text", "x");
    a.set("a.ok", "1.5");
    CHECK_EQ(a.getFloat("a.nan", 1.0f), 1.0f);
    CHECK_EQ(a.getFloat("a.inf", 1.0f), 1.0f);
    CHECK_EQ(a.getFloat("a.huge", 1.0f), 1.0f);
    CHECK_EQ(a.getFloat("a.text", 1.0f), 1.0f);
    CHECK_EQ(a.getFloat("a.ok", 1.0f), 1.5f);
}

#ifndef _WIN32
// save() writes a new file and renames it over the old one, which is never truncated in place (a
// crash midway would leave it empty): a reader that opened the old file still reads it whole.
TEST(ini_save_replaces_the_file) {
    const std::string path = tmpFile("scacelith_ini_replace", ".ini"), tmp = path + ".tmp";
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

// A read-only settings file: save() cannot write it (Settings::save writes its fallback copy) and
// writable() says so without writing, so that Settings::load reads that copy back.
TEST(ini_writable_read_only_file) {
    const std::string path = tmpFile("scacelith_ini_writable", ".ini");
    IniFile a;
    a.setInt("display.width", 1280);
    CHECK(a.save(path));
    CHECK(IniFile::writable(path));
    // No file yet, in a folder that takes new files: save() makes it.
    const std::string missing = tmpFile("scacelith_ini_missing", ".ini");
    std::remove(missing.c_str());
    CHECK(IniFile::writable(missing));
    CHECK(a.save(missing));
    std::remove(missing.c_str());
    // In a folder that does not exist: no.
    CHECK(!IniFile::writable(tmpFile("scacelith_ini_nowhere", "") + "/Scacelith.ini"));
#ifdef _WIN32
    namespace fs = std::filesystem;
    const fs::perms w = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
    fs::permissions(fs::u8path(path), w, fs::perm_options::remove);   // the read-only attribute
    CHECK(!IniFile::writable(path));
    fs::permissions(fs::u8path(path), w, fs::perm_options::add);
    CHECK(IniFile::writable(path));
#else
    // Read-only, in a folder that takes no new file: save() fails there, and writable() says no
    // (root, whom permissions do not stop, gets yes from both).
    const std::string dir = tmpFile("scacelith_ini_folder", ""), file = dir + "/Scacelith.ini";
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    CHECK(a.save(file));
    CHECK(chmod(file.c_str(), 0444) == 0 && chmod(dir.c_str(), 0555) == 0);
    bool answer = IniFile::writable(file);
    CHECK_EQ(answer, a.save(file));
    if (geteuid() != 0) CHECK(!answer);
    // No new file there either.
    const std::string other = dir + "/Other.ini";
    answer = IniFile::writable(other);
    CHECK_EQ(answer, a.save(other));
    if (geteuid() != 0) CHECK(!answer);
    std::remove(other.c_str());
    // In a folder that takes new files, save()'s rename replaces a read-only file: yes.
    CHECK(chmod(dir.c_str(), 0755) == 0 && chmod(file.c_str(), 0444) == 0);
    answer = IniFile::writable(file);
    CHECK(answer);
    CHECK_EQ(answer, a.save(file));
    std::remove(file.c_str());
    rmdir(dir.c_str());
#endif
    // Asking wrote nothing: the file is as save() made it.
    IniFile b;
    CHECK(b.load(path));
    CHECK_EQ(b.getInt("display.width"), 1280);
    std::remove(path.c_str());
}

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
    const std::string path = tmpFile("scacelith_png_test", ".png");
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
    const std::string path = tmpFile("scacelith_png_full", ".png");
    const char* link = path.c_str();
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

// Out-of-range integers saturate on every platform, as on Windows where long is 32-bit: never
// wrapped modulo 2^32 to a plausible value (4294969216 is 1920 + 2^32).
TEST(ini_int_out_of_range) {
    IniFile a;
    a.set("display.width", "4294969216");
    a.set("display.height", "-99999999999");
    a.set("online.api_port", "4294967739");
    a.set("engine.elo", "2147483647");
    CHECK_EQ(a.getInt("display.width"), INT_MAX);
    CHECK_EQ(a.getInt("display.height"), INT_MIN);
    CHECK_EQ(a.getInt("online.api_port"), INT_MAX);
    CHECK_EQ(a.getInt("engine.elo"), INT_MAX);
}

// Booleans are recognised in any case (a hand-edited "True"); anything else is still false.
TEST(ini_bool_any_case) {
    IniFile a;
    a.set("a.t1", "True");
    a.set("a.t2", "YES");
    a.set("a.t3", "On");
    a.set("a.t4", "1");
    a.set("a.f1", "FALSE");
    a.set("a.f2", "off");
    a.set("a.f3", "bogus");
    a.set("a.f4", "true ; comment");
    CHECK(a.getBool("a.t1"));
    CHECK(a.getBool("a.t2"));
    CHECK(a.getBool("a.t3"));
    CHECK(a.getBool("a.t4"));
    CHECK(!a.getBool("a.f1", true));
    CHECK(!a.getBool("a.f2", true));
    CHECK(!a.getBool("a.f3", true));
    CHECK(!a.getBool("a.f4", true));
    CHECK(a.getBool("a.missing", true));
    CHECK_EQ(a.getString("a.t1"), std::string("True"));
}

// A file saved by an editor as "UTF-8 with BOM" keeps its first section; a BOM elsewhere is text.
TEST(ini_utf8_bom) {
    const std::string path = net::sys::exeDirectory() + "ini-test-bom.ini";
    std::FILE* f = net::sys::openFile(path, "wb");
    CHECK(f != nullptr);
    if (!f) return;
    std::fputs("\xEF\xBB\xBF[display]\nwidth = 1920\n[graphics]\n\xEF\xBB\xBFquality = 2\n", f);
    std::fclose(f);
    IniFile a;
    CHECK(a.load(path));
    CHECK_EQ(a.getInt("display.width", -1), 1920);
    CHECK(!a.has("width"));
    CHECK(!a.has("graphics.quality"));
    CHECK_EQ(a.getInt("graphics.\xEF\xBB\xBFquality", -1), 2);
    net::sys::removeFile(path);
}

// The settings file may sit in a folder with any name: its UTF-8 path is the file's real name
// whatever the code page (on Windows the ANSI one mangles "\xC3\x89checs" unless the process runs
// in UTF-8). net::sys opens UTF-8 paths with the wide API.
TEST(ini_utf8_path) {
    const std::string path = net::sys::exeDirectory() + "ini-test-\xC3\x89" "checs.ini";
    IniFile a;
    a.setInt("display.width", 1280);
    CHECK(a.save(path));
    CHECK(net::sys::fileExists(path));
    IniFile b;
    CHECK(b.load(path));
    CHECK_EQ(b.getInt("display.width"), 1280);
    CHECK(net::sys::removeFile(path));
}

// A PNG that cannot be written whole is reported (a --shot run then fails), and no truncated file
// is left at the path.
TEST(image_png_write_failure_reported) {
    std::vector<uint8_t> px(64 * 64 * 3, 128);  // about 12 KB of PNG
    CHECK(!image::writePNG("/tmp/scacelith-no-such-folder/shot.png", 64, 64, 3, px.data()));
#ifndef _WIN32
    const std::string path = "/tmp/scacelith_png_test_" + std::to_string(getpid()) + ".png";
    CHECK(image::writePNG(path, 64, 64, 3, px.data()));
    // A short write, as on a full disk: the file size limit stops the file at 4 KB.
    rlimit old;
    getrlimit(RLIMIT_FSIZE, &old);
    rlimit low = old;
    low.rlim_cur = 4096;
    void (*prev)(int) = std::signal(SIGXFSZ, SIG_IGN);
    setrlimit(RLIMIT_FSIZE, &low);
    bool written = image::writePNG(path, 64, 64, 3, px.data());
    setrlimit(RLIMIT_FSIZE, &old);
    std::signal(SIGXFSZ, prev);
    CHECK(!written);
    FILE* f = std::fopen(path.c_str(), "rb");
    CHECK(!f);
    if (f) std::fclose(f);
    std::remove(path.c_str());
#endif
}
