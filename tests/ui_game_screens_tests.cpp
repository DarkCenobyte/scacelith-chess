// The screens over the table (src/ui/ui_screens.cpp, src/ui/ui_screens_game.cpp,
// src/ui/ui_hotseat.cpp) driven frame by frame (tests/ui_driver.h): the pause menu with its
// confirmations and its Options page, the promotion picker, the notifications, the game over card
// (folded and unfolded, with the extras of the other modes), the move list, the loading screen, the
// viewer's pause menu and overlay, the replay's bar, the game's pointer and the hot-seat overlay
// with its draw offer card. What a screen shows is read from its widgets and from the draw calls of
// the null OpenGL (one per layer and clip rectangle in a frame).
#include "test.h"
#include "ui_driver.h"
#include "chess/chess.h"
#include "game/settings.h"
#include "i18n/i18n.h"
#include <cmath>
#include <string>
#include <vector>

using ui::MenuAction;
using ui::gfx::Rect;

namespace {

std::string T(const char* key) { return i18n::tr(key); }

// The time a card takes to come in (0.6 s at most) and a little more.
void settle(uitest::Driver& d) { d.frames(45); }

int count(const std::vector<MenuAction>& v, MenuAction a) {
    int n = 0;
    for (MenuAction x : v) n += x == a ? 1 : 0;
    return n;
}

// The settings (the options page applies to them) and the language, restored when a test ends.
struct Restore {
    game::Settings settings = game::settings();
    ~Restore() {
        i18n::setLanguage("en");
        game::settings() = settings;
    }
};

}  // namespace

// The pause menu: Resume, Offer draw and Claim draw (greyed out when not possible), Resign and
// Main menu after a confirmation, Options on its own page; Esc resumes, but not on the frame the
// menu appears (the Esc that opened it); while a confirmation is up, the menu below is blocked.
TEST(ui_game_screens_pause_menu) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    std::vector<MenuAction> acts;
    bool shown = true, claim = false, offer = true, canResign = true;
    std::string question;
    d.screen = [&] {
        if (!shown) return;
        MenuAction a = question.empty() ? ui::pauseMenu(claim, offer) : ui::pauseMenu(claim, offer, question, canResign);
        if (a != MenuAction::None) acts.push_back(a);
    };
    settle(d);
    CHECK(d.disabled(T("pause.claim_draw")));
    CHECK(!d.disabled(T("pause.offer_draw")));
    REQUIRE(d.click(T("pause.resume")));
    CHECK_EQ(count(acts, MenuAction::Resume), 1);
    REQUIRE(d.click(T("pause.offer_draw")));
    CHECK_EQ(count(acts, MenuAction::OfferDraw), 1);
    REQUIRE(d.click(T("pause.claim_draw")));   // greyed out
    CHECK_EQ(count(acts, MenuAction::ClaimDraw), 0);
    claim = true;
    offer = false;
    d.frame();
    CHECK(d.disabled(T("pause.offer_draw")));
    REQUIRE(d.click(T("pause.claim_draw")));
    CHECK_EQ(count(acts, MenuAction::ClaimDraw), 1);
    // Resign: asked first. Cancel, then confirmed (the dialog's button is drawn after the entry
    // of the same name).
    REQUIRE(d.click(T("pause.resign")));
    d.frames(20);
    REQUIRE(d.has(T("common.cancel")));
    REQUIRE(d.click(T("pause.resume")));   // the menu is blocked under the question
    CHECK_EQ(count(acts, MenuAction::Resume), 1);
    REQUIRE(d.click(T("common.cancel")));
    CHECK(!d.has(T("common.cancel")));
    CHECK_EQ(count(acts, MenuAction::Resign), 0);
    REQUIRE(d.click(T("pause.resign")));
    d.frames(20);
    REQUIRE(d.clickLast(T("confirm.resign.ok")));
    CHECK_EQ(count(acts, MenuAction::Resign), 1);
    // Main menu: the leave question, Esc cancels it, then confirmed.
    REQUIRE(d.click(T("common.main_menu")));
    d.frames(20);
    REQUIRE(d.has(T("confirm.leave.ok")));
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(!d.has(T("confirm.leave.ok")));
    CHECK_EQ(count(acts, MenuAction::Resume), 1);   // that Esc only closed the question
    REQUIRE(d.click(T("common.main_menu")));
    d.frames(20);
    REQUIRE(d.click(T("confirm.leave.ok")));
    CHECK_EQ(count(acts, MenuAction::BackToMainMenu), 1);
    // Esc resumes...
    d.press(plat::KEY_ESCAPE);
    CHECK_EQ(count(acts, MenuAction::Resume), 2);
    // ...but not the Esc of the frame it appears on.
    shown = false;
    d.frame();
    shown = true;
    d.press(plat::KEY_ESCAPE);
    CHECK_EQ(count(acts, MenuAction::Resume), 2);
    // Options: its page in place of the menu, Back returns to the menu.
    settle(d);
    REQUIRE(d.click(T("menu.options")));
    d.frames(25);
    CHECK(d.has(T("common.apply")));
    CHECK(!d.has(T("pause.resume")));
    CHECK(ui::optionsOpen());
    REQUIRE(d.click(T("common.back")));
    d.frame();
    CHECK(d.has(T("pause.resume")));
    // Hot-seat: the question names the player; Resign greyed out while a move is on its way.
    question = "Resign for Alice? Bob will be declared the winner.";
    canResign = false;
    d.frames(2);
    CHECK(d.disabled(T("pause.resign")));
    canResign = true;
    d.frames(2);
    REQUIRE(d.click(T("pause.resign")));
    d.frames(20);
    REQUIRE(d.clickLast(T("confirm.resign.ok")));
    CHECK_EQ(count(acts, MenuAction::Resign), 2);
    // Opened on a question (the ui viewer's hook).
    shown = false;
    d.frame();
    ui::debug::openPauseConfirm(2);
    shown = true;
    d.frames(20);
    CHECK(d.has(T("confirm.leave.ok")));
}

// The promotion picker: a click on a figure, or its letter, returns the piece; Enter takes the
// focused one (the queen at first); for Black the same choices.
TEST(ui_game_screens_promotion_picker) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    uitest::Sounds sounds;
    int result = 0;
    bool shown = true, white = true;
    d.screen = [&] {
        if (!shown) return;
        int r = ui::promotionPicker(white);
        if (r) result = r;
    };
    settle(d);
    CHECK_EQ(result, 0);
    CHECK(sounds.count(ui::Sound::Open) >= 1);
    const std::vector<Rect> tiles = d.bare([](const ui::im::TracedItem& it) { return it.r.h == 204.0f; });
    REQUIRE(tiles.size() == 4u);
    CHECK(tiles[0].x < tiles[1].x);
    d.clickRect(tiles[2]);
    CHECK_EQ(result, int(chess::Bishop));
    CHECK(sounds.count(ui::Sound::Confirm) >= 1);
    d.press('N');
    CHECK_EQ(result, int(chess::Knight));
    d.press('R');
    CHECK_EQ(result, int(chess::Rook));
    d.press('Q');
    CHECK_EQ(result, int(chess::Queen));
    // The keyboard: Enter takes the figure in focus (the last one the mouse went over), the arrows
    // move along the row.
    result = 0;
    d.move(tiles[1].cx(), tiles[1].cy());
    d.move(1.0f, 1.0f);
    ui::im::setKeyboardMode(true);
    d.press(plat::KEY_ENTER);
    CHECK_EQ(result, int(chess::Rook));
    d.press(plat::KEY_LEFT);
    d.press(plat::KEY_ENTER);
    ui::im::setKeyboardMode(false);
    CHECK_EQ(result, int(chess::Queen));
    // Black's figures.
    shown = false;
    d.frame();
    white = false;
    shown = true;
    settle(d);
    result = 0;
    d.clickRect(d.bare([](const ui::im::TracedItem& it) { return it.r.h == 204.0f; })[3]);
    CHECK_EQ(result, int(chess::Knight));
}

// Notifications: shown for their time and a fade, the same message again extends it instead of
// stacking a copy, four at most (the oldest goes), a long one shrunk and cut to fit.
TEST(ui_game_screens_notifications) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    d.screen = [] { ui::drawNotifications(); };
    CHECK_EQ(d.drawsOfFrame(), 0u);
    ui::notify("Draw offer declined", 1.0f);
    d.frames(5);
    CHECK_EQ(d.drawsOfFrame(), 1u);   // the overlay layer
    d.wait(1.2f);
    CHECK_EQ(d.drawsOfFrame(), 1u);   // fading out
    d.wait(0.5f);
    CHECK_EQ(d.drawsOfFrame(), 0u);   // gone after its time and 0.6 s
    // Repeated before it ends: it stays the full time again.
    ui::notify("Threefold repetition", 1.0f);
    d.wait(1.2f);
    ui::notify("Threefold repetition", 1.0f);
    d.wait(1.0f);
    CHECK_EQ(d.drawsOfFrame(), 1u);
    d.wait(0.6f);
    CHECK_EQ(d.drawsOfFrame(), 0u);
    // A fifth message removes the first, however long it had to stay.
    ui::notify("A long one", 30.0f);
    for (const char* m : {"B", "C", "D", "E"}) ui::notify(m, 0.2f);   // 0.5 s at least
    d.wait(1.5f);
    CHECK_EQ(d.drawsOfFrame(), 0u);
    // Too long for the window: shrunk, then cut.
    ui::notify(std::string(400, 'W'), 1.0f);
    d.frames(5);
    CHECK_EQ(d.drawsOfFrame(), 1u);
}

// The game over card: Rematch and Main menu; "View the board" (or Esc) folds it into a bar whose
// Show (or Esc) unfolds it; the extras of the other modes (another line, a detail line, another
// primary button, greyed out, a report button).
TEST(ui_game_screens_game_over_card) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    std::vector<MenuAction> acts;
    ui::GameOverExtras extras;
    std::string result = "1-0", reason = "Checkmate";
    bool shown = true, won = true, drawn = false;
    int moves = 34;
    d.screen = [&] {
        if (!shown) return;
        MenuAction a = ui::gameOver(result, reason, won, drawn, moves, extras);
        if (a != MenuAction::None) acts.push_back(a);
    };
    settle(d);
    CHECK(!ui::gameOverFolded());
    // A reason too long for one line takes two: the card grows by a line, around its centre.
    REQUIRE(d.has(T("common.main_menu")));
    const float oneLine = d.find(T("common.main_menu"))->r.y;
    reason = T("reason.illegal_vs_insufficient");
    d.frames(2);
    REQUIRE(d.has(T("common.main_menu")));
    CHECK(std::fabs(d.find(T("common.main_menu"))->r.y - (oneLine + 16.0f)) < 0.5f);
    reason = "Checkmate";
    d.frames(2);
    REQUIRE(d.click(T("gameover.rematch")));
    CHECK_EQ(count(acts, MenuAction::Rematch), 1);
    REQUIRE(d.click(T("common.main_menu")));
    CHECK_EQ(count(acts, MenuAction::BackToMainMenu), 1);
    // Folded: the bar with Show; the card's buttons gone once it has slid away.
    REQUIRE(d.click(T("gameover.view_board")));
    CHECK(ui::gameOverFolded());
    settle(d);
    CHECK(d.has(T("gameover.show")));
    CHECK(!d.has(T("gameover.rematch")));
    REQUIRE(d.click(T("gameover.show")));
    CHECK(!ui::gameOverFolded());
    settle(d);
    CHECK(d.has(T("gameover.rematch")));
    CHECK(!d.has(T("gameover.show")));
    d.press(plat::KEY_ESCAPE);
    CHECK(ui::gameOverFolded());
    d.press(plat::KEY_ESCAPE);
    CHECK(!ui::gameOverFolded());
    // The lines of a draw and of a loss, with and without the move count.
    drawn = true;
    won = false;
    result = "1/2-1/2";
    d.frames(2);
    moves = 0;
    d.frames(2);
    drawn = false;
    result = "0-1";
    d.frames(2);
    moves = 12;
    d.frames(2);
    CHECK(d.has(T("gameover.rematch")));
    // The extras: a watched game's line and button, an online game's detail and report button,
    // the rematch greyed out (the Main menu takes the focus).
    extras.line = "White wins.";
    extras.detail = "Elo 1512 \xE2\x86\x92 1524 (+12)";
    extras.primaryLabel = "Watch again";
    extras.primaryDisabled = true;
    extras.reportLabel = "Report opponent";
    d.frames(2);
    CHECK(!d.has(T("gameover.rematch")));
    CHECK(d.disabled("Watch again"));
    REQUIRE(d.click("Watch again"));
    CHECK_EQ(count(acts, MenuAction::Rematch), 1);
    REQUIRE(d.click("Report opponent"));
    CHECK_EQ(count(acts, MenuAction::Report), 1);
    REQUIRE(d.hover(T("common.main_menu")));
    d.move(1.0f, 1.0f);
    ui::im::setKeyboardMode(true);
    d.press(plat::KEY_ENTER);
    ui::im::setKeyboardMode(false);
    CHECK_EQ(count(acts, MenuAction::BackToMainMenu), 2);
    // Opened folded (the ui viewer's hook).
    shown = false;
    d.frame();
    ui::debug::foldGameOver(true);
    shown = true;
    d.frames(3);
    CHECK(ui::gameOverFolded());
    CHECK(d.has(T("gameover.show")));
}

// The move list: drawn while visible (it fades in and out), its panel takes the mouse; empty, it
// says so; a long game scrolls (the wheel over it).
TEST(ui_game_screens_move_list) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    std::vector<std::string> san = {"e4", "e5", "Nf3", "Nc6", "Bb5"};
    bool visible = false;
    d.screen = [&] { ui::moveList(san, visible); };
    d.frames(3);
    CHECK_EQ(d.drawsOfFrame(), 0u);
    visible = true;
    d.frames(30);
    CHECK(d.drawsOfFrame() >= 1u);
    d.move(1700.0f, 400.0f);   // over the panel (the right edge, under the top bar)
    d.frame();
    CHECK(ui::wantsMouse());
    d.move(400.0f, 400.0f);
    d.frame();
    CHECK(!ui::wantsMouse());
    for (int i = 0; i < 60; ++i) san.push_back(i % 2 ? "Nf6" : "Ng1");
    d.frames(30);
    d.move(1700.0f, 400.0f);
    d.wheel(3.0f);
    d.frames(10);
    d.wheel(-6.0f);
    d.frames(10);
    CHECK(d.drawsOfFrame() >= 1u);
    san.clear();
    d.frames(2);
    CHECK(d.drawsOfFrame() >= 1u);
    visible = false;
    d.frames(40);
    CHECK_EQ(d.drawsOfFrame(), 0u);
}

// The loading screen covers everything and takes the mouse and the keyboard; its bar restarts
// when it is shown again after an absence; in Arabic it fills from the right.
TEST(ui_game_screens_loading_screen) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    float progress = 0.2f;
    bool shown = true;
    d.screen = [&] {
        if (shown) ui::loadingScreen(progress, "Preparing the hall");
    };
    CHECK_EQ(d.drawsOfFrame(), 2u);   // the backdrop layer and the main one
    d.frame();
    CHECK(ui::wantsMouse());
    CHECK(ui::wantsKeyboard());
    progress = 0.9f;
    d.frames(10);
    progress = 0.4f;   // a new stage: the bar goes back at once
    d.frame();
    shown = false;
    d.frames(2);
    shown = true;
    REQUIRE(i18n::setLanguage("ar"));
    CHECK_EQ(d.drawsOfFrame(), 2u);
}

// The viewer's pause menu: Resume, Options (its page in place of the menu), Main menu; Esc resumes.
TEST(ui_game_screens_viewer_pause_menu) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    std::vector<MenuAction> acts;
    d.screen = [&] {
        MenuAction a = ui::viewerPauseMenu();
        if (a != MenuAction::None) acts.push_back(a);
    };
    settle(d);
    REQUIRE(d.click(T("viewer.resume")));
    CHECK_EQ(count(acts, MenuAction::Resume), 1);
    REQUIRE(d.click(T("viewer.main_menu")));
    CHECK_EQ(count(acts, MenuAction::BackToMainMenu), 1);
    d.press(plat::KEY_ESCAPE);
    CHECK_EQ(count(acts, MenuAction::Resume), 2);
    REQUIRE(d.click(T("viewer.options")));
    d.frames(25);
    CHECK(d.has(T("common.apply")));
    CHECK(!d.has(T("viewer.main_menu")));
    d.press(plat::KEY_ESCAPE);   // nothing changed: the page closes
    d.frame();
    CHECK(d.has(T("viewer.main_menu")));
    CHECK_EQ(count(acts, MenuAction::Resume), 2);
}

// The viewer's overlay: nothing when hidden (H); the players, the controls (a replay's keys
// first), a viewpoint just chosen and the speed just changed, which fade out.
TEST(ui_game_screens_viewer_overlay) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    ui::ViewerHud hud;
    hud.white = "Stockfish \xC2\xB7 Expert \xC2\xB7 2100";
    hud.black = "Stockfish \xC2\xB7 Club Player \xC2\xB7 1500";
    hud.visible = false;
    d.screen = [&] { ui::viewerHud(hud); };
    CHECK_EQ(d.drawsOfFrame(), 0u);
    hud.visible = true;
    CHECK_EQ(d.drawsOfFrame(), 1u);
    hud.sideToMove = 1;
    hud.viewpoint = "Black's eyes";
    hud.viewpointAge = 0.5f;
    hud.speed = 2.4f;
    hud.speedAge = 0.3f;
    CHECK_EQ(d.drawsOfFrame(), 1u);
    hud.replay = true;
    hud.viewpointAge = 10.0f;   // faded out
    CHECK_EQ(d.drawsOfFrame(), 1u);
    REQUIRE(i18n::setLanguage("ar"));
    CHECK_EQ(d.drawsOfFrame(), 1u);
}

// The replay's bar: five mouse-only buttons (start, back, pause / resume, forward, end) with their
// tips; greyed out at the ends; above the folded game over bar when it shows; beside the viewer's
// controls when the window is too narrow to centre it.
TEST(ui_game_screens_replay_bar) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    std::vector<ui::ReplayAction> acts;
    ui::ReplayBar bar;
    bar.move = 12;
    bar.moves = 40;
    bar.speed = "\xC3\x97" "2";
    bool hud = false;
    d.screen = [&] {
        if (hud) {
            ui::ViewerHud h;
            h.replay = true;
            ui::viewerHud(h);
        }
        ui::ReplayAction a = ui::replayBar(bar);
        if (a != ui::ReplayAction::None) acts.push_back(a);
    };
    auto buttons = [&] {
        return d.bare([](const ui::im::TracedItem& it) { return (it.flags & ui::im::ITEM_MOUSE_ONLY) != 0; });
    };
    d.frames(2);
    std::vector<Rect> b = buttons();
    REQUIRE(b.size() == 5u);
    const ui::ReplayAction order[5] = {ui::ReplayAction::Start, ui::ReplayAction::Back, ui::ReplayAction::TogglePause,
                                       ui::ReplayAction::Forward, ui::ReplayAction::End};
    for (int i = 0; i < 5; ++i) {
        d.clickRect(b[size_t(i)]);
        REQUIRE(!acts.empty());
        CHECK(acts.back() == order[i]);
    }
    // A tip after a moment over a button (one more draw call: the top layer).
    d.move(b[2].cx(), b[2].cy());
    const uint64_t plain = d.drawsOfFrame();
    d.wait(0.8f);
    CHECK_EQ(d.drawsOfFrame(), plain + 1);
    // The keyboard is the game's: Enter presses nothing.
    acts.clear();
    ui::im::setKeyboardMode(true);
    d.press(plat::KEY_ENTER);
    ui::im::setKeyboardMode(false);
    CHECK(acts.empty());
    // At the start, then at the end (paused: the middle one resumes).
    bar.atStart = true;
    d.frame();
    d.clickRect(b[0]);
    d.clickRect(b[1]);
    CHECK(acts.empty());
    bar.atStart = false;
    bar.atEnd = true;
    d.frame();
    d.clickRect(b[2]);
    d.clickRect(b[3]);
    d.clickRect(b[4]);
    CHECK(acts.empty());
    bar.paused = true;
    d.frame();
    d.clickRect(b[2]);
    REQUIRE(acts.size() == 1u);
    CHECK(acts[0] == ui::ReplayAction::TogglePause);
    // Above the folded game over bar.
    bar.aboveCard = true;
    d.frames(2);
    CHECK(buttons()[0].y < b[0].y - 50.0f);
    bar.aboveCard = false;
    // A 4:3 window: centred, the bar would cover the controls panel on the left; it moves right.
    d.width = 1440;
    d.height = 1080;
    d.frames(2);
    const float centred = buttons()[0].x;
    hud = true;
    d.frames(2);
    CHECK(buttons()[0].x > centred + 20.0f);
}

// The game's own pointer: one shape per kind, on the top layer.
TEST(ui_game_screens_game_pointer) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    ui::GameCursor kind = ui::GameCursor::Idle;
    d.screen = [&] { ui::gameCursor(m::vec2(640.0f, 360.0f), kind); };
    for (ui::GameCursor k : {ui::GameCursor::Idle, ui::GameCursor::Waiting, ui::GameCursor::Piece, ui::GameCursor::Holding,
                             ui::GameCursor::Square, ui::GameCursor::Clock}) {
        kind = k;
        CHECK_EQ(d.drawsOfFrame(), 1u);
    }
}

// Hot-seat: the players with their ratings, the caption of the player whose turn begins (it fades
// out), and the draw offer card, whose mouse-only buttons answer once it is readable; while it
// fades out they are off.
TEST(ui_game_screens_hot_seat_overlay) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    std::vector<ui::HotSeatAction> acts;
    ui::HotSeatHud hud;
    hud.names[0] = "Alice";
    hud.names[1] = "\xD0\x91\xD0\xBE\xD1\x80\xD0\xB8\xD1\x81";   // "Борис"
    hud.ratings[0] = "1512";
    hud.toMove = 0;
    hud.caption = "Alice, your move";
    hud.captionAge = 0.5f;
    d.screen = [&] {
        ui::HotSeatAction a = ui::hotSeatHud(hud);
        if (a != ui::HotSeatAction::None) acts.push_back(a);
    };
    CHECK_EQ(d.drawsOfFrame(), 1u);   // the players and the caption: the main layer
    hud.captionAge = 10.0f;
    CHECK_EQ(d.drawsOfFrame(), 1u);
    CHECK(!d.has(T("hotseat.draw.accept")));
    // The offer: a click as the card appears is not taken.
    hud.toMove = 1;
    hud.drawOffer = true;
    hud.drawOfferText = "Alice offers a draw.";
    d.frame();
    REQUIRE(d.has(T("hotseat.draw.accept")));
    CHECK_EQ(d.drawsOfFrame(), 2u);   // and the overlay layer
    d.clickLast(T("hotseat.draw.accept"));
    CHECK(acts.empty());
    d.wait(0.4f);
    REQUIRE(d.click(T("hotseat.draw.accept")));
    REQUIRE(acts.size() == 1u);
    CHECK(acts[0] == ui::HotSeatAction::AcceptDraw);
    REQUIRE(d.click(T("hotseat.draw.decline")));
    REQUIRE(acts.size() == 2u);
    CHECK(acts[1] == ui::HotSeatAction::DeclineDraw);
    // Not for the keyboard (Space and Enter are the clock's).
    ui::im::setKeyboardMode(true);
    d.press(plat::KEY_ENTER);
    ui::im::setKeyboardMode(false);
    CHECK_EQ(acts.size(), size_t(2));
    // Withdrawn: the card fades out, its buttons off.
    hud.drawOffer = false;
    d.frame();
    REQUIRE(d.has(T("hotseat.draw.decline")));
    d.clickLast(T("hotseat.draw.decline"));
    CHECK_EQ(acts.size(), size_t(2));
    d.wait(1.0f);
    CHECK(!d.has(T("hotseat.draw.decline")));
    // Mirrored in Arabic.
    REQUIRE(i18n::setLanguage("ar"));
    hud.drawOffer = true;
    d.wait(0.5f);
    const ui::im::TracedItem* accept = d.find(T("hotseat.draw.accept"));
    REQUIRE(accept != nullptr);
    CHECK(accept->r.x < 960.0f);   // the card on the left
}
