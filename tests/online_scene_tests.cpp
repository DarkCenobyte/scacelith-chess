// Online games at the table (src/game/game_scene_online.cpp), played through the menus of the
// headless game against the in-process fakes (--online-mock, online_ui_app.h): matchmaking (the
// searching card, its Cancel), a challenge with the colour chosen, a private game, a challenge
// received in the menus, a direct match hosted and joined; in the game, the human's move sent
// (--play e2e4 with White), the opponent's moves, the Esc menu of an online game (abort before the
// first move, resign after it, offer a draw, report the opponent, leave), the opponent's absence and
// our own lost connection (the fakes' developer keys F9 and F10, F11 its stance), and the game over
// card (the rematch, the report). What the server recorded is read back from the account's game
// history; what the pages show, from their buttons.
#include "online_ui_app.h"
#include "game/online_mock.h"
#include "net/protocol_gen.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace onlinetest;

namespace {

game::OnlineSession& ses() { return game::onlineSession(); }

// The choice rows of the Play page (Challenge a player, Private game, Direct match), from the top.
std::vector<ui::im::TracedItem> playRows(headless::App& app) { return bare(app, 96.0f); }

// Waits for the game the session announces to be at the table (its link taken by the scene).
bool gameStarts(headless::App& app, int frames = 600) { return app.runUntil([&] { return ses().link() != nullptr; }, frames); }

// The game over card of an online game is up (its Main menu button).
bool gameOverCard(headless::App& app, int frames = 900) { return app.waitForLabel("Main menu", frames); }

// From the Play page: challenges 'who' with 'colour' as the colour asked for (0 random, 1 White,
// 2 Black, the page's selector starts from the setting); true once its game is at the table.
bool challenge(headless::App& app, const std::string& who, int colour) {
    game::settings().onlineColor = colour;
    std::vector<ui::im::TracedItem> rows = playRows(app);
    if (rows.size() != 3) return false;
    app.clickItem(rows[0]);
    if (!onPage(app, "Send challenge", 30)) return false;
    if (!fill(app, "User name", who) || !app.clickLabel("Send challenge")) return false;
    return gameStarts(app);
}

// Leaves the game being played from its Esc menu ("Leave the game", then the confirmation, which
// aborts before the first move and resigns after it); true once back on the Play page.
bool leaveGame(headless::App& app) {
    if (!pause(app) || !app.clickLabel("Leave the game")) return false;
    if (!app.clickLabel("Abort and leave") && !app.clickLabel("Resign and leave")) return false;
    return app.runUntil([&] { return ses().link() == nullptr && app.hasLabel("Find opponent"); }, 600);
}

}  // namespace

// Find opponent: the searching card (its Cancel ends the search), then the fake pairs us after two
// seconds and the game comes to the table. Before the first move the Esc menu offers to abort, not
// to resign; aborting ends the game on the card, unrated, and Main menu goes back to the Play page.
// The server's history has the aborted game.
TEST(online_scene_matchmaking_search_then_abort) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    REQUIRE(signIn(*app, "Matchmaker"));
    game::OnlineSession& s = ses();
    const net::RatingInfo* before = s.rating("5+3");
    REQUIRE(before);
    const int rating = before->rating;

    REQUIRE(app->clickLabel("Find opponent"));
    CHECK(s.queue().searching);
    CHECK_EQ(s.queue().category, std::string("5+3"));
    CHECK(s.queue().rated);
    CHECK(app->hasLabel("Cancel"));
    REQUIRE(app->clickLabel("Cancel"));
    CHECK(!s.queue().searching);
    app->frames(150);
    CHECK(s.link() == nullptr);              // no game after a search given up

    // Casual this time (the page's Game selector), to the end.
    game::settings().onlineRated = false;
    REQUIRE(app->clickLabel("Find opponent"));
    CHECK(!s.queue().rated);
    app->frames(70);                          // the queue's status, a second in
    CHECK(s.queue().searching);
    REQUIRE(gameStarts(*app, 300));
    CHECK(!s.queue().searching);
    REQUIRE(s.link());
    CHECK(s.link()->kind() == game::LinkKind::Server);
    CHECK(s.link()->canReport());
    CHECK_EQ(s.link()->eventName(), std::string("Scacelith (mock server)"));
    CHECK(s.pingMs() > 0);

    REQUIRE(pause(*app));
    CHECK(app->hasLabel("Abort the game"));
    CHECK(!app->hasLabel("Resign"));
    CHECK(app->hasLabel("Report opponent"));
    CHECK(enabled(*app, "Offer draw"));
    CHECK(!enabled(*app, "Claim draw"));
    REQUIRE(app->clickLabel("Resume"));
    app->frames(2);
    CHECK(!app->hasLabel("Resume"));
    REQUIRE(pause(*app));
    REQUIRE(app->clickLabel("Abort the game"));
    REQUIRE(gameOverCard(*app));
    CHECK(app->hasLabel("Report opponent"));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("Find opponent"); }, 300));
    CHECK(s.link() == nullptr);

    const std::vector<net::GameSummary> games = history(*app);
    REQUIRE(!games.empty());
    CHECK_EQ(games[0].status, int(net::proto::GameStatus::Aborted));
    CHECK_EQ(games[0].reason, int(net::proto::EndReason::Aborted));
    CHECK_EQ(games[0].result, std::string("*"));
    CHECK(!games[0].rated);
    CHECK(games[0].white.name == "Matchmaker" || games[0].black.name == "Matchmaker");
    REQUIRE(s.rating("5+3"));
    CHECK_EQ(s.rating("5+3")->rating, rating);   // an aborted game changes no rating
    CHECK(toTitle(*app));
}

// A challenge with White: the challenge page (the opponent's name, the colour from the setting),
// the card of the challenge waiting on the Play page, then the game. The human's scripted e2e4 goes
// to the server; the opponent answers. While the game goes on, the opponent leaves for 20 s (F9)
// and our own connection drops for 8 s (F10): the session reconnects and the game goes on. Then
// the human resigns (confirmed): the rating falls, the card offers the rematch and the report.
// The report goes from the card; Rematch starts a new game against the same player, which the
// human leaves at once. The history has the game: White's resignation after two plies.
TEST(online_scene_challenge_with_white_resign_and_rematch) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    REQUIRE(signIn(*app, "Whitehand"));
    game::OnlineSession& s = ses();
    const int rating = s.rating("5+3") ? s.rating("5+3")->rating : 0;

    game::settings().onlineColor = 1;
    std::vector<ui::im::TracedItem> rows = playRows(*app);
    REQUIRE(rows.size() == 3);
    app->clickItem(rows[0]);
    REQUIRE(onPage(*app, "Send challenge", 30));
    CHECK(app->hasLabel("Your colour"));
    CHECK(app->hasLabel("Rated"));
    CHECK(!enabled(*app, "Send challenge"));   // no opponent named yet
    REQUIRE(fill(*app, "User name", "Rival"));
    CHECK(enabled(*app, "Send challenge"));
    REQUIRE(app->clickLabel("Send challenge"));
    // Back on the Play page with the challenge's card (its Cancel) until the answer.
    CHECK(app->hasLabel("Find opponent"));
    CHECK(app->hasLabel("Cancel"));
    CHECK(s.outgoing().active);
    CHECK_EQ(s.outgoing().target, std::string("Rival"));
    CHECK_EQ(game::settings().onlineColor, 1);
    REQUIRE(gameStarts(*app));
    CHECK(!s.outgoing().active);

    // The scripted first move, then the opponent's answer (its first move comes within 2.4 s).
    app->frames(300);
    REQUIRE(pause(*app));
    CHECK(app->hasLabel("Resign"));            // the first move is made: no abort any more
    CHECK(!app->hasLabel("Abort the game"));
    REQUIRE(app->clickLabel("Resume"));

    // The opponent away for 20 s (its grace is 30 s: it comes back), the fakes' developer key.
    app->key(plat::KEY_F9);
    app->frames(60);
    // Our own connection lost for 8 s: the session reconnects by itself.
    app->key(plat::KEY_F10);
    REQUIRE(app->runUntil([&] { return s.conn() != net::ConnState::Online; }, 60));
    CHECK(s.link() && s.link()->reconnecting());
    REQUIRE(app->runUntil([&] { return s.conn() == net::ConnState::Online; }, 900));
    CHECK(!s.link()->reconnecting());
    app->key(plat::KEY_F11);   // the opponent's stance held (standing), then given back
    app->frames(30);
    app->key(plat::KEY_F11);
    app->key(plat::KEY_F11);
    app->key(plat::KEY_F11);
    app->key(plat::KEY_F11);
    app->frames(900);           // the opponent is back (20 s after leaving)

    REQUIRE(pause(*app));
    REQUIRE(app->clickLabel("Resign"));
    CHECK(app->hasLabel("Cancel"));            // the confirmation
    REQUIRE(clickLast(*app, "Resign"));
    REQUIRE(gameOverCard(*app));
    CHECK(app->hasLabel("Analyse the game"));  // a game with moves
    REQUIRE(s.rating("5+3"));
    CHECK(s.rating("5+3")->rating < rating);   // rated, lost

    // The report from the card: its dialog, sent at once (the game is over), then the button goes.
    REQUIRE(app->clickLabel("Report opponent"));
    REQUIRE(app->waitForLabel("Send report", 30));
    CHECK(app->hasLabel("Reason"));
    REQUIRE(fill(*app, "Comment", "Moves too fast for a human"));
    REQUIRE(app->clickLabel("Send report"));
    app->frames(60);
    CHECK(!app->hasLabel("Send report"));
    CHECK(!app->hasLabel("Report opponent"));   // sent, and taken by the server

    // Rematch: offered (or the opponent's offer accepted), then the next game against them.
    if (!app->clickLabel("Rematch")) CHECK(app->clickLabel("Accept rematch"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("Rematch offered\xE2\x80\xA6") || !app->hasLabel("Main menu"); }, 300));
    REQUIRE(app->runUntil([&] { return !app->hasLabel("Main menu"); }, 600));   // the card goes: the next game
    REQUIRE(app->runUntil([&] { return s.link() != nullptr && !app->hasLabel("Main menu"); }, 600));
    REQUIRE(leaveGame(*app));

    const std::vector<net::GameSummary> games = history(*app);
    REQUIRE(games.size() >= 2);
    const net::GameSummary& resigned = games[1];   // the rematch left is the newest
    CHECK_EQ(resigned.white.name, std::string("Whitehand"));
    CHECK_EQ(resigned.black.name, std::string("Rival"));
    CHECK_EQ(resigned.you, 0);
    CHECK_EQ(resigned.result, std::string("0-1"));
    CHECK_EQ(resigned.reason, int(net::proto::EndReason::Resignation));
    CHECK_EQ(resigned.plies, 2);
    CHECK(resigned.rated);
    CHECK(games[0].white.name == "Rival");         // the rematch: colours swapped
    CHECK(toTitle(*app));
}

// A challenge with Black: the opponent moves first, the human never moves. A report filled in during
// the game waits for its end (the Esc menu has no Report any more); a draw offer goes to the
// opponent; then the human aborts (allowed before their own first move, even after the opponent's).
// At the end the waiting report goes, and the card has no Report button.
TEST(online_scene_challenge_with_black_report_waits_for_the_end) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    REQUIRE(signIn(*app, "Blackhand"));
    REQUIRE(challenge(*app, "Opponent_B", 2));
    app->frames(240);   // the opponent's first move

    REQUIRE(pause(*app));
    REQUIRE(app->clickLabel("Report opponent"));
    REQUIRE(app->waitForLabel("Send report", 30));
    // The reason: Abusive behaviour (the selector's right arrow, with the keyboard).
    const ui::im::TracedItem* reason = app->find("Reason");
    REQUIRE(reason);
    app->clickItem(*reason);
    app->key(plat::KEY_RIGHT);
    REQUIRE(fill(*app, "Comment", "Rude"));
    REQUIRE(app->clickLabel("Send report"));
    REQUIRE(pause(*app));
    CHECK(!app->hasLabel("Report opponent"));   // queued until the end of the game
    CHECK(app->hasLabel("Abort the game"));
    REQUIRE(app->clickLabel("Offer draw"));
    app->frames(240);                            // the opponent's answer (a decline, or the draw)
    if (!app->hasLabel("Main menu")) {
        REQUIRE(pause(*app));
        CHECK(!enabled(*app, "Offer draw") || app->hasLabel("Offer draw"));
        REQUIRE(app->clickLabel("Abort the game"));
    }
    REQUIRE(gameOverCard(*app));
    app->frames(120);                            // the report goes 1.2 s after the end
    CHECK(!app->hasLabel("Report opponent"));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("Find opponent"); }, 300));
    const std::vector<net::GameSummary> games = history(*app);
    REQUIRE(!games.empty());
    CHECK_EQ(games[0].you, 1);
    CHECK_EQ(games[0].white.name, std::string("Opponent_B"));
    CHECK(toTitle(*app));
}

// A private game created: back on the Play page, its card shows the code the server gave until a
// friend joins (six seconds with the fakes). Then one joined with a friend's code: its game comes
// at once. Each is left before its first move (aborted).
TEST(online_scene_private_games_created_and_joined) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    REQUIRE(signIn(*app, "Privateer"));
    game::OnlineSession& s = ses();
    game::settings().onlineRated = false;
    std::vector<ui::im::TracedItem> rows = playRows(*app);
    REQUIRE(rows.size() == 3);
    app->clickItem(rows[1]);
    REQUIRE(onPage(*app, "Create the game", 30));
    CHECK(app->hasLabel("Join"));
    CHECK(!enabled(*app, "Join"));               // no code typed yet
    REQUIRE(app->clickLabel("Create the game"));
    REQUIRE(app->runUntil([&] { return !s.outgoing().code.empty(); }, 60));
    CHECK(s.outgoing().active);
    CHECK(s.outgoing().target.empty());
    CHECK_EQ(s.outgoing().code.size(), size_t(6));
    REQUIRE(gameStarts(*app, 600));
    CHECK(!s.outgoing().active);
    REQUIRE(leaveGame(*app));

    rows = playRows(*app);
    REQUIRE(rows.size() == 3);
    app->clickItem(rows[1]);
    REQUIRE(onPage(*app, "Join", 30));
    REQUIRE(fill(*app, "Code", "abc-123", 16));
    REQUIRE(enabled(*app, "Join"));
    REQUIRE(app->clickLabel("Join"));
    CHECK(s.joining());
    REQUIRE(gameStarts(*app, 300));
    CHECK(!s.joining());
    REQUIRE(leaveGame(*app));
    const std::vector<net::GameSummary> games = history(*app);
    REQUIRE(games.size() >= 2);
    CHECK(games[0].white.name == "Camille_D" || games[0].black.name == "Camille_D");
    CHECK(games[1].white.name == "a_friend" || games[1].black.name == "a_friend");
    CHECK(!games[1].rated);
    CHECK(toTitle(*app));
}

// A player challenges us while we idle in the menus (the fakes do it once, 25 s in): its card
// shows on whatever page is open until we answer it. Accepting it starts its game at once.
TEST(online_scene_challenge_received_and_accepted) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    REQUIRE(signIn(*app, "Idler"));
    game::OnlineSession& s = ses();
    REQUIRE(app->runUntil([&] { return !s.incoming().empty(); }, 1800));
    REQUIRE(app->waitForLabel("Accept", 60));
    app->frames(40);   // its buttons are live once the card is readable (it slides in)
    CHECK_EQ(s.incoming().size(), size_t(1));
    CHECK_EQ(s.incoming()[0].from.name, std::string("Eleonora_V"));
    CHECK(app->hasLabel("Decline"));
    REQUIRE(app->clickLabel("Accept"));
    REQUIRE(gameStarts(*app, 300));
    CHECK(s.incoming().empty());
    REQUIRE(leaveGame(*app));
    const std::vector<net::GameSummary> games = history(*app);
    REQUIRE(!games.empty());
    CHECK(games[0].white.name == "Eleonora_V" || games[0].black.name == "Eleonora_V");
    CHECK(toTitle(*app));
}

// From the title page, signed out: the sign-in page's Direct match, its two choices (Host a game,
// Join a game) shown and live.
static bool toDirectPage(headless::App& app) {
    if (!toSignInPage(app) || !app.clickLabel("Direct match")) return false;
    if (!app.runUntil([&] { return bare(app, 100.0f).size() == 2; }, 30)) return false;
    app.frames(30);
    return bare(app, 100.0f).size() == 2;
}

// The Esc menu's way out of a direct match; true once back on the Direct match page (where the
// menu comes back after a direct match).
static bool leaveDirectGame(headless::App& app) {
    if (!pause(app) || !app.clickLabel("Leave the game")) return false;
    if (!app.clickLabel("Abort and leave") && !app.clickLabel("Resign and leave")) return false;
    return app.runUntil([&] { return ses().link() == nullptr && bare(app, 100.0f).size() == 2; }, 600);
}

// A direct match hosted (no server, no account: from the sign-in page's Direct match): the host
// page (the colour, the port checked), the invitation while the friend comes (the address, port
// and code; its Copy), the game with White: the human's e2e4, the friend's answer, then the human
// resigns. A direct match has no report; its card's rematch swaps the colours, and that game is
// left at once. The resigned game goes to the saved games.
TEST(online_scene_direct_match_hosted) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    game::OnlineSession& s = ses();
    REQUIRE(toDirectPage(*app));
    game::settings().directColor = 1;
    app->clickItem(bare(*app, 100.0f)[0]);
    REQUIRE(onPage(*app, "Host", 30));
    CHECK(app->hasLabel("Your colour"));
    CHECK(app->hasLabel("Open the port on my router (UPnP)"));
    REQUIRE(fill(*app, "Port", "80", 6));
    CHECK(!enabled(*app, "Host"));               // below 1024
    REQUIRE(fill(*app, "Port", "47123", 6));
    REQUIRE(enabled(*app, "Host"));
    REQUIRE(app->clickLabel("Host"));
    CHECK_EQ(game::settings().directPort, 47123);
    CHECK(s.directActive());
    REQUIRE(app->waitForLabel("Copy", 120));     // the port opened: the invitation
    CHECK(app->hasLabel("Cancel"));
    const net::DirectInvite inv = s.direct().invite();
    CHECK_EQ(int(inv.port), 47123);
    CHECK_EQ(inv.publicAddress, std::string("203.0.113.47"));
    CHECK_EQ(inv.code.size(), size_t(14));
    CHECK(s.direct().isHost());
    fakeplat::record().clipboard.clear();
    REQUIRE(app->clickLabel("Copy"));
#if !defined(_WIN32)   // the test platform's clipboard (Windows' own is not faked: the copy fails, a notice says so)
    CHECK_EQ(fakeplat::record().clipboard, "203.0.113.47:47123 " + inv.code);
    CHECK(!fakeplat::record().clipboardSensitive);
#endif
    REQUIRE(gameStarts(*app, 400));
    CHECK(s.link()->kind() == game::LinkKind::Direct);
    CHECK(!s.link()->canReport());
    CHECK_EQ(s.pingMs(), 12);
    app->frames(300);                            // e2e4, and the friend's answer
    REQUIRE(pause(*app));
    CHECK(!app->hasLabel("Report opponent"));
    REQUIRE(app->clickLabel("Resign"));
    REQUIRE(clickLast(*app, "Resign"));
    REQUIRE(gameOverCard(*app));
    CHECK(!app->hasLabel("Report opponent"));
    if (!app->clickLabel("Rematch")) REQUIRE(app->clickLabel("Accept rematch"));
    REQUIRE(app->runUntil([&] { return !app->hasLabel("Main menu"); }, 600));
    REQUIRE(app->runUntil([&] { return s.link() != nullptr; }, 300));
    REQUIRE(leaveDirectGame(*app));
    CHECK(s.direct().state() == net::DirectMatch::State::Idle);
    int saved = 0;
    std::error_code ec;
    for (const auto& f : std::filesystem::directory_iterator(std::filesystem::u8path(app->folder().file("pgn")), ec))
        if (f.path().extension() == ".pgn") ++saved;
    CHECK(saved >= 1);
    CHECK(toTitle(*app));
}

// A direct match joined: a host that refuses the connection (its error, the form usable again),
// then the address pasted as the host's Copy writes it (address:port code), split into its fields
// when Join is pressed; the connection, the game (the colour drawn by lot), left at once.
TEST(online_scene_direct_match_joined) {
    ReleaseAtEnd release;
    headless::App* app = onlineApp();
    REQUIRE(app);
    game::OnlineSession& s = ses();
    REQUIRE(toDirectPage(*app));
    app->clickItem(bare(*app, 100.0f)[1]);
    REQUIRE(onPage(*app, "Address", 30));
    CHECK(app->hasLabel("Code"));
    CHECK(!enabled(*app, "Join"));
    REQUIRE(fill(*app, "Address", "refused.test"));
    REQUIRE(fill(*app, "Code", "ABCD-EFGH-JKMN", 16));
    REQUIRE(enabled(*app, "Join"));
    REQUIRE(app->clickLabel("Join"));
    CHECK_EQ(game::settings().directAddress, std::string("refused.test"));
    app->frames(10);
    CHECK(enabled(*app, "Join"));                // it failed: the form again
    CHECK(app->hasLabel("Back"));
    CHECK(s.direct().state() == net::DirectMatch::State::Idle);
    REQUIRE(fill(*app, "Code", "", 16));
    // The host's invitation pasted (Ctrl+V) into the emptied address field.
    fakeplat::record().clipboard = "192.168.1.23:47100 ABCD-EFGH-JKMN";
    REQUIRE(fill(*app, "Address", ""));
    app->key('V', false, true);
    REQUIRE(enabled(*app, "Join"));
    REQUIRE(app->clickLabel("Join"));
    CHECK_EQ(game::settings().directAddress, std::string("192.168.1.23"));
    CHECK_EQ(game::settings().directJoinPort, 47100);
    CHECK(app->hasLabel("Cancel"));              // connecting
    CHECK(!s.direct().isHost());
    REQUIRE(gameStarts(*app, 300));
    CHECK(s.link()->kind() == game::LinkKind::Direct);
    REQUIRE(leaveDirectGame(*app));
    CHECK(toTitle(*app));
}
