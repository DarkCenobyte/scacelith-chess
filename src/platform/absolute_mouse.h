// Internal to the Win32 layer: the motion of a captured mouse that reports positions only (Remote
// Desktop, VirtualBox/VMware mouse integration, a tablet in absolute mode). The layer reads the
// motion of relative raw input packets, and such a mouse sends none: the right-button look and the
// watch camera would not move. Header-only, for the unit tests.
#pragma once
#include <algorithm>

namespace plat {

class AbsoluteMouse {
public:
    // The longest step one packet makes (pixels): a pen lifted and set down elsewhere turns the
    // view a little, not half a turn.
    static constexpr float kMaxStep = 64.0f;

    // A capture starts: its first position is the reference.
    void reset() { *this = AbsoluteMouse(); }
    // A relative packet with motion: this mouse reports its own motion, and the positions of the
    // capture are left out, so a relative mouse keeps exactly the motion it reports.
    void relativeMotion() { relative_ = true; }
    // A position (pixels) during the capture: adds the step from the previous one, bounded, to dx/dy.
    void position(float x, float y, float& dx, float& dy) {
        if (relative_) return;
        if (hasLast_) {
            dx += std::clamp(x - lastX_, -kMaxStep, kMaxStep);
            dy += std::clamp(y - lastY_, -kMaxStep, kMaxStep);
        }
        hasLast_ = true;
        lastX_ = x;
        lastY_ = y;
    }
    // An absolute packet (coordinates 0..65535 across a screen 'width' x 'height' pixels) during
    // the capture. One with no coordinates carries only a button or the wheel, not a position: it
    // is no jump to the top-left corner (SDL reads them the same way).
    void packet(long x, long y, int width, int height, float& dx, float& dy) {
        if (x == 0 && y == 0) return;
        position(pixels(x, width), pixels(y, height), dx, dy);
    }
    // Positions drive this capture: the cursor is not put back at the centre every frame (the
    // pointer of a Remote Desktop client would follow it and cancel the motion), but once at the end.
    bool active() const { return hasLast_ && !relative_; }

    // A coordinate of an absolute packet (0..65535 across the screen) in pixels of a screen
    // 'extent' pixels wide or high.
    static float pixels(long v, int extent) { return float(v) * float(extent) / 65535.0f; }

private:
    bool relative_ = false;
    bool hasLast_ = false;
    float lastX_ = 0.0f, lastY_ = 0.0f;
};

}  // namespace plat
