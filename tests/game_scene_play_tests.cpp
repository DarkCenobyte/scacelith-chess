// The game scene against Stockfish (src/game/game_scene.cpp), played the way a player plays it on
// the headless game (headless.h): the title page, the New Game page, the handshake, then the
// pieces touched and put down with the mouse through the player's eyes, the clock pressed with
// Space or a click, the Esc menu (draw offer, resignation, leaving), the game over card (the board
// viewed, the rematch with the colours swapped), the look around, standing up, and the games saved
// in the App's folder. Also: illegal moves without the hints, a flag falling, Watch a Game, the
// window closed during a game, and a screenshot run (--shot). Stockfish plays at its weakest level
// without humanised thinking time, so that its replies come within a few frames; without the
// embedded engine (a build without Stockfish) the opponent plays random legal moves, and only what
// depends on the engine is skipped.
#include "game_scene_testkit.h"
#include "net/net_sys.h"

using namespace gstest;

namespace {

// Stockfish at once, at its weakest; the player with White at 5+0, no move made by the robot alone.
void playSettings(int timeControl) {
    game::Settings& s = game::settings();
    s.humanizeThinking = false;
    s.difficultyPreset = 0;
    s.timeControlPreset = timeControl;
    s.nextColor = 0;
    s.opponent = 0;
    s.showLegalMoves = true;
    s.autoPressClock = false;
    s.gameCursor = true;
    s.saveGames = true;
}

// From the title page through the New Game page: a game against Stockfish, the handshake done.
bool startFromMenu(headless::App& app, LogTail& log, const std::string& expect) {
    if (!app.clickLabel("New Game") || !app.waitForLabel("Start", 120) || !app.clickLabel("Start")) return false;
    if (!waitLog(app, log, expect)) return false;
    app.frames(200);   // the lights come up, the players shake hands, White's clock starts
    return true;
}

// A drag and drop: the button pressed on the piece, held while the pointer goes to the square,
// released there.
void dragMove(headless::App& app, m::vec3 piece, m::vec3 square) {
    pointAt(app, piece);
    plat::Input& in = fakeplat::next();
    in.mouseDown[plat::MOUSE_LEFT] = in.mousePressed[plat::MOUSE_LEFT] = true;
    app.frame();
    const m::vec2 a = toScreen(app, piece);
    for (int i = 1; i <= 12; ++i) {
        const m::vec2 b = toScreen(app, square);
        const float k = float(i) / 12.0f;
        app.moveMouse(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k);
    }
    pointAt(app, square);
    in.mouseDown[plat::MOUSE_LEFT] = false;
    in.mouseReleased[plat::MOUSE_LEFT] = true;
    app.frame();
}

}  // namespace

// A whole game against Stockfish by hand: touch-move (the king without a move given up, an
// illegal square refused, another piece refused), a draw offer made with a move before the clock
// press and declined, the look around, standing up and sitting down, a move by drag and drop and a
// clock pressed with the mouse, a window resized and minimised, the resignation. The game is saved
// with its moves, clocks and result, and rated; the card's board view, then the rematch with Black,
// left at once through the Esc menu: saved as a resignation too.
TEST(game_scene_play_against_stockfish_by_hand) {
    LogTail log;
    headless::App* app = titleApp();
    REQUIRE(app);
    SettingsGuard guard;
    playSettings(4);   // 5+0
    game::Settings& s = game::settings();
    const int gamesBefore = s.playerGames, lossesBefore = s.playerLosses;
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(startFromMenu(*app, log, "New game: human plays White, 5+0"));
    chess::Game g;

    // Space before any move only says to move first; the pointer on a piece offers it.
    pointAt(*app, piecePoint("g1", chess::Knight));
    pressKey(*app, ' ');
    // The king has no legal move: touched, it is given up for the pawn clicked next (FIDE 4.5).
    clickAt(*app, piecePoint("e1", chess::King));
    app->frames(20);
    clickAt(*app, piecePoint("d2", chess::Pawn));
    app->frames(20);
    clickAt(*app, squarePoint("d5"));                   // illegal with the hints on: refused
    clickAt(*app, squarePoint("c1", 0.004f));           // another piece of mine: touch-move says no
    clickAt(*app, piecePoint("d2", chess::Pawn));       // the piece in hand: its own square
    clickAt(*app, squarePoint("d4"));
    app->frames(150);                                   // put down: the clock waits for the press
    clickAt(*app, piecePoint("b1", chess::Knight));     // a click now only reminds of the press
    // A draw offered with the move, then the press (FIDE 9.1.2): Stockfish declines it.
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Offer draw", 30));
    REQUIRE(app->clickLabel("Offer draw"));
    pressKey(*app, ' ');
    REQUIRE(waitLog(*app, log, "] move 1: d4 (White"));
    REQUIRE(waitLog(*app, log, "] move 2: "));
    REQUIRE(g.play(g.position().parseUCI("d2d4")));
    const std::string reply = loggedMove(log, 2);
    REQUIRE(g.play(g.position().parseSAN(reply)));
    CHECK(g.status() == chess::GameStatus::Ongoing);

    // The look: the right button held and moved, the wheel leans in, S looks at the scoresheet and
    // back, C centres the view, Tab shows and hides the move list.
    {
        plat::Input& in = fakeplat::next();
        in.mouseDown[plat::MOUSE_RIGHT] = true;
        app->frame();
        CHECK(fakeplat::record().mouseCaptured);
        app->moveMouse(500.0f, 380.0f);
        app->moveMouse(470.0f, 360.0f);
        in.mouseDown[plat::MOUSE_RIGHT] = false;
        app->frame();
        CHECK(!fakeplat::record().mouseCaptured);
        app->wheel(2.0f);
        pressKey(*app, 'S');
        app->frames(40);
        pressKey(*app, 'S');
        in.mousePressed[plat::MOUSE_MIDDLE] = true;
        app->frame();
        pressKey(*app, 'C');
        pressKey(*app, plat::KEY_TAB);
        app->frames(20);
        pressKey(*app, plat::KEY_TAB);
        app->wheel(-2.0f);
        app->frames(60);
    }
    // Standing up: nothing on the board answers until the player sits back down.
    pressKey(*app, plat::KEY_UP);
    app->frames(150);
    clickAt(*app, piecePoint("g1", chess::Knight));
    pressKey(*app, ' ');
    pressKey(*app, plat::KEY_DOWN);
    app->frames(240);
    // The knight by drag and drop, the clock pressed with the mouse.
    dragMove(*app, piecePoint("g1", chess::Knight), squarePoint("f3"));
    app->frames(120);
    clickAt(*app, clockPoint());
    REQUIRE(waitLog(*app, log, "] move 3: Nf3 (White"));
    REQUIRE(waitLog(*app, log, "] move 4: "));
    // The window resized, then minimised (no frame drawn), then back.
    fakeplat::setSize(1600, 900);
    app->frames(3);
    fakeplat::setSize(0, 0);
    app->frames(3);
    fakeplat::setSize(1280, 720);
    app->frames(3);
    // Resigned through the Esc menu and its confirmation.
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Resign", 30));
    REQUIRE(app->clickLabel("Resign"));
    REQUIRE(app->waitForLabel("Cancel", 30));
    REQUIRE(clickLast(*app, "Resign"));
    REQUIRE(waitLog(*app, log, "Game over: 0-1"));

    const std::vector<chess::pgn::Record> games = newGames(folder, before);
    REQUIRE(games.size() == 1u);
    const chess::pgn::Record& r = games[0];
    CHECK_EQ(r.result, std::string("0-1"));
    CHECK_EQ(r.tag("White"), s.playerName);
    CHECK_EQ(r.tag("Black"), std::string("Stockfish"));
    CHECK_EQ(r.tag("ScacelithMode"), std::string("play"));
    CHECK_EQ(r.tag("TimeControl"), std::string("300"));
    REQUIRE(r.plies.size() == 4u);
    CHECK_EQ(r.plies[0].san, std::string("d4"));
    CHECK_EQ(r.plies[1].san, reply);
    CHECK_EQ(r.plies[2].san, std::string("Nf3"));
    for (const chess::pgn::Ply& p : r.plies) CHECK(p.clockMs > 0 && p.clockMs <= 300000);
#if defined(SCACELITH_HAS_STOCKFISH)
    // Rated: both players made a move.
    CHECK_EQ(s.playerGames, gamesBefore + 1);
    CHECK_EQ(s.playerLosses, lossesBefore + 1);
    CHECK_EQ(savedIni().getInt("player.games"), gamesBefore + 1);
#else
    (void)gamesBefore;
    (void)lossesBefore;
#endif

    // The card: folded to see the board (Tab shows the moves), shown again, then the rematch.
    REQUIRE(app->waitForLabel("Rematch", 1200));
    REQUIRE(app->clickLabel("View the board"));
    REQUIRE(app->waitForLabel("Show", 60));
    pressKey(*app, plat::KEY_TAB);
    app->frames(10);
    REQUIRE(app->clickLabel("Show"));
    app->frames(40);   // the card unfolds (its buttons wait for it)
    const size_t mark = log.mark();
    REQUIRE(app->clickLabel("Rematch"));
    // The colours alternate from one game to the next: Stockfish opens. The player's robot now
    // presses the clock by itself (Options > Gameplay).
    REQUIRE(waitLog(*app, log, "New game: human plays Black, 5+0", mark));
    REQUIRE(waitLog(*app, log, "] move 1: ", mark));
    s.autoPressClock = true;
    chess::Game g2;
    REQUIRE(g2.play(g2.position().parseSAN(loggedMove(log, 1, mark))));
    app->frames(60);
    playMove(*app, g2.position(), "g8f6");
    REQUIRE(waitLog(*app, log, "] move 2: Nf6 (Black", mark));
    REQUIRE(waitLog(*app, log, "] move 3: ", mark));
    app->frames(30);
    // Left through the Esc menu: a resignation, saved as such.
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Main menu", 30));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->waitForLabel("Leave", 30));
    REQUIRE(clickLast(*app, "Leave"));
    REQUIRE(app->waitForLabel("New Game", 600));
    const std::vector<chess::pgn::Record> both = newGames(folder, before);
    REQUIRE(both.size() == 2u);
    const chess::pgn::Record& left = both[0].plies.size() == 3 ? both[0] : both[1];
    CHECK_EQ(left.plies.size(), size_t(3));
    CHECK_EQ(left.result, std::string("1-0"));
    CHECK_EQ(left.tag("Black"), s.playerName);
    CHECK_EQ(left.tag("ScacelithEnd"), std::string("resignation"));
}

// Without the legal-move hints the arbiter judges a move once it is completed (untimed: as soon as
// it is put down): a first illegal move is taken back and the same piece must move (another one is
// refused), a second one loses the game (FIDE 7.5.5), saved as such.
TEST(game_scene_play_illegal_moves_without_hints) {
    LogTail log;
    headless::App* app = titleApp();
    REQUIRE(app);
    SettingsGuard guard;
    playSettings(0);   // no clock
    game::settings().showLegalMoves = false;
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(startFromMenu(*app, log, "New game: human plays White, Unlimited"));
    chess::Game g;
    playMove(*app, g.position(), "d2d4");
    REQUIRE(waitLog(*app, log, "] move 1: d4 (White"));
    REQUIRE(waitLog(*app, log, "] move 2: "));
    REQUIRE(g.play(g.position().parseUCI("d2d4")));
    REQUIRE(g.play(g.position().parseSAN(loggedMove(log, 2))));
    app->frames(60);
    // c2-c5 is no pawn move: put down all the same, then taken back by the arbiter.
    playMove(*app, g.position(), "c2c5");
    app->frames(200);
    CHECK(!log.has("Game over"));
    // The pawn touched must still be moved: the knight is refused.
    clickAt(*app, piecePoint("g1", chess::Knight));
    app->frames(20);
    playMove(*app, g.position(), "c2c5");
    REQUIRE(waitLog(*app, log, "Game over: 0-1"));
    CHECK(!log.has("] move 3: "));
    const std::vector<chess::pgn::Record> games = newGames(folder, before);
    REQUIRE(games.size() == 1u);
    CHECK_EQ(games[0].result, std::string("0-1"));
    CHECK_EQ(games[0].plies.size(), size_t(2));
    CHECK_EQ(games[0].tag("ScacelithEnd"), std::string("illegal_moves"));
    CHECK_EQ(games[0].tag("TimeControl"), std::string("-"));
    REQUIRE(app->waitForLabel("Rematch", 1200));
    REQUIRE(toTitle(*app));
}

// The player's flag falls (15 s with a 2 s delay, nobody moving; the Esc menu stops the clock
// meanwhile, and its Options page opens and closes): lost on time, saved without a move, not rated.
TEST(game_scene_play_flag_falls) {
    LogTail log;
    headless::App* app = titleApp();
    REQUIRE(app);
    SettingsGuard guard;
    playSettings(-1);   // the custom clock
    game::Settings& s = game::settings();
    s.customBaseSeconds = 15;
    s.customIncrementSeconds = 0;
    s.customDelaySeconds = 2;
    const int gamesBefore = s.playerGames;
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(startFromMenu(*app, log, "New game: human plays White, Custom 0:15+0 d2"));
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Options", 30));
    REQUIRE(app->clickLabel("Options"));
    app->frames(30);
    app->key(plat::KEY_ESCAPE);
    app->frames(330);   // paused: no time counts
    CHECK(!log.has("Game over"));
    REQUIRE(app->waitForLabel("Resume", 30));
    REQUIRE(app->clickLabel("Resume"));
    REQUIRE(runFor(*app, [&] { return log.has("Game over: 0-1"); }, 120.0, 1000, 0.05f));
    CHECK(log.has("Elo: game not rated (0 plies"));
    CHECK_EQ(s.playerGames, gamesBefore);
    const std::vector<chess::pgn::Record> games = newGames(folder, before);
    REQUIRE(games.size() == 1u);
    CHECK_EQ(games[0].result, std::string("0-1"));
    CHECK(games[0].plies.empty());
    CHECK_EQ(games[0].tag("ScacelithEnd"), std::string("timeout"));
    CHECK_EQ(games[0].tag("TimeControl"), std::string("15"));
    REQUIRE(app->waitForLabel("Rematch", 1200));
    REQUIRE(toTitle(*app));
}

// Watch a Game: two Stockfish players with 15 s each, watched from the free camera: the
// viewpoints (0 to 9), the controls hidden and shown (H, saved), the move list (Tab), flying
// (W A S D, Shift), looking around (right button), the speed (wheel), the Esc menu. The game ends
// (on time at the latest), nothing is saved; "Watch again" starts another, left from its Esc menu.
TEST(game_scene_play_watch_a_game) {
#if defined(SCACELITH_HAS_STOCKFISH)
    LogTail log;
    headless::App* app = titleApp();
    REQUIRE(app);
    SettingsGuard guard;
    game::Settings& s = game::settings();
    s.humanizeThinking = false;
    s.viewerWhitePreset = 0;
    s.viewerBlackPreset = 0;
    s.viewerTimeControl = -1;
    s.viewerCustomBaseSeconds = 15;
    s.viewerCustomIncrementSeconds = 0;
    s.viewerCustomDelaySeconds = 0;
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(app->clickLabel("Watch a Game"));
    REQUIRE(app->waitForLabel("Start", 120));
    REQUIRE(app->clickLabel("Start"));
    REQUIRE(waitLog(*app, log, "New game (watching): Novice vs Novice"));
    app->frames(120);
    for (int n = 0; n <= 9; ++n) {
        pressKey(*app, '0' + n);
        app->frames(20);
    }
    pressKey(*app, '1');
    const bool shown = s.viewerShowControls;
    pressKey(*app, 'H');
    CHECK(s.viewerShowControls != shown);
    CHECK(savedIni().getBool("viewer.show_controls", shown) != shown);
    pressKey(*app, 'H');
    CHECK(s.viewerShowControls == shown);
    pressKey(*app, plat::KEY_TAB);
    {
        plat::Input& in = fakeplat::next();
        in.keyDown['W'] = in.keyDown[plat::KEY_LSHIFT] = true;
        app->frames(10);
        in.keyDown['W'] = in.keyDown[plat::KEY_LSHIFT] = false;
        in.keyDown['A'] = true;
        app->frames(5);
        in.keyDown['A'] = false;
        in.mouseDown[plat::MOUSE_RIGHT] = true;
        app->frame();
        app->moveMouse(600.0f, 300.0f);
        app->moveMouse(620.0f, 310.0f);
        in.mouseDown[plat::MOUSE_RIGHT] = false;
        app->frame();
        app->wheel(1.0f);
        app->frames(5);
    }
    pressKey(*app, '0');   // through the eyes of the player to move, following the moves
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Resume", 30));
    app->frames(30);
    REQUIRE(app->clickLabel("Resume"));
    REQUIRE(runFor(*app, [&] { return log.has("Game over: "); }, 120.0, 3000, 0.05f));
    REQUIRE(app->waitForLabel("Watch again", 1200));
    CHECK(newGames(folder, before).empty());   // a watched game is never saved
    const size_t mark = log.mark();
    REQUIRE(app->clickLabel("Watch again"));
    REQUIRE(waitLog(*app, log, "New game (watching)", mark));
    app->frames(60);
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Main menu", 30));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->waitForLabel("New Game", 600));
#else
    SKIP("built without the embedded Stockfish");
#endif
}

// The window closed in the middle of a game (the scene shut down): the game is resigned, rated
// once both players have moved, and saved as it stands; the settings are saved on the way out.
TEST(game_scene_play_window_closed_mid_game) {
    LogTail log;
    headless::App* app = titleApp();
    REQUIRE(app);
    SettingsGuard guard;
    playSettings(4);   // 5+0
    game::Settings& s = game::settings();
    const int gamesBefore = s.playerGames;
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(startFromMenu(*app, log, "New game: human plays White, 5+0"));
    chess::Game g;
    playMove(*app, g.position(), "d2d4");
    app->frames(30);
    pressKey(*app, ' ');
    REQUIRE(waitLog(*app, log, "] move 1: d4 (White"));
    REQUIRE(waitLog(*app, log, "] move 2: "));
    app->frames(30);
    app->stop();
    const std::vector<chess::pgn::Record> games = newGames(folder, before);
    REQUIRE(games.size() == 1u);
    CHECK_EQ(games[0].result, std::string("0-1"));
    CHECK_EQ(games[0].tag("ScacelithEnd"), std::string("resignation"));
    CHECK_EQ(games[0].plies.size(), size_t(2));
#if defined(SCACELITH_HAS_STOCKFISH)
    CHECK_EQ(s.playerGames, gamesBefore + 1);
    CHECK_EQ(savedIni().getInt("player.games"), gamesBefore + 1);
#else
    CHECK_EQ(s.playerGames, gamesBefore);   // a random opponent rates nothing
#endif
}

// A screenshot run (--shot) with a settings file of its own (--ini): everything loaded before the
// first frame, a deterministic warp of the game's time, the player standing up by --stance as soon
// as their hands are free (then sitting back down with the arrow), a pointer and a camera of their
// own (--mouse, --cam, --look, --fov), the look at the scoresheet (--glance). Such a run leaves the
// settings file and the saved games alone, even when the game is resigned.
TEST(game_scene_play_screenshot_run) {
    LogTail log;
    headless::TempDir dir("shotini");
    const std::string ini = dir.file("shot.ini");
    const std::string iniBefore =
        "[graphics]\nbrightness_calibrated = true\n[newgame]\ndifficulty = 0\n[engine]\nhumanize = false\n";
    FILE* f = std::fopen(ini.c_str(), "wb");
    REQUIRE(f);
    std::fputs(iniBefore.c_str(), f);
    std::fclose(f);
    headless::App* app = headless::shared({"--shot", "shot.png", "--ini", ini, "--start", "--human", "black", "--tc", "4",
                                           "--no-intro", "--warp", "3", "--stance", "standing", "--mouse", "0.5,0.62",
                                           "--glance", "--cam", "0.62,1.24,0.8", "--look", "0,0.8,0", "--fov", "42"});
    REQUIRE(app);
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(waitLog(*app, log, "New game: human plays Black, 5+0, difficulty 0"));
    REQUIRE(waitLog(*app, log, "--stance: seat 1 asks for stance 1"));
    REQUIRE(waitLog(*app, log, "] move 1: "));              // Stockfish opens
    app->frames(120);
    const size_t mark = log.mark();
    pressKey(*app, plat::KEY_DOWN);                         // back in the chair
    REQUIRE(waitLog(*app, log, "Stance: seat 1 asks for stance 0", mark));
    app->frames(240);
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Resign", 30));
    REQUIRE(app->clickLabel("Resign"));
    REQUIRE(app->waitForLabel("Cancel", 30));
    REQUIRE(clickLast(*app, "Resign"));
    REQUIRE(waitLog(*app, log, "Game over: 1-0"));
    REQUIRE(app->waitForLabel("Rematch", 1200));
    CHECK(pgnFiles(folder) == before);
    app->stop();   // the settings are saved as the App stops: not in a screenshot run
    std::string iniAfter;
    CHECK(net::sys::readFile(ini, iniAfter, 1 << 20));
    CHECK(iniAfter == iniBefore);
    headless::stopShared();   // the next file's tests start their own App (and engine)
}
