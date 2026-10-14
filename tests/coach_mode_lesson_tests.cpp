// The rules lesson (Coach mode, level 0) in the game scene: src/game/game_scene_coach.cpp (the
// lesson's set-ups behind a fade, the coach's scripted moves, its demonstrations on the table and
// their rewinds, the wrong moves taken back by hand, the hold while the window is in the
// background, the idle hints, an illegal placement explained, endLesson and the lesson's end card,
// the chapter kept when the lesson is left), src/game/game_scene.cpp (the LessonWait turns, the end
// card's "First game") and, through them, src/coach/session.cpp and lesson.cpp. The voice folder is
// empty: the coach speaks through subtitles.
//
// The whole lesson is played once from its first chapter by --play: every exercise solved, after
// wrong moves where the coach answers with a demonstration (a capture, a king's escape, the
// solution itself after the third wrong move: en passant, a promotion) or with words only. Then
// the end card's "First game". A second App resumes the lesson where a player left it ([coach]
// lesson_chapter): the idle hints, an illegal move refused and explained, the mate played by hand,
// the lesson left during its last chapter.
#include "test.h"
#include "coach_mode_support.h"
#include "game_scene_testkit.h"
#include "core/ini.h"
#include "game/settings.h"
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace {

// The player's moves, chapter by chapter (src/coach/lesson.cpp builds the positions). A wrong move
// is taken back by the coach's hand before the next one is played.
const char* kLessonMoves =
    "e1d1,e1e2,"                                             // 0 the board: a wrong king move first
    "a1a3,a1a6,f1b5,d1h5,e1e2,g1f3,e2e4,e4d5,e2e4,g1f3,"     // 1 the pieces (the rook falls short first)
    "d1d5,d1a4,"                                             // 2 safety: the queen taken by the pawn, shown
    "e1d1,e1d1,d1e2,d1e2,c3e2,"                              // 3 check: the queen's block accepted at the 2nd try
    "a1a8,h7h5,h7e7,a7a8,b1b8,"                              // 4 mate: a check the king escapes, shown
    "e1f1,e1g1,e1c1,"                                        // 5 castling
    "e1d1,e1d1,e1d1,e5d6,"                                   // 6 en passant, shown after three wrong moves
    "e7e8r,e1d1,e1d1,e7e8q,"                                 // 7 promotion: a rook first, then shown
    "f2f7,f2f8";                                             // 8 stalemate, then the mate

constexpr int kWrongMoves = 14;   // the wrong moves above: each one taken back once

const std::string& lessonIni() {
    static headless::TempDir dir("coach-lesson-ini");
    static const std::string path = [] {
        const std::string p = dir.file("Scacelith.ini");
        coachtest::writeFile(p, "[graphics]\nbrightness_calibrated = true\n");
        return p;
    }();
    return path;
}

const std::vector<std::string>& lessonArgs() {
    static const std::vector<std::string> args = {"--no-intro", "--start", "--coach", "--coach-level", "0", "--play",
                                                  kLessonMoves, "--ini", lessonIni(), "--coach-dir", coachtest::voiceFolder()};
    return args;
}

// A player who left the lesson at its chapter "draws" (8). The script's only move fits no exercise
// of that chapter: refused when it is tried, it marks the moment the first exercise waits.
const std::string& resumeIni() {
    static headless::TempDir dir("coach-lesson-resume-ini");
    static const std::string path = [] {
        const std::string p = dir.file("Scacelith.ini");
        coachtest::writeFile(p, "[graphics]\nbrightness_calibrated = true\n[coach]\nlesson_chapter = 8\n");
        return p;
    }();
    return path;
}

const std::vector<std::string>& resumeArgs() {
    static const std::vector<std::string> args = {"--no-intro", "--start", "--coach", "--coach-level", "0", "--play",
                                                  "e1d1", "--ini", resumeIni(), "--coach-dir", coachtest::voiceFolder()};
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

// The lines the coach showed in this text of the log (an empty subtitle clears the last one).
int subtitles(const std::string& text) { return count(text, "coach: subtitle \"") - count(text, "coach: subtitle \"\""); }

// Frames of 'dt' until 'done', Space pressed every 'every' frames (Space skips the line being said;
// the lesson's exercises, demonstrations and rewinds stay).
bool runSkipping(headless::App& app, const std::function<bool()>& done, int maxFrames, int every = 6, float dt = 0.1f) {
    for (int i = 0; i < maxFrames; ++i) {
        if (done()) return true;
        if (modelPrompt(app)) return false;
        if (every > 0 && i % every == every - 1) fakeplat::next().keyPressed[plat::KEY_SPACE] = true;
        if (!app.frame(dt)) return done();
    }
    return done();
}

// Esc, then "Main menu" and its confirmation: back to the title page.
bool leaveToTitle(headless::App& app) {
    app.key(plat::KEY_ESCAPE);
    if (!app.waitForLabel("Main menu", 60)) return false;
    if (!app.clickLabel("Main menu") || !app.waitForLabel("Leave", 60)) return false;
    if (!coachtest::clickLast(app, "Leave")) return false;
    return app.runUntil([&] { return app.hasLabel("New Game"); }, 600, 1.0f / 20.0f);
}

bool g_lessonDone = false;    // the first test reached the lesson's end card

}  // namespace

// The lesson from its first chapter to its end card. The scene sets up each position behind a
// fade, plays the coach's scripted moves by hand (LessonWait, then the player's turn), lets the
// player move only while an exercise waits, and performs every demonstration on the table and
// puts it back; each wrong move is taken back by hand, and the coach's solution is never shown on
// a board the player has changed meanwhile. The coach holds while the window is in the background.
// Completed, the lesson is remembered (rules done, the next game is level 1's, the lesson starts
// over next time) and never saved as a game.
TEST(coach_mode_lesson_played_to_its_end_card) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(lessonArgs());
    REQUIRE(app);
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): level 0 (the rules lesson), human plays White", 3000));
    REQUIRE(!modelPrompt(*app));
    CHECK(!game::settings().coachRulesDone);

    // The window in the background while the coach greets: it holds (no new line), then goes on.
    REQUIRE(app->runUntil([&] { return subtitles(log.text()) >= 1; }, 400, 1.0f / 20.0f));
    fakeplat::setFocus(false);
    const size_t held = log.mark();
    app->frames(200, 0.1f);
    const int shownWhileHeld = subtitles(log.since(held));
    fakeplat::setFocus(true);
    CHECK_EQ(shownWhileHeld, 0);
    CHECK(app->runUntil([&] { return subtitles(log.since(held)) >= 1; }, 400, 0.1f));

    // Every exercise, Space skipping the talk: the end card offers the first real game.
    REQUIRE(runSkipping(*app, [&] { return app->hasLabel("First game"); }, 20000));
    const std::string text = log.text();
    // The coach's own moves of the lesson, played by its hand.
    CHECK(log.has("coach: lesson move e7e5"));
    CHECK(log.has("coach: lesson move d7d5"));
    // The demonstrations: castling (the chapter's own), the pawn taking the queen, the king's escape
    // from a check, and the solutions after three wrong moves (en passant, the promotion). The
    // under-promotion was taken back too: the rook back beside the board, the pawn on e7.
    for (const char* demo : {"e1g1", "e6d5", "e8d8", "e8e7", "e5d6", "e7e8q"})
        CHECK(log.has(std::string("coach: demonstrates ") + demo + " on the table"));
    CHECK(!log.has("is not legal on the table"));
    CHECK(!log.has("does not show the position after the rewind"));
    CHECK(!log.has("a move outside an exercise"));
    CHECK_EQ(count(text, "coach: takes back 1 plies"), kWrongMoves);
    CHECK(!log.has("--play: "));   // every scripted move was legal when it was played
    CHECK(log.has("move 1: Qf8#"));
    CHECK(log.has("coach: the rules lesson is over"));
    CHECK(log.has("coach: handshake done"));

    // The end card: the next step, not a game's choices.
    const std::vector<std::string> labels = app->labels();
    CHECK(has(labels, "Main menu"));
    CHECK(has(labels, "View the board"));
    CHECK(!has(labels, "Analyse the game"));
    CHECK(!has(labels, "Play again"));
    const game::Settings& s = game::settings();
    CHECK(s.coachRulesDone);
    CHECK_EQ(s.coachLevel, 1);
    CHECK_EQ(s.coachLessonChapter, 0);
    IniFile ini;
    REQUIRE(ini.load(s.path));
    CHECK(ini.getBool("coach.rules_done", false));
    CHECK_EQ(ini.getInt("coach.level", -1), 1);
    CHECK(s.coachHistory.empty());   // the lesson is not a game of the history
    CHECK(coachtest::filesIn(app->folder().path + "pgn/", ".pgn").empty());
    CHECK(!log.has("voice model download"));
    g_lessonDone = true;
}

// "First game": a level 1 game with the coach, the player with White, then back to the title page.
TEST(coach_mode_lesson_end_card_starts_the_first_game) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(lessonArgs());
    REQUIRE(app);
    if (!g_lessonDone) SKIP("follows coach_mode_lesson_played_to_its_end_card");
    app->frames(30);
    REQUIRE(app->clickLabel("First game"));
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): level 1, human plays White", 600));
    CHECK(!log.has("New game (coach): level 0"));
    app->frames(60, 1.0f / 20.0f);   // seated: the pause menu opens once the game is played
    REQUIRE(leaveToTitle(*app));
    // Left without a move: nothing saved, the lesson still done.
    CHECK(coachtest::filesIn(app->folder().path + "pgn/", ".pgn").empty());
    CHECK(game::settings().coachRulesDone);
}

// The lesson started again by a player who left it at its stalemate chapter: it resumes there.
// The exercise waits (the scripted move does not fit it: the script stops); the coach repeats the
// instruction after 20 s without a touch, then gives the hint with its pointing after 45 s. An
// illegal placement is refused and explained; the mate is played by hand. Left during the last
// chapter, the lesson keeps that chapter for next time.
TEST(coach_mode_lesson_resumed_at_its_chapter) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(resumeArgs());
    REQUIRE(app);
    game::Settings& s = game::settings();
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): level 0 (the rules lesson), human plays White", 3000));
    REQUIRE(!modelPrompt(*app));
    CHECK_EQ(s.coachLessonChapter, 8);
    CHECK(!s.coachRulesDone);
    // The exercise waits: the script's first move does not fit it.
    REQUIRE(runSkipping(*app, [&] { return log.has("--play: 'e1d1' is not legal here"); }, 2000));
    CHECK(!log.has("move 1:"));

    // Untouched: the instruction again after 20 s, then the hint after 45 s (nothing in between).
    size_t mark = log.mark();
    app->frames(150, 0.1f);
    CHECK_EQ(subtitles(log.since(mark)), 0);
    CHECK(app->runUntil([&] { return subtitles(log.since(mark)) >= 1; }, 100, 0.1f));
    mark = log.mark();
    app->frames(200, 0.1f);
    CHECK_EQ(subtitles(log.since(mark)), 0);
    REQUIRE(app->runUntil([&] { return subtitles(log.since(mark)) >= 1; }, 150, 0.1f));

    // While the hint is said, by hand: the queen touched and put on a square it cannot reach. The
    // placement is refused and explained at once (the hint is cut at its next pause); a second
    // attempt while the explanation is said gets no other one. Then the mate on f8.
    mark = log.mark();
    gstest::clickAt(*app, gstest::piecePoint("f2", chess::Queen));
    gstest::clickAt(*app, gstest::squarePoint("g4"));
    CHECK(coachtest::waitLog(*app, log, "coach: f2-g4 refused (not a legal move)", 20));
    CHECK(app->runUntil([&] { return subtitles(log.since(mark)) >= 1; }, 60, 1.0f / 20.0f));
    gstest::clickAt(*app, gstest::squarePoint("g4"));
    CHECK_EQ(count(log.since(mark), "coach: f2-g4 refused"), 2);
    CHECK_EQ(subtitles(log.since(mark)), 1);
    CHECK(!log.has("move 1:"));
    app->frames(60, 0.1f);
    gstest::clickAt(*app, gstest::squarePoint("f8"));
    REQUIRE(coachtest::waitLog(*app, log, "move 1: Qf8#", 400));

    // The rest of the chapter, then the last one begins ("Let me tell you how real games are
    // played"): left there, by the pause menu, which offers no resignation nor draw in the lesson.
    mark = log.mark();
    REQUIRE(runSkipping(*app, [&] { return log.since(mark).find("how real games are played") != std::string::npos; }, 3000));
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Resume", 60));
    const std::vector<std::string> labels = app->labels();
    CHECK(has(labels, "Main menu"));
    CHECK(has(labels, "Options"));
    CHECK(!has(labels, "Resign"));
    CHECK(!has(labels, "Offer draw"));
    CHECK(!has(labels, "Take back my move"));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->waitForLabel("Leave", 60));
    REQUIRE(coachtest::clickLast(*app, "Leave"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("New Game"); }, 600, 1.0f / 20.0f));
    CHECK(!log.has("coach: the rules lesson is over"));
    CHECK(!log.has("voice model download"));
    CHECK_EQ(s.coachLessonChapter, 9);
    IniFile ini;
    REQUIRE(ini.load(s.path));
    CHECK_EQ(ini.getInt("coach.lesson_chapter", -1), 9);
    CHECK(coachtest::filesIn(app->folder().path + "pgn/", ".pgn").empty());
}
