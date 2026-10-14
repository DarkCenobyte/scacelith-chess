// The coach's takeback card in a game of level 1 (src/game/game_scene_coach.cpp: the HUD, the
// session's review of the player's move, the coach's hands taking the move back on the table):
// after 1.e4 d5 (set up with --moves), the player hangs the queen with 2.Qg4 (Bxg4), then the bishop
// with 2.Ba6 (Nxa6 or bxa6). The coach reviews each move with Stockfish's analyses, says what goes
// wrong (subtitles: the voice model folder is empty) and offers to take the move back. The card is
// answered by itself (--coach-auto-answer yes, for scripted runs), by Backspace and by touching a
// piece (the player plays on); the pause menu's "Take back my move" (--play-then takeback) takes
// back the player's move and the coach's reply. The review waits seconds of game time for the
// analyses, which Stockfish makes in real time: the frames are paced so that the game's time never
// runs ahead of the wall clock while the review is made (fast frames otherwise), and the scripted
// moves wait for Stockfish to have started (seconds on a slow machine).
#include "test.h"
#include "coach_mode_game_kit.h"
#include "game/game_archive.h"
#include "game/settings.h"
#include "game_scene_testkit.h"
#include <string>
#include <vector>

namespace {

// The takeback card shows its two buttons.
bool offerShown(const headless::App& app) { return app.hasLabel("Take back") && app.hasLabel("Play on"); }

}  // namespace

// --coach-auto-answer yes: the card answers itself after a moment. The queen goes back by the
// player's hand, 2.Nf3 instead (the coach replies), then the pause menu's takeback of both moves,
// and the game left from the pause menu (--play-then takeback,leave).
TEST(coach_mode_game_takeback_auto_answer_then_pause_menu_takeback) {
#if !defined(SCACELITH_HAS_STOCKFISH)
    SKIP("built without the embedded Stockfish: the takeback offer follows the engine's review of the move");
#endif
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(coachgame::gameArgs({"--coach-level", "1", "--coach-colour", "white", "--moves",
                                                               "e2e4,d7d5", "--play", "d1g4,g1f3", "--play-then",
                                                               "takeback,leave", "--coach-auto-answer", "yes"}));
    REQUIRE(app);
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): level 1, human plays White", 3000));
    // The review, the card; its answer after a moment of the game's time (no engine to wait for
    // then: fast frames): the queen taken back by hand.
    REQUIRE(coachgame::paced(*app, [&] { return offerShown(*app); }, 60.0));
    CHECK(log.has("] move 3: Qg4 (White"));
    REQUIRE(coachgame::skipping(*app, [&] { return log.has("coach: takeback offer accepted (--coach-auto-answer)"); }, 30.0, 0));
    REQUIRE(coachtest::waitLog(*app, log, "coach: takes back 1 plies (to move 2)", 200));
    REQUIRE(coachgame::skipping(*app, [&] { return log.has("] move 3: Nf3 (White"); }, 30.0));
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("] move 4: "); }, 30.0));
    // Both moves back from the pause menu, then the game left: the title page.
    REQUIRE(coachgame::skipping(*app, [&] { return log.has("--play-then: takeback"); }, 30.0));
    REQUIRE(coachtest::waitLog(*app, log, "coach: takes back 2 plies (to move 2)", 60));
    REQUIRE(coachgame::skipping(*app, [&] { return log.has("--play-then: leave"); }, 30.0));
    REQUIRE(app->runUntil([&] { return app->hasLabel("New Game"); }, 600, 1.0f / 20.0f));
    // Saved unfinished: the moves set up, the ones taken back gone.
    const std::vector<std::string> saved = coachtest::filesIn(app->folder().path + "pgn/", ".pgn");
    REQUIRE(saved.size() == 1u);
    const game::archive::LoadResult r = game::archive::loadFile(saved[0]);
    REQUIRE(r.ok);
    CHECK_EQ(r.record.result, std::string("*"));
    CHECK_EQ(gstest::sanOf(r.record), (std::vector<std::string>{"e4", "d5"}));
    CHECK(!log.has("voice model download started"));   // Space never met the voice model's prompt
}

// Answered by hand: Backspace takes 2.Qg4 back; after 2.Ba6 the coach offers again, and touching
// one of the player's pieces plays on: the coach then plays. The player resigns from the pause menu.
TEST(coach_mode_game_takeback_answered_by_backspace_then_by_a_touch) {
#if !defined(SCACELITH_HAS_STOCKFISH)
    SKIP("built without the embedded Stockfish: the takeback offer follows the engine's review of the move");
#endif
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(coachgame::gameArgs({"--coach-level", "1", "--coach-colour", "white", "--moves",
                                                               "e2e4,d7d5", "--play", "d1g4,f1a6"}));
    REQUIRE(app);
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): level 1, human plays White", 3000));
    REQUIRE(coachgame::paced(*app, [&] { return offerShown(*app); }, 60.0));
    CHECK(log.has("] move 3: Qg4 (White"));
    app->frames(30);   // the card's buttons answer once it is up
    app->key(plat::KEY_BACKSPACE);
    REQUIRE(coachtest::waitLog(*app, log, "coach: takeback offer accepted", 20));
    REQUIRE(coachtest::waitLog(*app, log, "coach: takes back 1 plies (to move 2)", 200));
    // 2.Ba6 (fast frames until then), the second review and offer (what the coach says before it
    // skipped).
    REQUIRE(coachgame::skipping(*app, [&] { return log.has("] move 3: Ba6 (White"); }, 30.0));
    REQUIRE(coachgame::paced(*app, [&] { return offerShown(*app); }, 60.0));
    app->frames(30);
    gstest::clickAt(*app, gstest::piecePoint("g1", chess::Knight));
    REQUIRE(coachtest::waitLog(*app, log, "coach: takeback offer declined (a piece touched)", 20));
    // The coach plays its move (the card gone); the player resigns.
    REQUIRE(coachgame::skipping(*app, [&] { return log.has("] move 4: "); }, 30.0));
    app->frames(30);
    CHECK(!offerShown(*app));
    REQUIRE(coachgame::openPause(*app));
    REQUIRE(app->clickLabel("Resign"));
    REQUIRE(app->waitForLabel("Cancel", 30));
    REQUIRE(coachtest::clickLast(*app, "Resign"));
    REQUIRE(coachtest::waitLog(*app, log, "Game over: 0-1 (Resignation)", 60));
    REQUIRE(coachgame::waitEndCard(*app));
    CHECK(!log.has("voice model download started"));
}
