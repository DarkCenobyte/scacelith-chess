// When a move is completed and the turn passes, by time control (engine-free, unit-tested in
// tests/game_mode_tests.cpp; GameScene applies it in game_scene.cpp).
//   Timed game:   a move is completed by the clock press (FIDE 6.2.1; 7.5.1 for an illegal move),
//                 by hand or by the robot's auto-press.
//   Untimed game: there is no clock to press. A move is made, and completed, once its last piece
//                 is released (FIDE 4.7: the capturing piece, the castling rook, the promoted
//                 piece), and the turn passes at once. The clock shows dashes ("--:--"), its lever
//                 stays still, and it only keeps each side's used time for statistics.
#pragma once
#include "../chess/chess.h"

namespace game {

// Only an offline game without a time control is untimed: Play, HotSeat and Watch with "No clock",
// a Replay of a record without TimeControl or clocks (and Coach, which is always untimed). Online
// and direct games are always timed: the authority needs a base time, and the direct host page
// does not offer "No clock".
inline bool untimedGame(bool online, const chess::TimeControl& tc) { return !online && tc.unlimited; }

}  // namespace game
