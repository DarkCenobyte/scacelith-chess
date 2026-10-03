// The account pages' data (src/game/online_account.h): the history pager (the cursors of the pages
// seen, Previous / Next, an answer kept only when it is the one awaited, errors with the server's
// wait, the page count), what the answers of the account API change (AccountData::apply: the game
// asked for, the devices sorted and signed out, the preference, the account deleted, the token
// refused), the answers awaited (ServerAnswers: those of a server left dropped, except a PGN), Save
// and Replay on a game of the history (GameSaveState: a Replay given up with its page, the PGN of a
// game left still saved as that game, also through a change of server, the saved games looked at
// again on each visit), the result from the player's side, the moves of a server game with their
// clocks, the time control labels, the file name of an account export and the HTTPS errors in
// words; the GIFs of games: their file names, the PGN date and time, the wait in words, and the
// GifSaver (the answer awaited only, the file written never over another, the errors kept with
// their wait).
#include "test.h"
#include "alloc_fail.h"
#include "game/game_archive.h"
#include "game/online_account.h"
#include "i18n/i18n.h"
#include "net/net_sys.h"

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace game;
using Kind = net::Event::Kind;

namespace {

// An answer to the request of cursor 'before' and filter f.
net::GamesPage pageOf(uint64_t before, uint64_t next, int total, int count, uint64_t firstId,
                      const net::GamesFilter& f = net::GamesFilter()) {
    net::GamesPage p;
    p.before = before;
    p.filter = f;
    p.next = next;
    p.total = total;
    for (int i = 0; i < count; ++i) {
        net::GameSummary g;
        g.id = firstId - uint64_t(i);
        p.games.push_back(g);
    }
    return p;
}

net::Event event(Kind k, bool ok = true, const std::string& error = "") {
    net::Event e;
    e.kind = k;
    e.ok = ok;
    e.error = error;
    return e;
}

}  // namespace

TEST(account_history_pages_forward_and_back) {
    HistoryPager h;
    CHECK(!h.loaded());
    CHECK_EQ(h.pageCount(), 1);
    net::GamesFilter f;
    f.rated = 1;
    CHECK_EQ(h.restart(f), uint64_t(0));
    CHECK(h.waiting());
    CHECK_EQ(h.filter().rated, 1);
    uint64_t before = 0;
    CHECK(!h.next(before));  // nothing loaded yet
    CHECK(h.accept(pageOf(0, 900, 25, 10, 1000, f)));
    CHECK(!h.waiting());
    CHECK(h.loaded());
    CHECK_EQ(h.pageIndex(), 0);
    CHECK_EQ(h.pageCount(), 3);
    CHECK(h.hasNext());
    CHECK(!h.hasPrevious());
    CHECK(!h.previous(before));

    CHECK(h.next(before));
    CHECK_EQ(before, uint64_t(900));
    CHECK(!h.next(before));  // one request at a time
    CHECK(h.waiting());
    CHECK(h.accept(pageOf(900, 800, 25, 10, 899, f)));
    CHECK_EQ(h.pageIndex(), 1);
    CHECK(h.hasPrevious());

    CHECK(h.next(before));
    CHECK_EQ(before, uint64_t(800));
    CHECK(h.accept(pageOf(800, 0, 25, 5, 799, f)));
    CHECK_EQ(h.pageIndex(), 2);
    CHECK(!h.hasNext());
    CHECK_EQ(int(h.page().games.size()), 5);

    // Back: the cursor of the page before, kept from the way forward.
    CHECK(h.previous(before));
    CHECK_EQ(before, uint64_t(900));
    CHECK(h.accept(pageOf(900, 800, 25, 10, 899, f)));
    CHECK_EQ(h.pageIndex(), 1);
    CHECK(h.previous(before));
    CHECK_EQ(before, uint64_t(0));
    CHECK(h.accept(pageOf(0, 900, 25, 10, 1000, f)));
    CHECK_EQ(h.pageIndex(), 0);
    CHECK(!h.hasPrevious());
}

TEST(account_history_keeps_only_the_awaited_answer) {
    HistoryPager h;
    h.restart(net::GamesFilter());
    CHECK(!h.accept(pageOf(900, 0, 3, 3, 899)));  // not the cursor asked for
    CHECK(h.waiting());
    CHECK(h.accept(pageOf(0, 900, 12, 10, 1000)));
    CHECK(!h.accept(pageOf(0, 900, 12, 10, 1000)));  // nothing awaited any more
    h.reload();
    CHECK(!h.accept(pageOf(0, 900, 12, 10, 1000, net::GamesFilter{"3+2", -1, ""})));  // another filter
    CHECK(h.accept(pageOf(0, 900, 12, 10, 1000)));

    // A filter changed while a page was coming: the old answer is dropped.
    uint64_t before = 0;
    CHECK(h.next(before));
    net::GamesFilter wins;
    wins.result = "win";
    h.restart(wins);
    CHECK(!h.accept(pageOf(900, 0, 12, 2, 899)));
    CHECK(h.accept(pageOf(0, 0, 4, 4, 1000, wins)));
    CHECK_EQ(h.filter().result, std::string("win"));
    CHECK_EQ(h.pageCount(), 1);
}

// Every answer names its request (GamesPage::before and filter): an answer of an earlier filter,
// or the failure of an earlier request, never stands for the one awaited, in whatever order the
// answers arrive (the fake server's latencies differ from one call to the next).
TEST(account_history_answers_of_other_requests) {
    net::GamesFilter all, rated;
    rated.rated = 1;
    auto answer = [](const net::GamesFilter& f, uint64_t before, uint64_t next, int total, int count, uint64_t firstId) {
        net::Event e = event(Kind::GamesResult);
        e.gamesPage = pageOf(before, next, total, count, firstId);
        e.gamesPage.filter = f;
        return e;
    };
    auto failure = [](const net::GamesFilter& f, uint64_t before, const char* error) {
        net::Event e = event(Kind::GamesResult, false, error);
        e.gamesPage.before = before;
        e.gamesPage.filter = f;
        return e;
    };
    AccountData d;
    net::AccountInfo account;
    bool signedIn = true;

    // Games = All, then Rated before the first answer: All's first page arrives first, dropped.
    d.history.restart(all);
    d.history.restart(rated);
    CHECK(d.apply(answer(all, 0, 900, 45, 10, 1000), account, signedIn));
    CHECK(!d.history.loaded());
    CHECK(d.history.waiting());
    CHECK(d.apply(answer(rated, 0, 980, 20, 10, 990), account, signedIn));
    CHECK(d.history.loaded());
    CHECK(!d.history.waiting());
    CHECK_EQ(d.history.page().total, 20);
    CHECK_EQ(d.history.page().games.front().id, uint64_t(990));

    // Next in flight, then the filter changes and the old Next fails: the new filter's first page
    // is still awaited, and kept when it comes.
    uint64_t before = 0;
    CHECK(d.history.next(before));
    CHECK_EQ(before, uint64_t(980));
    d.history.restart(all);
    CHECK(d.apply(failure(rated, 980, "timeout"), account, signedIn));
    CHECK(d.history.waiting());
    CHECK(d.history.error().empty());
    CHECK(d.apply(answer(all, 0, 900, 45, 10, 1000), account, signedIn));
    CHECK(d.history.loaded());
    CHECK_EQ(d.history.page().total, 45);

    // The same request twice (Account > History while the first is in flight), the first fails:
    // the second still answers it.
    d.history.restart(rated);
    d.history.restart(rated);
    CHECK(d.apply(failure(rated, 0, "rate_limited"), account, signedIn));
    CHECK(d.history.waiting());
    CHECK(d.apply(answer(rated, 0, 980, 20, 10, 990), account, signedIn));
    CHECK(d.history.loaded());
    CHECK(d.history.error().empty());
    // All, Rated, All again: the first All fails, the Rated answer is not the one awaited, the
    // second All is kept.
    d.history.restart(all);
    d.history.restart(rated);
    d.history.restart(all);
    CHECK(d.apply(failure(all, 0, "timeout"), account, signedIn));
    CHECK(d.apply(answer(rated, 0, 980, 20, 10, 990), account, signedIn));
    CHECK(d.history.waiting());
    CHECK(d.apply(answer(all, 0, 900, 45, 10, 1000), account, signedIn));
    CHECK(d.history.loaded());
    CHECK_EQ(d.history.page().total, 45);
    // The failure of the request awaited, with none like it in flight: the error shows.
    CHECK(d.history.next(before));
    CHECK(d.apply(failure(all, 900, "timeout"), account, signedIn));
    CHECK(!d.history.waiting());
    CHECK_EQ(d.history.error(), std::string("timeout"));
    CHECK_EQ(d.history.page().total, 45);  // the page shown stays
}

TEST(account_history_errors_and_reload) {
    HistoryPager h;
    h.restart(net::GamesFilter());
    h.fail(pageOf(0, 0, 0, 0, 0), "");
    CHECK(!h.waiting());
    CHECK(!h.loaded());
    CHECK_EQ(h.error(), std::string("server_error"));
    CHECK_EQ(h.reload(), uint64_t(0));
    CHECK(h.error().empty());
    CHECK(h.accept(pageOf(0, 900, 30, 10, 1000)));
    uint64_t before = 0;
    CHECK(h.next(before));
    h.fail(pageOf(900, 0, 0, 0, 0), "network");
    CHECK_EQ(h.error(), std::string("network"));
    CHECK_EQ(h.pageIndex(), 0);  // the page shown stays
    CHECK_EQ(int(h.page().games.size()), 10);
    CHECK_EQ(h.reload(), uint64_t(0));  // the page shown, not the one that failed
    CHECK(h.accept(pageOf(0, 900, 30, 10, 1000)));
    CHECK(h.next(before));
    CHECK(h.accept(pageOf(900, 800, 30, 10, 899)));
    CHECK_EQ(h.reload(), uint64_t(900));
    CHECK(h.accept(pageOf(900, 800, 30, 10, 899)));
    CHECK_EQ(h.pageIndex(), 1);
}

// A refusal that ends with time (rate_limited: the account's budget of the server) keeps the
// server's wait with the error, for the history, the game and the devices pages; an answer or a
// new request forgets it.
TEST(account_errors_keep_the_server_wait) {
    HistoryPager h;
    h.restart(net::GamesFilter());
    h.fail(pageOf(0, 0, 0, 0, 0), "rate_limited", 60);
    CHECK_EQ(h.error(), std::string("rate_limited"));
    CHECK_EQ(h.retryAfterSec(), 60);
    CHECK_EQ(h.reload(), uint64_t(0));
    CHECK_EQ(h.retryAfterSec(), 0);
    CHECK(h.accept(pageOf(0, 900, 30, 10, 1000)));
    uint64_t before = 0;
    CHECK(h.next(before));
    h.fail(pageOf(900, 0, 0, 0, 0), "rate_limited", 45);
    CHECK_EQ(h.retryAfterSec(), 45);          // the page shown stays, with the error and its wait
    CHECK(h.loaded());
    h.fail(pageOf(900, 0, 0, 0, 0), "network");   // not awaited any more: nothing changes
    CHECK_EQ(h.retryAfterSec(), 45);
    CHECK_EQ(h.reload(), uint64_t(0));
    CHECK(h.accept(pageOf(0, 900, 30, 10, 1000)));
    CHECK(h.error().empty());
    CHECK_EQ(h.retryAfterSec(), 0);

    AccountData d;
    net::AccountInfo account;
    bool signedIn = true;
    d.history.restart(net::GamesFilter());
    net::Event games = event(Kind::GamesResult, false, "rate_limited");
    games.retryAfterSec = 60;
    CHECK(d.apply(games, account, signedIn));
    CHECK_EQ(d.history.error(), std::string("rate_limited"));
    CHECK_EQ(d.history.retryAfterSec(), 60);
    d.gameWanted = 42;
    net::Event game = event(Kind::GameDetailsResult, false, "rate_limited");
    game.gameId = 42;
    game.retryAfterSec = 30;
    CHECK(d.apply(game, account, signedIn));
    CHECK_EQ(d.gameError, std::string("rate_limited"));
    CHECK_EQ(d.gameRetryAfter, 30);
    game = event(Kind::GameDetailsResult);
    game.gameDetails.id = 42;
    CHECK(d.apply(game, account, signedIn));
    CHECK(d.gameError.empty());
    CHECK_EQ(d.gameRetryAfter, 0);
    net::Event sessions = event(Kind::SessionsResult, false, "rate_limited");
    sessions.retryAfterSec = 75;
    CHECK(d.apply(sessions, account, signedIn));
    CHECK_EQ(d.sessionsError, std::string("rate_limited"));
    CHECK_EQ(d.sessionsRetryAfter, 75);
    CHECK(d.apply(event(Kind::SessionsResult), account, signedIn));
    CHECK(d.sessionsError.empty());
    CHECK_EQ(d.sessionsRetryAfter, 0);
    CHECK(signedIn);
}

TEST(account_history_page_count_never_below_the_pages_seen) {
    HistoryPager h;
    h.restart(net::GamesFilter());
    CHECK(h.accept(pageOf(0, 900, 10, 10, 1000)));  // total stale: a game ended meanwhile
    CHECK_EQ(h.pageCount(), 2);
    uint64_t before = 0;
    CHECK(h.next(before));
    CHECK(h.accept(pageOf(900, 0, 11, 1, 899)));
    CHECK_EQ(h.pageCount(), 2);
    h.restart(net::GamesFilter());
    CHECK(h.accept(pageOf(0, 0, 0, 0, 0)));
    CHECK_EQ(h.pageCount(), 1);
}

TEST(account_apply_routes_history_and_game) {
    AccountData d;
    net::AccountInfo account;
    account.username = "Paul_M";
    bool signedIn = true;
    d.history.restart(net::GamesFilter());
    net::Event e = event(Kind::GamesResult);
    e.gamesPage = pageOf(0, 0, 2, 2, 50);
    CHECK(d.apply(e, account, signedIn));
    CHECK(d.history.loaded());

    // Only the game asked for is kept.
    d.gameWanted = 42;
    net::Event g = event(Kind::GameDetailsResult);
    g.gameDetails.id = 41;
    CHECK(d.apply(g, account, signedIn));
    CHECK(!d.gameLoaded);
    g.gameDetails.id = 42;
    g.gameDetails.plies = 3;
    CHECK(d.apply(g, account, signedIn));
    CHECK(d.gameLoaded);
    CHECK_EQ(d.game.plies, 3);
    net::Event missing = event(Kind::GameDetailsResult, false, "not_found");
    missing.gameId = 42;
    CHECK(d.apply(missing, account, signedIn));
    CHECK_EQ(d.gameError, std::string("not_found"));
    CHECK(signedIn);

    // Not an account API answer: nothing changes.
    CHECK(!d.apply(event(Kind::AccountResult, false, "unauthorized"), account, signedIn));
    CHECK(signedIn);
    // The token refused by any of them signs out.
    CHECK(d.apply(event(Kind::GamesResult, false, "unauthorized"), account, signedIn));
    CHECK(!signedIn);
    // So does a public read answered once the refused token was erased (sessionLost, ok).
    signedIn = true;
    net::Event pub = event(Kind::GameDetailsResult);
    pub.sessionLost = true;
    pub.gameDetails.id = 42;
    CHECK(d.apply(pub, account, signedIn));
    CHECK(d.gameLoaded);
    CHECK(!signedIn);
}

// Game A opened, Back, game B opened before A's answer: A's failure is not B's error (B's page
// keeps waiting for its own answer); a refused token signs out whichever game it came with.
TEST(account_apply_game_error_of_a_game_left) {
    AccountData d;
    net::AccountInfo account;
    bool signedIn = true;
    d.gameWanted = 200;
    for (const char* error : {"not_found", "timeout", "rate_limited"}) {
        net::Event a = event(Kind::GameDetailsResult, false, error);
        a.gameId = 100;
        CHECK(d.apply(a, account, signedIn));
        CHECK(d.gameError.empty());
        CHECK(!d.gameLoaded);
    }
    net::Event b = event(Kind::GameDetailsResult, false, "not_found");
    b.gameId = 200;
    CHECK(d.apply(b, account, signedIn));
    CHECK_EQ(d.gameError, std::string("not_found"));
    CHECK(signedIn);
    net::Event refused = event(Kind::GameDetailsResult, false, "unauthorized");
    refused.gameId = 100;
    CHECK(d.apply(refused, account, signedIn));
    CHECK(!signedIn);
}

TEST(account_apply_sessions_sorted_and_revoked) {
    AccountData d;
    net::AccountInfo account;
    bool signedIn = true;
    net::Event e = event(Kind::SessionsResult);
    for (int i = 0; i < 4; ++i) {
        net::SessionInfo s;
        s.id = 10 + i;
        s.lastSeenAtMs = 1000 * (i + 1);
        s.current = (i == 1);
        e.sessions.push_back(s);
    }
    CHECK(d.apply(e, account, signedIn));
    CHECK(d.sessionsLoaded);
    CHECK_EQ(int(d.sessions.size()), 4);
    CHECK_EQ(d.sessions[0].id, int64_t(11));  // this device first
    CHECK_EQ(d.sessions[1].id, int64_t(13));  // then the most recently active
    CHECK_EQ(d.sessions[2].id, int64_t(12));
    CHECK_EQ(d.sessions[3].id, int64_t(10));

    net::Event r = event(Kind::SessionRevoked);
    r.sessionId = 12;
    CHECK(d.apply(r, account, signedIn));
    CHECK_EQ(int(d.sessions.size()), 3);
    r = event(Kind::SessionRevoked, false, "not_found");  // gone already
    r.sessionId = 10;
    CHECK(d.apply(r, account, signedIn));
    CHECK_EQ(int(d.sessions.size()), 2);
    r = event(Kind::SessionRevoked, false, "network");
    r.sessionId = 13;
    CHECK(d.apply(r, account, signedIn));
    CHECK_EQ(int(d.sessions.size()), 2);

    CHECK(d.apply(event(Kind::SessionsResult, false, "rate_limited"), account, signedIn));
    CHECK_EQ(d.sessionsError, std::string("rate_limited"));
    CHECK_EQ(int(d.sessions.size()), 2);  // the list shown stays
}

TEST(account_apply_preferences_and_deletion) {
    AccountData d;
    net::AccountInfo account;
    account.username = "Paul_M";
    account.acceptChallenges = true;
    bool signedIn = true;
    net::Event p = event(Kind::PreferencesResult);
    p.account.acceptChallenges = false;
    CHECK(d.apply(p, account, signedIn));
    CHECK(!account.acceptChallenges);
    p.ok = false;
    p.error = "network";
    p.account.acceptChallenges = true;
    CHECK(d.apply(p, account, signedIn));
    CHECK(!account.acceptChallenges);  // unchanged on failure

    net::Event x = event(Kind::EmailChangeResult);
    x.status = "verification_sent";
    CHECK(d.apply(x, account, signedIn));
    CHECK(d.apply(event(Kind::AccountExportResult), account, signedIn));

    d.gameWanted = 7;
    d.sessionsLoaded = true;
    CHECK(d.apply(event(Kind::AccountDeleted, false, "invalid_password"), account, signedIn));
    CHECK(signedIn);
    CHECK_EQ(account.username, std::string("Paul_M"));
    CHECK(d.apply(event(Kind::AccountDeleted), account, signedIn));
    CHECK(!signedIn);
    CHECK(account.username.empty());
    CHECK_EQ(d.gameWanted, uint64_t(0));
    CHECK(!d.sessionsLoaded);
}

namespace {
using Save = GameSaveState::Save;

archive::ServerGame serverGame(uint64_t id) {
    archive::ServerGame g;
    g.server = "caissa.scacelith.com:443";
    g.gameId = id;
    g.endKey = "reason.checkmate";
    return g;
}
net::Event pgnOf(uint64_t id) {
    net::Event e = event(Kind::PgnResult);
    e.gameId = id;
    e.text = "[Event \"x\"]\n\n1. e4 *\n";
    return e;
}
// What the page does with a PGN arrived and the write that follows (accountPump).
void written(GameSaveState& s, const char* path) {
    s.save = Save::Writing;   // startSave(..., text)
    s.save = Save::Saved;     // the write job's result
    s.savedPath = path;
}
}  // namespace

// Replay pressed on game A, then Back before the download and the write end: they end as a plain
// save, and opening A later only to look at it starts no replay.
TEST(account_game_save_replay_given_up_with_its_page) {
    GameSaveState s;
    s.opened(false);
    CHECK(s.lookupDue(100, false));
    CHECK(s.save == Save::Checking);
    s.save = Save::NotSaved;              // the lookup's answer
    CHECK(!s.lookupDue(100, false));      // once per visit
    CHECK(s.request(serverGame(100), true));
    CHECK(s.save == Save::Downloading);
    CHECK(s.replayWanted);
    // Back: the history page; the PGN comes and is written there.
    CHECK(s.pgnArrived(pgnOf(100)));
    written(s, "/saved/2026-09-28.pgn");
    // Game A opened again.
    s.opened(false);
    CHECK(!s.replayDue(100));
    CHECK(s.lookupDue(100, false));
    s.save = Save::Saved;                 // already saved
    s.savedPath = "/saved/2026-09-28.pgn";
    CHECK(!s.replayDue(100));
    // Replay pressed on this visit: it starts (the game is saved already: no download).
    CHECK(!s.request(serverGame(100), true));
    CHECK(s.replayDue(100));
    CHECK(!s.replayDue(100));

    // The same with the page opened again while the write still runs.
    GameSaveState t;
    t.opened(false);
    CHECK(t.lookupDue(7, false));
    t.save = Save::NotSaved;
    CHECK(t.request(serverGame(7), true));
    CHECK(t.pgnArrived(pgnOf(7)));
    t.save = Save::Writing;
    t.opened(true);
    CHECK(t.save == Save::Writing);       // the write goes on
    t.save = Save::Saved;
    CHECK(!t.replayDue(7));
}

// Save on game A, Back, game B opened before A's PGN arrives: A's PGN is still saved as A, and B's
// lookup waits for it.
TEST(account_game_save_of_a_game_left_still_saved) {
    GameSaveState s;
    s.opened(false);
    CHECK(s.lookupDue(100, false));
    s.save = Save::NotSaved;
    CHECK(s.request(serverGame(100), false));
    s.opened(false);                      // game B's page
    CHECK(s.save == Save::Downloading);
    CHECK(!s.lookupDue(200, false));      // not while A's PGN is on its way
    CHECK(s.pgnArrived(pgnOf(100)));
    CHECK_EQ(s.saveGame.gameId, uint64_t(100));
    CHECK_EQ(s.saveGame.server, std::string("caissa.scacelith.com:443"));
    CHECK_EQ(s.saveGame.endKey, std::string("reason.checkmate"));
    CHECK(!s.pgnArrived(pgnOf(200)));     // only the PGN asked for
    net::Event failed = pgnOf(100);
    failed.ok = false;
    failed.error = "timeout";
    CHECK(!s.pgnArrived(failed));
    s.save = Save::Writing;
    CHECK(!s.lookupDue(200, true));
    written(s, "/saved/2026-09-28.pgn");
    CHECK(s.lookupDue(200, false));       // then B's turn
    CHECK_EQ(s.saveId, uint64_t(200));
    CHECK(s.savedPath.empty());
}

// Another server chosen (Options > Online) while the first one's answers are on their way: they are
// not the new server's (its history page, its devices, its session), and the new server's own
// answers are still awaited and kept.
TEST(account_answers_of_a_server_left) {
    const std::string s1 = "first.example:443", s2 = "second.example:443";
    auto from = [](net::Event e, const std::string& origin) {
        e.origin = origin;
        return e;
    };
    ServerAnswers a;
    a.setServer(s1);
    a.expect(Kind::GamesResult);
    a.expect(Kind::SessionsResult);
    CHECK(a.keep(from(event(Kind::SessionsResult), s1)));
    CHECK(!a.busy(Kind::SessionsResult));
    a.setServer(s2);                     // forgets what was awaited and kept
    CHECK(!a.busy(Kind::GamesResult));
    net::Event out;
    CHECK(!a.take(Kind::SessionsResult, out));
    a.expect(Kind::GamesResult);         // the new server's first page
    net::Event old = from(event(Kind::GamesResult), s1);
    old.gamesPage = pageOf(0, 0, 1, 1, 111111);
    CHECK(a.foreign(old));
    CHECK(!a.keep(old));                 // S1's page: dropped, the wait goes on
    CHECK(a.busy(Kind::GamesResult));
    CHECK(!a.take(Kind::GamesResult, out));
    CHECK(!a.keep(from(event(Kind::GamesResult, false, "timeout"), s1)));   // nor its failure
    CHECK(!a.keep(from(event(Kind::AccountResult, false, "unauthorized"), s1)));
    net::Event mine = from(event(Kind::GamesResult), s2);
    mine.gamesPage = pageOf(0, 0, 1, 1, 222222);
    CHECK(!a.foreign(mine));
    CHECK(a.keep(mine));
    CHECK(!a.busy(Kind::GamesResult));
    CHECK(a.take(Kind::GamesResult, out));
    CHECK_EQ(out.gamesPage.games[0].id, uint64_t(222222));
    CHECK(!a.take(Kind::GamesResult, out));   // handed over once
    // The realtime events name no server: never foreign.
    CHECK(!a.foreign(event(Kind::Welcome)));

    // What the history page does with them (OnlineSession::handleServer applies only the kept ones).
    AccountData d;
    net::AccountInfo account;
    bool signedIn = true;
    d.history.restart(net::GamesFilter());
    d.clear();                           // applyServer()
    d.history.restart(net::GamesFilter());
    if (!a.foreign(old)) d.apply(old, account, signedIn);
    CHECK(!d.history.loaded());
    CHECK(d.history.waiting());
    if (!a.foreign(mine)) d.apply(mine, account, signedIn);
    CHECK(d.history.loaded());
    CHECK_EQ(d.history.page().games[0].id, uint64_t(222222));
}

// Save game pressed, then another server chosen before the PGN was taken by the game page (the
// Online menu left meanwhile, or the answer still on its way): the PGN is kept through the change,
// saved as the game it was asked for, and the page is free for the new server's games after it.
TEST(account_game_save_through_a_change_of_server) {
    const std::string s1 = "caissa.scacelith.com:443", s2 = "second.example:443";
    for (int arrivedFirst = 0; arrivedFirst < 2; ++arrivedFirst) {
        ServerAnswers a;
        a.setServer(s1);
        GameSaveState s;
        s.opened(false);
        CHECK(s.lookupDue(100, false));
        s.save = Save::NotSaved;
        CHECK(s.request(serverGame(100), false));
        a.expect(Kind::PgnResult);
        net::Event pgn = pgnOf(100);
        pgn.origin = s1;
        if (arrivedFirst) CHECK(a.keep(pgn));
        a.setServer(s2);
        if (!arrivedFirst) {
            CHECK(a.busy(Kind::PgnResult));   // still awaited
            CHECK(a.foreign(pgn));
            CHECK(a.keep(pgn));               // and kept, though of the server left
        }
        CHECK(!a.busy(Kind::PgnResult));
        net::Event e;
        CHECK(a.take(Kind::PgnResult, e));    // the game page's pump
        CHECK(s.pgnArrived(e));
        CHECK_EQ(s.saveGame.server, s1);      // saved under the server it came from
        written(s, "/saved/2026-09-28.pgn");
        s.opened(false);                      // a game of the new server
        CHECK(s.lookupDue(300, false));
    }
}

// The saved games are looked at again on each visit: a file deleted meanwhile (or a replay of it
// that failed) does not leave the game stuck on "Saved".
TEST(account_game_save_looked_at_again_on_each_visit) {
    GameSaveState s;
    s.opened(false);
    CHECK(s.lookupDue(100, false));
    s.save = Save::NotSaved;
    CHECK(s.request(serverGame(100), false));
    CHECK(s.pgnArrived(pgnOf(100)));
    written(s, "/saved/2026-09-28.pgn");
    CHECK(!s.lookupDue(100, false));      // the same visit: known
    // The file deleted on the Saved games page, then game A opened again.
    s.opened(false);
    CHECK(s.lookupDue(100, false));
    s.save = Save::NotSaved;
    CHECK(s.request(serverGame(100), true));   // it can be saved (and replayed) again
    CHECK(s.save == Save::Downloading);
    // Opened again while it downloads: the download goes on and is still awaited.
    s.opened(false);
    CHECK(s.save == Save::Downloading);
    CHECK(s.pgnArrived(pgnOf(100)));
}

TEST(account_outcome_from_the_player_side) {
    net::GameSummary g;
    g.you = 0;
    g.status = 1;
    CHECK(outcomeOf(g) == Outcome::Win);
    g.status = 2;
    CHECK(outcomeOf(g) == Outcome::Loss);
    g.you = 1;
    CHECK(outcomeOf(g) == Outcome::Win);
    g.status = 1;
    CHECK(outcomeOf(g) == Outcome::Loss);
    g.status = 3;
    CHECK(outcomeOf(g) == Outcome::Draw);
    g.status = 4;
    CHECK(outcomeOf(g) == Outcome::Aborted);
    g.status = 0;
    CHECK(outcomeOf(g) == Outcome::None);
    g.status = 1;
    g.you = 2;
    CHECK(outcomeOf(g) == Outcome::None);
}

TEST(account_game_moves_with_clocks) {
    net::GameDetails g;
    const char* uci[] = {"e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "e1g1"};
    int64_t clock = 180000;
    for (const char* u : uci) {
        net::GameDetails::Ply p;
        p.uci = u;
        p.clockMs = clock;
        p.spentMs = 1500;
        clock -= 1000;
        g.moves.push_back(p);
    }
    // A ply may carry the packed move only.
    g.moves[0].move = net::packMove(12, 28, 0);  // e2e4
    g.moves[0].uci.clear();
    bool complete = false;
    std::vector<MoveLine> lines = gameMoves(g, &complete);
    CHECK(complete);
    CHECK_EQ(int(lines.size()), 7);
    CHECK_EQ(lines[0].san, std::string("e4"));
    CHECK_EQ(lines[4].san, std::string("Bb5"));
    CHECK_EQ(lines[6].san, std::string("O-O"));
    CHECK_EQ(lines[1].clockMs, int64_t(179000));
    CHECK_EQ(lines[1].spentMs, int64_t(1500));

    // A move that does not play ends the list.
    net::GameDetails::Ply bad;
    bad.uci = "e4e6";
    g.moves.insert(g.moves.begin() + 2, bad);
    lines = gameMoves(g, &complete);
    CHECK(!complete);
    CHECK_EQ(int(lines.size()), 2);

    // No moves (a game aborted before the first one).
    lines = gameMoves(net::GameDetails(), &complete);
    CHECK(complete);
    CHECK(lines.empty());
}

TEST(account_time_control_labels) {
    CHECK_EQ(timeControlLabel(180000, 2000), std::string("3+2"));
    CHECK_EQ(timeControlLabel(600000, 0), std::string("10+0"));
    CHECK_EQ(timeControlLabel(90000, 1000), std::string("1:30+1"));
    CHECK_EQ(timeControlLabel(30000, 0), std::string("0:30+0"));
    CHECK_EQ(timeControlLabel(-5, -5), std::string("0+0"));
}

// The end of a ban or a cooldown: its own time (std::localtime's shared buffer once made every
// instant read as now), with the date when it is another day.
TEST(account_local_time_text) {
    const std::time_t now = std::time(nullptr), later = now + 3 * 86400 + 5 * 3600 + 17 * 60;
    std::tm tm{};
    CHECK(archive::localTime(later, tm));
    char want[64];
    std::snprintf(want, sizeof want, "%02d.%02d.%04d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    CHECK_EQ(localTimeText(double(later) * 1000.0), i18n::ltr(want));
    CHECK(archive::localTime(now, tm));
    std::snprintf(want, sizeof want, "%02d:%02d", tm.tm_hour, tm.tm_min);
    CHECK_EQ(localTimeText(double(now) * 1000.0), i18n::ltr(want));
}

// A sign-in whose session could not be saved here (net: its token not protected by DPAPI, error
// "storage") is told as this computer's failure, not as the server's refusal (online.err.other).
TEST(account_error_text_of_a_session_not_saved) {
    for (const i18n::Language& lang : i18n::languages()) {
        CHECK(i18n::setLanguage(lang.code));
        const std::string text = onlineErrorText("storage");
        CHECK(i18n::has("online.err.storage"));
        CHECK_EQ(text, std::string(i18n::tr("online.err.storage")));
        CHECK(text != i18n::trf("online.err.other", {"storage"}));
        if (std::string(lang.code) != "en") CHECK(text != i18n::english("online.err.storage"));
    }
    CHECK(i18n::setLanguage("en"));
    CHECK_EQ(onlineErrorText("storage"), std::string("The session could not be saved on this computer."));
    CHECK_EQ(onlineErrorText("disk_full"), std::string("The server refused (disk_full)."));
}

// Sign out everywhere: done; refused with the session (401, "unauthorized": this computer is signed
// out too, so the player signs in again first); or failed otherwise (network, a cut answer, 429,
// 503), which keeps this computer signed in: the player tries again.
TEST(account_sign_out_everywhere_text) {
    CHECK(i18n::setLanguage("en"));
    net::Event e;
    e.kind = Kind::LogoutResult;
    e.ok = true;
    CHECK_EQ(signOutEverywhereText(e), std::string("You are signed out on every computer."));
    e.ok = false;
    e.error = "unauthorized";
    CHECK_EQ(signOutEverywhereText(e), std::string("Your other computers may still be signed in: sign in again, then sign them out "
                                                   "from Signed-in devices. Your session has ended: please sign in again."));
    e.error = "network";
    CHECK_EQ(signOutEverywhereText(e), std::string("Your other computers may still be signed in. Try Sign out everywhere again. "
                                                   "The server cannot be reached. Check your connection and the server address."));
    e.error = "rate_limited";
    e.retryAfterSec = 30;
    CHECK_EQ(signOutEverywhereText(e), i18n::trf("online.account.sign_out_all_retry", {onlineErrorText("rate_limited", 30)}));
    e.retryAfterSec = 0;
    for (const char* error : {"invalid_response", "maintenance", "server_error", "timeout"}) {
        e.error = error;
        CHECK_EQ(signOutEverywhereText(e), i18n::trf("online.account.sign_out_all_retry", {onlineErrorText(error)}));
    }
    for (const i18n::Language& lang : i18n::languages()) {
        CHECK(i18n::setLanguage(lang.code));
        e.error = "unauthorized";
        const std::string signedOut = signOutEverywhereText(e);
        e.error = "network";
        const std::string retry = signOutEverywhereText(e);
        CHECK(signedOut != retry);
        CHECK(retry.find(onlineErrorText("network")) != std::string::npos);
        if (std::string(lang.code) != "en")
            CHECK(std::string(i18n::tr("online.account.sign_out_all_retry")) != i18n::english("online.account.sign_out_all_retry"));
    }
    CHECK(i18n::setLanguage("en"));
}

// A refused sign-in: only to the account just registered from the sign-in pages does a wrong
// password say to open the mailed link first (the account exists only once it is used); every
// other refusal, and that one otherwise, keeps its plain text.
TEST(account_sign_in_error_text) {
    CHECK(i18n::setLanguage("en"));
    net::Event e;
    e.kind = Kind::LoginResult;
    e.error = "invalid_credentials";
    CHECK_EQ(signInErrorText(e, false), std::string("Wrong user name or password."));
    CHECK_EQ(signInErrorText(e, true),
             std::string("Wrong user name or password. If you have just signed up, open the link we sent you first."));
    e.error = "rate_limited";
    e.retryAfterSec = 30;
    CHECK_EQ(signInErrorText(e, true), onlineErrorText("rate_limited", 30));
    e.retryAfterSec = 0;
    e.error = "banned";
    e.account.bannedUntilMs = 4102444800000;
    CHECK_EQ(signInErrorText(e, true), onlineErrorText("banned", 0, 4102444800000));
    for (const char* error : {"email_unverified", "network", "invalid_code", "too_many_attempts"}) {
        e.error = error;
        CHECK_EQ(signInErrorText(e, true), onlineErrorText(error));
        CHECK_EQ(signInErrorText(e, false), onlineErrorText(error));
    }
    e.error = "invalid_credentials";
    for (const i18n::Language& lang : i18n::languages()) {
        CHECK(i18n::setLanguage(lang.code));
        CHECK_EQ(signInErrorText(e, false), onlineErrorText("invalid_credentials"));
        CHECK_EQ(signInErrorText(e, true), std::string(i18n::tr("online.err.invalid_credentials_pending")));
        CHECK(signInErrorText(e, true) != signInErrorText(e, false));
        if (std::string(lang.code) != "en")
            CHECK(std::string(i18n::tr("online.err.invalid_credentials_pending")) != i18n::english("online.err.invalid_credentials_pending"));
    }
    CHECK(i18n::setLanguage("en"));
}

TEST(account_export_file_name) {
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 9;
    tm.tm_mday = 1;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    const std::time_t when = std::mktime(&tm);
    CHECK_EQ(exportFileName("chess.example.org", "Paul_M", when), std::string("chess.example.org_Paul_M_2026-10-01.json"));
    const std::string odd = exportFileName("a/b:c", "x\\y", when);
    CHECK(odd.find('/') == std::string::npos);
    CHECK(odd.find('\\') == std::string::npos);
    CHECK(odd.find(':') == std::string::npos);
    CHECK(odd.size() > 16 && odd.compare(odd.size() - 16, 16, "_2026-10-01.json") == 0);
}

// ---- GIFs of games ------------------------------------------------------------------------------------

namespace {

std::time_t localTime(int y, int mo, int d, int h, int mi, int s) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

net::Event gifEvent(uint64_t gameId, const std::string& bytes) {
    net::Event e = event(Kind::GifResult);
    e.gameId = gameId;
    e.text = bytes;
    return e;
}

std::string readAll(const std::string& path) {
    std::string text;
    net::sys::readFile(path, text, 1 << 20);
    return text;
}

// A fresh folder beside the test executable; the files named are removed with it.
struct GifFolder {
    std::string path;
    explicit GifFolder(const char* tag) {
#ifdef _WIN32
        const unsigned pid = unsigned(GetCurrentProcessId());
#else
        const unsigned pid = unsigned(getpid());
#endif
        path = net::sys::exeDirectory() + "gif-test-" + tag + "-" + std::to_string(pid);
    }
    void remove(const std::vector<std::string>& names) const {
        for (const std::string& n : names) std::remove(game::archive::joinPath(path, n).c_str());
#ifdef _WIN32
        RemoveDirectoryA(path.c_str());
#else
        rmdir(path.c_str());
#endif
    }
};

}  // namespace

TEST(account_gif_file_names) {
    const std::time_t when = localTime(2026, 9, 27, 21, 47, 5);
    CHECK_EQ(gifFileName(when, "Magnus_T", "bob", 812), std::string("2026-09-27_214705_Magnus_T-vs-bob_812.gif"));
    CHECK_EQ(gifFileName(when, "Magnus_T", "bob", 0), std::string("2026-09-27_214705_Magnus_T-vs-bob.gif"));  // not a server's
    CHECK_EQ(gifFileName(0, "", "", 0), std::string("0000-00-00_000000_Unknown-vs-Unknown.gif"));
    CHECK_EQ(gifFileName(-5, "a", "b", 4100000000123ull), std::string("0000-00-00_000000_a-vs-b_4100000000123.gif"));
    const std::string odd = gifFileName(when, "a/b:c*?", "..\\x<y>|\"", 9);
    for (char c : std::string("/\\:*?<>|\"")) CHECK(odd.find(c) == std::string::npos);
    CHECK(odd.compare(0, 18, "2026-09-27_214705_") == 0);
    CHECK(odd.size() > 6 && odd.compare(odd.size() - 6, 6, "_9.gif") == 0);
    const std::string longName = gifFileName(when, std::string(300, 'w'), std::string(300, 'b'), 1);
    CHECK(longName.size() < 120);  // each name cut to 40 bytes

    // The start of a saved game, from its Date and Time tags.
    CHECK_EQ(pgnLocalTime("2026.09.27", "21:47:05", 77), when);
    CHECK_EQ(pgnLocalTime("2026.09.27", "21:47", 77), when - 5);
    CHECK_EQ(pgnLocalTime("2026.09.27", "", 77), localTime(2026, 9, 27, 0, 0, 0));
    CHECK_EQ(pgnLocalTime("2026.09.27", "??:??:??", 77), localTime(2026, 9, 27, 0, 0, 0));
    CHECK_EQ(pgnLocalTime("2026.??.??", "21:47:05", 77), std::time_t(77));
    CHECK_EQ(pgnLocalTime("2026.13.01", "", 77), std::time_t(77));
    CHECK_EQ(pgnLocalTime("", "", 77), std::time_t(77));
    CHECK_EQ(gifFileName(pgnLocalTime("2026.09.27", "21:47:05", 0), "Magnus_T", "bob", 812),
             std::string("2026-09-27_214705_Magnus_T-vs-bob_812.gif"));

    // The local Time first, else the server's UTC tags, else the local Date at midnight.
    CHECK_EQ(pgnGameStart("2026.09.27", "21:47:05", "2026.09.27", "19:47:05", 77), when);
    CHECK_EQ(pgnGameStart("2026.09.27", "", "2026.09.27", "21:47:12", 77), std::time_t(1790545632));
    CHECK_EQ(pgnGameStart("2026.09.27", "??:??:??", "2000.02.29", "00:00:00", 77), std::time_t(951782400));
    CHECK_EQ(pgnGameStart("????.??.??", "", "1970.01.01", "00:00:01", 77), std::time_t(1));
    CHECK_EQ(pgnGameStart("2026.09.27", "", "2026.09.27", "21:47", 77), localTime(2026, 9, 27, 0, 0, 0));
    CHECK_EQ(pgnGameStart("2026.09.27", "", "", "", 77), localTime(2026, 9, 27, 0, 0, 0));
    CHECK_EQ(pgnGameStart("", "", "2026.13.27", "21:47:12", 77), std::time_t(77));
    CHECK_EQ(pgnGameStart("", "", "", "", 77), std::time_t(77));
}

TEST(account_gif_wait_in_words) {
    CHECK(i18n::setLanguage("en"));
    CHECK_EQ(waitText(0), std::string("1 second"));
    CHECK_EQ(waitText(1), std::string("1 second"));
    CHECK_EQ(waitText(45), std::string("45 seconds"));
    CHECK_EQ(waitText(60), std::string("1 minute"));
    CHECK_EQ(waitText(61), std::string("1 minute and 1 second"));
    CHECK_EQ(waitText(150), std::string("2 minutes and 30 seconds"));
    CHECK_EQ(waitText(240), std::string("4 minutes"));
    CHECK_EQ(waitText(299), std::string("4 minutes and 59 seconds"));
    CHECK_EQ(waitText(300), std::string("5 minutes"));
    CHECK_EQ(waitText(301), std::string("6 minutes"));  // whole minutes, rounded up
    CHECK_EQ(waitText(3540), std::string("59 minutes"));
    CHECK(i18n::setLanguage("fr"));
    CHECK_EQ(waitText(150), std::string("2 minutes et 30 secondes"));
    CHECK_EQ(waitText(1), std::string("1 seconde"));
    for (const char* lang : {"de", "es", "uk", "ru", "ar", "ja", "zh-Hans", "zh-Hant"}) {
        CHECK(i18n::setLanguage(lang));
        for (int s : {1, 2, 3, 11, 59, 61, 150, 299, 1800}) {
            const std::string w = waitText(s);
            CHECK(!w.empty());
            CHECK(w.find('{') == std::string::npos);
        }
        CHECK(waitText(150).find("30") != std::string::npos);
        CHECK(waitText(1800).find("30") != std::string::npos);
    }
    CHECK(i18n::setLanguage("en"));
}

TEST(account_gif_saver_writes_never_over_a_file) {
    using Stage = GifSaver::Stage;
    GifFolder f("saver");
    const std::string bytes("GIF89a\0\x01\x02\xFF;", 11), other("GIF87a-other", 12);
    {
        GifSaver g;
        CHECK(g.stage() == Stage::Idle);
        CHECK(!g.busy());
        CHECK(!g.poll(true));
        CHECK(!g.finish(gifEvent(812, bytes)));  // nothing asked
        CHECK(g.begin("history:812", 812, f.path, "game.gif"));
        CHECK(g.busy());
        CHECK(g.stage() == Stage::Rendering);
        CHECK_EQ(g.owner(), std::string("history:812"));
        CHECK(!g.begin("library:k", 0, f.path, "other.gif"));  // one at a time
        CHECK_EQ(g.owner(), std::string("history:812"));
        CHECK(!g.finish(gifEvent(5, bytes)));  // another game's answer
        CHECK(!g.finish(gifEvent(0, bytes)));  // a PGN text's
        CHECK(!g.finish(event(Kind::PgnResult)));
        CHECK(g.stage() == Stage::Rendering);
        CHECK(g.finish(gifEvent(812, bytes)));
        CHECK(!g.finish(gifEvent(812, bytes)));  // once
        CHECK(g.busy());                         // being written
        CHECK(!g.begin("library:k", 0, f.path, "other.gif"));
        CHECK(g.poll(true));
        CHECK(g.stage() == Stage::Saved);
        CHECK(!g.busy());
        CHECK(!g.poll(true));  // said once
        CHECK_EQ(g.path(), game::archive::joinPath(f.path, "game.gif"));
        CHECK_EQ(readAll(g.path()), bytes);  // byte for byte
        const std::string first = g.path();

        // The same name again: beside the first file, which stays as it was.
        CHECK(g.begin("library:k", 0, f.path, "game.gif"));
        CHECK(g.path().empty());
        CHECK(g.finish(gifEvent(0, other)));
        CHECK(g.poll(true));
        CHECK_EQ(g.path(), game::archive::joinPath(f.path, "game_2.gif"));
        CHECK_EQ(readAll(g.path()), other);
        CHECK_EQ(readAll(first), bytes);

        // The server's refusal, kept with its wait; nothing written.
        CHECK(g.begin("history:9", 9, f.path, "late.gif"));
        net::Event no = event(Kind::GifResult, false, "rate_limited");
        no.gameId = 9;
        no.retryAfterSec = 150;
        CHECK(g.finish(std::move(no)));
        CHECK(g.stage() == Stage::Failed);
        CHECK(!g.busy());
        CHECK_EQ(g.error(), std::string("rate_limited"));
        CHECK_EQ(g.retryAfterSec(), 150);
        CHECK_EQ(g.owner(), std::string("history:9"));
        CHECK(!net::sys::fileExists(game::archive::joinPath(f.path, "late.gif")));
        CHECK(g.begin("history:9", 9, f.path, "late.gif"));  // asked again: the error forgotten
        CHECK(g.error().empty());
        CHECK_EQ(g.retryAfterSec(), 0);
        net::Event bare = event(Kind::GifResult, false, "");
        bare.gameId = 9;
        CHECK(g.finish(std::move(bare)));
        CHECK_EQ(g.error(), std::string("server_error"));

        // A folder that cannot be made (under a file): write_failed.
        CHECK(g.begin("library:k", 0, game::archive::joinPath(first, "sub"), "x.gif"));
        CHECK(g.finish(gifEvent(0, bytes)));
        CHECK(g.poll(true));
        CHECK(g.stage() == Stage::Failed);
        CHECK_EQ(g.error(), std::string("write_failed"));
        CHECK(g.path().empty());

        g.clear();
        CHECK(g.stage() == Stage::Idle);
        CHECK(g.owner().empty());
        CHECK(g.error().empty());

        // Destroyed while writing: the write ends first.
        CHECK(g.begin("library:k", 0, f.path, "game.gif"));
        CHECK(g.finish(gifEvent(0, other)));
    }
    CHECK_EQ(readAll(game::archive::joinPath(f.path, "game_3.gif")), other);
    f.remove({"game.gif", "game_2.gif", "game_3.gif"});
    CHECK(!net::sys::fileExists(game::archive::joinPath(f.path, "game.gif")));
}

// The file's bytes (a GIF of up to 16 MiB) go from the answer to the thread that writes them
// without a copy on the calling thread (the game's frame); a write that cannot start (no memory
// left, no thread) is a write_failed, never a saver left busy nor an exception out of the frame.
TEST(account_gif_saver_takes_the_bytes) {
    using Stage = GifSaver::Stage;
    if (!allocfail::available()) SKIP("AddressSanitizer build: no simulated out of memory");
    allocfail::Reset reset;
    GifFolder f("saver-take");
    {
        GifSaver g;
        CHECK(g.begin("history:812", 812, f.path, "big.gif"));
        net::Event e = gifEvent(812, "GIF89a" + std::string(net::OnlineClient::kGifMaxBytes - 6, '\x01'));
        allocfail::countFrom(size_t(1) << 20);
        CHECK(g.finish(std::move(e)));
        const size_t copied = allocfail::countedHere();
        allocfail::countFrom(0);
        CHECK_EQ(copied, size_t(0));
        CHECK(g.poll(true));
        CHECK(g.stage() == Stage::Saved);
        uint64_t size = 0;
        CHECK(net::sys::fileSize(g.path(), size));
        CHECK_EQ(size, uint64_t(net::OnlineClient::kGifMaxBytes));

        CHECK(g.begin("history:812", 812, f.path, "late.gif"));
        net::Event small = gifEvent(812, std::string("GIF89a;"));
        bool threw = false;
        allocfail::failNextHere();
        try {
            CHECK(g.finish(std::move(small)));
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(g.stage() == Stage::Failed);
        CHECK(!g.busy());
        CHECK_EQ(g.error(), std::string("write_failed"));
        CHECK(!g.poll(true));
        CHECK(!net::sys::fileExists(game::archive::joinPath(f.path, "late.gif")));
        CHECK(g.begin("history:812", 812, f.path, "late.gif"));   // the next GIF may start
    }
    f.remove({"big.gif"});
    CHECK(!net::sys::fileExists(game::archive::joinPath(f.path, "big.gif")));
}

// The GIF routes need the session. Their answers follow the network layer's one convention, as
// every account call: a session refused by the server is "unauthorized" with sessionLost, none
// saved is "unauthorized" (nothing sent); both sign out. The GIF's own errors keep the player in.
TEST(account_apply_gif_result) {
    AccountData d;
    net::AccountInfo account;
    account.username = "Paul_M";
    bool signedIn = true;
    CHECK(d.apply(gifEvent(812, "GIF89a"), account, signedIn));
    CHECK(signedIn);
    for (const char* kept : {"rate_limited", "server_busy", "game_too_long", "not_found", "network", "gif_disabled", "invalid_pgn",
                             "pgn_too_large", "invalid_game_id", "invalid_response"}) {
        CHECK(d.apply(event(Kind::GifResult, false, kept), account, signedIn));
        CHECK(signedIn);
    }
    CHECK_EQ(account.username, std::string("Paul_M"));
    // Refused by the server (its 401 erased the token).
    net::Event refused = event(Kind::GifResult, false, "unauthorized");
    refused.gameId = 812;
    refused.sessionLost = true;
    CHECK(d.apply(refused, account, signedIn));
    CHECK(!signedIn);
    // No session saved.
    signedIn = true;
    CHECK(d.apply(event(Kind::GifResult, false, "unauthorized"), account, signedIn));
    CHECK(!signedIn);
    // The server's code for a refused bearer and the old local code are not the network layer's:
    // neither a GIF nor any other answer has a sign-out rule of its own for them.
    for (const char* code : {"invalid_token", "not_logged_in"}) {
        signedIn = true;
        CHECK(d.apply(event(Kind::GifResult, false, code), account, signedIn));
        CHECK(signedIn);
        CHECK(d.apply(event(Kind::PgnResult, false, code), account, signedIn));
        CHECK(signedIn);
    }
}
