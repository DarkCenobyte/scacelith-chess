// Stances of a robot player: seated at the board, or standing up to look at it.
//
// A player may get up during a game to look at the board standing in front of their chair
// (Standing: the robot pushes the chair back and rises) or from one end of the table (SideLeft,
// SideRight: the player's own left and right as seen from the seat; the robot walks there round
// the table corner, through Standing). Only a seated player plays: the hands touch nothing while
// the robot stands. The values are those of the realtime protocol's Stance enum (minor 2,
// net::proto::Stance), so an online opponent's stance maps to one directly.
#pragma once
#include <cstdint>

namespace anim {

enum class Stance : uint8_t { Seated = 0, Standing = 1, SideLeft = 2, SideRight = 3 };

// A wire or saved value as a Stance: values a later version may add read as Seated.
inline Stance stanceFromCode(int code) {
    return code >= 0 && code <= 3 ? Stance(code) : Stance::Seated;
}

}  // namespace anim
