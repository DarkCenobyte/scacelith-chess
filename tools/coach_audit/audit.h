// coach_audit: an audit of the coach's review (src/coach/review.h) and of the Analysis mode's
// commentary (src/analysis/commentary.h) against the board.
//   coach_audit record --out FILE [--pgn FILE]... [--selfplay N --seed S] [--depth D] [--threads T]
//                      [--material-only] [--hang-rate R] [--positions FILE]... [--band LO HI]...
//                      [--stop FILE] [--puzzles FILE]... [--min-drop D]
//       runs the analyses a Coach game asks for on every move of the human side (embedded Stockfish);
//       --material-only: only the moves after which the coach can win 2 points or more by exchange
//       (the moves whose review shows a loss, a few times quicker to gather); --hang-rate R: in
//       self-play, the "human" plays with probability R a random move that leaves 3 points or more
//       to win by exchange (when one exists), for audits of the coach's demonstrations;
//       --positions FILE: in each undecided position before a recorded human move (a record file),
//       a move that costs what a --band says instead (every move of those that fit, one at random;
//       a band picked at random per position), for audits of bad moves only; --stop FILE: ends
//       before the next record once FILE exists (every record written stays whole);
//       --puzzles FILE: the first move of each puzzle of a Lichess puzzle CSV (the move the player
//       blundered into it, the side to move in its FEN being the human), tagged with the puzzle's
//       id, rating and themes; --min-drop D: of the PGN games' moves, only those that cost D win-%
//       points or more at depth 10 (each position searched once), tagged with the game's site,
//       ratings and time control; a game with an AuditPlies tag (ply numbers from 1, a sample's
//       picks) gives those plies only, without the scan, its AuditTag added to their tag
//   coach_audit reanalyse --in FILE --out FILE [--threads T] [--stop FILE]
//       the analyses of every record of FILE run again, the same requests at the same depth, and
//       written with the engine's win/draw/loss chances (records.cpp: "wdl"), the tag kept; a rerun
//       goes on after the records the output already holds
//   coach_audit truth --in FILE --out FILE [--depth D] [--threads T] [--stop FILE]
//       the reference the reviews are measured against: for every record of FILE, the position
//       before the human's move searched deeper (MultiPV 4, WDL, depth D, 22 by default for this
//       mode's callers; a fresh hash table each time) as a0, and the move played alone as a1 when
//       a0 holds no exact line for it; a rerun goes on after the records the output holds
//   coach_audit review --in FILE [--level L] [--out FILE] [--all] [--sequence]
//       replays each move through the Reviewer at every level (no engine), prints what the coach says
//       and shows, and checks every claim with board facts (oracles.cpp): a demonstration that stops
//       before the problem it announces, a piece named on the wrong square, a loss the best move
//       suffers too, a takeback offered for a small slip...
//       --sequence: the records of each game (a PGN game recorded without --min-drop, a self-play
//       game: each record continuing the last one's moves) through one Reviewer per level and side,
//       kept from move to move as in a Coach game:
//       the other side's moves played as the coach's (announced, remarked on: PRE lines), every
//       offer declined (DECL lines)
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
    bool sequence = false;  // review: one Reviewer per level and side through each game's records
    bool materialOnly = false;   // record: only the moves that leave material to win (2 points or more)
    double hangRate = 0.0;       // record, self-play: share of the human's moves that leave 3+ points to win
    double mistakeRate = 0.0;    // record, self-play: share of the human's moves picked among those that
                                 // cost 8 to 20 win-% points at depth 10 (around the mistake band)
    bool mistakesOnly = false;   // record, self-play: only the moves picked so (with --mistake-rate)
    std::vector<std::string> positions;   // record: record files whose positions get a bad move (--band)
    // record: the costs in win-% points at depth 12 of the moves picked (--mistake-rate, --positions),
    // one band picked at random per move; none = 10.5 to 14.5 (a mistake)
    std::vector<std::pair<double, double>> bands;
    std::string stop;            // record: ends before the next record once this file exists
    std::vector<std::string> puzzles;   // record: Lichess puzzle CSV files (the move that starts each)
    double minDrop = 0.0;        // record, PGN games: only the moves that cost this many win-% points
                                 // or more at depth 10 (0 = every move)
    int verify = 0;              // review: depth of the engine's check of every move shown or said (0 = none)
    std::string cache;           // review --verify: the verdicts kept between runs
};

// One human move and its analyses.
struct Record {
    std::string tag;                   // where the move comes from (a puzzle, a game), shown by review
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
int reanalyseMain(const Options& o);
int truthMain(const Options& o);
int reviewMain(const Options& o);
int commentaryMain(const Options& o);

}  // namespace audit
