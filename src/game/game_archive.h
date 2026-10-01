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
// Which games are saved (shouldSave): games against Stockfish, coach games of levels 1 to 6 (not
// the rules lesson; the coach's demonstration lines never reach the game record), hot-seat games
// and direct matches (no server); not the games of an online server (the server keeps them), not
// the viewer mode. Finished games with their result; a game left before its end with "*" and
// Termination "unterminated". Options [archive] save_games turns it off.
//
// Listing: every *.pgn file of the folder, one entry per game, newest first (Date and Time tags,
// UTCDate and UTCTime for exports, then the file's time). Only the tags and the number of moves are
// read (pgn::scan), and each file is read again only when its size or time changed: a folder of
// thousands of games lists quickly from the second time on. Files that cannot be read are listed
// with their error. Thread-safe: the cache has a lock (the library lists on a worker thread).
#pragma once
#include "../chess/pgn.h"
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
    Server,    // a game of an online server: never saved ("server")
    Watch,     // the viewer mode: never saved ("watch")
    Imported   // no ScacelithMode tag: a file from elsewhere
};
const char* modeName(Mode m);                 // "play" ... "watch", "" for Imported
Mode modeFromName(const std::string& name);   // Imported for anything else

// Whether a game is saved when it ends or is left. coachLevel: Coach games only (0 = the rules
// lesson); plies: moves played; finished: it has a result; enabled: Options [archive] save_games.
// Nothing is saved for a game left before its first move.
bool shouldSave(Mode mode, int coachLevel, int plies, bool finished, bool enabled);

// What the scene knows of a game besides its moves (makeRecord).
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

struct SaveResult {
    bool ok = false;
    std::string path;                  // the file written
    std::string error;                 // English, for the log
};
// Writes the record as a new file in 'folder' (created when missing), named by fileName(record,
// when) (when 0 = now), never replacing a file.
SaveResult save(const std::string& folder, const chess::pgn::Record& record, std::time_t when = 0);

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
    std::string error;                 // the folder could not be read ("" when it does not exist)
};
// Every game of every *.pgn file in 'folder', newest first. A missing folder is an empty list.
std::vector<Entry> list(const std::string& folder, ListStats* stats = nullptr);
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

}  // namespace archive
}  // namespace game
