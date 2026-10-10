// Analysis mode: the overlay of a game being analysed (ui.h, analysisHud): the evaluation bar at
// the left edge, and the panel at the right edge (the players, the moves with their symbols and
// the evaluation after each, the review's summary and progress, the controls). Same look as the
// viewer's overlay and the replay's bar (ui_screens_game.cpp); viewerHud with ViewerHud::analysis
// puts its controls hint right of the bar.
//
// Right-to-left UI (Arabic). The bar stays at the left edge: it is a gauge standing by the board,
// not text to be read in a direction. The panel stays at the right edge (mirrored to the left it
// would cover the bar) and is mirrored inside, as the other overlays mirror theirs (im::flipX in
// the panel): the players and their scores, the opening, the summary and the review's lines start
// on the right. Its move list keeps the left-to-right order of the moves of the saved games and of
// the online history (number, White's move, Black's move: notation, not text), and its transport
// buttons the order of the replay's bar (start on the left, end on the right, as the moves go);
// the three toggles follow them on the right.
#include "ui.h"
#include "ui_draw.h"
#include "ui_internal.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../coach/catalog.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace ui {

namespace detail {
gfx::Rect viewerControlsRect();   // ui_screens_game.cpp: the viewer's controls hint drawn last
}

using gfx::HAlign;
using gfx::Rect;
using gfx::TextStyle;
using m::vec2;
using m::vec4;
using namespace theme;
using namespace detail::helpers;

namespace {

// ---- Geometry (reference px) ----------------------------------------------------------------------
// The bar: at the left edge, the window's height but the margins of the viewer's overlay. viewerHud
// (ui_screens_game.cpp) starts its controls hint kGap right of it: keep the two in step.
constexpr float kBarX = 22.0f, kBarW = 26.0f, kTop = 40.0f, kBottom = 44.0f, kGap = 24.0f;
// The panel: about 22 % of the window's width within these bounds, at kGap from the right edge.
constexpr float kPanelMinW = 360.0f, kPanelMaxW = 480.0f, kPad = 24.0f;
constexpr float kRowH = 36.0f;    // a row of the move list
constexpr float kNumW = 50.0f;    // its move number's column

// ---- Colours (display sRGB) -----------------------------------------------------------------------
// The bar's parts: White's light, Black's dark.
constexpr vec4 kLight{0.929f, 0.929f, 0.929f, 1.0f};   // #EDEDED
constexpr vec4 kDark{0.169f, 0.169f, 0.169f, 1.0f};    // #2B2B2B
// Behind the move cells, very discreetly: the light part, the dark part (darker than the panel,
// which #2B2B2B is not), their boundary.
constexpr vec4 kCellLight{0.929f, 0.929f, 0.929f, 0.085f};
constexpr vec4 kCellDark{0.0f, 0.0f, 0.0f, 0.14f};
constexpr vec4 kCellEdge{0.929f, 0.929f, 0.929f, 0.24f};

// The annotation symbols by analysis::Nag (the PGN NAG numbers $1..$6) and their colours, the
// hues analysis::nagColor gives the marks on the board.
const char* nagSymbol(int nag) {
    static const char* const symbols[7] = {"", "!", "?", "!!", "??", "!?", "?!"};
    return nag >= 1 && nag <= 6 ? symbols[nag] : "";
}
vec4 nagColour(int nag) {
    switch (nag) {
    case 1: return vec4(0.361f, 0.545f, 0.690f, 1.0f);   // !  #5C8BB0
    case 2: return vec4(0.898f, 0.561f, 0.165f, 1.0f);   // ?  #E58F2A
    case 3: return vec4(0.106f, 0.675f, 0.651f, 1.0f);   // !! #1BACA6
    case 4: return vec4(0.792f, 0.204f, 0.192f, 1.0f);   // ?? #CA3431
    case 5: return vec4(0.710f, 0.498f, 0.839f, 1.0f);   // !? #B57FD6
    case 6: return vec4(0.969f, 0.753f, 0.271f, 1.0f);   // ?! #F7C045
    default: return muted;
    }
}

// ---- State kept from frame to frame -------------------------------------------------------------
struct State {
    uint64_t frame = 0;                  // the last frame the HUD was drawn (0 = never)
    float share = 0.5f, rate = 0.0f;     // the bar's boundary: White's share shown, its speed (/s)
    float scroll = 0.0f, target = 0.0f;  // the move list's scroll: shown, wanted
    int current = -1;                    // the position the list last followed
};
State S;

// Critically damped spring towards 'target' (no overshoot), solved exactly for any frame time:
// 1 - (1 + wt) e^-wt of the way after t, 95 % in 0.3 s.
void glide(float& x, float& v, float target, float dt) {
    const float w = 16.0f;
    const float x0 = x - target, k = v + w * x0, e = std::exp(-w * dt);
    x = target + (x0 + k * dt) * e;
    v = (v - w * k * dt) * e;
}

float baselineCentered(const Rect& r, const TextStyle& st) { return r.cy() + gfx::capHeight(st) * 0.5f; }

// A small Cinzel label ("WHITE", "ACCURACY"): spaced capitals carry Latin and Cyrillic at 'size';
// Arabic and CJK, without capitals, are set larger and closer (as the subtitles' tag).
TextStyle labelStyle(const std::string& text, float size, vec4 color, HAlign align) {
    bool caps = true;
    for (char32_t c : uni::decode(text))
        if (c >= 0x0600) caps = false;
    return style(font::FACE_TITLE, caps ? size : size * 1.3f, color, align, caps ? 0.2f : 0.05f);
}

// ---- Icons ----------------------------------------------------------------------------------------
// A filled triangle: a fan of strokes from 'a' to the side bc (the draw layer has no polygons).
void triangle(vec2 a, vec2 b, vec2 c, vec4 col) {
    const int n = std::max(6, int(std::ceil(m::length(c - b) / std::max(0.5f, gfx::px()))));
    for (int k = 0; k <= n; ++k) gfx::line(b + (c - b) * (float(k) / float(n)), a, col, 1.6f * gfx::px());
}
// An arc of circle (angles in radians, 0 = right, clockwise as y grows downwards).
void arc(vec2 c, float r, float a0, float a1, vec4 col, float thickness) {
    const int n = 10;
    vec2 prev(c.x + std::cos(a0) * r, c.y + std::sin(a0) * r);
    for (int k = 1; k <= n; ++k) {
        float a = a0 + (a1 - a0) * float(k) / float(n);
        vec2 p(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
        gfx::line(prev, p, col, thickness);
        prev = p;
    }
}

enum class Icon { Start, Back, Pause, Play, Forward, End, Comments, Voice, Arrows };

void icon(Icon which, vec2 c, vec4 col) {
    // The transport icons are the replay bar's.
    const float h = 16.0f, w = 13.0f, bar = 3.0f;
    auto pointing = [&](vec2 tip, float dir, float tw, float th) {
        triangle(tip, vec2(tip.x - dir * tw, tip.y - th * 0.5f), vec2(tip.x - dir * tw, tip.y + th * 0.5f), col);
    };
    switch (which) {
    case Icon::Start:
        gfx::fill(Rect(c.x - w * 0.5f - bar, c.y - h * 0.5f, bar, h), col);
        pointing(vec2(c.x - w * 0.5f + 1.0f, c.y), -1.0f, w, h);
        break;
    case Icon::Back: pointing(vec2(c.x - w * 0.5f, c.y), -1.0f, w, h); break;
    case Icon::Pause:
        gfx::fill(Rect(c.x - 6.0f, c.y - h * 0.5f, 4.0f, h), col);
        gfx::fill(Rect(c.x + 2.0f, c.y - h * 0.5f, 4.0f, h), col);
        break;
    case Icon::Play: pointing(vec2(c.x + w * 0.5f + 1.0f, c.y), 1.0f, w + 2.0f, h + 2.0f); break;
    case Icon::Forward: pointing(vec2(c.x + w * 0.5f, c.y), 1.0f, w, h); break;
    case Icon::End:
        pointing(vec2(c.x + w * 0.5f - 1.0f, c.y), 1.0f, w, h);
        gfx::fill(Rect(c.x + w * 0.5f, c.y - h * 0.5f, bar, h), col);
        break;
    case Icon::Comments: {  // a speech bubble with three dots
        gfx::stroke(Rect(c.x - 10.5f, c.y - 9.0f, 21.0f, 14.5f), col, 1.7f, 4.5f);
        triangle(vec2(c.x - 6.5f, c.y + 10.0f), vec2(c.x - 7.5f, c.y + 5.0f), vec2(c.x - 1.5f, c.y + 5.0f), col);
        for (int k = -1; k <= 1; ++k) gfx::circle(vec2(c.x + 4.6f * float(k), c.y - 1.8f), 1.5f, col);
        break;
    }
    case Icon::Voice: {  // a loudspeaker and two waves
        const float x = c.x - 4.0f;
        gfx::fill(Rect(x - 6.5f, c.y - 3.5f, 4.5f, 7.0f), col, 0.8f);
        triangle(vec2(x + 4.0f, c.y - 8.5f), vec2(x - 2.0f, c.y - 3.5f), vec2(x - 2.0f, c.y + 3.5f), col);
        triangle(vec2(x + 4.0f, c.y + 8.5f), vec2(x + 4.0f, c.y - 8.5f), vec2(x - 2.0f, c.y + 3.5f), col);
        arc(vec2(x + 4.0f, c.y), 5.0f, -0.85f, 0.85f, col, 1.6f);
        arc(vec2(x + 4.0f, c.y), 9.5f, -0.9f, 0.9f, col, 1.6f);
        break;
    }
    case Icon::Arrows: {  // a board arrow, up and to the right
        const vec2 from(c.x - 8.0f, c.y + 8.0f), tip(c.x + 8.5f, c.y - 8.5f);
        const vec2 d = m::normalize(tip - from), n(-d.y, d.x);
        gfx::line(from, tip - d * 7.0f, col, 2.8f);
        triangle(tip, tip - d * 9.0f + n * 5.5f, tip - d * 9.0f - n * 5.5f, col);
        break;
    }
    }
}

// A square HUD button with an icon (mouse only: the keys are the game's), its tooltip naming the
// key. toggle: -1 a plain button, 0 a toggle that is off (dim, struck through), 1 one that is on
// (lit). Disabled with a reason ('why'): greyed, the reason as its tooltip.
bool iconButton(const char* id, Icon which, const Rect& r, bool enabled, int toggle, const std::string& tip,
                const std::string& why = std::string()) {
    im::Item it = im::item(im::makeId(id), r, im::ITEM_MOUSE_ONLY | (enabled ? 0u : im::ITEM_DISABLED));
    const float t = enabled ? it.hoverT : 0.0f;
    gfx::pushAlpha(enabled ? 1.0f : 0.35f);
    gfx::fill(r, vec4(0, 0, 0, 0.28f), 2.0f);
    if (toggle == 1) gfx::fillV(r, withAlpha(gold, 0.15f), withAlpha(gold, 0.06f), 2.0f);
    gfx::fillV(r, withAlpha(gold, 0.08f * t), withAlpha(gold, 0.03f * t), 2.0f);
    gfx::stroke(r, withAlpha(gold, (toggle == 1 ? 0.55f : 0.30f) + 0.45f * t), 0.0f, 2.0f);
    const vec4 ink = toggle == 0 ? theme::mix(muted, ivoryDim, t) : theme::mix(toggle == 1 ? goldBright : ivoryDim, goldBright, t);
    const vec2 c(r.cx(), r.cy() + it.pressT);
    icon(which, c, ink);
    if (toggle == 0) {  // struck through, a dark rim keeping the stroke apart from the icon
        const vec2 a(c.x - 11.0f, c.y - 10.0f), b(c.x + 11.0f, c.y + 10.0f);
        gfx::line(a, b, vec4(0.03f, 0.026f, 0.024f, 0.9f), 4.6f);
        gfx::line(a, b, ink, 1.8f);
    }
    gfx::popAlpha();
    if (enabled) {
        im::tooltip(tip);
    } else if (!why.empty()) {
        // An item of its own over the greyed one, for the reason only (as im::disabledButton).
        im::item(im::makeId(std::string(id) + ".why"), r, im::ITEM_MOUSE_ONLY | im::ITEM_SILENT);
        im::tooltip(why);
    }
    if (enabled && it.activated) im::sound(toggle >= 0 ? Sound::Toggle : Sound::Click);
    return enabled && it.activated;
}

// ---- The bar ----------------------------------------------------------------------------------------
// The figure as the bar writes it: without its sign, which the end it stands at already gives
// ("+1.3" -> "1.3", "-M2" -> "M2"); a result ("0-1") as it is.
std::string barFigure(const std::string& s) {
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) return s.substr(1);
    if (s.compare(0, 3, "\xE2\x88\x92") == 0) return s.substr(3);   // U+2212 MINUS SIGN
    return s;
}

// White's part light, Black's dark, their boundary at the share shown (S.share, gliding); the
// figure at the end of the side that leads, in the colour of the other part.
void evalBar(const AnalysisHud& hud, const Rect& r) {
    // A soft shadow and a dark rim keep it apart from a light scene; then a neutral frame.
    gfx::shadow(r.offset(0.0f, 3.0f), 2.0f, 18.0f, vec4(0, 0, 0, 0.55f));
    gfx::fill(r.inset(-2.0f), vec4(0.02f, 0.02f, 0.02f, 0.7f), 2.0f);
    const float share = m::saturate(S.share);
    const float split = gfx::snap(hud.whiteBottom ? r.b() - r.h * share : r.y + r.h * share);
    const Rect upper(r.x, r.y, r.w, split - r.y), lower(r.x, split, r.w, r.b() - split);
    gfx::fill(hud.whiteBottom ? lower : upper, kLight);
    gfx::fill(hud.whiteBottom ? upper : lower, kDark);
    // A faint roundness across, the same over both parts.
    gfx::fillH(Rect(r.x, r.y, r.w * 0.4f, r.h), vec4(1, 1, 1, 0.07f), vec4(1, 1, 1, 0.0f));
    gfx::fillH(Rect(r.r() - r.w * 0.45f, r.y, r.w * 0.45f, r.h), vec4(0, 0, 0, 0.0f), vec4(0, 0, 0, 0.12f));
    // The middle (an even game): a notch on each side.
    const float mid = gfx::snap(r.cy());
    const vec4 notch(0.55f, 0.55f, 0.55f, 0.9f);
    gfx::hline(r.x, r.x + 4.0f, mid, notch, 1.5f);
    gfx::hline(r.r() - 4.0f, r.r(), mid, notch, 1.5f);
    gfx::stroke(r.inset(-1.0f), vec4(0.62f, 0.62f, 0.62f, 0.75f), 1.0f, 1.0f);

    if (hud.evalKnown && !hud.evalText.empty()) {
        const bool whiteLeads = hud.evalWhite >= 0.5f;
        const bool atBottom = whiteLeads == hud.whiteBottom;
        const std::string figure = barFigure(hud.evalText);
        TextStyle fs = style(font::FACE_TITLE, 15.0f, kDark, HAlign::Center);
        fs.dir = 0;
        fs.weight = 0.4f;
        fs.size = gfx::fitSize(figure, fs, r.w - 3.0f, 0.75f);
        const float cap = gfx::capHeight(fs);
        const float base = atBottom ? r.b() - 8.0f : r.y + 8.0f + cap;
        // In the colour of the part it stands on (the other one's while the boundary glides past).
        const float y = base - cap * 0.5f;
        const bool onLight = hud.whiteBottom ? y > split : y < split;
        fs.color = onLight ? kDark : kLight;
        gfx::text(figure, r.cx(), base, fs);
    }
}

// ---- The panel ----------------------------------------------------------------------------------------
// The result as the two sides' scores ("1-0": 1 and 0); none while the game is unfinished.
void scores(const std::string& result, std::string out[2]) {
    const std::string half = "\xC2\xBD";
    if (result == "1-0") {
        out[0] = "1";
        out[1] = "0";
    } else if (result == "0-1") {
        out[0] = "0";
        out[1] = "1";
    } else if (result == half + "-" + half || result == "1/2-1/2") {
        out[0] = out[1] = half;
    }
}

// The side to move in the position on the board (0 White, 1 Black); -1 at the end of a game
// that has a result.
int sideToMove(const AnalysisHud& hud) {
    const int n = int(hud.moves.size());
    if (hud.current >= n && !hud.result.empty() && hud.result != "*") return -1;
    return (std::max(0, hud.current) + (hud.blackFirst ? 1 : 0)) % 2;
}

// The players (a diamond marks the side to move), their scores once the game has a result, the
// opening, a rule. Mirrored in a right-to-left UI. Returns the y under it.
float header(const AnalysisHud& hud, const Rect& p, const Rect& in) {
    TextStyle side = labelStyle(T("viewer.white") + T("viewer.black"), 15.0f, gold, im::startAlign());
    TextStyle name = style(font::FACE_TEXT, 23.0f, ivory, im::startAlign());
    TextStyle sc = style(font::FACE_TITLE, 22.0f, goldBright, im::endAlign());
    sc.dir = 0;
    const char* sides[2] = {"viewer.white", "viewer.black"};
    const std::string* names[2] = {&hud.white, &hud.black};
    std::string score[2];
    scores(hud.result, score);
    const float sw = std::max(gfx::textWidth(T(sides[0]), side), gfx::textWidth(T(sides[1]), side));
    const float scw = std::max(gfx::textWidth(score[0], sc), gfx::textWidth(score[1], sc));
    const int toMove = sideToMove(hud);
    float y = p.y + 50.0f;
    const float lx = in.x + 14.0f;   // the labels, after the diamond's room
    for (int i = 0; i < 2; ++i, y += 36.0f) {
        const bool moving = toMove == i;
        if (moving) gfx::diamond(vec2(im::flipX(p, in.x + 2.0f), y - 6.0f), 3.5f, goldBright);
        side.color = moving ? goldBright : gold;
        gfx::text(T(sides[i]), im::flipX(p, lx), y, side);
        const float nx = lx + sw + 14.0f;
        const float room = in.r() - nx - (scw > 0.0f ? scw + 14.0f : 0.0f);
        TextStyle ns = name;
        ns.color = moving ? ivory : ivoryDim;
        ns.size = gfx::fitSize(*names[i], ns, room, 0.85f);
        gfx::text(im::elideToFit(*names[i], ns, room), im::flipX(p, nx), y + 1.0f, ns);
        if (!score[i].empty()) gfx::text(score[i], im::flipX(p, in.r()), y + 1.0f, sc);
    }
    y -= 36.0f;
    if (!hud.opening.empty()) {
        // Two lines at most: a longer name is cut at the end of one.
        TextStyle os = style(font::FACE_ITALIC, 20.0f, ivoryDim, im::startAlign());
        std::string opening = hud.opening;
        if (gfx::wrapLineCount(opening, in.r() - lx, os) > 2) opening = im::elideToFit(opening, os, in.r() - lx);
        int lines = gfx::textWrapped(opening, im::flipX(p, lx), y + 34.0f, in.r() - lx, os, 25.0f);
        y += 34.0f + 25.0f * float(lines - 1);
    }
    y += 26.0f;
    im::ornamentRule(p.cx(), y, in.w * 0.5f);
    return y + 14.0f;
}

// The evaluation after a move, very discreetly behind its cell: the light part from the cell's
// start edge and the dark part after it, their boundary a little stronger. In the bar's
// orientation: the part at the bar's bottom (the player's side: White's, or Black's when the
// player had Black) starts the cell, as the cells start their row on the left in every language.
void cellGauge(const Rect& cell, float white, bool whiteBottom) {
    const float first = m::saturate(whiteBottom ? white : 1.0f - white);
    const float x = gfx::snap(cell.x + cell.w * first);
    gfx::fill(Rect(cell.x, cell.y, x - cell.x, cell.h), whiteBottom ? kCellLight : kCellDark);
    gfx::fill(Rect(x, cell.y, cell.r() - x, cell.h), whiteBottom ? kCellDark : kCellLight);
    if (x > cell.x + 0.5f && x < cell.r() - 0.5f) gfx::vline(x, cell.y, cell.b(), kCellEdge);
}

// One move's cell: its gauge, the highlight of the current move or of the mouse, the move in
// figurines and its symbol. Returns true when clicked.
bool moveCell(const AnalysisMove& mv, int ply, const Rect& cell, bool current, bool whiteBottom) {
    im::Item it = im::item(im::makeId(ply), cell, im::ITEM_MOUSE_ONLY | im::ITEM_SILENT);
    if (mv.known) cellGauge(cell, mv.white, whiteBottom);
    if (current) {
        gfx::fill(cell, withAlpha(gold, 0.12f + 0.05f * it.hoverT), 2.0f);
        gfx::stroke(cell, withAlpha(goldBright, 0.8f), 1.2f, 2.0f);
    } else if (it.hoverT > 0.01f) {
        gfx::fill(cell, withAlpha(gold, 0.09f * it.hoverT), 2.0f);
        gfx::stroke(cell, withAlpha(gold, 0.45f * it.hoverT), 0.0f, 2.0f);
    }
    TextStyle ms = style(font::FACE_TEXT, 24.0f, current ? goldBright : theme::mix(ivory, goldBright, 0.6f * it.hoverT));
    ms.dir = 0;
    const float base = baselineCentered(cell, ms) + it.pressT;
    const float x = cell.x + 10.0f;
    const float w = gfx::text(coach::figurineSan(mv.san), x, base, ms);
    if (const char* sym = nagSymbol(mv.nag); *sym) {
        TextStyle ss = style(font::FACE_TEXT, 22.0f, nagColour(mv.nag));
        ss.dir = 0;
        ss.weight = 0.6f;
        gfx::text(sym, x + w + 2.0f, base, ss);
    }
    if (it.activated) im::sound(Sound::Click);
    return it.activated;
}

// The move list in 'area': rows of the number, White's move and Black's move (left to right in
// every language), the move that led to the position on the board highlighted and kept in view.
// Returns the ply clicked, or -1.
int moveList(const AnalysisHud& hud, const Rect& area, bool fresh) {
    const int n = int(hud.moves.size());
    if (n == 0) {
        TextStyle es = style(font::FACE_ITALIC, 21.0f, muted, HAlign::Center);
        gfx::text(T("analysis.hud.no_moves"), area.cx(), area.y + 40.0f, es);
        return -1;
    }
    const int shift = hud.blackFirst ? 1 : 0;   // a record that starts with Black: "40. … Kd7"
    const int rows = (n + shift + 1) / 2;
    const float contentH = float(rows) * kRowH;
    const float maxScroll = std::max(0.0f, contentH - area.h);
    const int current = std::clamp(hud.current, 0, n);
    // When the position changes (and only then: the wheel is free in between), its move's row is
    // brought into view, two rows from the edge it comes in by.
    if (fresh || current != S.current) {
        if (fresh) S.target = 0.0f;
        S.current = current;
        const float top = float(current > 0 ? (current - 1 + shift) / 2 : 0) * kRowH;
        const float margin = std::min(2.0f * kRowH, std::max(0.0f, (area.h - kRowH) * 0.5f));
        if (top - margin < S.target) S.target = top - margin;
        else if (top + kRowH + margin > S.target + area.h) S.target = top + kRowH + margin - area.h;
    }
    if (area.contains(im::mouse()) && im::wheel() != 0.0f) S.target -= im::wheel() * kRowH * 2.0f;
    S.target = m::clamp(S.target, 0.0f, maxScroll);
    S.scroll = fresh ? S.target : std::min(im::approach(S.scroll, S.target, 16.0f), maxScroll);

    const float cellW = std::floor((area.w - kNumW) * 0.5f);
    TextStyle ns = style(font::FACE_ITALIC, 20.0f, muted, HAlign::Right);
    ns.dir = 0;
    int clicked = -1;
    gfx::pushClip(area);
    im::pushId("analysis.moves");
    const int firstRow = std::max(0, int(S.scroll / kRowH)), lastRow = std::min(rows - 1, int((S.scroll + area.h) / kRowH));
    for (int r = firstRow; r <= lastRow; ++r) {
        const float y = area.y + float(r) * kRowH - S.scroll;
        // Rows fade at an edge with more moves beyond it.
        float a = 1.0f;
        if (S.scroll > 0.5f) a = std::min(a, m::saturate((y + kRowH - area.y) / (1.3f * kRowH)));
        if (S.scroll < maxScroll - 0.5f) a = std::min(a, m::saturate((area.b() - y) / (1.3f * kRowH)));
        gfx::pushAlpha(a);
        const Rect row(area.x, y, area.w, kRowH);
        gfx::text(num(hud.firstMoveNumber + r) + ".", area.x + kNumW - 10.0f, baselineCentered(row, ns), ns);
        for (int c = 0; c < 2; ++c) {
            const int ply = 2 * r + c - shift;
            const Rect cell(area.x + kNumW + float(c) * cellW + 2.0f, y + 2.0f, cellW - 4.0f, kRowH - 4.0f);
            if (ply < 0) {  // White's move before the record starts
                TextStyle es = style(font::FACE_TEXT, 22.0f, muted);
                es.dir = 0;
                gfx::text("\xE2\x80\xA6", cell.x + 10.0f, baselineCentered(cell, es), es);
                continue;
            }
            if (ply >= n) break;
            if (moveCell(hud.moves[size_t(ply)], ply, cell, ply + 1 == current, hud.whiteBottom)) clicked = ply;
        }
        gfx::popAlpha();
    }
    im::popId();
    gfx::popClip();
    if (maxScroll > 0.5f) {  // where the rows in view stand in the game, on the panel's right edge
        const float bh = std::max(24.0f, area.h * area.h / contentH);
        gfx::fill(Rect(area.r() + 9.0f, area.y + (area.h - bh) * (S.scroll / maxScroll), 2.0f, bh), withAlpha(gold, 0.35f), 1.0f);
    }
    return clicked;
}

// The review's summary: each side's accuracy and its ??, ? and ?! counts under their symbols, in
// three rows from the baseline y. Mirrored in a right-to-left UI.
void summary(const AnalysisHud& hud, const Rect& p, const Rect& in, float y) {
    const float colW = 44.0f;
    const float accEnd = in.r() - 3.0f * colW - 14.0f;   // the accuracy column's end
    auto colX = [&](int k) { return in.r() - colW * (2.5f - float(k)); };   // ??, ?, ?! (centres)
    static const int kNags[3] = {4, 2, 6};
    // Headings: the accuracy, then the symbols in their colours.
    const std::string accuracy = T("analysis.hud.accuracy");
    TextStyle hs = labelStyle(accuracy, 13.0f, muted, im::endAlign());
    hs.size = gfx::fitSize(accuracy, hs, accEnd - in.x - 70.0f, 0.75f);
    gfx::text(accuracy, im::flipX(p, accEnd), y, hs);
    for (int k = 0; k < 3; ++k) {
        TextStyle ss = style(font::FACE_TEXT, 20.0f, nagColour(kNags[k]), HAlign::Center);
        ss.dir = 0;
        ss.weight = 0.6f;
        gfx::text(nagSymbol(kNags[k]), im::flipX(p, colX(k)), y + 1.0f, ss);
    }
    TextStyle side = labelStyle(T("viewer.white") + T("viewer.black"), 14.0f, gold, im::startAlign());
    TextStyle as = style(font::FACE_TEXT, 22.0f, ivory, im::endAlign());
    TextStyle cs = style(font::FACE_TEXT, 21.0f, ivory, HAlign::Center);
    cs.dir = 0;
    const char* sides[2] = {"viewer.white", "viewer.black"};
    for (int i = 0; i < 2; ++i) {
        const float by = y + 27.0f + 26.0f * float(i);
        TextStyle s = side;
        s.size = gfx::fitSize(T(sides[i]), s, accEnd - in.x - 80.0f, 0.75f);
        gfx::text(T(sides[i]), im::flipX(p, in.x), by, s);
        const int pct = int(std::lround(std::clamp(hud.accuracy[i], 0.0, 100.0)));
        gfx::text(i18n::trf("number.percent", {num(pct)}), im::flipX(p, accEnd), by + 1.0f, as);
        for (int k = 0; k < 3; ++k) {  // none: a dash (the face's zero reads as an "o")
            const int count = hud.counts[i][kNags[k]];
            cs.color = count > 0 ? ivory : faint;
            gfx::text(count > 0 ? num(count) : std::string("\xE2\x80\x93"), im::flipX(p, colX(k)), by + 1.0f, cs);
        }
    }
}

// The review at work: its line and a hairline filling in the reading direction, from the baseline y.
void reviewProgress(float progress, const Rect& p, const Rect& in, float y) {
    progress = m::saturate(progress);
    const std::string pct = i18n::trf("number.percent", {num(int(progress * 100.0f))});
    const std::string line = i18n::trf("analysis.hud.reviewing", {pct});
    TextStyle ps = style(font::FACE_ITALIC, 19.0f, ivoryDim, im::startAlign());
    ps.size = gfx::fitSize(line, ps, in.w, 0.8f);
    gfx::text(line, im::flipX(p, in.x), y, ps);
    const float ly = gfx::snap(y + 12.0f), px = gfx::px();
    gfx::fill(Rect(in.x, ly, in.w, px), withAlpha(gold, 0.18f));
    const Rect done = im::flip(in, Rect(in.x, ly, in.w * progress, px));
    if (im::rtl()) gfx::fillH(done, goldBright, withAlpha(goldDeep, 0.8f));
    else gfx::fillH(done, withAlpha(goldDeep, 0.8f), goldBright);
    // A glint running along the part done.
    const float run = std::fmod(float(im::time()) * 0.6f, 1.0f);
    gfx::radial(vec2(im::flipX(in, in.x + in.w * progress * run), ly), vec2(18.0f, 4.0f), withAlpha(goldBright, 0.45f), 0.0f, 1.0f);
}

// The controls in a row from y: start, back, play / pause, forward, end on the left, the toggles of
// the comments, the voice and the arrows on the right (in every language: see the top). 'bs' is
// the buttons' size.
AnalysisAction controls(const AnalysisHud& hud, const Rect& in, float y, float bs, float gap) {
    AnalysisAction act = AnalysisAction::None;
    im::pushId("analysis.controls");
    struct B { const char* id; Icon icon; bool enabled; const char* tip; AnalysisAction action; };
    const B transport[5] = {
        {"start", Icon::Start, !hud.atStart, "replay.tip.start", AnalysisAction::Start},
        {"back", Icon::Back, !hud.atStart, "replay.tip.back", AnalysisAction::Back},
        {"play", hud.playing ? Icon::Pause : Icon::Play, hud.playing || !hud.atEnd,
         hud.playing ? "replay.tip.pause" : "analysis.hud.tip.play", AnalysisAction::TogglePlay},
        {"forward", Icon::Forward, !hud.atEnd, "replay.tip.forward", AnalysisAction::Forward},
        {"end", Icon::End, !hud.atEnd, "replay.tip.end", AnalysisAction::End},
    };
    float x = in.x;
    for (const B& b : transport) {
        if (iconButton(b.id, b.icon, Rect(x, y, bs, bs), b.enabled, -1, T(b.tip))) act = b.action;
        x += bs + gap;
    }
    x = in.r() - 3.0f * bs - 2.0f * gap;
    if (iconButton("comments", Icon::Comments, Rect(x, y, bs, bs), true, hud.commentsOn ? 1 : 0,
                   T(hud.commentsOn ? "analysis.hud.tip.comments_off" : "analysis.hud.tip.comments_on")))
        act = AnalysisAction::ToggleComments;
    x += bs + gap;
    const bool voice = hud.voiceAvailable && hud.voiceOn;
    if (iconButton("voice", Icon::Voice, Rect(x, y, bs, bs), hud.voiceAvailable, voice ? 1 : 0,
                   T(voice ? "analysis.hud.tip.voice_off" : "analysis.hud.tip.voice_on"), T("analysis.hud.tip.voice_missing")))
        act = AnalysisAction::ToggleVoice;
    x += bs + gap;
    if (iconButton("arrows", Icon::Arrows, Rect(x, y, bs, bs), true, hud.arrowsOn ? 1 : 0,
                   T(hud.arrowsOn ? "analysis.hud.tip.arrows_off" : "analysis.hud.tip.arrows_on")))
        act = AnalysisAction::ToggleArrows;
    im::popId();
    return act;
}

}  // namespace

AnalysisHudResult analysisHud(const AnalysisHud& hud) {
    AnalysisHudResult res;
    if (!hud.visible) {
        S.frame = 0;   // shown again: the bar and the list start where they stand, without gliding
        return res;
    }
    const bool fresh = S.frame == 0 || S.frame + 1 < im::frame();
    S.frame = im::frame();
    const vec2 v = gfx::viewSize();
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_MAIN);

    // The bar (not for the mouse: a right drag over it still looks around).
    const float target = hud.evalKnown ? m::saturate(hud.evalWhite) : 0.5f;
    if (fresh) {
        S.share = target;
        S.rate = 0.0f;
    } else {
        glide(S.share, S.rate, target, im::dt());
    }
    const Rect bar(kBarX, kTop, kBarW, v.y - kTop - kBottom);
    evalBar(hud, bar);

    // The panel, from the top: the header, the moves; from the bottom: the controls, the review's
    // lines, the summary.
    const float pw = std::floor(m::clamp(v.x * 0.22f, kPanelMinW, kPanelMaxW));
    const Rect p(v.x - kGap - pw, kTop, pw, v.y - kTop - kBottom);
    const Rect in(p.x + kPad, p.y, p.w - 2.0f * kPad, p.h);
    im::captureMouseRect(p);
    im::panel(p, 0.88f);   // a little denser than the viewer's: small print over a bright hall
    const float listTop = header(hud, p, in);

    const float gap = 6.0f;
    const float bs = std::min(40.0f, std::floor((in.w - 24.0f - 6.0f * gap) / 8.0f));
    const float controlsY = p.b() - 26.0f - bs;
    float y = controlsY - 18.0f;   // the bottom of what goes above the controls
    if (hud.engineMissing) {
        TextStyle es = style(font::FACE_ITALIC, 19.0f, withAlpha(danger, 0.95f), im::startAlign());
        const std::string line = T("analysis.hud.no_engine");
        const int lines = std::min(3, gfx::wrapLineCount(line, in.w, es));
        y -= 24.0f * float(lines) + 4.0f;
        gfx::textWrapped(line, im::flipX(p, in.x), y + 18.0f, in.w, es, 24.0f);
    } else if (hud.progress < 1.0f) {
        y -= 34.0f;
        reviewProgress(hud.progress, p, in, y + 14.0f);
    }
    if (hud.summaryKnown) {
        y -= 82.0f;
        summary(hud, p, in, y + 14.0f);
    }
    gfx::hlineFade(in.x, in.r(), gfx::snap(y - 8.0f), withAlpha(gold, 0.45f), 0.3f);
    const Rect list(in.x, listTop, in.w, std::max(kRowH, y - 18.0f - listTop));
    const int clicked = moveList(hud, list, fresh);
    if (clicked >= 0) {
        res.action = AnalysisAction::GoTo;
        res.position = clicked + 1;
    }
    const AnalysisAction act = controls(hud, in, controlsY, bs, gap);
    if (act != AnalysisAction::None) res.action = act;

    // The subtitles' span: between the bar and the panel, right of the viewer's controls hint
    // (bottom left) when it is drawn.
    res.freeLeft = bar.r() + kGap;
    const Rect hint = detail::viewerControlsRect();
    if (hint.w > 0.0f && hint.cx() < v.x * 0.5f) res.freeLeft = std::max(res.freeLeft, hint.r() + kGap);
    res.freeRight = p.x - kGap;
    gfx::setLayer(prev);
    return res;
}

}  // namespace ui
