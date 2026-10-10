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
    chess::Position rEnd, bestEnd, playedEnd;   // the positions where those lines stop
    int base = 0;                            // the human's material lead at p0 (points)
    int gain = -1;                           // plies of r after which the human is 2+ points down for good (-1: never)
    int loss = 0;                            // points the human is down for good within the lookahead
    // What the best move costs: what the human is still down near the end of the best line's first
    // 8 plies (from p0). A loss the best move suffers too is no reason the move played was bad.
    int bestLoss = 0;
    // How much the move costs in the engine's eyes: the human's centipawns after the best move minus
    // after the move played (mates as +-1000).
    int dropCp = 0;
    // Whether losing 'lost' points (for good, from p0) is the reason the move is bad: a real loss
    // (1 point when the best move loses nothing or the reason is about a pawn, else 2), one the best
    // move avoids (by as much), and one the size of the problem (3 points or more; else at least 40 %
    // of the engine's drop): a pawn named for a collapse would hide what really goes wrong.
    bool lossExplains(int lost, bool pawn = false) const;
    // The refutation loses material for good, beyond what the best move loses, within its first
    // 'plies' plies: a reason that is about something else (king safety, the endgame, a better plan)
    // would not be the real one.
    bool concreteLoss(int plies) const;
    // Points the human is down for good after the first 'plies' plies of r (relative to p0): a
    // recapture on the next ply undoes a loss, and where r stops, so does the exchange the human can
    // still win on the board (a line cut short right after a capture proves nothing).
    int lossAfter(int plies) const;
    // The most the human is down for good after any of the first 'plies' plies of r.
    int lossWithin(int plies) const;
};

// pov's material change, for good, after the first 'plies' plies of 'line' (relative to 'base'; 'end'
// is the position where the line stops): a gain the next ply takes back is none, and where the
// line stops, the side to move still takes back what an exchange on the board wins it.
int heldGain(const std::vector<LineStep>& line, size_t plies, int base, chess::Color pov, const chess::Position& end);
// pov's material loss, for good, after the first 'plies' plies of 'line' (relative to 'base'): a
// recapture on the next ply undoes it (not a capture there that is taken back on the ply after: that
// exchange counts whole), and where the line stops, so does what the side to move, when it is pov,
// takes back at once.
int heldLoss(const std::vector<LineStep>& line, size_t plies, int base, chess::Color pov, const chess::Position& end);
// The position after the whole of 'line', played from 'start'.
chess::Position lineEnd(const chess::Position& start, const std::vector<LineStep>& line);
// Where the engine's refutation stops early (a search cut short reports short lines), the natural
// replies the board shows, marked guessed, until 'r' holds 'want' plies: the human's recapture on
// the square the coach just took on (the least valuable piece whose recapture does not lose), and
// the coach's capture that wins the most by exchange (2 points at least). Never past a mate, and
// never a capture that leaves the human more than 'maxLoss' points down for good from 'base' (the
// engine's score says less is lost: guessLossBound); the human's guessed recapture before such a
// capture goes too.
void extendRefutation(std::vector<LineStep>& r, const chess::Position& p1, chess::Color human, size_t want, int base,
                      int maxLoss);
// The most points a guessed reply may leave the human down, from the engine's scores of the best
// and the played move (the side to move's view, before the move): what the move costs or what the
// position after it is worth, a pawn being about 100 centipawns, with 2 points to spare.
int guessLossBound(const ai::Score& best, const ai::Score& played);

// A chosen explanation: the lines that say why (with pointing), the demonstration, the policy bits.
// A detector tells what its sequence is (fullPlies, threatPlies, lost, targets); planDemo() decides,
// per level, how much of it the table shows.
struct Explanation {
    ExType type = ExType::None;
    std::vector<Beat> cause;   // said before the demonstration (the table shows p1)
    std::vector<Beat> tail;    // said after the whole sequence (full demonstrations; pieces where they stand then)
    // Said once the problem stands on the table (after threatPlies plies): in the middle of a full
    // demonstration, or at the end of one that stops there; pieces where they stand then.
    std::vector<Beat> threatTail;
    std::vector<Beat> tip;     // said after the rewind (no pointing at pieces a demonstration moved)
    // The sequence: plies of Ctx::r until the material is lost for good (the capture, and the
    // human's recapture when it follows) or the mate is given; the plies until the problem stands
    // on the board (the fork, the pin, the move before the mate; 0 when it already stands on p1);
    // the points lost then (from p0); the pieces that fall (squares after threatPlies plies, the
    // human's king for a mate) and whether the problem is a mate.
    int fullPlies = 0, threatPlies = 0, lost = 0;
    std::vector<chess::Square> targets;
    bool mate = false;
    bool full = false;         // planDemo(): the demonstration plays the whole sequence
    int demoPlies = 0;         // plies of Ctx::r shown on the table (planDemo())
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
// At most three pointing gestures per line; the others keep only their marks.
void limitPointing(Beat& b);

// ---- Explanation choice (in ExType order, first match wins) ----
bool findExplanation(const Ctx& c, Explanation& out);
// Opening principles and endgame technique tips, for moves that are not faults.
bool findTip(const Ctx& c, uint32_t tipsSaid, Explanation& out);

// How much of the sequence the table shows (Explanation::demoPlies, full): levels 1-3 the whole of it
// (fullPlies, within the band's demoPlies; a mate within mateLinePlies), else, and from level 4, up
// to where the problem stands (threatPlies, within the band's demoPlies; then the coach points at
// it); else nothing (the lines say it in words). Never a demonstration that stops before the point
// it makes, and never past the line the engine (or the board) proves.
void planDemo(const Ctx& c, Explanation& ex);
// Plies of c.r the demonstration shows (ex.demoPlies within the line).
int demoLength(const Ctx& c, const Explanation& ex);
// Demonstration beats for the first demoLength() plies of c.r (stops before a promotion of the
// human's colour and points at the square instead), with the threat lines once the problem stands
// and the closing lines once every planned ply was shown (the detector's, else a generic one: what
// was lost, or what falls next), and the rewind of exactly the moves shown.
void appendDemo(const Ctx& c, const Explanation& ex, Script& s);

}  // namespace detail
}  // namespace coach
