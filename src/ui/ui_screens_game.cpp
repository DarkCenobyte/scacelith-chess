// Game-mode screens: the "Watch a Game" page (Stockfish vs Stockfish with a preset per side), the
// viewer's pause menu and overlay (players, controls hint, viewpoints), and the player's Elo on the
// title page, the New Game page and the game over card. Same look as ui_screens.cpp.
#include "ui.h"
#include "ui_draw.h"
#include "ui_internal.h"
#include "ui_screens_game.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../game/elo.h"
#include "../game/settings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <vector>

namespace ui {

using gfx::HAlign;
using gfx::Rect;
using gfx::TextStyle;
using i18n::tr;
using i18n::trf;
using m::vec2;
using m::vec4;
using namespace theme;
using namespace detail::helpers;

namespace {

float baselineCentered(const Rect& r, const TextStyle& st) { return r.cy() + gfx::capHeight(st) * 0.5f; }

// The replay's bar (replayBar): its height, its gap to the bottom edge, and the room the
// viewer's centred labels leave under them for it.
constexpr float kReplayBarH = 64.0f, kReplayBarBottom = 44.0f;
constexpr float kReplayBarClear = kReplayBarBottom + kReplayBarH + 76.0f;
// Where the viewer's controls panel was drawn this frame (empty when hidden): the replay's bar,
// drawn after it, keeps clear of it.
Rect g_viewerControls;

// ---- Values (same steps as the New Game page) ----------------------------------------------------
using detail::baseTimeValues;
using detail::clockText;
using detail::nearestIndex;
using detail::nearestValue;
using detail::spacedPlus;
// "3+2" -> category (Lichess-style estimate: base + 40 x increment), as on the New Game page.
const char* categoryKey(const std::string& label) {
    int base = 0, inc = 0;
    if (std::sscanf(label.c_str(), "%d+%d", &base, &inc) != 2) return "tc.no_clock";
    return detail::tcCategoryKey(int64_t(base) * 60, inc);
}
std::string customClockSummary(const WatchSetup& s) {
    int b = s.customBaseSeconds;
    std::string r = b % 60 ? clockText(b) : trf("viewer.minutes", {num(b / 60)});
    if (s.customIncrementSeconds > 0) r += " + " + trf("viewer.seconds", {num(s.customIncrementSeconds)});
    if (s.customDelaySeconds > 0) r += ", " + trf("viewer.delay_summary", {num(s.customDelaySeconds)});
    return r;
}
// A signed amount ("+5") keeps its sign in front inside right-to-left text.
std::string signedSeconds(int s) { return trf("viewer.seconds", {i18n::ltr("+" + num(s))}); }
// "%.2g" with the language's decimal separator.
std::string decimal2(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2g", double(v));
    std::string s = buf;
    size_t p = s.find('.');
    if (p != std::string::npos) s.replace(p, 1, tr("number.decimal"));
    return s;
}

// ---- Presets --------------------------------------------------------------------------------------
// The viewer offers the fixed presets (the last entry of the list, "Custom", is left out).
int presetCount() { return std::max(1, int(detail::data().difficulties.size()) - 1); }
const DifficultyInfo* preset(int i) {
    auto& list = detail::data().difficulties;
    return i >= 0 && i < int(list.size()) ? &list[size_t(i)] : nullptr;
}
std::string presetLabel(int i) {
    const DifficultyInfo* d = preset(i);
    if (!d) return "";
    std::string name = presetName(d->name);
    return d->elo > 0 ? name + " (" + num(d->elo) + ")" : name;
}

void loadWatch(WatchSetup& w) {
    const game::Settings& g = game::settings();
    int n = presetCount();
    int tcn = int(detail::data().timeControls.size());
    w.whitePreset = std::clamp(g.viewerWhitePreset, 0, n - 1);
    w.blackPreset = std::clamp(g.viewerBlackPreset, 0, n - 1);
    w.timeControl = g.viewerTimeControl < 0 || g.viewerTimeControl >= tcn ? -1 : g.viewerTimeControl;
    // The base time as its stepper shows it: a hand-edited .ini may hold another.
    w.customBaseSeconds = nearestValue(baseTimeValues(), g.viewerCustomBaseSeconds);
    w.customIncrementSeconds = std::clamp(g.viewerCustomIncrementSeconds, 0, 60);
    w.customDelaySeconds = std::clamp(g.viewerCustomDelaySeconds, 0, 60);
}
void storeWatch(const WatchSetup& w) {
    game::Settings& g = game::settings();
    g.viewerWhitePreset = w.whitePreset;
    g.viewerBlackPreset = w.blackPreset;
    g.viewerTimeControl = w.timeControl;
    g.viewerCustomBaseSeconds = w.customBaseSeconds;
    g.viewerCustomIncrementSeconds = w.customIncrementSeconds;
    g.viewerCustomDelaySeconds = w.customDelaySeconds;
    g.save();
}

// One side's list of presets: name and rating per row (name at the start of the reading
// direction, rating at the end).
void presetColumn(const char* idStr, int& selected, const Rect& area, float rowH) {
    int n = presetCount();
    im::pushId(idStr);
    for (int i = 0; i < n; ++i) {
        const DifficultyInfo* d = preset(i);
        if (!d) break;
        Rect r(area.x, area.y + float(i) * rowH, area.w, rowH - 4.0f);
        im::Item it = im::item(im::makeId(i), r);
        bool sel = selected == i;
        if (it.activated && !sel) {
            selected = i;
            sel = true;
            im::sound(Sound::Toggle);
        }
        if (sel) {
            if (im::rtl()) gfx::fillH(r, withAlpha(gold, 0.04f), withAlpha(gold, 0.13f), 2.0f);
            else gfx::fillH(r, withAlpha(gold, 0.13f), withAlpha(gold, 0.04f), 2.0f);
            gfx::stroke(r, withAlpha(gold, 0.55f), 0.0f, 2.0f);
            gfx::diamond(vec2(im::flipX(r, r.x), r.cy()), 4.5f, goldBright);
        } else {
            im::rowHighlight(r, it.hoverT);
        }
        float eloW = 0.0f;
        if (d->elo > 0) {
            TextStyle es = style(font::FACE_ITALIC, 21.0f, sel ? gold : muted, im::endAlign());
            std::string elo = trf("viewer.elo_approx", {num(d->elo)});
            eloW = gfx::textWidth(elo, es) + 12.0f;
            gfx::text(elo, im::flipX(r, r.r() - 16.0f), baselineCentered(r, es), es);
        }
        TextStyle ns = style(font::FACE_TEXT, 25.0f, sel ? goldBright : theme::mix(ivory, goldBright, it.hoverT * 0.5f), im::startAlign());
        std::string name = presetName(d->name);
        ns.size = gfx::fitSize(name, ns, r.w - 38.0f - eloW);
        gfx::text(name, im::flipX(r, r.x + 22.0f), baselineCentered(r, ns), ns);
    }
    im::popId();
}

// ---- Viewer pause menu state ---------------------------------------------------------------------
bool g_pauseOptions = false;

}  // namespace

// ==== Watch a Game =====================================================================================
namespace detail {

MenuAction watchPage(WatchSetup& setup, float t, bool opened, bool& back) {
    vec2 v = gfx::viewSize();
    MenuAction act = MenuAction::None;
    if (opened) loadWatch(setup);
    int n = presetCount();
    setup.whitePreset = std::clamp(setup.whitePreset, 0, n - 1);
    setup.blackPreset = std::clamp(setup.blackPreset, 0, n - 1);
    auto& tcs = detail::data().timeControls;
    bool customTc = setup.timeControl < 0 || setup.timeControl >= int(tcs.size());

    dimBackground(t);
    float w = std::min(1480.0f, v.x - 80.0f), h = 1000.0f;
    Rect p(v.x * 0.5f - w * 0.5f, 40.0f + (1.0f - t) * 14.0f, w, h);
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(tr("viewer.title"), p.cx(), p.y + 78.0f);
    im::pushId("watch");

    // Laid out left to right, then mirrored inside the panel for a right-to-left language (White
    // on the right, the time controls on the left).
    float pad = 64.0f, gap = 56.0f;
    float colW = (p.w - 2.0f * pad - 2.0f * gap) / 3.0f;
    float lx0 = p.x + pad, lx1 = lx0 + colW + gap, lx2 = lx1 + colW + gap;
    auto colX = [&](float lx) { return im::flip(p, Rect(lx, 0.0f, colW, 0.0f)).x; };
    float x0 = colX(lx0), x1 = colX(lx1), x2 = colX(lx2);
    float top = p.y + 150.0f;
    float footer = p.b() - 118.0f;
    gfx::vline(im::flipX(p, lx2 - gap * 0.5f), top, footer - 20.0f, withAlpha(gold, 0.12f));

    // The two players.
    float rowH = 52.0f;
    im::sectionLabel(tr("viewer.white"), x0, top + 8.0f, colW);
    im::sectionLabel(tr("viewer.black"), x1, top + 8.0f, colW);
    Rect wl(x0, top + 30.0f, colW, rowH * float(n)), bl(x1, top + 30.0f, colW, rowH * float(n));
    presetColumn("white", setup.whitePreset, wl, rowH);
    presetColumn("black", setup.blackPreset, bl, rowH);
    float dy = wl.b() + 30.0f;
    TextStyle ds = style(font::FACE_ITALIC, 21.0f, ivoryDim, im::startAlign());
    auto description = [&](int i, const Rect& col) {
        if (const DifficultyInfo* d = preset(i))
            gfx::textWrapped(presetDescription(d->name, d->description), im::flipX(col, col.x + 4.0f), dy, colW - 8.0f, ds, 27.0f);
    };
    description(setup.whitePreset, wl);
    description(setup.blackPreset, bl);
    // The divider between the players ends on the swap button.
    Rect swapR = im::flip(p, Rect(lx1 - gap * 0.5f - 130.0f, dy + 72.0f, 260.0f, 46.0f));
    gfx::vline(im::flipX(p, lx1 - gap * 0.5f), top, swapR.y - 10.0f, withAlpha(gold, 0.10f));
    if (im::button(tr("viewer.swap"), swapR, im::ButtonKind::Quiet)) {
        std::swap(setup.whitePreset, setup.blackPreset);
        im::sound(Sound::Toggle);
    }
    TextStyle ns = style(font::FACE_ITALIC, 22.0f, muted, HAlign::Center);
    gfx::textWrapped(tr("viewer.note"), im::flipX(p, lx1 - gap * 0.5f), footer - 64.0f, 2.0f * colW, ns, 29.0f);

    // Time control.
    im::sectionLabel(tr("viewer.time_control"), x2, top + 8.0f, colW);
    int ntc = int(tcs.size()) + 1;
    int cols = 3;
    float cgap = 12.0f;
    float cw = (colW - cgap * float(cols - 1)) / float(cols), ch = 64.0f;
    float gy = top + 30.0f;
    im::pushId("tc");
    for (int i = 0; i < ntc; ++i) {
        bool isCustom = i == ntc - 1;
        Rect r = im::flip(p, Rect(lx2 + float(i % cols) * (cw + cgap), gy + float(i / cols) * (ch + cgap), cw, ch));
        bool sel = isCustom ? customTc : setup.timeControl == i;
        im::Item it = im::item(im::makeId(i), r);
        if (it.activated && !sel) {
            setup.timeControl = isCustom ? -1 : i;
            customTc = isCustom;
            sel = true;
            im::sound(Sound::Toggle);
        }
        gfx::fill(r, vec4(0, 0, 0, 0.25f), 2.0f);
        if (sel) {
            gfx::fillV(r, withAlpha(gold, 0.16f), withAlpha(gold, 0.06f), 2.0f);
            gfx::stroke(r, withAlpha(gold, 0.7f), 0.0f, 2.0f);
        } else {
            gfx::fill(r, withAlpha(gold, 0.08f * it.hoverT), 2.0f);
            gfx::stroke(r, withAlpha(gold, 0.18f + 0.4f * it.hoverT), 0.0f, 2.0f);
        }
        std::string label = isCustom ? std::string(tr("viewer.custom")) : spacedPlus(timeControlLabel(tcs[size_t(i)]));
        std::string cat = isCustom ? std::string(tr("viewer.tc.custom")) : std::string(tr(categoryKey(tcs[size_t(i)])));
        TextStyle ls = style(font::FACE_TEXT, 27.0f, sel ? goldBright : theme::mix(ivory, goldBright, it.hoverT * 0.6f), HAlign::Center);
        if (gfx::textWidth(label, ls) > cw - 12.0f) ls.size = 23.0f;
        ls.size = gfx::fitSize(label, ls, cw - 12.0f);
        gfx::text(label, r.cx(), r.y + 32.0f, ls);
        TextStyle cs = style(font::FACE_ITALIC, 18.0f, sel ? gold : muted, HAlign::Center);
        cs.size = gfx::fitSize(cat, cs, cw - 10.0f);
        gfx::text(cat, r.cx(), r.y + 54.0f, cs);
    }
    im::popId();
    float y = gy + float((ntc + cols - 1) / cols) * (ch + cgap) + 10.0f;
    if (customTc) {
        auto trow = [&]() {
            Rect r(x2, y, colW, 42.0f);  // x2 is already the mirrored column
            y += 44.0f;
            return r;
        };
        im::pushId("customtc");
        auto& bv = baseTimeValues();
        int bi = nearestIndex(bv, setup.customBaseSeconds);
        if (im::stepperRow(tr("viewer.base_time"), bi, int(bv.size()), [&](int i) { return clockText(bv[size_t(i)]); }, trow()))
            setup.customBaseSeconds = bv[size_t(bi)];
        int inc = std::clamp(setup.customIncrementSeconds, 0, 60);
        if (im::stepperRow(tr("viewer.increment"), inc, 61, [](int i) { return signedSeconds(i); }, trow()))
            setup.customIncrementSeconds = inc;
        int del = std::clamp(setup.customDelaySeconds, 0, 60);
        if (im::stepperRow(tr("viewer.delay"), del, 61,
                           [](int i) { return i == 0 ? std::string(tr("viewer.off")) : trf("viewer.seconds", {num(i)}); }, trow()))
            setup.customDelaySeconds = del;
        im::popId();
    }

    // Footer.
    float bw = 260.0f, bh = 58.0f;
    float by = p.b() - 52.0f - bh;
    gfx::hlineFade(p.x + 40.0f, p.r() - 40.0f, by - 26.0f, withAlpha(gold, 0.25f), 0.3f);
    bool backPressed = im::button(std::string(tr("viewer.back")) + "##viewer.back", im::flip(p, Rect(p.x + pad, by, bw, bh)), im::ButtonKind::Secondary);
    im::Id startId = im::makeId("##viewer.start");
    if (im::button(std::string(tr("viewer.start")) + "##viewer.start", im::flip(p, Rect(p.r() - pad - bw, by, bw, bh)), im::ButtonKind::Primary)) {
        storeWatch(setup);
        act = MenuAction::StartWatching;
    }
    {
        // "3 + 2" keeps its order inside an Arabic sentence.
        std::string tc = i18n::ltr(customTc ? customClockSummary(setup) : spacedPlus(timeControlLabel(tcs[size_t(setup.timeControl)])));
        std::string who = trf("viewer.summary", {presetLabel(setup.whitePreset), presetLabel(setup.blackPreset)});
        std::string summary = who + "  \xC2\xB7  " + tc;
        TextStyle ss = style(font::FACE_ITALIC, 22.0f, ivoryDim, im::endAlign());
        ss.size = gfx::fitSize(summary, ss, p.w - 2.0f * pad - 2.0f * bw - 60.0f);
        gfx::text(summary, im::flipX(p, p.r() - pad - bw - 30.0f), by + bh * 0.5f + 7.0f, ss);
    }
    im::setDefaultFocus(startId);
    im::popId();
    gfx::popAlpha();
    if (backPressed || im::consumeBack()) {
        if (!backPressed) im::sound(Sound::Back);
        back = true;
    }
    return act;
}

// ==== Elo ===============================================================================================
void titleRating(float x, float y) {
    const game::Settings& s = game::settings();
    // x is the start edge: the line grows to the right, or to the left in a right-to-left UI.
    const bool rtl = im::rtl();
    const float dir = rtl ? -1.0f : 1.0f;
    const HAlign align = im::startAlign();
    // A short gold hairline sets the rating apart from the menu entries.
    Rect hair(rtl ? x - 180.0f : x, y - 46.0f, 180.0f, 1.5f);
    if (rtl) gfx::fillH(hair, withAlpha(gold, 0.0f), withAlpha(gold, 0.55f));
    else gfx::fillH(hair, withAlpha(gold, 0.55f), withAlpha(gold, 0.0f));
    TextStyle ls = style(font::FACE_TITLE, 19.0f, gold, align, 0.22f);
    float lw = gfx::text(tr("elo.label"), x, y, ls);
    TextStyle vs = style(font::FACE_TEXT, 34.0f, ivory, align);
    float vw = gfx::text(num(s.playerElo), x + dir * (lw + 14.0f), y + 3.0f, vs);
    std::string line;
    if (s.playerGames <= 0) {
        line = tr("elo.no_games");
    } else {
        line = s.playerRecord().provisional() ? std::string(tr("elo.provisional"))
                                              : trf("elo.peak", {num(std::max(s.playerPeakElo, s.playerElo))});
        // "+12 =4 -9" reads left to right in every language.
        line += "  \xC2\xB7  " + i18n::ltr(trf("elo.record", {num(s.playerWins), num(s.playerDraws), num(s.playerLosses)}));
    }
    TextStyle is = style(font::FACE_ITALIC, 21.0f, withAlpha(ivoryDim, 0.9f), align);
    gfx::text(line, x + dir * (lw + 14.0f + vw + 18.0f), y, is);
}

void newGameRating(float x, float width, float y, int difficulty) {
    const game::Settings& s = game::settings();
    std::string text = trf("elo.yours", {num(s.playerElo)});
    const DifficultyInfo* d = preset(difficulty);
    if (d && d->elo > 0) {
        int pct = int(std::lround(100.0 * elo::expectedScore(s.playerElo, d->elo)));
        text += "  \xC2\xB7  " + trf("elo.expected", {num(pct)});
    }
    // At the end of the column's heading (its right end, the left one in a right-to-left UI),
    // never over the heading itself.
    Rect col(x, y - 20.0f, width, 26.0f);
    TextStyle label = style(font::FACE_TITLE, kSection, gold, HAlign::Left, 0.2f);
    TextStyle st = style(font::FACE_ITALIC, 21.0f, ivoryDim, im::endAlign());
    st.size = gfx::fitSize(text, st, width - gfx::textWidth(tr("newgame.opponent"), label) - 60.0f);
    float tw = gfx::textWidth(text, st);
    // Mask the fading rule of the section label behind the text.
    vec4 clear(0.05f, 0.043f, 0.039f, 0.0f), dark(0.05f, 0.043f, 0.039f, 0.9f);
    Rect fade = im::flip(col, Rect(col.r() - tw - 40.0f, col.y, 30.0f, col.h));
    if (im::rtl()) gfx::fillH(fade, dark, clear);
    else gfx::fillH(fade, clear, dark);
    gfx::fill(im::flip(col, Rect(col.r() - tw - 10.0f, col.y, tw + 10.0f, col.h)), dark);
    gfx::text(text, im::flipX(col, col.r()), y, st);
}

void gameOverDetail(const std::string& text, float cx, float y) {
    TextStyle st = style(font::FACE_TEXT, 23.0f, gold, HAlign::Center);
    st.size = gfx::fitSize(text, st, 640.0f);  // a hot-seat game names both players' ratings
    gfx::text(text, cx, y, st);
}

}  // namespace detail

// ==== Viewer pause menu ================================================================================
MenuAction viewerPauseMenu() {
    im::Id id = im::makeId("##viewerpause");
    im::Anim& a = im::anim(id);
    bool appear = a.firstFrame == im::frame();
    if (appear) {
        g_pauseOptions = false;
        im::sound(Sound::Open);
    }
    im::captureMouseAll();
    im::captureKeyboard();
    MenuAction act = MenuAction::None;
    if (g_pauseOptions) {
        if (detail::runOptionsPage(act)) g_pauseOptions = false;
        return act;
    }
    a.v[5] = appear ? 0.0f : std::min(1.0f, a.v[5] + im::dt() / 0.3f);
    float t = ease(a.v[5]);
    vec2 v = gfx::viewSize();
    detail::dimBackground(std::max(t, 0.001f));
    float w = 560.0f, h = 420.0f;
    Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 12.0f, w, h);
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(tr("viewer.paused"), p.cx(), p.y + 80.0f);
    im::pushId("viewerpause");
    float y = p.y + 150.0f, eh = 58.0f, step = 70.0f;
    Rect er(p.x + 40.0f, y, p.w - 80.0f, eh);
    std::string resume = tr("viewer.resume");
    im::Id resumeId = im::makeId(resume);
    if (im::menuEntry(resume, er, true, HAlign::Center)) act = MenuAction::Resume;
    if (im::menuEntry(tr("viewer.options"), er.offset(0, step), true, HAlign::Center)) {
        g_pauseOptions = true;
        detail::openOptionsPage();
    }
    if (im::menuEntry(tr("viewer.main_menu"), er.offset(0, 2 * step), true, HAlign::Center)) act = MenuAction::BackToMainMenu;
    im::setDefaultFocus(resumeId);
    im::popId();
    gfx::popAlpha();
    if (!appear && act == MenuAction::None && im::consumeBack()) {
        act = MenuAction::Resume;
        im::sound(Sound::Close);
    }
    return act;
}

// ==== Viewer overlay ===================================================================================
namespace detail {
// The viewer's controls hint as drawn by the last viewerHud() (empty when hidden): the analysis
// HUD (ui_analysis.cpp) keeps the commentary's subtitles clear of it.
Rect viewerControlsRect() { return g_viewerControls; }
}  // namespace detail

void viewerHud(const ViewerHud& hud) {
    g_viewerControls = Rect();
    if (!hud.visible) return;
    vec2 v = gfx::viewSize();
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_MAIN);
    float x = 56.0f;
    // Laid out from the left edge, mirrored to the right edge for a right-to-left language.
    const Rect screen(0.0f, 0.0f, v.x, v.y);
    // A game analysed: its evaluation bar holds the left edge (analysisHud, ui_analysis.cpp: 26
    // wide from x = 22, its overlay starts 24 right of it) and its panel the right edge, in every
    // language. The controls hint starts right of the bar and is mirrored inside itself only.
    if (hud.analysis) x += 32.0f;

    // Players, top left (a diamond marks the player to move); the analysis panel names them.
    if (!hud.analysis) {
        TextStyle side = style(font::FACE_TITLE, 17.0f, gold, im::startAlign(), 0.22f);
        TextStyle name = style(font::FACE_TEXT, 24.0f, ivory, im::startAlign());
        const std::string* names[2] = {&hud.white, &hud.black};
        const char* sides[2] = {"viewer.white", "viewer.black"};
        float sw = std::max(gfx::textWidth(tr(sides[0]), side), gfx::textWidth(tr(sides[1]), side));
        float nw = std::max(gfx::textWidth(hud.white, name), gfx::textWidth(hud.black, name));
        Rect p(x - 16.0f, 40.0f, 48.0f + sw + 20.0f + nw + 34.0f, 124.0f);
        im::panel(im::flip(screen, p), 0.82f);
        float tx = p.x + 48.0f;
        for (int i = 0; i < 2; ++i) {
            float y = p.y + 52.0f + float(i) * 38.0f;
            bool toMove = hud.sideToMove == i;
            if (toMove) gfx::diamond(vec2(im::flipX(screen, tx - 18.0f), y - 7.0f), 4.0f, goldBright);
            side.color = toMove ? goldBright : gold;
            gfx::text(tr(sides[i]), im::flipX(screen, tx), y, side);
            name.color = toMove ? ivory : ivoryDim;
            gfx::text(*names[i], im::flipX(screen, tx + sw + 20.0f), y + 1.0f, name);
        }
    }

    // Controls, bottom left (a replay's or an analysis' own keys first).
    {
        struct Line { const char* keys; const char* action; };
        static const Line replayLines[] = {
            {"viewer.keys.replay_pause", "viewer.controls.replay_pause"}, {"viewer.keys.replay_step", "viewer.controls.replay_step"},
            {"viewer.keys.replay_speed", "viewer.controls.replay_speed"}, {"viewer.keys.replay_ends", "viewer.controls.replay_ends"},
        };
        static const Line analysisLines[] = {
            {"viewer.keys.replay_pause", "analysis.hud.controls.play"},   {"viewer.keys.replay_step", "viewer.controls.replay_step"},
            {"viewer.keys.replay_ends", "viewer.controls.replay_ends"},   {"analysis.hud.keys.comments", "analysis.hud.controls.comments"},
            {"analysis.hud.keys.voice", "analysis.hud.controls.voice"},   {"analysis.hud.keys.arrows", "analysis.hud.controls.arrows"},
        };
        static const Line viewerLines[] = {
            {"viewer.keys.move", "viewer.controls.move"},     {"viewer.keys.updown", "viewer.controls.updown"},
            {"viewer.keys.fast", "viewer.controls.fast"},     {"viewer.keys.look", "viewer.controls.look"},
            {"viewer.keys.speed", "viewer.controls.speed"},   {"viewer.keys.views", "viewer.controls.views"},
            {"viewer.keys.eyes", "viewer.controls.eyes"},     {"viewer.keys.moves", "viewer.controls.moves"},
            {"viewer.keys.menu", "viewer.controls.menu"},     {"viewer.keys.hide", "viewer.controls.hide"},
        };
        std::vector<Line> lines;
        if (hud.analysis)
            for (const Line& l : analysisLines) lines.push_back(l);
        else if (hud.replay)
            for (const Line& l : replayLines) lines.push_back(l);
        for (const Line& l : viewerLines)   // Tab's small move list: the analysis panel has the moves
            if (!hud.analysis || std::string(l.keys) != "viewer.keys.moves") lines.push_back(l);
        const int count = int(lines.size());
        TextStyle ks = style(font::FACE_TEXT, 20.0f, goldBright, im::endAlign());
        TextStyle as = style(font::FACE_ITALIC, 20.0f, ivoryDim, im::startAlign());
        float kw = 0.0f, aw = 0.0f;
        for (const Line& l : lines) {
            kw = std::max(kw, gfx::textWidth(tr(l.keys), ks));
            aw = std::max(aw, gfx::textWidth(tr(l.action), as));
        }
        float lineH = 29.0f;
        float w = kw + aw + 110.0f, h = 76.0f + lineH * float(count);
        Rect p(x - 16.0f, v.y - h - 44.0f, w, h);
        const Rect& mirror = hud.analysis ? p : screen;   // what the layout is mirrored in
        g_viewerControls = im::flip(mirror, p);
        im::panel(g_viewerControls, 0.82f);
        TextStyle ts = style(font::FACE_TITLE, 17.0f, gold, im::startAlign(), 0.24f);
        gfx::text(tr("viewer.controls.title"), im::flipX(mirror, p.x + 30.0f), p.y + 40.0f, ts);
        float mid = p.x + 30.0f + kw + 20.0f;
        for (int i = 0; i < count; ++i) {
            float y = p.y + 76.0f + float(i) * lineH;
            gfx::text(tr(lines[size_t(i)].keys), im::flipX(mirror, mid - 12.0f), y, ks);
            gfx::diamond(vec2(im::flipX(mirror, mid), y - 6.0f), 2.5f, withAlpha(gold, 0.55f));
            gfx::text(tr(lines[size_t(i)].action), im::flipX(mirror, mid + 12.0f), y, as);
        }
    }

    // Viewpoint just selected / speed just changed: centred low, fading out.
    auto fading = [](float age, float hold) { return m::saturate(age / 0.2f) * m::saturate((hold + 0.6f - age) / 0.6f); };
    // Above the replay's bar, or the commentary's subtitles.
    float cy = v.y - (hud.replay ? kReplayBarClear : hud.analysis ? 270.0f : 120.0f);
    if (!hud.viewpoint.empty()) {
        float a = fading(hud.viewpointAge, 1.8f);
        if (a > 0.001f) {
            TextStyle vs = style(font::FACE_TITLE, 26.0f, withAlpha(ivory, a), HAlign::Center, 0.2f);
            float tw = gfx::textWidth(hud.viewpoint, vs);
            gfx::radial(vec2(v.x * 0.5f, cy - 9.0f), vec2(tw * 0.5f + 140.0f, 46.0f), vec4(0, 0, 0, 0.45f * a), 0.0f, 1.0f);
            gfx::text(hud.viewpoint, v.x * 0.5f, cy, vs);
            gfx::hlineFade(v.x * 0.5f - tw * 0.5f - 40.0f, v.x * 0.5f + tw * 0.5f + 40.0f, cy + 16.0f, withAlpha(gold, 0.6f * a), 0.45f);
            cy -= 56.0f;
        }
    }
    float sa = fading(hud.speedAge, 1.0f);
    if (sa > 0.001f) {
        TextStyle ss = style(font::FACE_ITALIC, 23.0f, withAlpha(ivoryDim, sa), HAlign::Center);
        gfx::radial(vec2(v.x * 0.5f, cy - 8.0f), vec2(200.0f, 34.0f), vec4(0, 0, 0, 0.4f * sa), 0.0f, 1.0f);
        gfx::text(trf("viewer.speed", {decimal2(hud.speed)}), v.x * 0.5f, cy, ss);
    }
    gfx::setLayer(prev);
}

// ==== Replay bar ========================================================================================
namespace {

// A filled triangle pointing left (dir -1) or right (dir 1), tip at 'tip', 'w' long and 'h' tall:
// a fan of strokes from the base to the tip (the draw layer has no polygons).
void replayTriangle(vec2 tip, float dir, float w, float h, vec4 c) {
    const float baseX = tip.x - dir * w;
    const int n = std::max(6, int(std::ceil(h / std::max(0.5f, gfx::px()))));
    for (int k = 0; k <= n; ++k) {
        float y = tip.y - h * 0.5f + h * float(k) / float(n);
        gfx::line(vec2(baseX, y), tip, c, 1.6f * gfx::px());
    }
}

enum class ReplayIcon { Start, Back, Pause, Play, Forward, End };

void replayIcon(ReplayIcon icon, vec2 c, vec4 col) {
    const float h = 16.0f, w = 13.0f, bar = 3.0f;
    switch (icon) {
    case ReplayIcon::Start:
        gfx::fill(Rect(c.x - w * 0.5f - bar, c.y - h * 0.5f, bar, h), col);
        replayTriangle(vec2(c.x - w * 0.5f + 1.0f, c.y), -1.0f, w, h, col);
        break;
    case ReplayIcon::Back: replayTriangle(vec2(c.x - w * 0.5f, c.y), -1.0f, w, h, col); break;
    case ReplayIcon::Pause:
        gfx::fill(Rect(c.x - 6.0f, c.y - h * 0.5f, 4.0f, h), col);
        gfx::fill(Rect(c.x + 2.0f, c.y - h * 0.5f, 4.0f, h), col);
        break;
    case ReplayIcon::Play: replayTriangle(vec2(c.x + w * 0.5f + 1.0f, c.y), 1.0f, w + 2.0f, h + 2.0f, col); break;
    case ReplayIcon::Forward: replayTriangle(vec2(c.x + w * 0.5f, c.y), 1.0f, w, h, col); break;
    case ReplayIcon::End:
        replayTriangle(vec2(c.x + w * 0.5f - 1.0f, c.y), 1.0f, w, h, col);
        gfx::fill(Rect(c.x + w * 0.5f, c.y - h * 0.5f, bar, h), col);
        break;
    }
}

// A square HUD button with an icon (mouse only: the keys are the game's), its tooltip naming the key.
bool replayButton(const char* id, ReplayIcon icon, const Rect& r, bool enabled, const std::string& tip) {
    im::Item it = im::item(im::makeId(id), r, im::ITEM_MOUSE_ONLY | (enabled ? 0u : im::ITEM_DISABLED));
    float t = enabled ? it.hoverT : 0.0f;
    gfx::pushAlpha(enabled ? 1.0f : 0.35f);
    gfx::fill(r, vec4(0, 0, 0, 0.28f), 2.0f);
    gfx::fillV(r, withAlpha(gold, 0.08f * t), withAlpha(gold, 0.03f * t), 2.0f);
    gfx::stroke(r, withAlpha(gold, 0.30f + 0.5f * t), 0.0f, 2.0f);
    replayIcon(icon, vec2(r.cx(), r.cy() + it.pressT), theme::mix(ivoryDim, goldBright, t));
    gfx::popAlpha();
    if (enabled) im::tooltip(tip);
    if (it.activated) im::sound(Sound::Click);
    return enabled && it.activated;
}

}  // namespace

ReplayAction replayBar(const ReplayBar& bar) {
    ReplayAction act = ReplayAction::None;
    vec2 v = gfx::viewSize();
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_MAIN);
    // Laid out left to right in every language, as the moves go: start on the left, end on the right.
    TextStyle ms = style(font::FACE_TITLE, 18.0f, gold, HAlign::Right, 0.18f);
    TextStyle ss = style(font::FACE_ITALIC, 23.0f, ivory, HAlign::Left);
    // Widths that do not change as the game goes on: the counter at its last move, the longest speed.
    std::string moveText = trf("replay.move", {i18n::ltr(num(bar.move)), i18n::ltr(num(bar.moves))});
    float mw = std::max(gfx::textWidth(moveText, ms),
                        gfx::textWidth(trf("replay.move", {i18n::ltr(num(bar.moves)), i18n::ltr(num(bar.moves))}), ms));
    float sw = std::max({gfx::textWidth(bar.speed, ss), gfx::textWidth(trf("replay.speed.factor", {"8"}), ss),
                         gfx::textWidth(tr("replay.speed.instant"), ss)});
    const float bs = 46.0f, gap = 10.0f, pad = 30.0f, sep = 26.0f;
    const float buttonsW = 5.0f * bs + 4.0f * gap;
    const float w = pad + mw + sep + buttonsW + sep + sw + pad, h = kReplayBarH;
    // Above the folded game over bar (64 tall, 36 from the bottom) when it is shown.
    const float bottom = bar.aboveCard ? 36.0f + 64.0f + 18.0f : kReplayBarBottom;
    Rect p(std::floor(v.x * 0.5f - w * 0.5f), v.y - bottom - h, w, h);
    // Beside the viewer's controls panel when the window is not wide enough to centre it (16:10,
    // 4:3, a long language): on the other side of it, else as far from it as the window allows.
    const Rect& c = g_viewerControls;
    const float margin = 24.0f;
    if (c.w > 0.0f && p.x < c.x + c.w + margin && p.x + p.w > c.x - margin) {
        if (c.cx() < v.x * 0.5f) p.x = std::floor(std::max(margin, std::min(c.x + c.w + margin, v.x - margin - p.w)));
        else p.x = std::floor(std::min(v.x - margin - p.w, std::max(margin, c.x - margin - p.w)));
    }
    im::captureMouseRect(p);
    im::panel(p, 0.86f);
    gfx::text(moveText, p.x + pad + mw, baselineCentered(p, ms), ms);
    im::pushId("replaybar");
    float x = p.x + pad + mw + sep, y = p.cy() - bs * 0.5f;
    struct B { const char* id; ReplayIcon icon; bool enabled; const char* tip; ReplayAction action; };
    const B buttons[5] = {
        {"start", ReplayIcon::Start, !bar.atStart, "replay.tip.start", ReplayAction::Start},
        {"back", ReplayIcon::Back, !bar.atStart, "replay.tip.back", ReplayAction::Back},
        {"pause", bar.paused ? ReplayIcon::Play : ReplayIcon::Pause, !bar.atEnd || bar.paused,
         bar.paused ? "replay.tip.resume" : "replay.tip.pause", ReplayAction::TogglePause},
        {"forward", ReplayIcon::Forward, !bar.atEnd, "replay.tip.forward", ReplayAction::Forward},
        {"end", ReplayIcon::End, !bar.atEnd, "replay.tip.end", ReplayAction::End},
    };
    for (const B& b : buttons) {
        if (replayButton(b.id, b.icon, Rect(x, y, bs, bs), b.enabled, tr(b.tip))) act = b.action;
        x += bs + gap;
    }
    im::popId();
    gfx::text(bar.speed, x - gap + sep, baselineCentered(p, ss), ss);
    gfx::setLayer(prev);
    return act;
}

void gameCursor(vec2 pixelPos, GameCursor kind) {
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_TOP);
    vec2 c = pixelPos * (1.0f / std::max(1e-3f, gfx::scale()));
    c = vec2(gfx::snap(c.x), gfx::snap(c.y));
    bool accent = kind == GameCursor::Piece || kind == GameCursor::Square || kind == GameCursor::Clock;
    bool ticks = kind == GameCursor::Holding || kind == GameCursor::Square;
    float a = kind == GameCursor::Waiting ? 0.5f : 1.0f;
    float r = kind == GameCursor::Piece || kind == GameCursor::Clock ? 13.0f : ticks ? 11.0f : 10.0f;
    vec4 ink = withAlpha(accent ? goldBright : ivory, 0.95f * a);
    vec4 shade = vec4(0.0f, 0.0f, 0.0f, 0.5f * a);
    // A dark rim under every stroke keeps the pointer readable on white marble and on the sun.
    gfx::radial(c, vec2(r + 12.0f, r + 12.0f), vec4(0.0f, 0.0f, 0.0f, 0.16f * a), 0.0f, 1.0f);
    gfx::circle(c, r + 1.2f, shade, 4.6f);
    gfx::circle(c, r, ink, 2.2f);
    if (ticks) {
        for (int i = 0; i < 4; ++i) {
            vec2 d = i == 0 ? vec2(1, 0) : i == 1 ? vec2(-1, 0) : i == 2 ? vec2(0, 1) : vec2(0, -1);
            gfx::line(c + d * (r + 3.0f), c + d * (r + 11.0f), shade, 4.4f);
            gfx::line(c + d * (r + 3.8f), c + d * (r + 10.2f), ink, 2.2f);
        }
    }
    float dot = kind == GameCursor::Square ? 2.6f : 1.8f;
    gfx::circle(c, dot + 1.2f, shade);
    gfx::circle(c, dot, ink);
    gfx::setLayer(prev);
}

}  // namespace ui
