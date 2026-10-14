// Internal to the coach's review (review.cpp, review_explain.cpp, appraisal.cpp): the facts of one
// reviewed move and the helpers that turn them into beats. Not a public API.
#pragma once
#include "assessment.h"
#include "review.h"
#include "tactics.h"

#include <map>
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
    // What the move does to the expected result (assessment.h): the engine's WDL and the practical
    // win percentage of the best, second and played lines.
    Assessment a;
    bool isBest = false;
    std::vector<LineStep> r;                 // refutation from p1 (the coach moves first)
    std::vector<LineStep> best;              // L1 from p0
    std::vector<LineStep> playedLine;        // Lp from p0 (the played move first)
    chess::Position rEnd, bestEnd, playedEnd;   // the positions where those lines stop
    int base = 0;                            // the human's material lead at p0 (points)
    int gain = -1;                           // plies of r after which the human is 2+ points down for good (-1: never)
    int loss = 0;                            // points the human is down for good within the lookahead
    // What the best move costs: what the human is still down at the end of the best line's first 8
    // plies (from p0), where the line settles (an exchange completed; what the human wins back within
    // four more plies taken off). A loss the best move suffers too is no reason the move played was bad.
    int bestLoss = 0;
    // What the move played loses for good along its own line (the deeper search's, from p0): the
    // most the human is down after any of its first 9 plies and still four plies later. -1 when that
    // line is too short to tell.
    int playedLoss = -1;
    // How much the move costs in the engine's eyes: the human's centipawns after the best move minus
    // after the move played (mates as +-1000).
    int dropCp = 0;
    // Whether losing 'lost' points (for good, from p0) is the reason the move is bad: a real loss
    // (1 point when the best move loses nothing, or a pawn at most and the reason is about a pawn,
    // else 2), one the best move avoids (by as much), and one the size of the problem (3 points or
    // more; else at least 40 % of the engine's drop): a pawn named for a collapse would hide what
    // really goes wrong.
    bool lossExplains(int lost, bool pawn = false) const;
    // The refutation loses material for good, beyond what the best move loses, or mates within its
    // first 'plies' plies, or the move allows a forced mate however long: a reason that is about
    // something else (king safety, the endgame, a better plan) would not be the real one.
    bool concreteLoss(int plies) const;
    // The refutation leaves the human down for good, beyond what the best move costs, after one of
    // its first 'plies' plies and still four plies later, or the move played's own line does
    // (playedLoss: the two searches can go different ways, Nb1 hxg4 ... Rxc7+ Qxc7 Qxc7+ Kxc7 losing
    // the exchange where the refutation loses it two plies later): a "better position" or a better
    // plan is then never the reason (the material is), even when no detector can name how it goes.
    bool losesMaterial(int plies) const;
    // Points the human is down for good after the first 'plies' plies of r (relative to p0): a
    // recapture on the next ply undoes a loss, and where r stops, so does the exchange the human can
    // still win on the board (a line cut short right after a capture proves nothing).
    int lossAfter(int plies) const;
    // The most the human is down for good after any of the first 'plies' plies of r.
    int lossWithin(int plies) const;
    // The plies of r, at most 'want', the coach can show or say (followablePlies): never a move that
    // leaves a bigger prize standing (a knight taken while the queen is en prise: whatever the
    // engine's reason, the table would show the queen ignored), unless its side wins as much back
    // (an in-between move, checks that end on a bigger capture: wonBackAt), which comes with that
    // capture: 'extend' goes on to it, else the part stops before the move. r itself is the
    // engine's line, whole: what is lost along it is counted on all of it.
    int followable(int want, bool extend) const;
    // The plies of the best line, at most 'want', the coach can say (at least its first move).
    int followableBest(int want) const;
    // How far a demonstration (levels 1-3) or a line said (levels 4-6) may go to reach a loss further
    // than the band's own plies, when every coach move among the first 'plies' plies of r forces the
    // reply (a check, a capture, a promotion) and the human can take nothing back where they end:
    // one forced move after another, a distant loss the player can follow (9 plies at level 1, 11
    // above). 0 when one of them is a quiet move, or a take-back is left.
    int forcingPlies(int plies) const;
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
    // Where the sequence stops, the human could take back, but that fails: the take-back the table
    // shows there (after fullPlies plies of r) and the coach's capture that answers it. Invalid
    // moves when the loss needs no such sentence.
    chess::Move retake, punish;
};

// ---- Claim checks (claims.cpp): what a reason's sequence establishes ----
// How firmly the consequence a reason states stands.
enum class Support : uint8_t { Established, Plausible, Unsupported };
struct ClaimCheck {
    Support support = Support::Established;
    int quiet = 0;          // the human's free moves inside the sequence (quietReplies)
    int compensation = 0;   // centipawns by which the engine's drop falls short of the material the move loses
    int held = -1;          // the loss that stands along the line, its plies and two more (lineLoss)
    bool ownLine = true;    // the move's own deeper line loses as much (or is mated), when it is long enough to tell
};
// The human's moves among the first 'plies' plies of r that the human chose: not a capture, not an
// answer to a check, not the only legal move. A sequence that needs several of them is one way the
// game may go, not what the move forces.
int quietReplies(const Ctx& c, int plies);
// The checks every reason's sequence goes through (claims.cpp).
ClaimCheck checkClaim(const Ctx& c, const Explanation& ex);

// ---- Sequences verified move by move (claims.cpp, ReviewInput::verified) ----
// Two moves of the same side are as good as each other when a search puts them within this many
// win-% points (the side to move's view), and within kVerifySlackCp centipawns where the win
// percentage no longer tells them apart (a decided game; a forced mate above any material).
constexpr double kVerifySlack = 3.0;
constexpr int kVerifySlackCp = 150;
bool asGood(const ai::Score& a, const ai::Score& b);   // 'a' as good as 'b', or better (side to move's view)
// A coach move of a line is replaced only when the comparison puts it this many win-% points below
// the best: a slightly weaker move still shows what the reason says (the reason is checked on the
// line as it is), while a new line from there may no longer show it.
constexpr double kReplaceSlack = 10.0;
// The searches the verification asks for, by key (Review::verifyNext, Reviewer::verifyRequest): a
// position's FEN for the short search of all its moves (MultiPV 3, Reviewer::kVerifyDepth), or
// "FEN | m1 m2" for the deeper comparison of two moves there (Reviewer::kCompareDepth): what a short
// search finds against the engine's own line is only a doubt (one search in eight of the moves it
// prefers holds at depth 20), the comparison settles it.
std::string compareKey(const std::string& fen, const std::string& a, const std::string& b);
// The two moves' lines of a comparison, null while it is not in 'v' (or failed); 'a' is a's line.
bool compared(const std::map<std::string, ai::Analysis>& v, const std::string& fen, const std::string& a,
              const std::string& b, const ai::PvLine*& la, const ai::PvLine*& lb);
// Another defence the player had, about as good in the short search as the one in the line: the
// line with it (the plies before it, then the search's line from there).
struct Defence {
    int at = 0;                         // the ply of the line where the player chooses
    std::vector<LineStep> line;
    ai::Score score;                    // the short search's score of that defence (the player's view)
};
struct Verification {
    int plies = 0;                      // plies of the line walked from its start (forced replies too)
    bool changed = false;               // a coach move replaced (the rest of the line with it)
    int changedAt = -1;                 // where
    std::vector<Defence> others;        // the player's other defences, about as good as the line's
    std::string next;                   // the key of the first search the walk still needs
    std::vector<std::string> notes;     // the moves replaced, and why (Review::verifyLog)
};
// 'line' (from 'start', where the coach moves first) walked against the searches in 'v', for at most
// 'want' plies. A coach move the short search finds weaker than its best is compared with it more
// deeply; when the comparison finds it clearly weaker (kReplaceSlack), the line goes on with the
// comparison's line from there.
// Within its first 'deepPlies' plies the line's own search saw deeper: its coach moves stay. The
// player's other moves about as good as the line's in the short search are kept as other defences
// (verifyReason weighs them). Stops at the first search missing (Verification::next); a reply the
// player is forced to make needs none. 'first' (optional) stands for the short search of 'start'
// (A2, on p1).
Verification verifyLine(const Ctx& c, std::vector<LineStep>& line, const chess::Position& start,
                        const std::map<std::string, ai::Analysis>& v, const ai::Analysis* first, int want,
                        int deepPlies);
// The most the human is down for good after any of the plies from..plies of 'line' (from p1),
// relative to p0's balance: a loss counts when no ply of the next kHeldPlies wins it back (each
// read where the captures that follow at once end: an exchange, or a capture answered by another,
// counted whole), nor a capture still due at the line's end. A defence's line often ends in the
// middle of the play that wins the material back.
constexpr int kHeldPlies = 4;
int lineLoss(const Ctx& c, const std::vector<LineStep>& line, int from, int plies);

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
// What the human gets on its square for the piece the coach takes at ply k of r: what that piece took
// there just before (unless it only took back as much as the coach had taken there: Rxg8+ Bxg8 Qxg8+
// trades the rooks and wins the bishop), and what the human takes back there at once (less what the
// coach takes back there then: Qxb4+ Qxb4 Bxb4+ trades the queens and wins the bishop). As much as
// the piece, and it is traded, not lost (Rdxd7+ Rxd7 Rxd7+; Qxf8+ Qxf8 Bxf8).
int tradedFor(const Ctx& c, int k);
// Opening principles and endgame technique tips, for moves that are not faults.
bool findTip(const Ctx& c, uint32_t tipsSaid, Explanation& out);
// The tip findTip gives is about a won game drawn by repetition or the fifty-move rule: then it is
// the reason the move costs so much.
bool drawTip(const Ctx& c, uint32_t tipsSaid);
// What a blunder without a reason missed: the points the better move wins for good (where its line
// settles) when the move played neither wins nor loses any along its own, and those points explain
// the drop by themselves (0: none, or not that plain). A hint can then say the better move wins
// material without naming it.
int missedGain(const Ctx& c);

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
// Whether the demonstration plays the whole sequence, to where Explanation::retake is said.
bool demoReachesEnd(const Ctx& c, const Explanation& ex);
// The line that says why the take-back fails (Explanation::retake), its pieces pointed at where
// they stand on 'table'; false when there is none.
bool retakeBeat(const Ctx& c, const Explanation& ex, const chess::Position& table, Beat& out);

}  // namespace detail
}  // namespace coach
