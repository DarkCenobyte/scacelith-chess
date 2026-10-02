// Chess rules engine (standard FIDE rules) + tournament arbitration + clock.
// Pure C++, no GL, no global state of its own (all tables are compile-time constants), thread-safe
// by construction: independent objects may be used from different threads. Only the texts read the
// i18n tables (main thread): endReasonText() and the Arbiter's verdict messages in the current UI
// language, Game::pgn()'s end-reason comment in English (i18n::english).
//
// Layers (kept independent from presentation so that local/online multiplayer can reuse them):
//   Position  - board state, legal move generation (bitboards), SAN/UCI/FEN, perft.
//   Game      - game record, automatic endings (mate, stalemate, dead position, 5-fold, 75 moves),
//               claims (3-fold, 50 moves), resignation, agreement, flag fall, PGN export.
//   Clock     - digital chess clock (Fischer increment, Bronstein delay, flag).
//   Arbiter   - "physical board" referee: touch-move (FIDE Art. 4) and illegal moves (Art. 7.5).
#pragma once
#include <cstddef>
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
    bool operator!=(const Piece& o) const { return !(*this == o); }
};

// Squares 0..63: a1 = 0, b1 = 1, ..., h1 = 7, a2 = 8, ..., h8 = 63. -1 = none.
using Square = int8_t;
constexpr Square NoSquare = -1;
inline int fileOf(Square s) { return s & 7; }
inline int rankOf(Square s) { return s >> 3; }
inline Square makeSquare(int file, int rank) { return Square(rank * 8 + file); }
std::string squareName(Square s);           // "e4" ("-" for NoSquare)
Square parseSquare(const std::string& s);   // "e4" -> 28, NoSquare on error

// Square sets (bitboards, bit s = square s) for the board queries of Position and the coach.
inline uint64_t squareBit(Square s) { return uint64_t(1) << s; }
std::vector<Square> squaresOf(uint64_t set);  // ascending order (a1 first)
int squareCount(uint64_t set);
// Squares a piece of type t and colour c standing on s attacks with the board occupancy 'occ'
// (sliders stop at the first piece; a pawn: its two capture squares).
uint64_t attacksOf(PieceType t, Color c, Square s, uint64_t occ);
// Squares strictly between two squares on one line (rank, file or diagonal); 0 when not aligned.
uint64_t squaresBetween(Square a, Square b);

// An en passant capture carries MoveCapture | MoveEnPassant; a capturing promotion carries
// MoveCapture | MovePromotion.
enum MoveFlags : uint8_t {
    MoveQuiet = 0, MoveCapture = 1, MoveEnPassant = 2, MoveCastleKing = 4, MoveCastleQueen = 8, MoveDoublePush = 16, MovePromotion = 32
};

struct Move {
    Square from = NoSquare, to = NoSquare;
    PieceType promotion = NoPiece;
    uint8_t flags = 0;
    bool valid() const { return from != NoSquare; }
    bool operator==(const Move& o) const { return from == o.from && to == o.to && promotion == o.promotion; }
    bool operator!=(const Move& o) const { return !(*this == o); }
};

enum CastlingRights : uint8_t { WhiteKingSide = 1, WhiteQueenSide = 2, BlackKingSide = 4, BlackQueenSide = 8 };

class Position {
public:
    Position();                                   // standard starting position
    void setStart();
    // Parses a FEN (the halfmove/fullmove fields are optional). Returns false (position unchanged)
    // on malformed or impossible input (missing/extra kings, pawns on the back ranks, side not to
    // move in check, more than 16 pieces or 8 pawns per side). Normalisation: castling rights
    // whose king/rook are not on their initial squares are dropped, and the en passant square is
    // kept only when an en passant capture is actually legal (FIDE 9.2.3), so fen() may differ.
    bool setFEN(const std::string& fen);
    std::string fen() const;

    Piece at(Square s) const { return board_[s]; }
    Color sideToMove() const { return side_; }
    uint8_t castling() const { return castling_; }
    Square epSquare() const { return ep_; }       // only when a legal en passant capture exists
    int halfmoveClock() const { return halfmove_; }
    int fullmoveNumber() const { return fullmove_; }
    uint64_t hash() const { return hash_; }       // Zobrist (repetition detection; includes ep only when capturable)

    std::vector<Move> legalMoves() const;
    std::vector<Move> legalMovesFrom(Square from) const;
    bool isLegal(const Move& m) const;            // compares from/to/promotion (flags ignored)
    // Legal move with its flags, or an invalid Move. 'promo' is required (Knight..Queen) for a
    // promotion and ignored for any other move.
    Move findLegal(Square from, Square to, PieceType promo = NoPiece) const;
    bool inCheck() const;
    bool isAttacked(Square s, Color by) const;
    Square kingSquare(Color c) const;
    void makeMove(const Move& m);                 // m must be legal (use findLegal); flags are re-derived
    bool hasInsufficientMaterial() const;         // dead position: K v K, K+B v K, K+N v K, only same-coloured bishops
    // FIDE 6.9 / 7.5.5 approximation: false when c has a bare king, when c has a single minor
    // piece and the opponent a bare king, or when the position is dead; true otherwise.
    bool canColorMate(Color c) const;

    bool hasLegalMove() const;                    // false = checkmate or stalemate
    bool isCheckmate() const { return inCheck() && !hasLegalMove(); }
    bool isStalemate() const { return !inCheck() && !hasLegalMove(); }
    // Repetition identity (FIDE 9.2.3): placement, side to move, castling rights, en passant.
    bool samePosition(const Position& o) const;
    bool isStandardStart() const;                 // the standard starting position at move 1, halfmove clock 0

    // ---- Board queries for explanations (the coach's tactics, src/coach/tactics.h) ----------------
    // Square sets are bitboards: bit s stands for square s (a1 = bit 0); squaresOf() lists them.
    uint64_t pieces(Color c) const { return colorBB_[c]; }
    uint64_t pieces(PieceType t) const { return typeBB_[t]; }
    uint64_t pieces(Color c, PieceType t) const { return colorBB_[c] & typeBB_[t]; }
    uint64_t occupancy() const { return occupied(); }
    // Pieces of both colours attacking s when the board holds 'occ' (pawns by their capture
    // pattern, the king included). Pass an occupancy without pieces already exchanged on s to see
    // the x-rays behind them; the result only holds pieces present in 'occ'.
    uint64_t attackersTo(Square s, uint64_t occ) const;
    uint64_t attackersTo(Square s, Color by) const { return attackersTo(s, occupied()) & colorBB_[by]; }
    // Squares the piece on s attacks (a pawn: its two capture squares, whatever stands there); 0 if s is empty.
    uint64_t attacksFrom(Square s) const;
    uint64_t checkers() const;                    // pieces giving check to the side to move
    uint64_t pinned(Color c) const;               // c's pieces pinned to their own king (absolute pins)
    // Null move for analysis: the other side is to move (en passant cleared, halfmove clock + 1,
    // hash updated). False, and nothing changes, when the side to move is in check. The result is
    // for questions such as "what does the opponent threaten": never feed it to a Game or the arbiter.
    bool passTurn();

    std::string toSAN(const Move& m) const;       // "Nbd7", "exd8=Q+", "O-O#" ("" if m is not legal)
    std::string toUCI(const Move& m) const;       // "e7e8q"
    Move parseUCI(const std::string& s) const;    // invalid Move if not legal here
    // Tolerant: "0-0", "O-O", missing or extra "+"/"#"/"!?", "e8Q", "e8=Q", "e8(Q)", "exd6e.p.",
    // "Ng1f3", "Ng1-f3", "1.e4", lowercase piece letters, and plain UCI ("e2e4").
    Move parseSAN(const std::string& s) const;
    uint64_t perft(int depth) const;

private:
    Piece board_[64];
    Color side_ = White;
    uint8_t castling_ = 0xF;
    Square ep_ = NoSquare;
    int halfmove_ = 0, fullmove_ = 1;
    uint64_t hash_ = 0;
    uint64_t colorBB_[2] = {0, 0};                 // occupancy per colour
    uint64_t typeBB_[7] = {0, 0, 0, 0, 0, 0, 0};   // occupancy per piece type (index = PieceType)
    void recomputeHash();
    void clear();
    void putPiece(Square s, Piece p);
    void removePiece(Square s);
    uint64_t occupied() const { return colorBB_[0] | colorBB_[1]; }
    bool attackedWith(Square s, Color by, uint64_t occ, uint64_t removed) const;
    uint64_t pinnedPieces(Color c, Square ksq) const;
    bool legalFull(const Move& m, Square ksq) const;
    int generatePseudo(Move* out) const;          // buffer of at least 256 moves
    int generateLegal(Move* out) const;
    void setEpIfCapturable(Square ep);
    uint64_t perftRec(int depth) const;
    Move parseSANStrict(const std::string& s) const;
};

enum class GameStatus : uint8_t { Ongoing, WhiteWins, BlackWins, Draw };
enum class GameEndReason : uint8_t {
    None, Checkmate, Resignation, Timeout, IllegalMoves,          // decisive
    Stalemate, InsufficientMaterial, TimeoutVsInsufficient,       // draws
    FivefoldRepetition, SeventyFiveMoves, ThreefoldClaim, FiftyMoveClaim, Agreement,
    IllegalMovesVsInsufficient                                    // draw: 2nd illegal move but the opponent cannot mate (7.5.5)
};
// Translation key of a reason in assets/i18n/*.lang ("reason.checkmate"; "" for None).
const char* endReasonKey(GameEndReason r);
// The reason in the current UI language (i18n): "Checkmate", "Threefold repetition (claimed)", ...
const char* endReasonText(GameEndReason r);
// PGN Termination value ("normal", "time forfeit", "rules infraction"; "unterminated" while ongoing).
const char* terminationTag(GameStatus status, GameEndReason reason);

// Optional PGN header values (the Seven Tag Roster is always written).
struct PgnTags {
    std::string event = "Casual game";
    std::string site = "Scacelith";
    std::string date;               // "YYYY.MM.DD"; empty = today (UTC)
    std::string round = "-";
    std::string timeControl = "?";  // PGN TimeControl value, see TimeControl::pgnTag()
};

// Game record: positions, moves, repetition and draw rules. Starts from the standard position.
class Game {
public:
    Game();
    void reset();
    bool resetFromFEN(const std::string& fen);    // custom start position (tests, analysis); false = invalid FEN
    const Position& position() const { return positions_.back(); }
    const Position& startPosition() const { return positions_.front(); }
    // Position before move 'ply' (0 = the start position, moves().size() = the current one); clamped.
    const Position& positionAt(size_t ply) const { return positions_[ply < positions_.size() ? ply : positions_.size() - 1]; }
    const std::vector<Move>& moves() const { return moves_; }
    const std::vector<std::string>& sanMoves() const { return san_; }
    std::vector<std::string> uciMoves() const;
    bool play(const Move& m);                     // legal move; updates status (mate, stalemate, 5-fold, 75, material)
    // Takes the last 'plies' moves back (a takeback, a demonstration line undone): the record is as
    // if they had never been played. The status is derived again from the position reached, so an
    // ending the moves brought (mate, stalemate, repetition) is lifted, and so is any other ending
    // (resignation, agreement, flag fall, claim, forfeit): taking moves back reopens the game.
    // False (nothing changes) unless 1 <= plies <= moves().size().
    bool undo(int plies = 1);
    GameStatus status() const { return status_; }
    GameEndReason endReason() const { return reason_; }
    bool isOver() const { return status_ != GameStatus::Ongoing; }
    // Off: the game never ends by itself (mate, stalemate, dead position, 5-fold, 75 moves): the
    // rules lesson of Coach mode plays exercises on positions that are over on load (two kings
    // alone) and goes on after a mate. On by default; reset() and resetFromFEN() keep the setting.
    void setEndDetection(bool on) { endDetection_ = on; }
    bool endDetection() const { return endDetection_; }
    const char* resultString() const;             // "1-0", "0-1", "1/2-1/2", "*"
    int repetitionCount() const;                  // occurrences of the current position
    bool canClaimThreefold() const;               // current position occurred >= 3 times
    bool canClaimFiftyMove() const;               // halfmove clock >= 100
    void claimDraw();                             // applies the first valid claim, if any
    void resign(Color loser);
    void agreeDraw();
    void flagFall(Color flagged);                 // Timeout, or draw if the opponent cannot mate
    void forfeitIllegal(Color offender);          // second completed illegal move (draw if the opponent cannot mate)
    std::string pgn(const std::string& whiteName, const std::string& blackName) const;
    std::string pgn(const std::string& whiteName, const std::string& blackName, const PgnTags& tags) const;

private:
    std::vector<Position> positions_;
    std::vector<Move> moves_;
    std::vector<std::string> san_;
    GameStatus status_ = GameStatus::Ongoing;
    GameEndReason reason_ = GameEndReason::None;
    bool endDetection_ = true;
    void finish(GameStatus s, GameEndReason r);
    void updateStatus();
};

// ---- Clock -------------------------------------------------------------------------------------
struct TimeControl {
    bool unlimited = false;
    int64_t baseMs = 5 * 60 * 1000;
    int64_t incrementMs = 0;       // Fischer increment, added after each completed move
    int64_t delayMs = 0;           // Bronstein-style delay (custom only; 0 = off)
    std::string label() const;     // "Unlimited", "5+3", "15+10", "Custom 7:30+2 d3"
    std::string pgnTag() const;    // PGN TimeControl tag: "-" (unlimited), "300+3"
    // FIDE category of the game (used for illegal move penalties): estimated duration for 60
    // moves = base + 60 x increment. <= 10 min Blitz, < 60 min Rapid, >= 60 min Standard
    // (FIDE Laws Appendix A.1 / B.1).
    enum class Category { Blitz, Rapid, Standard, Unlimited };
    Category category() const;
};
const std::vector<TimeControl>& timeControlPresets();  // Unlimited, 1+0, 3+0, 3+2, 5+0, 5+3, 10+0, 10+5, 15+10, 30+0, 30+20, 90+30

// Digital chess clock. With an unlimited time control nothing counts down and nobody flags:
// remainingMs(c) then reports the time c has used so far (counts up), to show elapsed time.
class Clock {
public:
    void setup(const TimeControl& tc);
    // Starts the given side (game start: White). After stop() (pause), start(running()) resumes
    // the current turn without granting a new delay window. A flagged clock does not restart.
    void start(Color running);
    void stop();
    // The player 'mover' pressed the clock at the end of their move: gives back the time used
    // within the delay (Bronstein), adds the increment, switches. Ignored if the clock is stopped,
    // if it is not 'mover''s clock that runs, or if 'mover' has flagged.
    void press(Color mover);
    void update(int64_t elapsedMs);               // advance the running side; stops the clock on a flag
    int64_t remainingMs(Color c) const;
    Color running() const { return running_; }
    bool isRunning() const { return active_; }
    bool flagged(Color c) const;
    void addTime(Color c, int64_t ms);            // arbiter penalties (ignored when unlimited)
    const TimeControl& timeControl() const { return tc_; }
    int64_t delayLeftMs() const { return delayLeft_; }
private:
    TimeControl tc_;
    int64_t remaining_[2] = {0, 0};
    int64_t delayLeft_ = 0;
    int64_t used_ = 0;              // time used by the running side in this turn
    Color running_ = White;
    bool active_ = false;
    bool turnBegun_ = false;
    bool flagged_[2] = {false, false};
};

// ---- Tournament arbitration ("physical" board) --------------------------------------------------
// Models the physical board the players manipulate, which may temporarily hold an illegal move
// when legal-move hints are disabled. Enforces FIDE Article 4 (touch-move) and Article 7.5
// (illegal moves are only "completed" when the clock is pressed).
//
// The arbiter keeps a copy of the game's current position. Every method taking a Game
// re-synchronises it; when the game has advanced (e.g. the opponent's move was played on the
// Game directly) the turn state (touched piece, pending placement) is cleared. The methods
// without a Game (touch(Square), cancelTouch()) use the position of the last synchronisation, so
// prefer touch(game, sq) or call sync(game) after every move. Call reset(game) at each new game.
class Arbiter {
public:
    void reset(const Game& game);
    void sync(const Game& game);
    // Touch-move: the player touched their own piece on 'sq'. Returns false if a different piece
    // is already committed this turn. After touching, the player must move that piece if it has
    // at least one legal move (touchedHasLegalMove()). Touching another piece when the committed
    // one has no legal move and was not displaced releases it (Art. 4.5), like cancelTouch().
    bool touch(Square sq);
    bool touch(const Game& game, Square sq);
    Square touchedSquare() const { return touched_; }
    bool touchedHasLegalMove(const Game& game) const;
    // The player released the touched piece on 'to' (any square not holding an own piece;
    // to == from means the piece was put back: nothing happens, returns false).
    // promotion: chosen piece when a pawn reaches the last rank (NoPiece = not chosen yet, see
    // pendingNeedsPromotion/choosePromotion). Records the physical move; nothing is final until
    // clockPressed(). Once released on a square, the square is final for this move (Art. 4.6):
    // a second place() fails, except to supply the missing promotion piece on the same square.
    struct Placement {
        Square from = NoSquare, to = NoSquare;
        PieceType promotion = NoPiece;
        bool castlingRookMove = false;   // king moved two squares toward a rook with castling rights
        Square captured = NoSquare;      // square of the piece physically removed (ep: the passed pawn)
    };
    bool place(const Game& game, Square to, PieceType promotion);
    bool hasPendingMove() const { return pending_; }
    Placement pendingMove() const { return placement_; }
    bool pendingNeedsPromotion(const Game& game) const;   // pawn released on the last rank without a choice yet
    bool choosePromotion(const Game& game, PieceType promotion);  // Knight..Queen, for the pending pawn
    // Takes back a pending placement that is not a legal move before the clock is pressed (the
    // illegal move is not completed yet); the same piece stays touched. False for a legal one.
    bool retractPlacement(const Game& game);
    // Clock press: completes the move. Returns the verdict; for a legal move 'move' holds the
    // move to play on the Game; for an illegal move the physical board must be restored and the
    // arbiter reports the penalty (time bonus for the opponent in ms, or forfeit). The caller
    // presses the Clock only for a legal move (or when moveStands).
    struct Verdict {
        bool legal = false;
        Move move;
        int64_t opponentBonusMs = 0;   // 2 min standard, 1 min rapid/blitz (FIDE 7.5.5 / A.4.2)
        bool forfeit = false;          // second completed illegal move -> Game::forfeitIllegal(mover)
        // Art. 7.5.2: a pawn moved to the last rank without being replaced counts as an illegal
        // move (penalised) but the move stands with a queen: play 'move' and press the clock.
        bool moveStands = false;
        // In the UI language (i18n, section "Arbiter"), shown to the player ("" for a legal move).
        std::string message;
    };
    Verdict clockPressed(const Game& game, const TimeControl& tc);
    void cancelTouch();                 // for touched pieces with no legal move only
    int illegalCount(Color c) const { return illegal_[c]; }
private:
    Square touched_ = NoSquare;
    bool pending_ = false;
    Placement placement_{};
    int illegal_[2] = {0, 0};
    Position pos_;                      // synchronised copy of the game position
    size_t ply_ = 0;                    // number of game moves pos_ corresponds to
    size_t seenPly_ = 0;                // highest move count observed on the Game (new-game detection)
    bool over_ = false;
    Square rawEp_ = NoSquare;           // square skipped by the last double pawn push (even if ep is illegal)
    bool inSync(const Game& game) const;
    void advance(const Move& m);
    bool pendingIsLegal() const;
    bool needsPromotion() const;
    Verdict illegal(Color mover, const TimeControl& tc, int kind);
};

}  // namespace chess
