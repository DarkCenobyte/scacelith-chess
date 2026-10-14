// The Analysis mode started from the command line and from the end of a game just played, in the
// headless game (src/game/game_scene_analysis.cpp, the analysis branches of src/game/game_scene.cpp):
// --analyse <file> --game G --analysis-at P driven by --replay-keys (each key once the board is
// still: a step forward by the robot, a step back by hand, a position of the list, the ends, an
// unknown key, the comments, the arrows and the voice switched off, Play, a wait, Leave); a hot-seat
// game played to a mate (--play) and analysed from its folded game over card ("Analyse the game"),
// at its last position, then left for the title page; and a screenshot run (--shot), whose
// review's searches are short and waited for, and which neither reads nor writes the evaluation
// cache. What happens is read from the game's log (the position of each key, the positions set,
// the comments decided), the overlay's controls, the settings and the files of the App's folder.
#include "analysis_mode_support.h"

#include <cstdlib>

using namespace analysismode;
using coachtest::LogCapture;

namespace {

const headless::TempDir& files() {
    static headless::TempDir dir("analysis-command-line");
    return dir;
}

// Two games: a short one, then the Scholar's mate with the players' clocks and ratings (--game 2).
const char* const kTwoGames =
    "[Event \"Blitz\"]\n[Site \"Lyon\"]\n[Date \"2026.10.01\"]\n[Round \"2\"]\n[White \"Boris\"]\n[Black \"Anna\"]\n"
    "[Result \"1/2-1/2\"]\n\n1. Nf3 Nf6 1/2-1/2\n\n"
    "[Event \"Autumn Open\"]\n[Site \"Lyon\"]\n[Date \"2026.10.01\"]\n[Round \"3\"]\n[White \"Anna\"]\n[Black \"Boris\"]\n"
    "[Result \"1-0\"]\n[WhiteElo \"2350\"]\n[BlackElo \"?\"]\n[TimeControl \"180+2\"]\n\n"
    "1. e4 {[%clk 0:03:01]} e5 {[%clk 0:03:00]} 2. Qh5 {[%clk 0:02:58]} Nc6 {[%clk 0:02:59]} 3. Bc4 {[%clk 0:02:57]} "
    "Nf6 {[%clk 0:02:50]} 4. Qxf7# {[%clk 0:02:55]} 1-0\n";

// A mate in one from a set-up position.
const char* const kMateInOne =
    "[Event \"Study\"]\n[White \"Composer\"]\n[Black \"Solver\"]\n[Result \"1-0\"]\n[SetUp \"1\"]\n"
    "[FEN \"7k/8/6K1/8/8/8/8/1Q6 w - - 0 1\"]\n\n1. Qb8# 1-0\n";

// The position the log gives for the key 'k' of --replay-keys ("--replay-keys: L (at position 3)"),
// -1 when it was not pressed.
int keyPosition(const LogCapture& log, const std::string& k) {
    const std::string text = log.text(), needle = "--replay-keys: " + k + " (at position ";
    const size_t at = text.find(needle);
    return at == std::string::npos ? -1 : std::atoi(text.c_str() + at + needle.size());
}

}  // namespace

// --analyse <file> --game 2 --analysis-at 3 and its keys: forward (the robot plays 3.Bc4), back
// (by hand), a position (set at once), the start, an unknown key (skipped), the comments off, Play,
// a wait while it plays, the end, the arrows and the voice off, then Leave: the menu on the Saved
// games page. The toggles are saved in the settings file.
TEST(analysis_mode_command_line_scripted_keys) {
    LogCapture log;
    coachtest::VoiceFolder modelFolder;   // the model store on the empty folder, the default again at the end
    headless::stopShared();   // the keys are pressed once, from the start of the App
    coachtest::writeFile(files().file("two.pgn"), kTwoGames);
    const std::string ini = writeIni(files());   // after the App before (it saves its settings as it stops)
    headless::App* app = headless::shared({"--no-intro", "--ini", ini, "--coach-dir", coachtest::voiceFolder(), "--analyse",
                                           files().file("two.pgn"), "--game", "2", "--analysis-at", "3", "--replay-keys",
                                           "L,J,Goto:6,Home,X,N,K,Wait:4,End,B,M,Leave"});
    REQUIRE(app);
    Restore restore;
    REQUIRE(coachtest::waitLog(*app, log, "Analysis: Anna vs Boris, 7 plies, 1-0, from position 3", 30000, 1.0f / 60.0f));
    CHECK(log.has("replay: board set at ply 3: rnbqkbnr/pppp1ppp/8/4p2Q/4P3/8/PPPP1PPP/RNB1KBNR b KQkq - 1 2"));
    REQUIRE(app->runUntil([&] { return shown(*app, "play"); }, 120));
    CHECK(enabled(*app, "start"));
    CHECK(enabled(*app, "end"));
    REQUIRE(coachtest::waitLog(*app, log, "--replay-keys: Leave", 4000));
    REQUIRE(app->waitForLabel("Replay", 600));   // the Saved games page
    CHECK(!shown(*app, "play"));

    CHECK_EQ(keyPosition(log, "L"), 3);
    CHECK_EQ(keyPosition(log, "J"), 4);     // the robot's move
    CHECK_EQ(keyPosition(log, "Goto:6"), 3);  // taken back by hand
    CHECK(log.has("replay: board set at ply 6: "));
    CHECK_EQ(keyPosition(log, "Home"), 6);
    CHECK(log.has("replay: board set at ply 0: "));
    CHECK_EQ(keyPosition(log, "X"), 0);
    CHECK(log.has("--replay-keys: 'X' is not J, K, L, Home, End, N, M, B, Goto:N, Wait:S or Leave"));
    CHECK_EQ(keyPosition(log, "N"), 0);
    CHECK_EQ(keyPosition(log, "K"), 0);
    const int waited = keyPosition(log, "Wait:4"), ended = keyPosition(log, "End");
    CHECK(waited >= 1);                     // Play's first move at once
    CHECK(ended > waited);                  // and the next ones meanwhile
    CHECK_EQ(keyPosition(log, "B"), 7);
    CHECK_EQ(keyPosition(log, "M"), 7);
    CHECK_EQ(keyPosition(log, "Leave"), 7);
    CHECK(!log.has("analysis: comment at position"));   // opened at 3 (no welcome), then switched off

    const game::Settings& s = game::settings();
    CHECK(!s.analysisComments);
    CHECK(!s.analysisArrows);
    CHECK(!s.analysisVoice);
    IniFile saved;
    REQUIRE(saved.load(ini));
    CHECK(!saved.getBool("analysis.comments", true));
    CHECK(!saved.getBool("analysis.arrows", true));
    CHECK(!saved.getBool("analysis.voice", true));
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(gstest::toTitle(*app));
}

// A hot-seat game to a mate (1.f3 e5 2.g4 Qh4#), its game over card folded to look at the board,
// then "Analyse the game" on its bar: the game just played in the Analysis mode, at its last
// position, from White's side; the game was saved once, as it ended. Left from the Esc menu: the
// title page.
TEST(analysis_mode_command_line_game_just_played) {
    LogCapture log;
    coachtest::VoiceFolder modelFolder;
    headless::stopShared();
    const std::string ini = writeIni(files());
    headless::App* app = headless::shared({"--no-intro", "--ini", ini, "--coach-dir", coachtest::voiceFolder(), "--start",
                                           "--hotseat", "--white-name", "Ann", "--black-name", "Bob", "--rated", "--handover",
                                           "0", "--tc", "0", "--play", "f2f3,e7e5,g2g4,d8h4"});
    REQUIRE(app);
    Restore restore;
    REQUIRE(coachtest::waitLog(*app, log, "Game over: 0-1", 30000, 1.0f / 60.0f));
    REQUIRE(app->waitForLabel("View the board", 1200));
    const std::string pgn = gstest::pgnFolder(*app);
    REQUIRE(app->runUntil([&] { return coachtest::filesIn(pgn, ".pgn").size() == 1; }, 120));
    REQUIRE(app->clickLabel("View the board"));
    app->frames(60);   // folded: the bar under the board has the card's buttons
    REQUIRE(coachtest::clickLast(*app, "Analyse the game"));
    REQUIRE(coachtest::waitLog(*app, log, "analysis: the game just played (hotseat, 4 plies, 0-1)", 60));
    REQUIRE(coachtest::waitLog(*app, log, "Analysis: Ann vs Bob, 4 plies, 0-1, from position 4", 600));
    REQUIRE(app->runUntil([&] { return shown(*app, "play"); }, 600));
    CHECK(!enabled(*app, "end"));
    CHECK(!enabled(*app, "forward"));
    CHECK(enabled(*app, "back"));
    CHECK(!app->hasLabel("Rematch"));   // declined by the analysis
    const size_t mark = log.mark();
    REQUIRE(press(*app, "start"));
    REQUIRE(app->runUntil([&] { return log.since(mark).find("replay: board set at ply 0: ") != std::string::npos; }, 60));
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Main menu", 60));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->waitForLabel("New Game", 600));   // the title page
    CHECK_EQ(coachtest::filesIn(pgn, ".pgn").size(), size_t(1));
}

// A screenshot run of an analysis: everything loaded before the first frame, the review's short
// searches waited for (the welcome, then the end's comment with the accuracies, the review being
// complete), and neither the evaluation cache nor the settings file touched.
TEST(analysis_mode_command_line_screenshot_run) {
#if !defined(SCACELITH_HAS_STOCKFISH)
    SKIP("built without the embedded Stockfish: the review's searches and comments need the engine");
#endif
    LogCapture log;
    coachtest::VoiceFolder modelFolder;
    headless::stopShared();
    coachtest::writeFile(files().file("mate.pgn"), kMateInOne);
    const std::string ini = writeIni(files());
    const std::string iniBefore = coachtest::readFile(ini);
    headless::App* app = headless::shared({"--shot", files().file("shot.png"), "--ini", ini, "--coach-dir",
                                           coachtest::voiceFolder(), "--no-intro", "--analyse", files().file("mate.pgn"),
                                           "--warp", "2"});
    REQUIRE(app);
    REQUIRE(coachtest::waitLog(*app, log, "Analysis: Composer vs Solver, 1 plies, 1-0, from position 0", 600));
    REQUIRE(coachtest::waitLog(*app, log, "analysis: comment at position 0 (", 600));
    REQUIRE(app->runUntil([&] { return shown(*app, "play"); }, 600));
    // One move forward: the end's comment waits for the review of the position before it (the
    // engine's start and its searches take their own time), then comes with the accuracies.
    const size_t mark = log.mark();
    app->key('L');
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.since(mark).find("analysis: comment at position 1 (") != std::string::npos; },
                                  60.0));
    CHECK(log.since(mark).find("analysis: comment at position 1 (2 lines)") != std::string::npos);
    CHECK(!enabled(*app, "forward"));
    CHECK(coachtest::filesIn(analysisFolder(*app), ".txt").empty());
    app->stop();   // the settings are saved as the App stops: not in a screenshot run
    CHECK(coachtest::readFile(ini) == iniBefore);
    headless::stopShared();   // the next file's tests start their own App
}
