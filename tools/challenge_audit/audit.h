// challenge_audit: checks the candidate positions of the coach's challenges (src/coach/challenge.h)
// with the embedded Stockfish and writes the game's file, assets/coach/challenges/challenges.txt.
//
//   challenge_audit [--candidates FILE] [--endgames FILE] [--out FILE] [--report FILE]
//                   [--depth D] [--play-depth D] [--confirm N] [--threads T] [--hash MB]
//                   [--only ID] [--all] [--dry-run]
//
// Inputs: tools/challenges/candidates.txt (tools/challenges/select.py: the Lichess puzzles, every
// set's header and the number of positions it needs, "# want <n>") and tools/challenges/
// endgames.txt (hand-written play-outs). The candidates of a set are checked in file order (the
// selection's preference order), until the set has its count (--all: every candidate, for the
// report); the kept positions are sorted by rating. A position is dropped at the first rule it
// fails: the coach must never call a correct move wrong, nor accept a losing one.
//
// Lines (the player's moves known). At every move of the player (k of K):
//   - mate lines (the last move mates): the line's move mates in exactly the moves left and no
//     other move mates as fast; at the last move, every other checkmate goes to `also`;
//   - other lines: the line's move is the engine's best, wins (>= +200 cp or a mate), and every
//     other move is clearly worse (>= 150 cp and 0.3 of Lichess' win chances below it). At the
//     last move, a move as good (another mate when the line's move mates, else within 30 cp and
//     winning) goes to `also`; a move neither as good nor clearly worse drops the position.
//   - the coach's answers are its best defence: the engine's best, or within 30 cp of it (80 cp in
//     a lost position, when the win chances differ by 0.02 at most), or the longest mate.
// Escape (made from a mate puzzle's set-up, the defender White to move, the line to be found):
//   exactly one move holds (>= -150 cp); every other move is >= 200 cp worse and below -300 cp;
//   at least three of them lose to mate (the king is in danger, the choice is not a guess between
//   two or three moves). The line is that one move.
// Play-outs: mate, the engine sees a forced mate; promote, >= +500 cp or a mate; hold, a score
//   within +-80 cp at --play-depth and at least one legal move that loses (+300 for Black or a
//   mate).
//
// Searches are depth-limited (no time limit) from a cleared hash table for each candidate. A
// candidate that passes is checked again --confirm plies deeper (24 and 34 by default) and kept
// only when it passes again with the same line and alternatives: a borderline verdict is dropped.
// With more than one thread Stockfish is not fully deterministic, so a borderline candidate may
// still change between runs (--threads 1 for exact reproducibility).
//
// Output: the header of the existing output file (its comment block), then the book written by
// coach::ChallengeBook::write(), read back with ChallengeBook::parse (no error allowed). A report
// (stdout, and --report FILE) lists each set's count and ratings, what the engine proved for each
// kept position, and every rejected candidate with its reason; stderr follows the progress. Exit status 1 when a set ends short of its count (the file is written anyway, with
// the positions found), 2 on a usage or input error.
#pragma once
#include "ai/analysis.h"
#include "chess/chess.h"
#include "coach/challenge.h"

#include <string>
#include <vector>

namespace challenges {

struct Options {
    std::string candidates = "tools/challenges/candidates.txt";
    std::string endgames = "tools/challenges/endgames.txt";
    std::string out = "assets/coach/challenges/challenges.txt";
    std::string report;      // "" = stdout only
    std::string only;        // check this set only (implies --dry-run)
    int depth = 20;          // lines and escapes
    int playDepth = 30;      // play-outs
    int confirm = 4;         // a candidate that passes is checked again this much deeper (0 = not)
    int threads = 3;
    int hashMB = 256;
    bool all = false;        // check every candidate, not only until the set is full
    bool dryRun = false;     // do not write the output file
};

// One candidate position, as read.
struct Candidate {
    std::string where;                 // "candidates.txt:123"
    coach::ChallengePosition pos;
    bool escape = false;               // the line is for the audit to find
    std::string error;                 // read error: dropped at once
};

struct Set {
    std::string id, group;
    int level = 1;
    int want = 0;
    std::vector<Candidate> candidates;
};

// Reads a candidates or endgames file into 'sets' (a challenge line names a set: a new one, or
// one read before). False with a message on a file that cannot be read.
bool readCandidates(const std::string& path, std::vector<Set>& sets, std::string& err);

// The engine's verdict on one candidate: "" when it is kept (pos then holds the checked line and
// its alternatives; note says what proved it: the scores of the player's moves...), else the
// reason, "<category>: <detail>".
class Oracle;
std::string checkCandidate(Oracle& o, const Set& set, const Candidate& c, coach::ChallengePosition& pos,
                           std::string& note);

// The embedded Stockfish, one search at a time.
class Oracle {
public:
    explicit Oracle(const Options& o);
    ~Oracle();
    bool ok() const { return ok_; }
    void newCandidate();                                  // clears the hash table
    void setExtraDepth(int plies) { extra_ = plies; }     // added to the depths below (confirmation)
    int depth() const { return opt_.depth + extra_; }     // lines and escapes
    int playDepth() const { return opt_.playDepth + extra_; }
    // MultiPV search of 'pos' (lines sorted best first; ok false on failure).
    ai::Analysis top(const chess::Position& pos, int lines, int depth);
    // The score of one move only (searchmoves).
    ai::Analysis only(const chess::Position& pos, const std::string& uci, int depth);
    const Options& options() const { return opt_; }

private:
    Options opt_;
    struct Impl;
    Impl* impl_;
    bool ok_ = false;
    int extra_ = 0;
};

}  // namespace challenges
