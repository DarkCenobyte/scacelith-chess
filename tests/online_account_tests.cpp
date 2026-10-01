// The account pages' data (src/game/online_account.h): the history pager (the cursors of the pages
// seen, Previous / Next, an answer kept only when it is the one awaited, errors, the page count),
// what the answers of the account API change (AccountData::apply: the game asked for, the devices
// sorted and signed out, the preference, the account deleted, the token refused), the result from
// the player's side, the moves of a server game with their clocks, the time control labels and the
// file name of an account export; the GIFs of games: their file names, the PGN date and time, the
// wait in words, and the GifSaver (the answer awaited only, the file written never over another,
// the errors kept with their wait).
#include "test.h"
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

net::GamesPage pageOf(uint64_t before, uint64_t next, int total, int count, uint64_t firstId) {
    net::GamesPage p;
    p.before = before;
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
    CHECK(h.accept(pageOf(0, 900, 25, 10, 1000)));
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
    CHECK(h.accept(pageOf(900, 800, 25, 10, 899)));
    CHECK_EQ(h.pageIndex(), 1);
    CHECK(h.hasPrevious());

    CHECK(h.next(before));
    CHECK_EQ(before, uint64_t(800));
    CHECK(h.accept(pageOf(800, 0, 25, 5, 799)));
    CHECK_EQ(h.pageIndex(), 2);
    CHECK(!h.hasNext());
    CHECK_EQ(int(h.page().games.size()), 5);

    // Back: the cursor of the page before, kept from the way forward.
    CHECK(h.previous(before));
    CHECK_EQ(before, uint64_t(900));
    CHECK(h.accept(pageOf(900, 800, 25, 10, 899)));
    CHECK_EQ(h.pageIndex(), 1);
    CHECK(h.previous(before));
    CHECK_EQ(before, uint64_t(0));
    CHECK(h.accept(pageOf(0, 900, 25, 10, 1000)));
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

    // A filter changed while a page was coming: the old answer is dropped.
    uint64_t before = 0;
    CHECK(h.next(before));
    net::GamesFilter wins;
    wins.result = "win";
    h.restart(wins);
    CHECK(!h.accept(pageOf(900, 0, 12, 2, 899)));
    CHECK(h.accept(pageOf(0, 0, 4, 4, 1000)));
    CHECK_EQ(h.filter().result, std::string("win"));
    CHECK_EQ(h.pageCount(), 1);
}

TEST(account_history_errors_and_reload) {
    HistoryPager h;
    h.restart(net::GamesFilter());
    h.fail("");
    CHECK(!h.waiting());
    CHECK(!h.loaded());
    CHECK_EQ(h.error(), std::string("server_error"));
    CHECK_EQ(h.reload(), uint64_t(0));
    CHECK(h.error().empty());
    CHECK(h.accept(pageOf(0, 900, 30, 10, 1000)));
    uint64_t before = 0;
    CHECK(h.next(before));
    h.fail("network");
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
    h.clear();
    CHECK(!h.loaded());
    CHECK(!h.waiting());
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
    CHECK(d.apply(event(Kind::GameDetailsResult, false, "not_found"), account, signedIn));
    CHECK_EQ(d.gameError, std::string("not_found"));
    CHECK(signedIn);

    // Not an account API answer: nothing changes.
    CHECK(!d.apply(event(Kind::AccountResult, false, "unauthorized"), account, signedIn));
    CHECK(signedIn);
    // The token refused by any of them signs out.
    CHECK(d.apply(event(Kind::GamesResult, false, "unauthorized"), account, signedIn));
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
        CHECK(g.finish(no));
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
        CHECK(g.finish(bare));
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

TEST(account_apply_gif_result) {
    AccountData d;
    net::AccountInfo account;
    account.username = "Paul_M";
    bool signedIn = true;
    CHECK(d.apply(gifEvent(812, "GIF89a"), account, signedIn));
    CHECK(signedIn);
    for (const char* kept : {"rate_limited", "server_busy", "game_too_long", "not_found", "network"}) {
        CHECK(d.apply(event(Kind::GifResult, false, kept), account, signedIn));
        CHECK(signedIn);
    }
    CHECK_EQ(account.username, std::string("Paul_M"));
    for (const char* out : {"unauthorized", "invalid_token", "not_logged_in"}) {
        signedIn = true;
        CHECK(d.apply(event(Kind::GifResult, false, out), account, signedIn));
        CHECK(!signedIn);
    }
    // Other answers keep the narrower rule.
    signedIn = true;
    CHECK(d.apply(event(Kind::PgnResult, false, "not_logged_in"), account, signedIn));
    CHECK(signedIn);
}
