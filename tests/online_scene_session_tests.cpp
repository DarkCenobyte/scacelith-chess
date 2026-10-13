// game::OnlineSession (src/game/online_session.cpp) on its own, without the game: a session of its
// own (not the game's) set up with the in-process fakes on their virtual clock, driven by its
// commands and update() as the game scene and the pages drive it. The quick start of
// --start-online (signed in and queued until the game comes, then its events and the game left),
// the refusals of challenges (oneself, a rated custom time control, a player away) and of a private
// game's code, a challenge cancelled, matchmaking cancelled, a challenge received and declined, the
// GIFs (of a game of the history, of a PGN text; the server's quota and its busy renderer), the
// direct match's host and guest states and their errors; and the texts of the errors (in the UI's
// language, from the language files). The fakes' clock switches are set back at the end of each
// test.
#include "test.h"
#include "headless.h"
#include "game/online_account.h"
#include "game/online_mock.h"
#include "game/online_session.h"
#include "i18n/i18n.h"
#include "net/protocol_gen.h"

#include <filesystem>
#include <string>
#include <vector>

namespace {

using Kind = net::Event::Kind;

// A session of the fakes on the virtual clock, set up as the game scene sets the game's up
// (--online-mock --warp); the fakes' switches back to their defaults when it goes.
struct MockSession {
    game::OnlineSession s;
    MockSession() { s.init(true, true); }
    ~MockSession() {
        net::mock::useVirtualClock(false);
        net::mock::useManualClock(false);
        net::mock::forceOpponentStance(-1);
    }
    MockSession(const MockSession&) = delete;
    MockSession& operator=(const MockSession&) = delete;

    // The fakes' time moves by 25 ms steps until 'done' (at most 'ms' of it); true once done.
    template <class F>
    bool runUntil(F done, double ms) {
        for (double t = 0.0; t <= ms; t += 25.0) {
            if (done()) return true;
            s.update(0.025f);
        }
        return done();
    }
    void run(double ms) {
        for (double t = 0.0; t < ms; t += 25.0) s.update(0.025f);
    }
    // The GIF being made is done (saved or refused): the server's answer within 'ms' of the fakes'
    // time, its file written (runMock waits for the writing, done off the calling thread).
    bool gifEnded(double ms) {
        return runUntil([&] {
            s.runMock(0.0);
            return !s.gif().busy();
        }, ms);
    }
    // Signs in as 'name' (any password) and waits for the realtime connection.
    bool signIn(const std::string& name) {
        s.api().login(name, "correct horse battery");
        s.expect(Kind::LoginResult);
        return runUntil([&] { return s.signedIn() && s.conn() == net::ConnState::Online; }, 5000.0);
    }
};

bool translated(const std::string& text, const std::string& key) { return !text.empty() && text == i18n::tr(key); }

}  // namespace

// The texts of the errors: each known code has its own sentence of the language file, the others
// a sentence that names them; the GIF's waits are said with their duration.
TEST(online_scene_session_error_texts) {
    // The server's error codes (net::proto::ErrorCode).
    CHECK(translated(game::serverErrorText(3), "online.err.unauthorized"));
    CHECK(translated(game::serverErrorText(101), "online.err.move_refused"));
    CHECK(translated(game::serverErrorText(104), "online.err.move_refused"));
    CHECK(translated(game::serverErrorText(109), "notify.no_draw_claim"));
    CHECK(translated(game::serverErrorText(207), "online.err.cooldown"));
    CHECK(translated(game::serverErrorText(242), "online.err.cheat_detected"));
    const std::string unknown = game::serverErrorText(999);
    CHECK(unknown.find("999") != std::string::npos);
    CHECK(unknown != i18n::tr("online.err.code"));
    // An event's error: its code, or its HTTPS error word when it has no code.
    net::Event e;
    e.code = 205;
    CHECK(translated(game::eventErrorText(e), "online.err.code_invalid"));
    e.code = 0;
    e.error = "network";
    CHECK_EQ(game::eventErrorText(e), game::onlineErrorText("network"));
    // The direct match's errors.
    for (const char* code : {"refused", "timeout", "wrong_code", "incompatible", "port_in_use", "network", "too_many_attempts",
                             "invalid_code", "bad_address", "not_found", "unreachable", "reset", "closed", "host_left"})
        CHECK(translated(game::directErrorText(code), std::string("direct.err.") + code));
    const std::string other = game::directErrorText("cosmic_ray");
    CHECK(other.find("cosmic_ray") != std::string::npos);
    // The GIF's: the waits said, or not when the server gave none.
    CHECK(translated(game::gifErrorText("rate_limited"), "gif.err.rate_limited"));
    CHECK(translated(game::gifErrorText("server_busy"), "gif.err.server_busy"));
    const std::string later = game::gifErrorText("rate_limited", 150);
    CHECK(later.find(game::waitText(150)) != std::string::npos);
    CHECK(game::gifErrorText("server_busy", 8).find(game::waitText(8)) != std::string::npos);
    CHECK(translated(game::gifErrorText("unauthorized"), "gif.err.signed_out"));
    for (const char* code : {"game_too_long", "pgn_too_large", "invalid_pgn", "render_failed", "gif_disabled", "write_failed", "not_found"})
        CHECK(translated(game::gifErrorText(code), std::string("gif.err.") + code));
    CHECK_EQ(game::gifErrorText("network"), game::onlineErrorText("network"));
    // The page shown in the browser at the end of a Google sign-in, in the UI's language.
    const net::SsoBrowserPage page = game::ssoBrowserPage();
    CHECK_EQ(page.lang, i18n::language());
    CHECK(translated(page.title, "online.sso.page.title"));
    CHECK(translated(page.done, "online.sso.page.done"));
    CHECK(translated(page.cancelledHeading, "online.sso.page.cancelled_heading"));
    CHECK(translated(page.foreign, "online.sso.page.foreign"));
}

// --start-online's quick start: signed in with the fakes, queued in the category, run until the
// game comes. The scene takes it (its snapshot, the link), its events follow (the opponent's
// gestures, its first move with White), then it is aborted and left.
TEST(online_scene_session_quick_start) {
    MockSession m;
    game::OnlineSession& s = m.s;
    CHECK(s.mock());
    s.quickStart("3+2", "Quick");
    CHECK(s.signedIn());
    REQUIRE(s.gameReady());
    CHECK(!s.queue().searching);
    net::OnlineGame snap;
    game::GameLink* link = s.takeGame(snap);
    REQUIRE(link);
    CHECK(!s.gameReady());
    CHECK(s.link() == link);
    CHECK(link->kind() == game::LinkKind::Server);
    CHECK_EQ(link->gameId(), snap.id);
    CHECK_EQ(snap.category, std::string("3+2"));
    CHECK(link->canReport());
    CHECK(!link->reconnecting());
    CHECK(link->serverNowMs() > 0.0);
    // Its events, up to the opponent's first move when it has White.
    int events = 0;
    bool moved = false;
    m.runUntil([&] {
        net::Event e;
        while (s.nextGameEvent(e)) {
            ++events;
            if (e.kind == Kind::MoveMade && !e.mine) moved = true;
        }
        return moved || events > 40;
    }, 6000.0);
    CHECK(events > 0);
    if (snap.you == 1) CHECK(moved);   // we have Black: they moved first
    link->abortGame();
    bool ended = false;
    m.runUntil([&] {
        net::Event e;
        while (s.nextGameEvent(e))
            if (e.kind == Kind::GameEnd) ended = e.game.status == int(net::proto::GameStatus::Aborted);
        return ended;
    }, 3000.0);
    CHECK(ended);
    s.leaveGame();
    CHECK(s.link() == nullptr);
    net::Event e;
    CHECK(!s.nextGameEvent(e));
}

// Challenges refused by the server (oneself, a rated custom time control, a player who is away), a
// private game's code that is not one, a challenge cancelled before its answer, matchmaking
// cancelled; then a challenge received (the fakes' after 25 s in the menus), declined.
TEST(online_scene_session_challenges_refused_cancelled_declined) {
    MockSession m;
    game::OnlineSession& s = m.s;
    REQUIRE(m.signIn("Asker"));
    CHECK_EQ(s.account().username, std::string("Asker"));

    s.challenge("asker", 300, 3, false, 0);           // oneself (the name in other letters)
    CHECK(s.outgoing().active);
    CHECK(m.runUntil([&] { return !s.outgoing().active; }, 2000.0));
    s.challenge("Rival", 20, 0, true, 0);             // rated needs an official time control
    CHECK(m.runUntil([&] { return !s.outgoing().active; }, 2000.0));
    s.challenge("away_one", 300, 3, false, 1);        // left at once
    CHECK(m.runUntil([&] { return !s.outgoing().active; }, 2000.0));

    // A challenge cancelled before its answer: no game.
    s.challenge("Friend", 300, 3, false, 1);
    m.run(500.0);
    REQUIRE(s.outgoing().active);
    CHECK_EQ(s.outgoing().target, std::string("Friend"));
    CHECK(s.outgoing().id != 0u);                     // named by the server
    s.cancelOutgoing();
    CHECK(!s.outgoing().active);
    m.run(4000.0);
    CHECK(!s.gameReady());

    // A private game's code that is not one.
    s.joinPrivateGame("12");
    CHECK(s.joining());
    CHECK(m.runUntil([&] { return !s.joining(); }, 2000.0));
    CHECK(!s.gameReady());

    // Matchmaking, cancelled.
    s.findOpponent("5+3", true);
    CHECK(m.runUntil([&] { return s.queue().searching; }, 2000.0));
    CHECK_EQ(s.queue().category, std::string("5+3"));
    s.cancelSearch();
    m.run(3000.0);
    CHECK(!s.queue().searching);
    CHECK(!s.gameReady());

    // A challenge received while in the menus, declined.
    REQUIRE(m.runUntil([&] { return !s.incoming().empty(); }, 40000.0));
    const game::OnlineSession::Incoming c = s.incoming()[0];
    CHECK_EQ(c.from.name, std::string("Eleonora_V"));
    CHECK(c.expiresMs > s.nowMs());
    s.answerChallenge(c.id, false);
    CHECK(s.incoming().empty());
    m.run(3000.0);
    CHECK(!s.gameReady());
}

// GIFs: of a game of the account's history (written to the folder given, whatever page shows),
// and of a game as PGN text; refused for a game the server's quota or its renderer turns away
// (their waits kept), and while another one is being made.
TEST(online_scene_session_gifs) {
    headless::TempDir dir("online-gifs");
    MockSession m;
    game::OnlineSession& s = m.s;
    REQUIRE(m.signIn("Painter"));
    s.loadHistory(net::GamesFilter());
    REQUIRE(m.runUntil([&] { return s.accountData().history.loaded(); }, 3000.0));
    REQUIRE(!s.accountData().history.page().games.empty());
    const uint64_t id = s.accountData().history.page().games[0].id;

    net::GifOptions opt;
    opt.orientation = "black";
    REQUIRE(s.saveGameGif("test:game", id, opt, dir.path, "game.gif"));
    CHECK(s.gif().busy());
    CHECK_EQ(s.gif().owner(), std::string("test:game"));
    // One at a time.
    CHECK(!s.saveGameGif("test:other", id, opt, dir.path, "other.gif"));
    REQUIRE(m.gifEnded(30000.0));
    REQUIRE(s.gif().stage() == game::GifSaver::Stage::Saved);
    CHECK(std::filesystem::exists(std::filesystem::u8path(s.gif().path())));
    CHECK(s.gif().path().find("game") != std::string::npos);

    // The server's quota used up (game 429), its renderer full (game 503): their waits.
    REQUIRE(s.saveGameGif("test:quota", 429, opt, dir.path, "quota.gif"));
    REQUIRE(m.gifEnded(5000.0));
    CHECK(s.gif().stage() == game::GifSaver::Stage::Failed);
    CHECK_EQ(s.gif().error(), std::string("rate_limited"));
    CHECK_EQ(s.gif().retryAfterSec(), 150);
    REQUIRE(s.saveGameGif("test:busy", 503, opt, dir.path, "busy.gif"));
    s.gifShown("test:busy");                           // a page shows it: no notification
    REQUIRE(m.gifEnded(5000.0));
    CHECK_EQ(s.gif().error(), std::string("server_busy"));
    CHECK_EQ(s.gif().retryAfterSec(), 8);
    s.clearGif();
    CHECK(s.gif().stage() == game::GifSaver::Stage::Idle);

    // A game as PGN text (the saved games' GIF).
    const std::string pgn = "[Event \"Test\"]\n[White \"Anna\"]\n[Black \"Boris\"]\n[Result \"1-0\"]\n\n"
                            "1. f3 e5 2. g4 Qh4# 1-0\n";
    REQUIRE(s.savePgnGif("test:pgn", pgn, net::GifOptions(), dir.path, "pgn.gif"));
    REQUIRE(m.gifEnded(30000.0));
    CHECK(s.gif().stage() == game::GifSaver::Stage::Saved);
    CHECK(std::filesystem::exists(std::filesystem::u8path(s.gif().path())));
    const std::string busyPgn = "[White \"serverbusy\"]\n[Black \"Boris\"]\n[Result \"*\"]\n\n1. e4 *\n";
    REQUIRE(s.savePgnGif("test:pgn-busy", busyPgn, net::GifOptions(), dir.path, "pgn-busy.gif"));
    REQUIRE(m.gifEnded(5000.0));
    CHECK_EQ(s.gif().error(), std::string("server_busy"));
}

// The direct match through the session: hosting (the port opened by the router; none found on
// 47199; carrier-grade NAT on 47198, no public address then), closed; joining hosts that refuse,
// do not answer, are not found, run another version, or take another code: each its error.
TEST(online_scene_session_direct_match_states) {
    MockSession m;
    game::OnlineSession& s = m.s;
    using DS = net::DirectMatch::State;
    using US = net::UpnpStatus::State;
    struct Host { uint16_t port; US upnp; bool publicAddress; };
    for (const Host& h : {Host{47123, US::Mapped, true}, Host{47199, US::NoGateway, false}, Host{47198, US::Mapped, false}}) {
        net::DirectHostOptions opt;
        opt.port = h.port;
        opt.upnp = true;
        opt.playerName = "Hostess";
        s.hostDirect(opt);
        CHECK(s.directActive());
        CHECK(s.direct().isHost());
        CHECK(s.direct().state() == DS::OpeningPort);
        REQUIRE(m.runUntil([&] { return s.direct().state() == DS::WaitingForGuest; }, 3000.0));
        CHECK(s.direct().upnp().state == h.upnp);
        CHECK_EQ(int(s.direct().invite().port), int(h.port));
        CHECK_EQ(!s.direct().invite().publicAddress.empty(), h.publicAddress);
        CHECK(!s.direct().invite().lanAddresses.empty());
        if (h.port == 47198) CHECK(s.direct().upnp().cgnatSuspected);
        s.closeDirect();
        CHECK(s.direct().state() == DS::Idle);
        CHECK(!s.gameReady());
    }

    struct Join { const char* address; const char* code; const char* error; };
    const Join joins[] = {
        {"refused.example", "ABCD-EFGH-JKMN", "refused"},  {"timeout.example", "ABCD-EFGH-JKMN", "timeout"},
        {"unknown.example", "ABCD-EFGH-JKMN", "not_found"}, {"old.example", "ABCD-EFGH-JKMN", "incompatible"},
        {"192.168.1.23", "2222-EFGH-JKMN", "wrong_code"},   {"192.168.1.23", "ABCD-EFG", "invalid_code"},
    };
    for (const Join& j : joins) {
        s.joinDirect(j.address, 47100, j.code);
        CHECK(!s.direct().isHost());
        REQUIRE(m.runUntil([&] { return s.direct().state() == DS::Failed; }, 3000.0));
        CHECK_EQ(s.direct().lastError(), std::string(j.error));
        CHECK(translated(game::directErrorText(s.direct().lastError()), std::string("direct.err.") + j.error));
        s.closeDirect();
    }
    // A host that answers: the game comes, from Camille.
    s.joinDirect("192.168.1.23", 47100, "ABCD-EFGH-JKMN");
    REQUIRE(m.runUntil([&] { return s.gameReady(); }, 5000.0));
    net::OnlineGame snap;
    game::GameLink* link = s.takeGame(snap);
    REQUIRE(link);
    CHECK(link->kind() == game::LinkKind::Direct);
    CHECK(!link->canReport());
    CHECK(snap.white.name == "Camille" || snap.black.name == "Camille");
    CHECK_EQ(link->eventName(), i18n::tr("direct.event"));
    s.leaveGame();                                     // closes the direct match
    CHECK(s.link() == nullptr);
    CHECK(s.direct().state() == DS::Idle);
}
