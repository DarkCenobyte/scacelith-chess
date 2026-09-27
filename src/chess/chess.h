// Chess rules engine (standard FIDE rules) + tournament arbitration + clock.
// Pure C++, no GL, no global state. Implemented by the chess-rules work package.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace chess {

enum Color : uint8_t { White = 0, Black = 1 };
inline Color opposite(Color c) { return Color(c ^ 1); }

enum PieceType : uint8_t { NoPiece = 0, Pawn = 1, Knight = 2, Bishop = 3, Rook = 4, Queen = 5, King = 6 };

struct Piece {
    PieceType type = NoPiece;
    Color color = White;
    bool empty() const { return type == NoPiece; }
    bool operator==(const Piece& o) const { return type == o.type && (type == NoPiece || color == o.color); }
};

// Squares 0..63: a1 = 0, b1 = 1, ..., h1 = 7, a2 = 8, ..., h8 = 63. -1 = none.
using Square = int8_t;
constexpr Square NoSquare = -1;
inline int fileOf(Square s) { return s & 7; }
inline int rankOf(Square s) { return s >> 3; }
inline Square makeSquare(int file, int rank) { return Square(rank * 8 + file); }
std::string squareName(Square s);           // "e4"
Square parseSquare(const std::string& s);   // "e4" -> 28, NoSquare on error

enum MoveFlags : uint8_t {
    MoveQuiet = 0, MoveCapture = 1, MoveEnPassant = 2, MoveCastleKing = 4, MoveCastleQueen = 8, MoveDoublePush = 16, MovePromotion = 32
};

struct Move {
    Square from = NoSquare, to = NoSquare;
    PieceType promotion = NoPiece;
    uint8_t flags = 0;
    bool valid() const { return from != NoSquare; }
    bool operator==(const Move& o) const { return from == o.from && to == o.to && promotion == o.promotion; }
};

enum CastlingRights : uint8_t { WhiteKingSide = 1, WhiteQueenSide = 2, BlackKingSide = 4, BlackQueenSide = 8 };

class Position {
public:
    Position();                                   // standard starting position
    void setStart();
    bool setFEN(const std::string& fen);
    std::string fen() const;

    Piece at(Square s) const { return board_[s]; }
    Color sideToMove() const { return side_; }
    uint8_t castling() const { return castling_; }
    Square epSquare() const { return ep_; }
    int halfmoveClock() const { return halfmove_; }
    int fullmoveNumber() const { return fullmove_; }
    uint64_t hash() const { return hash_; }       // Zobrist (repetition detection; includes ep only when capturable)

    std::vector<Move> legalMoves() const;
    std::vector<Move> legalMovesFrom(Square from) const;
    bool isLegal(const Move& m) const;            // also fills nothing; compare from/to/promotion
    Move findLegal(Square from, Square to, PieceType promo = NoPiece) const;  // with flags, or invalid
    bool inCheck() const;
    bool isAttacked(Square s, Color by) const;
    Square kingSquare(Color c) const;
    void makeMove(const Move& m);                 // m must be legal (use findLegal)
    bool hasInsufficientMaterial() const;         // neither side can mate (dead position subset)
    bool canColorMate(Color c) const;             // false when c has K, K+N, K+B only (FIDE 6.9)

    std::string toSAN(const Move& m) const;       // "Nbd7", "exd8=Q+", "O-O#"
    std::string toUCI(const Move& m) const;       // "e7e8q"
    Move parseUCI(const std::string& s) const;
    Move parseSAN(const std::string& s) const;
    uint64_t perft(int depth) const;

private:
    Piece board_[64];
    Color side_ = White;
    uint8_t castling_ = 0xF;
    Square ep_ = NoSquare;
    int halfmove_ = 0, fullmove_ = 1;
    uint64_t hash_ = 0;
    void recomputeHash();
    void pseudoMoves(std::vector<Move>& out) const;
};

enum class GameStatus : uint8_t { Ongoing, WhiteWins, BlackWins, Draw };
enum class GameEndReason : uint8_t {
    None, Checkmate, Resignation, Timeout, IllegalMoves,          // decisive
    Stalemate, InsufficientMaterial, TimeoutVsInsufficient,       // draws
    FivefoldRepetition, SeventyFiveMoves, ThreefoldClaim, FiftyMoveClaim, Agreement
};
const char* endReasonText(GameEndReason r);  // "Checkmate", "Threefold repetition (claimed)", ...

// Game record: positions, moves, repetition and draw rules. Starts from the standard position.
class Game {
public:
    Game();
    void reset();
    const Position& position() const { return positions_.back(); }
    const std::vector<Move>& moves() const { return moves_; }
    const std::vector<std::string>& sanMoves() const { return san_; }
    std::vector<std::string> uciMoves() const;
    bool play(const Move& m);                     // legal move; updates status (mate, stalemate, 5-fold, 75, material)
    GameStatus status() const { return status_; }
    GameEndReason endReason() const { return reason_; }
    int repetitionCount() const;                  // occurrences of the current position
    bool canClaimThreefold() const;               // current position occurred >= 3 times
    bool canClaimFiftyMove() const;               // halfmove clock >= 100
    void claimDraw();                             // applies the first valid claim, if any
    void resign(Color loser);
    void agreeDraw();
    void flagFall(Color flagged);                 // Timeout, or draw if the opponent cannot mate
    void forfeitIllegal(Color offender);          // second completed illegal move
    std::string pgn(const std::string& whiteName, const std::string& blackName) const;

private:
    std::vector<Position> positions_;
    std::vector<Move> moves_;
    std::vector<std::string> san_;
    GameStatus status_ = GameStatus::Ongoing;
    GameEndReason reason_ = GameEndReason::None;
    void finish(GameStatus s, GameEndReason r);
};

// ---- Clock -------------------------------------------------------------------------------------
struct TimeControl {
    bool unlimited = false;
    int64_t baseMs = 5 * 60 * 1000;
    int64_t incrementMs = 0;       // Fischer increment, added after each completed move
    int64_t delayMs = 0;           // Bronstein-style delay (custom only; 0 = off)
    std::string label() const;     // "Unlimited", "5+3", "15+10", "Custom 7+2 d3"
    // FIDE category of the game (used for illegal move penalties): estimated duration for 60 moves.
    enum class Category { Blitz, Rapid, Standard, Unlimited };
    Category category() const;
};
const std::vector<TimeControl>& timeControlPresets();  // Unlimited, 1+0, 3+0, 3+2, 5+0, 5+3, 10+0, 10+5, 15+10, 30+0, 30+20, 90+30

class Clock {
public:
    void setup(const TimeControl& tc);
    void start(Color running);                    // starts the given side (game start: White)
    void stop();
    // The player 'mover' pressed the clock at the end of their move: adds increment, switches.
    void press(Color mover);
    void update(int64_t elapsedMs);               // advance the running side
    int64_t remainingMs(Color c) const;
    Color running() const { return running_; }
    bool isRunning() const { return active_; }
    bool flagged(Color c) const;
    void addTime(Color c, int64_t ms);            // arbiter penalties
    const TimeControl& timeControl() const { return tc_; }
    int64_t delayLeftMs() const { return delayLeft_; }
private:
    TimeControl tc_;
    int64_t remaining_[2] = {0, 0};
    int64_t delayLeft_ = 0;
    Color running_ = White;
    bool active_ = false;
};

// ---- Tournament arbitration ("physical" board) --------------------------------------------------
// Models the physical board the players manipulate, which may temporarily hold an illegal move
// when legal-move hints are disabled. Enforces FIDE Article 4 (touch-move) and Article 7.5
// (illegal moves are only "completed" when the clock is pressed).
class Arbiter {
public:
    void reset(const Game& game);
    // Touch-move: the player touched their own piece on 'sq'. Returns false if a different piece
    // is already committed this turn. After touching, the player must move that piece if it has
    // at least one legal move (mustMoveTouched()).
    bool touch(Square sq);
    Square touchedSquare() const { return touched_; }
    bool touchedHasLegalMove(const Game& game) const;
    // The player released the touched piece on 'to' (any square not holding an own piece).
    // promotion: chosen piece when a pawn reaches the last rank. Records the physical move;
    // nothing is final until clockPressed().
    struct Placement { Square from, to; PieceType promotion; bool castlingRookMove; };
    bool place(const Game& game, Square to, PieceType promotion);
    bool hasPendingMove() const { return pending_; }
    Placement pendingMove() const { return placement_; }
    bool pendingNeedsPromotion(const Game& game) const;   // pawn released on the last rank without a choice yet
    // Clock press: completes the move. Returns the verdict; for a legal move 'move' holds the
    // move to play on the Game; for an illegal move the physical board must be restored and the
    // arbiter reports the penalty (time bonus for the opponent in ms, or forfeit).
    struct Verdict {
        bool legal = false;
        Move move;
        int64_t opponentBonusMs = 0;   // 2 min standard, 1 min rapid/blitz (FIDE 7.5.5 / A.4.2)
        bool forfeit = false;          // second completed illegal move
        std::string message;           // English, shown to the player
    };
    Verdict clockPressed(const Game& game, const TimeControl& tc);
    void cancelTouch();                 // for touched pieces with no legal move only
    int illegalCount(Color c) const { return illegal_[c]; }
private:
    Square touched_ = NoSquare;
    bool pending_ = false;
    Placement placement_{};
    int illegal_[2] = {0, 0};
};

}  // namespace chess
