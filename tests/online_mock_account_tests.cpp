// The account API of the in-process fake server (src/game/online_mock.h), which the account pages
// and their screenshots use: the generated game history (every game legal, endings that match the
// moves, clocks that follow the time control, the ratings of rated games), its pages and filters
// as the server serves them (GET /account/games), the details and the PGN of each game (read back
// through the saved games' importer), a game played against the fake added to the history, the
// signed-in devices, the preference, the e-mail change, the data export and the deletion, with the
// special inputs of the re-authentication; the animated GIFs (decoded by a reader of the test's
// own: sizes, frames, delays, the pieces, the last move, the check, either side), their quota, their
// special inputs and their route to the file. On the fakes' virtual clock (deterministic).
#include "test.h"
#include "chess/chess.h"
#include "chess/pgn.h"
#include "game/game_archive.h"
#include "game/online_account.h"
#include "game/online_mock.h"
#include "net/json.h"
#include "net/net_sys.h"

#include <cstdio>
#include <ctime>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using net::Event;
using Kind = net::Event::Kind;
namespace mock = net::mock;

namespace {

// The virtual clock for the test's lifetime.
struct VirtualClock {
    bool was;
    VirtualClock() : was(mock::virtualClock()) { mock::useVirtualClock(true); }
    ~VirtualClock() { mock::useVirtualClock(was); }
};

// Runs the fake until it delivers an event of kind 'k' (the others are dropped); false after a
// minute of its time without one.
bool await(mock::FakeServer& srv, Kind k, Event& out) {
    for (int i = 0; i < 1200; ++i) {
        Event e;
        while (srv.poll(e))
            if (e.kind == k) {
                out = e;
                return true;
            }
        mock::advance(50.0);
    }
    return false;
}

void signIn(mock::FakeServer& srv, const std::string& name) {
    net::ServerEndpoint ep;
    ep.host = "fake.example.org";
    srv.setServer(ep);
    srv.login(name, "correct horse battery");
    Event e;
    CHECK(await(srv, Kind::LoginResult, e));
    if (e.mfaRequired) {
        srv.loginMfa("123456");
        CHECK(await(srv, Kind::LoginResult, e));
    }
    CHECK(e.ok);
}

std::vector<net::GameSummary> allGames(mock::FakeServer& srv, const net::GamesFilter& filter, int pageSize, int* total) {
    std::vector<net::GameSummary> out;
    uint64_t before = 0;
    for (int guard = 0; guard < 100; ++guard) {
        srv.fetchMyGames(before, pageSize, filter);
        Event e;
        CHECK(await(srv, Kind::GamesResult, e));
        CHECK(e.ok);
        CHECK_EQ(e.gamesPage.before, before);
        CHECK(int(e.gamesPage.games.size()) <= pageSize);
        if (total) *total = e.gamesPage.total;
        out.insert(out.end(), e.gamesPage.games.begin(), e.gamesPage.games.end());
        if (!e.gamesPage.next) break;
        CHECK_EQ(e.gamesPage.next, e.gamesPage.games.back().id);
        before = e.gamesPage.next;
    }
    return out;
}

}  // namespace

TEST(mock_account_history_pages_and_filters) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    int total = 0;
    const std::vector<net::GameSummary> all = allGames(srv, net::GamesFilter(), 10, &total);
    CHECK(total >= 40 && total <= 50);
    CHECK_EQ(int(all.size()), total);
    for (size_t i = 1; i < all.size(); ++i) {
        CHECK(all[i].id < all[i - 1].id);  // newest first, no game twice
        CHECK(all[i].endedAtMs <= all[i - 1].startedAtMs);
    }
    // The same games whatever the page size.
    const std::vector<net::GameSummary> big = allGames(srv, net::GamesFilter(), 50, nullptr);
    CHECK_EQ(big.size(), all.size());

    int wins = 0, losses = 0, draws = 0, aborted = 0, rated = 0, custom = 0;
    for (const net::GameSummary& g : all) {
        CHECK(g.you == 0 || g.you == 1);
        CHECK_EQ((g.you == 0 ? g.white : g.black).name, std::string("Paul_M"));
        switch (game::outcomeOf(g)) {
        case game::Outcome::Win: ++wins; break;
        case game::Outcome::Loss: ++losses; break;
        case game::Outcome::Draw: ++draws; break;
        case game::Outcome::Aborted: ++aborted; break;
        default: CHECK(false);
        }
        rated += g.rated;
        if (g.category == "custom") {
            ++custom;
            CHECK(!g.rated);
        }
    }
    CHECK(wins > 0 && losses > 0 && draws > 0 && aborted > 0);
    CHECK(rated > 0 && rated < total);

    net::GamesFilter f;
    f.result = "win";
    int n = 0;
    for (const net::GameSummary& g : allGames(srv, f, 10, &n)) CHECK(game::outcomeOf(g) == game::Outcome::Win);
    CHECK_EQ(n, wins);
    f.result = "loss";
    allGames(srv, f, 10, &n);
    CHECK_EQ(n, losses);
    f.result = "draw";
    allGames(srv, f, 10, &n);
    CHECK_EQ(n, draws);  // aborted games only without a result filter
    f = net::GamesFilter();
    f.rated = 1;
    for (const net::GameSummary& g : allGames(srv, f, 10, &n)) CHECK(g.rated);
    CHECK_EQ(n, rated);
    f.rated = 0;
    allGames(srv, f, 10, &n);
    CHECK_EQ(n, total - rated);
    f = net::GamesFilter();
    f.category = "custom";
    allGames(srv, f, 10, &n);
    CHECK_EQ(n, custom);

    // Refusals.
    Event e;
    srv.fetchMyGames(0, 0, net::GamesFilter());
    CHECK(await(srv, Kind::GamesResult, e));
    CHECK_EQ(e.error, std::string("invalid_limit"));
    f = net::GamesFilter();
    f.result = "aborted";
    srv.fetchMyGames(0, 10, f);
    CHECK(await(srv, Kind::GamesResult, e));
    CHECK_EQ(e.error, std::string("invalid_filter"));
    f = net::GamesFilter();
    f.category = "4+4";
    srv.fetchMyGames(0, 10, f);
    CHECK(await(srv, Kind::GamesResult, e));
    CHECK_EQ(e.error, std::string("invalid_filter"));

    // Another account has other games; "newbie" none.
    mock::FakeServer other;
    signIn(other, "a_newbie");
    allGames(other, net::GamesFilter(), 10, &n);
    CHECK_EQ(n, 0);
}

// The history page's filter changed before the first answer (as OnlineSession::loadHistory does):
// whatever order the fake's answers come in, the page kept is the one of the filter shown. Every
// answer names its request, failures included.
TEST(mock_account_history_filter_changed_while_loading) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    int ratedTotal = 0;
    net::GamesFilter rated;
    rated.rated = 1;
    allGames(srv, rated, 10, &ratedTotal);
    CHECK(ratedTotal > 0);
    int wrong = 0;
    for (int trial = 0; trial < 40; ++trial) {
        game::AccountData d;
        net::AccountInfo account;
        bool signedIn = true;
        srv.fetchMyGames(d.history.restart(net::GamesFilter()), game::HistoryPager::kPageSize, net::GamesFilter());
        srv.fetchMyGames(d.history.restart(rated), game::HistoryPager::kPageSize, rated);
        for (int answers = 0, guard = 0; answers < 2 && guard < 1200; ++guard) {
            Event e;
            while (srv.poll(e))
                if (e.kind == Kind::GamesResult) {
                    d.apply(e, account, signedIn);
                    ++answers;
                }
            mock::advance(50.0);
        }
        CHECK(d.history.loaded());
        bool casual = false;
        for (const net::GameSummary& g : d.history.page().games) casual = casual || !g.rated;
        if (casual || d.history.page().total != ratedTotal) ++wrong;
    }
    CHECK_EQ(wrong, 0);
    // A refusal names its request too.
    net::GamesFilter bad;
    bad.result = "aborted";
    srv.fetchMyGames(30, 10, bad);
    Event e;
    CHECK(await(srv, Kind::GamesResult, e));
    CHECK_EQ(e.error, std::string("invalid_filter"));
    CHECK_EQ(e.gamesPage.before, uint64_t(30));
    CHECK_EQ(e.gamesPage.filter.result, std::string("aborted"));
}

// Every answer names the server its command went to (net::Event::origin), as net::OnlineClient's
// do: the history asked for before another server was chosen names the first one, and the game
// (game::ServerAnswers) drops it.
TEST(mock_account_answers_name_their_server) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    const std::string first = srv.server().origin();
    game::ServerAnswers answers;
    answers.setServer(first);
    srv.fetchMyGames(0, game::HistoryPager::kPageSize, net::GamesFilter());
    answers.expect(Kind::GamesResult);
    net::ServerEndpoint ep;
    ep.host = "other.example.org";
    srv.setServer(ep);
    answers.setServer(ep.origin());
    srv.fetchMyGames(0, game::HistoryPager::kPageSize, net::GamesFilter());   // signed out there
    answers.expect(Kind::GamesResult);
    std::vector<Event> got;
    for (int guard = 0; guard < 1200 && got.size() < 2; ++guard) {
        Event e;
        while (srv.poll(e))
            if (e.kind == Kind::GamesResult) got.push_back(e);
        mock::advance(50.0);
    }
    CHECK_EQ(got.size(), size_t(2));
    int dropped = 0, kept = 0;
    for (const Event& e : got) {
        if (e.ok) {
            CHECK_EQ(e.origin, first);
            CHECK(!e.gamesPage.games.empty());
            CHECK(!answers.keep(e));
            ++dropped;
        } else {
            CHECK_EQ(e.origin, std::string("other.example.org:443"));
            CHECK_EQ(e.error, std::string("unauthorized"));
            CHECK(answers.keep(e));
            ++kept;
        }
    }
    CHECK_EQ(dropped, 1);
    CHECK_EQ(kept, 1);
    Event e;
    CHECK(answers.take(Kind::GamesResult, e));
    CHECK_EQ(e.origin, std::string("other.example.org:443"));
}

TEST(mock_account_games_are_legal_and_consistent) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    std::set<int> reasons;
    for (const net::GameSummary& s : allGames(srv, net::GamesFilter(), 50, nullptr)) {
        srv.fetchGame(s.id);
        Event e;
        CHECK(await(srv, Kind::GameDetailsResult, e));
        CHECK(e.ok);
        const net::GameDetails& d = e.gameDetails;
        CHECK_EQ(d.id, s.id);
        CHECK_EQ(int(d.moves.size()), d.plies);
        reasons.insert(d.reason);
        bool complete = false;
        const std::vector<game::MoveLine> lines = game::gameMoves(d, &complete);
        CHECK(complete);
        // The result and its reason agree with the moves.
        chess::Position pos;
        for (const net::GameDetails::Ply& p : d.moves) pos.makeMove(pos.parseUCI(p.uci));
        if (d.reason == 1) CHECK(pos.isCheckmate());
        if (d.reason == 5) CHECK(pos.isStalemate());
        if (d.reason == 22 || d.reason == 23) {
            CHECK_EQ(d.status, 4);
            CHECK(d.plies <= 1);
        }
        CHECK_EQ(d.result, std::string(d.status == 1 ? "1-0" : d.status == 2 ? "0-1" : d.status == 3 ? "1/2-1/2" : "*"));
        // Clocks: none run before each side's first move; never below zero; the flag fall at zero.
        for (int i = 0; i < d.plies; ++i) {
            const net::GameDetails::Ply& p = d.moves[size_t(i)];
            CHECK(p.clockMs >= 0 && p.spentMs >= 0);
            if (i < 2) CHECK_EQ(p.clockMs, d.baseMs);
        }
        if (d.reason == 3 && d.plies >= 4) {
            const int loser = d.plies % 2;  // the side to move
            CHECK_EQ(d.status, loser == 0 ? 2 : 1);
            CHECK(d.moves[size_t(d.plies - 2)].clockMs < 6000 + d.incMs);
        }
        // Ratings: rated games change both, in opposite directions.
        if (d.rated && d.status != 4) {
            CHECK(d.white.ratingChanged && d.black.ratingChanged);
            CHECK_EQ(d.white.ratingAfter, d.white.rating + d.white.ratingDiff);
            CHECK(d.white.ratingDiff * d.black.ratingDiff <= 0);
        } else {
            CHECK(!d.white.ratingChanged && !d.black.ratingChanged);
        }
        CHECK(d.startedAtMs < d.endedAtMs);
        CHECK_EQ(int(lines.size()), d.plies);
    }
    // Every way a game ends, mates and stalemate included.
    for (int r : {1, 2, 3, 5, 10, 12, 20, 24}) CHECK(reasons.count(r) == 1);
    CHECK(reasons.count(22) + reasons.count(23) >= 1);

    // Failures name the game asked for, as net::OnlineClient's do (the page of another game
    // opened meanwhile ignores them).
    Event e;
    srv.fetchGame(12345);
    CHECK(await(srv, Kind::GameDetailsResult, e));
    CHECK_EQ(e.error, std::string("not_found"));
    CHECK_EQ(e.gameId, uint64_t(12345));
    srv.downloadPgn(12346);
    CHECK(await(srv, Kind::PgnResult, e));
    CHECK_EQ(e.error, std::string("not_found"));
    CHECK_EQ(e.gameId, uint64_t(12346));
    srv.logout(false);
    srv.fetchGame(12347);
    CHECK(await(srv, Kind::GameDetailsResult, e));
    CHECK(!e.ok);
    CHECK_EQ(e.gameId, uint64_t(12347));
}

TEST(mock_account_pgn_reads_back_into_the_saved_games) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    int checked = 0;
    for (const net::GameSummary& s : allGames(srv, net::GamesFilter(), 50, nullptr)) {
        srv.fetchGame(s.id);
        Event details;
        CHECK(await(srv, Kind::GameDetailsResult, details));
        srv.downloadPgn(s.id);
        Event e;
        CHECK(await(srv, Kind::PgnResult, e));
        CHECK(e.ok);
        CHECK_EQ(e.gameId, s.id);
        // Lines under 80 columns.
        size_t start = 0;
        while (start < e.text.size()) {
            size_t end = e.text.find('\n', start);
            if (end == std::string::npos) end = e.text.size();
            CHECK(end - start < 80);
            start = end + 1;
        }
        game::archive::ServerGame sg;
        sg.server = "fake.example.org:443";
        sg.gameId = s.id;
        chess::pgn::Record r;
        std::string err;
        CHECK(game::archive::serverRecord(e.text, sg, r, err));
        if (!err.empty()) std::fprintf(stderr, "  game %llu: %s\n", (unsigned long long)s.id, err.c_str());
        CHECK_EQ(int(r.plies.size()), s.plies);
        CHECK_EQ(r.result, s.result);
        CHECK_EQ(r.tag("ScacelithGameId"), std::to_string(s.id));
        CHECK_EQ(r.tag("Site"), std::string("fake.example.org"));
        CHECK_EQ(r.tag("PlyCount"), std::to_string(s.plies));
        CHECK_EQ(r.tag("Termination"), std::string(s.status == 4 ? "unterminated" : s.reason == 3 ? "time forfeit" : s.reason == 20 ? "abandoned"
                                                                                  : s.reason == 24                    ? "rules infraction"
                                                                                                                       : "normal"));
        CHECK_EQ(r.findTag("WhiteRatingDiff") != nullptr, s.white.ratingChanged);
        for (size_t i = 0; i < r.plies.size() && i < details.gameDetails.moves.size(); ++i) {
            const net::GameDetails::Ply& p = details.gameDetails.moves[i];
            CHECK_EQ(r.plies[i].clockMs, p.clockMs / 100 * 100);
            CHECK_EQ(r.plies[i].elapsedMs, p.spentMs / 100 * 100);
        }
        ++checked;
    }
    CHECK(checked >= 40);
}

TEST(mock_account_game_played_goes_into_the_history) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    int before = 0;
    allGames(srv, net::GamesFilter(), 10, &before);
    srv.connect();
    Event e;
    CHECK(await(srv, Kind::Welcome, e));
    srv.joinQueue("3+2", true);
    CHECK(await(srv, Kind::GameSnapshot, e));
    const uint64_t id = e.game.id;
    CHECK(id != 0);
    srv.resign(id);
    CHECK(await(srv, Kind::GameEnd, e));
    CHECK(await(srv, Kind::RatingUpdate, e));  // or an aborted game would have none: resigning plays on
    int after = 0;
    std::vector<net::GameSummary> games = allGames(srv, net::GamesFilter(), 10, &after);
    CHECK_EQ(after, before + 1);
    CHECK(!games.empty() && games[0].id == id);
    CHECK_EQ(games[0].reason, 2);
    CHECK(games[0].rated);
    CHECK((games[0].you == 0 ? games[0].white : games[0].black).ratingChanged);
    CHECK(game::outcomeOf(games[0]) == game::Outcome::Loss);
}

TEST(mock_account_devices_and_preferences) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    srv.fetchSessions();
    Event e;
    CHECK(await(srv, Kind::SessionsResult, e));
    CHECK(e.ok);
    CHECK(e.sessions.size() >= 3);
    int current = 0;
    int64_t other = 0;
    bool unknown = false;
    for (const net::SessionInfo& s : e.sessions) {
        current += s.current;
        if (!s.current) other = s.id;
        unknown = unknown || s.clientLabel.empty();
        CHECK(s.createdAtMs <= s.lastSeenAtMs);
    }
    CHECK_EQ(current, 1);
    CHECK(unknown);
    const size_t count = e.sessions.size();
    srv.revokeSession(other);
    CHECK(await(srv, Kind::SessionRevoked, e));
    CHECK(e.ok);
    CHECK_EQ(e.sessionId, other);
    srv.revokeSession(other);
    CHECK(await(srv, Kind::SessionRevoked, e));
    CHECK_EQ(e.error, std::string("not_found"));
    srv.fetchSessions();
    CHECK(await(srv, Kind::SessionsResult, e));
    CHECK_EQ(e.sessions.size(), count - 1);

    srv.setAcceptChallenges(false);
    CHECK(await(srv, Kind::PreferencesResult, e));
    CHECK(e.ok);
    CHECK(!e.account.acceptChallenges);
    srv.fetchAccount();
    CHECK(await(srv, Kind::AccountResult, e));
    CHECK(!e.account.acceptChallenges);
    CHECK(e.account.hasPassword);
    CHECK(e.account.createdAtMs > 0 && e.account.lastLoginAtMs >= e.account.createdAtMs);
}

TEST(mock_account_email_change) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    Event e;
    srv.changeEmail("new@example.org", "wrong", "");
    CHECK(await(srv, Kind::EmailChangeResult, e));
    CHECK_EQ(e.error, std::string("invalid_password"));
    srv.changeEmail("paul_m@example.com", "pw", "");
    CHECK(await(srv, Kind::EmailChangeResult, e));
    CHECK_EQ(e.error, std::string("same_email"));
    srv.changeEmail("not-an-address", "pw", "");
    CHECK(await(srv, Kind::EmailChangeResult, e));
    CHECK_EQ(e.error, std::string("invalid_email"));
    srv.changeEmail("Paul.New@Example.org", "pw", "");
    CHECK(await(srv, Kind::EmailChangeResult, e));
    CHECK(e.ok);
    CHECK_EQ(e.status, std::string("verification_sent"));
    srv.fetchAccount();
    CHECK(await(srv, Kind::AccountResult, e));
    CHECK_EQ(e.account.pendingEmail, std::string("paul.new@example.org"));
    CHECK_EQ(e.account.email, std::string("paul_m@example.com"));
    mock::advance(41000.0);  // the link is opened
    srv.fetchAccount();
    CHECK(await(srv, Kind::AccountResult, e));
    CHECK(e.account.pendingEmail.empty());
    CHECK_EQ(e.account.email, std::string("paul.new@example.org"));

    // Two-factor: the code (or a recovery code) is needed.
    mock::FakeServer mfa;
    signIn(mfa, "mfa_tester");
    mfa.changeEmail("t@example.org", "pw", "");
    CHECK(await(mfa, Kind::EmailChangeResult, e));
    CHECK_EQ(e.error, std::string("mfa_code_required"));
    mfa.changeEmail("t@example.org", "pw", "000000");
    CHECK(await(mfa, Kind::EmailChangeResult, e));
    CHECK_EQ(e.error, std::string("invalid_code"));
    mfa.changeEmail("t@example.org", "pw", "abcd-efgh-jk");
    CHECK(await(mfa, Kind::EmailChangeResult, e));
    CHECK(e.ok);

    // A server without e-mail confirmation changes it at once.
    mock::FakeServer direct;
    net::ServerEndpoint ep;
    ep.host = "noverify.example.org";
    direct.setServer(ep);
    direct.login("Paul_M", "pw");
    CHECK(await(direct, Kind::LoginResult, e));
    direct.changeEmail("taken@example.org", "pw", "");
    CHECK(await(direct, Kind::EmailChangeResult, e));
    CHECK_EQ(e.error, std::string("email_taken"));
    direct.changeEmail("paul2@example.org", "pw", "");
    CHECK(await(direct, Kind::EmailChangeResult, e));
    CHECK_EQ(e.status, std::string("email_changed"));
}

TEST(mock_account_export_and_deletion) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    Event e;
    srv.exportAccount("wrong", "");
    CHECK(await(srv, Kind::AccountExportResult, e));
    CHECK_EQ(e.error, std::string("invalid_password"));
    srv.exportAccount("pw", "");
    CHECK(await(srv, Kind::AccountExportResult, e));
    CHECK(e.ok);
    net::json::Value doc;
    net::json::Limits limits;
    limits.maxBytes = 64u << 20;
    CHECK(net::json::parse(e.text, doc, nullptr, limits));
    CHECK_EQ(doc["format"].asString(), std::string("scacelith-account-export"));
    CHECK_EQ(doc["version"].asInt(), int64_t(1));
    CHECK_EQ(doc["account"]["username"].asString(), std::string("Paul_M"));
    CHECK(doc["games"]["total"].asInt() >= 40);
    CHECK_EQ(int64_t(doc["games"]["list"].size()), doc["games"]["total"].asInt());
    CHECK(doc["sessions"].size() >= 3);
    CHECK(doc["notes"].size() >= 1);
    CHECK(e.text.find("recoveryCodes") == std::string::npos);
    CHECK(e.text.find("passwordHash") == std::string::npos);
    // Five attempts an hour, the wrong password above included.
    for (int i = 0; i < 3; ++i) {
        srv.exportAccount("pw", "");
        CHECK(await(srv, Kind::AccountExportResult, e));
        CHECK(e.ok);
    }
    srv.exportAccount("pw", "");
    CHECK(await(srv, Kind::AccountExportResult, e));
    CHECK_EQ(e.error, std::string("rate_limited"));
    CHECK(e.retryAfterSec > 0);

    srv.deleteAccount("wrong", "");
    CHECK(await(srv, Kind::AccountDeleted, e));
    CHECK(!e.ok);
    CHECK_EQ(e.error, std::string("invalid_password"));
    srv.deleteAccount("pw", "");
    CHECK(await(srv, Kind::AccountDeleted, e));
    CHECK(e.ok);
    CHECK(!srv.hasSavedSession());
    srv.fetchAccount();
    CHECK(await(srv, Kind::AccountResult, e));
    CHECK_EQ(e.error, std::string("unauthorized"));
    srv.fetchMyGames(0, 10, net::GamesFilter());
    CHECK(await(srv, Kind::GamesResult, e));
    CHECK_EQ(e.error, std::string("unauthorized"));
}

// The export's limit as on the server (account-export.js: rate [account_export 5/h, reauth],
// checked by the router before the handler): every attempt counts, failed ones included, and the
// limit is checked before the password.
TEST(mock_account_export_limit_counts_every_attempt) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    Event e;
    for (int i = 0; i < 5; ++i) {
        srv.exportAccount("wrong", "");
        CHECK(await(srv, Kind::AccountExportResult, e));
        CHECK_EQ(e.error, std::string("invalid_password"));
    }
    srv.exportAccount("pw", "");
    CHECK(await(srv, Kind::AccountExportResult, e));
    CHECK_EQ(e.error, std::string("rate_limited"));
    CHECK(e.retryAfterSec > 3500 && e.retryAfterSec <= 3600);
    srv.exportAccount("wrong", "");  // no word about the password
    CHECK(await(srv, Kind::AccountExportResult, e));
    CHECK_EQ(e.error, std::string("rate_limited"));
    mock::advance(3600000.0);
    srv.exportAccount("pw", "");
    CHECK(await(srv, Kind::AccountExportResult, e));
    CHECK(e.ok);
}

// ---- Animated GIFs -----------------------------------------------------------------------------------

namespace {

// A GIF reader as small as it can be (what the fake writes, and what any encoder may: palettes,
// extensions, sub-blocks, LZW with its variable width and clear codes): every frame decoded to
// palette indexes, its delay, and whether the file loops.
struct DecodedGif {
    int w = 0, h = 0, colours = 0;
    bool loops = false;
    std::vector<std::vector<uint8_t>> frames;   // w x h indexes each (full frames only)
    std::vector<int> delaysCs;
    std::string error;
    uint8_t at(size_t frame, int x, int y) const { return frames[frame][size_t(y) * size_t(w) + size_t(x)]; }
};

bool lzwDecode(const std::string& data, int minCode, size_t expect, std::vector<uint8_t>& out) {
    if (minCode < 2 || minCode > 8) return false;
    const int clear = 1 << minCode, end = clear + 1;
    std::vector<std::string> table(4096);
    for (int i = 0; i < clear; ++i) table[size_t(i)] = std::string(1, char(i));
    int width = minCode + 1, next = end + 1, prev = -1;
    size_t bit = 0;
    for (;;) {
        if (bit + size_t(width) > data.size() * 8) return false;   // no end code
        int code = 0;
        for (int i = 0; i < width; ++i, ++bit) code |= ((uint8_t(data[bit / 8]) >> (bit % 8)) & 1) << i;
        if (code == clear) {
            width = minCode + 1;
            next = end + 1;
            prev = -1;
            continue;
        }
        if (code == end) break;
        std::string entry;
        if (code < clear || (code > end && code < next)) entry = table[size_t(code)];
        else if (code == next && prev >= 0) entry = table[size_t(prev)] + table[size_t(prev)][0];
        else return false;   // a code not in the table
        out.insert(out.end(), entry.begin(), entry.end());
        if (prev >= 0 && next < 4096) {
            table[size_t(next++)] = table[size_t(prev)] + entry[0];
            if (next == (1 << width) && width < 12) ++width;
        }
        prev = code;
    }
    return out.size() == expect;
}

DecodedGif decodeGif(const std::string& s) {
    DecodedGif g;
    size_t at = 0;
    auto fail = [&](const char* why) {
        g.error = why;
        return g;
    };
    auto byte = [&]() -> int { return at < s.size() ? uint8_t(s[at++]) : -1; };
    auto word = [&]() {
        const int lo = byte(), hi = byte();
        return lo < 0 || hi < 0 ? -1 : lo | hi << 8;
    };
    auto blocks = [&](std::string* into) {   // sub-blocks up to the empty one
        for (;;) {
            const int n = byte();
            if (n < 0 || at + size_t(n) > s.size()) return false;
            if (n == 0) return true;
            if (into) into->append(s, at, size_t(n));
            at += size_t(n);
        }
    };
    if (s.size() < 13 || (s.compare(0, 6, "GIF89a") != 0 && s.compare(0, 6, "GIF87a") != 0)) return fail("signature");
    at = 6;
    g.w = word();
    g.h = word();
    const int packed = byte();
    byte();   // background
    byte();   // aspect
    if (g.w <= 0 || g.h <= 0) return fail("screen size");
    if (packed & 0x80) {
        g.colours = 2 << (packed & 7);
        at += size_t(g.colours) * 3;
    }
    int delay = 0;
    for (;;) {
        const int b = byte();
        if (b == 0x3B) break;
        if (b == 0x21) {
            const int label = byte();
            std::string body;
            if (!blocks(&body)) return fail("extension");
            if (label == 0xF9 && body.size() == 4) delay = uint8_t(body[1]) | uint8_t(body[2]) << 8;
            if (label == 0xFF && body.compare(0, 11, "NETSCAPE2.0") == 0) g.loops = true;
            continue;
        }
        if (b != 0x2C) return fail("block");
        const int x = word(), y = word(), w = word(), h = word(), ipacked = byte();
        if (x != 0 || y != 0 || w != g.w || h != g.h) return fail("partial frame");
        if (ipacked & 0x40) return fail("interlaced");
        int colours = g.colours;
        if (ipacked & 0x80) {
            colours = 2 << (ipacked & 7);
            at += size_t(colours) * 3;
        }
        const int minCode = byte();
        std::string data;
        if (!blocks(&data)) return fail("image data");
        std::vector<uint8_t> px;
        if (!lzwDecode(data, minCode, size_t(w) * size_t(h), px)) return fail("lzw");
        for (uint8_t c : px)
            if (int(c) >= colours) return fail("colour out of the palette");
        g.frames.push_back(std::move(px));
        g.delaysCs.push_back(delay);
        delay = 0;
    }
    if (at != s.size()) return fail("bytes after the trailer");
    return g;
}

// Palette indexes of the fake's pictures (online_mock.cpp).
enum : uint8_t { kLight = 0, kDark = 1, kLightMove = 2, kWhiteFill = 4, kBlackFill = 6, kFrame = 8, kCheck = 9 };

Event gifOf(mock::FakeServer& srv, uint64_t gameId, const net::GifOptions& o) {
    srv.downloadGameGif(gameId, o);
    Event e;
    CHECK(await(srv, Kind::GifResult, e));
    return e;
}
Event gifOfPgn(mock::FakeServer& srv, const std::string& pgn, const net::GifOptions& o) {
    srv.renderPgnGif(pgn, o);
    Event e;
    CHECK(await(srv, Kind::GifResult, e));
    return e;
}

const char kScholar[] = "[Event \"Casual\"]\n[White \"Paul_M\"]\n[Black \"bob\"]\n[Result \"1-0\"]\n\n"
                        "1. e4 e5 2. Qh5 Nc6 3. Bc4 Nf6 4. Qxf7# 1-0\n";

// Knights out and back: 'plies' legal moves.
std::string shuffles(int plies) {
    std::string pgn = "[Event \"Long\"]\n[White \"a\"]\n[Black \"b\"]\n[Result \"*\"]\n\n";
    for (int i = 0; i < plies; ++i) {
        if (i % 2 == 0) pgn += std::to_string(i / 2 + 1) + ". ";
        static const char* const cycle[4] = {"Nf3", "Nf6", "Ng1", "Ng8"};
        pgn += cycle[i % 4];
        pgn += i % 8 == 7 ? "\n" : " ";
    }
    return pgn + " *\n";
}

}  // namespace

TEST(mock_account_gif_decodes) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    srv.fetchMyGames(0, 10, net::GamesFilter());
    Event page;
    CHECK(await(srv, Kind::GamesResult, page));
    CHECK(page.ok && !page.gamesPage.games.empty());
    // A minute between the renders of PGN texts: the account's quota is four a minute.
    auto render = [&](const std::string& pgn, const net::GifOptions& o) {
        mock::advance(61000.0);
        return gifOfPgn(srv, pgn, o);
    };
    int checked = 0;
    for (const net::GameSummary& s : page.gamesPage.games) {
        if (checked == 3) break;
        Event e = gifOf(srv, s.id, net::GifOptions());
        CHECK(e.ok);
        CHECK_EQ(e.gameId, s.id);
        CHECK(e.text.size() < net::OnlineClient::kGifMaxBytes);
        DecodedGif g = decodeGif(e.text);
        if (!g.error.empty()) std::fprintf(stderr, "  game %llu: %s\n", (unsigned long long)s.id, g.error.c_str());
        CHECK(g.error.empty());
        CHECK_EQ(g.w, 8 * 12 + 12);   // medium: 12 pixels a square, a frame of half a square
        CHECK_EQ(g.h, g.w);
        CHECK_EQ(g.colours, 16);
        CHECK(g.loops);
        CHECK_EQ(int(g.frames.size()), s.plies + 1);   // the start, then a frame per move
        if (g.frames.size() >= 3) {
            CHECK_EQ(g.delaysCs.front(), 100);
            CHECK_EQ(g.delaysCs[1], 50);
            CHECK_EQ(g.delaysCs.back(), 300);
        }
        CHECK_EQ(int(g.at(0, 0, 0)), int(kFrame));
        ++checked;
    }
    CHECK_EQ(checked, 3);

    // A PGN text: the pieces, the move, the check, from either side; the sizes and the delay.
    Event e = render(kScholar, net::GifOptions());
    CHECK(e.ok);
    CHECK_EQ(e.gameId, uint64_t(0));
    DecodedGif w = decodeGif(e.text);
    CHECK(w.error.empty());
    CHECK_EQ(int(w.frames.size()), 8);
    if (w.frames.size() == 8) {
        const int b = 6, sq = 12;   // border, square
        CHECK_EQ(int(w.at(0, b + 6, b + 7 * sq + 8)), int(kWhiteFill));     // a1: the white rook at the bottom left
        CHECK_EQ(int(w.at(0, b + 4 * sq, b + 6 * sq)), int(kLight));        // e2 before 1. e4
        CHECK_EQ(int(w.at(1, b + 4 * sq, b + 6 * sq)), int(kLightMove));    // e2 and e4 after it
        CHECK_EQ(int(w.at(1, b + 4 * sq, b + 4 * sq)), int(kLightMove));
        CHECK_EQ(int(w.at(7, b + 4 * sq, b + 0 * sq)), int(kCheck));        // the king mated on e8
        CHECK_EQ(int(w.at(6, b + 4 * sq, b + 0 * sq)), int(kLight));        // not in check before
    }
    net::GifOptions o;
    o.orientation = "black";
    e = render(kScholar, o);
    CHECK(e.ok);
    DecodedGif bl = decodeGif(e.text);
    CHECK(bl.error.empty());
    if (bl.frames.size() == 8) {
        const int b = 6, sq = 12;
        CHECK_EQ(int(bl.at(0, b + 6, b + 7 * sq + 8)), int(kBlackFill));    // h8: the black rook at the bottom left
        CHECK_EQ(int(bl.at(1, b + 3 * sq, b + 1 * sq)), int(kLightMove));   // e2 seen from Black
        CHECK_EQ(int(bl.at(7, b + 3 * sq, b + 7 * sq)), int(kCheck));       // e8 at the bottom
    }
    o = net::GifOptions();
    o.size = "small";
    o.coords = false;
    o.delayMs = 1000;
    e = render(kScholar, o);
    DecodedGif sm = decodeGif(e.text);
    CHECK(sm.error.empty());
    CHECK_EQ(sm.w, 64);
    CHECK_EQ(int(sm.at(0, 0, 0)), int(kLight));   // a8, no frame
    CHECK_EQ(sm.delaysCs.size(), size_t(8));
    if (sm.delaysCs.size() == 8) CHECK_EQ(sm.delaysCs[3], 100);
    o.size = "large";
    o.coords = true;
    e = render(kScholar, o);
    DecodedGif lg = decodeGif(e.text);
    CHECK(lg.error.empty());
    CHECK_EQ(lg.w, 8 * 16 + 16);

    // 600 moves at most (from a position too).
    e = render(shuffles(600), net::GifOptions());
    CHECK(e.ok);
    DecodedGif longest = decodeGif(e.text);
    CHECK(longest.error.empty());
    CHECK_EQ(int(longest.frames.size()), 601);
    e = render(shuffles(601), net::GifOptions());
    CHECK_EQ(e.error, std::string("game_too_long"));
    const std::string fromFen = "[Event \"Ending\"]\n[White \"a\"]\n[Black \"b\"]\n[Result \"*\"]\n"
                                "[SetUp \"1\"]\n[FEN \"4k3/8/8/8/8/8/4P3/4K3 w - - 0 1\"]\n\n1. e4 Kd7 *\n";
    e = render(fromFen, net::GifOptions());
    CHECK(e.ok);
    DecodedGif fen = decodeGif(e.text);
    CHECK(fen.error.empty());
    CHECK_EQ(int(fen.frames.size()), 3);
    if (fen.frames.size() == 3) CHECK_EQ(int(fen.at(0, 6 + 6, 6 + 7 * 12 + 8)), int(kDark));   // a1 empty: dark
}

TEST(mock_account_gif_quota_and_special_inputs) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    srv.fetchMyGames(0, 10, net::GamesFilter());
    Event page;
    CHECK(await(srv, Kind::GamesResult, page));
    CHECK(!page.gamesPage.games.empty());
    if (page.gamesPage.games.empty()) return;
    const uint64_t id = page.gamesPage.games[0].id;
    net::GifOptions o;

    // Special games, and what is refused before any render.
    Event e = gifOf(srv, 429, o);
    CHECK(!e.ok);
    CHECK_EQ(e.error, std::string("rate_limited"));
    CHECK_EQ(e.retryAfterSec, 150);
    CHECK_EQ(e.gameId, uint64_t(429));
    e = gifOf(srv, 503, o);
    CHECK_EQ(e.error, std::string("server_busy"));
    CHECK_EQ(e.retryAfterSec, 8);
    CHECK_EQ(e.gameId, uint64_t(503));
    CHECK_EQ(gifOf(srv, 0, o).error, std::string("invalid_game_id"));
    CHECK_EQ(gifOf(srv, 987654321, o).error, std::string("not_found"));
    for (int bad = 0; bad < 4; ++bad) {
        net::GifOptions b;
        if (bad == 0) b.size = "huge";
        if (bad == 1) b.orientation = "left";
        if (bad == 2) b.delayMs = 50;
        if (bad == 3) b.delayMs = 3001;
        CHECK_EQ(gifOf(srv, id, b).error, std::string("invalid_option"));
    }
    std::string ratelimited = kScholar;
    ratelimited.replace(ratelimited.find("Paul_M"), 6, "RateLimited");
    e = gifOfPgn(srv, ratelimited, o);
    CHECK_EQ(e.error, std::string("rate_limited"));
    CHECK_EQ(e.retryAfterSec, 150);
    std::string busy = kScholar;
    busy.replace(busy.find("bob"), 3, "serverbusy");
    e = gifOfPgn(srv, busy, o);
    CHECK_EQ(e.error, std::string("server_busy"));
    CHECK_EQ(e.retryAfterSec, 8);
    CHECK_EQ(gifOfPgn(srv, "[Event \"x\"]\n\n1. e5 *\n", o).error, std::string("invalid_pgn"));
    CHECK_EQ(gifOfPgn(srv, std::string(net::OnlineClient::kGifMaxPgnBytes + 1, ' '), o).error, std::string("pgn_too_large"));

    // Four renders a minute; a GIF made before costs nothing.
    for (int d : {500, 600, 700, 800}) {
        o.delayMs = d;
        CHECK(gifOf(srv, id, o).ok);
    }
    o.delayMs = 600;
    CHECK(gifOf(srv, id, o).ok);
    o.delayMs = 900;
    e = gifOf(srv, id, o);
    CHECK_EQ(e.error, std::string("rate_limited"));
    CHECK(e.retryAfterSec >= 45 && e.retryAfterSec <= 60);
    CHECK_EQ(e.gameId, id);
    mock::advance(e.retryAfterSec * 1000.0);
    CHECK(gifOf(srv, id, o).ok);

    // Thirty an hour: then the wait is the hour's.
    int renders = 5, delay = 1000;
    for (int guard = 0; renders < 30 && guard < 200; ++guard) {
        o.delayMs = delay;
        e = gifOf(srv, id, o);
        if (e.ok) {
            ++renders;
            delay += 10;
            continue;
        }
        CHECK_EQ(e.error, std::string("rate_limited"));
        CHECK(e.retryAfterSec >= 1 && e.retryAfterSec <= 60);
        mock::advance(e.retryAfterSec * 1000.0);
    }
    CHECK_EQ(renders, 30);
    mock::advance(61000.0);   // the minute's window is free
    o.delayMs = delay;
    e = gifOf(srv, id, o);
    CHECK_EQ(e.error, std::string("rate_limited"));
    CHECK(e.retryAfterSec > 60 && e.retryAfterSec <= 3600);
    o.delayMs = 500;
    CHECK(gifOf(srv, id, o).ok);   // made before: still served
    mock::advance(e.retryAfterSec * 1000.0);
    o.delayMs = delay;
    CHECK(gifOf(srv, id, o).ok);
}

// Signed out, offline, or on a server without GIFs: the answers net::OnlineClient would give. A
// game's answer always names it (the GifSaver keeps only the one it awaits), and game 0 is refused
// before anything else, as the client refuses it without sending anything.
TEST(mock_account_gif_signed_out_and_disabled) {
    VirtualClock vc;
    {
        mock::FakeServer srv;
        net::ServerEndpoint ep;
        ep.host = "fake.example.org";
        srv.setServer(ep);
        Event e = gifOf(srv, 812, net::GifOptions());
        CHECK_EQ(e.error, std::string("unauthorized"));   // no session saved: the one code, signed out
        CHECK(!e.sessionLost);
        CHECK_EQ(e.gameId, uint64_t(812));
        CHECK_EQ(gifOfPgn(srv, kScholar, net::GifOptions()).error, std::string("unauthorized"));
        CHECK_EQ(gifOf(srv, 0, net::GifOptions()).error, std::string("invalid_game_id"));
    }
    {
        mock::FakeServer srv;
        net::ServerEndpoint ep;
        ep.host = "offline.example.org";
        srv.setServer(ep);
        Event e = gifOf(srv, 812, net::GifOptions());
        CHECK_EQ(e.error, std::string("network"));
        CHECK_EQ(e.gameId, uint64_t(812));
        CHECK_EQ(gifOfPgn(srv, kScholar, net::GifOptions()).error, std::string("network"));
    }
    {
        mock::FakeServer srv;
        net::ServerEndpoint ep;
        ep.host = "nogif.example.org";
        srv.setServer(ep);
        srv.login("Paul_M", "correct horse battery");
        Event e;
        CHECK(await(srv, Kind::LoginResult, e));
        CHECK(e.ok);
        CHECK_EQ(gifOf(srv, 812, net::GifOptions()).error, std::string("gif_disabled"));
        CHECK_EQ(gifOfPgn(srv, kScholar, net::GifOptions()).error, std::string("gif_disabled"));
    }
}

// The route of an answer as game::OnlineSession takes it (AccountData::apply, then the GifSaver):
// the file written, its name from the game, decoded back.
TEST(mock_account_gif_saved_by_the_saver) {
    VirtualClock vc;
    mock::FakeServer srv;
    signIn(srv, "Paul_M");
    srv.fetchMyGames(0, 10, net::GamesFilter());
    Event page;
    CHECK(await(srv, Kind::GamesResult, page));
    if (page.gamesPage.games.empty()) return;
    const net::GameSummary& s = page.gamesPage.games[0];
    const std::string folder = net::sys::exeDirectory() + "mock-gif-test-" + std::to_string(s.id);
    const std::string name = game::gifFileName(std::time_t(s.startedAtMs / 1000), s.white.name, s.black.name, s.id);
    game::GifSaver saver;
    game::AccountData data;
    net::AccountInfo account;
    bool signedIn = true;
    CHECK(saver.begin("history:" + std::to_string(s.id), s.id, folder, name));
    srv.downloadGameGif(s.id, net::GifOptions());
    Event e;
    CHECK(await(srv, Kind::GifResult, e));
    CHECK(data.apply(e, account, signedIn));
    CHECK(saver.finish(e));
    CHECK(saver.poll(true));
    CHECK(saver.stage() == game::GifSaver::Stage::Saved);
    CHECK_EQ(saver.path(), game::archive::joinPath(folder, name));
    std::string bytes;
    CHECK(net::sys::readFile(saver.path(), bytes, net::OnlineClient::kGifMaxBytes));
    CHECK_EQ(bytes, e.text);
    CHECK(decodeGif(bytes).error.empty());
    CHECK(name.find("_" + std::to_string(s.id) + ".gif") != std::string::npos);
    // The quota's answer for the next one: kept with its wait.
    CHECK(saver.begin("history:429", 429, folder, name));
    srv.downloadGameGif(429, net::GifOptions());
    CHECK(await(srv, Kind::GifResult, e));
    CHECK(data.apply(e, account, signedIn));
    CHECK(signedIn);
    CHECK(saver.finish(e));
    CHECK_EQ(saver.error(), std::string("rate_limited"));
    CHECK_EQ(saver.retryAfterSec(), 150);
    net::sys::removeFile(game::archive::joinPath(folder, name));
#ifdef _WIN32
    RemoveDirectoryA(folder.c_str());
#else
    rmdir(folder.c_str());
#endif
}
