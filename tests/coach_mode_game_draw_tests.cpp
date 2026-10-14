// Draws in a coach game (src/game/game_scene_coach.cpp: coachOfferDraw, coachEvaluateDraw and the
// answer in updateCoach; the coach's pause menu): positions set up on the command line (--moves),
// the player with White.
//   - 1.e4 e5 2.Nf3 Qg5?? 3.Nxg5: the coach is a queen down. The player offers a draw from the pause
//     menu with a piece in hand, then puts it down: the coach answers once its evaluation is in (of
//     the position after the player's move, should the move come first) and accepts.
//   - The knights out and back twice: the start position stands for the third time, the player
//     claims the draw from the pause menu.
// Both end with the coach's closing words, the handshake and the appraisal; a draw in the history.
#include "test.h"
#include "coach_mode_game_kit.h"
#include "game/game_archive.h"
#include "game/settings.h"
#include "game_scene_testkit.h"
#include <string>
#include <vector>

// The player offers a draw with the a-pawn in hand (the pause menu opens over the touched piece),
// then plays 4.a3: the coach, a queen down, accepts.
TEST(coach_mode_game_draw_offer_accepted_by_a_coach_a_queen_down) {
#if !defined(SCACELITH_HAS_STOCKFISH)
    SKIP("built without the embedded Stockfish: the coach answers a draw offer from the engine's evaluation");
#endif
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(coachgame::gameArgs(
        {"--coach-level", "1", "--coach-colour", "white", "--moves", "e2e4,e7e5,g1f3,d8g5,f3g5"}));
    REQUIRE(app);
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): level 1, human plays White", 3000));
    // The coach (Black) moves first; the player's turn then.
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("] move 6: "); }, 30.0));
    app->frames(40, 1.0f / 20.0f);
    gstest::clickAt(*app, gstest::piecePoint("a2", chess::Pawn));
    app->frames(6);
    gstest::pointAt(*app, gstest::squarePoint("a3"));
    const m::vec2 target = gstest::toScreen(*app, gstest::squarePoint("a3"));
    REQUIRE(coachgame::openPause(*app));
    REQUIRE(app->clickLabel("Offer draw"));
    // The piece put down at once: the answer waits for the move, then (the position changed) the
    // coach evaluates the new one.
    app->moveMouse(target.x, target.y);
    gstest::clickHere(*app);
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("coach: draw offer accepted ("); }, 30.0));
    REQUIRE(coachtest::waitLog(*app, log, "Game over: \xC2\xBD-\xC2\xBD (Draw by agreement)", 60));
    CHECK(log.has("] move 7: a3 (White"));
    REQUIRE(coachgame::waitEndCard(*app));
    const game::Settings& s = game::settings();
    REQUIRE(!s.coachHistory.empty());
    CHECK_EQ(s.coachHistory.back().level, 1);
    CHECK_EQ(s.coachHistory.back().result, 0);
    const std::vector<std::string> saved = coachtest::filesIn(app->folder().path + "pgn/", ".pgn");
    REQUIRE(saved.size() == 1u);
    const game::archive::LoadResult r = game::archive::loadFile(saved[0]);
    REQUIRE(r.ok);
    CHECK_EQ(r.record.result, std::string("1/2-1/2"));
    CHECK_EQ(r.record.plies.size(), size_t(7));
}

// The start position for the third time on the player's move: "Claim draw" in the pause menu ends
// the game at once.
TEST(coach_mode_game_draw_threefold_claimed) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(coachgame::gameArgs(
        {"--coach-level", "2", "--coach-colour", "white", "--moves", "g1f3,g8f6,f3g1,f6g8,g1f3,g8f6,f3g1,f6g8"}));
    REQUIRE(app);
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): level 2, human plays White", 3000));
    REQUIRE(app->runUntil([&] { return log.has("coach: subtitle \""); }, 600));
    REQUIRE(coachgame::openPause(*app));
    CHECK(!app->hasLabel("Take back my move"));   // the moves set up are on the scoresheets already
    REQUIRE(app->clickLabel("Claim draw"));
    REQUIRE(coachtest::waitLog(*app, log, "Game over: \xC2\xBD-\xC2\xBD (Threefold repetition (claimed))", 60));
    REQUIRE(coachgame::waitEndCard(*app));
    CHECK_EQ(game::settings().coachHistory.back().result, 0);
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("New Game"); }, 600, 1.0f / 20.0f));
}
