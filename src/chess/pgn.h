// PGN (Portable Game Notation): the reader for the files a player drops in the saved games folder
// (lichess, chess.com, ChessBase and other programs' exports) and the writer of the games the game
// saves (src/game/game_archive.h). Engine-free, no global state; independent objects may be used
// from different threads.
//
// Reader. Several games per file; tag pairs with \" and \\ escapes; movetext with move numbers,
// SAN read leniently (Position::parseSAN: "0-0", "e8Q", "Ng1f3", lowercase pieces, figurines,
// trailing +, #, !, ?), comments {...} and ; to the end of the line, NAGs $n (and !, ?, !!, ??,
// !?, ?! as $1..$6), variations ( ... ) nested and skipped, % escape lines, the four results.
// [SetUp "1"] + [FEN] start positions; a [Variant] other than standard chess is refused, and so
// is Chess960 unless its castling rights are those of a standard setup (Position knows only
// those). Per ply the comment text is kept and [%clk h:mm:ss(.f)] / [%emt h:mm:ss(.f)] are taken
// out of it into clockMs / elapsedMs.
// Files are untrusted: hard caps (Limits) on the input size, games per file, plies, tags, tag
// lengths, variation depth and comment length. The reader never throws or crashes on any input; a
// broken game gets an error with its line and column, and the games after it still load.
//
// Writer. Tags in a stable order (tagRank), movetext wrapped at 80 columns with move numbers
// (and "N..." for Black after a comment), NAGs, per-ply comments {[%clk ...] [%emt ...] text},
// the result. write() then read() gives back the same moves, tags, clocks, times and comments.
// chess::Game::pgn() stays the quick export of the scoresheets; this writer is the archive's.
#pragma once
#include "chess.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chess {
namespace pgn {

// Hard caps of the reader (the defaults are generous for real files, small for hostile ones).
struct Limits {
    size_t maxBytes = size_t(32) << 20;   // the whole input; larger inputs are refused
    int maxGames = 20000;                 // games read from one input; the rest is left out (truncated)
    int maxPlies = 1500;                  // main-line moves of one game
    int maxTags = 128;                    // tag pairs of one game
    size_t maxTagName = 64;               // bytes
    size_t maxTagValue = 2048;            // bytes
    int maxDepth = 64;                    // nesting of variations
    size_t maxComment = 8192;             // bytes of comment text kept per ply (the rest is cut)
    // scan() only: when set, a Summary keeps the tags it accepts, each name once (the reader's own
    // Variant, SetUp, FEN and Result too), so that a file repeating tags by the hundred stays small
    // in memory. Every tag still counts in maxTags.
    bool (*summaryTag)(const std::string& name) = nullptr;
};

struct Tag {
    std::string name, value;
    bool operator==(const Tag& o) const { return name == o.name && value == o.value; }
};

// One move of the main line.
struct Ply {
    Move move;                  // with its flags (Position::findLegal)
    std::string san;            // as Position::toSAN writes it ("Nf3", "exd8=Q+", "O-O")
    std::string comment;        // text of the comments after it, without [%clk] / [%emt]
    std::vector<int> nags;      // $1..$255
    int64_t clockMs = -1;       // [%clk]: the mover's clock after the move (increment included), -1 = none
    int64_t elapsedMs = -1;     // [%emt]: time the mover spent on it, -1 = none
};

struct Record {
    std::vector<Tag> tags;      // reader: file order; writer: sorted by tagRank
    std::string fen;            // start position, "" = the standard one
    std::string comment;        // comment before the first move
    std::vector<Ply> plies;
    std::string result = "*";   // "1-0", "0-1", "1/2-1/2", "*"

    const std::string* findTag(const std::string& name) const;   // exact name, nullptr when absent
    std::string tag(const std::string& name, const std::string& fallback = std::string()) const;
    void setTag(const std::string& name, const std::string& value);  // replaces, or appends
    void eraseTag(const std::string& name);
    Position startPosition() const;           // the standard one when fen is empty (or invalid)
    // Positions[i] is the position before ply i; positions().size() == plies.size() + 1.
    std::vector<Position> positions() const;
    bool hasClocks() const;                   // a ply carries [%clk]
    bool hasElapsed() const;                  // a ply carries [%emt]
    // The moves of a Game (start position, SAN, result); no tags, no times.
    static Record fromGame(const chess::Game& game);
    // The record replayed into 'game' (resetFromFEN + play). False when a move does not play there
    // (the Game ended earlier by itself, e.g. a fivefold repetition the file played on from).
    bool toGame(chess::Game& game) const;
};

// Where and why a game could not be read. line/column are 1-based (columns count characters).
struct Error {
    int line = 0, column = 0;
    std::string message;        // English ("illegal move 'Nf6'"); empty = no error
    bool empty() const { return message.empty(); }
    std::string text() const;   // "line 12, column 5: illegal move 'Nf6'"
};

// A game as read, with its place in the input.
struct ParsedGame {
    Record record;              // what could be read before an error (the tags at least)
    Error error;
    bool ok() const { return error.empty(); }
    size_t offset = 0, length = 0;   // its bytes in the input: [offset, offset + length)
    int line = 1, column = 1;        // where its first token is
};

// Header pass for listings: tags, main-line move count and result, without checking the moves.
struct Summary {
    std::vector<Tag> tags;
    int plies = 0;              // SAN tokens of the main line
    std::string result = "*";   // movetext result, else the Result tag, else "*"
    Error error;                // syntax errors only (an illegal move shows when the game is read)
    size_t offset = 0, length = 0;
    int line = 1, column = 1;
    std::string tag(const std::string& name, const std::string& fallback = std::string()) const;
};

template <class T> struct Result {
    std::vector<T> games;
    std::string error;          // the whole input was refused (too large), "" otherwise
    bool truncated = false;     // more than Limits::maxGames games: the rest was left out
};

// Where the text starts in its file, for the line/column of errors when reading a slice.
struct Origin {
    int line = 1, column = 1;
};

Result<ParsedGame> read(const std::string& text, const Limits& limits = Limits(), Origin origin = Origin());
Result<Summary> scan(const std::string& text, const Limits& limits = Limits(), Origin origin = Origin());

// The record as PGN text: tags, a blank line, the movetext, a final newline. Tags (rank order,
// then the record's order): the Seven Tag Roster (missing ones written "?", Date "????.??.??",
// Result from record.result), TimeControl, Termination, ECO, Opening, Variation, WhiteElo,
// BlackElo, PlyCount (computed), SetUp and FEN (from record.fen, when it is not the standard
// start), then every other tag (Scacelith's own: ScacelithMode, ScacelithEnd, CoachLevel...).
std::string write(const Record& record);
// Several records, one blank line between games.
std::string writeAll(const std::vector<Record>& records);
// Rank of a tag in write()'s order (0..16 for the named tags above, 100 for any other).
int tagRank(const std::string& name);

// Times of [%clk] and [%emt]: "h:mm:ss" with a fraction only when needed ("0:04:58", "0:00:02.5",
// "1:30:00.25"). parseTime also takes "m:ss" and "ss", with up to 3 fraction digits read.
std::string formatTime(int64_t ms);
bool parseTime(const std::string& s, int64_t& ms);
// PGN TimeControl "300+3" / "300" (seconds) into base and increment; false for "-", "?", "40/7200:3600"...
bool parseTimeControl(const std::string& tc, int64_t& baseMs, int64_t& incrementMs);
// The movetext result as written ("1-0", "0-1", "1/2-1/2", "*"); "" when s is not a result
// ("½-½" and "1/2" count as a draw, and so does "0.5-0.5" as a Result tag value: in movetext the
// reader takes its '.' for the period of a move number).
std::string normalizeResult(const std::string& s);
// The PGN Termination value of a finished Game ("normal", "time forfeit", "rules infraction"),
// "unterminated" while it goes on.
const char* terminationValue(GameStatus status, GameEndReason reason);

}  // namespace pgn
}  // namespace chess
