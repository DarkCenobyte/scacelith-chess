// Coach mode: the coach page of the title menu (level, colour), the coach's subtitles, the
// takeback offer card and the skip hint at the table, and the Esc menu of a coach game. Same look
// as ui_screens.cpp; mirrored with im::flip / im::flipX in a right-to-left language.
#include "ui.h"
#include "ui_draw.h"
#include "ui_internal.h"
#include "ui_screens_game.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../game/settings.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include <algorithm>
#include <cmath>

namespace ui {

using gfx::HAlign;
using gfx::Rect;
using gfx::TextStyle;
using m::vec2;
using m::vec4;
using namespace theme;
using namespace detail::helpers;

namespace {

// ---- Levels ---------------------------------------------------------------------------------------
int levelCount() { return int(detail::data().coachLevels.size()); }
const CoachLevelInfo& levelInfo(int i) { return detail::data().coachLevels[size_t(std::clamp(i, 0, levelCount() - 1))]; }
bool isLesson(const CoachLevelInfo& l) { return l.eloLow <= 0 && l.eloHigh <= 0; }
std::string levelKey(int i, const char* part) { return "coach.level." + num(i) + "." + part; }
std::string levelText(int i, const char* part) {
    std::string key = levelKey(i, part);
    return i18n::has(key.c_str()) ? T(key) : std::string();
}
// "600–900 Elo", "2100+ Elo" (the range keeps its order inside right-to-left text); "" for the lesson.
std::string bandText(const CoachLevelInfo& l) {
    if (isLesson(l)) return std::string();
    std::string range = l.eloHigh > 0 ? num(l.eloLow) + "\xE2\x80\x93" + num(l.eloHigh) : num(l.eloLow) + "+";
    return i18n::trf("coach.band", {i18n::ltr(range)});
}
std::string levelName(int i) {
    std::string n = levelText(i, "name");
    return n.empty() ? bandText(levelInfo(i)) : n;
}
// The level whose band holds the player's rating (once the player has rated games), else -1.
int recommendedLevel() {
    const game::Settings& g = game::settings();
    if (g.playerGames <= 0) return -1;
    int best = -1;
    for (int i = 0; i < levelCount(); ++i) {
        const CoachLevelInfo& l = levelInfo(i);
        if (isLesson(l)) continue;
        if (best < 0) best = i;  // below the first band: the first band
        if (g.playerElo >= l.eloLow) best = i;
    }
    return best;
}

void loadCoach(CoachSetup& c) {
    const game::Settings& g = game::settings();
    c.level = std::clamp(g.coachLevel, 0, std::max(0, levelCount() - 1));
    c.colour = std::clamp(g.coachColour, 0, 2);
}
void storeCoach(const CoachSetup& c) {
    game::Settings& g = game::settings();
    g.coachLevel = c.level;
    g.coachColour = c.colour;
    g.save();
}

// One row of the level list: name and band, description, the "recommended" tag.
bool levelRow(int i, bool sel, bool recommended, const Rect& r) {
    im::Item it = im::item(im::makeId(i), r);
    if (sel) {
        if (im::rtl()) gfx::fillH(r, withAlpha(gold, 0.04f), withAlpha(gold, 0.13f), 2.0f);
        else gfx::fillH(r, withAlpha(gold, 0.13f), withAlpha(gold, 0.04f), 2.0f);
        gfx::stroke(r, withAlpha(gold, 0.55f), 0.0f, 2.0f);
        gfx::diamond(vec2(im::flipX(r, r.x), r.cy()), 4.5f, goldBright);
    } else {
        im::rowHighlight(r, it.hoverT);
    }
    const CoachLevelInfo& l = levelInfo(i);
    std::string band = bandText(l);
    if (isLesson(l) && game::settings().coachRulesDone) band = T("coach.completed");
    TextStyle es = style(font::FACE_ITALIC, 22.0f, sel ? gold : muted, im::endAlign());
    float bandW = band.empty() ? 0.0f : gfx::textWidth(band, es) + 24.0f;
    float nameBase = r.y + 27.0f;
    TextStyle ns = style(font::FACE_TEXT, 27.0f, sel ? goldBright : theme::mix(ivory, goldBright, it.hoverT * 0.5f), im::startAlign());
    std::string name = levelName(i);
    ns.size = gfx::fitSize(name, ns, r.w - 40.0f - bandW);
    gfx::text(name, im::flipX(r, r.x + 22.0f), nameBase, ns);
    if (!band.empty()) gfx::text(band, im::flipX(r, r.r() - 18.0f), nameBase, es);
    float tagW = 0.0f;
    if (recommended) {
        TextStyle rs = style(font::FACE_TITLE, 14.0f, goldBright, im::endAlign(), 0.18f);
        std::string tag = uni::toUpper(T("coach.recommended"));
        tagW = gfx::textWidth(tag, rs) + 24.0f;
        gfx::text(tag, im::flipX(r, r.r() - 18.0f), r.y + 49.0f, rs);
    }
    std::string desc = levelText(i, "desc");
    if (!desc.empty()) {
        TextStyle ds = style(font::FACE_ITALIC, 20.0f, sel ? ivoryDim : muted, im::startAlign());
        ds.size = gfx::fitSize(desc, ds, r.w - 40.0f - tagW, 0.75f);
        gfx::text(desc, im::flipX(r, r.x + 22.0f), r.y + 50.0f, ds);
    }
    return it.activated;
}

// A note of the lesson column: a gold diamond and a wrapped line. Returns the height used.
float note(const std::string& text, const Rect& col, float x, float y, float w, vec4 color) {
    TextStyle ns = style(font::FACE_ITALIC, 21.0f, color, im::startAlign());
    gfx::diamond(vec2(im::flipX(col, x + 6.0f), y - 7.0f), 3.2f, withAlpha(gold, 0.75f));
    int lines = gfx::textWrapped(text, im::flipX(col, x + 22.0f), y, w - 22.0f, ns, 27.0f);
    return float(lines) * 27.0f + 9.0f;
}

// ---- Coach pause menu state -----------------------------------------------------------------------
struct PauseState {
    bool options = false;
    int confirm = 0;  // 1 resign, 2 main menu
};
PauseState g_pause;

}  // namespace

// ==== Coach page ========================================================================================
namespace detail {

MenuAction coachPage(CoachSetup& setup, float t, bool opened, bool& back) {
    vec2 v = gfx::viewSize();
    MenuAction act = MenuAction::None;
    if (opened) loadCoach(setup);
    int n = levelCount();
    setup.level = std::clamp(setup.level, 0, std::max(0, n - 1));
    setup.colour = std::clamp(setup.colour, 0, 2);

    dimBackground(t);
    float w = std::min(1480.0f, v.x - 80.0f), h = 940.0f;
    Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 14.0f, w, h);
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(T("coach.title"), p.cx(), p.y + 78.0f);
    im::pushId("coach");

    // Levels first in the reading direction (on the right in a right-to-left language).
    float pad = 64.0f, gap = 72.0f;
    float colW = (p.w - 2.0f * pad - gap) * 0.5f;
    float lx = im::flip(p, Rect(p.x + pad, 0, colW, 0)).x, rx = im::flip(p, Rect(p.x + pad + colW + gap, 0, colW, 0)).x;
    const Rect lcol(lx, 0, colW, 0), rcol(rx, 0, colW, 0);
    float top = p.y + 150.0f;
    float footer = p.b() - 118.0f;
    gfx::vline(p.cx(), top, footer - 20.0f, withAlpha(gold, 0.12f));

    // ---- Levels.
    im::sectionLabel(T("coach.section.level"), lx, top + 8.0f, colW);
    int recommended = recommendedLevel();
    if (recommended >= 0) {  // the player's rating at the end of the heading
        TextStyle es = style(font::FACE_ITALIC, 21.0f, ivoryDim, im::endAlign());
        std::string elo = i18n::trf("elo.yours", {num(game::settings().playerElo)});
        TextStyle label = style(font::FACE_TITLE, kSection, gold, HAlign::Left, 0.2f);
        es.size = gfx::fitSize(elo, es, colW - gfx::textWidth(T("coach.section.level"), label) - 60.0f);
        float tw = gfx::textWidth(elo, es);
        Rect band(lx, top - 12.0f, colW, 26.0f);
        vec4 clear(0.05f, 0.043f, 0.039f, 0.0f), dark(0.05f, 0.043f, 0.039f, 0.9f);
        Rect fade = im::flip(band, Rect(band.r() - tw - 40.0f, band.y, 30.0f, band.h));
        if (im::rtl()) gfx::fillH(fade, dark, clear);
        else gfx::fillH(fade, clear, dark);
        gfx::fill(im::flip(band, Rect(band.r() - tw - 10.0f, band.y, tw + 10.0f, band.h)), dark);
        gfx::text(elo, im::flipX(band, band.r()), top + 8.0f, es);
    }
    const float rowH = 66.0f;
    im::pushId("levels");
    for (int i = 0; i < n; ++i) {
        Rect r(lx, top + 30.0f + float(i) * rowH, colW, rowH - 5.0f);
        if (levelRow(i, setup.level == i, i == recommended, r) && setup.level != i) {
            setup.level = i;
            im::sound(Sound::Toggle);
        }
    }
    im::popId();
    // Without its voice files the coach still teaches, in writing.
    if (!setup.voiceAvailable) {
        float y = top + 30.0f + float(n) * rowH + 34.0f;
        TextStyle vs = style(font::FACE_ITALIC, 21.0f, danger, im::startAlign());
        gfx::diamond(vec2(im::flipX(lcol, lx + 6.0f), y - 7.0f), 3.5f, withAlpha(danger, 0.9f));
        std::string text = T("coach.no_voice");
        vs.size = gfx::fitSize(text, vs, colW - 26.0f, 0.8f);
        gfx::textWrapped(text, im::flipX(lcol, lx + 22.0f), y, colW - 26.0f, vs, 27.0f);
    }

    // ---- The lesson: what the selected level teaches, how the games go, the colour.
    const bool lessonNow = isLesson(levelInfo(setup.level));
    im::sectionLabel(T("coach.section.lesson"), rx, top + 8.0f, colW);
    float y = top + 70.0f;
    {
        TextStyle ts = style(font::FACE_TITLE, 26.0f, goldBright, im::startAlign(), 0.12f);
        std::string name = uni::toUpper(levelName(setup.level));
        ts.size = gfx::fitSize(name, ts, colW - 8.0f);
        gfx::text(name, im::flipX(rcol, rx + 2.0f), y, ts);
        std::string band = bandText(levelInfo(setup.level));
        if (!band.empty()) {
            TextStyle bs = style(font::FACE_ITALIC, 22.0f, gold, im::startAlign());
            y += 34.0f;
            gfx::text(i18n::trf("coach.band_for", {band}), im::flipX(rcol, rx + 2.0f), y, bs);
        }
        y += 44.0f;
        std::string detail = levelText(setup.level, "detail");
        if (!detail.empty()) {
            TextStyle ds = style(font::FACE_ITALIC, 23.0f, ivoryDim, im::startAlign());
            y += 31.0f * float(gfx::textWrapped(detail, im::flipX(rcol, rx + 2.0f), y, colW - 8.0f, ds, 31.0f));
        }
    }
    gfx::hlineFade(rx, rx + colW, y + 2.0f, withAlpha(gold, 0.25f), 0.25f);
    y += 38.0f;
    y += note(T("coach.note.clock"), rcol, rx, y, colW, ivoryDim);
    y += note(T(lessonNow ? "coach.note.lesson" : "coach.note.takeback"), rcol, rx, y, colW, ivoryDim);
    y += note(T("coach.note.unrated"), rcol, rx, y, colW, ivoryDim);
    y += 12.0f;
    // Colour: White in the lesson; else White, Black or alternating (the next colour is shown).
    {
        int colour = lessonNow ? 0 : setup.colour;
        if (im::selectorRow(L("coach.colour"), colour, {T("common.white"), T("common.black"), T("coach.colour.alternate")},
                            Rect(rx, y, colW, 50.0f), !lessonNow) &&
            !lessonNow)
            setup.colour = colour;
        im::tooltip(T("coach.colour.help"));
        y += 50.0f + 30.0f;
        game::Settings preview = game::settings();
        preview.coachLevel = setup.level;
        preview.coachColour = setup.colour;
        // Two lines, as on the New Game page (no sentence joining: CJK needs no space after its full stop).
        std::string line = T(preview.coachPlayerColour() == 0 ? "newgame.you_white" : "newgame.you_black");
        TextStyle cs = style(font::FACE_ITALIC, 22.0f, ivoryDim, im::startAlign());
        cs.size = gfx::fitSize(line, cs, colW - 8.0f);
        gfx::text(line, im::flipX(rcol, rx + 2.0f), y, cs);
        if (!lessonNow && setup.colour == 2) {
            std::string next = T("newgame.colour_alternate");
            TextStyle ns = style(font::FACE_ITALIC, 22.0f, muted, im::startAlign());
            ns.size = gfx::fitSize(next, ns, colW - 8.0f);
            gfx::text(next, im::flipX(rcol, rx + 2.0f), y + 28.0f, ns);
        }
    }

    // ---- Footer.
    float bw = 260.0f, bh = 58.0f;
    float by = p.b() - 52.0f - bh;
    gfx::hlineFade(p.x + 40.0f, p.r() - 40.0f, by - 26.0f, withAlpha(gold, 0.25f), 0.3f);
    bool backPressed = im::button(L("common.back"), im::flip(p, Rect(p.x + pad, by, bw, bh)), im::ButtonKind::Secondary);
    im::Id startId = im::makeId("##newgame.start");
    if (im::button(L("newgame.start"), im::flip(p, Rect(p.r() - pad - bw, by, bw, bh)), im::ButtonKind::Primary)) {
        storeCoach(setup);
        act = MenuAction::StartCoach;
    }
    {
        std::string summary = levelName(setup.level);
        std::string band = bandText(levelInfo(setup.level));
        if (!band.empty()) summary += "  \xC2\xB7  " + band;
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

}  // namespace detail

// ==== Subtitles =========================================================================================
float subtitleDuration(const std::string& text, float audioSeconds) {
    int latin = 0, cjk = 0;
    for (char32_t c : uni::decode(text)) {
        if (uni::isCjk(c)) ++cjk;
        else if (c > 0x20) ++latin;
    }
    float reading = float(latin) / 15.0f + float(cjk) / 7.0f;
    return std::max(audioSeconds, reading) + 0.4f;
}

void subtitles(const Subtitle& s) {
    // The last line stays while it fades out after the game cleared it.
    static std::string last;
    static float lastBottom = 0.0f, shown = 0.0f;
    static bool lastSpeaker = true;
    if (!s.text.empty()) {
        last = s.text;
        lastBottom = s.bottom;
        lastSpeaker = s.speaker;
        float in = m::saturate(s.age / 0.18f);
        float out = s.duration > 0.0f ? m::saturate((s.duration + 0.35f - s.age) / 0.35f) : 1.0f;
        shown = ease(in) * ease(out);
    } else {
        shown = std::max(0.0f, shown - im::dt() / 0.35f);
    }
    if (shown <= 0.001f || last.empty()) return;
    float a = shown;
    vec2 v = gfx::viewSize();
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_OVERLAY);
    // Two lines at most: a longer text is set smaller (the coach's lines are written to fit).
    float maxW = std::min(1100.0f, v.x - 240.0f);
    TextStyle ts = style(font::FACE_TEXT, 31.0f, withAlpha(ivory, a), HAlign::Center);
    int lines = gfx::wrapLineCount(last, maxW, ts);
    if (lines > 2) {
        ts.size = std::max(25.0f, 31.0f * 2.0f / float(lines) * 1.1f);
        lines = gfx::wrapLineCount(last, maxW, ts);
    }
    float lh = ts.size * (i18n::rtl() ? 1.5f : 1.38f);
    float textW = gfx::wrapWidth(last, maxW, ts);
    float pw = std::max(420.0f, textW + 160.0f), ph = float(lines) * lh + 34.0f;
    float bottom = lastBottom > 0.0f ? lastBottom : v.y - 96.0f;
    Rect plate(v.x * 0.5f - pw * 0.5f, bottom - ph + (1.0f - a) * 6.0f, pw, ph);
    // The band of the notifications: a dark core fading at both ends, two gold hairlines.
    band(plate, a, 0.74f, std::min(0.22f, 140.0f / pw));
    if (lastSpeaker) {  // "COACH", as printed on the robot's torso, set into the top hairline
        std::string tag = T("coach.speaker");
        // Capitals carry the Latin and Cyrillic tag at 16; Arabic and CJK, without them, need more.
        bool caps = true;
        for (char32_t c : uni::decode(tag))
            if (c >= 0x0600) caps = false;
        TextStyle sp = style(font::FACE_TITLE, caps ? 16.0f : 19.0f, withAlpha(gold, a), HAlign::Center, caps ? 0.24f : 0.1f);
        float tw = gfx::textWidth(tag, sp);
        vec4 d(0.02f, 0.017f, 0.015f, 0.74f * a);  // the band's core, over the top hairline
        gfx::fill(Rect(plate.cx() - tw * 0.5f - 18.0f, plate.y - 1.0f, tw + 36.0f, 3.0f), d);
        gfx::text(tag, plate.cx(), plate.y + gfx::capHeight(sp) * 0.5f, sp);
    }
    float base = plate.y + 17.0f + ts.size * 0.8f;
    TextStyle sh = ts;
    sh.color = vec4(0, 0, 0, 0.6f * a);
    sh.softness = 6.0f;
    sh.weight = 2.0f;
    gfx::textWrapped(last, plate.cx() + 1.5f, base + 2.5f, maxW, sh, lh);
    gfx::textWrapped(last, plate.cx(), base, maxW, ts, lh);
    gfx::setLayer(prev);
}

// ==== Coach HUD =========================================================================================
CoachHudAction coachHud(const CoachHud& hud) {
    CoachHudAction act = CoachHudAction::None;
    vec2 v = gfx::viewSize();
    const Rect screen(0.0f, 0.0f, v.x, v.y);
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_OVERLAY);

    // "Space  Skip", bottom start corner, while the coach says or shows something skippable.
    im::Anim& sa = im::anim(im::makeId("##coach.skiphint"));
    sa.v[0] = im::approach(sa.v[0], hud.skippable ? 1.0f : 0.0f, hud.skippable ? 6.0f : 10.0f);
    if (sa.v[0] > 0.01f) {
        float a = ease(sa.v[0]);
        TextStyle ks = style(font::FACE_TEXT, 19.0f, withAlpha(goldBright, a), HAlign::Center);
        TextStyle as = style(font::FACE_ITALIC, 21.0f, withAlpha(ivoryDim, a), im::startAlign());
        std::string key = T("controls.coach_skip.keys"), action = T("coach.skip");
        float kw = std::max(64.0f, gfx::textWidth(key, ks) + 26.0f), kh = 32.0f;
        float x = 56.0f, base = v.y - 46.0f;
        float aw = gfx::textWidth(action, as);
        Rect back(x - 16.0f, base - kh + 6.0f - 10.0f, kw + aw + 58.0f, kh + 20.0f);
        gfx::radial(vec2(im::flip(screen, back).cx(), back.cy()), vec2(back.w * 0.75f, back.h * 1.2f), vec4(0, 0, 0, 0.45f * a), 0.0f, 1.0f);
        Rect cap = im::flip(screen, Rect(x, base - kh + 8.0f, kw, kh));
        gfx::fill(cap, vec4(0.03f, 0.026f, 0.024f, 0.8f * a), 5.0f);
        gfx::stroke(cap, withAlpha(gold, 0.6f * a), 0.0f, 5.0f);
        gfx::text(key, cap.cx(), cap.cy() + gfx::capHeight(ks) * 0.5f, ks);
        gfx::text(action, im::flipX(screen, x + kw + 16.0f), base, as);
    }

    // The takeback offer: a card on the end side, buttons for the mouse only.
    im::Anim& oa = im::anim(im::makeId("##coach.offercard"));
    oa.v[0] = im::approach(oa.v[0], hud.offer ? 1.0f : 0.0f, 10.0f);
    static std::string lastText;  // kept while the card fades out
    if (hud.offer) lastText = hud.offerText.empty() ? T("coach.offer.text") : hud.offerText;
    if (oa.v[0] > 0.01f) {
        float t = ease(oa.v[0]);
        float w = 480.0f;
        TextStyle ts = style(font::FACE_ITALIC, kSmall, ivory, HAlign::Center);
        int lines = std::min(3, gfx::wrapLineCount(lastText, w - 60.0f, ts));
        float h = 190.0f + 28.0f * float(lines);
        Rect r = im::flip(screen, Rect(v.x - w - 40.0f + (1.0f - t) * 30.0f, v.y * 0.5f - h * 0.5f + 60.0f, w, h));
        im::captureMouseRect(r);
        im::occlude(r);
        gfx::pushAlpha(t);
        if (!hud.offer) im::pushBlock();
        im::panel(r);
        TextStyle cap = style(font::FACE_TITLE, 19.0f, gold, HAlign::Center, 0.2f);
        cap.size = gfx::fitSize(T("coach.offer.title"), cap, w - 40.0f);
        gfx::text(T("coach.offer.title"), r.cx(), r.y + 46.0f, cap);
        im::ornamentRule(r.cx(), r.y + 62.0f, 90.0f);
        gfx::textWrapped(lastText, r.cx(), r.y + 96.0f, w - 60.0f, ts, 28.0f);
        float y = r.y + 96.0f + 28.0f * float(lines);
        TextStyle hs = style(font::FACE_ITALIC, 18.0f, muted, HAlign::Center);
        hs.size = gfx::fitSize(T("coach.offer.hint"), hs, w - 40.0f, 0.75f);
        gfx::text(T("coach.offer.hint"), r.cx(), y + 4.0f, hs);
        float bw = 190.0f, bh = 48.0f, gap = 20.0f;
        float by = r.b() - 24.0f - bh;
        // Live only once the card is readable: a press as it appears was aimed at something else.
        bool ready = oa.v[0] >= 0.9f;
        im::pushId("coachoffer");
        if (!ready) im::pushBlock();
        if (im::button(L("coach.offer.decline"), im::flip(r, Rect(r.cx() - gap * 0.5f - bw, by, bw, bh)), im::ButtonKind::Secondary, true,
                       im::ITEM_MOUSE_ONLY))
            act = CoachHudAction::PlayOn;
        if (im::button(L("coach.offer.accept"), im::flip(r, Rect(r.cx() + gap * 0.5f, by, bw, bh)), im::ButtonKind::Primary, true,
                       im::ITEM_MOUSE_ONLY))
            act = CoachHudAction::TakeBack;
        if (!ready) im::popBlock();
        im::popId();
        if (!hud.offer) im::popBlock();
        gfx::popAlpha();
    }
    gfx::setLayer(prev);
    return act;
}

// ==== Coach pause menu ==================================================================================
MenuAction coachPauseMenu(const CoachPause& cp) {
    im::Id id = im::makeId("##coachpause");
    im::Anim& a = im::anim(id);
    bool appear = a.firstFrame == im::frame();
    if (appear) {
        g_pause = PauseState();
        im::sound(Sound::Open);
    }
    im::captureMouseAll();
    im::captureKeyboard();
    MenuAction act = MenuAction::None;
    if (g_pause.options) {
        if (detail::runOptionsPage(act)) g_pause.options = false;
        return act;
    }
    a.v[5] = appear ? 0.0f : std::min(1.0f, a.v[5] + im::dt() / 0.3f);
    float t = ease(a.v[5]);
    vec2 v = gfx::viewSize();
    detail::dimBackground(std::max(t, 0.001f));
    int entries = 3 + int(cp.canTakeBack) + int(cp.canOfferDraw) + int(cp.canClaimDraw) + int(cp.canResign);
    float step = 70.0f, eh = 58.0f;
    float w = 580.0f, h = 170.0f + step * float(entries) + 20.0f;
    Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 12.0f, w, h);
    if (g_pause.confirm) im::pushBlock();
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(T("pause.title"), p.cx(), p.y + 80.0f);
    im::pushId("coachpause");
    Rect er(p.x + 40.0f, p.y + 150.0f, p.w - 80.0f, eh);
    int i = 0;
    auto next = [&]() { return er.offset(0, step * float(i++)); };
    im::Id resumeId = im::makeId("##pause.resume");
    if (im::menuEntry(L("pause.resume"), next(), true, HAlign::Center)) act = MenuAction::Resume;
    if (cp.canTakeBack) {
        if (im::menuEntry(L("coach.pause.take_back"), next(), true, HAlign::Center)) act = MenuAction::TakeBack;
        im::tooltip(T("coach.pause.take_back.help"));
    }
    if (cp.canOfferDraw && im::menuEntry(L("pause.offer_draw"), next(), true, HAlign::Center)) act = MenuAction::OfferDraw;
    if (cp.canClaimDraw) {
        if (im::menuEntry(L("pause.claim_draw"), next(), cp.mayEndGame, HAlign::Center)) act = MenuAction::ClaimDraw;
        im::tooltip(T("pause.claim_draw.help"));
    }
    if (cp.canResign && im::menuEntry(L("pause.resign"), next(), cp.mayEndGame, HAlign::Center)) g_pause.confirm = 1;
    if (im::menuEntry(L("menu.options"), next(), true, HAlign::Center)) {
        g_pause.options = true;
        detail::openOptionsPage();
    }
    if (im::menuEntry(L("common.main_menu"), next(), true, HAlign::Center)) g_pause.confirm = 2;
    im::setDefaultFocus(resumeId);
    im::popId();
    gfx::popAlpha();
    if (g_pause.confirm) im::popBlock();
    if (!g_pause.confirm && !appear && im::consumeBack()) {
        act = MenuAction::Resume;
        im::sound(Sound::Close);
    }
    if (g_pause.confirm == 1) {
        int r = im::confirmDialog("##coachresign", T("confirm.resign.title"), T("coach.confirm.resign"), T("confirm.resign.ok"),
                                  T("common.cancel"), true);
        if (r == 1) act = MenuAction::Resign;
        if (r >= 0) g_pause.confirm = 0;
    } else if (g_pause.confirm == 2) {
        int r = im::confirmDialog("##coachleave", T("confirm.leave.title"), T("coach.confirm.leave"), T("confirm.leave.ok"),
                                  T("common.cancel"), true);
        if (r == 1) act = MenuAction::BackToMainMenu;
        if (r >= 0) g_pause.confirm = 0;
    }
    return act;
}

}  // namespace ui
