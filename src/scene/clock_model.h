// Digital lever chess clock (DGT 3000 style) in an elegant walnut case.
//
// Clock-local space (meters): origin at the centre of the base on the table, +Y up, long axis
// along Z (CLOCK_WIDTH), depth along X (CLOCK_DEPTH). The slanted front face with the two LCDs
// and the buttons looks towards -X (towards the board when the clock stands at +X). Half 0 is at
// local -Z (left for a viewer facing the displays), half 1 at local +Z; each half has its own LCD
// and the seesaw lever side above it.
//
// Lever: `lever` is modelled in clock-local space at its neutral (level) pose. Draw it with
//     clockToWorld * translate(leverPivot) * rotateAxis(leverAxis, angle) * translate(-leverPivot)
// leverAxis is +X: a positive angle lowers the +Z end (side 1). The lever always rests on one
// side: angle = +leverMaxAngle after side 1 was pressed, -leverMaxAngle after side 0 was pressed.
// pressPoint[i] is where a fingertip presses the top of the lever for side i (neutral pose:
// apply the same lever transform).
//
// LCD (MaterialId::ClockDisplay, shaders/materials/clock_display.glsl). DrawItem::inst[0] of the
// "displays" part:
//     x = side 0 time in milliseconds (local -Z half), y = side 1 time in ms,
//     z = flags (ClockDisplayFlags, as a float), w = running side (-1 none, 0 or 1).
// Formats (DGT 3000 like): H:MM:SS at 1 h and more, M:SS / MM:SS normally, S.t (seconds and
// tenths) under 20 s. Countdown values are truncated (4:59.9 shows 4:59). Mesh uv of the
// displays: window 0 spans uv.x in [0,1], window 1 uv.x in [2,3] (u grows towards +Z), uv.y in
// [0,1] bottom to top; window aspect CLOCK_LCD_W / CLOCK_LCD_H.
#pragma once
#include "model.h"
#include <cstdint>

enum ClockDisplayFlags : uint32_t {
    CLOCK_FLAG_UNLIMITED = 1u << 0,   // no time control: times are elapsed times counting up
                                      // (M:SS / H:MM:SS, no tenths); a negative time shows --:--
    CLOCK_FLAG_FALLEN_0 = 1u << 1,    // side 0 flag fell: flag symbol on LCD 0 (time shows 0.0)
    CLOCK_FLAG_FALLEN_1 = 1u << 2,    // side 1 flag fell
    CLOCK_FLAG_PAUSED = 1u << 3,      // clock paused: pause symbol on both halves, colon blinks,
                                      // no running indicator
    CLOCK_FLAG_OFF = 1u << 4,         // power off: blank LCDs (faint ghost segments only)
    CLOCK_FLAG_DASHES = 1u << 5,      // "--:--" on both halves (setup / no timing)
};

constexpr float CLOCK_LCD_W = 0.062f;  // LCD window size (m)
constexpr float CLOCK_LCD_H = 0.021f;

struct ClockModel {
    Model body;               // case (ClockCase), panel/feet (ClockPanel), "displays" (ClockDisplay), buttons (ClockLever)
    Model lever;              // seesaw lever (ClockLever), neutral pose
    m::vec3 leverPivot;       // clock-local
    m::vec3 leverAxis;        // +X
    float leverMaxAngle = 0;  // radians
    m::vec3 pressPoint[2];    // clock-local, neutral pose, index 0 at -Z
    m::vec3 displayCenter[2]; // centres of the LCD windows (clock-local)
    m::vec3 displayNormal;    // outward normal of the display face (clock-local)
};

ClockModel buildClock();

// Packs the LCD state for DrawItem::inst[0] (see above).
inline m::vec4 clockDisplayState(int64_t ms0, int64_t ms1, uint32_t flags, int runningSide) {
    return m::vec4(float(ms0), float(ms1), float(flags), float(runningSide));
}

// Configures MaterialId::ClockDisplay (surface shader + parameters). Call after materials::init().
void setupClockMaterials();
