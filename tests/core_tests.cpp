#include "test.h"
#include "math/math.h"
#include "core/ini.h"

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
