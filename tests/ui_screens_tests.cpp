// The title menu and its pages (src/ui/ui_screens.cpp, the Watch a Game page and the ratings of
// src/ui/ui_screens_game.cpp) driven frame by frame with the mouse and the keyboard
// (tests/ui_driver.h): the title page's entries, New Game (presets, the engine's own settings, the
// time controls and a custom one, two players on one PC), every tab of Options with Apply and the
// unsaved changes question, the language switched to Arabic and back, Credits and Licences, Watch a
// Game and the brightness calibration. Each test restores the settings, the language and the lists
// the game gives the menus.
#include "test.h"
#include "ui_driver.h"
#include "game/graphics_quality.h"
#include "game/settings.h"
#include "i18n/i18n.h"
#include "ui/ui_stepper_values.h"
#include <cmath>
#include <functional>
#include <string>
#include <vector>

using ui::MenuAction;
using ui::gfx::Rect;
namespace font = ui::font;

namespace {

std::string T(const char* key) { return i18n::tr(key); }

// The menus' four setups and what mainMenu() returned.
struct Menus {
    ui::NewGameSetup setup;
    ui::WatchSetup watch;
    ui::CoachSetup coach;
    ui::LibrarySetup library;
    std::vector<MenuAction> actions;
    void attach(uitest::Driver& d) {
        d.screen = [this] {
            MenuAction a = ui::mainMenu(setup, watch, coach, library);
            if (a != MenuAction::None) actions.push_back(a);
        };
    }
    int count(MenuAction a) const {
        int n = 0;
        for (MenuAction x : actions) n += x == a ? 1 : 0;
        return n;
    }
};

// Restores the settings, the language and the lists the menus show when a test ends; starts the
// test from known lists (the game gives others: ai::presets(), chess::timeControlPresets()).
struct Restore {
    game::Settings settings = game::settings();
    ui::detail::Data data = ui::detail::data();
    Restore() {
        ui::setDifficultyList({{"Novice", "Just learned the moves.", 800},
                               {"Beginner", "Knows the basics.", 1000},
                               {"Casual", "A relaxed opponent.", 1200},
                               {"Club Player", "Solid club player.", 1500},
                               {"Advanced", "Strong club player.", 1800},
                               {"Expert", "Tournament expert.", 2100},
                               {"Master", "Master strength.", 2400},
                               {"Grandmaster", "Extremely hard to beat.", 2700},
                               {"Stockfish Max", "Full strength.", 3500},
                               {"Custom", "Your own engine settings.", 0}});
        ui::setTimeControlList({"Unlimited", "1+0", "3+0", "3+2", "5+0", "5+3", "10+0", "10+5", "15+10", "30+0", "30+20", "90+30"});
        ui::setResolutionList({{1280, 720}, {1600, 900}, {1920, 1080}});
    }
    ~Restore() {
        i18n::setLanguage("en");
        game::settings() = settings;
        game::settings().save();
        ui::detail::data() = data;
    }
};

// The time a page takes to slide in (0.35 s) and a little more: its widgets stand still afterwards.
void settle(uitest::Driver& d) { d.frames(30); }

// The widgets without a label (list rows, time control cells) whose rectangle 'keep' selects.
std::vector<Rect> bare(const uitest::Driver& d, const std::function<bool(const Rect&)>& keep) {
    return d.bare([&](const ui::im::TracedItem& it) { return keep(it.r); });
}

// A text field's text replaced: a click at the end of its box, Backspace until it is empty, the
// new text typed, then a click on an empty corner ends the edit.
bool retype(uitest::Driver& d, const std::string& label, const std::string& text) {
    const ui::im::TracedItem* it = d.find(label);
    if (!it) return false;
    const Rect r = it->r;
    d.clickAt(r.r() - 30.0f, r.cy());
    d.press(plat::KEY_END);
    for (int i = 0; i < 30; ++i) d.press(plat::KEY_BACKSPACE);
    if (!text.empty()) d.type(text);
    d.clickAt(4.0f, 1070.0f);
    return true;
}

}  // namespace

// The title page: its entries (Saved games only with a folder of saved games), the first one
// focused for the keyboard, Quit, and a page opened by the keyboard; it is "shown" only while it
// is the page drawn.
TEST(ui_screens_title_page_entries) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    Menus m;
    m.attach(d);
    ui::setVersionString("1.2.3-test");
    game::settings().playerGames = 12;   // the rating line with a record (provisional)
    game::settings().playerWins = 7;
    d.frames(3);
    CHECK(ui::titlePageShown());
    for (const char* key : {"menu.new_game", "menu.coach", "menu.online", "menu.watch", "analysis.menu.entry", "menu.options",
                            "menu.credits", "menu.quit"})
        CHECK(d.has(T(key)));
    CHECK(!d.has(T("menu.library")));
    const Rect quitR = d.find(T("menu.quit"))->r;
    // The keyboard starts on New Game: Enter opens its page, Esc comes back. (No focus left over
    // by the tests before, as in a game that just started: one may have left it on Quit.)
    ui::im::setFocus(0);
    d.frame();
    ui::im::setKeyboardMode(true);
    d.press(plat::KEY_ENTER);
    settle(d);
    CHECK(!ui::titlePageShown());
    CHECK(d.has(T("newgame.start")));
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(d.has(T("menu.new_game")));
    ui::im::setKeyboardMode(false);
    // With a folder of saved games, a ninth entry (the others a little closer).
    m.library.folder = "pgn/";
    d.frames(2);
    CHECK(d.has(T("menu.library")));
    CHECK(d.find(T("menu.quit"))->r.y > quitR.y);
    d.click(T("menu.quit"));
    CHECK_EQ(m.count(MenuAction::Quit), 1);
    CHECK(ui::titlePageShown());
    // A rated player's line, and a fresh player's.
    game::settings().playerGames = 40;
    game::settings().playerRated = true;
    d.frames(2);
    game::settings().playerGames = 0;
    d.frames(2);
    CHECK(ui::titlePageShown());
}

// In a right-to-left language the title page is mirrored: the entries stand on the right.
TEST(ui_screens_title_page_mirrored_in_arabic) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    Menus m;
    m.attach(d);
    d.frames(3);
    const Rect en = d.find(T("menu.new_game"))->r;
    CHECK(en.x < 960.0f);
    REQUIRE(i18n::setLanguage("ar"));
    d.frames(3);
    const ui::im::TracedItem* ar = d.find(T("menu.new_game"));
    REQUIRE(ar != nullptr);
    CHECK(ar->r.x > 960.0f);
    CHECK(std::fabs((ar->r.x + ar->r.w) - (1920.0f - en.x)) < 1.0f);
}

// New Game: the presets list and the time control grid start from the saved choices; a click
// selects; Start stores the choice in the settings and returns StartGame; Back and Esc go back.
TEST(ui_screens_new_game_presets_and_time_controls) {
    REQUIRE(uitest::app());
    Restore restore;
    game::settings().difficultyPreset = 1;
    game::settings().timeControlPreset = 2;
    game::settings().opponent = 0;
    game::settings().nextColor = 1;
    uitest::Driver d;
    uitest::Sounds sounds;
    Menus m;
    m.attach(d);
    d.frames(2);
    REQUIRE(d.click(T("menu.new_game")));
    settle(d);
    CHECK_EQ(m.setup.difficulty, 1);
    CHECK_EQ(m.setup.timeControl, 2);
    const ui::im::TracedItem* kind = d.find(T("newgame.opponent_kind"));
    REQUIRE(kind != nullptr);
    const Rect col = kind->r;
    const std::vector<Rect> rows = bare(d, [&](const Rect& r) { return r.x == col.x && r.w == col.w; });
    REQUIRE(rows.size() == 10u);
    const std::vector<Rect> cells = bare(d, [&](const Rect& r) { return r.x > col.r() && r.h == 66.0f; });
    REQUIRE(cells.size() == 13u);   // the twelve presets and Custom
    d.clickRect(rows[4]);
    CHECK_EQ(m.setup.difficulty, 4);
    CHECK(sounds.count(ui::Sound::Toggle) >= 1);
    d.clickRect(cells[7]);
    CHECK_EQ(m.setup.timeControl, 7);
    CHECK(!d.has(T("engine.skill")));
    CHECK(!d.has(T("tc.base_time")));
    REQUIRE(d.click(T("newgame.start")));
    CHECK_EQ(m.count(MenuAction::StartGame), 1);
    CHECK_EQ(game::settings().difficultyPreset, 4);
    CHECK_EQ(game::settings().timeControlPreset, 7);
    CHECK(d.has(T("menu.new_game")));   // back on the title page
    // Opened again: the stored choice; Back leaves it.
    REQUIRE(d.click(T("menu.new_game")));
    settle(d);
    CHECK_EQ(m.setup.difficulty, 4);
    REQUIRE(d.click(T("common.back")));
    CHECK(d.has(T("menu.new_game")));
    CHECK_EQ(m.count(MenuAction::StartGame), 1);
}

// The last preset, Custom, folds the other rows and shows the engine's own settings: the skill
// level (off while the strength is limited by Elo), the Elo, the depth, the time per move and the
// node limit; the wheel scrolls the folded list. A custom time control shows its base time,
// increment and delay. Start stores them all.
TEST(ui_screens_new_game_custom_engine_and_clock) {
    REQUIRE(uitest::app());
    Restore restore;
    game::settings().difficultyPreset = 3;
    game::settings().timeControlPreset = 0;
    game::settings().opponent = 0;
    game::settings().customLimitElo = false;
    game::settings().customElo = 1800;
    game::settings().customDepth = 0;
    game::settings().customMoveTimeMs = 0;
    game::settings().customNodes = 0;
    game::settings().customBaseSeconds = 600;
    game::settings().customIncrementSeconds = 5;
    game::settings().customDelaySeconds = 0;
    uitest::Driver d;
    Menus m;
    m.attach(d);
    d.frames(2);
    REQUIRE(d.click(T("menu.new_game")));
    settle(d);
    const Rect col = d.find(T("newgame.opponent_kind"))->r;
    std::vector<Rect> rows = bare(d, [&](const Rect& r) { return r.x == col.x && r.w == col.w; });
    REQUIRE(rows.size() == 10u);
    d.clickRect(rows[9]);
    settle(d);
    CHECK_EQ(m.setup.difficulty, 9);
    for (const char* key : {"engine.skill", "engine.limit_elo", "engine.elo", "engine.depth", "engine.move_time", "engine.nodes"})
        CHECK(d.has(T(key)));
    CHECK(!d.disabled(T("engine.skill")));
    CHECK(d.disabled(T("engine.elo")));
    REQUIRE(d.nudge(T("engine.skill"), plat::KEY_RIGHT, 2));
    CHECK_EQ(m.setup.skillLevel, game::settings().customSkillLevel + 2);
    REQUIRE(d.click(T("engine.limit_elo")));
    CHECK(m.setup.limitElo);
    CHECK(d.disabled(T("engine.skill")));
    CHECK(!d.disabled(T("engine.elo")));
    REQUIRE(d.nudge(T("engine.elo"), plat::KEY_RIGHT, 3));
    CHECK_EQ(m.setup.elo, 1830);
    REQUIRE(d.nudge(T("engine.depth"), plat::KEY_RIGHT, 3));
    CHECK_EQ(m.setup.depth, 3);
    REQUIRE(d.nudge(T("engine.move_time"), plat::KEY_RIGHT, 2));
    CHECK_EQ(m.setup.moveTimeMs, ui::detail::moveTimeValues()[2]);
    REQUIRE(d.nudge(T("engine.nodes"), plat::KEY_RIGHT, 1));
    CHECK_EQ(m.setup.nodes, ui::detail::nodeValues()[1]);
    // The folded rows: the list no longer fits, the wheel scrolls it.
    rows = bare(d, [&](const Rect& r) { return r.x == col.x && r.w == col.w; });
    REQUIRE(rows.size() == 10u);
    const float firstY = rows[0].y;
    d.move(rows[0].cx(), rows[0].cy());
    d.wheel(-1.0f);
    settle(d);
    rows = bare(d, [&](const Rect& r) { return r.x == col.x && r.w == col.w; });
    CHECK(rows[0].y < firstY - 10.0f);
    d.wheel(3.0f);
    settle(d);
    rows = bare(d, [&](const Rect& r) { return r.x == col.x && r.w == col.w; });
    CHECK(std::fabs(rows[0].y - firstY) < 0.5f);
    // A custom clock.
    const std::vector<Rect> cells = bare(d, [&](const Rect& r) { return r.x > col.r() && r.h == 66.0f; });
    REQUIRE(cells.size() == 13u);
    d.clickRect(cells.back());
    settle(d);
    CHECK_EQ(m.setup.timeControl, -1);
    REQUIRE(d.nudge(T("tc.base_time"), plat::KEY_RIGHT, 1));
    const auto& bases = ui::detail::baseTimeValues();
    CHECK_EQ(m.setup.customBaseSeconds, bases[size_t(ui::detail::nearestIndex(bases, 600) + 1)]);
    REQUIRE(d.nudge(T("tc.increment"), plat::KEY_LEFT, 5));
    CHECK_EQ(m.setup.customIncrementSeconds, 0);
    REQUIRE(d.nudge(T("tc.delay"), plat::KEY_RIGHT, 2));
    CHECK_EQ(m.setup.customDelaySeconds, 2);
    REQUIRE(d.click(T("newgame.start")));
    CHECK_EQ(m.count(MenuAction::StartGame), 1);
    const game::Settings& g = game::settings();
    CHECK_EQ(g.difficultyPreset, 9);
    CHECK_EQ(g.timeControlPreset, -1);
    CHECK(g.customLimitElo);
    CHECK_EQ(g.customElo, 1830);
    CHECK_EQ(g.customDepth, 3);
    CHECK_EQ(g.customMoveTimeMs, ui::detail::moveTimeValues()[2]);
    CHECK_EQ(g.customNodes, ui::detail::nodeValues()[1]);
    CHECK_EQ(g.customIncrementSeconds, 0);
    CHECK_EQ(g.customDelaySeconds, 2);
}

// Two players on one PC: the names (in their handwriting), the hands, the swap of colours and a
// rated game. A name left empty is the default one, stored as "" (it follows the language).
TEST(ui_screens_new_game_two_players) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.opponent = 0;
    g.playerName = "Human";
    g.handStyle = font::HAND_CAVEAT;
    g.hotseatNames[0] = g.hotseatNames[1] = "";
    g.hotseatHands[0] = g.hotseatHands[1] = -1;
    g.hotseatRated = false;
    game::LocalPlayer alice;
    alice.name = "Alice";
    alice.record.games = 3;
    alice.record.rating = 1544;
    g.localPlayers = {alice};
    uitest::Driver d;
    Menus m;
    m.attach(d);
    d.frames(2);
    REQUIRE(d.click(T("menu.new_game")));
    settle(d);
    CHECK(!d.has(T("hotseat.swap")));
    REQUIRE(d.nudge(T("newgame.opponent_kind"), plat::KEY_RIGHT));
    settle(d);
    CHECK_EQ(m.setup.opponent, 1);
    CHECK(d.has(T("hotseat.white_player")));
    CHECK(d.has(T("hotseat.black_player")));
    CHECK_EQ(d.count(T("player.handwriting")), 2);
    // The defaults: the player of Options > Player, and "Player 2" in another hand.
    CHECK_EQ(m.setup.names[0], T("player.default_name"));
    CHECK_EQ(m.setup.names[1], T("hotseat.player2"));
    CHECK_EQ(m.setup.hands[0], int(font::HAND_CAVEAT));
    CHECK_EQ(m.setup.hands[1], int(font::HAND_MARCK));
    REQUIRE(retype(d, T("hotseat.white_player"), "Alice"));
    CHECK_EQ(m.setup.names[0], std::string("Alice"));
    REQUIRE(retype(d, T("hotseat.black_player"), "  "));
    CHECK_EQ(m.setup.names[1], std::string("  "));
    REQUIRE(d.nudge(T("player.handwriting"), plat::KEY_RIGHT, 2));
    CHECK_EQ(m.setup.hands[0], int(font::HAND_BADSCRIPT));
    REQUIRE(d.click(T("hotseat.swap")));
    CHECK_EQ(m.setup.names[1], std::string("Alice"));
    CHECK_EQ(m.setup.hands[1], int(font::HAND_BADSCRIPT));
    CHECK_EQ(m.setup.hands[0], int(font::HAND_MARCK));
    REQUIRE(d.click(T("hotseat.rated")));
    CHECK(m.setup.rated);
    d.frames(2);
    REQUIRE(d.click(T("newgame.start")));
    CHECK_EQ(m.count(MenuAction::StartGame), 1);
    // The empty name became White's default; stored as such, Alice by her name.
    CHECK_EQ(m.setup.names[0], T("player.default_name"));
    CHECK_EQ(m.setup.names[1], std::string("Alice"));
    CHECK_EQ(g.opponent, 1);
    CHECK_EQ(g.hotseatNames[0], std::string());
    CHECK_EQ(g.hotseatNames[1], std::string("Alice"));
    CHECK_EQ(g.hotseatHands[0], int(font::HAND_MARCK));
    CHECK_EQ(g.hotseatHands[1], int(font::HAND_BADSCRIPT));
    CHECK(g.hotseatRated);
    // Back to Stockfish: the presets again.
    REQUIRE(d.click(T("menu.new_game")));
    settle(d);
    CHECK_EQ(m.setup.names[1], std::string("Alice"));
    REQUIRE(d.nudge(T("newgame.opponent_kind"), plat::KEY_LEFT));
    settle(d);
    CHECK_EQ(m.setup.opponent, 0);
    CHECK(!d.has(T("hotseat.swap")));
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(d.has(T("menu.new_game")));
}

// Options > Display: the language changes at once (and is saved), here to Arabic, where the
// selector runs the other way, and back; the display mode enables the window size; Apply is off
// until something changed, then stores the options and returns OptionsChanged.
TEST(ui_screens_options_display_and_language) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.fullscreen = true;
    g.displayWidth = 1000;   // not in the list: shown as its own entry
    g.displayHeight = 700;
    g.vsync = true;
    g.renderScale = 1.0f;
    g.language = "en";
    uitest::Driver d;
    Menus m;
    m.attach(d);
    d.frames(2);
    REQUIRE(d.click(T("menu.options")));
    settle(d);
    CHECK(ui::optionsOpen());
    CHECK(d.has(T("options.language")));
    CHECK(d.disabled(T("common.apply")));
    CHECK(d.disabled(T("options.window_size")));
    // The language, step by step to Arabic. The frame of a switch still shows the old language's
    // labels: the row is found again after one more frame.
    auto language = [&](int key) {
        d.frame();
        return d.nudge(T("options.language"), key);
    };
    const std::vector<i18n::Language>& langs = i18n::languages();
    const int en = i18n::languageIndex("en");
    REQUIRE(en >= 0);
    REQUIRE(language(plat::KEY_RIGHT));
    CHECK_EQ(i18n::language(), std::string(langs[size_t(en + 1)].code));
    CHECK_EQ(g.language, i18n::language());
    for (int i = 0; i < 40 && i18n::language() != "ar"; ++i) REQUIRE(language(plat::KEY_RIGHT));
    REQUIRE(i18n::language() == "ar");
    CHECK(ui::im::rtl());
    // Mirrored, the Right arrow goes back in the list.
    const int ar = i18n::languageIndex("ar");
    REQUIRE(language(plat::KEY_RIGHT));
    CHECK_EQ(i18n::language(), std::string(langs[size_t(ar - 1)].code));
    CHECK(!ui::im::rtl());
    for (int i = 0; i < 40 && i18n::language() != "en"; ++i) REQUIRE(language(plat::KEY_LEFT));
    REQUIRE(i18n::language() == "en");
    CHECK_EQ(g.language, std::string("en"));
    d.frames(2);
    CHECK(d.disabled(T("common.apply")));   // the language is no pending change
    // Windowed: the size can be chosen.
    REQUIRE(d.nudge(T("options.display_mode"), plat::KEY_RIGHT));
    d.frame();   // the page reads the pending changes as a frame begins
    CHECK(!d.disabled(T("options.window_size")));
    CHECK(!d.disabled(T("common.apply")));
    REQUIRE(d.nudge(T("options.window_size"), plat::KEY_RIGHT));   // already the last entry (its own size)
    REQUIRE(d.nudge(T("options.window_size"), plat::KEY_LEFT));
    REQUIRE(d.click(T("options.vsync")));
    REQUIRE(d.nudge(T("options.render_scale"), plat::KEY_LEFT, 2));
    CHECK(g.fullscreen);   // nothing applied yet
    REQUIRE(d.click(T("common.apply")));
    CHECK_EQ(m.count(MenuAction::OptionsChanged), 1);
    CHECK(!g.fullscreen);
    CHECK_EQ(g.displayWidth, 1920);
    CHECK_EQ(g.displayHeight, 1080);
    CHECK(!g.vsync);
    CHECK(std::fabs(g.renderScale - 0.9f) < 1e-4f);
    CHECK(d.disabled(T("common.apply")));
    REQUIRE(d.click(T("common.back")));
    CHECK(d.has(T("menu.new_game")));
    CHECK(ui::optionsOpen());   // still true the frame after (the game reads it one frame late)
    d.frame();
    CHECK(!ui::optionsOpen());
}

// Options > Display: a click on the language's name shows the whole list; a name picked there is
// applied and saved at once. Enter on the row opens it too; Esc, or a click beside the list,
// closes it without a change, and the options page stays.
TEST(ui_screens_options_language_list) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.language = "en";
    uitest::Driver d;
    Menus m;
    m.attach(d);
    d.frames(2);
    REQUIRE(d.click(T("menu.options")));
    settle(d);
    const ui::im::TracedItem* row = d.find(T("options.language"));
    REQUIRE(row != nullptr);
    const Rect r = row->r;
    const float boxX = r.r() - 22.0f - 125.0f;   // the middle of the selector's box (250 wide)
    d.clickRect(Rect(boxX - 40.0f, r.y, 80.0f, r.h));
    d.frames(20);
    CHECK_EQ(i18n::language(), std::string("en"));   // no step: the list is open
    for (const i18n::Language& l : i18n::languages()) CHECK(d.has(l.nativeName));
    REQUIRE(d.click("Deutsch"));
    d.frame();
    CHECK_EQ(i18n::language(), std::string("de"));
    CHECK_EQ(g.language, std::string("de"));
    CHECK(!d.has("Italiano"));   // closed
    CHECK(ui::optionsOpen());
    CHECK(d.disabled(T("common.apply")));   // the language is no pending change
    // Enter on the row, then Esc: the list closes, the page stays.
    REQUIRE(d.focusRow(T("options.language")));
    d.press(plat::KEY_ENTER);
    d.frames(20);
    CHECK(d.has("Italiano"));
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(!d.has("Italiano"));
    CHECK(d.has(T("options.language")));
    CHECK_EQ(i18n::language(), std::string("de"));
    // A click beside the list closes it too.
    d.clickRect(Rect(boxX - 40.0f, r.y, 80.0f, r.h));
    d.frames(20);
    REQUIRE(d.has("Italiano"));
    d.clickAt(20.0f, 20.0f);
    d.frame();
    CHECK(!d.has("Italiano"));
    CHECK_EQ(i18n::language(), std::string("de"));
    // The keyboard: the list opens on the current language; Down then Enter picks the next one.
    REQUIRE(d.focusRow(T("options.language")));
    d.press(plat::KEY_ENTER);
    d.frames(20);
    d.press(plat::KEY_DOWN);
    d.press(plat::KEY_ENTER);
    d.frame();
    const int de = i18n::languageIndex("de");
    CHECK_EQ(i18n::language(), std::string(i18n::languages()[size_t(de + 1)].code));
    CHECK(ui::optionsOpen());
}

// Leaving Options with changes asks first: Discard (or Esc) drops them, Apply stores them.
TEST(ui_screens_options_unsaved_changes) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.showCoordinates = false;
    g.invertLook = false;
    uitest::Driver d;
    Menus m;
    m.attach(d);
    d.frames(2);
    REQUIRE(d.click(T("menu.options")));
    settle(d);
    d.press(plat::KEY_PAGEDOWN, false, false);   // Graphics
    d.press(plat::KEY_PAGEDOWN);                 // Audio
    d.press(plat::KEY_PAGEDOWN);                 // Gameplay
    d.frame();
    REQUIRE(d.click(T("options.coordinates")));
    d.press(plat::KEY_ESCAPE);
    d.frames(2);
    CHECK(d.has(T("common.discard")));
    REQUIRE(d.click(T("common.discard")));
    d.frame();
    CHECK(d.has(T("menu.new_game")));
    CHECK(!g.showCoordinates);
    CHECK_EQ(m.count(MenuAction::OptionsChanged), 0);
    // Again: Esc on the question is Discard too.
    REQUIRE(d.click(T("menu.options")));
    settle(d);
    REQUIRE(d.click(T("options.invert_look")));
    REQUIRE(d.click(T("common.back")));
    d.frames(2);
    REQUIRE(d.has(T("common.discard")));
    d.press(plat::KEY_ESCAPE);
    d.frames(2);
    CHECK(d.has(T("menu.new_game")));
    CHECK(!g.invertLook);
    // And the question's Apply stores them.
    REQUIRE(d.click(T("menu.options")));
    settle(d);
    REQUIRE(d.click(T("options.invert_look")));
    d.press(plat::KEY_ESCAPE);
    d.frames(2);
    REQUIRE(d.clickLast(T("common.apply")));
    d.frame();
    CHECK(g.invertLook);
    CHECK_EQ(m.count(MenuAction::OptionsChanged), 1);
    CHECK(d.has(T("menu.new_game")));
}

// Options > Graphics: a preset sets every level; changing one level picks the preset that has
// them (or Custom); an option the driver cannot do is greyed out; the brightness slider.
TEST(ui_screens_options_graphics) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.graphicsPreset = game::PresetMedium;
    g.graphicsLevels = game::presetLevels(game::PresetMedium);
    g.brightness = 0.0f;
    const bool tessellation = game::graphicsOptionAvailable(game::GfxTessellation);
    game::setGraphicsOptionAvailable(game::GfxTessellation, false);
    uitest::Driver d;
    Menus m;
    m.attach(d);
    ui::debug::setOptionsTab(1);
    d.frames(2);
    REQUIRE(d.click(T("menu.options")));
    settle(d);
    REQUIRE(d.has(T("options.quality")));
    CHECK(d.disabled(T("options.tessellation")));
    CHECK(!d.disabled(T("options.bloom")));
    REQUIRE(d.nudge(T("options.quality"), plat::KEY_RIGHT));   // High
    REQUIRE(d.click(T("options.bloom")));                       // High without bloom: Custom
    REQUIRE(d.nudge(T("options.shadows"), plat::KEY_LEFT));
    REQUIRE(d.nudge(T("options.brightness"), plat::KEY_RIGHT, 3));
    REQUIRE(d.click(T("common.apply")));
    CHECK_EQ(m.count(MenuAction::OptionsChanged), 1);
    game::GraphicsLevels want = game::presetLevels(game::PresetHigh);
    want[game::GfxBloom] = 1 - want[game::GfxBloom];
    want[game::GfxShadows] -= 1;
    CHECK(g.graphicsLevels == want);
    CHECK_EQ(g.graphicsPreset, game::matchingPreset(want));
    CHECK(std::fabs(g.brightness - 0.3f) < 1e-4f);
    // Back to a preset: every level follows.
    REQUIRE(d.nudge(T("options.quality"), plat::KEY_LEFT, 6));
    REQUIRE(d.click(T("common.apply")));
    CHECK_EQ(g.graphicsPreset, int(game::PresetVeryLow));
    CHECK(g.graphicsLevels == game::presetLevels(game::PresetVeryLow));
    game::setGraphicsOptionAvailable(game::GfxTessellation, tessellation);
}

// Options > Audio: the volumes, the ambience (its volume off with it), the voice's volume (off
// while the coach's voice is), the subtitles.
TEST(ui_screens_options_audio) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.masterVolume = 0.5f;
    g.effectsVolume = 1.0f;
    g.ambience = true;
    g.ambienceVolume = 0.7f;
    g.coachVoice = false;
    g.voiceVolume = 1.0f;
    g.subtitles = 0;
    uitest::Driver d;
    Menus m;
    m.attach(d);
    ui::debug::setOptionsTab(2);
    d.frames(2);
    REQUIRE(d.click(T("menu.options")));
    settle(d);
    REQUIRE(d.has(T("options.master_volume")));
    CHECK(d.disabled(T("options.voice_volume")));
    CHECK(!d.disabled(T("options.ambience_volume")));
    REQUIRE(d.nudge(T("options.master_volume"), plat::KEY_RIGHT, 2));
    REQUIRE(d.nudge(T("options.effects"), plat::KEY_LEFT, 1));
    REQUIRE(d.nudge(T("options.ambience_volume"), plat::KEY_LEFT, 4));
    REQUIRE(d.click(T("options.ambience")));
    CHECK(d.disabled(T("options.ambience_volume")));
    REQUIRE(d.nudge(T("options.ambience_volume"), plat::KEY_LEFT, 4));   // off: it stays
    REQUIRE(d.click(T("options.coach_voice")));
    CHECK(!d.disabled(T("options.voice_volume")));
    REQUIRE(d.nudge(T("options.voice_volume"), plat::KEY_LEFT, 3));
    REQUIRE(d.nudge(T("options.subtitles"), plat::KEY_RIGHT, 2));
    REQUIRE(d.click(T("common.apply")));
    CHECK_EQ(m.count(MenuAction::OptionsChanged), 1);
    CHECK(std::fabs(g.masterVolume - 0.6f) < 1e-4f);
    CHECK(std::fabs(g.effectsVolume - 0.95f) < 1e-4f);
    CHECK(std::fabs(g.ambienceVolume - 0.5f) < 1e-4f);
    CHECK(!g.ambience);
    CHECK(g.coachVoice);
    CHECK(std::fabs(g.voiceVolume - 0.85f) < 1e-4f);
    CHECK_EQ(g.subtitles, 2);
}

// Options > Gameplay, Player (the name in any script, its handwriting and its preview, saving the
// games), the Online and Controls tabs, reached with PageDown.
TEST(ui_screens_options_gameplay_and_player) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.showLegalMoves = true;
    g.autoPressClock = false;
    g.mouseSensitivity = 1.0f;
    g.handoverSeconds = 1.6f;
    g.playerName = "Human";
    g.handStyle = font::HAND_CAVEAT;
    g.saveGames = true;
    uitest::Driver d;
    Menus m;
    m.attach(d);
    ui::debug::setOptionsTab(3);
    d.frames(2);
    REQUIRE(d.click(T("menu.options")));
    settle(d);
    REQUIRE(d.has(T("options.legal_moves")));
    REQUIRE(d.click(T("options.legal_moves")));
    REQUIRE(d.click(T("options.auto_press")));
    REQUIRE(d.click(T("options.thinking_time")));
    REQUIRE(d.click(T("options.ignore_head")));
    REQUIRE(d.click(T("options.game_cursor")));
    REQUIRE(d.nudge(T("options.mouse_sensitivity"), plat::KEY_RIGHT, 4));
    REQUIRE(d.nudge(T("options.handover"), plat::KEY_LEFT, 10));   // down to the instant cut
    d.press(plat::KEY_PAGEDOWN);
    d.frames(2);
    REQUIRE(d.has(T("player.name")));
    REQUIRE(retype(d, T("player.name"), "\xD0\x90\xD0\xBD\xD0\xBD\xD0\xB0 \xE7\x8E\x8B"));   // "Анна 王"
    REQUIRE(d.nudge(T("player.handwriting"), plat::KEY_RIGHT));
    REQUIRE(d.click(T("options.save_games")));
    d.press(plat::KEY_PAGEDOWN);   // Online (the online worker's rows)
    d.frames(2);
    d.press(plat::KEY_PAGEDOWN);   // Controls: lines only
    d.frames(2);
    CHECK(!d.has(T("player.name")));
    CHECK(d.has(T("common.back")));
    REQUIRE(d.click(T("common.apply")));
    CHECK_EQ(m.count(MenuAction::OptionsChanged), 1);
    CHECK(!g.showLegalMoves);
    CHECK(g.autoPressClock);
    CHECK(!g.humanizeThinking);
    CHECK(g.ignoreOpponentHead);
    CHECK(!g.gameCursor);
    CHECK(std::fabs(g.mouseSensitivity - 1.2f) < 1e-4f);
    CHECK_EQ(g.handoverSeconds, 0.0f);
    CHECK_EQ(g.playerName, std::string("\xD0\x90\xD0\xBD\xD0\xBD\xD0\xB0 \xE7\x8E\x8B"));
    CHECK_EQ(int(g.handStyle), int(font::HAND_MARCK));
    CHECK(!g.saveGames);
    // A name of spaces only is stored as the default one.
    d.press(plat::KEY_PAGEUP);
    d.press(plat::KEY_PAGEUP);
    d.frames(2);
    REQUIRE(retype(d, T("player.name"), "   "));
    REQUIRE(d.click(T("common.apply")));
    CHECK_EQ(g.playerName, std::string("Human"));
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(d.has(T("menu.new_game")));
}

// Credits: the entries scroll with the wheel and the keys; Licences lists the texts embedded with
// the game (the keyboard selects as it moves), Back and Esc go back one page each.
TEST(ui_screens_credits_and_licences) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    uitest::Sounds sounds;
    Menus m;
    m.attach(d);
    d.frames(2);
    REQUIRE(d.click(T("menu.credits")));
    settle(d);
    REQUIRE(d.has(T("credits.licences")));
    for (int key : {plat::KEY_PAGEDOWN, plat::KEY_END, plat::KEY_PAGEUP, plat::KEY_HOME, plat::KEY_DOWN, plat::KEY_UP}) {
        d.press(key);
        d.frames(3);
    }
    d.move(960, 500);
    d.wheel(-2.0f);
    d.wheel(2.0f);
    CHECK(d.has(T("credits.licences")));
    REQUIRE(d.click(T("credits.licences")));
    settle(d);
    // The list: one row per licence file, under the page title.
    const std::vector<Rect> rows = bare(d, [](const Rect& r) { return r.x < 700.0f && r.y > 150.0f && r.h > 20.0f; });
    REQUIRE(rows.size() >= 10u);
    const int toggles = sounds.count(ui::Sound::Toggle);
    d.clickRect(rows[2]);
    CHECK_EQ(sounds.count(ui::Sound::Toggle), toggles + 1);
    d.clickRect(rows[2]);   // already selected
    CHECK_EQ(sounds.count(ui::Sound::Toggle), toggles + 1);
    // The keyboard: each row it reaches is selected.
    ui::im::setKeyboardMode(true);
    d.press(plat::KEY_DOWN);
    d.press(plat::KEY_DOWN);
    CHECK(sounds.count(ui::Sound::Toggle) >= toggles + 2);
    ui::im::setKeyboardMode(false);
    for (int key : {plat::KEY_PAGEDOWN, plat::KEY_END, plat::KEY_HOME}) {
        d.press(key);
        d.frames(3);
    }
    d.move(1200, 500);
    d.wheel(-3.0f);
    d.frames(5);
    d.press(plat::KEY_ESCAPE);
    settle(d);
    CHECK(d.has(T("credits.licences")));
    REQUIRE(d.click(T("common.back")));
    CHECK(d.has(T("menu.new_game")));
}

// Watch a Game: the two players' presets (Custom left out), the swap, the time control grid and a
// custom one; Start stores the choice and returns StartWatching; Esc goes back.
TEST(ui_screens_watch_a_game) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.viewerWhitePreset = 5;
    g.viewerBlackPreset = 4;
    g.viewerTimeControl = 5;
    g.viewerCustomBaseSeconds = 300;
    g.viewerCustomIncrementSeconds = 3;
    g.viewerCustomDelaySeconds = 0;
    uitest::Driver d;
    Menus m;
    m.attach(d);
    d.frames(2);
    REQUIRE(d.click(T("menu.watch")));
    settle(d);
    CHECK_EQ(m.watch.whitePreset, 5);
    CHECK_EQ(m.watch.blackPreset, 4);
    REQUIRE(d.has(T("viewer.swap")));
    // Rows of 52 (two columns of nine presets), cells of 64 (13 time controls).
    std::vector<Rect> rows = bare(d, [](const Rect& r) { return r.h == 48.0f; });
    REQUIRE(rows.size() == 18u);
    d.clickRect(rows[1]);        // White: Beginner
    d.clickRect(rows[9 + 7]);    // Black: Grandmaster
    CHECK_EQ(m.watch.whitePreset, 1);
    CHECK_EQ(m.watch.blackPreset, 7);
    REQUIRE(d.click(T("viewer.swap")));
    CHECK_EQ(m.watch.whitePreset, 7);
    CHECK_EQ(m.watch.blackPreset, 1);
    const std::vector<Rect> cells = bare(d, [](const Rect& r) { return r.h == 64.0f; });
    REQUIRE(cells.size() == 13u);
    d.clickRect(cells[3]);
    CHECK_EQ(m.watch.timeControl, 3);
    d.clickRect(cells.back());
    settle(d);
    CHECK_EQ(m.watch.timeControl, -1);
    REQUIRE(d.nudge(T("viewer.base_time"), plat::KEY_RIGHT));
    REQUIRE(d.nudge(T("viewer.increment"), plat::KEY_RIGHT, 2));
    REQUIRE(d.nudge(T("viewer.delay"), plat::KEY_RIGHT, 1));
    CHECK_EQ(m.watch.customIncrementSeconds, 5);
    CHECK_EQ(m.watch.customDelaySeconds, 1);
    REQUIRE(d.click(T("viewer.start")));
    CHECK_EQ(m.count(MenuAction::StartWatching), 1);
    CHECK_EQ(g.viewerWhitePreset, 7);
    CHECK_EQ(g.viewerBlackPreset, 1);
    CHECK_EQ(g.viewerTimeControl, -1);
    const auto& bases = ui::detail::baseTimeValues();
    CHECK_EQ(g.viewerCustomBaseSeconds, bases[size_t(ui::detail::nearestIndex(bases, 300) + 1)]);
    CHECK_EQ(g.viewerCustomIncrementSeconds, 5);
    CHECK_EQ(g.viewerCustomDelaySeconds, 1);
    CHECK(d.has(T("menu.watch")));
    REQUIRE(d.click(T("menu.watch")));
    settle(d);
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(d.has(T("menu.watch")));
    CHECK_EQ(m.count(MenuAction::StartWatching), 1);
}

// The brightness calibration opens instead of the title page: Continue keeps the slider's value,
// Esc the stored one; both complete it (saved) and go on to the title page.
TEST(ui_screens_brightness_calibration) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.brightness = 0.5f;
    g.brightnessCalibrated = false;
    uitest::Driver d;
    Menus m;
    m.attach(d);
    ui::openBrightnessCalibration();
    d.frames(3);
    CHECK(!ui::titlePageShown());
    REQUIRE(d.has(T("calibration.continue")));
    REQUIRE(d.nudge(T("options.brightness"), plat::KEY_LEFT, 7));
    REQUIRE(d.click(T("calibration.continue")));
    CHECK(std::fabs(g.brightness - (-0.2f)) < 1e-4f);
    CHECK(g.brightnessCalibrated);
    CHECK(ui::titlePageShown());
    // Again, left with Esc: the stored brightness stays.
    g.brightnessCalibrated = false;
    ui::openBrightnessCalibration();
    d.frames(3);
    REQUIRE(d.nudge(T("options.brightness"), plat::KEY_RIGHT, 5));
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(std::fabs(g.brightness - (-0.2f)) < 1e-4f);
    CHECK(g.brightnessCalibrated);
    CHECK(ui::titlePageShown());
}

// The pages a debug hook or the game opens directly: a page asked for while the menu is up opens
// at once; Saved games without a folder falls back to the title page.
TEST(ui_screens_pages_opened_directly) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    Menus m;
    m.attach(d);
    d.frames(2);
    ui::debug::openMenuPage(ui::debug::MenuPage::Credits);
    d.frames(2);
    CHECK(d.has(T("credits.licences")));
    ui::debug::openMenuPage(ui::debug::MenuPage::Licences);
    d.frames(2);
    CHECK(!d.has(T("credits.licences")));
    CHECK(d.has(T("common.back")));
    ui::debug::openMenuPage(ui::debug::MenuPage::Library);   // no folder of saved games
    d.frames(2);
    CHECK(ui::titlePageShown());
    ui::openSavedGames();   // the same: back from a replay, without a folder
    d.frames(2);
    CHECK(ui::titlePageShown());
    ui::debug::openMenuPage(ui::debug::MenuPage::Options);
    d.frames(2);
    CHECK(ui::optionsOpen());
    CHECK(d.has(T("options.language")));
    d.press(plat::KEY_ESCAPE);
    d.frames(2);
    CHECK(ui::titlePageShown());
}
