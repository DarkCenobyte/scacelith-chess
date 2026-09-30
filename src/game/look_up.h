// Looking up at the opponent without the right mouse button: with the pointer near the top of the
// window, the first-person gaze rises towards the opponent's face (at the default look the whole
// head is above the frame). GameScene::updateCamera drives it; the math is here, unit-tested
// (tests/look_up_tests.cpp).
#pragma once
#include "../math/math.h"

namespace game {

// The band: pointer height as a fraction of the window height, from the top edge (the vertical
// field of view is fixed, so the fractions hold at every aspect ratio). The lift starts at
// kLookUpBandStart and is full at kLookUpBandFull. At the default look, with the pointer at the
// band's start, the highest piece of the far rank (a king's top) stands at 0.31 of the height: the
// band starts well above it, so the pointer never lifts the view while it is on a piece.
constexpr float kLookUpBandStart = 0.20f;
constexpr float kLookUpBandFull = 0.04f;
// Looking down (a negative Look::pitch) raises the far rank by about 1.1 window heights per
// radian: the band shrinks with it, and is gone once it would start nearer the top than
// kLookUpBandMin (a thinner band would swing the view more than twice as fast under the pointer).
constexpr float kLookUpFarRankRise = 1.1f;
constexpr float kLookUpBandMin = 0.13f;
// The absolute gaze pitch that frames the opponent's whole head, its top about 5% of the height
// under the top edge. Leaning in (0..1) brings the eyes 11 cm closer and 5 cm lower: a higher one.
constexpr float kFaceGazePitch = -0.275f;
constexpr float kFaceGazePitchLean = 0.065f;

// Where the band starts for the look offset 'lookPitch' (Look::pitch, radians); 0 without a band.
inline float lookUpBandStart(float lookPitch) {
    float v0 = m::clamp(kLookUpBandStart + kLookUpFarRankRise * std::min(lookPitch, 0.0f), 0.0f, kLookUpBandStart);
    return v0 > kLookUpBandMin ? v0 : 0.0f;
}

// How far into the band the pointer is at height v (fraction, from the top): 0 below the band, 1 in
// its top part (from kLookUpBandFull, or its share of a shrunken band, to the edge).
inline float lookUpWeight(float v, float lookPitch) {
    float v0 = lookUpBandStart(lookPitch);
    if (v0 <= 0.0f) return 0.0f;
    return m::smoothstep(v0, v0 * (kLookUpBandFull / kLookUpBandStart), v);
}

// The pitch the band adds to 'basePitch' (the absolute gaze pitch without it): a blend towards the
// face by 'weight', and nothing when the look is already as high.
inline float lookUpLift(float weight, float basePitch, float lean) {
    return m::saturate(weight) * std::max(0.0f, kFaceGazePitch + kFaceGazePitchLean * m::saturate(lean) - basePitch);
}

}  // namespace game
