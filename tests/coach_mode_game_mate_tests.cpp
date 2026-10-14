// A coach game won by the player (src/game/game_scene_coach.cpp: coachMoveCompleted ending the
// game, coachGameOver, persistCoachResults and the level the coach suggests): the fool's mate the
// other way round, 1.f3 e5 2.g4 set up on the command line (--moves), the player with Black mates
// with 2...Qh4# (--play). "Play again" mates once more: two accurate wins at level 2, the coach
// suggests level 3 (the Coach page's choice next time), and the end card answered by the Space
// that skips the coach's last words keeps the game's results. "Analyse the game" opens the game
// just played in the Analysis mode.
#include "test.h"
#include "coach_mode_game_kit.h"
#include "game/settings.h"
#include "game_scene_testkit.h"
#include <string>
#include <vector>

namespace {

const std::vector<std::string>& mateArgs() {
    static const std::vector<std::string> args = coachgame::gameArgs(
        {"--coach-level", "2", "--coach-colour", "black", "--moves", "f2f3,e7e5,g2g4", "--play", "d8h4"});
    return args;
}

}  // namespace

// Checkmate on the board: the game ends with the player's move, the coach's closing words, the
// handshake, the appraisal; a win at level 2 in the history.
TEST(coach_mode_game_mate_by_the_player) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(mateArgs());
    REQUIRE(app);
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): level 2, human plays Black", 3000));
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("Game over: "); }, 30.0));
    CHECK(log.has("] move 4: Qh4# (Black"));
    CHECK(log.has("Game over: 0-1 (Checkmate)"));
    REQUIRE(coachgame::waitEndCard(*app));
    CHECK(log.has("coach: handshake done, the appraisal follows"));
    const game::Settings& s = game::settings();
    REQUIRE(s.coachHistory.size() == 1u);
    CHECK_EQ(s.coachHistory.back().level, 2);
    CHECK_EQ(s.coachHistory.back().result, 1);
    CHECK(app->hasLabel("Analyse the game"));
}

// A second win the same way, "Play again" answered with Enter (the card's default button: the
// keyboard's focus stays on it from one card to the next). The Space that skips the coach's
// appraisal brings the card up in that frame; the same key does not answer it, and the game just
// over is kept as soon as the card shows. Two accurate wins at level 2: the coach suggests level
// 3, the Coach page's choice from now on.
TEST(coach_mode_game_mate_again_card_waits_level_suggested) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    gstest::SettingsGuard guard;
    headless::App* app = headless::shared(mateArgs());
    REQUIRE(app);
    REQUIRE(coachgame::waitEndCard(*app));
    app->frames(30);
    game::Settings& s = game::settings();
    const size_t games = s.coachHistory.size();
    REQUIRE(games >= 1u);
    REQUIRE(s.coachHistory.back().result == 1);
    const size_t start = log.mark();
    auto after = [&](size_t mark, const char* needle) { return log.since(mark).find(needle) != std::string::npos; };
    app->key(plat::KEY_ENTER);
    REQUIRE(app->runUntil([&] { return after(start, "New game (coach): level 2, human plays Black"); }, 600, 1.0f / 20.0f));
    REQUIRE(coachtest::waitEngine(*app, [&] { return after(start, "Game over: 0-1 (Checkmate)"); }, 30.0));
    // Space while the appraisal's first line is said.
    REQUIRE(app->runUntil([&] { return after(start, "coach: handshake done, the appraisal follows"); }, 600, 1.0f / 20.0f));
    const size_t appraisal = log.mark();
    REQUIRE(app->runUntil([&] { return after(appraisal, "coach: subtitle \""); }, 600, 1.0f / 60.0f));
    app->frames(15);
    CHECK(!app->hasLabel("Play again"));
    app->key(plat::KEY_SPACE);
    REQUIRE(app->runUntil([&] { return app->hasLabel("Play again"); }, 600, 1.0f / 20.0f));
    REQUIRE(s.coachHistory.size() == games + 1);
    app->frames(10);
    CHECK(app->hasLabel("Play again"));
    CHECK(!after(appraisal, "New game (coach)"));
    app->key(plat::KEY_ENTER);
    REQUIRE(app->runUntil([&] { return after(appraisal, "New game (coach): level 2"); }, 600, 1.0f / 20.0f));
    CHECK_EQ(s.coachHistory.back().result, 1);
    CHECK(after(start, "coach: suggests level 3 (was 2)"));
    CHECK_EQ(s.coachLevel, 3);
    CHECK_EQ(gstest::savedIni().getInt("coach.level", -1), 3);
    // The third game ends the same way; its card waited for, "Analyse the game" opens it in the
    // Analysis mode (its voice not offered: switched off).
    REQUIRE(coachtest::waitEngine(*app, [&] { return after(appraisal, "Game over: 0-1 (Checkmate)"); }, 30.0));
    REQUIRE(coachgame::waitEndCard(*app));
    CHECK_EQ(s.coachHistory.size(), games + 2);
    s.coachVoice = false;
    app->frames(30);
    REQUIRE(app->clickLabel("Analyse the game"));
    REQUIRE(app->runUntil([&] { return after(appraisal, "analysis: the game just played (coach, 4 plies"); }, 600, 1.0f / 20.0f));
    REQUIRE(gstest::toTitle(*app));
    CHECK(!log.has("voice model download started"));
}
