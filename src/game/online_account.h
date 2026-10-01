// The account pages' data on the game's side (the account API of the dedicated server,
// dedicated-server/docs/API.md): the pages of the player's game history, the game opened from it,
// the signed-in devices, and what the answers change in the account. game::OnlineSession keeps one
// AccountData and routes the answers of net::OnlineClient to it (AccountData::apply); the pages
// (ui/ui_screens_account.cpp) read it. The animated GIFs of games (a game of the history, a game
// of the saved games: ui/ui_library.cpp) go through one GifSaver, which writes each file to the
// GIF folder. Engine-free (no GL, no UI): unit-tested in tests/online_account_tests.cpp.
#pragma once
#include "../net/online_client.h"
#include "game_archive.h"
#include <cstdint>
#include <ctime>
#include <future>
#include <string>
#include <vector>

namespace game {

// The game history, page by page, newest first: the server's cursor (GamesPage::next) leads to the
// next page; the cursors of the pages already seen are kept, so that Previous needs none of its own.
// One request at a time: next() and previous() refuse while one is in flight (the page greys its
// buttons meanwhile), and an answer is kept only when it is the one awaited (same cursor).
class HistoryPager {
public:
    static constexpr int kPageSize = 10;

    // Starts again from the first page of 'filter'; returns the cursor to request (0).
    uint64_t restart(const net::GamesFilter& filter);
    // The cursor of the next / previous page to request; false when there is none or a request is
    // in flight.
    bool next(uint64_t& before);
    bool previous(uint64_t& before);
    // The current page again (after an error): its cursor.
    uint64_t reload();
    // The answer of the request in flight: true when it was kept.
    bool accept(const net::GamesPage& page);
    void fail(const std::string& error);   // the request failed: the page shown stays
    void clear();                          // nothing loaded (signed out, another server)

    const net::GamesFilter& filter() const { return filter_; }
    const net::GamesPage& page() const { return page_; }
    bool loaded() const { return loaded_; }          // a page of this filter arrived
    bool waiting() const { return waiting_; }        // a request is in flight
    const std::string& error() const { return error_; }
    int pageIndex() const { return index_; }         // 0 = the newest games
    int pageCount() const;                           // from the total, at least 1
    bool hasNext() const { return loaded_ && page_.next != 0; }
    bool hasPrevious() const { return loaded_ && index_ > 0; }

private:
    net::GamesFilter filter_;
    net::GamesPage page_;
    std::vector<uint64_t> cursors_;   // cursors_[i]: the 'before' of page i
    int index_ = 0;
    bool loaded_ = false, waiting_ = false;
    uint64_t wantBefore_ = 0;
    int wantIndex_ = 0;
    std::string error_;
};

struct AccountData {
    HistoryPager history;
    // The game opened from the history (GET /games/:id).
    uint64_t gameWanted = 0;          // the id asked for
    bool gameLoaded = false;          // 'game' is the one asked for
    net::GameDetails game;
    std::string gameError;
    // The signed-in devices (GET /auth/sessions).
    bool sessionsLoaded = false;
    std::vector<net::SessionInfo> sessions;
    std::string sessionsError;

    void clear();
    // Applies an answer of the account API: keeps what the pages show (the history page awaited,
    // the game asked for, the devices, a device signed out) and updates the account (accept
    // challenges; an account deleted signs out and forgets it; "unauthorized", the token refused,
    // signs out, and so do a GIF's "invalid_token" and "not_logged_in"). False for the other kinds
    // of events (nothing changed).
    bool apply(const net::Event& e, net::AccountInfo& account, bool& signedIn);
};

// The result of a finished game from the player's side.
enum class Outcome { Win, Loss, Draw, Aborted, None };   // None: not one of the players, or unknown
Outcome outcomeOf(const net::GameSummary& g);

// The moves of a server game for the details page: SAN from the standard start, the mover's clock
// after the move and the time charged for it (-1 = unknown). A move that does not play (never
// expected from a server) ends the list there; complete then says false.
struct MoveLine {
    std::string san;
    int64_t clockMs = -1, spentMs = -1;
};
std::vector<MoveLine> gameMoves(const net::GameDetails& g, bool* complete = nullptr);

// "3+2", "7+3", "1:30+1": a time control as the history shows it (base in minutes, or m:ss).
std::string timeControlLabel(int64_t baseMs, int64_t incMs);

// The file of an account export: "<host>_<username>_<YYYY-MM-DD>.json", the date in local time, the
// host and the user name made safe for a file name (game::archive::sanitizeName).
std::string exportFileName(const std::string& host, const std::string& username, std::time_t when);

// ---- Animated GIFs of games (GET /games/:id/gif, POST /gif) -------------------------------------------
// The file of a game's GIF, named like the saved games' PGN files:
// "<YYYY-MM-DD_HHMMSS>_<White>-vs-<Black>_<gameId>.gif", the start of the game in local time
// ("0000-00-00_000000" when unknown: started 0), the names made safe for a file name
// (archive::sanitizeName), "_<gameId>" only for a game with a server id. GifSaver writes it with
// archive::saveFile, never over a file ("..._2.gif" next to one of the same name).
std::string gifFileName(std::time_t started, const std::string& white, const std::string& black, uint64_t gameId);
// The local time of a PGN's Date ("2026.09.27") and Time ("21:47:05", "21:47") tags, the time
// taken as midnight when it is missing; 'fallback' when the date is not a whole one ("2026.??.??").
std::time_t pgnLocalTime(const std::string& date, const std::string& time, std::time_t fallback);
// The start of a saved game: its local Date and Time tags when it has a Time, else its UTCDate
// and UTCTime (a server's PGN as the server writes it: "2026.09.27", "21:47:12"), else the local
// Date at midnight, else 'fallback'.
std::time_t pgnGameStart(const std::string& date, const std::string& time, const std::string& utcDate, const std::string& utcTime,
                         std::time_t fallback);
// A wait in words in the interface language (i18n "gif.wait.*"): "45 seconds", "2 minutes and
// 30 seconds", "13 minutes" (whole minutes, rounded up, from 5 minutes on); at least 1 second.
std::string waitText(int seconds);

// One GIF at a time, asked by a page (a game of the account's history, a game of the saved games)
// and written to the GIF folder when the server's answer arrives, whatever page shows by then.
// The page sends the request (ServerApi::downloadGameGif or renderPgnGif) and calls begin() with
// what it is about and where the file goes; game::OnlineSession routes the answer (GifResult) to
// finish(), which writes the file off the calling thread (archive::saveFile: a new file, never one
// replaced), and calls poll() every frame until the file is written. The pages show the state of
// their own game (owner()): rendering, written (path()), or the error (the server's code with its
// retryAfterSec, "write_failed" when the file could not be written).
class GifSaver {
public:
    enum class Stage { Idle, Rendering, Writing, Saved, Failed };

    GifSaver() = default;
    ~GifSaver();                          // waits for a write in progress
    GifSaver(const GifSaver&) = delete;
    GifSaver& operator=(const GifSaver&) = delete;

    // A request was sent: owner = what the page shows it for ("history:812", a saved game's key),
    // gameId = the GifResult's gameId awaited (0 for a PGN text). False (nothing changes) while
    // another GIF is being made: one at a time.
    bool begin(const std::string& owner, uint64_t gameId, const std::string& folder, const std::string& fileName);
    // The server's answer: false when it is not the one awaited (nothing asked, another game).
    bool finish(const net::Event& e);
    // The write in progress ended (wait: until it has): true on the call that moved to Saved or Failed.
    bool poll(bool wait = false);
    void clear();                         // back to Idle (a write in progress is waited for)

    Stage stage() const { return stage_; }
    bool busy() const { return stage_ == Stage::Rendering || stage_ == Stage::Writing; }
    const std::string& owner() const { return owner_; }
    const std::string& path() const { return path_; }     // Saved: the file written
    const std::string& error() const { return error_; }   // Failed
    int retryAfterSec() const { return retryAfterSec_; }  // Failed: rate_limited, server_busy

private:
    Stage stage_ = Stage::Idle;
    std::string owner_, folder_, fileName_, path_, error_;
    uint64_t gameId_ = 0;
    int retryAfterSec_ = 0;
    std::future<archive::SaveResult> job_;
};

}  // namespace game
