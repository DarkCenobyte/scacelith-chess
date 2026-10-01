// The account pages' data on the game's side (the account API of the dedicated server,
// dedicated-server/docs/API.md): the pages of the player's game history, the game opened from it,
// the signed-in devices, and what the answers change in the account. game::OnlineSession keeps one
// AccountData and routes the answers of net::OnlineClient to it (AccountData::apply); the pages
// (ui/ui_screens_account.cpp) read it. Engine-free (no GL, no UI): unit-tested in
// tests/online_account_tests.cpp.
#pragma once
#include "../net/online_client.h"
#include "game_archive.h"
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace game {

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
    // page shown stays with the error.
    void fail(const net::GamesPage& request, const std::string& error);
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
    // The signed-in devices (GET /auth/sessions).
    bool sessionsLoaded = false;
    std::vector<net::SessionInfo> sessions;
    std::string sessionsError;

    void clear();
    // Applies an answer of the account API: keeps what the pages show (the history page awaited,
    // the game asked for, the devices, a device signed out) and updates the account (accept
    // challenges; an account deleted signs out and forgets it; "unauthorized" or sessionLost, the
    // token refused, signs out). False for the other kinds of events (nothing changed).
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

// The file of an account export: "<host>_<username>_<YYYY-MM-DD>.json", the date in local time, the
// host and the user name made safe for a file name (game::archive::sanitizeName).
std::string exportFileName(const std::string& host, const std::string& username, std::time_t when);

}  // namespace game
