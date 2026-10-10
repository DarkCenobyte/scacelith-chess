// coach_audit: an audit of the coach's review (src/coach/review.h) and of the Analysis mode's
// commentary (src/analysis/commentary.h) against the board.
//   coach_audit record --out FILE [--pgn FILE]... [--selfplay N --seed S] [--depth D] [--threads T]
//       runs the analyses a Coach game asks for on every move of the human side (embedded Stockfish)
//   coach_audit review --in FILE [--level L] [--out FILE] [--all]
//       replays each move through the Reviewer at every level (no engine), prints what the coach says
//       and shows, and checks every claim with board facts (oracles.cpp): a demonstration that stops
//       before the problem it announces, a piece named on the wrong square, a loss the best move
//       suffers too, a takeback offered for a small slip...
//   coach_audit commentary --in FILE [--out FILE] [--all]
//       the Analysis mode's comment on each recorded move (A0 before it, A2 or A1's line after it),
//       its material claims checked against the board (commentary.cpp)
#pragma once
#include "ai/analysis.h"
#include "chess/chess.h"
#include "coach/review.h"

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

int recordMain(const Options& o);
int reviewMain(const Options& o);
int commentaryMain(const Options& o);

}  // namespace audit
