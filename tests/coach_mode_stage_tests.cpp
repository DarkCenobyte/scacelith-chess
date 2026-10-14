// Coach mode's stage test in the game scene (--coach-stage-test, GameScene::runStageTest in
// src/game/game_scene_coach.cpp): the coach's Stage performs a fixed sequence without the session
// (a line shown as a subtitle, forced on, with its pointing, its knight's trace and its marks; two
// demonstration moves played by the coach's hand on the table and taken back; the takeback card),
// then the Esc menu of a coach game without a session, resigning, the end card and "Play again".
// "--coach-stage-test lesson" sets up the rules lesson's first position behind a fade instead of
// the demonstrations; an unknown --coach-challenge falls back to a coach game. The voice folder is
// empty: the coach speaks through subtitles.
#include "test.h"
#include "coach_mode_support.h"
#include "game/settings.h"
#include <algorithm>
#include <string>
#include <vector>

namespace {

const std::vector<std::string>& stageArgs() {
    static const std::vector<std::string> args = {"--no-intro", "--start", "--coach", "--coach-stage-test", "--coach-dir",
                                                  coachtest::voiceFolder()};
    return args;
}

const std::vector<std::string>& lessonStageArgs() {
    static const std::vector<std::string> args = {"--no-intro", "--start", "--coach", "--coach-stage-test", "lesson",
                                                  "--coach-challenge", "nosuch", "--coach-dir", coachtest::voiceFolder()};
    return args;
}

// The voice model's download prompt (its "Not now" button). It never opens here: the voice is on
// from the start and its folder stays empty. Keys are pressed only while it is not shown, since
// its "Download" button would take Space or Enter.
bool modelPrompt(headless::App& app) { return app.hasLabel("Not now"); }

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

int count(const std::string& text, const std::string& needle) {
    int n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

// Frames until no item shows the label (a card or a menu fading out).
bool waitGone(headless::App& app, const std::string& label, int maxFrames = 120) {
    return app.runUntil([&] { return !app.hasLabel(label); }, maxFrames);
}

bool g_sequenceOver = false;   // the first test answered the stage test's card

}  // namespace

// The sequence: the line is shown (a subtitle: no voice), the coach demonstrates 1.e4 e5 on the
// table, which then shows the demonstrations' position while the game's stays the start position,
// and takes both moves back (Space hurries the rewind): the table shows the game's position again.
// The takeback card comes up; Backspace answers it.
TEST(coach_mode_stage_demonstrates_rewinds_and_offers_the_takeback) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(stageArgs());
    REQUIRE(app);
    // The world is built first (seconds of wall-clock time for the first App of the run).
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("New game (coach): level 1, human plays White, stage test"); }, 120.0));
    REQUIRE(coachtest::waitLog(*app, log, "coach stage test: step 5", 1200));
    REQUIRE(!modelPrompt(*app));
    CHECK(log.has("coach stage test: the session stays silent"));
    CHECK(log.has("request 0)"));   // no voice: nothing to synthesise
    CHECK(log.has("coach: subtitle \"I answer"));
    CHECK(log.has("coach: demonstrates e2e4 on the table"));
    CHECK(log.has("coach: demonstrates e7e5 on the table"));
    CHECK(log.has("table after the demonstration: rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2"));
    app->key(plat::KEY_SPACE);   // the rest of the rewind at the pace of play
    REQUIRE(app->runUntil([&] { return app->hasLabel("Take back"); }, 600, 1.0f / 20.0f));
    CHECK(log.has("rewound, the table shows the game's position"));
    CHECK(!log.has("does NOT show"));
    CHECK(app->hasLabel("Play on"));
    app->frames(20);
    app->key(plat::KEY_BACKSPACE);
    CHECK(log.has("coach: takeback offer accepted"));
    CHECK(waitGone(*app, "Take back"));
    CHECK(coachtest::waitLog(*app, log, "card answered", 60));
    CHECK(!log.has("move 1:"));   // nothing was played
    g_sequenceOver = true;
}

// The Esc menu of a coach game without a session: no takeback, the draw offer once per move,
// resigning (cancelled once, then confirmed). The end card of a game without moves has no
// "Analyse the game"; nothing goes to the coach's history. "Play again" runs the sequence again,
// whose card is declined with its button this time; then back to the title page.
TEST(coach_mode_stage_pause_menu_resign_and_play_again) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(stageArgs());
    REQUIRE(app);
    if (!g_sequenceOver) SKIP("follows coach_mode_stage_demonstrates_rewinds_and_offers_the_takeback");
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Resume", 30));
    std::vector<std::string> labels = app->labels();
    CHECK(has(labels, "Offer draw"));
    CHECK(has(labels, "Resign"));
    CHECK(has(labels, "Main menu"));
    CHECK(!has(labels, "Take back my move"));
    CHECK(!has(labels, "Claim draw"));

    // The draw offer: answered from an evaluation of the position, and not offered twice at the
    // same move.
    REQUIRE(app->clickLabel("Offer draw"));
    CHECK(waitGone(*app, "Resume"));
#if defined(SCACELITH_HAS_STOCKFISH)
    CHECK(coachtest::waitEngine(*app, [&] { return log.has("coach: draw offer declined"); }));
#endif
    app->frames(10);
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Resume", 30));
    CHECK(!app->hasLabel("Offer draw"));

    // Resign: the confirmation's own Resign button (the last item with that label).
    REQUIRE(app->clickLabel("Resign"));
    REQUIRE(app->waitForLabel("Cancel", 30));
    REQUIRE(coachtest::clickLast(*app, "Cancel"));
    CHECK(waitGone(*app, "Cancel"));
    CHECK(app->hasLabel("Resume"));
    REQUIRE(app->clickLabel("Resign"));
    REQUIRE(app->waitForLabel("Cancel", 30));
    REQUIRE(coachtest::clickLast(*app, "Resign"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("Play again"); }, 1200, 1.0f / 20.0f));
    labels = app->labels();
    CHECK(has(labels, "Main menu"));
    CHECK(has(labels, "View the board"));
    CHECK(!has(labels, "Analyse the game"));   // no move was played
    CHECK(game::settings().coachHistory.empty());   // the stage test records nothing
    // The resignation is saved as a coach game (a loss of the player's, without moves).
    const std::vector<std::string> saved = coachtest::filesIn(app->folder().path + "pgn/", ".pgn");
    REQUIRE(saved.size() == 1u);
    const std::string pgn = coachtest::readFile(saved[0]);
    CHECK(pgn.find("[Result \"0-1\"]") != std::string::npos);
    CHECK(pgn.find("[Black \"Coach\"]") != std::string::npos);

    // Folded away and shown again.
    REQUIRE(app->clickLabel("View the board"));
    app->frames(60);
    CHECK(!app->hasLabel("Play again"));
    REQUIRE(app->clickLabel("Show"));
    REQUIRE(app->waitForLabel("Play again", 60));
    app->frames(60);

    // Play again: the same level, the sequence once more; its card declined with its button.
    const size_t mark = log.mark();
    REQUIRE(app->clickLabel("Play again"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("Play on"); }, 2500, 1.0f / 20.0f));
    CHECK(log.since(mark).find("New game (coach): level 1, human plays White, stage test") != std::string::npos);
    app->frames(20);   // the card's buttons answer once it is readable
    REQUIRE(app->clickLabel("Play on"));
    CHECK(log.has("coach: takeback offer declined"));
    CHECK(waitGone(*app, "Play on"));

    // Leave: confirmed, to the title page.
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Main menu", 30));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->waitForLabel("Leave", 30));
    REQUIRE(coachtest::clickLast(*app, "Leave"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("New Game"); }, 600, 1.0f / 20.0f));
    CHECK_EQ(count(log.text(), "New game (coach)"), 1);
    CHECK(!log.has("voice model download"));
}

// "--coach-stage-test lesson": the rules lesson's first position is set up behind a fade, and the
// line is said on it; no demonstration follows. An unknown challenge on the command line (a typo,
// a newer .ini) gives a coach game instead.
TEST(coach_mode_stage_lesson_position_and_an_unknown_challenge) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(lessonStageArgs());
    REQUIRE(app);
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("New game (coach): level 1, human plays White, stage test"); }, 120.0));
    CHECK(log.has("coach: no challenge 'nosuch', a coach game instead"));
    CHECK(!log.has("the challenge 'nosuch'"));
    REQUIRE(coachtest::waitLog(*app, log, "lesson position shown", 1200));
    CHECK(log.has("coach stage test: lesson position 4k3/8/8/8/8/8/8/4K3 w - - 0 1"));
    CHECK(!log.has("demonstrates"));
    CHECK(!log.has("lesson position '"));   // the position was valid
    app->frames(40, 1.0f / 20.0f);
    CHECK(!log.has("step 5"));   // the sequence stops there
    CHECK(!app->hasLabel("Take back"));
}
