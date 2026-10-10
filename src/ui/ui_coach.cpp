// Coach mode: the coach page of the title menu (its Training tab: level, colour; its Challenges
// tab: the sets of positions), the coach's subtitles, the takeback or hint offer card, the key
// hints and a challenge's progress at the table, and the Esc menu of a coach game. Same look as
// ui_screens.cpp; mirrored with im::flip / im::flipX in a right-to-left language.
#include "ui.h"
#include "ui_draw.h"
#include "ui_internal.h"
#include "ui_online_pages.h"
#include "ui_screens_game.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../coach/challenge.h"
#include "../game/settings.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

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
    c.tab = std::clamp(g.coachTab, 0, 1);
    c.level = std::clamp(g.coachLevel, 0, std::max(0, levelCount() - 1));
    c.colour = std::clamp(g.coachColour, 0, 2);
    c.challenge = g.coachChallenge;
}
void storeCoach(const CoachSetup& c) {
    game::Settings& g = game::settings();
    g.coachTab = c.tab;
    g.coachLevel = c.level;
    g.coachColour = c.colour;
    g.coachChallenge = c.challenge;
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

// ---- Challenges -----------------------------------------------------------------------------------
std::string challengeText(const std::string& id, const char* part) {
    return i18n::trOr("challenge." + id + "." + part, std::string());
}
std::string groupTitle(const std::string& group) { return i18n::trOr("challenge.group." + group, uni::toUpper(group)); }

// The challenge the page selects: the one chosen last while the book still has it, else the first
// one not completed yet, else the first.
std::string pickChallenge(const std::string& wanted) {
    const coach::ChallengeBook& book = coach::ChallengeBook::shared();
    if (book.find(wanted)) return wanted;
    for (const coach::Challenge& c : book.challenges())
        if (!game::settings().coachChallengeDone(c.id)) return c.id;
    return book.challenges().empty() ? std::string() : book.challenges().front().id;
}

// The entries of the list, in menu order: each group's heading, then its challenges.
constexpr float kChallengeRow = 62.0f, kGroupHead = 46.0f, kGroupGap = 22.0f;
struct ListEntry {
    int challenge = -1;        // index into the book; -1: the heading of 'group'
    std::string group;
    float y = 0.0f, h = 0.0f;  // in the list's content
};
std::vector<ListEntry> listEntries(const coach::ChallengeBook& book, float& height) {
    std::vector<ListEntry> out;
    float y = 0.0f;
    for (const std::string& g : book.groups()) {
        if (!out.empty()) y += kGroupGap;
        out.push_back({-1, g, y, kGroupHead});
        y += kGroupHead;
        for (size_t i = 0; i < book.challenges().size(); ++i) {
            if (book.challenges()[i].group != g) continue;
            out.push_back({int(i), g, y, kChallengeRow});
            y += kChallengeRow;
        }
    }
    height = y;
    return out;
}

// The difficulty: five small diamonds from x towards the end side, 'level' of them filled.
constexpr float kDiamondStep = 14.0f;
void difficulty(int level, const Rect& area, float x, float cy, bool bright) {
    for (int i = 0; i < 5; ++i) {
        vec2 c(im::flipX(area, x + float(i) * kDiamondStep), cy);
        if (i < level) gfx::diamond(c, 5.2f, bright ? goldBright : gold);
        else gfx::diamond(c, 5.2f, withAlpha(gold, 0.45f), 1.2f);
    }
}

// A check mark 's' wide centred at c (not mirrored: a check reads the same in every script).
void checkMark(vec2 c, float s, vec4 color) {
    vec2 a = c + vec2(-0.46f, 0.0f) * s, b = c + vec2(-0.12f, 0.34f) * s, e = c + vec2(0.48f, -0.38f) * s;
    float th = std::max(2.0f, s * 0.15f);
    // The strokes run on past the bend by half their width: no notch where they meet.
    vec2 da = m::normalize(b - a) * (th * 0.5f), de = m::normalize(e - b) * (th * 0.5f);
    vec4 shade(0.02f, 0.017f, 0.015f, 0.6f * color.w);
    gfx::line(a, b + da, shade, th + 2.0f);
    gfx::line(b - de, e, shade, th + 2.0f);
    gfx::line(a, b + da, color, th);
    gfx::line(b - de, e, color, th);
}

// One row of the challenge list: name and difficulty, the one-line description, and in a column
// of its own at the end, the check mark of a completed challenge.
void challengeRow(const coach::Challenge& c, bool sel, bool done, float hoverT, const Rect& r) {
    if (sel) {
        if (im::rtl()) gfx::fillH(r, withAlpha(gold, 0.04f), withAlpha(gold, 0.13f), 2.0f);
        else gfx::fillH(r, withAlpha(gold, 0.13f), withAlpha(gold, 0.04f), 2.0f);
        gfx::stroke(r, withAlpha(gold, 0.55f), 0.0f, 2.0f);
        gfx::diamond(vec2(im::flipX(r, r.x), r.cy()), 4.5f, goldBright);
    } else {
        im::rowHighlight(r, hoverT);
    }
    const float checkW = 48.0f, diamondsX = r.r() - checkW - 4.0f * kDiamondStep;
    const float nameBase = r.y + 26.0f;
    TextStyle ns = style(font::FACE_TEXT, 25.0f, sel ? goldBright : theme::mix(ivory, goldBright, hoverT * 0.5f), im::startAlign());
    std::string name = challengeName(c.id);
    float nameW = diamondsX - 22.0f - (r.x + 22.0f);
    ns.size = gfx::fitSize(name, ns, nameW);
    gfx::text(im::elideToFit(name, ns, nameW), im::flipX(r, r.x + 22.0f), nameBase, ns);
    difficulty(c.level, r, diamondsX, nameBase - 8.0f, sel);
    std::string desc = challengeText(c.id, "desc");
    if (!desc.empty()) {
        TextStyle ds = style(font::FACE_ITALIC, 19.0f, sel ? ivoryDim : muted, im::startAlign());
        float descW = r.w - 22.0f - checkW;
        ds.size = gfx::fitSize(desc, ds, descW, 0.8f);
        gfx::text(im::elideToFit(desc, ds, descW), im::flipX(r, r.x + 22.0f), r.y + 48.0f, ds);
    }
    if (done) checkMark(vec2(im::flipX(r, r.r() - checkW * 0.5f + 2.0f), r.cy()), 17.0f, green);
}

// The selected challenge in the end column: its group, name, difficulty, number of positions and
// completion, what it trains, and the notes on how challenges go.
void challengeDetail(const coach::Challenge& c, float rx, float colW, float top) {
    const Rect rcol(rx, 0, colW, 0);
    im::sectionLabel(groupTitle(c.group), rx, top + 8.0f, colW);
    float y = top + 70.0f;
    TextStyle ts = style(font::FACE_TITLE, 26.0f, goldBright, im::startAlign(), 0.12f);
    std::string name = uni::toUpper(challengeName(c.id));
    ts.size = gfx::fitSize(name, ts, colW - 8.0f);
    gfx::text(name, im::flipX(rcol, rx + 2.0f), y, ts);
    y += 36.0f;
    difficulty(c.level, rcol, rx + 7.0f, y - 7.0f, true);
    float x = rx + 7.0f + 4.0f * kDiamondStep + 24.0f;
    TextStyle bs = style(font::FACE_ITALIC, 22.0f, gold, im::startAlign());
    std::string count = i18n::trn("coach.challenge.positions", static_cast<long long>(c.positions.size()));
    gfx::text(count, im::flipX(rcol, x), y, bs);
    if (game::settings().coachChallengeDone(c.id)) {
        x += gfx::textWidth(count, bs) + 30.0f;
        checkMark(vec2(im::flipX(rcol, x + 8.0f), y - 7.0f), 16.0f, green);
        TextStyle ds = style(font::FACE_ITALIC, 22.0f, green, im::startAlign());
        gfx::text(T("coach.challenge.done"), im::flipX(rcol, x + 26.0f), y, ds);
    }
    y += 44.0f;
    std::string detail = challengeText(c.id, "detail");
    if (!detail.empty()) {
        TextStyle ds = style(font::FACE_ITALIC, 23.0f, ivoryDim, im::startAlign());
        y += 31.0f * float(gfx::textWrapped(detail, im::flipX(rcol, rx + 2.0f), y, colW - 8.0f, ds, 31.0f));
    }
    gfx::hlineFade(rx, rx + colW, y + 2.0f, withAlpha(gold, 0.25f), 0.25f);
    y += 38.0f;
    y += note(T("coach.challenge.note.white"), rcol, rx, y, colW, ivoryDim);
    y += note(T("coach.challenge.note.hint"), rcol, rx, y, colW, ivoryDim);
    note(T("coach.challenge.note.unsaved"), rcol, rx, y, colW, ivoryDim);
}

// ---- Coach page state -----------------------------------------------------------------------------
struct ChallengeList {
    float scroll = 0.0f, target = 0.0f;
    bool shown = false;        // the Challenges tab was drawn last frame
    bool keyboard = false;     // last frame's im::keyboardMode()
    std::string lastClick;     // double click on a row: start
    double lastClickTime = 0.0;
};
ChallengeList g_list;
int g_forcedTab = -1;          // openCoachTab

// ---- Coach pause menu state -----------------------------------------------------------------------
struct PauseState {
    bool options = false;
    int confirm = 0;  // 1 resign, 2 main menu
};
PauseState g_pause;

}  // namespace

std::string challengeName(const std::string& id) { return i18n::trOr("challenge." + id + ".name", id); }

std::string challengeProgress(const std::string& id, int index, int count) {
    return challengeName(id) + "  \xC2\xB7  " + i18n::trf("coach.challenge.progress", {num(index + 1), num(count)});
}

// ==== Coach page ========================================================================================
namespace detail {
namespace {

// The Training tab: the levels on the start side, the lesson of the selected one and the colour on
// the end side.
void trainingTab(CoachSetup& setup, float lx, float rx, float colW, float top) {
    const Rect lcol(lx, 0, colW, 0), rcol(rx, 0, colW, 0);
    int n = levelCount();

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
}

// The Challenges tab: the list on the start side (scrolled with the wheel; Up / Down move the
// selection, which follows the keyboard focus; Enter or a double click on the selected row sets
// 'start'), the selected challenge on the end side. The list runs from 'top' to 'bottom'. Returns
// the selected row's id (the focus to give by default), 0 when there is none.
im::Id challengesTab(CoachSetup& setup, float lx, float rx, float colW, float top, float bottom, bool opened, bool& start) {
    const coach::ChallengeBook& book = coach::ChallengeBook::shared();
    if (!book.find(setup.challenge)) setup.challenge = pickChallenge(setup.challenge);
    float contentH = 0.0f;
    const std::vector<ListEntry> entries = listEntries(book, contentH);
    auto idOf = [&](const ListEntry& e) -> const std::string& { return book.challenges()[size_t(e.challenge)].id; };
    int sel = -1;  // the selection's entry
    for (size_t e = 0; e < entries.size(); ++e)
        if (entries[e].challenge >= 0 && idOf(entries[e]) == setup.challenge) sel = int(e);
    ChallengeList& s = g_list;
    const Rect area(lx, top - 16.0f, colW, bottom - top + 16.0f);
    // An entry brought into view, clear of the fades at the edges, with its group's heading when
    // it is the group's first row.
    auto reveal = [&](int e) {
        float y0 = entries[size_t(e)].y - 22.0f, y1 = entries[size_t(e)].y + entries[size_t(e)].h + 22.0f;
        if (e > 0 && entries[size_t(e - 1)].challenge < 0) y0 = entries[size_t(e - 1)].y;
        if (y0 < s.target) s.target = y0;
        if (y1 > s.target + area.h) s.target = y1 - area.h;
    };
    // On the tab's first frame the selection shows, a third of the way down when it is further.
    const bool fresh = opened || !s.shown;
    s.shown = true;
    if (fresh) {
        s.target = 0.0f;
        if (sel >= 0 && entries[size_t(sel)].y + kChallengeRow > area.h) s.target = entries[size_t(sel)].y - area.h * 0.35f;
        s.lastClick.clear();
    }
    detail::onl::wheelScroll(s.scroll, s.target, area, contentH, kChallengeRow * 1.5f, fresh);
    // The keyboard focus shown again (a mouse user's first arrow press) on a selected row scrolled
    // out of view: the row comes back into view.
    const bool revealed = im::keyboardMode() && !s.keyboard;
    s.keyboard = im::keyboardMode();
    std::string newSel;
    bool clickedRow = false;
    gfx::pushClip(Rect(area.x - 12.0f, area.y, area.w + 24.0f, area.h));
    im::pushId("challenges");
    for (int e = 0; e < int(entries.size()); ++e) {
        const ListEntry& le = entries[size_t(e)];
        const float y = area.y + le.y - s.scroll;
        const bool visible = y + le.h > area.y && y < area.b();
        if (le.challenge < 0) {
            if (visible) im::sectionLabel(groupTitle(le.group), lx, y + 24.0f, colW);
            continue;
        }
        // Rows out of view take part next to the selection only: Up / Down then reach the next
        // challenge rather than the nearest row on screen.
        if (!visible && (sel < 0 || std::abs(e - sel) > 2)) continue;
        const coach::Challenge& c = book.challenges()[size_t(le.challenge)];
        const bool isSel = e == sel;
        Rect r(lx, y, colW, le.h - 4.0f);
        im::Item it = im::item(im::makeId(c.id), r);
        if (it.focused && im::keyboardMode() && !isSel) newSel = c.id;
        if (it.focused && revealed && isSel && !visible) reveal(e);
        if (it.clicked) {
            if (s.lastClick == c.id && im::time() - s.lastClickTime < 0.45) start = true;
            s.lastClick = c.id;
            s.lastClickTime = im::time();
            if (!isSel) {
                newSel = c.id;
                clickedRow = true;
            }
        } else if (it.activated) {
            if (isSel) start = true;  // Enter on the selected challenge
            else newSel = c.id;
        }
        if (visible) challengeRow(c, isSel, game::settings().coachChallengeDone(c.id), it.hoverT, r);
    }
    if (!newSel.empty()) {
        setup.challenge = newSel;
        for (int e = 0; e < int(entries.size()); ++e)
            if (entries[size_t(e)].challenge >= 0 && idOf(entries[size_t(e)]) == newSel) reveal(e);
        im::sound(clickedRow ? Sound::Toggle : Sound::Tick);
    }
    im::Id selId = setup.challenge.empty() ? 0 : im::makeId(setup.challenge);
    im::popId();
    gfx::popClip();
    detail::onl::scrollDecor(area, s.scroll, contentH);

    if (const coach::Challenge* cur = book.find(setup.challenge)) challengeDetail(*cur, rx, colW, top);
    return selId;
}

}  // namespace

void openCoachTab(int tab) { g_forcedTab = std::clamp(tab, 0, 1); }

MenuAction coachPage(CoachSetup& setup, float t, bool opened, bool& back) {
    vec2 v = gfx::viewSize();
    MenuAction act = MenuAction::None;
    if (opened) {
        loadCoach(setup);
        if (g_forcedTab >= 0) setup.tab = g_forcedTab;
        g_forcedTab = -1;
        g_list.shown = false;
    }
    setup.tab = std::clamp(setup.tab, 0, 1);
    setup.level = std::clamp(setup.level, 0, std::max(0, levelCount() - 1));
    setup.colour = std::clamp(setup.colour, 0, 2);

    dimBackground(t);
    float w = std::min(1480.0f, v.x - 80.0f), h = 990.0f;
    Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 14.0f, w, h);
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(T("coach.title"), p.cx(), p.y + 78.0f);
    im::pushId("coach");
    // The tabs (Left / Right on the bar, PageUp / PageDown anywhere), then two columns: the levels
    // or the challenges first in the reading direction (on the right in a right-to-left language).
    im::tabBar({T("coach.tab.training"), T("coach.tab.challenges")}, setup.tab, Rect(p.x + 60.0f, p.y + 118.0f, p.w - 120.0f, 50.0f));
    float pad = 64.0f, gap = 72.0f;
    float colW = (p.w - 2.0f * pad - gap) * 0.5f;
    float lx = im::flip(p, Rect(p.x + pad, 0, colW, 0)).x, rx = im::flip(p, Rect(p.x + pad + colW + gap, 0, colW, 0)).x;
    float top = p.y + 200.0f;
    float footer = p.b() - 118.0f;
    gfx::vline(p.cx(), top, footer - 20.0f, withAlpha(gold, 0.12f));
    const bool challenges = setup.tab == 1;
    bool startRow = false;
    im::Id rowId = 0;
    if (challenges) {
        rowId = challengesTab(setup, lx, rx, colW, top, footer - 20.0f, opened, startRow);
    } else {
        trainingTab(setup, lx, rx, colW, top);
        g_list.shown = false;
    }

    // ---- Footer.
    float bw = 260.0f, bh = 58.0f;
    float by = p.b() - 52.0f - bh;
    gfx::hlineFade(p.x + 40.0f, p.r() - 40.0f, by - 26.0f, withAlpha(gold, 0.25f), 0.3f);
    bool backPressed = im::button(L("common.back"), im::flip(p, Rect(p.x + pad, by, bw, bh)), im::ButtonKind::Secondary);
    const Rect startRect = im::flip(p, Rect(p.r() - pad - bw, by, bw, bh));
    im::Id startId = 0;
    std::string summary;
    if (challenges) {
        const coach::Challenge* cur = coach::ChallengeBook::shared().find(setup.challenge);
        startId = im::makeId("##coach.challenge.start");
        if ((im::button(L("coach.challenge.start"), startRect, im::ButtonKind::Primary, cur != nullptr) || startRow) && cur) {
            storeCoach(setup);
            act = MenuAction::StartChallenge;
        }
        if (cur)
            summary = challengeName(cur->id) + "  \xC2\xB7  " +
                      i18n::trn("coach.challenge.positions", static_cast<long long>(cur->positions.size()));
    } else {
        startId = im::makeId("##newgame.start");
        if (im::button(L("newgame.start"), startRect, im::ButtonKind::Primary)) {
            storeCoach(setup);
            act = MenuAction::StartCoach;
        }
        summary = levelName(setup.level);
        std::string band = bandText(levelInfo(setup.level));
        if (!band.empty()) summary += "  \xC2\xB7  " + band;
    }
    if (!summary.empty()) {
        TextStyle ss = style(font::FACE_ITALIC, 22.0f, ivoryDim, im::endAlign());
        ss.size = gfx::fitSize(summary, ss, p.w - 2.0f * pad - 2.0f * bw - 60.0f);
        gfx::text(summary, im::flipX(p, p.r() - pad - bw - 30.0f), by + bh * 0.5f + 7.0f, ss);
    }
    // Enter starts: on the Challenges tab the selected row has the focus (Up / Down move it).
    im::setDefaultFocus(rowId ? rowId : startId);
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
    static std::string last, lastTag;
    static float lastBottom = 0.0f, lastLeft = 0.0f, lastRight = 0.0f, shown = 0.0f;
    static bool lastSpeaker = true;
    if (!s.text.empty()) {
        last = s.text;
        lastBottom = s.bottom;
        lastSpeaker = s.speaker;
        lastTag = s.tag;
        lastLeft = s.spanLeft;
        lastRight = s.spanRight;
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
    // Centred in the span given (the analysis: between its bar and its panel), else the window.
    const bool span = lastRight > lastLeft + 1.0f;
    const float left = span ? lastLeft : 0.0f, right = span ? lastRight : v.x;
    // Two lines at most: a longer text is set smaller (the coach's lines are written to fit).
    float maxW = std::max(300.0f, std::min(1100.0f, right - left - (span ? 180.0f : 240.0f)));
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
    Rect plate((left + right) * 0.5f - pw * 0.5f, bottom - ph + (1.0f - a) * 6.0f, pw, ph);
    // The band of the notifications: a dark core fading at both ends, two gold hairlines.
    band(plate, a, 0.74f, std::min(0.22f, 140.0f / pw));
    if (lastSpeaker) {  // "COACH", as printed on the robot's torso, set into the top hairline
        std::string tag = lastTag.empty() ? T("coach.speaker") : lastTag;
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
namespace {

// A key cap and what it does ("Space  Skip"), from the start edge: 'base' is the baseline of the
// action, 'kw' the width of the cap, 'a' the opacity.
void keyHint(const Rect& screen, const std::string& key, const std::string& action, float base, float kw, float a) {
    TextStyle ks = style(font::FACE_TEXT, 19.0f, withAlpha(goldBright, a), HAlign::Center);
    TextStyle as = style(font::FACE_ITALIC, 21.0f, withAlpha(ivoryDim, a), im::startAlign());
    float kh = 32.0f, x = 56.0f;
    float aw = gfx::textWidth(action, as);
    Rect back(x - 16.0f, base - kh + 6.0f - 10.0f, kw + aw + 58.0f, kh + 20.0f);
    gfx::radial(vec2(im::flip(screen, back).cx(), back.cy()), vec2(back.w * 0.75f, back.h * 1.2f), vec4(0, 0, 0, 0.45f * a), 0.0f, 1.0f);
    Rect cap = im::flip(screen, Rect(x, base - kh + 8.0f, kw, kh));
    gfx::fill(cap, vec4(0.03f, 0.026f, 0.024f, 0.8f * a), 5.0f);
    gfx::stroke(cap, withAlpha(gold, 0.6f * a), 0.0f, 5.0f);
    gfx::text(key, cap.cx(), cap.cy() + gfx::capHeight(ks) * 0.5f, ks);
    gfx::text(action, im::flipX(screen, x + kw + 16.0f), base, as);
}

}  // namespace

CoachHudAction coachHud(const CoachHud& hud) {
    CoachHudAction act = CoachHudAction::None;
    vec2 v = gfx::viewSize();
    const Rect screen(0.0f, 0.0f, v.x, v.y);
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_OVERLAY);

    // A challenge's progress ("FORKS  ·  3 / 6"), top centre, above the notifications.
    im::Anim& pa = im::anim(im::makeId("##coach.progress"));
    pa.v[0] = im::approach(pa.v[0], hud.progress.empty() ? 0.0f : 1.0f, 6.0f);
    static std::string lastProgress;  // kept while the band fades out
    if (!hud.progress.empty()) lastProgress = hud.progress;
    if (pa.v[0] > 0.01f && !lastProgress.empty()) {
        float a = ease(pa.v[0]);
        std::string text = uni::toUpper(lastProgress);
        // Capitals carry Latin and Cyrillic at 17; Arabic and CJK, without them, need more.
        bool caps = true;
        for (char32_t c : uni::decode(text))
            if (c >= 0x0600) caps = false;
        TextStyle ps = style(font::FACE_TITLE, caps ? 17.0f : 20.0f, withAlpha(gold, a), HAlign::Center, caps ? 0.22f : 0.08f);
        ps.size = gfx::fitSize(text, ps, v.x * 0.5f);
        float tw = gfx::textWidth(text, ps);
        Rect r(v.x * 0.5f - tw * 0.5f - 110.0f, 18.0f - (1.0f - a) * 6.0f, tw + 220.0f, 40.0f);
        band(r, a, 0.72f, std::min(0.3f, 100.0f / r.w));
        gfx::text(text, r.cx(), r.cy() + gfx::capHeight(ps) * 0.5f, ps);
    }

    // "Space  Skip", bottom start corner, while the coach says or shows something skippable; above
    // it (or in its place) "H  Hint" while the player may ask for a hint in a challenge.
    im::Anim& sa = im::anim(im::makeId("##coach.skiphint"));
    sa.v[0] = im::approach(sa.v[0], hud.skippable ? 1.0f : 0.0f, hud.skippable ? 6.0f : 10.0f);
    im::Anim& ha = im::anim(im::makeId("##coach.hintkey"));
    ha.v[0] = im::approach(ha.v[0], hud.hintKey ? 1.0f : 0.0f, hud.hintKey ? 6.0f : 10.0f);
    if (sa.v[0] > 0.01f || ha.v[0] > 0.01f) {
        TextStyle ks = style(font::FACE_TEXT, 19.0f, goldBright, HAlign::Center);
        std::string skipKey = T("controls.coach_skip.keys"), hintKey = T("controls.coach_hint.keys");
        float skipW = std::max(64.0f, gfx::textWidth(skipKey, ks) + 26.0f), hintW = std::max(64.0f, gfx::textWidth(hintKey, ks) + 26.0f);
        // Stacked, the two caps take the same width so that their actions line up.
        float both = std::min(sa.v[0], ha.v[0]), wide = std::max(skipW, hintW);
        float base = v.y - 46.0f;
        if (sa.v[0] > 0.01f) keyHint(screen, skipKey, T("coach.skip"), base, m::lerp(skipW, wide, both), ease(sa.v[0]));
        if (ha.v[0] > 0.01f)
            keyHint(screen, hintKey, T("coach.hint.key"), base - 50.0f * ease(sa.v[0]), m::lerp(hintW, wide, both), ease(ha.v[0]));
    }

    // The takeback offer (or, in a challenge, the hint offer): a card on the end side, buttons for
    // the mouse only.
    im::Anim& oa = im::anim(im::makeId("##coach.offercard"));
    oa.v[0] = im::approach(oa.v[0], hud.offer ? 1.0f : 0.0f, 10.0f);
    static std::string lastText;  // kept while the card fades out
    static bool lastHint = false;
    if (hud.offer) {
        lastHint = hud.hintOffer;
        lastText = !hud.offerText.empty() ? hud.offerText : T(lastHint ? "coach.hint.text" : "coach.offer.text");
    }
    if (oa.v[0] > 0.01f) {
        const std::string title = T(lastHint ? "coach.hint.title" : "coach.offer.title");
        const std::string keys = T(lastHint ? "coach.hint.hint" : "coach.offer.hint");
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
        cap.size = gfx::fitSize(title, cap, w - 40.0f);
        gfx::text(title, r.cx(), r.y + 46.0f, cap);
        im::ornamentRule(r.cx(), r.y + 62.0f, 90.0f);
        gfx::textWrapped(lastText, r.cx(), r.y + 96.0f, w - 60.0f, ts, 28.0f);
        float y = r.y + 96.0f + 28.0f * float(lines);
        TextStyle hs = style(font::FACE_ITALIC, 18.0f, muted, HAlign::Center);
        hs.size = gfx::fitSize(keys, hs, w - 40.0f, 0.75f);
        gfx::text(keys, r.cx(), y + 4.0f, hs);
        float bw = 190.0f, bh = 48.0f, gap = 20.0f;
        float by = r.b() - 24.0f - bh;
        // Live only once the card is readable: a press as it appears was aimed at something else.
        bool ready = oa.v[0] >= 0.9f;
        im::pushId("coachoffer");
        if (!ready) im::pushBlock();
        if (im::button(L(lastHint ? "coach.hint.decline" : "coach.offer.decline"), im::flip(r, Rect(r.cx() - gap * 0.5f - bw, by, bw, bh)),
                       im::ButtonKind::Secondary, true, im::ITEM_MOUSE_ONLY))
            act = CoachHudAction::PlayOn;
        if (im::button(L(lastHint ? "coach.hint.accept" : "coach.offer.accept"), im::flip(r, Rect(r.cx() + gap * 0.5f, by, bw, bh)),
                       im::ButtonKind::Primary, true, im::ITEM_MOUSE_ONLY))
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
        int r = im::confirmDialog("##coachresign", T("confirm.resign.title"),
                                  T(cp.resignDraws ? "coach.confirm.resign_draw" : "coach.confirm.resign"),
                                  T("confirm.resign.ok"), T("common.cancel"), true);
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
