// The replay of a saved game in the game scene (src/game/game_scene_replay.cpp, GameMode::Replay):
// the two robots play a record's moves again at the pace they were played, watched from the free
// camera. Played on the headless game (headless.h) from the command line (--replay, --game,
// --replay-speed, --replay-paused, --replay-keys) and from the title menu's "Saved Games" page,
// with the keys (K, J, L, Shift+J, Shift+L, Home, End), the buttons of the replay bar, the result
// card's "Replay again" and the Esc menu. The game's log says which moves were played and where
// the board was set; nothing is rated or saved. No engine is needed.
#include "game_scene_testkit.h"

using namespace gstest;

namespace {

// Two games in one file: a short one, then a game with clocks, ratings (one unknown), an event and
// a round, which ends in a mate (the 2nd game, --game 2).
const char* kTwoGames =
    "[Event \"Blitz\"]\n[Site \"Lyon\"]\n[Date \"2026.10.01\"]\n[Round \"2\"]\n[White \"Boris\"]\n[Black \"Anna\"]\n"
    "[Result \"1/2-1/2\"]\n\n1. Nf3 Nf6 1/2-1/2\n\n"
    "[Event \"Autumn Open\"]\n[Site \"Lyon\"]\n[Date \"2026.10.01\"]\n[Round \"3\"]\n[White \"Anna\"]\n[Black \"Boris\"]\n"
    "[Result \"1-0\"]\n[WhiteElo \"2350\"]\n[BlackElo \"?\"]\n[TimeControl \"180+2\"]\n\n"
    "1. e4 {[%clk 0:03:01]} e5 {[%clk 0:03:00]} 2. Qh5 {[%clk 0:02:58]} Nc6 {[%clk 0:02:59]} 3. Bc4 {[%clk 0:02:57]} "
    "Nf6 {[%clk 0:02:50]} 4. Qxf7# {[%clk 0:02:55]} 1-0\n";

// A game from a position with Black to move at move 40, without a clock or a date, unfinished.
const char* kFenGame =
    "[Event \"?\"]\n[White \"?\"]\n[Black \"Carla\"]\n[Result \"*\"]\n[SetUp \"1\"]\n"
    "[FEN \"8/8/8/4k3/8/8/4P3/4K3 b - - 0 40\"]\n\n40... Kd5 41. Kd2 Ke4 42. e3 *\n";

// A game as Scacelith saves it: clocks without a time control, the end told by its own tag.
const char* kSavedGame =
    "[Event \"Casual game\"]\n[Site \"Scacelith\"]\n[Date \"2026.09.30\"]\n[Round \"-\"]\n[White \"Human\"]\n"
    "[Black \"Stockfish\"]\n[Result \"0-1\"]\n[TimeControl \"?\"]\n[ScacelithMode \"play\"]\n"
    "[ScacelithEnd \"resignation\"]\n\n1. d4 {[%clk 0:04:58]} d5 {[%clk 0:04:59]} 0-1\n";

// The records of this file, in a folder that lives as long as the run (the shared App's
// arguments name them).
const headless::TempDir& recordsDir() {
    static headless::TempDir dir("replays");
    return dir;
}

std::string writeFile(const std::string& path, const std::string& text) {
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    return path;
}

std::string record(const std::string& name, const char* text) { return writeFile(recordsDir().file(name), text); }

// The five buttons of the replay bar, left to right (start, back, pause, forward, end): the only
// row of five square items without a label.
std::vector<ui::im::TracedItem> barButtons(headless::App& app) {
    std::vector<ui::im::TracedItem> squares;
    for (const ui::im::TracedItem& it : app.items())
        if (it.label.empty() && it.r.w > 10.0f && std::fabs(it.r.w - it.r.h) < 0.5f) squares.push_back(it);
    for (const ui::im::TracedItem& a : squares) {
        std::vector<ui::im::TracedItem> row;
        for (const ui::im::TracedItem& b : squares)
            if (std::fabs(b.r.y - a.r.y) < 0.5f && std::fabs(b.r.w - a.r.w) < 0.5f) row.push_back(b);
        if (row.size() == 5) {
            std::sort(row.begin(), row.end(), [](const ui::im::TracedItem& x, const ui::im::TracedItem& y) { return x.r.x < y.r.x; });
            return row;
        }
    }
    return {};
}

enum BarButton { BarStart, BarBack, BarPause, BarForward, BarEnd };

bool clickBar(headless::App& app, BarButton b) {
    const std::vector<ui::im::TracedItem> row = barButtons(app);
    if (row.size() != 5) return false;
    app.clickItem(row[size_t(b)]);
    app.frame();
    return true;
}

}  // namespace

// The 2nd game of a file at x8 with the keys: pause (K), a move back (J) and forward (L), slower
// and faster (Shift+J, Shift+L), the start (Home); then the bar's buttons: the end (the result
// card), the start from the folded card, pause, one move forward and back, the end again, and
// "Replay again" from the card. Left from the viewer's Esc menu: nothing was rated or saved.
TEST(game_scene_replay_keys_bar_and_replay_again) {
    LogTail log;
    const std::string path = record("two_games.pgn", kTwoGames);
    headless::App* app = headless::shared({"--no-intro", "--replay", path, "--game", "2", "--replay-speed", "x8"});
    REQUIRE(app);
    SettingsGuard guard;
    const int gamesBefore = game::settings().playerGames;
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(waitLog(*app, log, "Replay: Anna vs Boris, 2026.10.01, 7 plies, 1-0"));
    REQUIRE(waitLog(*app, log, "replay: ply 1 e4 at"));
    // The keys.
    pressKey(*app, 'K');
    size_t mark = log.mark();
    pressKey(*app, 'J');
    REQUIRE(waitLog(*app, log, "replay: board set at ply ", mark));
    app->frames(30);
    mark = log.mark();
    pressKey(*app, 'L');
    REQUIRE(waitLog(*app, log, "replay: ply ", mark));
    pressKey(*app, 'J', true);
    pressKey(*app, 'L', true);
    mark = log.mark();
    pressKey(*app, plat::KEY_HOME);
    REQUIRE(waitLog(*app, log, "replay: board set at ply 0: rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq", mark));
    pressKey(*app, 'K');
    REQUIRE(waitLog(*app, log, "replay: ply 1 e4 at", mark));

    // The bar: to the end, which shows the result card.
    app->frames(5);
    mark = log.mark();
    REQUIRE(clickBar(*app, BarEnd));
    REQUIRE(waitLog(*app, log, "replay: board set at ply 7", mark));
    REQUIRE(waitLog(*app, log, "Replay over: 1-0", mark));
    REQUIRE(app->waitForLabel("Replay again", 1200));
    // The card folded ("View the board"), the bar above it: back to the start, the replay goes
    // on; paused, one move forward and back.
    REQUIRE(app->clickLabel("View the board"));
    app->frames(30);
    mark = log.mark();
    REQUIRE(clickBar(*app, BarStart));
    REQUIRE(waitLog(*app, log, "replay: board set at ply 0", mark));
    app->frames(30);
    REQUIRE(clickBar(*app, BarPause));
    mark = log.mark();
    REQUIRE(clickBar(*app, BarForward));
    REQUIRE(waitLog(*app, log, "replay: ply 1 e4 at", mark));
    app->frames(60);
    mark = log.mark();
    REQUIRE(clickBar(*app, BarBack));
    REQUIRE(waitLog(*app, log, "replay: board set at ply 0", mark));
    app->frames(30);
    mark = log.mark();
    REQUIRE(clickBar(*app, BarEnd));
    REQUIRE(waitLog(*app, log, "Replay over: 1-0", mark));
    // "Replay again": from the start, at once.
    REQUIRE(app->waitForLabel("Replay again", 1200));
    app->frames(30);
    mark = log.mark();
    REQUIRE(app->clickLabel("Replay again"));
    REQUIRE(waitLog(*app, log, "replay: board set at ply 0", mark));
    REQUIRE(waitLog(*app, log, "replay: ply 1 e4 at", mark));
    // The viewer's Esc menu: back to the menu, on the saved games.
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Main menu", 30));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(toTitle(*app));
    CHECK(pgnFiles(folder) == before);
    CHECK_EQ(game::settings().playerGames, gamesBefore);
    CHECK(!log.has("Elo:"));
}

// A game from a FEN position (Black to move at move 40, no clock, unfinished), instant and paused
// from the start, driven by --replay-keys: forward and back, the start, an unknown key (skipped),
// the end (the card: no result), a move back from there (the replay goes on), then "Leave".
TEST(game_scene_replay_fen_game_scripted_keys) {
    LogTail log;
    const std::string path = record("fen_game.pgn", kFenGame);
    headless::App* app = headless::shared({"--no-intro", "--replay", path, "--replay-speed", "instant", "--replay-paused",
                                           "--replay-keys", "Shift+L,Shift+J,L,L,J,Home,X,End,K,J,Leave"});
    REQUIRE(app);
    REQUIRE(waitLog(*app, log, "Replay: ? vs Carla, ?, 4 plies, *"));
    REQUIRE(waitLog(*app, log, "--replay-keys: Leave", 0, 120.0, 20000));
    REQUIRE(app->waitForLabel("New Game", 600) || app->waitForLabel("Back", 600));
    CHECK(log.has("replay: ply 1 Kd5 at"));
    CHECK(log.has("replay: ply 2 Kd2 at"));
    CHECK(log.has("replay: board set at ply 1: "));
    CHECK(log.has("replay: board set at ply 0: 8/8/8/4k3/8/8/4P3/4K3 b - - 0 40"));
    CHECK(log.has("--replay-keys: 'X' is not K, J, L"));
    CHECK(log.has("replay: board set at ply 4: "));
    CHECK(log.has("Replay over: * ("));
    CHECK(log.has("replay: board set at ply 3: "));
    CHECK(!log.has("replay: ply 3 "));   // never played by the robots: set at once
    REQUIRE(toTitle(*app));
}

// A file that cannot be read (--replay): the menu instead. Then a game saved by Scacelith, put in
// the App's folder of saved games, replayed from the "Saved Games" page to its end (its clocks
// without a time control, its own reason of the end); back to the title page. Nothing is saved.
TEST(game_scene_replay_from_the_saved_games_page) {
    LogTail log;
    headless::stopShared();   // a fresh App: its folder of saved games holds only the game below
    headless::App* app = headless::shared({"--no-intro", "--replay", recordsDir().file("missing.pgn")});
    REQUIRE(app);
    REQUIRE(waitLog(*app, log, "--replay: no game to replay, to the menu"));
    CHECK(log.has("replay: game 1 of "));
    REQUIRE(app->waitForLabel("Saved Games", 30000));
    const std::string folder = pgnFolder(*app);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
    writeFile(folder + "saved.pgn", kSavedGame);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(app->clickLabel("Saved Games"));
    REQUIRE(app->waitForLabel("Replay", 600));
    app->frames(10);
    REQUIRE(clickLast(*app, "Replay"));
    REQUIRE(waitLog(*app, log, "library: replay saved.pgn, game 1"));
    REQUIRE(waitLog(*app, log, "Replay: Human vs Stockfish, 2026.09.30, 2 plies, 0-1"));
    REQUIRE(waitLog(*app, log, "Replay over: 0-1 (Resignation), 2 plies", 0, 120.0, 20000));
    REQUIRE(app->waitForLabel("Replay again", 1200));
    REQUIRE(toTitle(*app));
    CHECK(pgnFiles(folder) == before);
    headless::stopShared();   // the next file's tests start their own App
}
