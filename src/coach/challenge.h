// Coach mode's challenges: short sets of positions, each set training one skill (mates in one to
// three, forks, pins, the escape of a king in danger, basic endgames...). The coach sets each
// position up on the board, plays the move that leads into it when there is one, and waits for
// the player's answer; the player always plays White (positions where Black was to move are
// mirrored when the file is made). Nothing is recorded: no scoresheet, no clock, no rating, no
// saved game. Only the completion of a whole set is remembered (Settings::coachChallengesDone).
//
// Two kinds of positions:
//   - a line (ChallengeGoal::Line): the solution is known. The player's moves are judged against
//     it (with the other moves the audit accepted, `also`), the coach answers with the line's own
//     moves;
//   - a play-out (Mate, Promote, Hold): a basic endgame played against Stockfish to the goal,
//     every move of the player judged by the engine (the session's part).
//
// The positions are in assets/coach/challenges/challenges.txt (the format is in its header),
// made by tools/challenges/select.py from the Lichess puzzle database (CC0) and checked with the
// embedded Stockfish by tools/challenge_audit, which also fills the accepted alternatives.
//
// Engine-free: every chess fact here comes from chess::Position.
#pragma once
#include "../chess/chess.h"
#include <string>
#include <vector>

namespace coach {

enum class ChallengeGoal : uint8_t {
    Line,      // play the solution: the coach answers with the line's moves
    Mate,      // play it out: checkmate the coach (it defends with Stockfish)
    Promote,   // play it out: promote a pawn and keep the win
    Hold       // play it out: hold the draw for 'moves' moves against the coach's attempts to win
};
const char* challengeGoalName(ChallengeGoal g);   // "line", "mate", "promote", "hold"

struct ChallengePosition {
    std::string source;                      // "lichess:0000D" (a trailing "/m": mirrored), "scacelith:kq1"
    int rating = 0;                          // the puzzle's rating (Lichess), 0 = none
    std::string fen;                         // the set-up
    std::string lead;                        // UCI: the coach's move into the position, played by hand
                                             // once the set-up stands ("" = none: White to move at once)
    ChallengeGoal goal = ChallengeGoal::Line;
    // Line: the solution after the lead, UCI: the player's move, the coach's answer, ..., ending
    // with the player's move (an odd count).
    std::vector<std::string> line;
    // Line: other moves accepted for the player's move k (line[2k]); filled by the audit (another
    // mate in the same number of moves, an equal win at the last move). Never at a move the coach
    // still answers: those positions have one solution.
    std::vector<std::vector<std::string>> also;
    int moves = 0;                           // Hold: the player's moves to hold the draw

    int playerMoves() const { return int(line.size() + 1) / 2; }
    bool playOut() const { return goal != ChallengeGoal::Line; }
    // The line ends in checkmate (the last move of the line mates).
    bool endsInMate() const;
    // The position the player moves from first (the set-up after the lead). False when the FEN or
    // the lead is not legal.
    bool start(chess::Position& out) const;
    // The position before the player's move k (0-based) of a line: the start, then the line's
    // moves before line[2k]. False when it does not exist or a move is not legal.
    bool beforeMove(int k, chess::Position& out) const;
    // Is 'uci' a solution of the player's move k? The line's move, a move of also[k], or any
    // checkmate.
    bool accepts(int k, const chess::Position& before, const std::string& uci) const;
};

struct Challenge {
    std::string id;                          // UI texts challenge.<id>.name / .desc (assets/i18n), speech
                                             // ch.intro.<id> and ch.task.<id> (assets/coach/speech)
    std::string group;                       // "mates", "tactics", "defence", "endgames"
    int level = 1;                           // difficulty 1..5
    std::vector<ChallengePosition> positions;
};

class ChallengeBook {
public:
    // The game's challenges, from the embedded file (parsed on first use; thread-safe).
    static const ChallengeBook& shared();
    // Parses the text of a challenges file. Lines it cannot read are left out and reported in
    // *errors ("line 12: ..."); the FEN and moves are checked for legality here too.
    static ChallengeBook parse(const std::string& text, std::vector<std::string>* errors = nullptr);
    // Writes the book in the file's format (the audit's output; parse(write()) gives it back).
    std::string write() const;

    const std::vector<Challenge>& challenges() const { return challenges_; }
    std::vector<Challenge>& challenges() { return challenges_; }
    const Challenge* find(const std::string& id) const;
    int indexOf(const std::string& id) const;   // -1 when unknown
    // The groups in menu order, each once ("mates", "tactics", ...).
    std::vector<std::string> groups() const;

private:
    std::vector<Challenge> challenges_;
};

// ---- Hints --------------------------------------------------------------------------------------
// What the coach shows when the player asks for a hint about the move 'uci' (a legal move of
// 'pos'), the step-th time at this move (1-based):
//   1. the piece to play, or the square to look at when that piece is the only one with a legal
//      move (naming it would say nothing);
//   2. the square to look at (the move's target);
//   3. and later: the move itself, demonstrated.
struct ChallengeHint {
    enum class Kind : uint8_t { None, Piece, Square, Show } kind = Kind::None;
    chess::Square piece = chess::NoSquare;   // the piece to move (Piece; Show: the move's from)
    chess::Square square = chess::NoSquare;  // the target (Square; Show: the move's to)
};
ChallengeHint challengeHint(const chess::Position& pos, const std::string& uci, int step);
// The number of hint steps a move has (2 when the first hint already gives the square, else 3).
int challengeHintSteps(const chess::Position& pos, const std::string& uci);

// The mirror of a position: ranks reversed, colours swapped (castling rights and the en passant
// square follow). A position with Black to move becomes the same problem with White to move.
std::string mirrorFen(const std::string& fen);
std::string mirrorUci(const std::string& uci);

}  // namespace coach
