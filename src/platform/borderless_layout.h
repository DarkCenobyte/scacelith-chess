// Internal to the Win32 layer: where the borderless fullscreen window goes. Header-only, for the
// unit tests.
//
// A window whose rectangle is exactly its monitor's is taken for a fullscreen game by Windows and
// its OpenGL drivers: its frames go straight to the screen, and the desktop compositor (DWM) keeps
// the copy of the window it made the last time it composed it. Everything that reads that copy
// then shows an old frame: the screen capture of Win+Shift+S and the Snipping Tool (the menu, or
// the frame of the previous capture, is what freezes on screen and what gets saved), the taskbar
// preview, and windows that should show on top. The borderless window therefore reaches kSpill
// pixels beyond one edge of its monitor (no longer a fullscreen window to the drivers, every frame
// goes through the compositor), and a window region cuts that strip off, so that none of it shows
// on a neighbouring monitor. Qt (setHasBorderInFullScreen), GTK and Godot work around the same
// behaviour with a border or an oversized window.
//
// The strip goes to the right or above, the two sides where the picture needs no offset: the
// renderer draws from the bottom left corner of the client area (OpenGL's origin), and only the
// pointer's coordinates move when the strip is above. It goes where no other monitor is, so that
// the window stays on one monitor (its refresh rate, its graphics adapter); with monitors on both
// sides, the one whose refresh rate is closest to this monitor's.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <vector>

namespace plat {

struct ScreenRect {
    int left = 0, top = 0, right = 0, bottom = 0;  // virtual screen pixels, right and bottom excluded
    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool operator==(const ScreenRect& o) const {
        return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
    }
    bool intersects(const ScreenRect& o) const {
        return left < o.right && o.left < right && top < o.bottom && o.top < bottom;
    }
};

struct MonitorArea {
    ScreenRect rect;
    int hz = 0;  // refresh rate, 0 when unknown
};

struct BorderlessLayout {
    ScreenRect window;               // the window's rectangle
    int viewX = 0, viewY = 0;        // the monitor's top left corner in the client area
    int spillX = 0, spillY = 0;      // the client area's size beyond the monitor's
};

constexpr int kSpill = 2;

// 'monitors': every monitor of the desktop ('screen' among them or not).
inline BorderlessLayout borderlessLayout(const MonitorArea& screen, const std::vector<MonitorArea>& monitors) {
    const ScreenRect& s = screen.rect;
    const ScreenRect right{s.right, s.top, s.right + kSpill, s.bottom};
    const ScreenRect above{s.left, s.top - kSpill, s.right, s.top};
    // The largest refresh rate difference with a monitor the strip would reach, -1 for none.
    auto mismatch = [&](const ScreenRect& strip) {
        int worst = -1;
        for (const MonitorArea& m : monitors)
            if (m.rect.intersects(strip)) worst = std::max(worst, std::abs(m.hz - screen.hz));
        return worst;
    };
    int r = mismatch(right), a = mismatch(above);
    BorderlessLayout l;
    l.window = s;
    if (r < 0 || (a >= 0 && r <= a)) {
        l.window.right += kSpill;
        l.spillX = kSpill;
    } else {
        l.window.top -= kSpill;
        l.viewY = kSpill;
        l.spillY = kSpill;
    }
    return l;
}

}  // namespace plat
