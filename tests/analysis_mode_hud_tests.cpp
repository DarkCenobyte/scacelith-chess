// The overlay of the Analysis mode (src/ui/ui_analysis.cpp, ui::analysisHud) driven frame by frame
// with made-up ui::AnalysisHud values (tests/ui_driver.h), without the game: the action each
// control returns (start, back, play / pause, forward, end, the comments', voice's and arrows'
// toggles) and the sound it makes, the controls greyed at the ends of the game and the voice's
// toggle without its model (the reason as its tooltip), a click on a move (the position after it),
// the move list kept on the move of the board and scrolled by the wheel, a record that starts with
// Black, a game without moves, the lines under the list (the summary, the review's progress, no
// engine) taking room from it, the evaluation bar left to the mouse and the panel taking it, the
// overlay hidden and shown again, the subtitles' span beside the viewer's controls hint, and the
// panel mirrored in Arabic. The widgets are found by their ids (the controls under
// "analysis.controls", the moves under "analysis.moves" by ply).
#include "test.h"
#include "coach_mode_support.h"
#include "ui_driver.h"
#include "i18n/i18n.h"
#include <string>
#include <vector>

using ui::AnalysisAction;
using ui::gfx::Rect;

namespace {

// The 1920 x 1080 canvas of the driver: the bar at the left edge, the panel 22 % of the width at
// the right edge (ui_analysis.cpp's geometry).
constexpr float kBarRight = 22.0f + 26.0f, kGap = 24.0f;
constexpr float kPanelX = 1920.0f - kGap - 422.0f;

ui::im::Id control(const char* name) { return coachtest::idIn({"analysis.controls"}, name); }
ui::im::Id cell(int ply) { return coachtest::idIn({"analysis.moves"}, ply); }

const ui::im::TracedItem* byId(const uitest::Driver& d, ui::im::Id id) {
    for (const auto& it : d.items())
        if (it.id == id) return &it;
    return nullptr;
}

bool isDisabled(const uitest::Driver& d, const char* name) {
    const ui::im::TracedItem* it = byId(d, control(name));
    return it && (it->flags & ui::im::ITEM_DISABLED) != 0;
}

// The cells of the moves drawn in the last frame, among plies 0 .. n - 1.
std::vector<int> cellsShown(const uitest::Driver& d, int n) {
    std::vector<int> out;
    for (int ply = 0; ply < n; ++ply)
        if (byId(d, cell(ply))) out.push_back(ply);
    return out;
}

// A game of 'plies' moves as the scene fills the overlay, at position 'current': each move with
// an evaluation after it, a few with symbols, the opening named, the review complete.
ui::AnalysisHud gameHud(int plies, int current) {
    static const char* const kSan[] = {"e4", "e5", "Qh5", "Nc6", "Bc4", "Nf6", "Qxf7#"};
    ui::AnalysisHud h;
    h.evalKnown = true;
    h.evalWhite = 0.62f;
    h.evalText = "+0.4";
    h.white = "Stockfish (3190)";
    h.black = "Human";
    h.result = "1-0";
    h.opening = "King's Pawn Game: Wayward Queen Attack";
    for (int i = 0; i < plies; ++i) {
        ui::AnalysisMove m;
        m.san = kSan[i % 7];
        m.nag = i % 7;   // every symbol, none included
        m.known = i % 5 != 4;
        m.white = 0.1f * float(i % 10);
        h.moves.push_back(m);
    }
    h.current = current;
    h.atStart = current == 0;
    h.atEnd = current >= plies;
    h.voiceAvailable = true;
    return h;
}

// The overlay drawn every frame from 'hud', its last result and the actions it returned. The
// viewer's overlay is drawn hidden before it, as the scene does after H: no controls hint (the
// last one a game drew, kept by the UI, would move the subtitles' span).
struct Overlay {
    ui::AnalysisHud hud;
    ui::AnalysisHudResult last;
    std::vector<AnalysisAction> actions;
    std::vector<int> positions;
    void attach(uitest::Driver& d) {
        d.screen = [this] {
            ui::ViewerHud viewer;
            viewer.visible = false;
            ui::viewerHud(viewer);
            last = ui::analysisHud(hud);
            if (last.action != AnalysisAction::None) {
                actions.push_back(last.action);
                positions.push_back(last.position);
            }
        };
    }
    AnalysisAction lastAction() const { return actions.empty() ? AnalysisAction::None : actions.back(); }
};

}  // namespace

// In the middle of a game every control is enabled: each returns its action, the transport
// buttons with a click, the toggles with the toggle's sound. A click on a move asks for the
// position after it. The subtitles get the span between the bar and the panel.
TEST(analysis_mode_hud_controls_and_moves_return_their_actions) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    uitest::Sounds sounds;
    Overlay o;
    o.hud = gameHud(7, 3);
    o.attach(d);
    d.frames(3);
    CHECK_EQ(o.last.freeLeft, kBarRight + kGap);
    CHECK_EQ(o.last.freeRight, kPanelX - kGap);
    CHECK(o.actions.empty());

    struct Button { const char* name; AnalysisAction action; };
    const Button buttons[] = {{"start", AnalysisAction::Start},     {"back", AnalysisAction::Back},
                              {"play", AnalysisAction::TogglePlay}, {"forward", AnalysisAction::Forward},
                              {"end", AnalysisAction::End},         {"comments", AnalysisAction::ToggleComments},
                              {"voice", AnalysisAction::ToggleVoice}, {"arrows", AnalysisAction::ToggleArrows}};
    float x = 0.0f;
    for (const Button& b : buttons) {
        const ui::im::TracedItem* it = byId(d, control(b.name));
        REQUIRE(it);
        CHECK(!isDisabled(d, b.name));
        CHECK(it->r.x > x);   // left to right in this order
        x = it->r.x;
        const size_t before = o.actions.size();
        d.clickRect(it->r);
        CHECK_EQ(o.actions.size(), before + 1);
        CHECK(o.lastAction() == b.action);
    }
    CHECK_EQ(sounds.count(ui::Sound::Click), 5);
    CHECK_EQ(sounds.count(ui::Sound::Toggle), 3);

    // The moves: White's in the left column, Black's in the right one, a row a move number. A
    // click on Black's second move (ply 3) goes to the position after it.
    const ui::im::TracedItem* w2 = byId(d, cell(2));
    const ui::im::TracedItem* b2 = byId(d, cell(3));
    REQUIRE(w2);
    REQUIRE(b2);
    CHECK(w2->r.x < b2->r.x);
    CHECK_EQ(w2->r.y, b2->r.y);
    CHECK(byId(d, cell(0))->r.y < w2->r.y);
    CHECK(cellsShown(d, 7) == std::vector<int>({0, 1, 2, 3, 4, 5, 6}));
    d.clickRect(b2->r);
    CHECK(o.lastAction() == AnalysisAction::GoTo);
    CHECK_EQ(o.positions.back(), 4);
    // The move of the board is a cell like the others: a click goes there again.
    o.hud.current = 4;
    d.frames(2);
    d.clickRect(byId(d, cell(3))->r);
    CHECK_EQ(o.positions.back(), 4);
    CHECK_EQ(sounds.count(ui::Sound::Click), 7);
}

// At the start, start and back are greyed and do nothing; at the end, forward, end and play (play
// comes back while the moves play, as the pause); the voice's toggle without its model is greyed,
// with an item of its own for the reason. The toggles stay enabled, lit or not.
TEST(analysis_mode_hud_greys_what_cannot_be_done) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    Overlay o;
    o.hud = gameHud(7, 0);
    o.hud.commentsOn = false;
    o.hud.arrowsOn = false;
    o.hud.voiceOn = false;
    o.attach(d);
    d.frames(3);
    CHECK(isDisabled(d, "start"));
    CHECK(isDisabled(d, "back"));
    CHECK(!isDisabled(d, "play"));
    CHECK(!isDisabled(d, "forward"));
    CHECK(!isDisabled(d, "end"));
    CHECK(!isDisabled(d, "comments"));
    CHECK(!isDisabled(d, "arrows"));
    d.clickRect(byId(d, control("start"))->r);
    d.clickRect(byId(d, control("back"))->r);
    CHECK(o.actions.empty());
    d.clickRect(byId(d, control("comments"))->r);   // off: a click switches it on
    CHECK(o.lastAction() == AnalysisAction::ToggleComments);

    o.hud = gameHud(7, 7);
    d.frames(2);
    CHECK(!isDisabled(d, "start"));
    CHECK(!isDisabled(d, "back"));
    CHECK(isDisabled(d, "play"));
    CHECK(isDisabled(d, "forward"));
    CHECK(isDisabled(d, "end"));
    o.actions.clear();
    d.clickRect(byId(d, control("forward"))->r);
    d.clickRect(byId(d, control("play"))->r);
    CHECK(o.actions.empty());
    o.hud.playing = true;   // still playing at the end: the middle button pauses
    d.frames(2);
    CHECK(!isDisabled(d, "play"));
    d.clickRect(byId(d, control("play"))->r);
    CHECK(o.lastAction() == AnalysisAction::TogglePlay);

    // No voice model: the toggle is greyed, its reason an item over it; a click does nothing.
    o.hud.voiceAvailable = false;
    o.actions.clear();
    d.frames(2);
    CHECK(isDisabled(d, "voice"));
    const ui::im::TracedItem* why = byId(d, control("voice.why"));
    REQUIRE(why);
    CHECK(why->r.x == byId(d, control("voice"))->r.x);
    d.move(why->r.cx(), why->r.cy());
    d.wait(1.0f);   // the reason's tooltip shows
    d.clickRect(why->r);
    CHECK(o.actions.empty());
    o.hud.voiceAvailable = true;
    d.frames(2);
    CHECK(!isDisabled(d, "voice"));
    CHECK(!byId(d, control("voice.why")));
}

// A long game: the list shows the move of the board, two rows from the edge it comes in by, and
// the wheel scrolls it (the board's move stays where it is); a new position on the board brings
// its move back into view.
TEST(analysis_mode_hud_move_list_follows_the_board_and_the_wheel) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    Overlay o;
    o.hud = gameHud(80, 0);
    o.attach(d);
    d.frames(3);
    std::vector<int> shown = cellsShown(d, 80);
    REQUIRE(!shown.empty());
    CHECK_EQ(shown.front(), 0);
    CHECK(shown.back() < 79);   // not every row fits

    // At the end: the last move in view, the first ones scrolled away.
    o.hud.current = 80;
    o.hud.atEnd = true;
    d.wait(1.0f);
    shown = cellsShown(d, 80);
    REQUIRE(!shown.empty());
    CHECK_EQ(shown.back(), 79);
    CHECK(shown.front() > 0);
    const ui::im::TracedItem* lastCell = byId(d, cell(79));
    REQUIRE(lastCell);
    const Rect lastRect = lastCell->r;

    // The wheel over the list: up to the first moves, whatever the board shows.
    d.move(lastRect.cx(), lastRect.cy() - 200.0f);
    for (int i = 0; i < 12; ++i) d.wheel(1.0f);
    d.wait(1.0f);
    shown = cellsShown(d, 80);
    REQUIRE(!shown.empty());
    CHECK_EQ(shown.front(), 0);
    CHECK(!byId(d, cell(79)));
    // And down again; the wheel outside the list does nothing.
    for (int i = 0; i < 12; ++i) d.wheel(-1.0f);
    d.wait(1.0f);
    CHECK(byId(d, cell(79)));
    d.move(kBarRight + 300.0f, 500.0f);
    for (int i = 0; i < 12; ++i) d.wheel(1.0f);
    d.wait(0.5f);
    CHECK(byId(d, cell(79)));

    // A position in the middle: its move comes into view.
    o.hud.current = 41;
    o.hud.atEnd = false;
    d.wait(1.0f);
    CHECK(byId(d, cell(40)));
    CHECK(!byId(d, cell(79)));
    CHECK(!byId(d, cell(0)));
    // A click on a move far from it.
    d.clickRect(byId(d, cell(cellsShown(d, 80).front()))->r);
    CHECK(o.lastAction() == AnalysisAction::GoTo);
    CHECK(o.positions.back() >= 1);
}

// A record that starts with Black ("40... Kd5"): its first move in Black's column after an empty
// White cell, then the rows as usual. A game without moves has no cell (its line says so); its
// controls are those of a game at its start and its end at once.
TEST(analysis_mode_hud_black_first_and_no_moves) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    Overlay o;
    o.hud = gameHud(0, 0);
    o.hud.firstMoveNumber = 40;
    o.hud.blackFirst = true;
    o.hud.result = "*";
    o.hud.white = "?";
    o.hud.black = "Carla";
    for (const char* san : {"Kd5", "Kd2", "Ke4", "e3"}) {
        ui::AnalysisMove m;
        m.san = san;
        o.hud.moves.push_back(m);
    }
    o.hud.current = 4;
    o.hud.atEnd = true;
    o.attach(d);
    d.frames(3);
    const ui::im::TracedItem* first = byId(d, cell(0));
    const ui::im::TracedItem* second = byId(d, cell(1));
    const ui::im::TracedItem* third = byId(d, cell(2));
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);
    CHECK(first->r.x > second->r.x);   // Black's column
    CHECK(second->r.y > first->r.y);   // White's move on the next row
    CHECK_EQ(third->r.y, second->r.y);
    CHECK(byId(d, cell(3)));
    // The game has no result: the side to move is marked at the end too; nothing to click.
    CHECK(isDisabled(d, "end"));

    o.hud = gameHud(0, 0);
    o.hud.atEnd = true;
    d.frames(2);
    CHECK(cellsShown(d, 4).empty());
    CHECK(isDisabled(d, "start"));
    CHECK(isDisabled(d, "end"));
    CHECK(isDisabled(d, "play"));
    CHECK(!isDisabled(d, "comments"));
}

// What goes under the list takes room from it: the summary of the review, its progress while it
// runs, or the line saying there is no engine. The bar shows each kind of figure (White or Black
// leading, a mate, a result, nothing known) at either end, for either side at the bottom.
TEST(analysis_mode_hud_lines_under_the_list_and_the_bar) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    Overlay o;
    o.attach(d);
    auto rowsWith = [&](const ui::AnalysisHud& h) {
        o.hud = h;
        d.frames(3);
        return cellsShown(d, int(h.moves.size())).size();
    };
    const ui::AnalysisHud plain = gameHud(80, 0);
    const size_t all = rowsWith(plain);
    REQUIRE(all > 4);

    ui::AnalysisHud summary = plain;
    summary.summaryKnown = true;
    summary.accuracy[0] = 91.6;
    summary.accuracy[1] = 104.0;   // shown as 100 %
    summary.counts[0][4] = 2;      // ??
    summary.counts[1][2] = 1;      // ?
    summary.counts[1][6] = 12;     // ?!
    const size_t withSummary = rowsWith(summary);
    CHECK(withSummary < all);

    ui::AnalysisHud progress = plain;
    progress.progress = 0.37f;
    const size_t withProgress = rowsWith(progress);
    CHECK(withProgress < all);
    CHECK(withProgress > withSummary);   // one line, the summary three

    ui::AnalysisHud both = summary;
    both.progress = 0.8f;
    CHECK(rowsWith(both) < withSummary);

    ui::AnalysisHud noEngine = plain;
    noEngine.engineMissing = true;
    noEngine.progress = 0.0f;   // not shown without an engine
    const size_t withoutEngine = rowsWith(noEngine);
    CHECK(withoutEngine < all);

    // The bar: a gauge, not a control. The panel takes the mouse, the bar leaves it to the view
    // (a right drag over it still looks around).
    struct Figure { bool known; float white; const char* text; bool whiteBottom; const char* result; };
    const Figure figures[] = {
        {true, 0.7f, "+1.3", true, "1-0"},            {true, 0.2f, "-M2", true, "0-1"},
        {true, 0.45f, "\xE2\x88\x92" "0.4", false, "\xC2\xBD-\xC2\xBD"}, {true, 1.0f, "1-0", false, "1/2-1/2"},
        {true, 0.0f, "M5", false, "*"},               {false, 0.5f, "", true, ""},
    };
    for (const Figure& f : figures) {
        ui::AnalysisHud h = plain;
        h.evalKnown = f.known;
        h.evalWhite = f.white;
        h.evalText = f.text;
        h.whiteBottom = f.whiteBottom;
        h.result = f.result;
        o.hud = h;
        d.frames(20);   // the boundary glides to its place
        CHECK(byId(d, control("start")));
    }
    // A long opening name keeps to two lines: the list starts no lower than with a short one.
    ui::AnalysisHud shortName = plain;
    const float firstRow = [&] { rowsWith(shortName); return byId(d, cell(0))->r.y; }();
    ui::AnalysisHud longName = plain;
    longName.opening = "Sicilian Defense: Najdorf Variation, English Attack, Anti-English with an extra long name "
                       "that would never fit on two lines of the panel whatever its width";
    rowsWith(longName);
    REQUIRE(byId(d, cell(0)));
    CHECK(byId(d, cell(0))->r.y <= firstRow + 25.0f);
    ui::AnalysisHud noName = plain;
    noName.opening.clear();
    rowsWith(noName);
    CHECK(byId(d, cell(0))->r.y < firstRow);

    o.hud = plain;
    d.move(22.0f + 13.0f, 540.0f);   // over the bar
    d.frames(2);
    CHECK(!ui::wantsMouse());
    d.move(kPanelX + 200.0f, 140.0f);   // over the panel's header
    d.frames(2);
    CHECK(ui::wantsMouse());
    d.clickAt(22.0f + 13.0f, 540.0f);
    CHECK(o.actions.empty());
}

// Hidden (H): nothing is drawn, the subtitles have the whole window (span 0); shown again, the
// overlay is back at once, the bar and the list where they stand.
TEST(analysis_mode_hud_hidden_then_shown) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    Overlay o;
    o.hud = gameHud(7, 2);
    o.attach(d);
    d.frames(3);
    REQUIRE(byId(d, control("play")));
    o.hud.visible = false;
    d.frames(2);
    CHECK(!byId(d, control("play")));
    CHECK(cellsShown(d, 7).empty());
    CHECK_EQ(o.last.freeLeft, 0.0f);
    CHECK_EQ(o.last.freeRight, 0.0f);
    o.hud.visible = true;
    o.hud.evalWhite = 0.1f;
    d.frame();
    CHECK(byId(d, control("play")));
    CHECK(byId(d, cell(1)));
    CHECK_EQ(o.last.freeLeft, kBarRight + kGap);
}

// The viewer's controls hint (viewerHud of an analysis, bottom left, right of the bar): the
// subtitles start right of it when two lines of a comment still fit beside it, else they go over
// it from the bar.
TEST(analysis_mode_hud_subtitles_beside_the_viewer_hint) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    Overlay o;
    o.hud = gameHud(7, 2);
    ui::ViewerHud vh;
    vh.analysis = true;
    vh.white = "Stockfish";
    vh.black = "Human";
    d.screen = [&] {
        ui::viewerHud(vh);
        o.last = ui::analysisHud(o.hud);
    };
    d.frames(3);
    CHECK(o.last.freeLeft > kBarRight + kGap);
    CHECK(o.last.freeRight - o.last.freeLeft >= 640.0f);
    CHECK_EQ(o.last.freeRight, kPanelX - kGap);
    // A narrower window: the panel keeps its least width, the plate goes over the hint.
    d.width = 1280;
    d.frames(3);
    CHECK_EQ(o.last.freeLeft, kBarRight + kGap);
    CHECK_EQ(o.last.freeRight, 1280.0f - kGap - 360.0f - kGap);
    // The hint hidden with the viewer's overlay: from the bar.
    d.width = 1920;
    vh.visible = false;
    d.frames(3);
    CHECK_EQ(o.last.freeLeft, kBarRight + kGap);
}

// In Arabic the panel stays at the right edge and its inside is mirrored, but the moves keep the
// order of the notation (White's column on the left) and the transport buttons that of the
// replay's bar (start on the left), the toggles after them.
TEST(analysis_mode_hud_right_to_left) {
    REQUIRE(uitest::app());
    struct English {
        ~English() { i18n::setLanguage("en"); }
    } english;
    i18n::setLanguage("ar");
    uitest::Driver d;
    Overlay o;
    o.hud = gameHud(7, 5);
    o.hud.summaryKnown = true;
    o.hud.progress = 0.5f;
    o.hud.accuracy[0] = 88.0;
    o.hud.accuracy[1] = 47.5;
    o.hud.counts[1][4] = 1;
    o.attach(d);
    d.frames(3);
    float x = 0.0f;
    for (const char* name : {"start", "back", "play", "forward", "end", "comments", "voice", "arrows"}) {
        const ui::im::TracedItem* it = byId(d, control(name));
        REQUIRE(it);
        CHECK(it->r.x > x);
        CHECK(it->r.x >= kPanelX);
        x = it->r.x;
    }
    REQUIRE(byId(d, cell(0)));
    REQUIRE(byId(d, cell(1)));
    CHECK(byId(d, cell(0))->r.x < byId(d, cell(1))->r.x);
    d.clickRect(byId(d, control("end"))->r);
    CHECK(o.lastAction() == AnalysisAction::End);
}
