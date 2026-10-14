// Coach mode challenges in the game scene: src/game/game_scene_coach.cpp (the challenge's set-up,
// the coach's move into each position played by its hand, the wrong moves answered on the table
// and taken back, the hint offer and the H key, endChallenge, saveChallengeDone, nextChallenge),
// src/game/game_scene.cpp (the end card's "Next challenge" and "Main menu" back to the Coach page's
// Challenges tab) and, through them, src/coach/challenge_run.cpp. The voice folder is empty: the
// coach speaks through subtitles.
//
// First a set started from the command line at its last position (mate1, a mate in one after the
// coach's move into it): three wrong moves made by --play, the hint offer accepted with H, the
// hints asked with H (the last one shows the move by hand), the mate played by hand; then "Next
// challenge", the way back to the Coach page and the set started again from there. Then a set
// solved by --play from its first position (escape: six positions of one move), back to the list
// of challenges from its end card, and the rules lesson started from the page's Training tab.
#include "test.h"
#include "coach_mode_support.h"
#include "game_scene_testkit.h"
#include "core/ini.h"
#include "game/settings.h"
#include <algorithm>
#include <string>
#include <vector>

namespace {

// The .ini of an App, in its own folder: the brightness calibrated.
std::string iniIn(const headless::TempDir& dir) {
    const std::string p = dir.file("Scacelith.ini");
    coachtest::writeFile(p, "[graphics]\nbrightness_calibrated = true\n");
    return p;
}

// mate1's last position: the coach plays g8f7 into it, Bg6 mates. Kh2 three times first.
const std::vector<std::string>& mateArgs() {
    static headless::TempDir dir("coach-challenge-ini");
    static const std::vector<std::string> args = {"--no-intro", "--start", "--coach", "--coach-challenge", "mate1",
                                                  "--coach-challenge-position", "8", "--play", "g1h2,g1h2,g1h2",
                                                  "--ini", iniIn(dir), "--coach-dir", coachtest::voiceFolder()};
    return args;
}

// escape from its first position: six positions, each wanting one move with White to move at
// once, which --play solves.
const std::vector<std::string>& escapeArgs() {
    static headless::TempDir dir("coach-challenge-escape-ini");
    static const std::vector<std::string> args = {"--no-intro", "--start", "--coach", "--coach-challenge", "escape",
                                                  "--play", "f7f1,f2e3,h1h4,f6e5,e3e4,f1f2", "--ini", iniIn(dir),
                                                  "--coach-dir", coachtest::voiceFolder()};
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

// H pressed every few frames until the log says 'needle' (a hint is given only once the previous
// one is said).
bool pressHUntil(headless::App& app, const coachtest::LogCapture& log, const std::string& needle, int tries = 100) {
    for (int i = 0; i < tries; ++i) {
        if (log.has(needle)) return true;
        app.key('H');
        app.frames(5, 1.0f / 20.0f);
    }
    return log.has(needle);
}

// The piece of 'type' on 'from' put on 'to' by hand, tried again until the log has one more
// 'played' line (the player's hand may move only while the position waits).
bool moveByHand(headless::App& app, const coachtest::LogCapture& log, const char* from, chess::PieceType type,
                const char* to, const std::string& played) {
    const int before = count(log.text(), played);
    for (int i = 0; i < 12; ++i) {
        app.frames(20, 1.0f / 20.0f);
        gstest::clickAt(app, gstest::piecePoint(from, type));
        gstest::clickAt(app, gstest::squarePoint(to));
        if (app.runUntil([&] { return count(log.text(), played) > before; }, 40, 1.0f / 20.0f)) return true;
    }
    return false;
}

bool g_mateSolved = false;   // the first test reached mate1's end card

}  // namespace

// The coach plays its move into the position by hand (a scripted move of the coach's, LessonWait
// meanwhile); each wrong move is judged by the engine, answered with the coach's reply shown on
// the table and put back, then taken back. After three of them the coach offers a hint: H accepts
// it. The next hints are asked with H, the second one with the bishop in hand (put back first);
// the third shows the move on the table and takes it back. The mate solves the last position: the
// set is completed and remembered, its end card offers the next one; nothing is saved as a game.
TEST(coach_mode_challenge_wrong_moves_hints_and_the_mate) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(mateArgs());
    REQUIRE(app);
    // The world is built first (seconds of wall-clock time for the first App of the run).
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("New game (coach): the challenge 'mate1'"); }, 120.0));
    REQUIRE(coachtest::waitLog(*app, log, "coach: lesson move g8f7", 600));
    REQUIRE(!modelPrompt(*app));
    CHECK(log.has("coach challenge mate1: 8 positions (from 8)"));

    // Three wrong moves, each taken back; the offer of a hint follows the third.
    REQUIRE(coachtest::waitEngine(*app, [&] { return app->hasLabel("No, thanks"); }));
    CHECK(log.has("coach challenge mate1, position 8: g1h2 is not the move (3)"));
    CHECK_EQ(count(log.text(), "coach: takes back 1 plies (to move 1)"), 3);
#if defined(SCACELITH_HAS_STOCKFISH)
    CHECK_EQ(count(log.text(), "coach: demonstrates "), 3);   // the coach's answer, shown and put back
#endif
    CHECK(!log.has("does not show the position after the rewind"));
    CHECK(!log.has("--play: "));
    CHECK(!log.has("move 2: Bg6"));
    app->frames(20);   // the card's buttons answer once it is readable; H answers at once
    app->key('H');
    CHECK(log.has("coach: hint offer accepted"));
    REQUIRE(coachtest::waitLog(*app, log, "coach challenge mate1, position 8: hint 1 (f5g6)", 200));
    CHECK(app->runUntil([&] { return !app->hasLabel("No, thanks"); }, 60));   // the card fades out

    // The bishop in hand, H: put back, and the next hint. Then the last one: the move shown.
    gstest::clickAt(*app, gstest::piecePoint("f5", chess::Bishop));
    REQUIRE(pressHUntil(*app, log, "hint 2 (f5g6)"));
    CHECK(log.has("coach: hint asked (H)"));
    REQUIRE(pressHUntil(*app, log, "hint 3 (f5g6)"));
    REQUIRE(coachtest::waitLog(*app, log, "coach: demonstrates f5g6 on the table", 400));
    CHECK(!log.has("move 2: Bg6"));   // shown on the table only

    // The mate by hand, once the coach's hand is back and it asks for the move.
    bool mated = false;
    for (int i = 0; i < 12 && !mated; ++i) {
        app->frames(30, 1.0f / 20.0f);
        gstest::clickAt(*app, gstest::piecePoint("f5", chess::Bishop));
        gstest::clickAt(*app, gstest::squarePoint("g6"));
        mated = coachtest::waitLog(*app, log, "move 2: Bg6#", 60);
    }
    REQUIRE(mated);
    CHECK(coachtest::waitLog(*app, log, "coach challenge mate1: completed", 60));
    CHECK(log.has("coach: the challenge 'mate1' is completed (1 of them)"));
    const game::Settings& s = game::settings();
    CHECK(s.coachChallengeDone("mate1"));
    IniFile ini;
    REQUIRE(ini.load(s.path));
    CHECK(ini.getString("coach.challenges_done").find("mate1") != std::string::npos);

    // The closing words (Space skips them), the handshake, the end card.
    REQUIRE(coachtest::waitLabelSkipping(*app, "Next challenge", 1200));
    CHECK(log.has("coach: the challenge 'mate1' is over"));
    const std::vector<std::string> labels = app->labels();
    CHECK(has(labels, "Main menu"));
    CHECK(has(labels, "View the board"));
    CHECK(!has(labels, "Analyse the game"));
    CHECK(!has(labels, "Play again"));
    CHECK(coachtest::filesIn(app->folder().path + "pgn/", ".pgn").empty());
    g_mateSolved = true;
}

// "Next challenge": the next set not completed yet (mate2, from its first position), which the
// Coach page then shows selected. The scripted moves do not fit it (the script stops): three wrong
// moves by hand, then the hint offer, declined by touching a piece, which is then played: the
// first move of the mate, which the coach answers by its hand. The pause menu has no resignation
// nor draw; "Main menu" leaves to the Coach page's Challenges tab, whose "Start" starts the set
// again.
TEST(coach_mode_challenge_next_one_by_hand_then_the_coach_page) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(mateArgs());
    REQUIRE(app);
    if (!g_mateSolved) SKIP("follows coach_mode_challenge_wrong_moves_hints_and_the_mate");
    app->frames(30);
    REQUIRE(app->clickLabel("Next challenge"));
    CHECK(log.has("coach: next challenge 'mate2'"));
    CHECK_EQ(game::settings().coachChallenge, std::string("mate2"));
    IniFile ini;
    REQUIRE(ini.load(game::settings().path));
    CHECK_EQ(ini.getString("coach.challenge"), std::string("mate2"));
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): the challenge 'mate2'", 600));
    CHECK(coachtest::waitLog(*app, log, "coach challenge mate2: 6 positions (from 1)", 200));
    REQUIRE(coachtest::waitLog(*app, log, "--play: 'g1h2' is not legal here", 1200));

    // The king to h3, three times: each judged, answered on the table and taken back.
    for (int i = 1; i <= 3; ++i) {
        REQUIRE(moveByHand(*app, log, "h2", chess::King, "h3", "move 2: Kh3"));
        REQUIRE(coachtest::waitEngine(*app, [&] { return count(log.text(), "coach: takes back 1 plies (to move 1)") >= i; }));
    }
    REQUIRE(coachtest::waitEngine(*app, [&] { return app->hasLabel("No, thanks"); }));
    CHECK(log.has("coach challenge mate2, position 1: h2h3 is not the move (3)"));
    app->frames(20);
    gstest::clickAt(*app, gstest::piecePoint("h6", chess::Queen));
    CHECK(log.has("coach: hint offer declined (a piece touched)"));
    CHECK(!log.has("hint offer accepted"));
    CHECK(app->runUntil([&] { return !app->hasLabel("No, thanks"); }, 60));
    REQUIRE(moveByHand(*app, log, "h6", chess::Queen, "h7", "move 2: Qh7+"));
    REQUIRE(coachtest::waitLog(*app, log, "coach: lesson move g8f8", 600));
    CHECK(!log.has("hint 1"));   // no hint was given

    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Resume", 60));
    std::vector<std::string> labels = app->labels();
    CHECK(has(labels, "Main menu"));
    CHECK(!has(labels, "Resign"));
    CHECK(!has(labels, "Offer draw"));
    CHECK(!has(labels, "Take back my move"));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->waitForLabel("Leave", 60));
    REQUIRE(coachtest::clickLast(*app, "Leave"));
    const ui::im::Id row = coachtest::idIn({"coach", "challenges"}, std::string("mate2"));
    REQUIRE(app->runUntil([&] { return coachtest::findId(*app, row) != nullptr; }, 600, 1.0f / 20.0f));
    CHECK(app->hasLabel("Start"));
    CHECK(!app->hasLabel("Your colour"));   // the Training tab's
    CHECK(coachtest::filesIn(app->folder().path + "pgn/", ".pgn").empty());

    // Started again from there: from its first position.
    app->frames(20);
    REQUIRE(app->clickLabel("Start"));
    REQUIRE(app->runUntil([&] { return count(log.text(), "coach challenge mate2: 6 positions (from 1)") == 2; }, 600,
                          1.0f / 20.0f));
    CHECK_EQ(count(log.text(), "New game (coach): the challenge 'mate2'"), 2);
    CHECK(!log.has("voice model download"));
}

// A set solved from its first position: each position set up by the coach, each scripted move
// accepted at once; the set completed and remembered, its end card's "Main menu" back to the list
// of challenges. From there the page's Training tab starts the rules lesson.
TEST(coach_mode_challenge_set_solved_back_to_the_list) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(escapeArgs());
    REQUIRE(app);
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("New game (coach): the challenge 'escape'"); }, 120.0));
    REQUIRE(!modelPrompt(*app));
    CHECK(coachtest::waitLog(*app, log, "coach challenge escape: 6 positions (from 1)", 200));
    REQUIRE(coachtest::waitLabelSkipping(*app, "Next challenge", 6000, 0));
    for (const char* move : {"Rf1", "fxe3", "Rxh4", "Be5", "Qe4", "Rf2"})
        CHECK(log.has(std::string("move 1: ") + move));
    CHECK(!log.has("is not the move"));
    CHECK(!log.has("--play: "));
    CHECK(log.has("coach challenge escape: completed"));
    CHECK(game::settings().coachChallengeDone("escape"));
    IniFile ini;
    REQUIRE(ini.load(game::settings().path));
    CHECK(ini.getString("coach.challenges_done").find("escape") != std::string::npos);

    // "Main menu": the list of challenges (its first row in view: the set came from the command
    // line, not from a choice on the page).
    app->frames(30);
    REQUIRE(app->clickLabel("Main menu"));
    const ui::im::Id row = coachtest::idIn({"coach", "challenges"}, std::string("mate1"));
    REQUIRE(app->runUntil([&] { return coachtest::findId(*app, row) != nullptr; }, 600, 1.0f / 20.0f));
    CHECK(app->hasLabel("Start"));
    CHECK(!app->hasLabel("Your colour"));   // the Training tab's
    CHECK(coachtest::filesIn(app->folder().path + "pgn/", ".pgn").empty());

    // The Training tab: the rules lesson chosen there and started, then left for the title page.
    app->frames(20);
    app->key(plat::KEY_PAGEUP);   // the tab before
    const ui::im::Id rules = coachtest::idIn({"coach", "levels"}, 0);
    REQUIRE(app->runUntil([&] { return coachtest::findId(*app, rules) != nullptr; }, 60));
    REQUIRE(coachtest::clickId(*app, rules));
    app->frames(5);
    const size_t mark = log.mark();
    REQUIRE(app->clickLabel("Start"));
    REQUIRE(coachtest::waitLog(*app, log, "New game (coach): level 0 (the rules lesson), human plays White", 600));
    CHECK(log.since(mark).find("the challenge") == std::string::npos);
    app->frames(60, 1.0f / 20.0f);
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Main menu", 60));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->waitForLabel("Leave", 60));
    REQUIRE(coachtest::clickLast(*app, "Leave"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("New Game"); }, 600, 1.0f / 20.0f));
    CHECK_EQ(game::settings().coachLevel, 0);
    CHECK(!log.has("voice model download"));
}
