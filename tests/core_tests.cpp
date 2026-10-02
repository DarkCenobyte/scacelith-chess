#include "test.h"
#include "math/math.h"
#include "core/image.h"
#include "core/ini.h"
#include <cstdio>
#include <string>
#include <vector>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
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
