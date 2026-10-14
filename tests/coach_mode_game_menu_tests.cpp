// A coach game started from the title page's Coach page and played by hand in the game scene
// (src/game/game_scene_coach.cpp, the coach branches of src/game/game_scene.cpp): the level chosen
// on the page, the colour alternating from one game to the next ([coach] next_colour), the lights
// coming up on the table with the opening handshake (no --no-intro), a move refused as illegal, a
// draw offered while the coach thinks (it answers from its own evaluation, its move held until
// then), the pause menu's "Take back my move" (both moves taken back by hand), the Options applied
// during the game, the pointer's free look and the window losing the focus, a resignation from the
// pause menu and leaving for the title page (the unfinished game saved). The voice model folder is
// empty and the voice switched off: the coach speaks through subtitles, no download is offered.
#include "test.h"
#include "coach_mode_game_kit.h"
#include "game/settings.h"
#include "game_scene_testkit.h"
#include "ui/ui.h"
#include <set>
#include <string>
#include <vector>

namespace {

const std::vector<std::string>& menuArgs() {
    static const std::vector<std::string> args = {"--coach-dir", coachtest::voiceFolder()};
    return args;
}

// The shared App on its title page.
headless::App* titlePage() {
    headless::App* app = headless::shared(menuArgs());
    if (!app || !coachgame::declineVoicePrompt(*app)) return nullptr;
    if (!app->waitForLabel("New Game", 30000) || !gstest::toTitle(*app)) return nullptr;
    return app;
}

// The Coach page opened from the title page, the level row 'level' chosen, Start: frames until the
// game is played (the human's colour in 'human', the log's "White" or "Black").
bool startFromCoachPage(headless::App& app, const coachtest::LogCapture& log, int level, const std::string& human) {
    if (!app.clickLabel("Coach") || !app.waitForLabel("Your colour", 120)) return false;
    if (!coachtest::clickId(app, coachtest::idIn({"coach", "levels"}, level))) return false;
    if (!app.clickLabel("Start")) return false;
    return coachtest::waitLog(app, log, "New game (coach): level " + std::to_string(level) + ", human plays " + human, 600);
}

// A move played by hand once the coach lets the player touch a piece: tried again until the log
// says 'done' after 'mark' (a touch is refused while the coach's hands are on the table).
bool playByHand(headless::App& app, const coachtest::LogCapture& log, size_t mark, const chess::Position& pos,
                const std::string& uci, const std::string& done) {
    auto played = [&] { return log.since(mark).find(done) != std::string::npos; };
    for (int i = 0; i < 10 && !played(); ++i) {
        app.runUntil(played, 40, 1.0f / 20.0f);   // the hands at rest
        if (!played()) gstest::playMove(app, pos, uci);
    }
    return app.runUntil(played, 60, 1.0f / 20.0f);
}

}  // namespace

// Level 2 from the Coach page, the colours alternating (White this time, Black next). The player
// plays 1.e4 by hand after trying e2-e5; while the coach thinks, a draw is offered from the pause
// menu: declined at once, and the coach plays on. "Take back my move" then takes both moves back by
// hand; 1.d4 instead, the Options applied, and the game left from the pause menu (saved unfinished).
TEST(coach_mode_game_menu_started_from_coach_page_and_played_by_hand) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    gstest::SettingsGuard guard;
    headless::App* app = titlePage();
    REQUIRE(app);
    game::Settings& s = game::settings();
    s.coachVoice = false;   // subtitles only: the Coach entry offers no download
    s.coachColour = 2;      // alternate
    s.coachNextColour = 0;
    s.coachLevel = 1;
    const std::string folder = gstest::pgnFolder(*app);
    const std::set<std::string> before = gstest::pgnFiles(folder);
    REQUIRE(startFromCoachPage(*app, log, 2, "White"));
    CHECK_EQ(s.coachLevel, 2);   // the page's choice, kept
    // The lights come up, the handshake across the board, then the coach greets the player.
    REQUIRE(app->runUntil([&] { return log.has("coach: subtitle \""); }, 600));
    CHECK_EQ(s.coachNextColour, 1);   // Black next time
    CHECK_EQ(gstest::savedIni().getInt("coach.next_colour", -1), 1);

    // e2-e5 refused (the coach's game knows no illegal moves), then 1.e4.
    chess::Game g;
    gstest::clickAt(*app, gstest::piecePoint("e2", chess::Pawn));
    app->frames(6);
    gstest::clickAt(*app, gstest::squarePoint("e5"));
    REQUIRE(coachtest::waitLog(*app, log, "coach: e2-e5 refused (not a legal move)", 20));
    gstest::clickAt(*app, gstest::squarePoint("e4"));
    REQUIRE(coachgame::skipping(*app, [&] { return log.has("] move 1: e4 (White"); }, 10.0, 0));
    g.play(g.position().parseUCI("e2e4"));

    // The coach's turn: a draw offered. It answers once its evaluation is in, its move waiting.
    REQUIRE(coachgame::openPause(*app));
    CHECK(app->hasLabel("Take back my move"));
    REQUIRE(app->clickLabel("Offer draw"));
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("coach: draw offer "); }, 30.0));
    CHECK(log.has("coach: draw offer declined ("));
    CHECK(!log.has("] move 2: "));   // held until the answer
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("] move 2: "); }, 30.0));
    REQUIRE(coachgame::openPause(*app));
    CHECK(!app->hasLabel("Claim draw"));
    REQUIRE(app->clickLabel("Take back my move"));
    REQUIRE(coachtest::waitLog(*app, log, "coach: takes back 2 plies (to move 0)", 60));
    const size_t mark = log.mark();

    // The table back at the start (by hand): 1.d4 instead.
    REQUIRE(playByHand(*app, log, mark, chess::Position(), "d2d4", "] move 1: d4 (White"));
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.since(mark).find("] move 2: ") != std::string::npos; }, 30.0));

    // The Options from the pause menu, applied during the game (the ambience switched).
    const bool ambience = s.ambience;
    REQUIRE(coachgame::openPause(*app));
    REQUIRE(app->clickLabel("Options"));
    REQUIRE(app->waitForLabel("Apply", 60));
    for (int i = 0; i < 8 && !app->hasLabel("Ambience"); ++i) app->key(plat::KEY_PAGEDOWN);   // to the Audio tab
    REQUIRE(app->clickLabel("Ambience"));
    REQUIRE(app->clickLabel("Apply"));
    CHECK(s.ambience != ambience);
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(app->waitForLabel("Resume", 60));
    REQUIRE(app->clickLabel("Resume"));
    app->frames(10);
    CHECK(!app->hasLabel("Resume"));

    // Left from the pause menu (confirmed): the title page; the game saved unfinished.
    REQUIRE(coachgame::openPause(*app));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->waitForLabel("Leave", 30));
    REQUIRE(coachtest::clickLast(*app, "Leave"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("New Game"); }, 600, 1.0f / 20.0f));
    const std::vector<chess::pgn::Record> games = gstest::newGames(folder, before);
    REQUIRE(games.size() == 1u);
    CHECK_EQ(games[0].result, std::string("*"));
    CHECK_EQ(games[0].tag("CoachLevel"), std::string("2"));
    REQUIRE(games[0].plies.size() == 2u);
    CHECK_EQ(games[0].plies[0].san, std::string("d4"));
}

// The next game from the Coach page: the player has Black (the colours alternate) and the coach
// opens. The free look (right button held) ends when Esc opens the pause menu; the window losing
// the focus holds the coach; the player resigns from the pause menu after the confirmation.
TEST(coach_mode_game_menu_alternated_colour_then_resigned) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    gstest::SettingsGuard guard;
    headless::App* app = titlePage();
    REQUIRE(app);
    game::Settings& s = game::settings();
    s.coachVoice = false;
    s.coachColour = 2;
    s.coachNextColour = 1;
    REQUIRE(startFromCoachPage(*app, log, 3, "Black"));
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("] move 1: "); }, 30.0));
    CHECK_EQ(s.coachNextColour, 0);   // White next time
    CHECK(log.has("coach: repertoire move "));
    // The window in the background, then back.
    fakeplat::setFocus(false);
    app->frames(30);
    fakeplat::setFocus(true);
    app->frames(5);
    // Looking around with the right button held; Esc opens the menu and ends the look.
    plat::Input& in = fakeplat::next();
    in.mouseDown[plat::MOUSE_RIGHT] = in.mousePressed[plat::MOUSE_RIGHT] = true;
    app->frame();
    in.mousePressed[plat::MOUSE_RIGHT] = false;
    app->frames(3);
    REQUIRE(coachgame::openPause(*app));
    in.mouseDown[plat::MOUSE_RIGHT] = false;
    in.mouseReleased[plat::MOUSE_RIGHT] = true;
    app->frame();
    CHECK(!fakeplat::record().mouseCaptured);
    // Resigned (confirmed): the coach's closing words, the handshake, the end card.
    REQUIRE(app->clickLabel("Resign"));
    REQUIRE(app->waitForLabel("Cancel", 30));
    REQUIRE(coachtest::clickLast(*app, "Resign"));
    REQUIRE(coachtest::waitLog(*app, log, "Game over: 1-0 (Resignation)", 60));
    REQUIRE(coachgame::waitEndCard(*app));
    CHECK_EQ(s.coachHistory.back().level, 3);
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->runUntil([&] { return app->hasLabel("New Game"); }, 600, 1.0f / 20.0f));
}
