// Where the borderless fullscreen window goes (src/platform/borderless_layout.h): a strip beyond
// one edge of its monitor, so that Windows composes every frame and screen captures are current.
#include "test.h"
#include "platform/borderless_layout.h"

using plat::BorderlessLayout;
using plat::MonitorArea;
using plat::ScreenRect;
using plat::kSpill;

namespace {
// The picture's rectangle on the desktop: the monitor's part of the client area.
ScreenRect picture(const BorderlessLayout& l) {
    int x = l.window.left + l.viewX, y = l.window.top + l.viewY;
    return {x, y, x + l.window.width() - l.spillX, y + l.window.height() - l.spillY};
}
}  // namespace

TEST(borderless_layout_single_monitor_spills_right) {
    MonitorArea m{{0, 0, 1920, 1080}, 144};
    BorderlessLayout l = plat::borderlessLayout(m, {m});
    // Never exactly the monitor's rectangle: that is what Windows takes for a fullscreen game.
    CHECK(!(l.window == m.rect));
    CHECK(l.window == (ScreenRect{0, 0, 1920 + kSpill, 1080}));
    CHECK_EQ(l.spillX, kSpill);
    CHECK_EQ(l.spillY, 0);
    // The picture starts at the client area's corner: the renderer needs no offset.
    CHECK_EQ(l.viewX, 0);
    CHECK_EQ(l.viewY, 0);
    CHECK(picture(l) == m.rect);
}

TEST(borderless_layout_avoids_the_neighbouring_monitor) {
    // Two monitors side by side, the game on the left one: the strip goes above, off every screen.
    MonitorArea left{{0, 0, 2560, 1440}, 144}, right{{2560, 0, 4480, 1080}, 60};
    BorderlessLayout l = plat::borderlessLayout(left, {left, right});
    CHECK(l.window == (ScreenRect{0, -kSpill, 2560, 1440}));
    CHECK_EQ(l.spillX, 0);
    CHECK_EQ(l.spillY, kSpill);
    // The picture is lower in the client area (the pointer's coordinates move), still at its
    // bottom left corner, OpenGL's origin.
    CHECK_EQ(l.viewX, 0);
    CHECK_EQ(l.viewY, kSpill);
    CHECK(picture(l) == left.rect);
    // On the right one, the right edge is free.
    l = plat::borderlessLayout(right, {left, right});
    CHECK(l.window == (ScreenRect{2560, 0, 4480 + kSpill, 1080}));
    CHECK(picture(l) == right.rect);
    // A monitor to the right that only starts lower than the game's monitor ends: not in the way.
    MonitorArea below{{2560, 1440, 4480, 2520}, 60};
    l = plat::borderlessLayout(left, {left, below});
    CHECK_EQ(l.spillX, kSpill);
}

TEST(borderless_layout_monitors_on_both_sides_take_the_closest_refresh_rate) {
    // Monitors to the right and above: the strip goes over the one whose refresh rate is the
    // game monitor's, or closest to it.
    MonitorArea game{{0, 0, 1920, 1080}, 144};
    MonitorArea right{{1920, 0, 3840, 1080}, 60};
    MonitorArea above{{0, -1080, 1920, 0}, 144};
    BorderlessLayout l = plat::borderlessLayout(game, {game, right, above});
    CHECK_EQ(l.spillY, kSpill);
    CHECK_EQ(l.spillX, 0);
    above.hz = 60;
    right.hz = 144;
    l = plat::borderlessLayout(game, {game, right, above});
    CHECK_EQ(l.spillX, kSpill);
    // A tie keeps the right edge (no pointer offset).
    above.hz = 144;
    l = plat::borderlessLayout(game, {game, right, above});
    CHECK_EQ(l.spillX, kSpill);
    CHECK(picture(l) == game.rect);
}

TEST(borderless_layout_negative_desktop_coordinates) {
    // A secondary monitor left of and above the primary one has negative coordinates.
    MonitorArea primary{{0, 0, 1920, 1080}, 60}, secondary{{-1920, -300, 0, 780}, 60};
    BorderlessLayout l = plat::borderlessLayout(secondary, {primary, secondary});
    // Its right edge touches the primary monitor; its top edge is free.
    CHECK(l.window == (ScreenRect{-1920, -300 - kSpill, 0, 780}));
    CHECK(picture(l) == secondary.rect);
    // The list may leave the game's monitor out.
    l = plat::borderlessLayout(primary, {secondary});
    CHECK(l.window == (ScreenRect{0, 0, 1920 + kSpill, 1080}));
}
