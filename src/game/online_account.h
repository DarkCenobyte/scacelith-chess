// The account pages' data on the game's side (the account API of the dedicated server,
// dedicated-server/docs/API.md): the pages of the player's game history, the game opened from it,
// the signed-in devices, and what the answers change in the account. game::OnlineSession keeps one
// AccountData and routes the answers of net::OnlineClient to it (AccountData::apply); the pages
// (ui/ui_screens_account.cpp) read it. Engine-free (no GL, no UI): unit-tested in
// tests/online_account_tests.cpp.
#pragma once
#include "../net/online_client.h"
#include <cstdint>
#include <ctime>
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
    // challenges; an account deleted signs out and forgets it; "unauthorized" or sessionLost, the
    // token refused, signs out). False for the other kinds of events (nothing changed).
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

}  // namespace game
