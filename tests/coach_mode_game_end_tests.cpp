// The end of a coach game in the game scene (src/game/game_scene_coach.cpp, the coach branches of
// src/game/game_scene.cpp): a game of level 4 started from the command line, the player with
// Black (--coach-colour black) answers the coach's first move by hand (--play g8f6) and resigns
// from the Esc menu (--play-then resign). The coach's closing words, the handshake, its appraisal,
// the end card of a coach game (folded and unfolded), the results kept in the [coach] history and
// the game in the saved games; "Play again" at the same level and "Main menu". The voice model
// folder is empty: the coach speaks through subtitles.
#include "test.h"
#include "coach_mode_game_kit.h"
#include "game/game_archive.h"
#include "game/settings.h"
#include "game_scene_testkit.h"
#include "ui/ui.h"
#include <algorithm>
#include <string>
#include <vector>

namespace {

const std::vector<std::string>& resignArgs() {
    static const std::vector<std::string> args =
        coachgame::gameArgs({"--coach-level", "4", "--coach-colour", "black", "--play", "g8f6", "--play-then", "resign"});
    return args;
}

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

}  // namespace

// The coach opens from its repertoire, the player answers 1...Nf6 and resigns after the coach's
// second move. The closing words, the handshake and the appraisal come before the end card, which
// offers "Play again" (a coach game is not rated); the loss is kept in the coach's history and the
// game saved with its level.
TEST(coach_mode_game_end_resigned_then_appraised_and_saved) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(resignArgs());
    REQUIRE(app);
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): level 4, human plays Black", 3000));
    game::Settings& s = game::settings();
    const int nextColour = s.coachNextColour;
    // The coach (White) plays first from its teaching repertoire, without a search.
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("] move 1: "); }, 30.0));
    CHECK(log.has("coach: repertoire move "));
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("] move 2: Nf6 (Black"); }, 30.0));
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("Game over: "); }, 30.0));
    CHECK(log.has("--play-then: resign"));
    CHECK(log.has("Game over: 1-0 (Resignation)"));
    CHECK(log.has("[Event \"Coach level 4 (1650)\"]"));   // the coach's level and rating in the log's PGN
    CHECK(log.has("[White \"Coach\"]"));
    // The closing words (subtitles), the handshake, the appraisal, then the end card.
    REQUIRE(coachgame::waitEndCard(*app));
    CHECK(log.has("coach: subtitle \""));
    CHECK(log.has("coach: handshake done, the appraisal follows"));
    CHECK(log.has("coach: results saved (1 games in the history)"));
    const std::vector<std::string> labels = app->labels();
    CHECK(has(labels, "Main menu"));
    CHECK(has(labels, "Analyse the game"));   // the game has moves
    CHECK(!has(labels, "Rematch"));
    // The loss in the coach's history; the colour given on the command line does not alternate.
    REQUIRE(s.coachHistory.size() == 1u);
    CHECK_EQ(s.coachHistory.back().level, 4);
    CHECK_EQ(s.coachHistory.back().result, -1);
    CHECK_EQ(s.coachNextColour, nextColour);
    const IniFile ini = gstest::savedIni();
    CHECK_EQ(ini.getString("coach.history").compare(0, 5, "4:-1:"), 0);
    // Saved with the coach's level: the coach has White, the player resigned.
    const std::vector<std::string> saved = coachtest::filesIn(app->folder().path + "pgn/", ".pgn");
    REQUIRE(saved.size() == 1u);
    const game::archive::LoadResult r = game::archive::loadFile(saved[0]);
    REQUIRE(r.ok);
    CHECK_EQ(r.record.result, std::string("1-0"));
    CHECK_EQ(r.record.tag("CoachLevel"), std::string("4"));
    CHECK_EQ(r.record.tag("White"), std::string("Coach"));
    REQUIRE(r.record.plies.size() == 3u);   // the resignation on the player's turn, after the coach's reply
    CHECK_EQ(r.record.plies[1].san, std::string("Nf6"));

    // "View the board" folds the card (the subtitles then sit above its bar), Tab shows the moves.
    REQUIRE(app->clickLabel("View the board"));
    app->frames(20);
    CHECK(ui::gameOverFolded());
    CHECK(app->hasLabel("Show"));
    app->key(plat::KEY_TAB);
    app->frames(5);
    app->key(plat::KEY_TAB);
    REQUIRE(app->clickLabel("Show"));
    app->frames(30);   // the card unfolds (its buttons answer once it is up)
    CHECK(!ui::gameOverFolded());
    CHECK(app->hasLabel("Play again"));
}

// "Play again": the same level and colour, a new game (played and resigned the same way by the
// script); the history keeps the last games only. "Main menu" then leaves for the title page.
TEST(coach_mode_game_end_play_again_then_main_menu) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(resignArgs());
    REQUIRE(app);
    gstest::SettingsGuard guard;
    REQUIRE(coachgame::waitEndCard(*app));
    app->frames(30);
    game::Settings& s = game::settings();
    // A long history: the oldest games go once the new one is added.
    s.coachHistory.assign(size_t(game::Settings::kCoachHistoryMax), game::Settings::CoachGame{2, 1, 80.0});
    REQUIRE(app->clickLabel("Play again"));
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): level 4, human plays Black", 600));
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("--play-then: resign"); }, 30.0));
    REQUIRE(coachgame::waitEndCard(*app));
    CHECK(log.has("coach: results saved (40 games in the history)"));
    REQUIRE(s.coachHistory.size() == size_t(game::Settings::kCoachHistoryMax));
    CHECK_EQ(s.coachHistory.back().level, 4);
    CHECK_EQ(s.coachHistory.back().result, -1);
    CHECK_EQ(s.coachHistory.front().level, 2);
    CHECK_EQ(coachtest::filesIn(app->folder().path + "pgn/", ".pgn").size(), size_t(2));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("New Game") && app->hasLabel("Coach"); }, 600, 1.0f / 20.0f));
}
