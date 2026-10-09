// Board facts for the coach's explanations: material and exchanges (SEE), hanging and trapped
// pieces, tactical motifs (fork, pin, skewer, discovered attack), checks and mates (mate in one,
// escape squares, back rank, mate patterns), king safety, pawn races, development, structured
// facts about a move and the path a finger traces for it.
//
// Stockfish decides whether a move is good or bad (review.h); these helpers say *why*, and every
// sentence the coach speaks about the board is checked with them first.
// Engine-free pure functions of chess::Position: no i18n, no allocation in the hot ones, safe from
// any thread. Square sets are bitboards (chess::squaresOf lists them).
#pragma once
#include "../chess/chess.h"
#include <cstdint>
#include <string>
#include <vector>

namespace coach {

// ---- Material -------------------------------------------------------------------------------
constexpr int kPieceValueCp[7] = {0, 100, 320, 330, 500, 900, 20000};  // exchanges (the king: large)
constexpr int kPiecePoints[7] = {0, 1, 3, 3, 5, 9, 0};                 // speech: "a knight is worth 3"

int material(const chess::Position& p, chess::Color c);             // points of c's pieces, king excluded
int materialBalance(const chess::Position& p, chess::Color pov);    // pov's points minus the opponent's
int majorsAndMinors(const chess::Position& p);                      // knights, bishops, rooks, queens, both sides

// Static exchange evaluation of 'm' on its target square, centipawns for the mover (the mover is
// the colour of the piece on m.from, so it works for the side not to move too). Every capture on
// the square is taken into account, least valuable attacker first, with x-rays; pins and checks
// are ignored and a king never captures a defended piece. A quiet move gives 0 or the loss of the
// moved piece when the opponent can take it with profit.
int see(const chess::Position& p, const chess::Move& m);
// Best exchange balance for 'by' starting a capture on 'sq' (cp; <= 0 when no capture wins
// anything; 0 when 'by' has no attacker or the square is empty or holds a piece of 'by').
int seeSquare(const chess::Position& p, chess::Square sq, chess::Color by);
// The same two exchanges counted in points (kPiecePoints: a knight and a bishop are worth the
// same), the unit the coach speaks in. Whatever a sentence says about winning or losing material
// is decided with these: in centipawns a knight taking a defended bishop "wins" 10.
int seePoints(const chess::Position& p, const chess::Move& m);
int seeSquarePoints(const chess::Position& p, chess::Square sq, chess::Color by);
// The most 'by' wins with one exchange anywhere on the board (points, 0 when nothing pays): what
// the side to move takes back where a line of moves stops. For the side to move, its legal
// captures only (nothing when it is mated or stalemated, only an answer to a check).
int bestCapturePoints(const chess::Position& p, chess::Color by);
// Pieces of each side that take part in an exchange on 'sq' (it holds a piece): the attackers of
// the square and the sliders behind them on its lines (x-rays), without a piece pinned to its king
// off the line to 'sq'. directOnly: the attackers seen at once, pinned or not (what a player counts
// at first glance).
uint64_t exchangeParticipants(const chess::Position& p, chess::Square sq, bool directOnly);
// c's pieces (king excluded) the opponent attacks and can win. undefendedOnly (levels 1-2):
// attacked and not defended at all ("it has no protector"); otherwise any piece whose capture wins
// material (seeSquarePoints > 0: also a piece defended but attacked by a cheaper one).
uint64_t hangingPieces(const chess::Position& p, chess::Color c, bool undefendedOnly);
// The piece on s is attacked and nothing of its own colour defends it.
bool isUndefended(const chess::Position& p, chess::Square s);

// ---- Tactical motifs ------------------------------------------------------------------------
// The piece on 'sq' (just moved there) attacks two or more enemy targets that are the king, worth
// more than it in points, or winning by SEE, and stands safe there (no enemy capture of it breaks
// even). Returns the targets (0 when fewer than two).
uint64_t forkTargets(const chess::Position& after, chess::Square sq);

struct Pin {
    chess::Square pinner = chess::NoSquare, pinned = chess::NoSquare, behind = chess::NoSquare;
    bool absolute = false;   // behind is the king: the pinned piece may not move off the line
};
// Pieces of 'victim' pinned by an enemy slider, to the king or to a piece worth more points. A
// pinned piece that can capture its pinner along the line is not listed (the pin does not hold).
std::vector<Pin> pins(const chess::Position& p, chess::Color victim);

struct Skewer {
    chess::Square attacker = chess::NoSquare, front = chess::NoSquare, behind = chess::NoSquare;
};
// An enemy slider attacks a piece of 'victim' (the king, or a piece worth more points than the one
// behind it) and another piece of 'victim' stands behind it on the same line.
std::vector<Skewer> skewers(const chess::Position& p, chess::Color victim);

struct Discovery {
    chess::Square slider = chess::NoSquare, target = chess::NoSquare;
    bool check = false;      // the target is the king
};
// Attacks move 'm' (legal in 'before') uncovers: a slider of the mover now attacks an enemy piece
// through the square the moved piece left. Castling uncovers nothing.
std::vector<Discovery> discoveredAttacks(const chess::Position& before, const chess::Move& m);
bool isDoubleCheck(const chess::Position& p);

// ---- Checks, mates, back rank ------------------------------------------------------------------
bool mateInOne(const chess::Position& p, chess::Move* out = nullptr);   // the side to move mates at once
// 'by' (either side) would mate in one if it were its turn: the threat after the opponent's move.
bool mateThreat(const chess::Position& p, chess::Color by, chess::Move* out = nullptr);
std::vector<chess::Move> checkingMoves(const chess::Position& p);
// Squares next to c's king it could step to: empty or enemy-held, and not attacked once the king
// has left its square (so a square behind the king on a checking line is not counted).
uint64_t escapeSquares(const chess::Position& p, chess::Color c);
// c's king stands on its first rank with no flight square off it, and the opponent has a rook or
// a queen: a heavy piece reaching that rank mates unless it is covered (confirm with the engine).
bool backRankWeak(const chess::Position& p, chess::Color c);
enum class MatePattern : uint8_t { None, BackRank, Smothered, Support, Ladder, Epaulette, Other };
MatePattern classifyMate(const chess::Position& mated);   // the side to move is checkmated

// ---- Pieces and pawns ------------------------------------------------------------------------
// The piece on s (knight, bishop, rook or queen) is attacked with a winning capture and every move
// it has loses by SEE too, in points (lichess-puzzler's is_trapped); false when its side is in
// check or the piece is pinned. Works for either colour (the turn is passed on a copy when needed).
bool isTrapped(const chess::Position& p, chess::Square s);
bool isPassed(const chess::Position& p, chess::Square pawn);
int undevelopedMinors(const chess::Position& p, chess::Color c);   // knights / bishops on b1 c1 f1 g1 (b8 c8 f8 g8)
bool isForced(const chess::Position& p);                  // exactly one legal move
// Move 'ply' of the game captures on the square where the previous move captured.
bool isRecapture(const chess::Game& g, size_t ply);
// King zone: the (up to 8) squares next to c's king that the opponent attacks.
int kingZoneAttacks(const chess::Position& p, chess::Color c);
// Rule of the square: the pawn on 'pawn' promotes before the enemy king can catch it (no help from
// other pieces). defenderToMove: the king's side moves next.
bool outsideSquare(const chess::Position& p, chess::Square pawn, bool defenderToMove);
chess::Square promotionSquare(const chess::Position& p, chess::Square pawn);

// ---- Game phase (lichess Divider) -------------------------------------------------------------
// Index of the first position (after that many plies) that starts the middlegame / the endgame, -1
// when none: majors and minors <= 10, a sparse back rank or a mixed board start the middlegame;
// majors and minors <= 6 the endgame (only once a middlegame exists).
struct Phases { int middlegame = -1, endgame = -1; };
Phases dividePhases(const chess::Game& g);
int phaseOfPly(const Phases& ph, int ply);   // 0 opening, 1 middlegame, 2 endgame (the position before 'ply')

// ---- Moves: facts, lines, paths ----------------------------------------------------------------
struct MoveFacts {
    chess::Color mover = chess::White;
    chess::PieceType piece = chess::NoPiece;
    chess::Square from = chess::NoSquare, to = chess::NoSquare;
    chess::PieceType captured = chess::NoPiece;
    chess::Square capturedOn = chess::NoSquare;    // en passant: the passed pawn's square
    bool enPassant = false, castleKing = false, castleQueen = false;
    chess::PieceType promotion = chess::NoPiece;
    bool check = false, doubleCheck = false, discoveredCheck = false, stalemate = false;
    int seeCp = 0;               // exchange balance of the move for the mover (see())
    int seePts = 0;              // the same in points (seePoints()): what the coach says about it
    uint64_t newlyAttacked = 0;  // enemy pieces attacked after the move and not before
    uint64_t forks = 0;          // forkTargets(after, to)
    uint64_t undefended = 0;     // own pieces the moved piece defended before and no longer does
};
MoveFacts analyzeMove(const chess::Position& before, const chess::Move& m);

// One ply of a line replayed on a copy of the board (a Stockfish PV).
struct LineStep {
    chess::Move move;
    std::string uci, san;
    chess::Color mover = chess::White;
    chess::PieceType piece = chess::NoPiece;
    chess::PieceType captured = chess::NoPiece;
    chess::PieceType promotion = chess::NoPiece;
    bool check = false, mate = false, stalemate = false;
    int balance = 0;             // materialBalance(after, pov): the listener's material lead after the ply
    bool guessed = false;        // not the engine's: a natural reply added where its line stopped
};
// The legal prefix of 'uci' from 'start' (at most maxPlies), the balance seen by 'pov'.
std::vector<LineStep> replayLine(const chess::Position& start, const std::vector<std::string>& uci, chess::Color pov,
                                 int maxPlies = 64);
std::string sanLine(const std::vector<LineStep>& steps, size_t from, size_t count);   // "Nxe5 dxe5 Qg4"

// Squares a finger passes over for a move of a piece of type t, in order, 'to' last: a slider the
// squares in between, a knight an L (long leg first: g1-f3 gives g2 g3 f3), a king or a pawn just
// 'to' (a double step its middle square too).
std::vector<chess::Square> movePath(chess::PieceType t, chess::Square from, chess::Square to);
// Gesture path of a move: {from, to}, or {from, corner, to} for a knight (the L's corner, long leg
// first: g1-f3 turns on g3), as the animator's Trace and the board's arrows expect.
std::vector<chess::Square> tracePath(chess::PieceType t, chess::Square from, chess::Square to);
chess::Square knightCorner(chess::Square from, chess::Square to);   // NoSquare unless a knight's jump

}  // namespace coach
