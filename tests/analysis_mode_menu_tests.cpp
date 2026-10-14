// The Analysis mode opened from the title menu of the headless game (src/game/game_scene_analysis.cpp,
// the analysis branches of src/game/game_scene.cpp and of the Analysis and Saved games pages of
// src/ui/ui_library.cpp): a game of the command line that cannot be read (the menu instead), the
// Analysis page without saved games and its Paste a PGN (an empty clipboard, no game in it, two
// games: the first one analysed), a saved game whose review is in the evaluation cache (it opens
// complete: the commentator's welcome, Play to the end with the comments of the blunder and of the
// end, the steps forward by the robots and back by hand, queued or cut short by a jump, the moves
// of the list, the controls and the keys of the overlay, the toggles saved in the settings, the
// overlay hidden, the Esc menu's Options in another language), a game reviewed by the engine (the
// review saved in the cache when it completes, then found there when the game is opened again),
// and the Saved games page's Analyse (back to that page; a file gone meanwhile). What happens is
// read from the game's log (the positions set, the comments decided), the overlay's controls, the
// settings and the files of the App's folder.
#include "analysis_mode_support.h"

using namespace analysismode;
using coachtest::LogCapture;

namespace {

// The files of these tests for the whole run (the shared App's arguments name them): a file of two
// games, the settings.
const headless::TempDir& files() {
    static headless::TempDir dir("analysis-menu");
    return dir;
}

const char* const kTwoGames =
    "[Event \"Blitz\"]\n[White \"Boris\"]\n[Black \"Anna\"]\n[Result \"1/2-1/2\"]\n\n1. Nf3 Nf6 1/2-1/2\n\n"
    "[Event \"Blitz\"]\n[White \"Anna\"]\n[Black \"Boris\"]\n[Result \"1-0\"]\n\n1. e4 e5 2. Qf3 Nc6 1-0\n";

// The Scholar's mate, lost by the player of this PC with Black ("Human" is their default name).
const char* const kScholar =
    "[Event \"Casual game\"]\n[Site \"Lyon\"]\n[Date \"2026.10.02\"]\n[White \"Stockfish\"]\n[Black \"Human\"]\n"
    "[Result \"1-0\"]\n[TimeControl \"180+2\"]\n\n"
    "1. e4 {[%clk 0:03:01]} e5 {[%clk 0:03:00]} 2. Qh5 {[%clk 0:02:58]} Nc6 {[%clk 0:02:59]} "
    "3. Bc4 {[%clk 0:02:57]} Nf6 {[%clk 0:02:50]} 4. Qxf7# {[%clk 0:02:55]} 1-0\n";

// A mate in one from a set-up position: one position for the engine to review.
const char* const kMateFen = "7k/8/6K1/8/8/8/8/1Q6 w - - 0 1";
const char* const kMateInOne =
    "[Event \"Study\"]\n[White \"Composer\"]\n[Black \"Solver\"]\n[Result \"1-0\"]\n[SetUp \"1\"]\n"
    "[FEN \"7k/8/6K1/8/8/8/8/1Q6 w - - 0 1\"]\n\n1. Qb8# 1-0\n";

// The App of these tests: a game of the command line it cannot read (the 5th of a file of two),
// then the title menu.
const std::vector<std::string>& menuArgs() {
    static const std::vector<std::string> args = {"--no-intro", "--ini", files().file("analysis.ini"), "--coach-dir",
                                                  coachtest::voiceFolder(), "--analyse", files().file("two.pgn"),
                                                  "--game", "5"};
    return args;
}

// The shared App on its title page.
headless::App* titleApp() {
    coachtest::writeFile(files().file("two.pgn"), kTwoGames);
    writeIni(files());
    headless::App* app = headless::shared(menuArgs());
    // Loaded once a widget shows (the loading screen has none).
    if (!app || !app->runUntil([&] { return !app->items().empty(); }, 30000)) return nullptr;
    return gstest::toTitle(*app) ? app : nullptr;
}

// The saved games of the App's folder: only these.
std::string savedGames(headless::App& app, std::initializer_list<std::pair<const char*, const char*>> games) {
    const std::string folder = gstest::pgnFolder(app);
    makeFolder(folder);
    clearFolder(folder, ".pgn");
    for (const auto& g : games) coachtest::writeFile(folder + g.first, g.second);
    return folder;
}

bool waitEnabled(headless::App& app, const std::string& label, int frames) {
    return app.runUntil(
        [&] {
            const ui::im::TracedItem* it = coachtest::findLast(app, label);
            return it && !coachtest::disabled(it);
        },
        frames);
}

// The log says 'needle' after 'mark' (frames of 1/20 s, at most 'frames').
bool waitAfter(headless::App& app, const LogCapture& log, size_t mark, const std::string& needle, int frames = 600) {
    return app.runUntil([&] { return log.since(mark).find(needle) != std::string::npos; }, frames, 1.0f / 20.0f);
}

// Game time for a step of the robots (a move played, or taken back) to be over.
void settle(headless::App& app, int frames = 100) { app.frames(frames, 1.0f / 20.0f); }

// From the title page: the Analysis page, its only game analysed once its details are read.
bool analyseFromAnalysisPage(headless::App& app) {
    if (!app.clickLabel("Analysis") || !waitEnabled(app, "Analyse", 600)) return false;
    return coachtest::clickLast(app, "Analyse");
}

// The analysis left from the viewer's Esc menu ("Main menu"): the menu on the page it was opened
// from, whose button 'pageLabel' shows.
bool leaveAnalysis(headless::App& app, const std::string& pageLabel) {
    app.key(plat::KEY_ESCAPE);
    if (!app.waitForLabel("Main menu", 60) || !app.clickLabel("Main menu")) return false;
    return app.waitForLabel(pageLabel, 600);
}

}  // namespace

// --analyse names a game the file does not have: the title menu instead, the Analysis entry on it.
TEST(analysis_mode_menu_command_line_game_that_cannot_be_read) {
    LogCapture log;
    coachtest::VoiceFolder modelFolder;   // the model store on the empty folder, the default again at the end
    headless::stopShared();   // its start is what this test looks at
    headless::App* app = titleApp();
    REQUIRE(app);
    CHECK(log.has("replay: game 5 of "));
    CHECK(log.has("--analyse: no game to analyse, to the menu"));
    CHECK(!log.has("Analysis: "));
    CHECK(app->hasLabel("Analysis"));
    CHECK(!shown(*app, "play"));
}

// The Analysis page without saved games: nothing to analyse (Analyse greyed), the clipboard pasted:
// empty, then text without a game (nothing starts), then two games (the first one is analysed
// from its start, the record's own players). Back from it, the Analysis page again.
TEST(analysis_mode_menu_paste_a_pgn) {
    LogCapture log;
    coachtest::VoiceFolder modelFolder;
    headless::App* app = titleApp();
    REQUIRE(app);
    Restore restore;
    struct Clipboard {
        ~Clipboard() { fakeplat::record().clipboard.clear(); }
    } clipboard;
    savedGames(*app, {});
    REQUIRE(app->clickLabel("Analysis"));
    REQUIRE(app->waitForLabel("Paste a PGN", 60));
    app->frames(20);
    CHECK(coachtest::disabled(coachtest::findLast(*app, "Analyse")));
    CHECK(!app->hasLabel("My online games"));   // not signed in

    fakeplat::record().clipboard = " \n\t ";
    REQUIRE(app->clickLabel("Paste a PGN"));
    app->frames(10);
    CHECK(app->hasLabel("Paste a PGN"));   // still on the page
    const std::string noGame = "Not a chess game, only words.";
    fakeplat::record().clipboard = noGame;
    REQUIRE(app->clickLabel("Paste a PGN"));
    app->frames(10);
    CHECK(log.has("analysis: no game in the pasted text (" + std::to_string(noGame.size()) + " bytes"));
    CHECK(!log.has("Analysis: "));
    CHECK(app->hasLabel("Paste a PGN"));

    fakeplat::record().clipboard =
        "[White \"Pasted White\"]\n[Black \"Pasted Black\"]\n[Result \"0-1\"]\n\n1. f3 e5 2. g4 Qh4# 0-1\n\n"
        "[White \"Second\"]\n[Black \"Game\"]\n[Result \"*\"]\n\n1. d4 *\n";
    REQUIRE(app->clickLabel("Paste a PGN"));
    REQUIRE(coachtest::waitLog(*app, log, "Analysis: Pasted White vs Pasted Black, 4 plies, 0-1, from position 0", 600));
    CHECK(log.has("analysis: a pasted game of 4 plies ("));
    CHECK(log.has(", the first of several"));
    REQUIRE(app->runUntil([&] { return shown(*app, "play"); }, 600));
    CHECK(!enabled(*app, "start"));
    CHECK(enabled(*app, "end"));
    // Played to its end and left: back on the Analysis page, nothing saved.
    app->key(plat::KEY_END);
    settle(*app, 20);
    CHECK(log.has("replay: board set at ply 4: "));
    CHECK(!enabled(*app, "end"));
    REQUIRE(leaveAnalysis(*app, "Paste a PGN"));
    CHECK(coachtest::filesIn(gstest::pgnFolder(*app), ".pgn").empty());
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(gstest::toTitle(*app));
}

// A saved game whose review the cache holds (made up here: 3...Nf6?? allows the mate): it opens at
// its start with the review complete and the commentator's welcome. Play goes to the end by itself,
// the comments of the blunder and of the end said on the way; then the steps of the keys and of the
// overlay's controls, the moves of its list, the toggles (saved in the settings), the overlay
// hidden and shown, the Esc menu with its Options (another language applied), and back to the
// Analysis page. Nothing is saved but the settings.
TEST(analysis_mode_menu_saved_game_with_its_review_cached) {
    LogCapture log;
    coachtest::VoiceFolder modelFolder;
    headless::App* app = titleApp();
    REQUIRE(app);
    Restore restore;
    const std::string pgn = savedGames(*app, {{"scholar.pgn", kScholar}});
    clearFolder(analysisFolder(*app), ".txt");
    const std::string key = writeReview(analysisFolder(*app), reviewOf(nullptr, {"e4", "e5", "Qh5", "Nc6", "Bc4", "Nf6", "Qxf7#"}),
                                        {{{30, "e2e4 e7e5"}, {25, "d2d4"}},
                                         {{-30, "e7e5 g1f3"}, {-35, "c7c5"}},
                                         {{35, "g1f3 b8c6"}, {30, "f1c4"}},
                                         {{-20, "b8c6 f1c4"}, {-40, "g7g6"}},
                                         {{25, "f1c4 g7g6"}, {10, "d2d3"}},
                                         {{-30, "g7g6 h5f3"}, {-40, "d8e7"}},
                                         {{0, "h5f7", 1}, {200, "h5e5"}}});
    REQUIRE(!key.empty());

    REQUIRE(analyseFromAnalysisPage(*app));
    REQUIRE(coachtest::waitLog(*app, log, "Analysis: Stockfish vs Human, 7 plies, 1-0, from position 0", 600));
    CHECK(log.has("analysis: analyse scholar.pgn, game 1"));
    CHECK(log.has("analysis: evaluations of an earlier review restored (100% final)"));
    REQUIRE(coachtest::waitLog(*app, log, "analysis: comment at position 0 (", 200));
    REQUIRE(app->runUntil([&] { return shown(*app, "play"); }, 200));
    CHECK(!enabled(*app, "start"));
    CHECK(!enabled(*app, "back"));
    CHECK(enabled(*app, "play"));
    CHECK(enabled(*app, "forward"));
    CHECK(!enabled(*app, "voice"));   // no voice model: the comments are written only
    CHECK(enabled(*app, "comments"));
    CHECK(enabled(*app, "arrows"));
    CHECK(!app->hasLabel("Not now"));

    // Play: the moves one after another, the comment of the blunder and the end's on the way, then
    // it stops at the end.
    size_t mark = log.mark();
    app->key('K');
    REQUIRE(waitAfter(*app, log, mark, "analysis: comment at position 6 (", 2000));
    REQUIRE(waitAfter(*app, log, mark, "analysis: comment at position 7 (", 2000));
    CHECK(log.since(mark).find("comment at position 3 (") == std::string::npos);   // a quiet move
    REQUIRE(app->runUntil([&] { return !enabled(*app, "play"); }, 600, 1.0f / 20.0f));
    CHECK(!enabled(*app, "forward"));
    CHECK(!enabled(*app, "end"));
    CHECK(enabled(*app, "start"));

    // One move back by hand (the pawn taken comes back to f7), seen through the eyes of the player
    // to move; then forward again with the overlay's button: the end's comment again.
    app->key('0');
    app->key('J');
    settle(*app);
    CHECK(enabled(*app, "forward"));
    mark = log.mark();
    REQUIRE(press(*app, "forward"));
    REQUIRE(waitAfter(*app, log, mark, "analysis: comment at position 7 (", 600));
    app->key('2');   // above the board

    // The start and the end, set at once; a move of the list: the position after it.
    mark = log.mark();
    REQUIRE(press(*app, "start"));
    REQUIRE(waitAfter(*app, log, mark, "replay: board set at ply 0: rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq", 60));
    app->frames(5);
    CHECK(!enabled(*app, "start"));
    mark = log.mark();
    REQUIRE(press(*app, "end"));
    REQUIRE(waitAfter(*app, log, mark, "replay: board set at ply 7: ", 60));
    mark = log.mark();
    REQUIRE(coachtest::clickId(*app, cell(3)));   // 2...Nc6
    REQUIRE(waitAfter(*app, log, mark, "replay: board set at ply 4: r1bqkbnr/pppp1ppp/2n5/4p2Q/4P3/8/PPPP1PPP/RNB1KBNR w KQkq", 60));
    // Back and forward with the buttons: by hand, then by the robot (no comment on 3.Bc4).
    REQUIRE(press(*app, "back"));
    settle(*app);
    REQUIRE(press(*app, "forward"));
    settle(*app);
    // Two steps asked for at once: the second follows the first, to the blunder.
    mark = log.mark();
    app->key('L');
    app->key('L');
    REQUIRE(waitAfter(*app, log, mark, "analysis: comment at position 6 (", 600));
    CHECK(log.since(mark).find("replay: board set at ply") == std::string::npos);   // played, not set
    // A step back cut short by a jump to the start; then a step forward cut short by the end.
    mark = log.mark();
    app->key('J');
    app->key(plat::KEY_HOME);
    REQUIRE(waitAfter(*app, log, mark, "replay: board set at ply 0: ", 60));
    mark = log.mark();
    REQUIRE(press(*app, "play"));
    app->frames(4, 1.0f / 20.0f);   // the first move under way
    app->key(plat::KEY_END);
    REQUIRE(waitAfter(*app, log, mark, "replay: board set at ply 7: ", 60));
    settle(*app, 40);
    CHECK(!enabled(*app, "play"));   // stopped by the jump, at the end

    // The toggles, by key and by button, saved at once; the voice's button is greyed (no model).
    game::Settings& s = game::settings();
    const bool comments = s.analysisComments, arrows = s.analysisArrows, voice = s.analysisVoice;
    app->key('N');
    CHECK(s.analysisComments == !comments);
    REQUIRE(press(*app, "comments"));
    CHECK(s.analysisComments == comments);
    app->key('B');
    CHECK(s.analysisArrows == !arrows);
    app->key('M');
    CHECK(s.analysisVoice == !voice);
    REQUIRE(press(*app, "voice"));
    CHECK(s.analysisVoice == !voice);   // greyed: nothing
    IniFile ini = gstest::savedIni();
    CHECK(ini.getBool("analysis.arrows", arrows) == !arrows);
    CHECK(ini.getBool("analysis.voice", voice) == !voice);
    CHECK(ini.getBool("analysis.comments", !comments) == comments);
    REQUIRE(press(*app, "arrows"));
    app->key('M');
    CHECK(s.analysisArrows == arrows);
    CHECK(s.analysisVoice == voice);
    // The comments off: a step forward says nothing (and Play does not wait for comments).
    app->key('N');
    mark = log.mark();
    app->key(plat::KEY_HOME);
    settle(*app, 10);
    app->key('L');
    settle(*app);
    CHECK(log.since(mark).find("analysis: comment at position") == std::string::npos);
    app->key('N');

    // H hides the overlay with the viewer's, and shows it again.
    app->key('H');
    app->frames(2);
    CHECK(!shown(*app, "play"));
    app->key('H');
    app->frames(2);
    CHECK(shown(*app, "play"));

    // The Esc menu: the analysis waits behind it; its Options apply another language (and a
    // display option) to the commentator.
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Resume", 60));
    settle(*app, 20);
    CHECK(!shown(*app, "play"));
    REQUIRE(app->clickLabel("Options"));
    REQUIRE(app->waitForLabel("Apply", 60));
    REQUIRE(app->clickLabel("Vertical sync"));
    const ui::im::TracedItem* language = app->find("Language");
    REQUIRE(language);
    const ui::gfx::Rect row = language->r;
    app->moveMouse((row.x + 14.0f) * ui::gfx::scale(), row.cy() * ui::gfx::scale());   // the row takes the focus
    app->key(plat::KEY_RIGHT);
    app->frames(2);
    CHECK(i18n::language() != std::string("en"));
    REQUIRE(app->clickLabel(i18n::tr("common.apply")));
    REQUIRE(app->clickLabel(i18n::tr("common.back")));
    REQUIRE(app->waitForLabel(i18n::tr("viewer.resume"), 60));
    REQUIRE(app->clickLabel(i18n::tr("viewer.resume")));
    REQUIRE(app->runUntil([&] { return shown(*app, "play"); }, 60));
    i18n::setLanguage("en");
    s.language = restore.settings.language;

    // Left: back on the Analysis page; the saved games and the cache as they were.
    REQUIRE(leaveAnalysis(*app, "Paste a PGN"));
    CHECK_EQ(coachtest::filesIn(pgn, ".pgn").size(), size_t(1));
    CHECK(coachtest::filesIn(analysisFolder(*app), ".txt") ==
          std::vector<std::string>({std::filesystem::u8path(analysis::cachePath(analysisFolder(*app), key)).u8string()}));
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(gstest::toTitle(*app));
}

// A game the engine reviews (one position: a mate in one): the review is saved in the cache as it
// completes; the step to the end then says how the game ended with each side's accuracy. Opened
// again, the game finds its review in the cache.
TEST(analysis_mode_menu_reviewed_by_the_engine_then_found_in_the_cache) {
    LogCapture log;
    coachtest::VoiceFolder modelFolder;
    headless::App* app = titleApp();
    REQUIRE(app);
    Restore restore;
    savedGames(*app, {{"mate.pgn", kMateInOne}});
    clearFolder(analysisFolder(*app), ".txt");
    const std::string cache = analysis::cachePath(analysisFolder(*app), reviewOf(kMateFen, {"Qb8#"}).key());

    REQUIRE(analyseFromAnalysisPage(*app));
    REQUIRE(coachtest::waitLog(*app, log, "Analysis: Composer vs Solver, 1 plies, 1-0, from position 0", 600));
    CHECK(!log.has("evaluations of an earlier review restored"));
    REQUIRE(coachtest::waitEngine(*app, [&] { return !coachtest::filesIn(analysisFolder(*app), ".txt").empty(); }, 60.0));
    CHECK(coachtest::filesIn(analysisFolder(*app), ".txt") ==
          std::vector<std::string>({std::filesystem::u8path(cache).u8string()}));
    CHECK(coachtest::readFile(cache).find("e 0 ") != std::string::npos);
    REQUIRE(coachtest::waitLog(*app, log, "analysis: comment at position 0 (", 600));
    settle(*app, 60);
    const size_t mark = log.mark();
    app->key('L');
    REQUIRE(waitAfter(*app, log, mark, "analysis: comment at position 1 (", 600));
    REQUIRE(leaveAnalysis(*app, "Paste a PGN"));

    // Opened again: complete at once.
    REQUIRE(waitEnabled(*app, "Analyse", 600));
    REQUIRE(coachtest::clickLast(*app, "Analyse"));
    REQUIRE(waitAfter(*app, log, mark, "analysis: evaluations of an earlier review restored (100% final)", 600));
    REQUIRE(app->runUntil([&] { return shown(*app, "play"); }, 600));
    REQUIRE(leaveAnalysis(*app, "Paste a PGN"));
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(gstest::toTitle(*app));
}

// The Saved games page's Analyse: the game in the Analysis mode, and back to that page. A file
// removed after the page read it: Analyse finds no game, the page stays.
TEST(analysis_mode_menu_from_the_saved_games_page) {
    LogCapture log;
    coachtest::VoiceFolder modelFolder;
    headless::App* app = titleApp();
    REQUIRE(app);
    Restore restore;
    const std::string folder = savedGames(*app, {{"club.pgn", kTwoGames}});
    REQUIRE(app->clickLabel("Saved Games"));
    REQUIRE(waitEnabled(*app, "Analyse", 600));
    REQUIRE(coachtest::clickLast(*app, "Analyse"));
    REQUIRE(coachtest::waitLog(*app, log, "library: analyse club.pgn, game ", 60));
    REQUIRE(coachtest::waitLog(*app, log, "Analysis: ", 600));
    REQUIRE(app->runUntil([&] { return shown(*app, "play"); }, 600));
    REQUIRE(leaveAnalysis(*app, "Replay"));

    REQUIRE(waitEnabled(*app, "Analyse", 600));
    clearFolder(folder, ".pgn");
    const size_t mark = log.mark();
    REQUIRE(coachtest::clickLast(*app, "Analyse"));
    REQUIRE(waitAfter(*app, log, mark, "cannot be read", 60));
    app->frames(30);
    CHECK(log.since(mark).find("Analysis: ") == std::string::npos);
    CHECK(app->hasLabel("Replay"));   // the Saved games page again
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(gstest::toTitle(*app));
}
