// The first of the coach review's three steps (src/coach/review.h): what a move does to the expected
// result, before any reason is looked for (the established facts: review_explain.cpp; the wording:
// review.cpp). Two scales, read from the same analysis:
//  - the engine's: Stockfish's win/draw/loss chances (UCI_ShowWDL), expected score E = W + D/2, what
//    the position is worth with best play on both sides. A move that changes it changes the result.
//    Stockfish fits that model on its own games, where an advantage of 1.5 pawns already wins
//    almost surely: it says whether a result is at stake, not how hard it is to reach.
//  - the practical one: the lichess win percentage of the centipawns (review.h: winPercent), fitted
//    on human games, what the move costs or gains over the board. The move classes come from it.
// Forced mates stay apart (E is 1 or 0 for any of them; their length tells the moves apart).
//
// Engine-free: the scores come from the Reviewer's analyses (A0, A1).
#pragma once
#include "../ai/analysis.h"

#include <cstdint>
#include <vector>

namespace coach {

// One line's worth for the side to move.
struct Outcome {
    double e = 0.5;          // engine expected score, 0..1 (W + D/2 from the WDL)
    double w = 50.0;         // lichess win percentage, 0..100
    int win = 0, draw = 1000, loss = 0;   // per mille, side to move
    bool wdl = false;        // the engine sent its WDL; else e falls back to the lichess curve
    bool mates = false;      // the side to move mates (any distance)
    bool mated = false;      // the side to move is mated
};
Outcome outcomeOf(const ai::Score& s);

enum class ResultClass : uint8_t { Loss, Draw, Win };
// The result the engine expects with best play: a win when W >= 50 %, a loss when L >= 50 %, else a
// draw (forced mates count as wins and losses).
ResultClass resultOf(const Outcome& o);

// The engine's and the practical worth of the move played against the moves it could have played.
struct Assessment {
    bool wdl = false;            // the lines compared all carry the engine's WDL
    Outcome best, played;        // A0's best line, the move played (same root)
    Outcome second;              // A0's second line (only when hasSecond)
    bool hasSecond = false;
    bool playedIsBest = false;
    // The lowest of A0's lines and the move played, on each scale.
    double eLow = 0.5, wLow = 50.0;

    // How much the move lowers the engine's expected score (0 for the best move).
    double eDrop() const;
    // The best move's lead over the second best (engine E, lichess W%): the move that alone keeps
    // the result has a large one. 0 without a second line.
    double eLead() const;
    double wLead() const;
    // How far apart the moves compared lie (A0's lines and the move played; engine E, lichess W%):
    // a position where they all lie together is one where the move makes no difference.
    double eSpread() const;
    double wSpread() const;
    // The move changes the result the engine expects (a win becomes a draw, a draw a loss...):
    // the classes differ and E falls by kResultDrop or more.
    bool changesResult() const;
};

// ---- Thresholds (engine E in 0..1, lichess W% in points) ------------------------------------------
// A drop of E this large, with the result's class changing, is a result thrown away.
inline constexpr double kResultDrop = 0.15;
// The best move's lead over the second one that makes it the only move keeping the result (a
// pawn's worth near equality, a win against a hard draw).
inline constexpr double kOnlyLead = 0.10;
// In a position the practical scale calls decided (W% >= 90 or <= 10), only a move this far ahead
// of the second one is worth a word: it alone keeps the win, or saves the draw.
inline constexpr double kDecidedLead = 0.25;
// The moves compared lie this close on both scales: the move makes no difference to the result.
inline constexpr double kFlatE = 0.03;
inline constexpr double kFlatW = 5.0;

// Build from A0's lines (best first; at least one) and the move played's score (A0's own line or A1's).
Assessment assess(const std::vector<ai::PvLine>& a0, const ai::Score& played, bool playedIsBest);

}  // namespace coach
