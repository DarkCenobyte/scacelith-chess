// What Coach mode draws over the table (src/ui/ui_coach.cpp) driven frame by frame
// (tests/ui_driver.h) with made-up values: the coach's subtitles (their time on screen, their fade,
// the speaker's tag, a span of their own, long and right-to-left lines), the HUD (the takeback and
// hint offer cards and their mouse-only buttons, the takebacks left, the Space and H key caps, a
// challenge's progress band) and the Esc menu of a coach game (its entries by situation, the
// confirmations, its Options page). What a screen shows is read from its widgets and from the draw
// calls of the null OpenGL (one per layer and clip rectangle in a frame): the subtitles and the HUD
// are drawn on the overlay layer, so a frame of them alone makes one draw.
#include "test.h"
#include "ui_driver.h"
#include "game/settings.h"
#include "i18n/i18n.h"
#include <cmath>
#include <string>
#include <vector>

using ui::CoachHudAction;
using ui::MenuAction;
using ui::gfx::Rect;

namespace {

std::string T(const char* key) { return i18n::tr(key); }

bool almost(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

int count(const std::vector<MenuAction>& v, MenuAction a) {
    int n = 0;
    for (MenuAction x : v) n += x == a ? 1 : 0;
    return n;
}
int count(const std::vector<CoachHudAction>& v, CoachHudAction a) {
    int n = 0;
    for (CoachHudAction x : v) n += x == a ? 1 : 0;
    return n;
}

// The settings (the Options page applies to them) and the language, restored when a test ends.
struct Restore {
    game::Settings settings = game::settings();
    ~Restore() {
        i18n::setLanguage("en");
        game::settings() = settings;
        game::settings().save();
    }
};

// A subtitle that the test ages itself (the game follows the audio clock), drawn every frame.
struct SubtitleScreen {
    ui::Subtitle s;
    bool aging = true;
    void attach(uitest::Driver& d) {
        d.screen = [this, &d] {
            ui::subtitles(s);
            if (aging) s.age += d.dt;
        };
    }
    void say(const std::string& text, float duration = 0.0f) {
        s.text = text;
        s.age = 0.0f;
        s.duration = duration;
    }
};

}  // namespace

// The time a line stays: the audio's length, or the time to read it (15 Latin or 7 CJK characters
// a second, spaces and controls not counted) when that is longer, and 0.4 s more.
TEST(coach_ui_table_subtitle_duration) {
    CHECK(almost(ui::subtitleDuration("", 0.0f), 0.4f));
    CHECK(almost(ui::subtitleDuration("abc def ghi jkl mno", 0.0f), 15.0f / 15.0f + 0.4f));
    CHECK(almost(ui::subtitleDuration("abc def ghi jkl mno", 3.0f), 3.4f));
    CHECK(almost(ui::subtitleDuration("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C\xE6\x88\x91\xE4\xBB\xAC\xE5\xA5\xBD", 0.0f),
               1.0f + 0.4f));   // seven CJK characters
    CHECK(almost(ui::subtitleDuration("ab \xE4\xBD\xA0\t\n", 0.0f), 2.0f / 15.0f + 1.0f / 7.0f + 0.4f));
}

// A line fades in, stays its duration, then fades out over 0.35 s; cleared early, it fades out at
// once. It is drawn on the overlay layer and gives the layer it found back.
TEST(coach_ui_table_subtitles_come_and_go) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    SubtitleScreen sub;
    sub.attach(d);
    d.wait(0.5f);   // a line left by another test fades out
    CHECK_EQ(d.drawsOfFrame(), 0u);
    sub.say("Look at the knight on f3: it guards the king.", 1.0f);
    CHECK_EQ(d.drawsOfFrame(), 0u);   // its first frame: faded out still
    CHECK_EQ(d.drawsOfFrame(), 1u);
    d.wait(0.85f);
    CHECK_EQ(d.drawsOfFrame(), 1u);
    d.wait(0.6f);   // past its duration and the fade
    CHECK_EQ(d.drawsOfFrame(), 0u);
    // No duration: it stays until it is cleared, then fades out.
    sub.say("Your move.");
    d.wait(3.0f);
    CHECK_EQ(d.drawsOfFrame(), 1u);
    sub.s.text.clear();
    CHECK_EQ(d.drawsOfFrame(), 1u);
    d.wait(0.4f);
    CHECK_EQ(d.drawsOfFrame(), 0u);
    // The layer is given back: what follows it is drawn on the main layer (one more draw).
    sub.say("Your move.");
    d.screen = [&] {
        ui::subtitles(sub.s);
        ui::text("after", m::vec2(100, 100), 30.0f, m::vec4(1, 1, 1, 1));
        sub.s.age += d.dt;
    };
    d.frames(2);
    CHECK_EQ(d.drawsOfFrame(), 2u);
    d.screen = [&] { ui::subtitles(ui::Subtitle()); };
    d.wait(0.5f);
    CHECK_EQ(d.drawsOfFrame(), 0u);
}

// The forms of a line: without the speaker's tag, with a tag of its own (the analysis's), in a
// span of the window, above a raised bottom, too long for two lines (set smaller), and in
// languages without capitals (Japanese) or written right to left (Arabic). Each is one plate on
// the overlay.
TEST(coach_ui_table_subtitles_forms) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    SubtitleScreen sub;
    sub.attach(d);
    sub.say("A line without the tag.");
    sub.s.speaker = false;
    d.wait(0.3f);
    CHECK_EQ(d.drawsOfFrame(), 1u);
    sub.say("The analysis speaks.");
    sub.s.speaker = true;
    sub.s.tag = "ANALYSIS";
    sub.s.spanLeft = 120.0f;
    sub.s.spanRight = 1400.0f;
    sub.s.bottom = 970.0f;
    d.wait(0.3f);
    CHECK_EQ(d.drawsOfFrame(), 1u);
    std::string longText;
    for (int i = 0; i < 14; ++i) longText += "The coach explains why this move loses a piece. ";
    sub.say(longText);
    sub.s.tag.clear();
    sub.s.spanLeft = sub.s.spanRight = 0.0f;
    sub.s.bottom = 0.0f;
    d.wait(0.3f);
    CHECK_EQ(d.drawsOfFrame(), 1u);
    REQUIRE(i18n::setLanguage("ja"));
    sub.say("\xE3\x83\x8A\xE3\x82\xA4\xE3\x83\x88\xE3\x82\x92\xE8\xA6\x8B\xE3\x81\xA6\xE3\x80\x82");
    d.wait(0.3f);
    CHECK_EQ(d.drawsOfFrame(), 1u);
    REQUIRE(i18n::setLanguage("ar"));
    sub.say("\xD8\xA7\xD9\x86\xD8\xB8\xD8\xB1 \xD8\xA5\xD9\x84\xD9\x89 \xD8\xA7\xD9\x84\xD8\xAD\xD8\xB5\xD8\xA7\xD9\x86.");
    d.wait(0.3f);
    CHECK_EQ(d.drawsOfFrame(), 1u);
    sub.s.text.clear();
    d.wait(0.5f);
    CHECK_EQ(d.drawsOfFrame(), 0u);
}

// The takeback offer: a card on the end side with two mouse-only buttons, live once it has come in
// (a click as it appears was aimed at something else). "Play on" and "Take back" answer it; with
// the count of takebacks left the card is taller; after the answer it fades out, its buttons dead.
// The mouse over it belongs to the HUD.
TEST(coach_ui_table_takeback_offer_card) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    ui::CoachHud hud;
    std::vector<CoachHudAction> acts;
    d.screen = [&] {
        CoachHudAction a = ui::coachHud(hud);
        if (a != CoachHudAction::None) acts.push_back(a);
    };
    d.wait(0.5f);
    CHECK_EQ(d.drawsOfFrame(), 0u);
    CHECK(!d.has(T("coach.offer.accept")));
    hud.offer = true;
    d.frame();
    REQUIRE(d.has(T("coach.offer.accept")));
    REQUIRE(d.has(T("coach.offer.decline")));
    CHECK((d.find(T("coach.offer.accept"))->flags & ui::im::ITEM_MOUSE_ONLY) != 0);
    CHECK(d.find(T("coach.offer.accept"))->r.x > 960.0f);
    REQUIRE(d.click(T("coach.offer.accept")));   // as it comes in: ignored
    CHECK(acts.empty());
    d.wait(0.4f);
    const Rect accept = d.find(T("coach.offer.accept"))->r;
    d.move(accept.cx(), accept.cy() - 120.0f);   // over the card's text
    d.frame();
    CHECK(ui::wantsMouse());
    REQUIRE(d.click(T("coach.offer.decline")));
    CHECK_EQ(count(acts, CoachHudAction::PlayOn), 1);
    REQUIRE(d.click(T("coach.offer.accept")));
    CHECK_EQ(count(acts, CoachHudAction::TakeBack), 1);
    // Enter and Space are the game's: the buttons never take the keyboard.
    d.press(plat::KEY_ENTER);
    d.press(plat::KEY_SPACE);
    CHECK_EQ(acts.size(), size_t(2));
    // A question of its own, and the count of takebacks left (accepting uses one): taller.
    hud.offerText = "Take it back? The rook on a1 was left alone.";
    hud.takebacksLeft = 2;
    d.frame();
    const Rect plain = d.find(T("coach.offer.accept"))->r;
    hud.offerCount = true;
    d.wait(0.3f);
    const Rect counted = d.find(T("coach.offer.accept"))->r;
    CHECK(almost(counted.y, plain.y + 15.0f, 0.5f));
    hud.offerCount = false;
    d.wait(0.3f);
    CHECK(d.find(T("coach.offer.accept"))->r.y < counted.y);
    // Answered: the card fades out with what it said; its buttons do nothing meanwhile.
    hud.offer = false;
    hud.takebacksLeft = -1;
    d.frame();
    REQUIRE(d.has(T("coach.offer.accept")));
    REQUIRE(d.click(T("coach.offer.accept")));
    CHECK_EQ(acts.size(), size_t(2));
    d.wait(0.6f);
    CHECK(!d.has(T("coach.offer.accept")));
    CHECK_EQ(d.drawsOfFrame(), 0u);
}

// In a challenge the same card offers a hint ("Hint" answers TakeBack, "No, thanks" PlayOn); in a
// right-to-left language it stands on the start side, at the left.
TEST(coach_ui_table_hint_offer_card) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    ui::CoachHud hud;
    std::vector<CoachHudAction> acts;
    d.screen = [&] {
        CoachHudAction a = ui::coachHud(hud);
        if (a != CoachHudAction::None) acts.push_back(a);
    };
    hud.offer = true;
    hud.hintOffer = true;
    hud.takebacksLeft = 3;
    hud.offerCount = true;   // never shown on a hint offer
    d.wait(0.5f);
    CHECK(!d.has(T("coach.offer.accept")));
    REQUIRE(d.has(T("coach.hint.accept")));
    REQUIRE(d.click(T("coach.hint.decline")));
    CHECK_EQ(count(acts, CoachHudAction::PlayOn), 1);
    REQUIRE(d.click(T("coach.hint.accept")));
    CHECK_EQ(count(acts, CoachHudAction::TakeBack), 1);
    REQUIRE(i18n::setLanguage("ar"));
    hud.hintOffer = false;
    d.wait(0.3f);
    const ui::im::TracedItem* accept = d.find(T("coach.offer.accept"));
    REQUIRE(accept != nullptr);
    CHECK(accept->r.r() < 960.0f);
    REQUIRE(d.click(T("coach.offer.accept")));
    CHECK_EQ(count(acts, CoachHudAction::TakeBack), 2);
    hud.offer = false;
    hud.takebacksLeft = -1;
    d.wait(0.6f);
    CHECK_EQ(d.drawsOfFrame(), 0u);
}

// The corner and the top of the HUD: "Takebacks left", the Space and H key caps (alone or
// stacked), a challenge's progress band (in capitals, or without capitals in Japanese) come in
// and fade out on the overlay layer. The progress line names the set and the position.
TEST(coach_ui_table_hud_keys_and_progress) {
    REQUIRE(uitest::app());
    Restore restore;
    CHECK_EQ(ui::challengeName("fork"), T("challenge.fork.name"));
    CHECK_EQ(ui::challengeName("no-such-set"), std::string("no-such-set"));
    CHECK_EQ(ui::challengeProgress("fork", 2, 6), T("challenge.fork.name") + "  \xC2\xB7  3 / 6");
    uitest::Driver d;
    ui::CoachHud hud;
    d.screen = [&] { CHECK(ui::coachHud(hud) == CoachHudAction::None); };
    d.wait(0.5f);
    CHECK_EQ(d.drawsOfFrame(), 0u);
    struct Step {
        bool skip, hint;
        int left;
        const char* progress;
    };
    const Step steps[] = {{true, false, -1, ""}, {false, true, -1, ""}, {true, true, 2, ""}, {false, false, 0, ""},
                          {false, false, -1, "fork"}, {true, true, 1, "mate1"}};
    for (const Step& s : steps) {
        hud.skippable = s.skip;
        hud.hintKey = s.hint;
        hud.takebacksLeft = s.left;
        hud.progress = *s.progress ? ui::challengeProgress(s.progress, 0, 6) : std::string();
        d.wait(0.3f);
        CHECK_EQ(d.drawsOfFrame(), 1u);
        CHECK(d.items().empty());   // nothing to click
        hud = ui::CoachHud();
        d.frames(3);
        CHECK_EQ(d.drawsOfFrame(), 1u);   // fading out
        d.wait(1.0f);
        CHECK_EQ(d.drawsOfFrame(), 0u);
    }
    REQUIRE(i18n::setLanguage("ja"));
    hud.progress = ui::challengeProgress("fork", 1, 6);
    hud.skippable = true;
    d.wait(0.3f);
    CHECK_EQ(d.drawsOfFrame(), 1u);
    REQUIRE(i18n::setLanguage("ar"));
    hud.takebacksLeft = 1;
    hud.hintKey = true;
    d.wait(0.3f);
    CHECK_EQ(d.drawsOfFrame(), 1u);
    hud = ui::CoachHud();
    d.wait(1.0f);
    CHECK_EQ(d.drawsOfFrame(), 0u);
}

// The Esc menu of a coach game: the entries its situation allows (Resume, Options and Main menu
// always), Claim draw and Resign greyed out while a move is on its way, each entry's action; Resign
// and Main menu are confirmed first, and the menu below is blocked meanwhile; Esc resumes, but not
// on the frame the menu appears.
TEST(coach_ui_table_pause_menu_entries) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    uitest::Sounds sounds;
    ui::CoachPause cp;
    cp.canResign = false;   // the rules lesson
    std::vector<MenuAction> acts;
    bool shown = true;
    d.screen = [&] {
        if (!shown) return;
        MenuAction a = ui::coachPauseMenu(cp);
        if (a != MenuAction::None) acts.push_back(a);
    };
    d.frames(30);
    CHECK_EQ(sounds.count(ui::Sound::Open), 1);
    for (const char* key : {"pause.resume", "menu.options", "common.main_menu"}) CHECK(d.has(T(key)));
    for (const char* key : {"coach.pause.take_back", "pause.offer_draw", "pause.claim_draw", "pause.resign"}) CHECK(!d.has(T(key)));
    const float mainY = d.find(T("common.main_menu"))->r.y;

    // A game: every entry.
    cp = ui::CoachPause();
    cp.canTakeBack = cp.canOfferDraw = cp.canClaimDraw = true;
    d.frame();
    for (const char* key : {"coach.pause.take_back", "pause.offer_draw", "pause.claim_draw", "pause.resign"}) REQUIRE(d.has(T(key)));
    CHECK(d.find(T("common.main_menu"))->r.y > mainY);
    REQUIRE(d.click(T("pause.resume")));
    REQUIRE(d.click(T("coach.pause.take_back")));
    REQUIRE(d.click(T("pause.offer_draw")));
    REQUIRE(d.click(T("pause.claim_draw")));
    CHECK_EQ(count(acts, MenuAction::Resume), 1);
    CHECK_EQ(count(acts, MenuAction::TakeBack), 1);
    CHECK_EQ(count(acts, MenuAction::OfferDraw), 1);
    CHECK_EQ(count(acts, MenuAction::ClaimDraw), 1);
    // A move on its way: Claim draw and Resign greyed out.
    cp.mayEndGame = false;
    d.frame();
    CHECK(d.disabled(T("pause.claim_draw")));
    CHECK(d.disabled(T("pause.resign")));
    REQUIRE(d.click(T("pause.claim_draw")));
    CHECK_EQ(count(acts, MenuAction::ClaimDraw), 1);
    cp.mayEndGame = true;
    d.frame();

    // Resign: asked first (the menu below is blocked), Cancel, then confirmed. The coach that can
    // no longer checkmate makes it a draw: the same question, another text.
    REQUIRE(d.click(T("pause.resign")));
    d.frames(20);
    REQUIRE(d.has(T("common.cancel")));
    REQUIRE(d.click(T("pause.resume")));
    CHECK_EQ(count(acts, MenuAction::Resume), 1);
    REQUIRE(d.click(T("common.cancel")));
    CHECK(!d.has(T("common.cancel")));
    CHECK_EQ(count(acts, MenuAction::Resign), 0);
    cp.resignDraws = true;
    REQUIRE(d.click(T("pause.resign")));
    d.frames(20);
    REQUIRE(d.clickLast(T("confirm.resign.ok")));
    CHECK_EQ(count(acts, MenuAction::Resign), 1);
    // Main menu: the leave question; Esc cancels it, without resuming; then confirmed.
    REQUIRE(d.click(T("common.main_menu")));
    d.frames(20);
    REQUIRE(d.has(T("confirm.leave.ok")));
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(!d.has(T("confirm.leave.ok")));
    CHECK_EQ(count(acts, MenuAction::Resume), 1);
    REQUIRE(d.click(T("common.main_menu")));
    d.frames(20);
    REQUIRE(d.click(T("confirm.leave.ok")));
    CHECK_EQ(count(acts, MenuAction::BackToMainMenu), 1);
    // Esc resumes, with its sound...
    const int closes = sounds.count(ui::Sound::Close);
    d.press(plat::KEY_ESCAPE);
    CHECK_EQ(count(acts, MenuAction::Resume), 2);
    CHECK_EQ(sounds.count(ui::Sound::Close), closes + 1);
    // ...but not the Esc of the frame the menu appears on.
    shown = false;
    d.frame();
    shown = true;
    d.press(plat::KEY_ESCAPE);
    CHECK_EQ(count(acts, MenuAction::Resume), 2);
    CHECK_EQ(sounds.count(ui::Sound::Open), 2);
}

// Options from the Esc menu: its page in place of the menu; an option changed and applied returns
// OptionsChanged (stored in the settings); Back returns to the menu.
TEST(coach_ui_table_pause_menu_options) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.ambience = true;
    uitest::Driver d;
    ui::CoachPause cp;
    std::vector<MenuAction> acts;
    d.screen = [&] {
        MenuAction a = ui::coachPauseMenu(cp);
        if (a != MenuAction::None) acts.push_back(a);
    };
    d.frames(30);
    ui::debug::setOptionsTab(2);
    REQUIRE(d.click(T("menu.options")));
    d.frames(25);
    CHECK(ui::optionsOpen());
    CHECK(!d.has(T("pause.resume")));
    REQUIRE(d.click(T("options.ambience")));
    REQUIRE(d.click(T("common.apply")));
    CHECK_EQ(count(acts, MenuAction::OptionsChanged), 1);
    CHECK(!g.ambience);
    REQUIRE(d.click(T("common.back")));
    d.frame();
    CHECK(d.has(T("pause.resume")));
    CHECK(!ui::optionsOpen());
}
