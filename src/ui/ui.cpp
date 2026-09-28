// UI core: frame lifecycle, low-level drawing API and the data the game supplies.
#include "ui.h"
#include "ui_draw.h"
#include "ui_font.h"
#include "ui_internal.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../core/log.h"
#include "../i18n/i18n.h"

namespace ui {
namespace detail {
Data& data() {
    static Data d = [] {
        Data x;
        // Defaults for standalone use (viewer); the game replaces them with ai::presets() and
        // chess::timeControlPresets().
        x.difficulties = {
            {"Novice", "Just learned the moves: hangs pieces and misses simple threats.", 800},
            {"Beginner", "Knows the basics, but still blunders material regularly.", 1000},
            {"Casual", "A relaxed evening opponent: sensible moves, frequent mistakes.", 1200},
            {"Club Player", "Solid club player who punishes obvious blunders.", 1500},
            {"Advanced", "Strong club player with good tactical vision.", 1800},
            {"Expert", "Tournament expert: rarely errs, converts advantages.", 2100},
            {"Master", "Master strength: deep calculation and fine positional play.", 2400},
            {"Grandmaster", "Grandmaster level: extremely hard to beat.", 2700},
            {"Stockfish Max", "Stockfish 16 at full strength. Good luck.", 3500},
            {"Custom", "Your own engine settings.", 0},
        };
        x.timeControls = {"Unlimited", "1+0", "3+0", "3+2", "5+0", "5+3", "10+0", "10+5", "15+10", "30+0", "30+20", "90+30"};
        x.resolutions = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
        return x;
    }();
    return d;
}
}  // namespace detail

namespace {
bool g_inited = false;
bool g_fontsOk = false;
bool g_inFrame = false;

int faceOf(FontStyle st) {
    switch (st) {
        case FontStyle::Title: return font::FACE_TITLE;
        case FontStyle::Italic: return font::FACE_ITALIC;
        default: return font::FACE_TEXT;
    }
}
gfx::HAlign halign(Align a) {
    return a == Align::Center ? gfx::HAlign::Center : a == Align::Right ? gfx::HAlign::Right : gfx::HAlign::Left;
}
}  // namespace

bool init() {
    if (g_inited) return true;
    bool ok = gfx::init();
    g_fontsOk = font::init();
    if (!g_fontsOk) LOGE("ui: fonts unavailable, text will not be drawn");
    g_inited = true;
    detail::screensReset();
    return ok && g_fontsOk;
}

void shutdown() {
    if (!g_inited) return;
    font::shutdown();
    gfx::shutdown();
    g_inited = false;
}

void beginFrame(int width, int height, float dt) {
    gfx::beginFrame(width, height);
    im::beginFrame(dt);
    detail::screensBeginFrame(dt);
    g_inFrame = true;
}

void endFrame() {
    if (!g_inFrame) return;
    detail::screensEndFrame();
    im::endFrame();
    if (g_inited) gfx::endFrame();
    g_inFrame = false;
}

bool wantsMouse() { return im::mouseCapturedLastFrame(); }
bool wantsKeyboard() { return im::keyboardCapturedLastFrame(); }

void text(const std::string& s, m::vec2 pos, float sizePx, m::vec4 color, Align align, FontStyle st, float tracking) {
    gfx::TextStyle ts;
    ts.face = faceOf(st);
    ts.size = sizePx;
    ts.color = color;
    ts.align = halign(align);
    ts.tracking = tracking;
    gfx::text(s, pos.x, pos.y + font::metrics(ts.face).ascent * sizePx, ts);
}

m::vec2 measure(const std::string& s, float sizePx, FontStyle st, float tracking) {
    gfx::TextStyle ts;
    ts.face = faceOf(st);
    ts.size = sizePx;
    ts.tracking = tracking;
    const font::Metrics& mt = font::metrics(ts.face);
    return {gfx::textWidth(s, ts), (mt.ascent + mt.descent) * sizePx};
}

void rect(m::vec2 pos, m::vec2 size, m::vec4 color, float radius) { gfx::fill(gfx::Rect(pos.x, pos.y, size.x, size.y), color, radius); }

void fullscreenTint(m::vec4 color) {
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_BACK);
    m::vec2 v = gfx::viewSize();
    gfx::fill(gfx::Rect(0, 0, v.x, v.y), color);
    gfx::setLayer(prev);
}

m::vec2 viewSize() { return gfx::viewSize(); }
float pixelScale() { return gfx::scale(); }

void panel(m::vec2 pos, m::vec2 size) {
    gfx::Rect r(pos.x, pos.y, size.x, size.y);
    im::panel(r);
    im::captureMouseRect(r);
}

bool button(const std::string& label, m::vec2 pos, m::vec2 size, bool primary, bool enabled) {
    gfx::Rect r(pos.x, pos.y, size.x, size.y);
    im::captureMouseRect(r);
    return im::button(label, r, primary ? im::ButtonKind::Primary : im::ButtonKind::Secondary, enabled, im::ITEM_MOUSE_ONLY);
}

void setDifficultyList(const std::vector<DifficultyInfo>& list) {
    if (!list.empty()) detail::data().difficulties = list;
}
void setTimeControlList(const std::vector<std::string>& labels) {
    if (!labels.empty()) detail::data().timeControls = labels;
}
void setResolutionList(const std::vector<m::ivec2>& sizes) {
    if (!sizes.empty()) detail::data().resolutions = sizes;
}
void setVersionString(const std::string& v) { detail::data().version = v; }

namespace {
// "Club Player" -> "preset.club_player" (the section "Opponent presets" of the .lang files).
std::string presetKey(const std::string& englishName) {
    std::string k = "preset.";
    for (char ch : englishName) {
        if (ch >= 'A' && ch <= 'Z') k += char(ch - 'A' + 'a');
        else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) k += ch;
        else if (k.back() != '_') k += '_';
    }
    return k;
}
}  // namespace

std::string presetName(const std::string& englishName) {
    return i18n::trOr(presetKey(englishName) + ".name", englishName);
}
std::string presetDescription(const std::string& englishName, const std::string& englishDescription) {
    return i18n::trOr(presetKey(englishName) + ".desc", englishDescription);
}
std::string timeControlLabel(const std::string& label) {
    return label == "Unlimited" ? std::string(i18n::tr("tc.unlimited")) : label;
}
void setSoundCallback(std::function<void(Sound)> cb) { im::setSoundCallback(std::move(cb)); }

}  // namespace ui
