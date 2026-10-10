// Stances of a robot player: seated at the board, or standing up to look at it.
//
// A player may get up during a game to look at the board standing in front of their chair
// (Standing: the robot pushes the chair back and rises) or from one end of the table (SideLeft,
// SideRight: the player's own left and right as seen from the seat; the robot walks there round
// the table corner, through Standing). Only a seated player plays: the hands touch nothing while
// the robot stands. The values are those of the realtime protocol's Stance enum (minor 2,
// net::proto::Stance), so an online opponent's stance maps to one directly.
#pragma once
#include "../game/layout.h"
#include "../math/math.h"
#include <cmath>
#include <cstdint>

namespace anim {

enum class Stance : uint8_t { Seated = 0, Standing = 1, SideLeft = 2, SideRight = 3 };

// A wire or saved value as a Stance: values a later version may add read as Seated.
inline Stance stanceFromCode(int code) {
    return code >= 0 && code <= 3 ? Stance(code) : Stance::Seated;
}

// Where the robot stands still at a stance: the hip joint centre (the pelvis bone, world) and the
// direction its body faces on the floor (unit, y = 0). seatSign: +1 for the seat at +Z (White in
// the layout, facing -Z), -1 for the seat at -Z. Left and right are the seated player's own (for
// the seat at +Z, left is -X). The ends of the table are shared: each robot stands on its own half
// of its end (|z| = kSideSpotZ on its own side), so two players at the same end fit side by side.
// The animator ends its stance changes exactly on these spots; the scene and the online mock read
// them to know where a standing head is.
constexpr float kStandSpotZ = 0.70f;        // |z| of the pelvis standing in front of the chair
constexpr float kStandPelvisY = 0.995f;     // pelvis height of the robot standing, knees almost straight
constexpr float kSideSpotX = 0.88f;         // |x| of the pelvis at an end of the table
constexpr float kSideSpotZ = 0.28f;         // |z| of the pelvis there, on the player's own half
constexpr float kChairSlideMax = 0.36f;     // how far the robot pushes its chair back to stand up

struct StanceSpot {
    m::vec3 pelvis;
    m::vec3 forward;
};

inline StanceSpot stanceSpot(Stance s, float seatSign) {
    float zs = seatSign < 0.0f ? -1.0f : 1.0f;
    m::vec3 ahead(0.0f, 0.0f, -zs);
    switch (s) {
    case Stance::Standing: return {m::vec3(0.0f, kStandPelvisY, zs * kStandSpotZ), ahead};
    case Stance::SideLeft:
    case Stance::SideRight: {
        // The seat at +Z has its right at +X.
        float xs = (s == Stance::SideRight ? 1.0f : -1.0f) * zs;
        m::vec3 p(xs * kSideSpotX, kStandPelvisY, zs * kSideSpotZ);
        m::vec3 f(-p.x, 0.0f, -p.z);
        float l = std::sqrt(f.x * f.x + f.z * f.z);
        return {p, m::vec3(f.x / l, 0.0f, f.z / l)};
    }
    case Stance::Seated:
    default: return {m::vec3(0.0f, layout::PLAYER_PELVIS_Y, zs * layout::PLAYER_PELVIS_Z), ahead};
    }
}

}  // namespace anim
