// Game screens: title menu, new game setup, options, credits, pause menu, promotion picker,
// notifications, game over card, loading screen and move list. Layout is in reference pixels
// (1080 tall); the look is a film title card: ivory Garamond, Cinzel capitals, gold hairlines on
// black velvet, never covering more of the hall than necessary.
// Every player-visible text comes from assets/i18n (i18n::tr); in a right-to-left language
// (Arabic) the layouts are mirrored with im::flip / im::flipX.
#include "ui.h"
#include "ui_draw.h"
#include "ui_internal.h"
#include "ui_online.h"
#include "ui_screens_game.h"
#include "ui_screens_online.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../chess/chess.h"
#include "../game/online_session.h"
#include "../game/settings.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include "../platform/platform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui {

using gfx::HAlign;
using gfx::Rect;
using gfx::TextStyle;
using m::vec2;
using m::vec4;
using namespace theme;

namespace {

// ---- State ----------------------------------------------------------------------------------------
enum class Page { Title, NewGame, Options, Credits, Watch, Online };

struct OptionsState {
    game::Settings work;
    std::string help;
    int tab = 0;
    bool confirmDiscard = false;
    float pageT = 0.0f;
};

struct Toast {
    std::string text;
    float age = 0.0f, duration = 3.0f;
};

struct State {
    // main menu
    Page page = Page::Title;
    int forcedPage = -1;
    float pageT = 0.0f;
    bool pageFresh = false;
    uint64_t menuFrame = 0;
    float presetScroll = 0.0f, presetScrollTarget = 0.0f;
    bool resumeOnline = false;      // an online game started from the online page: back to it after
    bool optionsToOnline = false;   // Options opened from the online page: back to it on close
    // options (shared by both menus)
    OptionsState opt;
    int forcedTab = -1;
    bool optionsVisible = false, optionsVisiblePrev = false;
    // pause
    bool pauseOptions = false;
    int pauseConfirm = 0;
    int forcedPauseConfirm = 0;
    // game over
    bool goFolded = false;
    int forcedFold = -1;
    GameOverExtras goExtras;
    // toasts
    std::vector<Toast> toasts;
    // move list
    float mlT = 0.0f;
    float mlScroll = 0.0f, mlScrollTarget = 0.0f;
    size_t mlCount = 0;
    // loading
    float loadShown = 0.0f;
    uint64_t loadFrame = 0;
};
State S;

float ease(float t) { return m::smootherstep(t); }
void setPage(Page p) {
    S.page = p;
    S.pageT = 0.0f;
    S.pageFresh = true;
}
vec2 view() { return gfx::viewSize(); }

TextStyle style(int face, float size, vec4 color, HAlign align = HAlign::Left, float tracking = 0.0f) {
    TextStyle st;
    st.face = face;
    st.size = size;
    st.color = color;
    st.align = align;
    st.tracking = tracking;
    return st;
}

float baselineCentered(const Rect& r, const TextStyle& st) { return r.cy() + gfx::capHeight(st) * 0.5f; }

std::string format(const char* fmt, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, v);
    return buf;
}

// Translation of a key, and a translated widget label that keeps the key as its id
// ("Apply##common.apply"), so focus and animations survive a language change.
std::string T(const char* key) { return i18n::tr(key); }
std::string L(const char* key) { return std::string(i18n::tr(key)) + "##" + key; }

// Number with 'digits' decimals and the language's decimal separator ("2,5" in French).
std::string decimal(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    std::string s = buf;
    const char* sep = i18n::tr("number.decimal");
    size_t dot = s.find('.');
    if (dot != std::string::npos && std::strcmp(sep, ".") != 0) s.replace(dot, 1, sep);
    return s;
}
std::string percent(float x) { return i18n::trf("number.percent", {decimal(x * 100.0, 0)}); }

// Whole screen, for mirroring page layouts in a right-to-left language.
Rect screenRect() {
    vec2 v = gfx::viewSize();
    return Rect(0, 0, v.x, v.y);
}

// Replaces '-' by an en dash for result lines ("1-0" -> "1–0").
std::string typographicResult(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch == '-') out += "\xE2\x80\x93";
        else out += ch;
    }
    return out;
}

std::string upper(const std::string& s) { return uni::toUpper(s); }

// Glyph drawn centred on its ink box (used for chess figures).
void glyphCentered(uint32_t cp, vec2 c, float size, const TextStyle& base) {
    int used = base.face;
    const font::Glyph* g = font::glyph(base.face, cp, &used);
    if (!g) return;
    std::string s;
    if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
    TextStyle st = base;
    st.size = size;
    st.align = HAlign::Left;
    float x = c.x - (g->x0 + g->x1) * 0.5f * size;
    float by = c.y - (g->y0 + g->y1) * 0.5f * size;
    gfx::text(s, x, by, st);
}

// ---- Value tables -----------------------------------------------------------------------------------
const std::vector<int>& baseTimeValues() {
    static std::vector<int> v = [] {
        std::vector<int> r;
        for (int s = 15; s < 180; s += 15) r.push_back(s);
        for (int s = 180; s < 600; s += 30) r.push_back(s);
        for (int s = 600; s < 3600; s += 60) r.push_back(s);
        for (int s = 3600; s <= 10800; s += 300) r.push_back(s);
        return r;
    }();
    return v;
}
const std::vector<int>& moveTimeValues() {
    static const std::vector<int> v = {0, 100, 200, 300, 500, 750, 1000, 1500, 2000, 3000, 5000, 7500, 10000, 15000, 20000, 30000};
    return v;
}
const std::vector<int>& nodeValues() {
    static const std::vector<int> v = {0, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000,
                                       1000000, 2000000, 5000000, 10000000, 20000000, 50000000};
    return v;
}
int nearestIndex(const std::vector<int>& v, int value) {
    int best = 0;
    for (int i = 0; i < int(v.size()); ++i)
        if (std::abs(v[size_t(i)] - value) < std::abs(v[size_t(best)] - value)) best = i;
    return best;
}
std::string clockText(int seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
    return buf;
}
std::string moveTimeText(int ms) {
    if (ms <= 0) return T("engine.no_limit");
    return i18n::trf("tc.seconds", {decimal(ms / 1000.0, ms % 1000 ? 1 : 0)});
}
std::string nodesText(int n) {
    if (n <= 0) return T("engine.no_limit");
    if (n >= 1000000) return i18n::trf("engine.million", {decimal(n / 1e6, 0)});
    return i18n::trf("engine.thousand", {decimal(n / 1e3, 0)});
}

// "3+2" -> category name (Lichess-style estimate: base + 40 x increment).
std::string timeCategory(const std::string& label) {
    int base = 0, inc = 0;
    if (std::sscanf(label.c_str(), "%d+%d", &base, &inc) != 2) return T("tc.no_clock");
    int est = base * 60 + 40 * inc;
    if (est < 180) return T("tc.bullet");
    if (est < 480) return T("tc.blitz");
    if (est < 1500) return T("tc.rapid");
    return T("tc.classical");
}
std::string customClockSummary(const NewGameSetup& s) {
    int b = s.customBaseSeconds;
    std::string r = b % 60 ? clockText(b) : i18n::trf("tc.minutes", {std::to_string(b / 60)});
    if (s.customIncrementSeconds > 0) r = i18n::trf("tc.summary_increment", {r, std::to_string(s.customIncrementSeconds)});
    if (s.customDelaySeconds > 0) r = i18n::trf("tc.summary_delay", {r, std::to_string(s.customDelaySeconds)});
    return r;
}
std::string spacedPlus(const std::string& label) {
    size_t p = label.find('+');
    if (p == std::string::npos) return label;
    return label.substr(0, p) + "\xE2\x80\x89+\xE2\x80\x89" + label.substr(p + 1);
}

// ---- Settings helpers -------------------------------------------------------------------------------
// Player name as stored: surrounding spaces removed, never empty (the scoresheets need a name).
std::string cleanName(const std::string& name) {
    size_t a = name.find_first_not_of(' '), b = name.find_last_not_of(' ');
    return a == std::string::npos ? std::string("Human") : name.substr(a, b - a + 1);
}
void copyOptions(game::Settings& dst, const game::Settings& src) {
    dst.displayWidth = src.displayWidth;
    dst.displayHeight = src.displayHeight;
    dst.fullscreen = src.fullscreen;
    dst.vsync = src.vsync;
    dst.renderScale = src.renderScale;
    dst.quality = src.quality;
    dst.motionBlur = src.motionBlur;
    dst.depthOfField = src.depthOfField;
    dst.brightness = src.brightness;
    dst.masterVolume = src.masterVolume;
    dst.effectsVolume = src.effectsVolume;
    dst.ambienceVolume = src.ambienceVolume;
    dst.ambience = src.ambience;
    dst.showLegalMoves = src.showLegalMoves;
    dst.showCoordinates = src.showCoordinates;
    dst.mouseSensitivity = src.mouseSensitivity;
    dst.invertLook = src.invertLook;
    dst.gameCursor = src.gameCursor;
    dst.handoverSeconds = src.handoverSeconds;
    dst.humanizeThinking = src.humanizeThinking;
    dst.language = src.language;
    dst.playerName = cleanName(src.playerName);
    dst.handStyle = src.handStyle;
    detail::copyOnlineOptions(dst, src);
}
bool sameOptions(const game::Settings& a, const game::Settings& b) {
    auto feq = [](float x, float y) { return std::fabs(x - y) < 1e-4f; };
    return a.displayWidth == b.displayWidth && a.displayHeight == b.displayHeight && a.fullscreen == b.fullscreen &&
           a.vsync == b.vsync && feq(a.renderScale, b.renderScale) && a.quality == b.quality && a.motionBlur == b.motionBlur &&
           a.depthOfField == b.depthOfField && feq(a.brightness, b.brightness) && feq(a.masterVolume, b.masterVolume) &&
           feq(a.effectsVolume, b.effectsVolume) && feq(a.ambienceVolume, b.ambienceVolume) && a.ambience == b.ambience &&
           a.showLegalMoves == b.showLegalMoves && a.showCoordinates == b.showCoordinates &&
           feq(a.mouseSensitivity, b.mouseSensitivity) && a.invertLook == b.invertLook && a.gameCursor == b.gameCursor &&
           a.humanizeThinking == b.humanizeThinking && feq(a.handoverSeconds, b.handoverSeconds) &&
           cleanName(a.playerName) == cleanName(b.playerName) && a.handStyle == b.handStyle && detail::sameOnlineOptions(a, b);
}

// Hot-seat defaults: White is the player of Options > Player, Black "Player 2" in another hand.
std::string hotSeatDefaultName(int colour) {
    if (colour == 0) {
        std::string n = cleanName(game::settings().playerName);
        return n == "Human" ? T("player.default_name") : n;
    }
    return T("hotseat.player2");
}
int hotSeatDefaultHand(int colour) {
    int mine = std::clamp(int(game::settings().handStyle), 0, int(font::HAND_STYLE_COUNT) - 1);
    return colour == 0 ? mine : (mine + 1) % int(font::HAND_STYLE_COUNT);
}
// A hot-seat name as played: trimmed, never empty.
std::string hotSeatName(const NewGameSetup& s, int colour) {
    std::string n = s.names[colour & 1];
    size_t a = n.find_first_not_of(' '), b = n.find_last_not_of(' ');
    return a == std::string::npos ? hotSeatDefaultName(colour) : n.substr(a, b - a + 1);
}

void loadSetupFromSettings(NewGameSetup& s) {
    const game::Settings& g = game::settings();
    int n = int(detail::data().difficulties.size());
    int tcn = int(detail::data().timeControls.size());
    s.difficulty = std::clamp(g.difficultyPreset, 0, std::max(0, n - 1));
    s.timeControl = g.timeControlPreset < 0 || g.timeControlPreset >= tcn ? -1 : g.timeControlPreset;
    s.customBaseSeconds = std::clamp(g.customBaseSeconds, 15, 10800);
    s.customIncrementSeconds = std::clamp(g.customIncrementSeconds, 0, 60);
    s.customDelaySeconds = std::clamp(g.customDelaySeconds, 0, 60);
    s.skillLevel = std::clamp(g.customSkillLevel, 0, 20);
    s.limitElo = g.customLimitElo;
    s.elo = std::clamp(g.customElo, 1320, 3190);
    s.depth = std::clamp(g.customDepth, 0, 30);
    s.moveTimeMs = std::max(0, g.customMoveTimeMs);
    s.nodes = std::max(0, g.customNodes);
    // Hot-seat: the names and hands of the last two-player game, else Options > Player for White
    // and "Player 2" in another hand for Black.
    s.opponent = std::clamp(g.opponent, 0, 1);
    for (int i = 0; i < 2; ++i) {
        s.names[i] = g.hotseatNames[i].empty() ? hotSeatDefaultName(i) : g.hotseatNames[i];
        s.hands[i] = g.hotseatHands[i] >= 0 ? g.hotseatHands[i] : hotSeatDefaultHand(i);
    }
    s.clockRightOf = std::clamp(g.hotseatClockRightOf, 0, 1);
    s.rated = g.hotseatRated;
}
void storeSetupToSettings(const NewGameSetup& s) {
    game::Settings& g = game::settings();
    g.difficultyPreset = s.difficulty;
    g.timeControlPreset = s.timeControl;
    g.customBaseSeconds = s.customBaseSeconds;
    g.customIncrementSeconds = s.customIncrementSeconds;
    g.customDelaySeconds = s.customDelaySeconds;
    g.customSkillLevel = s.skillLevel;
    g.customLimitElo = s.limitElo;
    g.customElo = s.elo;
    g.customDepth = s.depth;
    g.customMoveTimeMs = s.moveTimeMs;
    g.customNodes = s.nodes;
    g.opponent = s.opponent;
    // A default name or hand is stored as "" / -1: it follows Options > Player and the language.
    for (int i = 0; i < 2; ++i) {
        g.hotseatNames[i] = s.names[i] == hotSeatDefaultName(i) ? std::string() : s.names[i];
        g.hotseatHands[i] = s.hands[i] == hotSeatDefaultHand(i) ? -1 : s.hands[i];
    }
    g.hotseatClockRightOf = s.clockRightOf;
    g.hotseatRated = s.rated;
    g.save();
}

// Full-screen dim behind menu panels (keeps the hall visible).
void dimScene(float a) {
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_BACK);
    vec2 v = view();
    gfx::fill(Rect(0, 0, v.x, v.y), vec4(0, 0, 0, 0.42f * a));
    gfx::setLayer(prev);
}

// ---- Player profile ---------------------------------------------------------------------------------
constexpr int kMaxNameLength = 24;  // characters; fits the name field of the scoresheet

// A strip of scoresheet paper with the name and a few moves in the player's handwriting: the
// live preview of Options > Player, and the proof that names in any script are handwritten
// (Latin and Cyrillic in the chosen style, Arabic, kana, kanji and hanzi in their own hands).
void handwritingPreview(const std::string& name, int hand, const Rect& area) {
    TextStyle cap = style(font::FACE_TITLE, 17.0f, withAlpha(gold, 0.85f), im::startAlign(), 0.2f);
    gfx::text(T("player.preview"), im::flipX(area, area.x + 4.0f), area.y + 12.0f, cap);
    Rect paper(area.x, area.y + 28.0f, area.w, area.h - 28.0f);
    gfx::shadow(paper.offset(0, 6), 2, 18, withAlpha(black, 0.55f));
    gfx::fillV(paper, vec4(0.95f, 0.93f, 0.86f, 1.0f), vec4(0.89f, 0.86f, 0.78f, 1.0f), 2.0f);
    // Ruled lines and the margin rule of a scoresheet.
    const float lineGap = paper.h * 0.5f;
    for (int i = 1; i <= 2; ++i)
        gfx::hline(paper.x + 14.0f, paper.r() - 14.0f, paper.y + lineGap * float(i) - 12.0f, vec4(0.32f, 0.42f, 0.58f, 0.38f));
    float margin = 76.0f;
    gfx::vline(im::flipX(paper, paper.x + margin), paper.y + 4.0f, paper.b() - 4.0f, vec4(0.72f, 0.24f, 0.22f, 0.45f));
    // The name sits on the first rule, aligned the way its own script runs.
    TextStyle ns;
    ns.hand = hand;
    ns.size = 58.0f;
    ns.color = vec4(0.10f, 0.13f, 0.30f, 0.95f);
    float avail = paper.w - margin - 40.0f;
    ns.size = gfx::fitSize(name, ns, avail, 0.5f);
    bool nameRtl = gfx::textDirection(name, ns) == 1;
    ns.align = nameRtl ? HAlign::Right : HAlign::Left;
    ns.dir = nameRtl ? 1 : 0;
    float left = im::rtl() ? paper.x + 24.0f : paper.x + margin + 20.0f;
    float right = im::rtl() ? paper.r() - margin - 20.0f : paper.r() - 24.0f;
    gfx::text(name, nameRtl ? right : left, paper.y + lineGap - 20.0f, ns);
    // A line of moves as they will be written (figurine-free algebraic notation).
    TextStyle ms = ns;
    ms.size = 38.0f;
    ms.align = HAlign::Left;
    ms.dir = 0;
    ms.color = vec4(0.10f, 0.13f, 0.30f, 0.85f);
    gfx::text("1. e4  e5   2. Nf3  Nc6   3. Bb5  a6", left, paper.y + 2.0f * lineGap - 20.0f, ms);
}

// ---- Options page -----------------------------------------------------------------------------------
void openOptions() {
    S.opt.work = game::settings();
    S.opt.confirmDiscard = false;
    S.opt.pageT = 0.0f;
    if (S.forcedTab >= 0) {
        S.opt.tab = S.forcedTab;
        S.forcedTab = -1;
    }
    im::sound(Sound::Open);
}

// Draws the options page. Returns true when the page is closed; 'act' receives OptionsChanged
// when the player applied changes.
bool optionsPage(MenuAction& act) {
    OptionsState& o = S.opt;
    S.optionsVisible = true;
    o.pageT = std::min(1.0f, o.pageT + im::dt() / 0.3f);
    float t = ease(o.pageT);
    vec2 v = view();
    dimScene(1.0f);
    float w = std::min(1120.0f, v.x - 80.0f), h = 760.0f;
    Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 14.0f, w, h);
    im::captureMouseAll();
    im::captureKeyboard();
    bool closing = false, apply = false;
    bool dirty = !sameOptions(o.work, game::settings());
    if (o.confirmDiscard) im::pushBlock();
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(T("options.title"), p.cx(), p.y + 80.0f);
    im::pushId("options");
    const std::vector<std::string> tabs = {T("options.tab.display"), T("options.tab.graphics"), T("options.tab.audio"),
                                           T("options.tab.gameplay"), T("options.tab.player"), T("options.tab.online"),
                                           T("options.tab.controls")};
    im::tabBar(tabs, o.tab, Rect(p.x + 60.0f, p.y + 124.0f, p.w - 120.0f, 50.0f));

    game::Settings& s = o.work;
    float rx = p.x + 70.0f, rw = p.w - 140.0f, rh = 60.0f;
    float y = p.y + 200.0f;
    auto row = [&]() {
        Rect r(rx, y, rw, rh - 4.0f);
        y += rh;
        return r;
    };
    auto pct = [](float x) { return percent(x); };
    im::pushId(o.tab);
    im::beginHelpSink();
    switch (o.tab) {
        case 0: {
            // Language: applied and saved at once (the page itself changes language).
            const std::vector<i18n::Language>& langs = i18n::languages();
            std::vector<std::string> names;
            for (const i18n::Language& l : langs) names.push_back(l.nativeName);
            int li = std::max(0, i18n::languageIndex(i18n::language()));
            if (im::selectorRow(L("options.language"), li, names, row())) {
                const char* code = langs[size_t(li)].code;
                i18n::setLanguage(code);
                game::settings().language = code;
                s.language = code;
                game::settings().save();
            }
            im::tooltip(T("options.language.help"));
            int mode = s.fullscreen ? 0 : 1;
            if (im::selectorRow(L("options.display_mode"), mode, {T("options.fullscreen"), T("options.windowed")}, row()))
                s.fullscreen = mode == 0;
            auto& res = detail::data().resolutions;
            std::vector<std::string> labels;
            int cur = -1;
            for (size_t i = 0; i < res.size(); ++i) {
                labels.push_back(std::to_string(res[i].x) + " \xC3\x97 " + std::to_string(res[i].y));
                if (res[i].x == s.displayWidth && res[i].y == s.displayHeight) cur = int(i);
            }
            if (cur < 0) {
                labels.push_back(std::to_string(s.displayWidth) + " \xC3\x97 " + std::to_string(s.displayHeight));
                cur = int(labels.size()) - 1;
            }
            int sel = cur;
            if (im::selectorRow(L("options.window_size"), sel, labels, row(), !s.fullscreen) && sel < int(res.size())) {
                s.displayWidth = res[size_t(sel)].x;
                s.displayHeight = res[size_t(sel)].y;
            }
            im::tooltip(T("options.window_size.help"));
            im::toggleRow(L("options.vsync"), s.vsync, row());
            im::tooltip(T("options.vsync.help"));
            im::sliderRow(L("options.render_scale"), s.renderScale, 0.5f, 2.0f, 0.05f, pct, row());
            im::tooltip(T("options.render_scale.help"));
            break;
        }
        case 1: {
            int q = std::clamp(s.quality, 0, 3);
            if (im::selectorRow(L("options.quality"), q,
                                {T("options.quality.low"), T("options.quality.medium"), T("options.quality.high"), T("options.quality.ultra")},
                                row()))
                s.quality = q;
            im::tooltip(T("options.quality.help"));
            im::toggleRow(L("options.motion_blur"), s.motionBlur, row());
            im::toggleRow(L("options.depth_of_field"), s.depthOfField, row());
            im::sliderRow(L("options.brightness"), s.brightness, -2.0f, 2.0f, 0.1f,
                          [](float x) {
                              if (std::fabs(x) < 0.05f) return T("options.brightness.neutral");
                              return std::string(x > 0.0f ? "+" : "\xE2\x88\x92") + decimal(std::fabs(x), 1) + " EV";
                          },
                          row());
            im::tooltip(T("options.brightness.help"));
            break;
        }
        case 2: {
            im::sliderRow(L("options.master_volume"), s.masterVolume, 0.0f, 1.0f, 0.05f, pct, row());
            im::sliderRow(L("options.effects"), s.effectsVolume, 0.0f, 1.0f, 0.05f, pct, row());
            im::toggleRow(L("options.ambience"), s.ambience, row());
            im::tooltip(T("options.ambience.help"));
            im::sliderRow(L("options.ambience_volume"), s.ambienceVolume, 0.0f, 1.0f, 0.05f, pct, row(), s.ambience);
            break;
        }
        case 3: {
            im::toggleRow(L("options.legal_moves"), s.showLegalMoves, row());
            im::tooltip(T("options.legal_moves.help"));
            im::toggleRow(L("options.coordinates"), s.showCoordinates, row());
            im::toggleRow(L("options.thinking_time"), s.humanizeThinking, row());
            im::tooltip(T("options.thinking_time.help"));
            im::sliderRow(L("options.mouse_sensitivity"), s.mouseSensitivity, 0.25f, 3.0f, 0.05f,
                          [](float x) { return decimal(x, 2) + " \xC3\x97"; }, row());
            im::toggleRow(L("options.invert_look"), s.invertLook, row());
            im::toggleRow(L("options.game_cursor"), s.gameCursor, row());
            im::tooltip(T("options.game_cursor.help"));
            {
                // Hot-seat handover: an instant cut, or a flight of 0.8 to 2 s.
                static const float kHandover[] = {0.0f, 0.8f, 1.0f, 1.2f, 1.4f, 1.6f, 1.8f, 2.0f};
                int n = int(sizeof(kHandover) / sizeof(kHandover[0])), hi = 0;
                for (int i = 0; i < n; ++i)
                    if (std::fabs(kHandover[i] - s.handoverSeconds) < std::fabs(kHandover[hi] - s.handoverSeconds)) hi = i;
                if (im::stepperRow(L("options.handover"), hi, n,
                                   [](int i) {
                                       return i == 0 ? T("options.handover.cut")
                                                     : i18n::trf("options.handover.flight", {decimal(kHandover[i], 1)});
                                   },
                                   row()))
                    s.handoverSeconds = kHandover[hi];
                im::tooltip(T("options.handover.help"));
            }
            break;
        }
        case 4: {
            // Player profile: the name and hand written on the scoresheets.
            // The untouched default name (stored as "Human") reads in the interface language.
            std::string name = s.playerName == "Human" ? T("player.default_name") : s.playerName;
            if (im::textField(L("player.name"), name, row(), kMaxNameLength)) s.playerName = name;
            im::tooltip(T("player.name.help"));
            int hs = std::clamp(int(s.handStyle), 0, int(font::HAND_STYLE_COUNT) - 1);
            if (im::selectorRow(L("player.handwriting"), hs, {T("hand.caveat"), T("hand.marck"), T("hand.badscript")}, row()))
                s.handStyle = font::HandStyle(hs);
            im::tooltip(T("player.handwriting.help"));
            std::string written = cleanName(s.playerName);
            handwritingPreview(written == "Human" ? T("player.default_name") : written, hs, Rect(rx, y + 18.0f, rw, 170.0f));
            break;
        }
        case detail::kOnlineOptionsTab: detail::onlineOptionsRows(s, rx, rw, y); break;
        default: {
            struct Line { const char* keys; const char* action; };
            static const Line lines[] = {
                {"controls.touch.keys", "controls.touch"},
                {"controls.clock.keys", "controls.clock"},
                {"controls.look.keys", "controls.look"},
                {"controls.sheet.keys", "controls.sheet"},
                {"controls.moves.keys", "controls.moves"},
                {"controls.menu.keys", "controls.menu"},
            };
            TextStyle ks = style(font::FACE_TITLE, 19.0f, gold, im::endAlign(), 0.14f);
            TextStyle as = style(font::FACE_TEXT, kBody, ivory, im::startAlign());
            float mid = p.cx() - 10.0f;
            for (const Line& l : lines) {
                Rect r = row();
                TextStyle k = ks, a = as;
                k.size = gfx::fitSize(T(l.keys), ks, mid - 24.0f - rx - 10.0f);
                a.size = gfx::fitSize(T(l.action), as, rx + rw - mid - 34.0f);
                gfx::text(T(l.keys), im::flipX(p, mid - 24.0f), baselineCentered(r, k), k);
                gfx::diamond(vec2(im::flipX(p, mid), r.cy()), 3.0f, withAlpha(gold, 0.6f));
                gfx::text(T(l.action), im::flipX(p, mid + 24.0f), baselineCentered(r, a), a);
                gfx::hlineFade(rx + 60.0f, rx + rw - 60.0f, r.b() + 2.0f, withAlpha(gold, 0.10f), 0.3f);
            }
            break;
        }
    }
    std::string help = im::endHelpSink();
    im::popId();

    // Help line for the highlighted row.
    float bw = 240.0f, bh = 56.0f;
    float by = p.b() - 50.0f - bh;
    {
        im::Anim& ha = im::anim(im::makeId("##help"));
        if (!help.empty()) S.opt.help = help;
        ha.v[3] = im::approach(ha.v[3], help.empty() ? 0.0f : 1.0f, 10.0f);
        if (ha.v[3] > 0.01f && !S.opt.help.empty()) {
            TextStyle hs = style(font::FACE_ITALIC, 23.0f, withAlpha(ivoryDim, ha.v[3]), HAlign::Center);
            float hy = by - 88.0f;
            gfx::hlineFade(p.cx() - 200.0f, p.cx() + 200.0f, hy - 34.0f, withAlpha(gold, 0.3f * ha.v[3]), 0.5f);
            gfx::textWrapped(S.opt.help, p.cx(), hy, p.w - 220.0f, hs, 30.0f);
        }
    }
    // Footer.
    TextStyle hs = style(font::FACE_ITALIC, kCaption, withAlpha(muted, dirty ? 1.0f : 0.0f), HAlign::Center);
    hs.size = gfx::fitSize(T("options.pending"), hs, p.w - 120.0f - 2.0f * bw - 40.0f);
    gfx::text(T("options.pending"), p.cx(), by + bh * 0.5f + 7.0f, hs);
    if (im::button(L("common.back"), im::flip(p, Rect(p.x + 60.0f, by, bw, bh)), im::ButtonKind::Secondary)) closing = true;
    im::Id applyId = im::makeId("##common.apply");
    if (im::button(L("common.apply"), im::flip(p, Rect(p.r() - 60.0f - bw, by, bw, bh)), im::ButtonKind::Primary, dirty))
        apply = true;
    im::setDefaultFocus(applyId);
    if (!dirty) im::setDefaultFocus(im::makeId("##tabs"));
    im::popId();
    gfx::popAlpha();
    if (o.confirmDiscard) im::popBlock();
    if (!o.confirmDiscard && im::consumeBack()) {
        closing = true;
        im::sound(Sound::Back);
    }
    if (closing && dirty) {
        o.confirmDiscard = true;
        closing = false;
    }
    if (o.confirmDiscard) {
        int r = im::confirmDialog("##discard", T("options.unsaved.title"), T("options.unsaved.text"), T("common.apply"),
                                  T("common.discard"), false);
        if (r == 1) { apply = true; closing = true; }
        if (r == 0) closing = true;
        if (r >= 0) o.confirmDiscard = false;
    }
    if (apply) {
        game::Settings before = game::settings();
        copyOptions(game::settings(), o.work);
        game::settings().save();
        // Another server: its own sign-in (credentials are kept per server by the network layer).
        if (detail::onlineServerChanged(before, game::settings())) game::onlineSession().applyServer();
        act = MenuAction::OptionsChanged;
        o.work = game::settings();
        im::sound(Sound::Confirm);
    }
    if (closing) im::sound(Sound::Close);
    return closing;
}

// ---- Title page -----------------------------------------------------------------------------------
MenuAction titlePage(float t) {
    vec2 v = view();
    MenuAction act = MenuAction::None;
    const Rect sr = screenRect();  // the page is mirrored in a right-to-left language
    const HAlign start = im::startAlign();
    float x = std::max(110.0f, v.x * 0.085f);
    // Legibility gradient on the start side, fading into the hall.
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_BACK);
    if (im::rtl()) gfx::fillH(im::flip(sr, Rect(0, 0, v.x * 0.62f, v.y)), vec4(0, 0, 0, 0.0f), vec4(0, 0, 0, 0.66f * t));
    else gfx::fillH(Rect(0, 0, v.x * 0.62f, v.y), vec4(0, 0, 0, 0.66f * t), vec4(0, 0, 0, 0.0f));
    gfx::fillV(Rect(0, v.y - 200.0f, v.x, 200.0f), vec4(0, 0, 0, 0.0f), vec4(0, 0, 0, 0.45f * t));
    gfx::setLayer(prev);
    gfx::pushAlpha(t);
    float slide = (1.0f - t) * 10.0f;

    TextStyle wm = style(font::FACE_TITLE, kWordmark, ivory, start, kTrackWordmark);
    float wmBase = 330.0f + slide;
    float wmW = gfx::textWidth("SCACELITH", wm);
    TextStyle sh = wm;
    sh.color = vec4(0, 0, 0, 0.55f);
    sh.softness = 10.0f;
    sh.weight = 3.0f;
    gfx::text("SCACELITH", im::flipX(sr, x) + 2.0f, wmBase + 5.0f, sh);
    TextStyle gl = wm;
    gl.color = withAlpha(gold, 0.16f);
    gl.softness = 16.0f;
    gl.weight = 4.0f;
    gfx::text("SCACELITH", im::flipX(sr, x), wmBase, gl);
    gfx::text("SCACELITH", im::flipX(sr, x), wmBase, wm);
    float ruleY = wmBase + 38.0f;
    Rect rule = im::flip(sr, Rect(x, gfx::snap(ruleY), wmW, gfx::px()));
    if (im::rtl()) gfx::fillH(rule, withAlpha(gold, 0.0f), withAlpha(gold, 0.9f));
    else gfx::fillH(rule, withAlpha(gold, 0.9f), withAlpha(gold, 0.0f));
    gfx::diamond(vec2(im::flipX(sr, x), ruleY + gfx::px() * 0.5f), 4.0f, goldBright);
    TextStyle sub = style(font::FACE_ITALIC, 30.0f, ivoryDim, start);
    gfx::text(T("menu.subtitle"), im::flipX(sr, x + 4.0f), ruleY + 48.0f, sub);

    float ey = 490.0f + slide;
    float eh = 62.0f, ew = 440.0f, step = 70.0f;
    im::pushId("title");
    im::Id first = im::makeId("##menu.new_game");
    if (im::menuEntry(L("menu.new_game"), im::flip(sr, Rect(x, ey, ew, eh)))) {
        setPage(Page::NewGame);
        im::sound(Sound::Open);
    }
    if (im::menuEntry(L("menu.online"), im::flip(sr, Rect(x, ey + step, ew, eh)))) {
        setPage(Page::Online);
        im::sound(Sound::Open);
    }
    if (im::menuEntry(L("menu.watch"), im::flip(sr, Rect(x, ey + 2.0f * step, ew, eh)))) {
        setPage(Page::Watch);
        im::sound(Sound::Open);
    }
    if (im::menuEntry(L("menu.options"), im::flip(sr, Rect(x, ey + 3.0f * step, ew, eh)))) {
        setPage(Page::Options);
        openOptions();
    }
    if (im::menuEntry(L("menu.credits"), im::flip(sr, Rect(x, ey + 4.0f * step, ew, eh)))) {
        setPage(Page::Credits);
        im::sound(Sound::Open);
    }
    if (im::menuEntry(L("menu.quit"), im::flip(sr, Rect(x, ey + 5.0f * step, ew, eh)))) act = MenuAction::Quit;
    im::setDefaultFocus(first);
    im::popId();

    detail::titleRating(im::flipX(sr, x), ey + 5.0f * step + 132.0f);
    TextStyle vs = style(font::FACE_ITALIC, 19.0f, withAlpha(muted, 0.85f), start);
    gfx::text(i18n::trf("menu.version", {i18n::ltr(detail::data().version)}), im::flipX(sr, x), v.y - 48.0f, vs);
    gfx::popAlpha();
    return act;
}

// ---- New game page ----------------------------------------------------------------------------------
// Row heights of the opponent list: full rows carry the one-line description; while "Custom" is
// selected the other rows fold to a single line to make room for the engine parameters.
constexpr float kPresetRowTall = 60.0f, kPresetRowShort = 40.0f;

float presetListHeight(int n, int selected, bool compact) {
    float h = 0.0f;
    for (int i = 0; i < n; ++i) h += (!compact || i == selected) ? kPresetRowTall : kPresetRowShort;
    return h;
}

bool difficultyList(NewGameSetup& setup, const Rect& area, bool opened) {
    auto& list = detail::data().difficulties;
    int n = int(list.size());
    bool compact = n > 0 && setup.difficulty == n - 1;
    if (area.contains(im::mouse()) && im::wheel() != 0.0f) S.presetScrollTarget -= im::wheel() * kPresetRowTall;
    bool changed = false;
    gfx::pushClip(Rect(area.x - 12.0f, area.y, area.w + 24.0f, area.h));
    im::pushId("difficulty");
    float y = 0.0f;
    for (int i = 0; i < n; ++i) {
        const DifficultyInfo& d = list[size_t(i)];
        bool sel = setup.difficulty == i;
        im::Id rid = im::makeId(i);
        im::Anim& a = im::anim(rid);
        float target = (!compact || sel) ? kPresetRowTall : kPresetRowShort;
        a.v[3] = (opened || a.firstFrame == im::frame()) ? target : im::approach(a.v[3], target, 16.0f);
        float rh = a.v[3];
        float full = m::saturate((rh - kPresetRowShort) / (kPresetRowTall - kPresetRowShort));
        Rect r(area.x, area.y + y - S.presetScroll, area.w, rh - 4.0f);
        float top = y;
        y += rh;
        im::Item it = im::item(rid, r);
        if (it.focused && im::keyboardMode()) {  // keep the focused row visible
            if (top < S.presetScrollTarget) S.presetScrollTarget = top;
            if (top + rh > S.presetScrollTarget + area.h) S.presetScrollTarget = top + rh - area.h;
        }
        if (it.activated && !sel) {
            setup.difficulty = i;
            changed = true;
            im::sound(Sound::Toggle);
        }
        if (r.b() < area.y || r.y > area.b()) continue;
        if (sel) {
            if (im::rtl()) gfx::fillH(r, withAlpha(gold, 0.04f), withAlpha(gold, 0.13f), 2.0f);
            else gfx::fillH(r, withAlpha(gold, 0.13f), withAlpha(gold, 0.04f), 2.0f);
            gfx::stroke(r, withAlpha(gold, 0.55f), 0.0f, 2.0f);
            gfx::diamond(vec2(im::flipX(r, r.x), r.cy()), 4.5f, goldBright);
        } else {
            im::rowHighlight(r, it.hoverT);
        }
        float nameBase = r.y + m::lerp(26.0f, 25.0f, full);
        std::string elo = d.elo > 0 ? i18n::trf("newgame.elo", {std::to_string(d.elo)}) : std::string();
        TextStyle es = style(font::FACE_ITALIC, 22.0f, sel ? gold : muted, im::endAlign());
        float eloW = elo.empty() ? 0.0f : gfx::textWidth(elo, es) + 24.0f;
        TextStyle ns = style(font::FACE_TEXT, 27.0f, sel ? goldBright : theme::mix(ivory, goldBright, it.hoverT * 0.5f), im::startAlign());
        std::string name = presetName(d.name);
        ns.size = gfx::fitSize(name, ns, r.w - 40.0f - eloW);
        gfx::text(name, im::flipX(r, r.x + 22.0f), nameBase, ns);
        if (!elo.empty()) gfx::text(elo, im::flipX(r, r.r() - 18.0f), nameBase, es);
        if (full > 0.02f) {
            TextStyle ds = style(font::FACE_ITALIC, 20.0f, withAlpha(sel ? ivoryDim : muted, full * full), im::startAlign());
            std::string desc = presetDescription(d.name, d.description);
            ds.size = gfx::fitSize(desc, ds, r.w - 40.0f, 0.75f);
            gfx::text(desc, im::flipX(r, r.x + 22.0f), r.y + 47.0f, ds);
        }
    }
    im::popId();
    gfx::popClip();
    float contentH = y;
    float maxScroll = std::max(0.0f, contentH - area.h);
    S.presetScrollTarget = m::clamp(S.presetScrollTarget, 0.0f, maxScroll);
    S.presetScroll = opened ? S.presetScrollTarget : im::approach(S.presetScroll, S.presetScrollTarget, 18.0f);
    S.presetScroll = std::min(S.presetScroll, maxScroll);
    if (maxScroll > 0.5f) {  // discreet scroll indicator and edge fades
        float frac = area.h / contentH;
        float pos = S.presetScroll / maxScroll;
        float bh = area.h * frac;
        gfx::fill(Rect(area.r() + 10.0f, area.y + (area.h - bh) * pos, 2.0f, bh), withAlpha(gold, 0.35f), 1.0f);
        vec4 pc(0.05f, 0.043f, 0.039f, 0.95f), pz(0.05f, 0.043f, 0.039f, 0.0f);
        if (S.presetScroll > 0.5f) gfx::fillV(Rect(area.x - 12.0f, area.y, area.w + 24.0f, 22.0f), pc, pz);
        if (S.presetScroll < maxScroll - 0.5f) gfx::fillV(Rect(area.x - 12.0f, area.b() - 22.0f, area.w + 24.0f, 22.0f), pz, pc);
    }
    return changed;
}

// The opponent column of a two-player game (hot-seat): both names in their handwriting, the clock
// side (the player with the clock on the left plays left-handed), rated or friendly.
void hotSeatColumn(NewGameSetup& setup, float x, float w, float y) {
    const Rect col(x, 0, w, 0);
    const float rh = 54.0f;
    auto row = [&]() {
        Rect r(x, y, w, rh - 6.0f);
        y += rh;
        return r;
    };
    const std::vector<std::string> hands = {T("hand.caveat"), T("hand.marck"), T("hand.badscript")};
    im::pushId("hotseat");
    for (int c = 0; c < 2; ++c) {
        im::pushId(c);
        TextStyle ns;
        ns.hand = std::clamp(setup.hands[c], 0, int(font::HAND_STYLE_COUNT) - 1);
        ns.size = 36.0f;
        ns.color = ivory;
        std::string name = setup.names[c];
        if (im::textField(c == 0 ? L("hotseat.white_player") : L("hotseat.black_player"), name, row(), kMaxNameLength, &ns))
            setup.names[c] = name;
        im::tooltip(T("hotseat.name.help"));
        int hs = ns.hand;
        if (im::selectorRow(L("player.handwriting"), hs, hands, row())) setup.hands[c] = hs;
        im::tooltip(T("player.handwriting.help"));
        im::popId();
        if (c == 0) {
            // Swap colours: between the two players.
            TextStyle qs = style(font::FACE_ITALIC, kSmall, muted);
            float bw = std::max(200.0f, gfx::textWidth(T("hotseat.swap"), qs) + 40.0f);
            if (im::button(L("hotseat.swap"), im::flip(col, Rect(x + w - bw, y - 2.0f, bw, 40.0f)), im::ButtonKind::Quiet)) {
                std::swap(setup.names[0], setup.names[1]);
                std::swap(setup.hands[0], setup.hands[1]);
                setup.clockRightOf = 1 - setup.clockRightOf;  // each keeps the hand they play with
                im::sound(Sound::Toggle);
            }
            y += 46.0f;
        }
    }
    gfx::hlineFade(x, x + w, y + 4.0f, withAlpha(gold, 0.25f), 0.25f);
    y += 18.0f;
    std::string white = hotSeatName(setup, 0), black = hotSeatName(setup, 1);
    int side = std::clamp(setup.clockRightOf, 0, 1);
    if (im::selectorRow(L("hotseat.clock"), side,
                        {i18n::trf("hotseat.clock_right_of", {white}), i18n::trf("hotseat.clock_right_of", {black})}, row()))
        setup.clockRightOf = side;
    im::tooltip(T("hotseat.clock.help"));
    TextStyle note = style(font::FACE_ITALIC, 21.0f, ivoryDim, im::startAlign());
    std::string lefty = i18n::trf("hotseat.left_hand", {side == 0 ? black : white});
    note.size = gfx::fitSize(lefty, note, w - 30.0f);
    gfx::diamond(vec2(im::flipX(col, x + 6.0f), y + 4.0f), 3.0f, withAlpha(gold, 0.7f));
    gfx::text(lefty, im::flipX(col, x + 22.0f), y + 11.0f, note);
    y += 40.0f;
    im::toggleRow(L("hotseat.rated"), setup.rated, row());
    im::tooltip(T("hotseat.rated.help"));
    std::string line;
    if (setup.rated) {
        const game::Settings& g = game::settings();
        auto rating = [&](const std::string& n) {
            const game::LocalPlayer* lp = g.findLocalPlayer(n);
            return lp && lp->record.games > 0 ? std::to_string(lp->record.rating) : T("hotseat.new_rating");
        };
        line = i18n::trf("hotseat.ratings", {white, rating(white), black, rating(black)});
    } else {
        line = T("hotseat.friendly_note");
    }
    note.color = muted;
    note.size = gfx::fitSize(line, style(font::FACE_ITALIC, 21.0f, muted), w - 30.0f);
    gfx::diamond(vec2(im::flipX(col, x + 6.0f), y + 4.0f), 3.0f, withAlpha(gold, 0.5f));
    gfx::text(line, im::flipX(col, x + 22.0f), y + 11.0f, note);
    im::popId();
}

MenuAction newGamePage(NewGameSetup& setup, bool opened) {
    vec2 v = view();
    MenuAction act = MenuAction::None;
    if (opened) {
        loadSetupFromSettings(setup);
        S.presetScrollTarget = std::max(0.0f, float(setup.difficulty) * kPresetRowTall - 200.0f);
        S.presetScroll = S.presetScrollTarget;
    }
    auto& diffs = detail::data().difficulties;
    auto& tcs = detail::data().timeControls;
    int nd = int(diffs.size());
    setup.difficulty = std::clamp(setup.difficulty, 0, std::max(0, nd - 1));
    bool custom = nd > 0 && setup.difficulty == nd - 1;
    bool customTc = setup.timeControl < 0 || setup.timeControl >= int(tcs.size());

    float t = ease(S.pageT);
    dimScene(t);
    float w = std::min(1480.0f, v.x - 80.0f), h = 1000.0f;
    Rect p(v.x * 0.5f - w * 0.5f, 40.0f + (1.0f - t) * 14.0f, w, h);
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(T("newgame.title"), p.cx(), p.y + 78.0f);
    im::pushId("newgame");

    float pad = 64.0f, gap = 72.0f;
    float colW = (p.w - 2.0f * pad - gap) * 0.5f;
    // Opponent column first in the reading direction (on the right in a right-to-left language).
    float lx = im::flip(p, Rect(p.x + pad, 0, colW, 0)).x, rx = im::flip(p, Rect(p.x + pad + colW + gap, 0, colW, 0)).x;
    float top = p.y + 150.0f;
    float footer = p.b() - 118.0f;
    // Column divider.
    gfx::vline(p.cx(), top, footer - 20.0f, withAlpha(gold, 0.12f));

    // Opponent column: Stockfish (a preset, or custom engine parameters), or a second player on
    // this PC (hot-seat).
    im::sectionLabel(T("newgame.opponent"), lx, top + 8.0f, colW);
    bool hotSeat = setup.opponent == 1;
    if (!hotSeat) detail::newGameRating(lx, colW, top + 8.0f, setup.difficulty);
    {
        int kind = hotSeat ? 1 : 0;
        if (im::selectorRow(L("newgame.opponent_kind"), kind, {T("newgame.vs_stockfish"), T("newgame.vs_human")},
                            Rect(lx, top + 30.0f, colW, 50.0f))) {
            setup.opponent = kind;
            hotSeat = kind == 1;
            custom = !hotSeat && nd > 0 && setup.difficulty == nd - 1;
        }
        im::tooltip(T("newgame.opponent_kind.help"));
    }
    const float listTop = top + 92.0f;
    if (hotSeat) {
        custom = false;
        hotSeatColumn(setup, lx, colW, listTop);
    }
    float customH = custom ? 6.0f * 44.0f + 18.0f : 0.0f;
    float available = std::max(150.0f, footer - listTop - customH - 8.0f);
    Rect listArea(lx, listTop, colW, std::min(available, presetListHeight(nd, setup.difficulty, custom)));
    if (!hotSeat && difficultyList(setup, listArea, opened)) {
        custom = setup.difficulty == nd - 1;
    }
    if (custom) {
        float y = listArea.b() + 16.0f;
        gfx::hlineFade(lx, lx + colW, y - 8.0f, withAlpha(gold, 0.3f), 0.25f);
        auto crow = [&]() {
            Rect r(lx, y, colW, 42.0f);
            y += 44.0f;
            return r;
        };
        im::pushId("engine");
        float skill = float(setup.skillLevel);
        if (im::sliderRow(L("engine.skill"), skill, 0.0f, 20.0f, 1.0f, [](float x) { return format("%.0f", x); }, crow(),
                          !setup.limitElo))
            setup.skillLevel = int(std::lround(skill));
        im::tooltip(T("engine.skill.help"));
        im::toggleRow(L("engine.limit_elo"), setup.limitElo, crow());
        im::tooltip(T("engine.limit_elo.help"));
        float elo = float(setup.elo);
        if (im::sliderRow(L("engine.elo"), elo, 1320.0f, 3190.0f, 10.0f, [](float x) { return format("%.0f", x); }, crow(),
                          setup.limitElo))
            setup.elo = int(std::lround(elo));
        int depth = std::clamp(setup.depth, 0, 30);
        if (im::stepperRow(L("engine.depth"), depth, 31, [](int i) { return i == 0 ? T("engine.no_limit") : i18n::trn("engine.plies", i); },
                           crow()))
            setup.depth = depth;
        auto& mt = moveTimeValues();
        int mti = nearestIndex(mt, setup.moveTimeMs);
        if (im::stepperRow(L("engine.move_time"), mti, int(mt.size()), [&](int i) { return moveTimeText(mt[size_t(i)]); }, crow()))
            setup.moveTimeMs = mt[size_t(mti)];
        auto& nv = nodeValues();
        int ni = nearestIndex(nv, setup.nodes);
        if (im::stepperRow(L("engine.nodes"), ni, int(nv.size()), [&](int i) { return nodesText(nv[size_t(i)]); }, crow()))
            setup.nodes = nv[size_t(ni)];
        im::popId();
    }

    // Time control column.
    im::sectionLabel(T("newgame.time_control"), rx, top + 8.0f, colW);
    int ntc = int(tcs.size()) + 1;
    int cols = 4;
    float cgap = 12.0f;
    float cw = (colW - cgap * float(cols - 1)) / float(cols), ch = 66.0f;
    float gy = top + 30.0f;
    im::pushId("tc");
    for (int i = 0; i < ntc; ++i) {
        bool isCustom = i == ntc - 1;
        Rect r = im::flip(Rect(rx, gy, colW, 0), Rect(rx + float(i % cols) * (cw + cgap), gy + float(i / cols) * (ch + cgap), cw, ch));
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
        std::string label = isCustom ? T("tc.custom") : spacedPlus(timeControlLabel(tcs[size_t(i)]));
        std::string cat = isCustom ? T("tc.your_own") : timeCategory(tcs[size_t(i)]);
        TextStyle ls = style(font::FACE_TEXT, 28.0f, sel ? goldBright : theme::mix(ivory, goldBright, it.hoverT * 0.6f), HAlign::Center);
        if (label.size() > 8) ls.size = 25.0f;
        ls.size = gfx::fitSize(label, ls, cw - 12.0f);
        gfx::text(label, r.cx(), r.y + 33.0f, ls);
        TextStyle cs = style(font::FACE_ITALIC, 18.0f, sel ? gold : muted, HAlign::Center);
        cs.size = gfx::fitSize(cat, cs, cw - 10.0f);
        gfx::text(cat, r.cx(), r.y + 55.0f, cs);
    }
    im::popId();
    float y = gy + float((ntc + cols - 1) / cols) * (ch + cgap) + 10.0f;
    if (customTc) {
        auto trow = [&]() {
            Rect r(rx, y, colW, 42.0f);
            y += 44.0f;
            return r;
        };
        im::pushId("customtc");
        auto& bv = baseTimeValues();
        int bi = nearestIndex(bv, setup.customBaseSeconds);
        if (im::stepperRow(L("tc.base_time"), bi, int(bv.size()), [&](int i) { return clockText(bv[size_t(i)]); }, trow()))
            setup.customBaseSeconds = bv[size_t(bi)];
        im::tooltip(T("tc.base_time.help"));
        int inc = std::clamp(setup.customIncrementSeconds, 0, 60);
        if (im::stepperRow(L("tc.increment"), inc, 61, [](int i) { return i18n::trf("tc.seconds", {i18n::ltr("+" + std::to_string(i))}); }, trow()))
            setup.customIncrementSeconds = inc;
        im::tooltip(T("tc.increment.help"));
        int del = std::clamp(setup.customDelaySeconds, 0, 60);
        if (im::stepperRow(L("tc.delay"), del, 61,
                           [](int i) { return i == 0 ? T("tc.no_delay") : i18n::trf("tc.seconds", {std::to_string(i)}); }, trow()))
            setup.customDelaySeconds = del;
        im::tooltip(T("tc.delay.help"));
        im::popId();
        y += 6.0f;
    }

    // Colour note.
    int next = game::settings().nextColor;
    std::string colourLine = T(next == 0 ? "newgame.you_white" : next == 1 ? "newgame.you_black" : "newgame.colour_lot");
    std::string colourNext = T("newgame.colour_alternate");
    if (hotSeat) {
        colourLine = i18n::trf("hotseat.colours", {hotSeatName(setup, 0), hotSeatName(setup, 1)});
        colourNext = T("hotseat.colours_swap");
    }
    const Rect col(rx, 0, colW, 0);
    TextStyle cs = style(font::FACE_ITALIC, 22.0f, ivoryDim, im::startAlign());
    float noteY = std::max(y + 26.0f, footer - 70.0f);
    gfx::diamond(vec2(im::flipX(col, rx + 6.0f), noteY - 7.0f), 3.5f, withAlpha(gold, 0.8f));
    cs.size = gfx::fitSize(colourLine, cs, colW - 26.0f);
    gfx::text(colourLine, im::flipX(col, rx + 22.0f), noteY, cs);
    cs.color = muted;
    cs.size = gfx::fitSize(colourNext, style(font::FACE_ITALIC, 22.0f, muted), colW - 26.0f);
    gfx::text(colourNext, im::flipX(col, rx + 22.0f), noteY + 28.0f, cs);

    // Footer.
    float bw = 260.0f, bh = 58.0f;
    float by = p.b() - 52.0f - bh;
    gfx::hlineFade(p.x + 40.0f, p.r() - 40.0f, by - 26.0f, withAlpha(gold, 0.25f), 0.3f);
    bool back = im::button(L("common.back"), im::flip(p, Rect(p.x + pad, by, bw, bh)), im::ButtonKind::Secondary);
    im::Id startId = im::makeId("##newgame.start");
    if (im::button(L("newgame.start"), im::flip(p, Rect(p.r() - pad - bw, by, bw, bh)), im::ButtonKind::Primary)) {
        for (int c = 0; c < 2; ++c) setup.names[c] = hotSeatName(setup, c);
        storeSetupToSettings(setup);
        act = MenuAction::StartGame;
    }
    // Summary of the choice next to the Start button.
    {
        std::string opp = nd > 0 ? presetName(diffs[size_t(setup.difficulty)].name) : "";
        if (hotSeat) opp = i18n::trf("hotseat.summary", {hotSeatName(setup, 0), hotSeatName(setup, 1)});
        std::string tc = customTc ? customClockSummary(setup) : spacedPlus(timeControlLabel(tcs[size_t(setup.timeControl)]));
        TextStyle ss = style(font::FACE_ITALIC, 22.0f, ivoryDim, im::endAlign());
        std::string summary = opp + "  \xC2\xB7  " + i18n::ltr(tc);
        ss.size = gfx::fitSize(summary, ss, p.w - 2.0f * pad - 2.0f * bw - 60.0f);
        gfx::text(summary, im::flipX(p, p.r() - pad - bw - 30.0f), by + bh * 0.5f + 7.0f, ss);
    }
    im::setDefaultFocus(startId);
    im::popId();
    gfx::popAlpha();
    if (back || im::consumeBack()) {
        if (!back) im::sound(Sound::Back);
        setPage(Page::Title);
    }
    return act;
}

// ---- Credits ----------------------------------------------------------------------------------------
void creditsPage() {
    vec2 v = view();
    float t = ease(S.pageT);
    dimScene(t);
    float w = std::min(1100.0f, v.x - 80.0f), h = 880.0f;
    Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 14.0f, w, h);
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(T("credits.title"), p.cx(), p.y + 80.0f);
    struct Entry { const char* head; const char* lines[3]; };
    static const Entry entries[] = {
        {"credits.game.head", {"credits.game", nullptr, nullptr}},
        {"credits.engine.head", {"credits.engine", "credits.engine.licence", nullptr}},
        {"credits.fonts.head", {"credits.fonts", "credits.fonts.hand", "credits.fonts.figures"}},
    };
    float y = p.y + 160.0f;
    TextStyle hs = style(font::FACE_TITLE, kSection, gold, HAlign::Center, 0.22f);
    TextStyle ls = style(font::FACE_TEXT, 24.0f, ivoryDim, HAlign::Center);
    for (const Entry& e : entries) {
        gfx::text(T(e.head), p.cx(), y, hs);
        y += 40.0f;
        for (const char* l : e.lines) {
            if (!l) break;
            y += 32.0f * float(gfx::textWrapped(T(l), p.cx(), y, w - 160.0f, ls, 32.0f));
        }
        y += 30.0f;
    }
    TextStyle qs = style(font::FACE_ITALIC, 24.0f, muted, HAlign::Center);
    qs.size = gfx::fitSize(T("credits.quote"), qs, w - 120.0f);
    gfx::text(T("credits.quote"), p.cx(), p.b() - 140.0f, qs);
    im::pushId("credits");
    im::Id backId = im::makeId("##common.back");
    bool back = im::button(L("common.back"), Rect(p.cx() - 130.0f, p.b() - 110.0f, 260.0f, 56.0f), im::ButtonKind::Secondary);
    im::setDefaultFocus(backId);
    im::popId();
    gfx::popAlpha();
    if (back || im::consumeBack()) {
        if (!back) im::sound(Sound::Back);
        setPage(Page::Title);
    }
}

}  // namespace

// ==== Public screens ==================================================================================
namespace detail {
void screensReset() { S = State(); }

// Hooks for the viewer mode's pages (ui_screens_game.cpp).
bool runOptionsPage(MenuAction& act) { return optionsPage(act); }
void openOptionsOnTab(int tab) {
    S.opt.tab = tab;
    S.forcedTab = tab;
    S.optionsToOnline = S.page == Page::Online;
    setPage(Page::Options);
    openOptions();
}
void openOptionsPage() { openOptions(); }
void dimBackground(float a) { dimScene(a); }

void screensBeginFrame(float dt) {
    for (auto& t : S.toasts) t.age += dt;
    S.toasts.erase(std::remove_if(S.toasts.begin(), S.toasts.end(), [](const Toast& t) { return t.age > t.duration + 0.6f; }),
                   S.toasts.end());
    S.optionsVisiblePrev = S.optionsVisible;
    S.optionsVisible = false;
}
void screensEndFrame() {}
}  // namespace detail

namespace debug {
void openMenuPage(MenuPage page) { S.forcedPage = int(page); }
void openOnlineMenu(const std::string& sub) {
    S.forcedPage = int(Page::Online);
    openOnlinePage(sub);
}
void setOptionsTab(int tab) { S.forcedTab = tab; S.opt.tab = tab; }
void openPauseConfirm(int which) { S.forcedPauseConfirm = which; }
void foldGameOver(bool folded) { S.forcedFold = folded ? 1 : 0; }
}  // namespace debug

bool optionsOpen() { return S.optionsVisible || S.optionsVisiblePrev; }

MenuAction mainMenu(NewGameSetup& setup) {
    static WatchSetup watch;
    return mainMenu(setup, watch);
}

MenuAction mainMenu(NewGameSetup& setup, WatchSetup& watch) {
    im::Id menuId = im::makeId("##mainmenu");
    bool appear = im::appearing(menuId);
    if (appear) {
        setPage(S.forcedPage >= 0 ? Page(S.forcedPage) : S.resumeOnline ? Page::Online : Page::Title);
        if (S.page == Page::Options) openOptions();
        S.forcedPage = -1;
        S.resumeOnline = false;
    }
    im::captureMouseAll();
    im::captureKeyboard();
    S.pageT = std::min(1.0f, S.pageT + im::dt() / 0.35f);
    MenuAction act = MenuAction::None;
    bool fresh = S.pageFresh;
    S.pageFresh = false;
    switch (S.page) {
        case Page::Title: act = titlePage(ease(S.pageT)); break;
        case Page::NewGame: act = newGamePage(setup, fresh); break;
        case Page::Options:
            if (optionsPage(act)) {
                setPage(S.optionsToOnline ? Page::Online : Page::Title);
                S.optionsToOnline = false;
            }
            break;
        case Page::Credits: creditsPage(); break;
        case Page::Watch: {
            bool back = false;
            act = detail::watchPage(watch, ease(S.pageT), fresh, back);
            if (back) setPage(Page::Title);
            break;
        }
        case Page::Online: {
            bool back = false;
            detail::onlinePage(ease(S.pageT), fresh, back);
            if (back) setPage(Page::Title);
            break;
        }
    }
    // Online: challenge cards on every page once signed in, the ping on the online page. A game
    // that starts from the online page brings the menu back to it afterwards.
    detail::onlineMenuOverlay(S.page == Page::Online);
    if (detail::onlineGameStarting()) S.resumeOnline = S.page == Page::Online;
    if (act == MenuAction::StartGame || act == MenuAction::Quit) setPage(Page::Title);
    if (act == MenuAction::StartWatching) setPage(Page::Title);
    return act;
}

MenuAction pauseMenu(bool canClaimDraw, bool canOfferDraw) { return pauseMenu(canClaimDraw, canOfferDraw, std::string()); }

MenuAction pauseMenu(bool canClaimDraw, bool canOfferDraw, const std::string& resignQuestion) {
    im::Id id = im::makeId("##pause");
    im::Anim& a = im::anim(id);
    bool appear = a.firstFrame == im::frame();
    if (appear) {
        S.pauseOptions = false;
        S.pauseConfirm = S.forcedPauseConfirm;
        S.forcedPauseConfirm = 0;
        im::sound(Sound::Open);
    }
    im::captureMouseAll();
    im::captureKeyboard();
    MenuAction act = MenuAction::None;
    if (S.pauseOptions) {
        if (optionsPage(act)) S.pauseOptions = false;
        return act;
    }
    a.v[5] = appear ? 0.0f : std::min(1.0f, a.v[5] + im::dt() / 0.3f);
    float t = ease(appear ? 0.0f : a.v[5]);
    if (appear) t = 0.0f;
    vec2 v = view();
    dimScene(std::max(t, 0.001f));
    float w = 560.0f, h = 640.0f;
    Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 12.0f, w, h);
    if (S.pauseConfirm) im::pushBlock();
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(T("pause.title"), p.cx(), p.y + 80.0f);
    im::pushId("pause");
    float y = p.y + 150.0f, eh = 58.0f, step = 70.0f;
    Rect er(p.x + 40.0f, y, p.w - 80.0f, eh);
    im::Id resumeId = im::makeId("##pause.resume");
    if (im::menuEntry(L("pause.resume"), er, true, HAlign::Center)) act = MenuAction::Resume;
    if (im::menuEntry(L("pause.offer_draw"), er.offset(0, step), canOfferDraw, HAlign::Center)) act = MenuAction::OfferDraw;
    if (im::menuEntry(L("pause.claim_draw"), er.offset(0, 2 * step), canClaimDraw, HAlign::Center)) act = MenuAction::ClaimDraw;
    im::tooltip(T("pause.claim_draw.help"));
    if (im::menuEntry(L("pause.resign"), er.offset(0, 3 * step), true, HAlign::Center)) S.pauseConfirm = 1;
    if (im::menuEntry(L("menu.options"), er.offset(0, 4 * step), true, HAlign::Center)) {
        S.pauseOptions = true;
        openOptions();
    }
    if (im::menuEntry(L("common.main_menu"), er.offset(0, 5 * step), true, HAlign::Center)) S.pauseConfirm = 2;
    im::setDefaultFocus(resumeId);
    im::popId();
    gfx::popAlpha();
    if (S.pauseConfirm) im::popBlock();
    if (!S.pauseConfirm && !appear && im::consumeBack()) {
        act = MenuAction::Resume;
        im::sound(Sound::Close);
    }
    if (S.pauseConfirm == 1) {
        int r = im::confirmDialog("##resign", T("confirm.resign.title"),
                                  resignQuestion.empty() ? T("confirm.resign.text") : resignQuestion, T("confirm.resign.ok"),
                                  T("common.cancel"), true);
        if (r == 1) act = MenuAction::Resign;
        if (r >= 0) S.pauseConfirm = 0;
    } else if (S.pauseConfirm == 2) {
        int r = im::confirmDialog("##leave", T("confirm.leave.title"), T("confirm.leave.text"), T("confirm.leave.ok"),
                                  T("common.cancel"), true);
        if (r == 1) act = MenuAction::BackToMainMenu;
        if (r >= 0) S.pauseConfirm = 0;
    }
    return act;
}

int promotionPicker(bool playerIsWhite) {
    im::Id id = im::makeId("##promotion");
    im::Anim& a = im::anim(id);
    bool appear = a.firstFrame == im::frame();
    a.v[5] = appear ? 0.0f : std::min(1.0f, a.v[5] + im::dt() / 0.25f);
    float t = ease(a.v[5]);
    if (appear) im::sound(Sound::Open);
    im::captureMouseAll();
    im::captureKeyboard();
    vec2 v = view();
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_BACK);
    gfx::fillV(Rect(0, v.y * 0.45f, v.x, v.y * 0.55f), vec4(0, 0, 0, 0.0f), vec4(0, 0, 0, 0.5f * t));
    gfx::setLayer(prev);

    struct Choice { const char* name; char key; uint32_t filled, outline; int piece; };
    static const Choice choices[4] = {
        {"piece.queen", 'Q', 0x265B, 0x2655, chess::Queen},
        {"piece.rook", 'R', 0x265C, 0x2656, chess::Rook},
        {"piece.bishop", 'B', 0x265D, 0x2657, chess::Bishop},
        {"piece.knight", 'N', 0x265E, 0x2658, chess::Knight},
    };
    float tile = 168.0f, gap = 22.0f;
    float w = 4.0f * tile + 3.0f * gap + 2.0f * 56.0f, h = 330.0f;
    Rect p(v.x * 0.5f - w * 0.5f, v.y - h - 90.0f + (1.0f - t) * 16.0f, w, h);
    gfx::pushAlpha(t);
    im::panel(p);
    TextStyle ts = style(font::FACE_TITLE, 22.0f, gold, HAlign::Center, 0.24f);
    gfx::text(T("promotion.title"), p.cx(), p.y + 58.0f, ts);
    im::ornamentRule(p.cx(), p.y + 76.0f, 150.0f);
    int result = 0;
    im::pushId("promo");
    for (int i = 0; i < 4; ++i) {
        const Choice& c = choices[i];
        Rect r = im::flip(p, Rect(p.x + 56.0f + float(i) * (tile + gap), p.y + 100.0f, tile, tile + 36.0f));
        im::Item it = im::item(im::makeId(i), r);
        if (i == 0) im::setDefaultFocus(it.id);
        float hv = it.hoverT;
        gfx::fillV(r, withAlpha(gold, 0.05f + 0.10f * hv), withAlpha(gold, 0.02f + 0.04f * hv), 3.0f);
        gfx::stroke(r, withAlpha(gold, 0.25f + 0.55f * hv), 0.0f, 3.0f);
        vec2 gc(r.cx(), r.y + 82.0f - 3.0f * hv);
        TextStyle gs;
        gs.face = font::FACE_SYMBOL;
        // Contact shadow.
        TextStyle sh = gs;
        sh.color = vec4(0, 0, 0, 0.6f);
        sh.softness = 6.0f;
        sh.weight = 2.0f;
        glyphCentered(c.filled, gc + vec2(3.0f, 6.0f), 128.0f, sh);
        if (playerIsWhite) {
            TextStyle body = gs;
            body.color = vec4(0.93f, 0.89f, 0.80f, 1.0f);
            glyphCentered(c.filled, gc, 128.0f, body);
            TextStyle ink = gs;
            ink.color = vec4(0.20f, 0.15f, 0.11f, 1.0f);
            glyphCentered(c.outline, gc, 128.0f, ink);
        } else {
            TextStyle halo = gs;  // faint light behind the ebony figure so its silhouette reads
            halo.color = withAlpha(ivory, 0.16f + 0.08f * hv);
            halo.softness = 12.0f;
            halo.weight = 5.0f;
            glyphCentered(c.filled, gc, 128.0f, halo);
            TextStyle body = gs;
            body.color = vec4(0.075f, 0.062f, 0.055f, 1.0f);
            glyphCentered(c.filled, gc, 128.0f, body);
            TextStyle rim = gs;
            rim.color = theme::mix(withAlpha(gold, 0.85f), goldBright, hv);
            glyphCentered(c.outline, gc, 128.0f, rim);
        }
        TextStyle ns = style(font::FACE_TITLE, 19.0f, theme::mix(ivoryDim, goldBright, hv), HAlign::Center, 0.18f);
        ns.size = gfx::fitSize(T(c.name), ns, tile - 12.0f);
        gfx::text(T(c.name), r.cx(), r.b() - 34.0f, ns);
        TextStyle ks = style(font::FACE_ITALIC, 18.0f, muted, HAlign::Center);
        gfx::text(std::string(1, c.key), r.cx(), r.b() - 11.0f, ks);
        if (it.activated || im::keyPressed(c.key)) result = c.piece;
    }
    im::popId();
    gfx::popAlpha();
    if (result) im::sound(Sound::Confirm);
    return result;
}

void notify(const std::string& message, float seconds) {
    for (auto& t : S.toasts) {
        if (t.text == message) {  // repeated message: extend instead of stacking
            t.age = std::min(t.age, 0.25f);
            t.duration = std::max(seconds, 0.5f);
            return;
        }
    }
    S.toasts.push_back({message, 0.0f, std::max(seconds, 0.5f)});
    if (S.toasts.size() > 4) S.toasts.erase(S.toasts.begin());
}

void drawNotifications() {
    if (S.toasts.empty()) return;
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_OVERLAY);
    vec2 v = view();
    float y = 70.0f;
    TextStyle ts = style(font::FACE_ITALIC, 27.0f, ivory, HAlign::Center);
    for (const Toast& t : S.toasts) {
        float in = m::saturate(t.age / 0.3f);
        float out = m::saturate((t.duration + 0.6f - t.age) / 0.6f);
        float a = ease(in) * ease(out);
        if (a <= 0.001f) continue;
        float tw = gfx::textWidth(t.text, ts);
        float bw = tw + 260.0f, bh = 58.0f;
        float yy = y - (1.0f - ease(in)) * 8.0f;
        Rect band(v.x * 0.5f - bw * 0.5f, yy, bw, bh);
        float edge = 0.28f;
        vec4 d(0.02f, 0.017f, 0.015f, 0.72f * a), z(0.02f, 0.017f, 0.015f, 0.0f);
        gfx::fillH(Rect(band.x, band.y, band.w * edge, bh), z, d);
        gfx::fill(Rect(band.x + band.w * edge, band.y, band.w * (1.0f - 2.0f * edge), bh), d);
        gfx::fillH(Rect(band.r() - band.w * edge, band.y, band.w * edge, bh), d, z);
        gfx::hlineFade(band.x + 40.0f, band.r() - 40.0f, band.y, withAlpha(gold, 0.6f * a), 0.45f);
        gfx::hlineFade(band.x + 40.0f, band.r() - 40.0f, band.b() - 1.0f, withAlpha(gold, 0.6f * a), 0.45f);
        ts.color = withAlpha(ivory, a);
        gfx::text(t.text, band.cx(), band.cy() + 8.0f, ts);
        y += bh + 12.0f;
    }
    gfx::setLayer(prev);
}

MenuAction gameOver(const std::string& result, const std::string& reason, bool playerWon, bool draw, int moveCount) {
    return gameOver(result, reason, playerWon, draw, moveCount, GameOverExtras{});
}

bool gameOverFolded() { return S.goFolded; }

MenuAction gameOver(const std::string& result, const std::string& reason, bool playerWon, bool draw, int moveCount,
                    const GameOverExtras& extras) {
    S.goExtras = extras;
    im::Id id = im::makeId("##gameover");
    im::Anim& a = im::anim(id);
    bool appear = a.firstFrame == im::frame();
    if (appear) {
        S.goFolded = S.forcedFold == 1;
        S.forcedFold = -1;
        im::sound(Sound::Open);
    }
    a.v[5] = appear ? 0.0f : std::min(1.0f, a.v[5] + im::dt() / 0.6f);
    a.v[4] = im::approach(a.v[4], S.goFolded ? 1.0f : 0.0f, 12.0f);
    if (appear) a.v[4] = S.goFolded ? 1.0f : 0.0f;
    float t = ease(a.v[5]);
    float fold = a.v[4];
    im::captureKeyboard();
    vec2 v = view();
    MenuAction act = MenuAction::None;
    im::pushId("gameover");
    std::string res = typographicResult(result);
    if (!appear && im::consumeBack()) {
        S.goFolded = !S.goFolded;
        im::sound(S.goFolded ? Sound::Close : Sound::Open);
    }
    if (fold < 0.999f) {
        float ct = t * (1.0f - fold);
        float w = 700.0f, h = 380.0f + (S.goExtras.detail.empty() ? 0.0f : 38.0f);
        Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + 60.0f + (1.0f - t) * 20.0f + fold * 40.0f, w, h);
        im::captureMouseRect(p);
        gfx::Layer prev = gfx::layer();
        gfx::setLayer(gfx::LAYER_BACK);
        gfx::radial(p.center(), vec2(w * 0.95f, h * 1.3f), vec4(0, 0, 0, 0.55f * ct), 0.0f, 1.0f);
        gfx::setLayer(prev);
        gfx::pushAlpha(ct);
        bool blockCard = fold > 0.5f;
        if (blockCard) im::pushBlock();
        im::panel(p);
        TextStyle rs = style(font::FACE_TITLE, 76.0f, goldBright, HAlign::Center, 0.08f);
        TextStyle glow = rs;
        glow.color = withAlpha(gold, 0.25f);
        glow.softness = 14.0f;
        glow.weight = 3.0f;
        gfx::text(res, p.cx(), p.y + 118.0f, glow);
        gfx::text(res, p.cx(), p.y + 118.0f, rs);
        im::ornamentRule(p.cx(), p.y + 146.0f, 190.0f);
        TextStyle why = style(font::FACE_TITLE, 24.0f, ivory, HAlign::Center, 0.2f);
        why.size = gfx::fitSize(upper(reason), why, w - 60.0f);
        gfx::text(upper(reason), p.cx(), p.y + 196.0f, why);
        std::string line;
        if (draw) line = moveCount > 0 ? i18n::trn("gameover.draw_moves", moveCount) : T("gameover.draw");
        else if (playerWon) line = moveCount > 0 ? i18n::trn("gameover.win_moves", moveCount) : T("gameover.win");
        else line = moveCount > 0 ? i18n::trn("gameover.loss_moves", moveCount) : T("gameover.loss");
        if (!S.goExtras.line.empty()) line = S.goExtras.line;
        TextStyle ls = style(font::FACE_ITALIC, 25.0f, ivoryDim, HAlign::Center);
        ls.size = gfx::fitSize(line, ls, w - 60.0f);
        gfx::text(line, p.cx(), p.y + 238.0f, ls);
        if (!S.goExtras.detail.empty()) detail::gameOverDetail(S.goExtras.detail, p.cx(), p.y + 276.0f);
        float bw = 250.0f, bh = 56.0f, gap = 28.0f;
        float by = p.b() - 44.0f - bh;
        if (im::button(L("common.main_menu"), im::flip(p, Rect(p.cx() - gap * 0.5f - bw, by, bw, bh)), im::ButtonKind::Secondary))
            act = MenuAction::BackToMainMenu;
        std::string primary = S.goExtras.primaryLabel.empty() ? L("gameover.rematch") : S.goExtras.primaryLabel + "##gameover.rematch";
        im::Id rematchId = im::makeId("##gameover.rematch");
        if (im::button(primary, im::flip(p, Rect(p.cx() + gap * 0.5f, by, bw, bh)), im::ButtonKind::Primary, !S.goExtras.primaryDisabled))
            act = MenuAction::Rematch;
        im::setDefaultFocus(S.goExtras.primaryDisabled ? im::makeId("##common.main_menu") : rematchId);
        if (!S.goExtras.reportLabel.empty()) {  // online: report the opponent (start corner, quiet)
            TextStyle rq = style(font::FACE_ITALIC, kSmall, muted);
            float rw = std::max(176.0f, gfx::textWidth(S.goExtras.reportLabel, rq) + 24.0f);
            if (im::button(S.goExtras.reportLabel + "##gameover.report", im::flip(p, Rect(p.x + 14.0f, p.y + 14.0f, rw, 40.0f)),
                           im::ButtonKind::Quiet))
                act = MenuAction::Report;
        }
        TextStyle qs = style(font::FACE_ITALIC, kSmall, muted);
        float vw = std::max(176.0f, gfx::textWidth(T("gameover.view_board"), qs) + 24.0f);
        if (im::button(L("gameover.view_board"), im::flip(p, Rect(p.r() - 14.0f - vw, p.y + 14.0f, vw, 40.0f)), im::ButtonKind::Quiet)) {
            S.goFolded = true;
            im::sound(Sound::Close);
        }
        if (blockCard) im::popBlock();
        gfx::popAlpha();
    }
    if (fold > 0.001f) {
        float bt = t * fold;
        TextStyle bs = style(font::FACE_TITLE, 22.0f, ivory, im::startAlign(), 0.14f);
        std::string summary = res + "  \xC2\xB7  " + upper(reason);
        float sw = gfx::textWidth(summary, bs);
        float w = sw + 300.0f, h = 64.0f;
        Rect bar(v.x * 0.5f - w * 0.5f, v.y - h - 36.0f + (1.0f - fold) * 20.0f, w, h);
        im::captureMouseRect(bar);
        gfx::pushAlpha(bt);
        bool blockBar = fold <= 0.5f;
        if (blockBar) im::pushBlock();
        im::panel(bar);
        gfx::text(summary, im::flipX(bar, bar.x + 40.0f), baselineCentered(bar, bs), bs);
        if (im::button(L("gameover.show"), im::flip(bar, Rect(bar.r() - 150.0f, bar.y + 12.0f, 120.0f, 40.0f)), im::ButtonKind::Quiet)) {
            S.goFolded = false;
            im::sound(Sound::Open);
        }
        if (blockBar) im::popBlock();
        gfx::popAlpha();
    }
    im::popId();
    if (act != MenuAction::None) im::sound(Sound::Click);
    return act;
}

void moveList(const std::vector<std::string>& san, bool visible) {
    S.mlT = im::approach(S.mlT, visible ? 1.0f : 0.0f, 12.0f);
    if (S.mlT < 0.002f) {
        S.mlCount = san.size();
        return;
    }
    float t = ease(S.mlT);
    vec2 v = view();
    float w = 340.0f, h = std::min(760.0f, v.y - 280.0f);
    Rect p(v.x - w - 44.0f + (1.0f - t) * 40.0f, 150.0f, w, h);
    if (visible) im::captureMouseRect(p);
    gfx::pushAlpha(t);
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_MAIN);
    im::panel(p, 0.92f);
    TextStyle hs = style(font::FACE_TITLE, 21.0f, gold, HAlign::Center, 0.24f);
    gfx::text(T("movelist.title"), p.cx(), p.y + 50.0f, hs);
    im::ornamentRule(p.cx(), p.y + 68.0f, 110.0f);
    Rect area(p.x + 20.0f, p.y + 90.0f, p.w - 40.0f, p.h - 110.0f);
    float rowH = 38.0f;
    int rows = int((san.size() + 1) / 2);
    float contentH = float(rows) * rowH;
    float maxScroll = std::max(0.0f, contentH - area.h);
    if (san.size() != S.mlCount) {  // follow the game
        S.mlScrollTarget = maxScroll;
        S.mlCount = san.size();
    }
    if (visible && area.contains(im::mouse()) && im::wheel() != 0.0f) S.mlScrollTarget -= im::wheel() * rowH * 2.0f;
    S.mlScrollTarget = m::clamp(S.mlScrollTarget, 0.0f, maxScroll);
    S.mlScroll = im::approach(S.mlScroll, S.mlScrollTarget, 16.0f);
    gfx::pushClip(area);
    TextStyle ns = style(font::FACE_ITALIC, 22.0f, muted, HAlign::Right);
    TextStyle ms = style(font::FACE_TEXT, 25.0f, ivory);
    for (int r = 0; r < rows; ++r) {
        float y = area.y + float(r) * rowH - S.mlScroll;
        if (y + rowH < area.y || y > area.b()) continue;
        if (r % 2 == 1) gfx::fill(Rect(area.x, y, area.w, rowH), vec4(1, 0.9f, 0.7f, 0.025f));
        float base = y + rowH * 0.5f + 8.0f;
        gfx::text(std::to_string(r + 1) + ".", area.x + 50.0f, base, ns);
        for (int c = 0; c < 2; ++c) {
            size_t i = size_t(r * 2 + c);
            if (i >= san.size()) break;
            bool last = i + 1 == san.size();
            ms.color = last ? goldBright : ivory;
            float x = area.x + 72.0f + float(c) * 118.0f;
            if (last) gfx::diamond(vec2(x - 10.0f, base - 7.0f), 3.0f, withAlpha(gold, 0.9f));
            gfx::text(san[i], x, base, ms);
        }
    }
    if (san.empty()) {
        TextStyle es = style(font::FACE_ITALIC, 22.0f, muted, HAlign::Center);
        gfx::text(T("movelist.empty"), area.cx(), area.y + 40.0f, es);
    }
    gfx::popClip();
    if (maxScroll > 0.0f) {
        gfx::fillV(Rect(area.x, area.y, area.w, 24.0f), vec4(0.04f, 0.035f, 0.03f, 0.9f * m::saturate(S.mlScroll / 24.0f)), vec4(0.04f, 0.035f, 0.03f, 0.0f));
    }
    gfx::setLayer(prev);
    gfx::popAlpha();
}

void loadingScreen(float progress, const std::string& label) {
    uint64_t f = im::frame();
    if (S.loadFrame + 1 < f) S.loadShown = 0.0f;
    S.loadFrame = f;
    progress = m::saturate(progress);
    S.loadShown = std::max(S.loadShown, 0.0f);
    S.loadShown = progress < S.loadShown ? progress : im::approach(S.loadShown, progress, 8.0f);
    im::captureMouseAll();
    im::captureKeyboard();
    vec2 v = view();
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_BACK);
    gfx::fill(Rect(0, 0, v.x, v.y), vec4(0.0f, 0.0f, 0.0f, 1.0f));
    gfx::radial(vec2(v.x * 0.5f, v.y * 0.47f), vec2(v.x * 0.45f, v.y * 0.5f), vec4(0.10f, 0.075f, 0.05f, 0.55f), 0.0f, 1.0f);
    gfx::setLayer(gfx::LAYER_MAIN);
    TextStyle wm = style(font::FACE_TITLE, 66.0f, ivory, HAlign::Center, 0.26f);
    gfx::text("SCACELITH", v.x * 0.5f, v.y * 0.5f - 30.0f, wm);
    float bw = 460.0f;
    float x0 = v.x * 0.5f - bw * 0.5f, y = v.y * 0.5f + 16.0f;
    float px = gfx::px();
    gfx::fill(Rect(x0, gfx::snap(y), bw, px), withAlpha(gold, 0.18f));
    // The bar fills in the reading direction.
    Rect bar(x0, gfx::snap(y), bw, px);
    float fx = x0 + bw * S.loadShown;
    if (im::rtl()) gfx::fillH(im::flip(bar, Rect(x0, bar.y, fx - x0, px)), goldBright, withAlpha(goldDeep, 0.8f));
    else gfx::fillH(Rect(x0, bar.y, fx - x0, px), withAlpha(goldDeep, 0.8f), goldBright);
    gfx::radial(vec2(im::flipX(bar, fx), y), vec2(26.0f, 7.0f), withAlpha(goldBright, 0.5f), 0.0f, 1.0f);
    gfx::diamond(vec2(x0 - 14.0f, y), 3.0f, withAlpha(gold, 0.7f));
    gfx::diamond(vec2(x0 + bw + 14.0f, y), 3.0f, withAlpha(gold, 0.7f));
    TextStyle ls = style(font::FACE_ITALIC, 23.0f, muted, HAlign::Center);
    gfx::text(label, v.x * 0.5f, y + 50.0f, ls);
    gfx::setLayer(prev);
}

}  // namespace ui
