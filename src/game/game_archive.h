// Saved games: the PGN files of the player's games in a folder of their own (the game passes
// plat::appDataDirectory() + "pgn/"), and every PGN file the player drops there (exports of
// lichess, chess.com, other programs; several games per file). Engine-free (no GL, no Stockfish):
// the library page (ui_library.cpp), the scene (saving at the end of a game, the replay) and the
// unit tests (tests/game_archive_tests.cpp) use it.
//
// Saving: one game per file, "YYYY-MM-DD_HHMMSS_<mode>_<White>-vs-<Black>.pgn" (local time; the
// names made safe for Windows and Linux file names, Unicode kept), written to a temporary file in
// the folder and moved to its name in one step that never replaces a file (a suffix _2, _3...
// on a collision): a crash or a full disk never leaves half a game under a game's name.
//
// Which games are saved when they end (shouldSave): games against Stockfish, coach games of levels
// 1 to 6 (not the rules lesson; the coach's demonstration lines never reach the game record),
// hot-seat games and direct matches (no server); not the viewer mode, and not the games of an
// online server (the server keeps them). Finished games with their result; a game left before its
// end with "*" and Termination "unterminated". Options [archive] save_games turns it off.
//
// Games of an online server are saved on the player's request instead (the account pages' game
// history, "Save to saved games" and "Replay"): the server's own PGN (GET /games/:id/pgn, read as
// untrusted input) with the tags ScacelithMode "server", ScacelithServer (the server's origin) and
// ScacelithGameId, each game of a server once (saveServerGame). [archive] save_games does not
// apply: the player asked for it.
//
// Listing: every *.pgn file of the folder, one entry per game, newest first (Date and Time tags,
// UTCDate and UTCTime for exports, then the file's time). Only the tags and the number of moves are
// read (pgn::scan), and each file is read again only when its size or time changed: a folder of
// thousands of games lists quickly from the second time on. Files that cannot be read are listed
// with their error. At most kMaxListed games are listed, from the most recently written files.
// Thread-safe: the cache has a lock (the library lists on a worker thread), held only to look
// files up and store them, never while a file is read; the cache is never destroyed, so a listing
// still running when the program exits never touches a destroyed object.
#pragma once
#include "../chess/pgn.h"
#include <atomic>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace game {
namespace archive {

// Where a game was played: the ScacelithMode tag and the <mode> part of the file names.
enum class Mode {
    Play,      // against Stockfish ("play")
    Coach,     // against the coach ("coach")
    HotSeat,   // two players on one PC ("hotseat")
    Direct,    // a direct match, no server ("direct")
    Server,    // a game of an online server: saved on request only ("server")
    Watch,     // the viewer mode: never saved ("watch")
    Imported   // no ScacelithMode tag: a file from elsewhere
};
const char* modeName(Mode m);                 // "play" ... "watch", "" for Imported
Mode modeFromName(const std::string& name);   // Imported for anything else

// Whether a game is saved when it ends or is left. coachLevel: Coach games only (0 = the rules
// lesson); plies: moves played; finished: it has a result; enabled: Options [archive] save_games.
// Nothing is saved for a game left before its first move, and never a game of an online server
// (Mode::Server: saveServerGame, on the player's request).
bool shouldSave(Mode mode, int coachLevel, int plies, bool finished, bool enabled);

// What the scene knows of a game besides its moves (makeRecord). Termination, when not given:
// "unterminated" for "*", else from endKey (the authority's ending of a direct match: "time
// forfeit", "rules infraction", "abandoned"), else from the Game, else "normal".
struct GameInfo {
    Mode mode = Mode::Play;
    std::string white, black;          // the names on the scoresheets
    int whiteElo = 0, blackElo = 0;    // 0 = not written
    std::string event;                 // "" = per mode: "Casual game", "Coach game", "Direct match"
    std::string site = "Scacelith";
    int round = 0;                     // 0 = "-"
    std::time_t started = 0;           // start of the game (Date and Time tags, local time); 0 = now
    std::string timeControl = "-";     // chess::TimeControl::pgnTag(): "-" untimed, "300+3"
    int coachLevel = -1;               // Coach: 1..6 (CoachLevel tag)
    // The ending: "" = from the Game (status, reason). result overrides the Game's result (a
    // direct match ended by the authority); endKey is the i18n key of the reason
    // ("reason.checkmate", "reason.online.abandonment"), written without "reason." as ScacelithEnd;
    // termination overrides the Termination value.
    std::string result, endKey, termination;
    std::string opening, eco;          // Opening / ECO tags when known
    // Per ply (index = ply), -1 = unknown: the time the mover spent on the move ([%emt]) and the
    // mover's clock after it, increment included ([%clk], timed games only). Rounded to 0.1 s.
    // A vector longer than the game (moves taken back without trimming it) cannot be matched to
    // the plies: its times are left out rather than put on the wrong moves.
    std::vector<int64_t> elapsedMs, clockMs;
};
chess::pgn::Record makeRecord(const chess::Game& game, const GameInfo& info);

// ---- Files ----------------------------------------------------------------------------------------
// A name made safe as a part of a file name: control and format characters dropped (bidi
// overrides included), < > : " / \ | ? * and white space as '_', no leading or trailing dots,
// underscores or dashes, at most maxBytes bytes of UTF-8 (whole characters), Windows device names
// (CON, NUL, COM1...) given a trailing '_', "Unknown" when nothing is left.
std::string sanitizeName(const std::string& name, size_t maxBytes = 40);
// "2026-10-01_210409_coach_Olivier-vs-Coach.pgn": 'when' in local time, the mode from the
// ScacelithMode tag ("game" without one), White and Black from the tags.
std::string fileName(const chess::pgn::Record& record, std::time_t when);
// "folder/name" with the folder's separator (or the platform's when it has none).
std::string joinPath(const std::string& folder, const std::string& name);
// Creates the folder and its missing parents (the library's "Open folder" before the first save).
bool makeFolder(const std::string& folder);

struct SaveResult {
    bool ok = false;
    std::string path;                  // the file written
    std::string error;                 // English, for the log
};
// Writes the record as a new file in 'folder' (created when missing), named by fileName(record,
// when) (when 0 = now), never replacing a file.
SaveResult save(const std::string& folder, const chess::pgn::Record& record, std::time_t when = 0);
// Writes 'text' as a new file 'name' in 'folder' (created when missing), the way save() writes a
// game: a temporary file moved to its name in one step that never replaces a file, "name_2.ext",
// "name_3.ext"... on a collision. The account page's data export is written this way.
SaveResult saveFile(const std::string& folder, const std::string& name, const std::string& text);

// ---- Listing ---------------------------------------------------------------------------------------
struct Entry {
    std::string path;                  // the file
    std::string file;                  // its name
    int index = 0;                     // the game's place in the file (0 = the first)
    int games = 1;                     // games in the file
    uint64_t fileSize = 0;
    int64_t fileTimeMs = 0;            // last write, ms since 1970-01-01 UTC
    size_t offset = 0, length = 0;     // the game's bytes in the file
    int line = 1, column = 1;          // where it starts
    // The tags the listing uses (the Seven Tag Roster, dates and times, Elo, time control,
    // opening, termination and Scacelith's own), first of each name; load() gives them all.
    std::vector<chess::pgn::Tag> tags;
    int plies = 0;                     // moves of the main line (counted, not checked)
    std::string result = "*";
    Mode mode = Mode::Imported;
    // This game (or the whole file) could not be read: English, with the line and column when the
    // fault is inside a game. A listed game can still fail to load (an illegal move: load()).
    std::string error;
    bool fileError = false;            // the error concerns the whole file (too large, unreadable)

    std::string tag(const std::string& name, const std::string& fallback = std::string()) const;
    // Date and time to show and sort by: "2026.10.01" (Date, else UTCDate; '?' kept), "21:04:09"
    // (Time, else UTCTime) or "".
    std::string date() const;
    std::string time() const;
    int coachLevel() const;            // CoachLevel tag, -1 = none
    bool removable() const { return games == 1 && !fileError; }
};

struct ListStats {
    int files = 0;                     // *.pgn files in the folder
    int read = 0;                      // files read this time (new or changed)
    int cached = 0;                    // files taken from the cache
    bool truncated = false;            // more than kMaxListed games: the oldest files are left out
    bool cancelled = false;            // stopped by 'cancel': the list is incomplete
    std::string error;                 // the folder could not be read ("" when it does not exist)
};
// Games listed at most (memory and the copy of each listing stay bounded whatever the folder
// holds): the files written last come first, so the player's own new games are always in.
constexpr int kMaxListed = 20000;
// Every game of every *.pgn file in 'folder', newest first. A missing folder is an empty list.
// 'cancel' (optional) is checked between files: set, the listing stops early (the program exits).
std::vector<Entry> list(const std::string& folder, ListStats* stats = nullptr, const std::atomic<bool>* cancel = nullptr);
void clearCache();                     // forget every listed file (tests)

struct LoadResult {
    bool ok = false;
    chess::pgn::Record record;         // moves, start position, per-ply clocks and times, tags
    std::string error;                 // English, with the line and column of a fault in the game
};
// The whole game of a listed entry (a replay, the details pane). When the file changed since it
// was listed, the game at the same index in the file as it is now.
LoadResult load(const Entry& entry);
// The game 'index' of a file (the replay's command line: --replay <file> --game N).
LoadResult loadFile(const std::string& path, int index = 0);

enum class RemoveStatus {
    Removed,
    SeveralGames,                      // the file holds other games too: never deleted from here
    Changed,                           // the file changed since it was listed: list it again
    NotFound,                          // already gone
    Failed                             // the file system refused (read-only, in use)
};
struct RemoveResult {
    RemoveStatus status = RemoveStatus::Failed;
    std::string error;                 // English, for the log ("" when removed)
    bool ok() const { return status == RemoveStatus::Removed; }
};
// Deletes the entry's file when it holds this one game only.
RemoveResult remove(const Entry& entry);

// ---- Games of an online server (saved on request) ---------------------------------------------------
struct ServerGame {
    std::string server;                // the server's origin ("caissa.scacelith.com:443"): ScacelithServer
    uint64_t gameId = 0;               // the server's id of the game: ScacelithGameId
    std::string endKey;                // i18n key of the ending ("reason.checkmate"), "" = unknown:
                                       // ScacelithEnd (without "reason.") of a finished game
};
// The largest PGN text taken from a server for one game.
constexpr size_t kMaxServerPgnBytes = size_t(4) << 20;
// The server's PGN of one game as a record of the saved games. The text is untrusted: at most
// kMaxServerPgnBytes, exactly one game, read without error. Tags set: ScacelithMode "server",
// ScacelithServer, ScacelithGameId (a different id in the text is refused), ScacelithEnd (finished
// games, when endKey is known); Date and Time become the local date and time of the start, from
// UTCDate and UTCTime (the folder's own games are in local time; UTCDate and UTCTime are kept).
// False with an English error when the text cannot be used.
bool serverRecord(const std::string& pgnText, const ServerGame& game, chess::pgn::Record& out, std::string& error);
// The entry of a listing that is this game of this server (ScacelithMode "server", the same
// ScacelithServer and ScacelithGameId), nullptr when there is none.
const Entry* findServerGame(const std::vector<Entry>& entries, const std::string& server, uint64_t gameId);
enum class ServerSaveStatus {
    Saved,                             // written now
    AlreadySaved,                      // a file of the folder holds it: nothing written
    NeedsText,                         // not saved yet, and no PGN text was given (a lookup only)
    Invalid,                           // the text is not a usable game (serverRecord)
    Failed                             // the file could not be written
};
struct ServerSaveResult {
    ServerSaveStatus status = ServerSaveStatus::Failed;
    std::string path;                  // the file of the game (Saved, AlreadySaved)
    int index = 0;                     // the game's index in that file
    std::string error;                 // English, for the log
};
// Saves a game of a server in 'folder' once: when a file of the folder holds it already, says so
// (AlreadySaved, with its file) and writes nothing; otherwise serverRecord(pgnText) is saved with
// save(), named after the start of the game. An empty pgnText only looks (NeedsText when the game
// is not there yet). Lists the folder (list(): cached, but the first listing of a large folder
// takes a while): call it off the UI thread.
ServerSaveResult saveServerGame(const std::string& folder, const ServerGame& game, const std::string& pgnText);

}  // namespace archive
}  // namespace game
