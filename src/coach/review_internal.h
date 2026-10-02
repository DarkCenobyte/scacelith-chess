// Internal to the coach's review (review.cpp, review_explain.cpp, appraisal.cpp): the facts of one
// reviewed move and the helpers that turn them into beats. Not a public API.
#pragma once
#include "review.h"
#include "tactics.h"

#include <string>
#include <vector>

namespace coach {
namespace detail {

// Everything known about the human's move under review.
struct Ctx {
    Band b;
    int level = 1;
    chess::Color human = chess::White, coach = chess::Black;
    const chess::Game* g = nullptr;
    int ply = 0;
    chess::Position p0, p1;                  // before and after the move (p1 is on the table)
    chess::Move played;
    std::string playedUci, playedSan;
    MoveFacts f;                             // analyzeMove(p0, played)
    const ai::PvLine* l1 = nullptr;          // best line
    const ai::PvLine* l2 = nullptr;          // second best (only-move tests), may be null
    const ai::PvLine* lp = nullptr;          // the played move's line
    Judgement j;
    bool isBest = false;
    std::vector<LineStep> r;                 // refutation from p1 (the coach moves first)
    std::vector<LineStep> best;              // L1 from p0
    std::vector<LineStep> playedLine;        // Lp from p0 (the played move first)
    int base = 0;                            // the human's material lead at p0 (points)
    int gain = -1;                           // plies of r after which the human is 2+ points down for good (-1: never)
    int loss = 0;                            // points the human is down for good within the lookahead
    // Points down for good after the first 'plies' plies of r (a recapture on the next ply undoes a loss).
    int lossWithin(int plies) const;
};

// A chosen explanation: the lines that say why (with pointing), the demonstration, the policy bits.
struct Explanation {
    ExType type = ExType::None;
    std::vector<Beat> cause;   // said before the demonstration (the table shows p1)
    std::vector<Beat> tail;    // said after a demonstration of exactly one ply (pieces where they stand then)
    std::vector<Beat> tip;     // said after the rewind (no pointing at pieces a demonstration moved)
    int demoPlies = 0;         // plies of Ctx::r shown on the table
    bool offer = false;        // the takeback is offered whatever the class (mates, stalemate, missed pieces)
    bool concrete = false;     // the cause lies within the band's reach (the level 1-2 voice threshold)
    int tipBit = -1;           // which principle (Reviewer::tipsSaid_)
    bool includesBest = false; // the cause already names the better move
    bool missed = false;       // the human missed something (no demonstration: the table shows p1)
    chess::Square hintSquare = chess::NoSquare;   // takeback hint (levels 1-2): the piece at risk, on p0
};

// ---- Engine requests and figures (shared with the appraisal) ----
// The start position as sent to the engine: "" for the standard start, else its FEN.
std::string startFenOf(const chess::Game& g);
// A request on the position after the first 'plies' moves of g (search settings left to the caller).
ai::AnalysisRequest requestAt(const chess::Game& g, size_t plies);
// A score (the side to move's view) as White's centipawns, mates as +-1000, clamped.
int whiteCp(const ai::Score& s, bool whiteToMove);
Arg evalArg(const ai::Score& s);
inline int points(chess::PieceType t) { return kPiecePoints[t]; }
inline bool hasBit(uint64_t set, chess::Square s) { return s != chess::NoSquare && (set & chess::squareBit(s)); }

// ---- Beats ----
std::string bandKey(const std::string& family, int level);   // family + ".b" + level
Beat sayBeat(const std::string& key, Look look, int ply, Priority pr = Priority::Normal);
Arg pieceArg(const chess::Position& p, chess::Square s, chess::Color listener);
Arg moveArg(const chess::Position& before, const chess::Move& m);
Arg moveArg(const LineStep& s);
// Gestures with their marks (the highlight comes with the finger).
void pointPiece(Beat& b, chess::Square s, const std::string& anchor, bool emphasis = false);
void pointSquare(Beat& b, chess::Square s, const std::string& anchor);
void traceMove(Beat& b, chess::PieceType t, chess::Square from, chess::Square to, const std::string& anchor);
void markSquare(Beat& b, chess::Square s, const std::string& anchor);
void markPiece(Beat& b, chess::Square s, const std::string& anchor);
void markArrow(Beat& b, chess::PieceType t, chess::Square from, chess::Square to, const std::string& anchor);
// Research-pedagogy R6: at most three pointing gestures per line; the others keep only their marks.
void limitPointing(Beat& b);

// ---- Explanation choice (research-pedagogy §2.0 table, first match wins) ----
bool findExplanation(const Ctx& c, Explanation& out);
// Opening principles and endgame technique tips (§2.16, §2.17), for moves that are not faults.
bool findTip(const Ctx& c, uint32_t tipsSaid, Explanation& out);

// Demonstration beats for the first 'plies' plies of c.r (stops before a promotion of the human's
// colour and points at the square instead), followed by the tail lines and the rewind of exactly
// the moves shown.
void appendDemo(const Ctx& c, const Explanation& ex, Script& s);

}  // namespace detail
}  // namespace coach
