// The first-person look-up band (game/look_up.h): the pointer at the top of the window lifts the
// gaze to the opponent's face.
#include "test.h"
#include "game/look_up.h"
#include <cmath>

using namespace game;

namespace {

constexpr float kBaseGazePitch = -0.62f;     // GameScene::kBaseGazePitch
constexpr float kHalfFov = 26.0f * m::DEG;   // GameScene's kFov is 52 degrees (vertical)

// The gaze GameScene::updateCamera aims at for a pointer at height v (fraction from the top),
// the drift towards the pointer included.
float targetPitch(float v, float lookPitch, float lean) {
    float base = kBaseGazePitch + lookPitch - (v - 0.5f) * 0.08f;
    return base + lookUpLift(lookUpWeight(v, lookPitch), base, lean);
}

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

}  // namespace

TEST(look_up_nothing_below_the_band) {
    for (float v = kLookUpBandStart; v <= 1.0f; v += 0.01f) {
        CHECK_EQ(lookUpWeight(v, 0.0f), 0.0f);
        CHECK_EQ(lookUpLift(lookUpWeight(v, 0.0f), kBaseGazePitch, 0.0f), 0.0f);
    }
}

TEST(look_up_full_at_the_top_frames_the_face) {
    for (float v : {0.0f, 0.02f, kLookUpBandFull}) {
        CHECK(near(lookUpWeight(v, 0.0f), 1.0f));
        CHECK(near(targetPitch(v, 0.0f, 0.0f), kFaceGazePitch));
        CHECK(near(targetPitch(v, 0.0f, 1.0f), kFaceGazePitch + kFaceGazePitchLean));
    }
    // Half a lean, half the lean's share; the lean is clamped to 0..1.
    CHECK(near(targetPitch(0.0f, 0.0f, 0.5f), kFaceGazePitch + 0.5f * kFaceGazePitchLean));
    CHECK(near(lookUpLift(1.0f, kBaseGazePitch, 3.0f), lookUpLift(1.0f, kBaseGazePitch, 1.0f)));
}

TEST(look_up_rises_steadily_and_the_pointer_with_it) {
    // Going up the window the gaze never drops, and neither does the point under the pointer: the
    // band never pushes the view away from where the pointer goes.
    for (float lean : {0.0f, 1.0f}) {
        for (float lookPitch : {-0.06f, 0.0f, 0.2f}) {
            float prevGaze = -10.0f, prevPointer = -10.0f, maxGain = 0.0f;
            for (int i = 1000; i >= 0; --i) {
                float v = float(i) / 1000.0f;
                float gaze = targetPitch(v, lookPitch, lean);
                float pointer = gaze + std::atan((1.0f - 2.0f * v) * std::tan(kHalfFov));
                CHECK(gaze >= prevGaze - 1e-6f);
                CHECK(pointer > prevPointer);
                if (i < 1000) maxGain = std::max(maxGain, (gaze - prevGaze) * 1000.0f);
                prevGaze = gaze;
                prevPointer = pointer;
            }
            // Radians of view per window height, at most: 3.0 at the default look (3.6 leaning in),
            // and less than twice that in the thinnest band.
            CHECK(maxGain < (lookPitch < 0.0f ? 2.0f : 1.0f) * 3.7f);
        }
    }
}

TEST(look_up_never_overshoots_a_high_look) {
    // A look raised with the right button beyond the face gets no lift; one below it goes no
    // higher than the face.
    for (float v = 0.0f; v <= 1.0f; v += 0.01f) {
        CHECK_EQ(lookUpLift(lookUpWeight(v, 0.5f), kBaseGazePitch + 0.5f, 0.0f), 0.0f);
        CHECK(targetPitch(v, 0.25f, 0.0f) <= std::max(kFaceGazePitch, kBaseGazePitch + 0.25f + 0.04f) + 1e-6f);
        CHECK(targetPitch(v, 0.0f, 0.0f) <= kFaceGazePitch + 1e-6f);
    }
    CHECK_EQ(lookUpLift(1.0f, kFaceGazePitch + 0.01f, 0.0f), 0.0f);
}

TEST(look_up_band_follows_the_far_rank_when_looking_down) {
    CHECK_EQ(lookUpBandStart(0.0f), kLookUpBandStart);
    CHECK_EQ(lookUpBandStart(0.3f), kLookUpBandStart);  // looking up: the far rank goes down
    CHECK(near(lookUpBandStart(-0.05f), kLookUpBandStart - 0.05f * kLookUpFarRankRise));
    // The band shrinks to its full height's share, and is gone once too thin.
    float v0 = lookUpBandStart(-0.05f);
    CHECK_EQ(lookUpWeight(v0 + 0.001f, -0.05f), 0.0f);
    CHECK(near(lookUpWeight(v0 * kLookUpBandFull / kLookUpBandStart, -0.05f), 1.0f));
    CHECK_EQ(lookUpBandStart(-0.07f), 0.0f);
    for (float v = 0.0f; v <= 1.0f; v += 0.05f) CHECK_EQ(lookUpWeight(v, -0.07f), 0.0f);
    CHECK_EQ(lookUpBandStart(-1.0f), 0.0f);
}
