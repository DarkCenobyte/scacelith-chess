// The motion of a captured mouse that reports positions only (src/platform/absolute_mouse.h; the
// Win32 layer feeds it the absolute raw input packets of a capture, Remote Desktop or a tablet).
#include "test.h"
#include "platform/absolute_mouse.h"
#include <cmath>

using plat::AbsoluteMouse;

TEST(absolute_mouse_steps_between_positions) {
    AbsoluteMouse a;
    float dx = 0.0f, dy = 0.0f;
    CHECK(!a.active());
    // The first position of a capture is the reference: no motion.
    a.position(500.0f, 300.0f, dx, dy);
    CHECK_EQ(dx, 0.0f);
    CHECK_EQ(dy, 0.0f);
    CHECK(a.active());
    a.position(510.0f, 296.0f, dx, dy);
    a.position(513.5f, 290.0f, dx, dy);
    CHECK_EQ(dx, 13.5f);
    CHECK_EQ(dy, -10.0f);
    // A new capture starts from its own first position.
    a.reset();
    CHECK(!a.active());
    dx = dy = 0.0f;
    a.position(0.0f, 0.0f, dx, dy);
    CHECK_EQ(dx, 0.0f);
    CHECK_EQ(dy, 0.0f);
}

TEST(absolute_mouse_bounds_jumps) {
    // A pen lifted and set down across the screen: one bounded step, then the motion goes on from
    // where it landed.
    AbsoluteMouse a;
    float dx = 0.0f, dy = 0.0f;
    a.position(100.0f, 900.0f, dx, dy);
    a.position(1800.0f, 50.0f, dx, dy);
    CHECK_EQ(dx, AbsoluteMouse::kMaxStep);
    CHECK_EQ(dy, -AbsoluteMouse::kMaxStep);
    a.position(1805.0f, 52.0f, dx, dy);
    CHECK_EQ(dx, AbsoluteMouse::kMaxStep + 5.0f);
    CHECK_EQ(dy, -AbsoluteMouse::kMaxStep + 2.0f);
}

TEST(absolute_mouse_leaves_relative_mice_alone) {
    // A relative mouse sends no position: nothing changes, and the Win32 layer re-centres it every
    // frame as before.
    AbsoluteMouse a;
    a.relativeMotion();
    CHECK(!a.active());
    // Positions that come with relative motion in the same capture (another device, a driver that
    // sends both) are left out: the motion stays what the relative packets report.
    float dx = 3.0f, dy = -2.0f;
    a.position(400.0f, 400.0f, dx, dy);
    a.position(450.0f, 380.0f, dx, dy);
    CHECK_EQ(dx, 3.0f);
    CHECK_EQ(dy, -2.0f);
    CHECK(!a.active());
    // Even after positions: the first relative motion ends them for the capture.
    AbsoluteMouse b;
    dx = dy = 0.0f;
    b.position(10.0f, 10.0f, dx, dy);
    b.position(20.0f, 10.0f, dx, dy);
    b.relativeMotion();
    CHECK(!b.active());
    b.position(60.0f, 10.0f, dx, dy);
    CHECK_EQ(dx, 10.0f);
}

TEST(absolute_mouse_normalised_coordinates) {
    // RAWMOUSE's absolute coordinates go from 0 to 65535 across the screen (or the virtual desktop).
    CHECK_EQ(AbsoluteMouse::pixels(0, 1920), 0.0f);
    CHECK_EQ(AbsoluteMouse::pixels(65535, 1920), 1920.0f);
    CHECK(std::fabs(AbsoluteMouse::pixels(32768, 1080) - 540.0f) < 0.01f);
    // One pixel of a 3840 pixel wide desktop is about 17 units.
    CHECK(std::fabs(AbsoluteMouse::pixels(17, 3840) - 0.996f) < 0.01f);
}

TEST(absolute_mouse_skips_packets_without_position) {
    // A wheel notch (leaning) or a button during the look, from a mouse that reports positions:
    // the packet has no coordinates, and is no jump to the top-left corner and back.
    AbsoluteMouse a;
    float dx = 0.0f, dy = 0.0f;
    a.packet(32768, 32768, 1920, 1080, dx, dy);
    a.packet(0, 0, 1920, 1080, dx, dy);
    CHECK_EQ(dx, 0.0f);
    CHECK_EQ(dy, 0.0f);
    a.packet(32768 + 341, 32768 - 121, 1920, 1080, dx, dy);
    CHECK(std::fabs(dx - 10.0f) < 0.05f);
    CHECK(std::fabs(dy + 2.0f) < 0.05f);
    // Before any position, it is not the reference either.
    AbsoluteMouse b;
    dx = dy = 0.0f;
    b.packet(0, 0, 1920, 1080, dx, dy);
    CHECK(!b.active());
    b.packet(65535, 65535, 1920, 1080, dx, dy);
    CHECK_EQ(dx, 0.0f);
    CHECK_EQ(dy, 0.0f);
}
