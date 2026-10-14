// The Coach page of the title menu (src/ui/ui_coach.cpp: ui::detail::coachPage, opened from the
// title page's "Coach" entry of ui::mainMenu) driven frame by frame (tests/ui_driver.h): its
// Training tab (the level rows read from the [coach] settings, the rules lesson that plays White,
// the colour, a list of levels of the game's own, the player's rating, the line said when the voice
// is missing), its Challenges tab (the selection, the list scrolled with the wheel and walked with
// the keyboard, a double click or Enter that starts the selected set, the page opened on that tab
// when the player comes back from a challenge) and both mirrored in Arabic. What is chosen is read
// from the CoachSetup, from the settings and from the .ini they are saved to on Start.
#include "test.h"
#include "coach_mode_support.h"
#include "ui_driver.h"
#include "coach/challenge.h"
#include "core/ini.h"
#include "game/settings.h"
#include "i18n/i18n.h"
#include <cmath>
#include <string>
#include <vector>

using ui::MenuAction;
using ui::gfx::Rect;

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

// The settings (saved again as they were), the coach levels the menus show and the language,
// restored when a test ends.
struct Restore {
    game::Settings settings = game::settings();
    ui::detail::Data data = ui::detail::data();
    ~Restore() {
        i18n::setLanguage("en");
        game::settings() = settings;
        game::settings().save();
        ui::detail::data() = data;
    }
};

// The time a page takes to slide in (0.35 s) and a little more.
void settle(uitest::Driver& d) { d.frames(30); }

ui::im::Id levelId(int i) { return coachtest::idIn({"coach", "levels"}, i); }
ui::im::Id challengeId(const std::string& id) { return coachtest::idIn({"coach", "challenges"}, id); }

const ui::im::TracedItem* byId(const uitest::Driver& d, ui::im::Id id) {
    for (const auto& it : d.items())
        if (it.id == id) return &it;
    return nullptr;
}
bool clickId(uitest::Driver& d, ui::im::Id id) {
    const ui::im::TracedItem* it = byId(d, id);
    if (!it) return false;
    d.clickRect(it->r);
    return true;
}

// The title page, then its "Coach" entry: the page settled.
void openCoach(uitest::Driver& d) {
    d.frames(2);
    d.click(T("menu.coach"));
    settle(d);
}

// The value of a key of the .ini the settings are saved to.
int savedInt(const char* key) {
    IniFile ini;
    if (!ini.load(game::settings().path)) return -1000;
    return ini.getInt(key, -1000);
}
std::string savedString(const char* key) {
    IniFile ini;
    if (!ini.load(game::settings().path)) return "?";
    return ini.getString(key, "?");
}

// Clicks the value box of the selector row 'label' (its middle: the next option).
bool clickValue(uitest::Driver& d, const std::string& label) {
    const ui::im::TracedItem* it = d.find(label);
    if (!it) return false;
    const Rect r = it->r;
    d.clickAt(r.r() - 22.0f - 130.0f, r.cy());
    d.frame();
    return true;
}

}  // namespace

// The Training tab opens on the level and the colour of the settings; a click on a row picks its
// level (with a sound), the rules lesson greys the colour out (it is always played with White), a
// game's colour turns with the value box and the arrows. Back leaves without saving anything, so the
// page opens on the settings again; Start saves the choice to the .ini and returns StartCoach.
TEST(coach_ui_page_training_tab_level_and_colour) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.coachTab = 0;
    g.coachLevel = 2;
    g.coachColour = 1;
    g.playerGames = 0;
    uitest::Driver d;
    uitest::Sounds sounds;
    Menus m;
    m.attach(d);
    openCoach(d);
    CHECK(!ui::titlePageShown());
    CHECK_EQ(m.coach.tab, 0);
    CHECK_EQ(m.coach.level, 2);
    CHECK_EQ(m.coach.colour, 1);
    // A row per level (the rules lesson and six bands), from the top down.
    for (int i = 0; i < 7; ++i) {
        const ui::im::TracedItem* row = byId(d, levelId(i));
        REQUIRE(row != nullptr);
        if (i > 0) CHECK(row->r.y > byId(d, levelId(i - 1))->r.y);
    }
    CHECK(byId(d, levelId(7)) == nullptr);
    REQUIRE(d.has(T("coach.colour")));
    CHECK(!d.disabled(T("coach.colour")));
    CHECK(d.has(T("newgame.start")));
    CHECK(d.has(T("common.back")));

    // The rules lesson: picked with a click, its colour greyed out (White).
    const int toggles = sounds.count(ui::Sound::Toggle);
    REQUIRE(clickId(d, levelId(0)));
    CHECK_EQ(m.coach.level, 0);
    CHECK_EQ(sounds.count(ui::Sound::Toggle), toggles + 1);
    CHECK(d.disabled(T("coach.colour")));
    REQUIRE(clickValue(d, T("coach.colour")));
    CHECK_EQ(m.coach.colour, 1);   // greyed out: unchanged
    REQUIRE(clickId(d, levelId(0)));   // the selected row again: nothing changes, no sound
    CHECK_EQ(sounds.count(ui::Sound::Toggle), toggles + 1);

    // A game: the colour turns with the value box (Black -> Alternate -> White) and the arrows.
    REQUIRE(clickId(d, levelId(4)));
    CHECK_EQ(m.coach.level, 4);
    CHECK(!d.disabled(T("coach.colour")));
    REQUIRE(clickValue(d, T("coach.colour")));
    CHECK_EQ(m.coach.colour, 2);
    REQUIRE(clickValue(d, T("coach.colour")));
    CHECK_EQ(m.coach.colour, 0);
    REQUIRE(d.nudge(T("coach.colour"), plat::KEY_RIGHT, 2));
    CHECK_EQ(m.coach.colour, 2);
    // Its help shows after a moment on the row (the tooltips' own layer: one more draw).
    REQUIRE(d.hover(T("coach.colour")));
    const uint64_t before = d.drawsOfFrame();
    d.wait(0.8f);
    CHECK(d.drawsOfFrame() > before);

    // Back: nothing saved; the page opens again on the settings.
    REQUIRE(d.click(T("common.back")));
    CHECK(ui::titlePageShown());
    CHECK_EQ(g.coachLevel, 2);
    CHECK_EQ(g.coachColour, 1);
    openCoach(d);
    CHECK_EQ(m.coach.level, 2);
    CHECK_EQ(m.coach.colour, 1);
    // Esc goes back too, with its sound.
    const int backs = sounds.count(ui::Sound::Back);
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(ui::titlePageShown());
    CHECK_EQ(sounds.count(ui::Sound::Back), backs + 1);

    // Start: the choice saved, StartCoach, the title page behind the game.
    openCoach(d);
    REQUIRE(clickId(d, levelId(5)));
    REQUIRE(clickValue(d, T("coach.colour")));   // Black -> Alternate
    REQUIRE(d.click(T("newgame.start")));
    CHECK_EQ(m.count(MenuAction::StartCoach), 1);
    CHECK_EQ(g.coachLevel, 5);
    CHECK_EQ(g.coachColour, 2);
    CHECK_EQ(g.coachTab, 0);
    CHECK_EQ(savedInt("coach.level"), 5);
    CHECK_EQ(savedInt("coach.colour"), 2);
    CHECK_EQ(savedInt("coach.tab"), 0);
    d.frame();
    CHECK(ui::titlePageShown());
}

// The levels are the game's list (ui::setCoachLevels): a level without texts of its own is named
// by its band, an open band reads "N+", and the settings' level is kept inside the list. The
// player's rating, the rules lesson completed, the alternation's next colour and the missing voice
// change what the page writes, not its rows; Enter starts with the keyboard.
TEST(coach_ui_page_levels_of_the_game_and_the_player) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    // Nine levels: the seventh to the ninth have no texts in assets/i18n.
    ui::setCoachLevels({{0, 0}, {600, 900}, {900, 1200}, {1200, 1500}, {1500, 1800}, {1800, 2100}, {2100, 2400},
                        {2400, 2700}, {2700, 0}});
    ui::setCoachLevels({});   // an empty list keeps the one given
    CHECK_EQ(ui::detail::data().coachLevels.size(), size_t(9));
    g.coachTab = 0;
    g.coachLevel = 40;     // beyond the list: the last level
    g.coachColour = 7;     // likewise: alternate
    g.coachNextColour = 1;
    g.playerGames = 0;
    uitest::Driver d;
    Menus m;
    m.attach(d);
    m.coach.voiceAvailable = false;
    openCoach(d);
    CHECK_EQ(m.coach.level, 8);
    CHECK_EQ(m.coach.colour, 2);
    for (int i = 0; i < 9; ++i) CHECK(byId(d, levelId(i)) != nullptr);
    std::vector<Rect> unrated;
    for (int i = 0; i < 9; ++i) unrated.push_back(byId(d, levelId(i))->r);

    // A rated player: the band of the rating recommended (1250 is in 1200-1500), the rating at the
    // end of the heading; a rating below every band recommends the first; the rules lesson done.
    for (int elo : {1250, 300, 3000}) {
        g.playerGames = 12;
        g.playerElo = elo;
        g.coachRulesDone = elo == 300;
        d.frames(2);
        for (int i = 0; i < 9; ++i) {
            const ui::im::TracedItem* row = byId(d, levelId(i));
            REQUIRE(row != nullptr);
            CHECK(std::fabs(row->r.y - unrated[size_t(i)].y) < 0.01f);
            CHECK(std::fabs(row->r.h - unrated[size_t(i)].h) < 0.01f);
        }
    }
    // The levels without texts are picked like the others.
    REQUIRE(clickId(d, levelId(7)));
    CHECK_EQ(m.coach.level, 7);
    REQUIRE(clickId(d, levelId(1)));
    CHECK_EQ(m.coach.level, 1);
    // Enter: the keyboard starts from the Start button (the page's default focus).
    ui::im::setFocus(0);
    d.frame();
    d.press(plat::KEY_ENTER);
    d.frame();
    CHECK_EQ(m.count(MenuAction::StartCoach), 1);
    CHECK_EQ(g.coachLevel, 1);
    CHECK_EQ(g.coachColour, 2);
    ui::im::setKeyboardMode(false);
}

// The Challenges tab (PageDown, or a click on the tab bar): the challenge of the settings selected,
// else the first not completed yet; a click selects a row, a second click on it starts its set,
// as does Enter on the selected row; Up and Down move the selection; the wheel scrolls the list.
// Start saves the tab and the challenge and returns StartChallenge.
TEST(coach_ui_page_challenges_tab) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.coachTab = 0;
    g.coachChallenge = "no-such-set";
    g.coachChallengesDone = {"mate1", "mate2"};
    uitest::Driver d;
    uitest::Sounds sounds;
    Menus m;
    m.attach(d);
    openCoach(d);
    d.press(plat::KEY_PAGEDOWN);
    d.frames(3);
    CHECK_EQ(m.coach.tab, 1);
    CHECK(!d.has(T("coach.colour")));
    // The first set not completed yet: mate3.
    CHECK_EQ(m.coach.challenge, std::string("mate3"));
    REQUIRE(byId(d, challengeId("mate1")) != nullptr);
    REQUIRE(byId(d, challengeId("mate3")) != nullptr);
    CHECK(byId(d, challengeId("endgame")) == nullptr);   // far below: not drawn
    REQUIRE(d.has(T("coach.challenge.start")));

    // A click selects (with the toggle sound); a second click on the same row starts.
    const int toggles = sounds.count(ui::Sound::Toggle);
    REQUIRE(clickId(d, challengeId("backrank")));
    CHECK_EQ(m.coach.challenge, std::string("backrank"));
    CHECK_EQ(sounds.count(ui::Sound::Toggle), toggles + 1);
    CHECK_EQ(m.count(MenuAction::StartChallenge), 0);
    REQUIRE(clickId(d, challengeId("backrank")));
    CHECK_EQ(m.count(MenuAction::StartChallenge), 1);
    CHECK_EQ(g.coachTab, 1);
    CHECK_EQ(g.coachChallenge, std::string("backrank"));
    CHECK_EQ(savedInt("coach.tab"), 1);
    CHECK_EQ(savedString("coach.challenge"), std::string("backrank"));
    d.frame();
    CHECK(ui::titlePageShown());

    // Back from the challenge: the page on its Challenges tab, whatever the settings say.
    g.coachTab = 0;
    ui::openCoachPage(1);
    settle(d);
    CHECK_EQ(m.coach.tab, 1);
    CHECK_EQ(m.coach.challenge, std::string("backrank"));
    REQUIRE(byId(d, challengeId("backrank")) != nullptr);

    // The keyboard: Down then Up move the selection (with a tick each), Enter on the selected row
    // starts. The first arrow press shows the focus (on the selected row) without moving it.
    const int ticks = sounds.count(ui::Sound::Tick);
    ui::im::setFocus(0);
    d.frame();
    d.press(plat::KEY_DOWN);
    d.frames(4);
    CHECK_EQ(m.coach.challenge, std::string("backrank"));
    d.press(plat::KEY_DOWN);
    d.frames(4);
    CHECK_EQ(m.coach.challenge, std::string("patterns"));
    d.press(plat::KEY_DOWN);
    d.frames(4);
    CHECK_EQ(m.coach.challenge, std::string("hanging"));   // past the group's heading
    d.press(plat::KEY_UP);
    d.frames(4);
    CHECK_EQ(m.coach.challenge, std::string("patterns"));
    CHECK(sounds.count(ui::Sound::Tick) >= ticks + 3);
    // Enter on the focused row while another is selected (chosen by other means between two
    // frames): it selects the focused row, it does not start.
    m.coach.challenge = "hanging";
    d.press(plat::KEY_ENTER);
    d.frame();
    CHECK_EQ(m.coach.challenge, std::string("patterns"));
    CHECK_EQ(m.count(MenuAction::StartChallenge), 1);
    d.press(plat::KEY_ENTER);
    d.frame();
    CHECK_EQ(m.count(MenuAction::StartChallenge), 2);
    CHECK_EQ(g.coachChallenge, std::string("patterns"));
    ui::im::setKeyboardMode(false);

    // The wheel: the last sets come into view, the first ones leave it; a click on the tab bar
    // goes back to Training.
    ui::openCoachPage(1);
    settle(d);
    const Rect first = byId(d, challengeId("mate1"))->r;
    d.move(first.cx(), first.cy());
    for (int i = 0; i < 12; ++i) d.wheel(-3.0f);
    d.frames(30);
    CHECK(byId(d, challengeId("endgame")) != nullptr);
    CHECK(byId(d, challengeId("mate1")) == nullptr);
    // The Start button starts the selected set.
    REQUIRE(d.click(T("coach.challenge.start")));
    CHECK_EQ(m.count(MenuAction::StartChallenge), 3);
    CHECK_EQ(g.coachChallenge, std::string("patterns"));
}

// A selection far down the list shows when the tab opens; once it is scrolled out of view, the
// first arrow press (the keyboard taking over from the mouse) brings it back. Every set completed:
// the first is selected.
TEST(coach_ui_page_challenges_selection_in_view) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.coachTab = 1;
    g.coachChallenge = "endgame";   // the last set
    g.coachChallengesDone.clear();
    uitest::Driver d;
    Menus m;
    m.attach(d);
    openCoach(d);
    CHECK_EQ(m.coach.tab, 1);
    const ui::im::TracedItem* last = byId(d, challengeId("endgame"));
    REQUIRE(last != nullptr);
    CHECK(last->r.y > 200.0f && last->r.b() < 960.0f);
    CHECK(byId(d, challengeId("mate1")) == nullptr);
    // Scrolled to the top with the wheel: the selection leaves the view.
    d.move(last->r.cx(), last->r.cy());
    for (int i = 0; i < 12; ++i) d.wheel(3.0f);
    d.frames(30);
    REQUIRE(byId(d, challengeId("mate1")) != nullptr);
    // The first arrow press shows the focus: the selected row scrolls back into view.
    ui::im::setFocus(0);
    d.frame();
    d.press(plat::KEY_UP);
    d.frames(40);
    CHECK_EQ(m.coach.challenge, std::string("endgame"));
    const ui::im::TracedItem* back = byId(d, challengeId("endgame"));
    REQUIRE(back != nullptr);
    CHECK(back->r.y > 200.0f && back->r.b() < 960.0f);
    ui::im::setKeyboardMode(false);
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(ui::titlePageShown());

    // Every set completed: the first one.
    g.coachChallenge.clear();
    g.coachChallengesDone.clear();
    for (const coach::Challenge& c : coach::ChallengeBook::shared().challenges()) g.coachChallengesDone.push_back(c.id);
    openCoach(d);
    CHECK_EQ(m.coach.challenge, coach::ChallengeBook::shared().challenges().front().id);
}

// In Arabic the page is mirrored: the level rows and the challenge rows stand on the right, the
// colour's arrows run the other way; a click still selects.
TEST(coach_ui_page_mirrored_in_arabic) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.coachTab = 0;
    g.coachLevel = 3;
    g.coachColour = 0;
    g.coachChallenge = "fork";
    g.coachChallengesDone = {"fork"};
    g.playerGames = 5;
    g.playerElo = 1600;
    uitest::Driver d;
    Menus m;
    m.attach(d);
    REQUIRE(i18n::setLanguage("ar"));
    openCoach(d);
    const ui::im::TracedItem* row = byId(d, levelId(3));
    REQUIRE(row != nullptr);
    CHECK(row->r.x > 960.0f);
    REQUIRE(clickId(d, levelId(2)));
    CHECK_EQ(m.coach.level, 2);
    // Right-to-left, the right arrow steps back: White stays White, Left goes to Black.
    REQUIRE(d.nudge(T("coach.colour"), plat::KEY_RIGHT, 1));
    CHECK_EQ(m.coach.colour, 0);
    REQUIRE(d.nudge(T("coach.colour"), plat::KEY_LEFT, 1));
    CHECK_EQ(m.coach.colour, 1);
    d.press(plat::KEY_PAGEDOWN);
    d.frames(3);
    const ui::im::TracedItem* fork = byId(d, challengeId("fork"));
    REQUIRE(fork != nullptr);
    CHECK(fork->r.x > 960.0f);
    REQUIRE(clickId(d, challengeId("pin")));
    CHECK_EQ(m.coach.challenge, std::string("pin"));
}
