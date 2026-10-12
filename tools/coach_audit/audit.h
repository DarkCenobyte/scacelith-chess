// coach_audit: an audit of the coach's review (src/coach/review.h) and of the Analysis mode's
// commentary (src/analysis/commentary.h) against the board.
//   coach_audit record --out FILE [--pgn FILE]... [--selfplay N --seed S] [--depth D] [--threads T]
//                      [--material-only] [--hang-rate R]
//       runs the analyses a Coach game asks for on every move of the human side (embedded Stockfish);
//       --material-only: only the moves after which the coach can win 2 points or more by exchange
//       (the moves whose review shows a loss, a few times quicker to gather); --hang-rate R: in
//       self-play, the "human" plays with probability R a random move that leaves 3 points or more
//       to win by exchange (when one exists), for audits of the coach's demonstrations
//   coach_audit review --in FILE [--level L] [--out FILE] [--all]
//       replays each move through the Reviewer at every level (no engine), prints what the coach says
//       and shows, and checks every claim with board facts (oracles.cpp): a demonstration that stops
//       before the problem it announces, a piece named on the wrong square, a loss the best move
//       suffers too, a takeback offered for a small slip...
//       --verify D [--threads T] [--cache FILE]: also every move the coach shows or says, judged by
//       the engine at depth D (verify.cpp: what it costs its mover against the best move there;
//       the cache keeps the verdicts between runs)
//   coach_audit commentary --in FILE [--out FILE] [--all]
//       the Analysis mode's comment on each recorded move (A0 before it, A2 or A1's line after it),
//       its material claims checked against the board (commentary.cpp)
#pragma once
#include "ai/analysis.h"
#include "chess/chess.h"
#include "coach/review.h"

#include <memory>
#include <string>
#include <vector>

namespace audit {

struct Options {
    std::string mode, out, in;
    std::vector<std::string> pgns;
    int selfplay = 0;
    unsigned seed = 1;
    int depth = 18;
    int threads = 1;
    int maxPlies = 160;
    int level = 0;          // review: 0 = every level
    bool all = false;       // review: print every reviewed move, not only those with a script
    bool materialOnly = false;   // record: only the moves that leave material to win (2 points or more)
    double hangRate = 0.0;       // record, self-play: share of the human's moves that leave 3+ points to win
    int verify = 0;              // review: depth of the engine's check of every move shown or said (0 = none)
    std::string cache;           // review --verify: the verdicts kept between runs
};

// One human move and its analyses.
struct Record {
    std::string startFen;              // "" = standard start
    std::vector<std::string> moves;    // before the human's move
    std::string played;                // the human's move (UCI)
    chess::Color human = chess::White;
    ai::Analysis a0, a1, a2, a3;
    bool hasA1 = false, hasA2 = false;
};

std::string writeRecord(const Record& r);
// Reads every record of a file's text; false on a syntax error (message in err).
bool readRecords(const std::string& text, std::vector<Record>& out, std::string& err);

// The engine's verdict on the moves of a line (review --verify): what a move costs its mover, in
// win percentage points, against the engine's best move in that position at a fixed depth.
class MoveJudge {
public:
    MoveJudge(int depth, int threads, const std::string& cachePath);
    ~MoveJudge();
    bool ok() const;
    // The points the side to move in 'p' gives away with 'm' (0 when it is the engine's best move,
    // which goes to 'best' in UCI); < 0 when the engine cannot tell.
    double cost(const chess::Position& p, const chess::Move& m, std::string* best = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

int recordMain(const Options& o);
int reviewMain(const Options& o);
int commentaryMain(const Options& o);

}  // namespace audit
