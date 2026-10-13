// Hot-seat games in the game scene (src/game/game_scene_hotseat.cpp and the hot-seat branches of
// src/game/game_scene.cpp): two people at one computer, each moving from their own robot's eyes,
// played on the headless game (headless.h) with the mouse and the keyboard. The view goes over to
// the next player after every completed move (a camera flight or a cut through black), draw
// offers go with a move and are answered on a card, a resignation is named, a flag falls, a game
// is left unfinished, the rematch swaps the colours, a rated game updates both names' local
// ratings, and scripted moves (--play, --play-then) are made as a player would make them. Every
// game is checked where it ends up: the game's log, the saved game in the App's folder, the
// settings. No engine is needed: these tests run in a build without Stockfish too.
#include "game_scene_testkit.h"

using namespace gstest;

namespace {

// A rated game between Alice and Bob from the command line, 5+0, the view flying over in 0.8 s.
const std::vector<std::string>& ratedArgs() {
    static const std::vector<std::string> args = {"--no-intro", "--start",  "--hotseat",  "--white-name", "Alice", "--black-name",
                                                  "Bob",        "--rated",  "--handover", "0.8",          "--tc",  "4"};
    return args;
}

// The view has reached 'name' (the handover landed) after 'from' in the log.
bool viewReaches(headless::App& app, LogTail& log, const std::string& name, size_t from) {
    return waitLog(app, log, "hot-seat: the view is in " + name + "'s eyes", from);
}

// The draw offer of the Esc menu (it goes with the move about to be completed).
bool offerDraw(headless::App& app) {
    app.key(plat::KEY_ESCAPE);
    if (!app.waitForLabel("Offer draw", 30) || !app.clickLabel("Offer draw")) return false;
    app.frames(10);
    return true;
}

// A move played by hand and completed with Space (a timed game), then the view gone over to the
// next player ('next'). Returns false when the move or the handover never came.
bool moveAndHandOver(headless::App& app, LogTail& log, chess::Game& g, const std::string& uci, const std::string& next) {
    const size_t from = log.mark();
    const chess::Move mv = g.position().parseUCI(uci);
    if (!mv.valid()) return false;
    const std::string san = g.position().toSAN(mv);
    playMove(app, g.position(), uci);
    app.frames(30);
    pressKey(app, ' ');
    if (!waitLog(app, log, ": " + san + " (", from) || !viewReaches(app, log, next, from)) return false;
    g.play(mv);
    return true;
}

// A new game recorded in the App's folder since 'before': the only one.
bool onlyNewGame(const std::string& folder, const std::set<std::string>& before, chess::pgn::Record& out) {
    const std::vector<chess::pgn::Record> games = newGames(folder, before);
    if (games.size() != 1) return false;
    out = games[0];
    return true;
}

}  // namespace

// A rated game, 5+0, with camera flights: a Space press held through the flight is ignored, a draw
// offered with a move and declined by touching a piece, another declined on its card, a third
// accepted. Saved with both names, rated for both (the two-player ratings in the settings file).
// The rematch swaps the colours (saved as the next names); its first player resigns at once,
// confirmed with their name: saved, not rated.
TEST(game_scene_hotseat_rated_game_drawn_by_agreement) {
    LogTail log;
    headless::App* app = headless::shared(ratedArgs());
    REQUIRE(app);
    SettingsGuard guard;
    game::Settings& s = game::settings();
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(waitLog(*app, log, "New game (hot-seat): Alice (White) vs Bob (Black), 5+0, rated"));
    app->frames(200);
    chess::Game g;

    // Alice: d2-d4, the clock pressed with Space, held down through the flight to Bob.
    playMove(*app, g.position(), "d2d4");
    app->frames(30);
    size_t mark = log.mark();
    plat::Input& in = fakeplat::next();
    in.keyDown[plat::KEY_SPACE] = in.keyPressed[plat::KEY_SPACE] = true;
    REQUIRE(waitLog(*app, log, "] move 1: d4 (White", mark));
    REQUIRE(log.has("hot-seat: handover to Bob (flight", mark));
    REQUIRE(viewReaches(*app, log, "Bob", mark));
    REQUIRE(g.play(g.position().parseUCI("d2d4")));
    // Still held: Bob's input waits for the key's release.
    clickAt(*app, piecePoint("g8", chess::Knight));
    app->frames(10);
    in.keyDown[plat::KEY_SPACE] = false;
    app->frames(10);

    // Bob: Nf6 with a draw offer; Alice declines it by touching her c-pawn, and offers one too.
    playMove(*app, g.position(), "g8f6");
    REQUIRE(offerDraw(*app));
    mark = log.mark();
    pressKey(*app, ' ');
    REQUIRE(waitLog(*app, log, "] move 2: Nf6 (Black", mark));
    REQUIRE(viewReaches(*app, log, "Alice", mark));
    REQUIRE(g.play(g.position().parseUCI("g8f6")));
    REQUIRE(app->waitForLabel("Decline", 60));
    app->frames(30);
    clickAt(*app, piecePoint("c2", chess::Pawn));
    app->frames(30);
    CHECK(!app->hasLabel("Decline"));
    REQUIRE(offerDraw(*app));
    clickAt(*app, squarePoint("c4"));
    app->frames(30);
    mark = log.mark();
    pressKey(*app, ' ');
    REQUIRE(waitLog(*app, log, "] move 3: c4 (White", mark));
    REQUIRE(viewReaches(*app, log, "Bob", mark));
    REQUIRE(g.play(g.position().parseUCI("c2c4")));

    // Bob declines on the card, plays e6; Alice offers again with Nc3, and Bob accepts.
    REQUIRE(app->waitForLabel("Decline", 60));
    app->frames(30);
    REQUIRE(app->clickLabel("Decline"));
    app->frames(10);
    REQUIRE(moveAndHandOver(*app, log, g, "e7e6", "Alice"));
    REQUIRE(offerDraw(*app));
    REQUIRE(moveAndHandOver(*app, log, g, "b1c3", "Bob"));
    REQUIRE(app->waitForLabel("Accept", 60));
    app->frames(30);
    REQUIRE(app->clickLabel("Accept"));
    REQUIRE(waitLog(*app, log, "Game over: \xC2\xBD-\xC2\xBD (Draw by agreement)"));
    CHECK(log.has("hot-seat: Bob accepts the draw offered by Alice"));
    CHECK(log.has("hot-seat Elo: Alice "));

    chess::pgn::Record r;
    REQUIRE(onlyNewGame(folder, before, r));
    CHECK_EQ(r.result, std::string("1/2-1/2"));
    CHECK_EQ(r.tag("White"), std::string("Alice"));
    CHECK_EQ(r.tag("Black"), std::string("Bob"));
    CHECK_EQ(r.tag("ScacelithMode"), std::string("hotseat"));
    CHECK_EQ(r.tag("ScacelithEnd"), std::string("agreement"));
    CHECK_EQ(r.tag("TimeControl"), std::string("300"));
    CHECK(sanOf(r) == g.sanMoves());
    // Both names rated: one draw each, in the settings file.
    const game::LocalPlayer* alice = s.findLocalPlayer("Alice");
    const game::LocalPlayer* bob = s.findLocalPlayer("Bob");
    REQUIRE(alice && bob);
    CHECK_EQ(alice->record.games, 1);
    CHECK_EQ(alice->record.draws, 1);
    CHECK_EQ(bob->record.draws, 1);
    const IniFile ini = savedIni();
    std::set<std::string> names;
    for (int n = 1; n <= 2; ++n) names.insert(ini.getString("local_player_" + std::to_string(n) + ".name"));
    CHECK(names == std::set<std::string>({"Alice", "Bob"}));
    CHECK_EQ(s.playerGames, 0);   // the rating against Stockfish never changes

    // The rematch: the colours swap, saved as the next game's names.
    REQUIRE(app->waitForLabel("Rematch", 1200));
    mark = log.mark();
    REQUIRE(app->clickLabel("Rematch"));
    REQUIRE(waitLog(*app, log, "New game (hot-seat): Bob (White) vs Alice (Black), 5+0, rated", mark));
    CHECK_EQ(s.hotseatNames[0], std::string("Bob"));
    CHECK_EQ(savedIni().getString("hotseat.white_name"), std::string("Bob"));
    app->frames(200);
    // Bob resigns before his first move, confirmed in his name: saved, not rated.
    const std::set<std::string> before2 = pgnFiles(folder);
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Resign", 30));
    REQUIRE(app->clickLabel("Resign"));
    REQUIRE(app->waitForLabel("Cancel", 30));
    REQUIRE(clickLast(*app, "Resign"));
    REQUIRE(waitLog(*app, log, "Game over: 0-1", mark));
    CHECK(log.has("hot-seat: game not rated (0 plies)", mark));
    chess::pgn::Record r2;
    REQUIRE(onlyNewGame(folder, before2, r2));
    CHECK_EQ(r2.result, std::string("0-1"));
    CHECK(r2.plies.empty());
    CHECK_EQ(r2.tag("White"), std::string("Bob"));
    CHECK_EQ(r2.tag("ScacelithEnd"), std::string("resignation"));
    CHECK_EQ(s.findLocalPlayer("Bob")->record.games, 1);
    REQUIRE(app->waitForLabel("Rematch", 1200));
    REQUIRE(toTitle(*app));
}

// A friendly game without a clock from the New Game page, the view cut through black: a move is
// completed as its piece is put down. Black gets up and sits back down, then touches his queen
// as soon as the view reaches him and mates (the fool's mate): saved as a friendly game.
TEST(game_scene_hotseat_friendly_fools_mate_from_the_menu) {
    LogTail log;
    headless::App* app = titleApp();
    REQUIRE(app);
    SettingsGuard guard;
    game::Settings& s = game::settings();
    s.opponent = 1;
    s.hotseatNames[0] = "Ann";
    s.hotseatNames[1] = "Ben";
    s.hotseatHands[0] = s.hotseatHands[1] = -1;
    s.hotseatRated = false;
    s.timeControlPreset = 0;
    s.handoverSeconds = 0.0f;
    s.showLegalMoves = true;
    s.saveGames = true;
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(app->clickLabel("New Game"));
    REQUIRE(app->waitForLabel("Start", 120));
    REQUIRE(app->clickLabel("Start"));
    REQUIRE(waitLog(*app, log, "New game (hot-seat): Ann (White) vs Ben (Black), Unlimited, friendly"));
    app->frames(200);
    chess::Game g;
    size_t mark = log.mark();
    playMove(*app, g.position(), "f2f3");
    REQUIRE(waitLog(*app, log, "] move 1: f3 (White", mark));
    REQUIRE(log.has("hot-seat: handover to Ben (cut", mark));
    REQUIRE(viewReaches(*app, log, "Ben", mark));
    REQUIRE(g.play(g.position().parseUCI("f2f3")));
    // Ben stands up (the arrow keys are the player to move's), then sits back down to play.
    mark = log.mark();
    pressKey(*app, plat::KEY_UP);
    app->frames(150);
    pressKey(*app, plat::KEY_DOWN);
    REQUIRE(waitLog(*app, log, "Stance: seat 1 asks for stance 0", mark));
    app->frames(240);
    playMove(*app, g.position(), "e7e5");
    REQUIRE(viewReaches(*app, log, "Ann", mark));
    REQUIRE(g.play(g.position().parseUCI("e7e5")));
    mark = log.mark();
    playMove(*app, g.position(), "g2g4");
    REQUIRE(viewReaches(*app, log, "Ben", mark));
    REQUIRE(g.play(g.position().parseUCI("g2g4")));
    // The queen touched at once, before Ben recorded Ann's move: written after his own.
    playMove(*app, g.position(), "d8h4");
    REQUIRE(waitLog(*app, log, "Game over: 0-1", mark));
    CHECK(log.has("] move 4: Qh4# (Black", mark));
    CHECK(!log.has("hot-seat Elo:", mark));
    chess::pgn::Record r;
    REQUIRE(onlyNewGame(folder, before, r));
    CHECK_EQ(r.result, std::string("0-1"));
    CHECK_EQ(r.tag("White"), std::string("Ann"));
    CHECK_EQ(r.tag("Black"), std::string("Ben"));
    CHECK_EQ(r.tag("ScacelithEnd"), std::string("checkmate"));
    CHECK_EQ(r.tag("TimeControl"), std::string("-"));
    CHECK_EQ(r.plies.size(), size_t(4));
    CHECK(s.localPlayers.empty());
    REQUIRE(app->waitForLabel("Rematch", 1200));
    REQUIRE(toTitle(*app));
}

// A flag falls in a hot-seat game (15 s, the second player never moves): the game is over on
// time, saved. The rematch is then left from the Esc menu after a move: an abandoned hot-seat
// game is saved unfinished ("*"), without a winner.
TEST(game_scene_hotseat_flag_fall_then_left_unfinished) {
    LogTail log;
    headless::App* app = titleApp();
    REQUIRE(app);
    SettingsGuard guard;
    game::Settings& s = game::settings();
    s.opponent = 1;
    s.hotseatNames[0] = "Ann";
    s.hotseatNames[1] = "Ben";
    s.hotseatRated = false;
    s.timeControlPreset = -1;
    s.customBaseSeconds = 15;
    s.customIncrementSeconds = 0;
    s.customDelaySeconds = 0;
    s.handoverSeconds = 0.8f;
    s.saveGames = true;
    const std::string folder = pgnFolder(*app);
    std::set<std::string> before = pgnFiles(folder);
    REQUIRE(app->clickLabel("New Game"));
    REQUIRE(app->waitForLabel("Start", 120));
    REQUIRE(app->clickLabel("Start"));
    REQUIRE(waitLog(*app, log, "New game (hot-seat): Ann (White) vs Ben (Black), Custom 0:15+0, friendly"));
    app->frames(200);
    chess::Game g;
    REQUIRE(moveAndHandOver(*app, log, g, "d2d4", "Ben"));
    REQUIRE(runFor(*app, [&] { return log.has("Game over: 1-0"); }, 120.0, 1000, 0.05f));
    chess::pgn::Record r;
    REQUIRE(onlyNewGame(folder, before, r));
    CHECK_EQ(r.result, std::string("1-0"));
    CHECK_EQ(r.tag("ScacelithEnd"), std::string("timeout"));
    CHECK_EQ(r.plies.size(), size_t(1));

    // The rematch: Ben has White now; Ann leaves on her turn.
    REQUIRE(app->waitForLabel("Rematch", 1200));
    size_t mark = log.mark();
    REQUIRE(app->clickLabel("Rematch"));
    REQUIRE(waitLog(*app, log, "New game (hot-seat): Ben (White) vs Ann (Black)", mark));
    app->frames(200);
    before = pgnFiles(folder);
    chess::Game g2;
    REQUIRE(moveAndHandOver(*app, log, g2, "g1f3", "Ann"));
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Main menu", 30));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->waitForLabel("Leave", 30));
    REQUIRE(clickLast(*app, "Leave"));
    REQUIRE(app->waitForLabel("New Game", 600));
    CHECK(!log.has("Game over", mark));
    chess::pgn::Record left;
    REQUIRE(onlyNewGame(folder, before, left));
    CHECK_EQ(left.result, std::string("*"));
    CHECK_EQ(left.tag("White"), std::string("Ben"));
    CHECK_EQ(left.tag("ScacelithEnd"), std::string(""));
    CHECK_EQ(left.plies.size(), size_t(1));
}

// Scripted moves (--play), as a player makes them: touched, carried, put down. A move that is not
// legal stops the script; then the Esc menu's choices of --play-then are made (an unknown one is
// skipped): Black resigns. The same name on both sides is never rated.
TEST(game_scene_hotseat_scripted_moves_then_resignation) {
    LogTail log;
    headless::App* app = headless::shared({"--no-intro", "--start", "--hotseat", "--white-name", "Ann", "--black-name", "Ann",
                                           "--rated", "--handover", "0", "--tc", "0", "--play", "e2e4,e7e5,g1f3,zz",
                                           "--play-then", "bogus,resign"});
    REQUIRE(app);
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(waitLog(*app, log, "New game (hot-seat): Ann (White) vs Ann (Black), Unlimited, rated"));
    REQUIRE(waitLog(*app, log, "Game over: 1-0", 0, 120.0, 6000));
    CHECK(log.has("] move 3: Nf3 (White"));
    CHECK(log.has("--play: 'zz' is not legal here"));
    CHECK(log.has("--play-then: 'bogus' is not resign, leave or takeback"));
    CHECK(log.has("--play-then: resign"));
    CHECK(log.has("hot-seat: game not rated (3 plies, same name twice)"));
    chess::pgn::Record r;
    REQUIRE(onlyNewGame(folder, before, r));
    CHECK_EQ(r.result, std::string("1-0"));
    CHECK_EQ(r.tag("ScacelithEnd"), std::string("resignation"));
    CHECK(sanOf(r) == std::vector<std::string>({"e4", "e5", "Nf3"}));
    headless::stopShared();   // the next file's tests start their own App
}
