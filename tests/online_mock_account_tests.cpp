// The account API of the in-process fake server (src/game/online_mock.h), which the account pages
// and their screenshots use: the generated game history (every game legal, endings that match the
// moves, clocks that follow the time control, the ratings of rated games), its pages and filters
// as the server serves them (GET /account/games), the details and the PGN of each game (read back
// through the saved games' importer), a game played against the fake added to the history, the
// signed-in devices, the preference, the e-mail change, the data export and the deletion, with the
// special inputs of the re-authentication. On the fakes' virtual clock (deterministic).
#include "test.h"
#include "chess/chess.h"
#include "chess/pgn.h"
#include "game/game_archive.h"
#include "game/online_account.h"
#include "game/online_mock.h"
#include "net/json.h"

#include <set>
#include <string>
#include <vector>

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
