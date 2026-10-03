// The account pages' data on the game's side (the account API of the dedicated server,
// dedicated-server/docs/API.md): the pages of the player's game history, the game opened from it,
// the signed-in devices, and what the answers change in the account. game::OnlineSession keeps the
// answers the menus wait for in a ServerAnswers (those of a server left meanwhile dropped) and one
// AccountData, and routes the answers of net::OnlineClient to it (AccountData::apply); the pages
// (ui/ui_screens_account.cpp) read it. The animated GIFs of games (a game of the history, a game
// of the saved games: ui/ui_library.cpp) go through one GifSaver, which writes each file to the
// GIF folder. The HTTPS errors in words (onlineErrorText) are here too. Engine-free (no GL, no UI):
// unit-tested in tests/online_account_tests.cpp.
#pragma once
#include "../net/online_client.h"
#include "game_archive.h"
#include <cstdint>
#include <ctime>
#include <future>
#include <map>
#include <string>
#include <vector>

namespace game {

// The answers of the HTTPS requests the menus wait for (OnlineSession::expect, busy and take), for
// the server in use. Every answer names the server its request went to (net::Event::origin): the
// answers of a server left meanwhile (Options > Online, applyServer) are not this server's history,
// game, devices, account or refused session, and are dropped, except a PGN: the game page saves it
// as the game it asked for, under the server that game came from (GameSaveState::saveGame), so a
// Save game pressed before the change still ends (never left waiting for an answer thrown away).
class ServerAnswers {
public:
    // The server in use (its origin). Any call forgets what was awaited and kept, except the PGN.
    void setServer(const std::string& origin);
    const std::string& origin() const { return origin_; }
    // An answer of another server than the one in use ("" origin: a realtime event, never foreign).
    bool foreign(const net::Event& e) const { return !e.origin.empty() && e.origin != origin_; }
    void expect(net::Event::Kind k);              // a request was sent: busy() until its answer
    bool busy(net::Event::Kind k) const;
    bool take(net::Event::Kind k, net::Event& out);   // the answer arrived, handed over once
    // An answer arrived: kept for take() (the latest of its kind), one fewer awaited. False, and
    // nothing changes, for a foreign one other than a PGN. The rvalue form moves the answer (an
    // export or a PGN may be megabytes), and leaves it untouched when it returns false.
    bool keep(const net::Event& e);
    bool keep(net::Event&& e);

private:
    std::string origin_;
    std::map<int, net::Event> results_;
    std::map<int, int> pending_;
};

// The game history, page by page, newest first: the server's cursor (GamesPage::next) leads to the
// next page; the cursors of the pages already seen are kept, so that Previous needs none of its own.
// One request at a time: next() and previous() refuse while one is in flight (the page greys its
// buttons meanwhile). restart() and reload() may be asked while one is in flight (the filters
// change, the page opens again): every answer names its request (GamesPage::before and filter),
// and an answer is kept only when it is the one awaited (same cursor and filter); a failure stops
// the wait only when it is the one awaited and no request like it is still in flight.
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
    // An answer (page: GamesPage::before and filter name its request): true when it was kept.
    bool accept(const net::GamesPage& page);
    // A request failed (request: its GamesPage::before and filter): when it is the one awaited, the
    // page shown stays with the error (and the server's wait, rate_limited: Event::retryAfterSec).
    void fail(const net::GamesPage& request, const std::string& error, int retryAfterSec = 0);

    const net::GamesFilter& filter() const { return filter_; }
    const net::GamesPage& page() const { return page_; }
    bool loaded() const { return loaded_; }          // a page of this filter arrived
    bool waiting() const { return waiting_; }        // a request is in flight
    const std::string& error() const { return error_; }
    int retryAfterSec() const { return retryAfter_; } // with error(): the server's wait, 0 = none
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
    int retryAfter_ = 0;
    struct Request {
        uint64_t before;
        net::GamesFilter filter;
    };
    std::vector<Request> inFlight_;   // the requests sent and not answered yet, oldest first
    void sent();                      // a request of filter_ and wantBefore_
    bool answered(const net::GamesPage& request);  // forgets it; true when it is the one awaited
};

struct AccountData {
    HistoryPager history;
    // The game opened from the history (GET /games/:id).
    uint64_t gameWanted = 0;          // the id asked for
    bool gameLoaded = false;          // 'game' is the one asked for
    net::GameDetails game;
    std::string gameError;
    int gameRetryAfter = 0;           // with gameError: the server's wait (rate_limited), 0 = none
    // The signed-in devices (GET /auth/sessions).
    bool sessionsLoaded = false;
    std::vector<net::SessionInfo> sessions;
    std::string sessionsError;
    int sessionsRetryAfter = 0;       // with sessionsError, as gameRetryAfter

    void clear();
    // Applies an answer of the account API: keeps what the pages show (the history page awaited,
    // the game asked for, the devices, a device signed out) and updates the account (accept
    // challenges; an account deleted signs out and forgets it; "unauthorized" or sessionLost, the
    // token refused or none saved, signs out, whatever the call: a GIF's too). False for the other
    // kinds of events (nothing changed).
    bool apply(const net::Event& e, net::AccountInfo& account, bool& signedIn);
};

// "Save to saved games" and "Replay" on the page of a game of the history (ui_screens_account.cpp):
// the game the state is about, where its save stands, the game a PGN being downloaded is saved as,
// and whether a replay waits for the save. The page runs the lookups in the saved games and the
// writes off the UI thread (game::archive::saveServerGame); 'job' below: one of them runs.
struct GameSaveState {
    enum class Save { Unknown, Checking, NotSaved, Downloading, Writing, Saved, Failed };
    uint64_t saveId = 0;              // the game the state is about
    Save save = Save::Unknown;
    archive::ServerGame saveGame;     // the game the PGN being downloaded is saved as
    std::string savedPath;            // Saved: the file, and the game's index in it
    int savedIndex = 0;
    bool replayWanted = false;        // Replay pressed: the replay starts once the game is saved

    // The page of a game opens (from the history, back from a replay...). A Replay pressed on an
    // earlier visit is given up (a download or write still running ends as a plain save), and a
    // finished state is forgotten so that the saved games are looked at again (the file may have
    // been deleted meanwhile); one still in progress goes on.
    void opened(bool job);
    // Whether the saved games must be looked at for game 'gameId' now: true makes the state about
    // that game (Checking; the page starts the lookup). Not while a lookup, a download or a write
    // runs (the PGN of another game on its way is still saved as that game).
    bool lookupDue(uint64_t gameId, bool job);
    // Save or Replay pressed on game g: true when its PGN must be downloaded (Downloading).
    bool request(const archive::ServerGame& g, bool replay);
    // The PGN awaited (PgnResult while Downloading): true to write it as saveGame, whatever game
    // the page shows now.
    bool pgnArrived(const net::Event& e) const;
    // Whether the replay of game 'gameId' starts now (saved, asked for on this visit).
    bool replayDue(uint64_t gameId);
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

// "14:32": the local time of an epoch-ms instant (the end of a ban or of a matchmaking cooldown),
// "17.10.2026 14:32" when it is not today.
std::string localTimeText(double epochMs);
// "0:45" (a duration).
std::string durationText(double ms);
// Friendly texts (i18n) of the network layer's HTTPS errors: an error code ("invalid_credentials",
// "rate_limited" with the retry delay, "banned" with its end, "network", "tls", "certificate",
// "incompatible"...); game::OnlineSession's other error texts (online_session.h) build on it.
std::string onlineErrorText(const std::string& code, int retryAfterSec = 0, int64_t bannedUntilMs = 0);
// A refused sign-in (LoginResult) in words. justRegistered: the player signs in to the account they
// registered from the sign-in pages, which exists only once its mailed link is used: until then the
// server refuses it as a wrong password (invalid_credentials), and the text says to open the link
// first (told here only: the server never tells a waiting signup apart).
std::string signInErrorText(const net::Event& e, bool justRegistered);
// The answer to Sign out everywhere (LogoutResult of logout(true)) in words: done; refused with the
// session ("unauthorized": this computer is signed out too, so the player signs in again and signs
// the others out from the signed-in devices); or failed otherwise (network, a cut answer, 429,
// 503), which keeps this computer signed in to try again.
std::string signOutEverywhereText(const net::Event& e);

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
    // The server's answer: false when it is not the one awaited (nothing asked, another game), e
    // then left as it was. The one awaited gives its file (e.text) to the write: moved, never
    // copied on the calling thread; a write that cannot start is a write_failed.
    bool finish(net::Event&& e);
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
