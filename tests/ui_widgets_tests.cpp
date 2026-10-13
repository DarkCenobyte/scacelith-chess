// The immediate-mode widget core of the menus (src/ui/ui_widgets.cpp) and the small public UI
// calls of src/ui/ui.cpp, drawn by the tests' own frames (tests/ui_driver.h): ids, the keyboard
// and mouse focus, buttons, form rows (toggle, slider, stepper, selector), text fields (typing,
// the caret keys, Backspace / Delete, paste, Tab and Enter between fields, Esc), the tab bar,
// tooltips and info marks, the confirmation dialog, input blocking and occlusion, the sounds, and
// the mirrored rows of a right-to-left language.
#include "test.h"
#include "fake_platform.h"
#include "ui_driver.h"
#include "i18n/i18n.h"
#include "ui/ui_draw.h"
#include "ui/ui_screens_online.h"
#include <cmath>
#include <string>

using ui::gfx::Rect;
using namespace ui::im;

namespace {

// Restores English (and the glyphs of its strings) when a test switched the language.
struct English {
    ~English() { i18n::setLanguage("en"); }
};

}  // namespace

// A label may carry its id after "##": the translated label keeps the widget (its focus, its
// animations) across a language change. The id stack keeps the rows of two lists apart.
TEST(ui_widgets_ids_follow_the_key_after_the_hashes) {
    CHECK_EQ(makeId("Apply##common.apply"), makeId("Appliquer##common.apply"));
    CHECK_EQ(makeId(std::string("Apply##common.apply")), makeId("##common.apply"));
    CHECK(makeId("Apply") != makeId("Back"));
    CHECK_EQ(displayText("Apply##common.apply"), std::string("Apply"));
    CHECK_EQ(displayText("Plain"), std::string("Plain"));
    const Id plain = makeId(3);
    pushId("list");
    const Id inList = makeId(3);
    const Id named = makeId("Row");
    pushId(7);
    const Id nested = makeId(3);
    popId();
    popId();
    popId();   // one too many: harmless
    CHECK(plain != inList);
    CHECK(inList != nested);
    CHECK(named != makeId("Row"));
    CHECK_EQ(makeId(3), plain);
}

// The arrows move the keyboard focus to the nearest item in their direction (Down past the last
// row wraps to the first), Enter and Space activate the focused item; disabled and mouse-only
// items never take the focus. A page's default focus is taken when the focus is elsewhere.
TEST(ui_widgets_arrow_keys_move_the_focus_and_enter_activates) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    int clicked = -1;
    d.screen = [&] {
        const char* names[3] = {"First", "Second", "Third"};
        for (int i = 0; i < 3; ++i)
            if (button(names[i], Rect(760, 300 + 100.0f * float(i), 400, 60))) clicked = i;
        if (button("Side", Rect(1300, 400, 300, 60))) clicked = 3;
        button("Off", Rect(760, 600, 400, 60), ButtonKind::Secondary, false);
        if (button("Hud", Rect(1300, 700, 300, 60), ButtonKind::Secondary, true, ITEM_MOUSE_ONLY)) clicked = 9;
        setDefaultFocus(makeId("Second"));
    };
    d.frames(2);
    d.press(plat::KEY_ENTER);
    CHECK_EQ(clicked, 1);   // the default focus
    CHECK(keyboardMode());
    clicked = -1;
    d.press(plat::KEY_DOWN);
    d.press(plat::KEY_SPACE);
    CHECK_EQ(clicked, 2);
    clicked = -1;
    d.press(plat::KEY_DOWN);   // nothing below Third ("Off" is disabled, "Hud" mouse-only): wraps
    d.press(plat::KEY_ENTER);
    CHECK_EQ(clicked, 0);
    clicked = -1;
    d.press(plat::KEY_DOWN);
    d.press(plat::KEY_RIGHT);  // from Second to the item on its right
    d.press(plat::KEY_ENTER);
    CHECK_EQ(clicked, 3);
    clicked = -1;
    d.press(plat::KEY_LEFT);
    d.press(plat::KEY_UP);
    d.press(plat::KEY_ENTER);
    CHECK_EQ(clicked, 0);
    // A mouse user's first arrow press only shows where the focus is.
    clicked = -1;
    d.move(10, 10);
    CHECK(!keyboardMode());
    d.press(plat::KEY_DOWN);
    CHECK(keyboardMode());
    d.press(plat::KEY_ENTER);
    CHECK_EQ(clicked, 0);
}

// The mouse: hovering gives the focus, a click is a press and a release on the same item, a
// mouse-only item is clicked but never focused, and a press dragged off the item clicks nothing.
TEST(ui_widgets_mouse_clicks_and_focus) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    int clicked = -1, captured = 0;
    d.screen = [&] {
        if (button("Left", Rect(300, 400, 300, 60))) clicked = 0;
        if (button("Right", Rect(800, 400, 300, 60))) clicked = 1;
        if (button("Hud", Rect(1300, 400, 300, 60), ButtonKind::Primary, true, ITEM_MOUSE_ONLY)) clicked = 9;
        if (captured == 1) captureMouseRect(Rect(0, 0, 200, 200));
        if (captured == 2) {
            captureMouseAll();
            captureKeyboard();
        }
    };
    d.frames(2);
    REQUIRE(d.hover("Right"));
    d.press(plat::KEY_ENTER);
    CHECK_EQ(clicked, 1);   // hovering focused it
    clicked = -1;
    REQUIRE(d.click("Left"));
    CHECK_EQ(clicked, 0);
    clicked = -1;
    REQUIRE(d.click("Hud"));
    CHECK_EQ(clicked, 9);
    clicked = -1;
    d.press(plat::KEY_ENTER);   // the focus stayed on Left
    CHECK_EQ(clicked, 0);
    clicked = -1;
    d.pressAt(450, 430);
    d.move(950, 430);
    d.release();
    CHECK_EQ(clicked, -1);
    // What the UI takes (ui::wantsMouse / wantsKeyboard read the frame before).
    d.move(100, 100);
    CHECK(!ui::wantsMouse());
    CHECK(!ui::wantsKeyboard());
    captured = 1;
    d.frame();
    CHECK(ui::wantsMouse());
    d.move(500, 500);
    CHECK(!ui::wantsMouse());   // outside the captured rectangle
    captured = 2;
    d.frame();
    CHECK(ui::wantsMouse());
    CHECK(ui::wantsKeyboard());
}

// The form rows: a toggle flips on a click or Enter; a slider follows the mouse along its track,
// in steps, and the arrow keys step it; a stepper steps on its minus and plus and repeats while
// held; a selector goes back and forth on its arrows and cycles on its value. Disabled rows ignore
// all of it.
TEST(ui_widgets_form_rows_change_their_values) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    uitest::Sounds sounds;
    bool on = false, locked = true;
    float volume = 0.0f;
    int step = 0, sel = 0;
    bool enabled = true;
    const std::vector<std::string> options = {"Alpha", "Beta", "Gamma"};
    d.screen = [&] {
        toggleRow("Toggle##t", on, Rect(200, 100, 1000, 52), enabled);
        toggleRow("Locked##l", locked, Rect(200, 160, 1000, 52), false);
        sliderRow("Volume##v", volume, 0.0f, 1.0f, 0.1f, [](float v) { return std::to_string(int(v * 100.0f + 0.5f)); },
                  Rect(200, 220, 1000, 52), enabled);
        stepperRow("Steps##s", step, 50, [](int i) { return std::to_string(i); }, Rect(200, 280, 1000, 52), enabled);
        selectorRow("Choice##c", sel, options, Rect(200, 340, 1000, 52), enabled);
    };
    d.frames(2);
    d.clickAt(400, 126);
    CHECK(on);
    CHECK(sounds.count(ui::Sound::Toggle) >= 1);
    d.press(plat::KEY_ENTER);   // the row has the focus since the mouse went over it
    CHECK(!on);
    d.clickAt(400, 186);
    CHECK(locked);
    // The slider's track runs from 47 % of the row to its value column (x 670 to 1040 here).
    d.clickAt(855, 246);
    CHECK(std::fabs(volume - 0.5f) < 1e-4f);
    d.pressAt(855, 246);
    d.move(1500, 246);           // dragged past the end, the button still held
    d.release();
    CHECK(std::fabs(volume - 1.0f) < 1e-4f);
    d.press(plat::KEY_LEFT);     // focused by the mouse: the arrows step it
    d.press(plat::KEY_LEFT);
    CHECK(std::fabs(volume - 0.8f) < 1e-4f);
    CHECK(sounds.count(ui::Sound::Tick) >= 1);
    // The stepper: minus at x 963, plus at x 1163.
    d.clickAt(1163, 306);
    d.clickAt(1163, 306);
    CHECK_EQ(step, 2);
    d.clickAt(963, 306);
    CHECK_EQ(step, 1);
    d.pressAt(1163, 306);        // held: repeats after 0.4 s, faster and faster
    d.wait(1.0f);
    d.release();
    CHECK(step >= 8);
    CHECK(step <= 14);
    const int held = step;
    d.press(plat::KEY_RIGHT);
    CHECK_EQ(step, held + 1);
    // The selector: its box spans x 928 to 1178, the arrows at its ends.
    d.clickAt(1166, 366);
    CHECK_EQ(sel, 1);
    d.clickAt(940, 366);
    CHECK_EQ(sel, 0);
    d.clickAt(940, 366);         // nothing before the first
    CHECK_EQ(sel, 0);
    d.clickAt(1053, 366);
    d.clickAt(1053, 366);
    d.clickAt(1053, 366);        // the value cycles
    CHECK_EQ(sel, 0);
    d.press(plat::KEY_RIGHT);
    d.press(plat::KEY_RIGHT);
    d.press(plat::KEY_RIGHT);    // clamped at the last
    CHECK_EQ(sel, 2);
    // Disabled, none of them moves.
    enabled = false;
    d.frame();
    d.clickAt(400, 126);
    d.clickAt(855, 246);
    d.clickAt(1163, 306);
    d.clickAt(1166, 366);
    CHECK(!on);
    CHECK(std::fabs(volume - 0.8f) < 1e-4f);
    CHECK_EQ(step, held + 1);
    CHECK_EQ(sel, 2);
}

// A text field: a click on its box starts the edit; typed text goes in at the caret, which the
// arrows, Home and End move; Backspace and Delete remove a character; Ctrl+V pastes the clipboard
// on one line (AltGr+V does not); the length is capped; Esc gives the text back as it was.
TEST(ui_widgets_text_field_editing) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    std::string name = "Ann", other = "Bob";
    bool enabled = true;
    int changes = 0;
    d.screen = [&] {
        if (textField("Name##name", name, Rect(200, 300, 1000, 56), 12)) ++changes;
        textField("Other##other", other, Rect(200, 400, 1000, 56), 12, nullptr, enabled);
    };
    d.frames(2);
    CHECK(!editingText());
    d.clickAt(1150, 328);        // the box spans x 698 to 1178: the caret goes to the end
    CHECK(editingText());
    d.type("ie");
    CHECK_EQ(name, std::string("Annie"));
    d.press(plat::KEY_HOME);
    d.type("J");
    CHECK_EQ(name, std::string("JAnnie"));
    d.press(plat::KEY_DELETE);   // the A after the caret
    CHECK_EQ(name, std::string("Jnnie"));
    d.press(plat::KEY_END);
    d.press(plat::KEY_BACKSPACE);
    d.press(plat::KEY_LEFT);
    d.press(plat::KEY_BACKSPACE);
    d.press(plat::KEY_RIGHT);
    d.type("s");
    CHECK_EQ(name, std::string("Jnis"));
    CHECK(changes >= 6);
    // Paste: line breaks and tabs become spaces, control characters go; no more than fits.
    fakeplat::record().clipboard = std::string("a\nb\tc\x01" "defghijklmnop");
    d.press('V', false, true);
    CHECK_EQ(name, std::string("Jnisa b cdef"));   // 12 characters at most
    d.press(plat::KEY_HOME);
    d.press(plat::KEY_DELETE);
    d.press(plat::KEY_DELETE);
    d.press(plat::KEY_DELETE);
    d.press(plat::KEY_DELETE);
    // AltGr (Ctrl + right Alt on Windows) + V types a character on some layouts: not a paste.
    d.in.keyDown[plat::KEY_RALT] = true;
    d.press('V', false, true);
    d.in.keyDown[plat::KEY_RALT] = false;
    CHECK_EQ(name, std::string("a b cdef"));
    // Esc: the text as it was when the edit began, and the edit ends.
    d.type("zz");
    d.press(plat::KEY_ESCAPE);
    CHECK_EQ(name, std::string("Ann"));
    CHECK(!editingText());
    // A click elsewhere ends the edit and keeps the text.
    d.clickAt(1150, 328);
    d.type("a");
    d.clickAt(100, 900);
    CHECK(!editingText());
    CHECK_EQ(name, std::string("Anna"));
    // A disabled field does not start an edit.
    enabled = false;
    d.frame();
    d.clickAt(1150, 428);
    CHECK(!editingText());
    CHECK_EQ(other, std::string("Bob"));
}

// The text fields of a page form one form: Tab goes to the next field and starts its edit
// (Shift+Tab the previous one, wrapping around), Enter too, and Enter in the last field activates
// the page's submit button.
TEST(ui_widgets_tab_and_enter_go_from_field_to_field) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    std::string a = "one", b = "two", c = "three";
    int submitted = 0;
    d.screen = [&] {
        textField("A##a", a, Rect(200, 200, 1000, 56));
        formField("B##b", b, Rect(200, 280, 1000, 56), 24, FIELD_SECRET, "secret");
        formField("C##c", c, Rect(200, 360, 1000, 56), 24, FIELD_LTR, "placeholder");
        if (button("Send##send", Rect(800, 460, 300, 60), ButtonKind::Primary, true, ITEM_SUBMIT)) ++submitted;
    };
    d.frames(2);
    d.press(plat::KEY_TAB);       // no field edited: the first one
    d.frame();
    CHECK(editingText());
    d.type("!");
    CHECK_EQ(a, std::string("one!"));
    d.press(plat::KEY_TAB);
    d.frame();
    d.type("?");
    CHECK_EQ(b, std::string("two?"));
    d.press(plat::KEY_TAB, true); // Shift+Tab: back to the first
    d.frame();
    d.type("1");
    CHECK_EQ(a, std::string("one!1"));
    d.press(plat::KEY_TAB, true); // from the first one: the last
    d.frame();
    d.type("3");
    CHECK_EQ(c, std::string("three3"));
    CHECK_EQ(submitted, 0);
    d.press(plat::KEY_ENTER);     // in the last field: the page submits
    d.frame();
    CHECK_EQ(submitted, 1);
    CHECK(!editingText());
    // Enter in the first field goes on to the second one.
    d.press(plat::KEY_TAB);
    d.frame();
    d.press(plat::KEY_ENTER);
    d.frame();
    d.type("x");
    CHECK_EQ(b, std::string("two?x"));
    CHECK_EQ(submitted, 1);
    // An empty field shows its placeholder (and the secret field its dots) without an edit.
    c.clear();
    d.press(plat::KEY_ESCAPE);
    d.frames(2);
    CHECK(!editingText());
    CHECK(c.empty());
}

// A right-to-left name in a text field: Left moves the caret forward in the text (the way the
// arrow points on screen), and a click in the box places the caret where it lands.
TEST(ui_widgets_text_field_caret_in_right_to_left_text) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    std::string name = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";   // سلام
    d.screen = [&] { textField("Name##rtl", name, Rect(200, 300, 1000, 56), 20); };
    d.frames(2);
    d.clickAt(1150, 328);
    REQUIRE(editingText());
    d.press(plat::KEY_HOME);
    d.press(plat::KEY_LEFT);     // one character forward in a right-to-left text
    d.type("x");
    CHECK_EQ(name, std::string("\xD8\xB3" "x" "\xD9\x84\xD8\xA7\xD9\x85"));
    d.press(plat::KEY_ESCAPE);
    // A click at the right end of the box puts the caret at the start of the text (it reads from
    // the right).
    d.clickAt(1170, 328);
    d.type("y");
    CHECK_EQ(name, std::string("y\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85"));
    d.press(plat::KEY_ESCAPE);
}

// The tab bar: a click on a tab, PageUp / PageDown from anywhere, Left / Right while focused; long
// labels are tightened to fit.
TEST(ui_widgets_tab_bar) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    int current = 0, wide = 0;
    bool changed = false;
    const std::vector<std::string> tabs = {"One", "Two", "Three"};
    const std::vector<std::string> many(9, "A rather long tab label");
    d.screen = [&] {
        if (tabBar(tabs, current, Rect(200, 100, 1000, 50))) changed = true;
        pushId("wide");
        tabBar(many, wide, Rect(200, 600, 600, 50));
        popId();
    };
    d.frames(2);
    // The three labels are centred in the bar: "Three" is the right-most, near x 860.
    d.clickAt(860, 125);
    CHECK_EQ(current, 2);
    CHECK(changed);
    d.press(plat::KEY_PAGEUP);
    CHECK_EQ(current, 1);
    d.press(plat::KEY_PAGEDOWN);
    d.press(plat::KEY_PAGEDOWN);   // clamped
    CHECK_EQ(current, 2);
    d.move(700, 125);              // the focus on the bar: the arrows change the tab
    d.press(plat::KEY_LEFT);
    CHECK_EQ(current, 1);
    d.press(plat::KEY_RIGHT);
    CHECK_EQ(current, 2);
    // PageUp / PageDown went to the other bar too (they work from anywhere); the arrows did not.
    CHECK_EQ(wide, 2);
}

// A tooltip shows (in the top layer, one more draw) once the mouse has rested on its item a
// moment, or the keyboard focus stayed on it; an info mark after a form row's label shows its
// text when the mouse rests on the label or the mark, and a click on the mark does not operate
// the row.
TEST(ui_widgets_tooltips_and_info_marks) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    bool sound = false, music = false;
    d.screen = [&] {
        button("Tip##tip", Rect(200, 100, 300, 60));
        tooltip("What this button does");
        beginInfoMarks();
        toggleRow("Sound##snd", sound, Rect(200, 300, 1000, 52));
        tooltip("Plays the sounds of the game");
        const float used = formLabel("Music##mus", Rect(200, 400, 1000, 52), 300.0f);
        toggleRow("##music", music, Rect(200 + used, 400, 1000 - used, 52));
        tooltip("Plays music");   // after a row without a label: for the keyboard focus only
        endInfoMarks();
    };
    d.move(1800, 1000);
    const uint64_t plain = d.drawsOfFrame();
    REQUIRE(d.hover("Tip"));
    d.wait(0.3f);
    CHECK_EQ(d.drawsOfFrame(), plain);   // not yet
    d.wait(0.5f);
    CHECK_EQ(d.drawsOfFrame(), plain + 1);
    // The label of the Sound row ends near x 300; its mark is 21.5 units after it.
    const float labelW = ui::gfx::textWidth("Sound", [] {
        ui::gfx::TextStyle st;
        st.face = ui::font::FACE_TEXT;
        st.size = 27.0f;
        return st;
    }());
    const float markX = 200.0f + 22.0f + labelW + 21.5f;
    d.move(markX, 326);
    d.wait(0.3f);
    CHECK_EQ(d.drawsOfFrame(), plain + 1);
    d.clickAt(markX, 326);
    CHECK(!sound);                       // the mark explains, it does not toggle
    d.clickAt(900, 326);
    CHECK(sound);
    d.move(240, 326);                    // on the label: the tip after a longer rest
    d.wait(0.2f);
    CHECK_EQ(d.drawsOfFrame(), plain);
    d.wait(0.4f);
    CHECK_EQ(d.drawsOfFrame(), plain + 1);
    // The keyboard: the focused row shows its tip after a moment.
    d.move(1800, 1000);
    d.press(plat::KEY_DOWN);
    d.press(plat::KEY_DOWN);
    d.wait(1.0f);
    CHECK_EQ(d.drawsOfFrame(), plain + 1);
}

// The confirmation dialog: modal (the page under it neither takes the keyboard nor the mouse), the
// focus on Cancel, Esc cancels (not on the frame it opens), and each button gives its answer.
TEST(ui_widgets_confirm_dialog) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    uitest::Sounds sounds;
    int answer = -2, behind = 0;
    bool open = true;
    d.screen = [&] {
        if (button("Behind##behind", Rect(760, 520, 400, 60))) ++behind;
        if (open) {
            const int r = confirmDialog("##test.confirm", "Resign?", "The game will be lost.", "Resign", "Cancel", true);
            if (r >= 0) {
                answer = r;
                open = false;
            }
        }
    };
    d.press(plat::KEY_ESCAPE);   // the frame it opens: Esc is not taken
    CHECK_EQ(answer, -2);
    d.frames(30);
    REQUIRE(d.has("Resign"));
    REQUIRE(d.has("Cancel"));
    d.clickAt(960, 550);         // "Behind" is under the dialog
    CHECK_EQ(behind, 0);
    d.press(plat::KEY_ENTER);    // the focus is on Cancel
    CHECK_EQ(answer, 0);
    open = true;
    d.frames(30);
    d.press(plat::KEY_ESCAPE);
    CHECK_EQ(answer, 0);
    answer = -2;
    open = true;
    d.frames(30);
    REQUIRE(d.click("Resign"));
    CHECK_EQ(answer, 1);
    CHECK(sounds.count(ui::Sound::Confirm) >= 1);
    CHECK(sounds.count(ui::Sound::Back) >= 1);
    CHECK_EQ(behind, 0);
}

// A disabled button with a reason never activates; the mouse resting on it shows the reason.
// Long labels are shrunk then cut with an ellipsis.
TEST(ui_widgets_disabled_buttons_and_long_labels) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    d.screen = [&] {
        disabledButton("Save##save", Rect(400, 400, 300, 60), ButtonKind::Secondary, "Sign in first", Rect(300, 300, 800, 400));
        button("A label far too long for the narrow frame it is given##long", Rect(400, 600, 200, 60), ButtonKind::Primary);
        button("Quiet##quiet", Rect(400, 700, 200, 40), ButtonKind::Quiet);
    };
    d.move(1800, 1000);
    const uint64_t plain = d.drawsOfFrame();
    REQUIRE(d.count("Save") == 2);   // the button and the item of its tip
    d.clickAt(550, 430);
    d.press(plat::KEY_ENTER);
    d.wait(0.8f);
    CHECK_EQ(d.drawsOfFrame(), plain + 1);   // the reason shows
    // Labels: shrunk to fit, then cut.
    const std::string longLabel = "A label far too long for the narrow frame it is given";
    CHECK(!buttonLabelFits(longLabel, 200.0f));
    CHECK(buttonLabelFits("Back", 200.0f));
    CHECK(buttonWidthFor("Back##x") < buttonWidthFor(longLabel));
    ui::gfx::TextStyle st;
    st.size = 22.0f;
    const std::string cut = elideToFit(longLabel, st, 150.0f);
    CHECK(cut.size() < longLabel.size());
    CHECK(cut.compare(cut.size() - 3, 3, "\xE2\x80\xA6") == 0);
    CHECK(ui::gfx::textWidth(cut, st) <= 150.0f);
    CHECK_EQ(elideToFit("Back", st, 150.0f), std::string("Back"));
}

// Items under a block (a dialog's page) draw but do not react, and the keys and the wheel read
// nothing there; an item a higher layer occluded on the frame before is not hovered.
TEST(ui_widgets_blocked_and_occluded_items) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    int clicked = 0;
    bool block = true, occludeIt = false;
    bool keySeen = false, backSeen = false;
    float wheelSeen = 0.0f;
    d.screen = [&] {
        if (block) pushBlock();
        CHECK_EQ(blocked(), block);
        if (button("Under##under", Rect(400, 400, 300, 60))) ++clicked;
        keySeen = keySeen || keyPressed(plat::KEY_ENTER);
        backSeen = backSeen || consumeBack();
        wheelSeen += wheel();
        int dx = 0, dy = 0;
        if (consumeNavigation(&dx, &dy)) keySeen = true;
        if (block) popBlock();
        popBlock();   // one too many: harmless
        if (occludeIt) {
            ui::gfx::setLayer(ui::gfx::LAYER_MODAL);
            occlude(Rect(300, 300, 600, 300));
            ui::gfx::setLayer(ui::gfx::LAYER_MAIN);
        }
    };
    d.frames(2);
    d.clickAt(550, 430);
    d.press(plat::KEY_ENTER);
    d.press(plat::KEY_ESCAPE);
    d.press(plat::KEY_DOWN);
    d.wheel(2.0f);
    CHECK_EQ(clicked, 0);
    CHECK(!keySeen);
    CHECK(!backSeen);
    CHECK_EQ(wheelSeen, 0.0f);
    block = false;
    occludeIt = true;
    d.frames(2);
    d.clickAt(550, 430);
    d.wheel(1.0f);
    CHECK_EQ(clicked, 0);
    CHECK_EQ(wheelSeen, 0.0f);
    occludeIt = false;
    d.frames(2);
    d.clickAt(550, 430);
    d.wheel(1.0f);
    d.press(plat::KEY_ESCAPE);
    CHECK_EQ(clicked, 1);
    CHECK_EQ(wheelSeen, 1.0f);
    CHECK(backSeen);
}

// The UI sounds: one per event, a hover sound at most every 30 ms and a tick every 45 ms (a
// slider dragged across its whole track does not rattle), none without a callback.
TEST(ui_widgets_sounds_are_throttled) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    float v = 0.0f;
    d.screen = [&] {
        menuEntry("Entry##e1", Rect(200, 100, 500, 56));
        menuEntry("Centre##e2", Rect(200, 200, 500, 56), true, ui::gfx::HAlign::Center);
        menuEntry("Right##e3", Rect(200, 300, 500, 56), true, ui::gfx::HAlign::Right);
        menuEntry("Disabled##e4", Rect(200, 400, 500, 56), false);
        sliderRow("Slider##sl", v, 0.0f, 100.0f, 1.0f, nullptr, Rect(200, 600, 1000, 52));
    };
    d.frames(2);
    {
        uitest::Sounds sounds;
        d.dt = 0.01f;            // 10 ms frames
        d.move(400, 128);
        d.move(400, 228);        // another entry 10 ms later: no second hover sound
        CHECK_EQ(sounds.count(ui::Sound::Hover), 1);
        d.move(400, 328);
        d.move(400, 128);
        d.move(400, 228);
        CHECK(sounds.count(ui::Sound::Hover) >= 2);
        CHECK(sounds.count(ui::Sound::Hover) <= 3);
        const size_t before = sounds.played.size();
        REQUIRE(d.click("Centre"));
        CHECK(sounds.count(ui::Sound::Click) == 1);
        CHECK(sounds.played.size() > before);
        d.clickAt(400, 428);     // disabled: silent
        CHECK(sounds.count(ui::Sound::Click) == 1);
        const int ticks = sounds.count(ui::Sound::Tick);
        d.pressAt(670, 626);
        for (int x = 680; x <= 1040; x += 20) d.move(float(x), 626);   // 19 changes in 190 ms
        d.release();
        CHECK(v > 99.0f);
        CHECK(sounds.count(ui::Sound::Tick) - ticks <= 6);
        CHECK(sounds.count(ui::Sound::Tick) - ticks >= 2);
        d.dt = 1.0f / 60.0f;
    }
    // No callback: nothing to call (and nothing breaks).
    REQUIRE(d.click("Entry"));
}

// In a right-to-left language the rows are mirrored: the label on the right, the control on the
// left, and the arrow keys go the way they point (Left is forward in a selector, up in a slider).
TEST(ui_widgets_right_to_left_rows) {
    REQUIRE(uitest::app());
    English english;
    REQUIRE(i18n::setLanguage("ar"));
    uitest::Driver d;
    int sel = 0, step = 5;
    float v = 0.5f;
    const std::vector<std::string> options = {"1", "2", "3"};
    d.screen = [&] {
        selectorRow("Choice##rtl.sel", sel, options, Rect(200, 100, 1000, 52));
        stepperRow("Steps##rtl.step", step, 10, nullptr, Rect(200, 200, 1000, 52));
        sliderRow("Level##rtl.slider", v, 0.0f, 1.0f, 0.1f, nullptr, Rect(200, 300, 1000, 52));
    };
    d.frames(2);
    CHECK(rtl());
    CHECK(startAlign() == ui::gfx::HAlign::Right);
    CHECK(endAlign() == ui::gfx::HAlign::Left);
    const Rect area(100, 0, 1000, 100);
    CHECK_EQ(flipX(area, 150.0f), 1050.0f);
    CHECK_EQ(flip(area, Rect(100, 10, 200, 20)).x, 900.0f);
    // The selector's box is now at the left end: its right arrow (x 264..304) goes back.
    d.move(600, 126);
    d.press(plat::KEY_LEFT);
    CHECK_EQ(sel, 1);
    d.clickAt(220 + 2.0f, 126);   // the left arrow: forward
    CHECK_EQ(sel, 2);
    d.move(600, 226);
    d.press(plat::KEY_LEFT);
    CHECK_EQ(step, 6);
    d.move(600, 326);
    d.press(plat::KEY_LEFT);
    CHECK(std::fabs(v - 0.6f) < 1e-4f);
    i18n::setLanguage("en");
    d.frame();
    CHECK(!rtl());
    CHECK(startAlign() == ui::gfx::HAlign::Left);
    CHECK_EQ(flipX(area, 150.0f), 150.0f);
}

// The public calls of ui.h for the game's HUD: a panel takes the mouse over it, a button is mouse
// only (Enter and Space are the game's), text in each style and alignment.
TEST(ui_widgets_public_hud_calls) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    int pressed = 0;
    d.screen = [&] {
        ui::panel(m::vec2(100, 100), m::vec2(400, 200));
        if (ui::button("Offer draw", m::vec2(150, 150), m::vec2(240, 56))) ++pressed;
        ui::button("Off", m::vec2(150, 220), m::vec2(240, 56), true, false);
        ui::text("Regular", m::vec2(600, 100), 30.0f, m::vec4(1, 1, 1, 1));
        ui::text("Title", m::vec2(600, 150), 30.0f, m::vec4(1, 1, 1, 1), ui::Align::Center, ui::FontStyle::Title, 0.2f);
        ui::text("Italic", m::vec2(600, 200), 30.0f, m::vec4(1, 1, 1, 1), ui::Align::Right, ui::FontStyle::Italic);
    };
    d.frames(2);
    CHECK_EQ(ui::viewSize().y, 1080.0f);
    CHECK_EQ(ui::viewSize().x, 1920.0f);
    CHECK_EQ(ui::pixelScale(), 1.0f);
    REQUIRE(d.hover("Offer draw"));
    d.press(plat::KEY_ENTER);
    CHECK_EQ(pressed, 0);
    CHECK(ui::wantsMouse());
    REQUIRE(d.click("Offer draw"));
    CHECK_EQ(pressed, 1);
    d.move(300, 280);
    CHECK(ui::wantsMouse());   // over the panel
    d.move(1500, 800);
    CHECK(!ui::wantsMouse());
    // A smaller window: the canvas stays 1080 units tall.
    d.width = 1280;
    d.height = 720;
    d.frame();
    CHECK_EQ(ui::viewSize().y, 1080.0f);
    CHECK(std::fabs(ui::pixelScale() - 720.0f / 1080.0f) < 1e-5f);
}

// Copying to the clipboard (the direct match's invitation, the recovery codes): the platform
// layer's clipboard, marked sensitive when asked (kept out of the clipboard history).
TEST(ui_widgets_copy_to_clipboard) {
    fakeplat::record().clipboard.clear();
#ifdef _WIN32
    // The tests' platform layer has no Win32 clipboard to open: the copy reports its failure.
    CHECK(!ui::detail::setClipboardText("ABCD-EFGH", true));
#else
    CHECK(ui::detail::setClipboardText("ABCD-EFGH\nline two", true));
    CHECK_EQ(fakeplat::record().clipboard, std::string("ABCD-EFGH\nline two"));
    CHECK(fakeplat::record().clipboardSensitive);
    CHECK(ui::detail::setClipboardText("scacelith://join", false));
    CHECK(!fakeplat::record().clipboardSensitive);
#endif
    fakeplat::record().clipboard.clear();
}
