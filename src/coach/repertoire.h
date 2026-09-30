// The coach's teaching repertoire: its own opening moves in Coach mode, taken from the opening book so that what
// it plays is worth naming and teaching (research-openings.md §2.4).
#pragma once
#include "coach/openings.h"

namespace coach {

// While the game is in book and before ply 16, a sound book move of a common opening for the side to move:
//   levels 1-2  open games from tier-1 families: 1.e4 e5 2.Nf3 Nc6 3.Bc4 (sometimes 3.d4) as White; 1...e5 against
//               1.e4, 1...d5 against 1.d4 and 1.Nf3, 1...e5 against 1.c4 as Black;
//   levels 3-4  1.e4 or 1.d4, the Open Games, the Sicilian, the Caro-Kann, the French, the Queen's Gambit and the
//               Indian defences, among tier-1 and tier-2 families;
//   levels 5-6  a wider, flatter mix (1.c4 and 1.Nf3 too, the Sicilian first against 1.e4).
// Off the curated first moves, the move is drawn among the book moves that lead to the most teaching lines
// (OpeningBook::teachingLines: no gambit, trap or dubious sideline). Returns an invalid Move when Stockfish should
// play instead: level 0, custom start position, position out of book, ply >= 16, game over, or no suitable move.
// Deterministic: the same game, level and seed always give the same move.
chess::Move repertoireMove(const chess::Game& game, int level, uint64_t seed,
                           const OpeningBook& book = OpeningBook::instance());

constexpr int kRepertoireMaxPly = 16;

}  // namespace coach
