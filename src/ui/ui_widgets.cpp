#include "ui_widgets.h"
#include "ui_theme.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include "../platform/platform.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace ui {
namespace im {
namespace {

using namespace theme;
using gfx::HAlign;
using gfx::TextStyle;

struct FocusEntry {
    Id id;
    Rect r;
    uint32_t flags;
};

struct LastItem {
    Id id = 0;
    Rect r;
    bool highlight = false;
    bool hovered = false;
    float highlightTime = 0.0f;
    // Form rows: the label drawn at the start of the row (its advance, the row's height), for the
    // info mark of a following tooltip().
    bool hasLabel = false;
    Rect label;
    bool enabled = true;
};

struct Ctx {
    float dt = 0.0f;
    double time = 0.0;
    uint64_t frame = 1;
    vec2 mouse{-1000, -1000};
    bool mouseKnown = false;
    bool mouseMoved = false;
    bool mouseInWindow = true;
    bool mDown = false, mPressed = false, mReleased = false;
    bool kUp = false, kDown = false, kLeft = false, kRight = false, kActivate = false, kBack = false;
    bool kPgUp = false, kPgDn = false;
    bool activateConsumed = false, backConsumed = false, navConsumed = false;
    bool kbMode = false;
    Id focus = 0, defaultFocus = 0, active = 0;
    Id hoveredNow = 0, hoveredPrev = 0;
    float activeTime = 0.0f;
    std::vector<FocusEntry> prevList, curList;
    std::unordered_map<Id, Anim> anims;
    std::vector<Id> idStack;
    int blockDepth = 0;
    bool capMouseAll = false, capKb = false;
    std::vector<Rect> capRects;
    bool lastMouse = false, lastKb = false;
    std::function<void(Sound)> soundCb;
    double lastSoundTime[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    LastItem last;
    bool mouseOverride = false;
    vec2 mouseOverridePos;
    bool infoMarks = false;  // between beginInfoMarks() and endInfoMarks()
    // Info marks drawn this frame and the previous one, with the id of their row: a click on a
    // mark shows its definition and does not operate the row.
    std::vector<std::pair<Id, Rect>> marks, prevMarks;
    const plat::Input* inputOverride = nullptr;
    float wheel = 0.0f;
    // Text field being edited (textField): it owns the keyboard until the edit ends.
    Id editId = 0;
    int caret = 0;                 // logical character index
    float caretTime = 0.0f;        // blink phase
    std::string editOriginal;      // restored by Esc
    bool editSeen = false;         // the edited field was drawn this frame
};
Ctx c;

const plat::Input& input() { return c.inputOverride ? *c.inputOverride : plat::input(); }

const FocusEntry* findEntry(const std::vector<FocusEntry>& list, Id id) {
    for (auto& e : list)
        if (e.id == id) return &e;
    return nullptr;
}

Id spatialNext(const FocusEntry& cur, int dx, int dy) {
    vec2 d = dy ? vec2(0.0f, float(dy)) : vec2(float(dx), 0.0f);
    bool vertical = dy != 0;
    Id best = 0;
    float bestScore = 1e30f;
    for (auto& e : c.prevList) {
        if (e.id == cur.id) continue;
        vec2 delta = e.r.center() - cur.r.center();
        float along = vertical ? delta.y * d.y : delta.x * d.x;
        if (along <= 1.0f) continue;
        float perp = vertical ? std::fabs(delta.x) : std::fabs(delta.y);
        bool overlap = vertical ? (e.r.x < cur.r.r() && e.r.r() > cur.r.x) : (e.r.y < cur.r.b() && e.r.b() > cur.r.y);
        float score = along + perp * (overlap ? 0.25f : 2.5f);
        if (score < bestScore) { bestScore = score; best = e.id; }
    }
    if (!best && vertical) {  // wrap around vertically
        for (auto& e : c.prevList) {
            if (e.id == cur.id) continue;
            vec2 delta = e.r.center() - cur.r.center();
            float along = delta.y * d.y;
            if (along >= -1.0f) continue;
            float perp = std::fabs(delta.x);
            bool overlap = e.r.x < cur.r.r() && e.r.r() > cur.r.x;
            float score = along + perp * (overlap ? 0.25f : 2.5f);
            if (score < bestScore) { bestScore = score; best = e.id; }
        }
    }
    return best;
}

uint32_t fnv(const void* data, size_t n, uint32_t h) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

Id seed() { return c.idStack.empty() ? 2166136261u : c.idStack.back(); }

}  // namespace

// ---- Frame ----------------------------------------------------------------------------------------
void beginFrame(float dt) {
    const plat::Input& in = input();
    float s = gfx::scale();
    c.wheel = in.wheel;
    c.dt = dt;
    c.time += dt;
    c.frame++;
    vec2 mp = c.mouseOverride ? c.mouseOverridePos : vec2(in.mouseX / s, in.mouseY / s);
    c.mouseMoved = c.mouseKnown && m::length(mp - c.mouse) > 0.3f;
    c.mouseKnown = true;
    c.mouse = mp;
    c.mouseInWindow = in.mouseInWindow || c.mouseOverride;
    c.mDown = in.mouseDown[plat::MOUSE_LEFT];
    c.mPressed = in.mousePressed[plat::MOUSE_LEFT];
    c.mReleased = in.mouseReleased[plat::MOUSE_LEFT];
    if (c.active && c.mDown) c.activeTime += dt;
    c.kUp = in.keyPressed[plat::KEY_UP];
    c.kDown = in.keyPressed[plat::KEY_DOWN];
    c.kLeft = in.keyPressed[plat::KEY_LEFT];
    c.kRight = in.keyPressed[plat::KEY_RIGHT];
    // While a text field is edited Space types a space instead of activating.
    c.kActivate = in.keyPressed[plat::KEY_ENTER] || (in.keyPressed[plat::KEY_SPACE] && !c.editId);
    c.kBack = in.keyPressed[plat::KEY_ESCAPE];
    c.kPgUp = in.keyPressed[plat::KEY_PAGEUP];
    c.kPgDn = in.keyPressed[plat::KEY_PAGEDOWN];
    c.activateConsumed = c.backConsumed = c.navConsumed = false;
    if (c.mouseMoved || c.mPressed) c.kbMode = false;

    int dx = int(c.kRight) - int(c.kLeft), dy = int(c.kDown) - int(c.kUp);
    if ((dx || dy) && !c.prevList.empty()) {
        bool wasHidden = !c.kbMode;
        c.kbMode = true;
        const FocusEntry* cur = findEntry(c.prevList, c.focus);
        if (cur && wasHidden && !(dx && !dy && (cur->flags & ITEM_HORIZONTAL))) {
            // The focus was not visible (mouse user): the first arrow press only reveals it.
            c.navConsumed = true;
            sound(Sound::Hover);
        } else if (!cur) {
            c.focus = c.prevList.front().id;
            c.navConsumed = true;
            sound(Sound::Hover);
        } else if (!(dx && !dy && (cur->flags & ITEM_HORIZONTAL))) {
            Id n = spatialNext(*cur, dx, dy);
            if (n && n != c.focus) {
                c.focus = n;
                sound(Sound::Hover);
            }
            c.navConsumed = true;
        }
    }
    if (c.kActivate) c.kbMode = true;
}

void endFrame() {
    c.prevList.swap(c.curList);
    c.curList.clear();
    c.prevMarks.swap(c.marks);
    c.marks.clear();
    if (c.defaultFocus && !findEntry(c.prevList, c.focus) && findEntry(c.prevList, c.defaultFocus)) c.focus = c.defaultFocus;
    c.defaultFocus = 0;
    if (c.editId && !c.editSeen) c.editId = 0;  // the field left the screen
    c.editSeen = false;
    c.hoveredPrev = c.hoveredNow;
    c.hoveredNow = 0;
    if (!c.mDown) c.active = 0;
    bool overRect = false;
    for (auto& r : c.capRects)
        if (r.contains(c.mouse)) overRect = true;
    c.lastMouse = c.capMouseAll || overRect || c.active != 0;
    c.lastKb = c.capKb;
    c.capMouseAll = c.capKb = false;
    c.capRects.clear();
    c.blockDepth = 0;
    c.idStack.clear();
    if ((c.frame % 240) == 0) {
        for (auto it = c.anims.begin(); it != c.anims.end();) {
            if (it->second.lastFrame + 600 < c.frame) it = c.anims.erase(it);
            else ++it;
        }
    }
}

float dt() { return c.dt; }
double time() { return c.time; }
uint64_t frame() { return c.frame; }
vec2 mouse() { return c.mouse; }
bool keyboardMode() { return c.kbMode; }
bool keyPressed(int key) { return c.blockDepth == 0 && key >= 0 && key < plat::KEY_COUNT && input().keyPressed[key]; }
float wheel() { return c.blockDepth == 0 ? c.wheel : 0.0f; }
void setInputOverride(const plat::Input* in) { c.inputOverride = in; }
bool consumeBack() {
    if (c.blockDepth > 0 || !c.kBack || c.backConsumed) return false;
    c.backConsumed = true;
    return true;
}
bool consumeNavigation(int* dx, int* dy) {
    int x = int(c.kRight) - int(c.kLeft), y = int(c.kDown) - int(c.kUp);
    if (c.blockDepth > 0 || (!x && !y)) return false;
    if (dx) *dx = x;
    if (dy) *dy = y;
    return true;
}

void pushBlock() { c.blockDepth++; }
void popBlock() { if (c.blockDepth > 0) c.blockDepth--; }
bool blocked() { return c.blockDepth > 0; }
void captureMouseAll() { c.capMouseAll = true; }
void captureMouseRect(const Rect& r) { c.capRects.push_back(r); }
void captureKeyboard() { c.capKb = true; }
bool mouseCapturedLastFrame() { return c.lastMouse; }
bool keyboardCapturedLastFrame() { return c.lastKb; }
void setKeyboardMode(bool on) { c.kbMode = on; }
void setMouseOverride(bool on, vec2 pos) {
    c.mouseOverride = on;
    c.mouseOverridePos = pos;
}

// ---- Ids ------------------------------------------------------------------------------------------
Id makeId(const char* s) {
    const char* key = std::strstr(s, "##");
    if (key) s = key + 2;
    return fnv(s, std::char_traits<char>::length(s), seed());
}
Id makeId(const std::string& s) { return makeId(s.c_str()); }
std::string displayText(const std::string& label) {
    size_t p = label.find("##");
    return p == std::string::npos ? label : label.substr(0, p);
}
Id makeId(int i) { return fnv(&i, sizeof(i), seed() ^ 0x9E3779B9u); }
void pushId(const char* s) { c.idStack.push_back(makeId(s)); }
void pushId(int i) { c.idStack.push_back(makeId(i)); }
void popId() { if (!c.idStack.empty()) c.idStack.pop_back(); }

Anim& anim(Id id) {
    Anim& a = c.anims[id];
    if (a.lastFrame + 1 < c.frame) {  // new, or absent for at least one frame: restart
        a = Anim();
        a.firstFrame = c.frame;
    }
    a.lastFrame = c.frame;
    return a;
}
bool appearing(Id id) { return anim(id).firstFrame == c.frame; }
float approach(float cur, float target, float rate) { return cur + (target - cur) * (1.0f - std::exp(-rate * c.dt)); }

void setFocus(Id id) { c.focus = id; }
void setDefaultFocus(Id id) { c.defaultFocus = id; }

void setSoundCallback(std::function<void(Sound)> cb) { c.soundCb = std::move(cb); }
void sound(Sound s) {
    if (!c.soundCb) return;
    int i = int(s) & 7;
    double minGap = s == Sound::Tick ? 0.045 : s == Sound::Hover ? 0.03 : 0.0;
    if (c.lastSoundTime[i] >= 0.0 && c.time - c.lastSoundTime[i] < minGap) return;
    c.lastSoundTime[i] = c.time;
    c.soundCb(s);
}

// ---- Items ----------------------------------------------------------------------------------------
Item item(Id id, const Rect& r, uint32_t flags) {
    Item it;
    it.id = id;
    Anim& a = anim(id);
    bool interactive = c.blockDepth == 0 && !(flags & ITEM_DISABLED);
    bool focusable = (flags & ITEM_FOCUSABLE) != 0 && !(flags & ITEM_MOUSE_ONLY);
    if (interactive) {
        if (focusable) c.curList.push_back({id, r, flags});
        it.hovered = c.mouseInWindow && r.contains(c.mouse) && gfx::clipContains(c.mouse);
        if (it.hovered) {
            c.hoveredNow = id;
            if (focusable && c.mouseMoved && !c.editId) c.focus = id;  // an edit keeps the focus
            if (c.hoveredPrev != id && c.mouseMoved && !(flags & ITEM_SILENT)) sound(Sound::Hover);
            if (c.mPressed) {
                c.active = id;
                c.activeTime = 0.0f;
                it.pressed = true;
                if (focusable) c.focus = id;
            }
        }
        if (c.active == id) {
            it.held = c.mDown;
            it.heldTime = c.activeTime;
            if (c.mReleased && it.hovered) it.clicked = true;
        }
        it.focused = focusable && c.focus == id;
        if (it.focused && (flags & ITEM_HORIZONTAL)) {
            it.left = c.kLeft;
            it.right = c.kRight;
        }
        if (it.focused && c.kActivate && !c.activateConsumed) {
            it.activated = true;
            c.activateConsumed = true;
        }
        for (const auto& mk : c.prevMarks)
            if (it.clicked && mk.first == id && mk.second.contains(c.mouse)) it.clicked = false;
        if (it.clicked) it.activated = true;
    }
    it.highlight = interactive && (it.hovered || (it.focused && c.kbMode));
    a.v[0] = approach(a.v[0], it.highlight ? 1.0f : 0.0f, 16.0f);
    a.v[1] = approach(a.v[1], it.held && it.hovered ? 1.0f : 0.0f, 24.0f);
    a.v[2] = it.highlight ? a.v[2] + c.dt : 0.0f;
    it.hoverT = a.v[0];
    it.pressT = a.v[1];
    c.last.id = id;
    c.last.r = r;
    c.last.highlight = it.highlight;
    c.last.hovered = it.hovered;
    c.last.highlightTime = a.v[2];
    c.last.hasLabel = false;
    c.last.enabled = !(flags & ITEM_DISABLED);
    return it;
}

// ---- Reading direction ----------------------------------------------------------------------------
bool rtl() { return i18n::rtl(); }
HAlign startAlign() { return rtl() ? HAlign::Right : HAlign::Left; }
HAlign endAlign() { return rtl() ? HAlign::Left : HAlign::Right; }
float flipX(const Rect& area, float x) { return rtl() ? area.x + area.r() - x : x; }
Rect flip(const Rect& area, const Rect& r) { return rtl() ? Rect(area.x + area.r() - r.r(), r.y, r.w, r.h) : r; }

// ---- Decorations ----------------------------------------------------------------------------------
void panel(const Rect& r, float al) {
    gfx::shadow(r.offset(0, 14), 4, 70, withAlpha(black, 0.55f * al));
    gfx::fillV(r, withAlpha(panelTop, al), withAlpha(panelBottom, al), 3.0f);
    // Faint warm sheen near the top edge.
    gfx::fillV(Rect(r.x, r.y, r.w, std::min(r.h, 160.0f)), vec4(0.85f, 0.70f, 0.45f, 0.035f * al), vec4(0.85f, 0.70f, 0.45f, 0.0f), 3.0f);
    gfx::stroke(r, withAlpha(gold, 0.50f * al), 0.0f, 3.0f);
    Rect in = r.inset(7.0f);
    gfx::stroke(in, withAlpha(gold, 0.20f * al), 0.0f, 1.0f);
    vec4 dc = withAlpha(gold, 0.70f * al);
    gfx::diamond(vec2(in.x, in.y), 4.0f, dc);
    gfx::diamond(vec2(in.r(), in.y), 4.0f, dc);
    gfx::diamond(vec2(in.x, in.b()), 4.0f, dc);
    gfx::diamond(vec2(in.r(), in.b()), 4.0f, dc);
}

void ornamentRule(float cx, float y, float halfWidth, float al) {
    float px = gfx::px();
    float gap = 14.0f;
    float yy = gfx::snap(y);
    gfx::fillH(Rect(cx - halfWidth, yy, halfWidth - gap, px), withAlpha(gold, 0.0f), withAlpha(gold, 0.75f * al));
    gfx::fillH(Rect(cx + gap, yy, halfWidth - gap, px), withAlpha(gold, 0.75f * al), withAlpha(gold, 0.0f));
    gfx::diamond(vec2(cx, yy + px * 0.5f), 6.5f, withAlpha(gold, 0.85f * al), 1.0f);
    gfx::diamond(vec2(cx, yy + px * 0.5f), 2.6f, withAlpha(goldBright, 0.9f * al));
}

void pageTitle(const std::string& title, float cx, float y) {
    TextStyle st;
    st.face = font::FACE_TITLE;
    st.size = kPageTitle;
    st.color = ivory;
    st.align = HAlign::Center;
    st.tracking = 0.2f;
    float w = gfx::textWidth(title, st);
    gfx::text(title, cx, y, st);
    ornamentRule(cx, y + 24.0f, w * 0.5f + 70.0f);
}

void sectionLabel(const std::string& text, float x, float y, float width) {
    TextStyle st;
    st.face = font::FACE_TITLE;
    st.size = kSection;
    st.color = gold;
    st.tracking = 0.2f;
    st.align = startAlign();
    st.size = gfx::fitSize(text, st, width - 30.0f);
    Rect area(x, y, width, 0.0f);
    float w = gfx::text(text, flipX(area, x), y, st);
    float lx = x + w + 16.0f;
    if (x + width > lx + 10.0f) {
        Rect line = flip(area, Rect(lx, gfx::snap(y - gfx::capHeight(st) * 0.5f), x + width - lx, gfx::px()));
        if (rtl()) gfx::fillH(line, withAlpha(gold, 0.0f), withAlpha(gold, 0.35f));
        else gfx::fillH(line, withAlpha(gold, 0.35f), withAlpha(gold, 0.0f));
    }
}

void rowHighlight(const Rect& r, float t) {
    if (t < 0.002f) return;
    // Brightest at the start of the reading direction, with the gold bar on that side.
    if (rtl()) gfx::fillH(r, withAlpha(gold, 0.015f * t), withAlpha(gold, 0.10f * t), 2.0f);
    else gfx::fillH(r, withAlpha(gold, 0.10f * t), withAlpha(gold, 0.015f * t), 2.0f);
    gfx::fill(flip(r, Rect(r.x, r.y + 6.0f, std::max(2.0f, 2.0f * gfx::px()), r.h - 12.0f)), withAlpha(goldBright, 0.9f * t));
}

// ---- Widgets --------------------------------------------------------------------------------------
namespace {
TextStyle labelStyle(bool enabled) {
    TextStyle st;
    st.face = font::FACE_TEXT;
    st.size = kBody;
    st.color = enabled ? ivory : withAlpha(muted, 0.8f);
    return st;
}
TextStyle valueStyle(bool enabled, float hover) {
    TextStyle st;
    st.face = font::FACE_TEXT;
    st.size = kBody;
    st.color = enabled ? theme::mix(ivoryDim, goldBright, hover) : withAlpha(muted, 0.7f);
    return st;
}
float centerBaseline(const Rect& r, const TextStyle& st) { return r.cy() + gfx::capHeight(st) * 0.5f; }

// The label of a Primary or Secondary button: Cinzel, shrunk down to 62 % to fit kButtonLabelPad
// inside each end of the frame, and cut ("…") beyond that, so that it never crosses the frame.
constexpr float kButtonMinScale = 0.62f, kButtonLabelPad = 14.0f;
TextStyle buttonLabelStyle() {
    TextStyle st;
    st.face = font::FACE_TITLE;
    st.size = kButton;
    st.tracking = kTrackTitle;
    st.align = HAlign::Center;
    return st;
}
std::string elideToFit(const std::string& s, const TextStyle& st, float maxWidth) {
    // Half a unit of slack: a label fitSize() shrank to the exact width measures a hair over it.
    if (gfx::textWidth(s, st) <= maxWidth + 0.5f) return s;
    const char* const ellipsis = "\xE2\x80\xA6";
    std::u32string cps = uni::decode(s);
    size_t lo = 0, hi = cps.size();
    while (lo < hi) {  // the longest start that fits with the ellipsis
        size_t mid = (lo + hi + 1) / 2;
        if (gfx::textWidth(uni::encode(cps.substr(0, mid)) + ellipsis, st) <= maxWidth) lo = mid;
        else hi = mid - 1;
    }
    std::u32string head = cps.substr(0, lo);
    while (!head.empty() && head.back() == U' ') head.pop_back();
    return uni::encode(head) + ellipsis;
}

// Info mark: a circled "i" this far after the label (centre), and the room a label leaves for it.
constexpr float kInfoRadius = 8.5f;
constexpr float kInfoOffset = 13.0f + kInfoRadius;
constexpr float kInfoRoom = kInfoOffset + kInfoRadius + 10.0f;

// Label of a form row on the start side, shrunk (down to 70 %) to leave 'reserved' units free for
// the control at the end of the row (long German and Russian labels), and room for an info mark
// between info mark calls. Recorded for a following tooltip(). Returns its advance width.
float rowLabel(const std::string& label, const Rect& r, float reserved, bool enabled) {
    TextStyle ls = labelStyle(enabled);
    std::string shown = displayText(label);
    if (c.infoMarks) reserved += kInfoRoom;
    ls.size = gfx::fitSize(shown, ls, std::max(40.0f, r.w - 44.0f - reserved));
    ls.align = startAlign();
    float w = gfx::text(shown, flipX(r, r.x + 22.0f), centerBaseline(r, ls), ls);
    c.last.hasLabel = true;
    c.last.label = flip(r, Rect(r.x + 22.0f, r.y, w, r.h));
    return w;
}
}  // namespace

bool menuEntry(const std::string& label, const Rect& r, bool enabled, HAlign align) {
    Item it = item(makeId(label), r, enabled ? ITEM_FOCUSABLE : ITEM_DISABLED);
    const std::string shown = displayText(label);
    if (align == HAlign::Left && rtl()) align = HAlign::Right;
    float t = it.hoverT;
    TextStyle st;
    st.face = font::FACE_TITLE;
    st.size = kMenuEntry;
    st.tracking = kTrackTitle;
    st.align = align;
    st.color = enabled ? theme::mix(ivoryDim, goldBright, t) : withAlpha(faint, 0.75f);
    st.size = gfx::fitSize(shown, st, r.w - (align == HAlign::Center ? 90.0f : 10.0f));
    float w = gfx::textWidth(shown, st);
    float base = centerBaseline(r, st) + it.pressT * 1.5f;
    float x = align == HAlign::Left ? r.x : align == HAlign::Center ? r.cx() : r.r();
    float x0 = align == HAlign::Left ? x : align == HAlign::Center ? x - w * 0.5f : x - w;
    if (t > 0.002f) {
        // Soft glow behind the word and a gold rule drawn out from the start of the line.
        TextStyle glow = st;
        glow.color = withAlpha(gold, 0.22f * t);
        glow.softness = 7.0f;
        glow.weight = 2.0f;
        gfx::text(shown, x, base, glow);
        float ruleY = base + 12.0f;
        float ruleW = w * (0.35f + 0.65f * t);
        if (align == HAlign::Right)
            gfx::fillH(Rect(x0 + w - ruleW, gfx::snap(ruleY), ruleW, gfx::px()), withAlpha(gold, 0.0f), withAlpha(gold, 0.85f * t));
        else
            gfx::fillH(Rect(x0, gfx::snap(ruleY), ruleW, gfx::px()), withAlpha(gold, 0.85f * t), withAlpha(gold, 0.0f));
        float dy = base - gfx::capHeight(st) * 0.5f;
        if (align == HAlign::Center) {
            gfx::diamond(vec2(x0 - 26.0f, dy), 4.5f * t, withAlpha(goldBright, t));
            gfx::diamond(vec2(x0 + w + 26.0f, dy), 4.5f * t, withAlpha(goldBright, t));
        } else if (align == HAlign::Right) {
            gfx::diamond(vec2(x0 + w + 24.0f + 6.0f * (1.0f - t), dy), 4.5f, withAlpha(goldBright, t));
        } else {
            gfx::diamond(vec2(x0 - 24.0f - 6.0f * (1.0f - t), dy), 4.5f, withAlpha(goldBright, t));
        }
    }
    gfx::text(shown, x, base, st);
    if (it.activated) sound(Sound::Click);
    return it.activated;
}

bool button(const std::string& label, const Rect& r, ButtonKind kind, bool enabled, uint32_t extraFlags) {
    uint32_t flags = (extraFlags & ITEM_MOUSE_ONLY) ? 0u : ITEM_FOCUSABLE;
    Item it = item(makeId(label), r, (enabled ? flags : ITEM_DISABLED) | extraFlags);
    std::string shown = displayText(label);
    float t = it.hoverT, p = it.pressT;
    gfx::pushAlpha(enabled ? 1.0f : 0.4f);
    TextStyle st = buttonLabelStyle();
    if (kind == ButtonKind::Primary) {
        gfx::shadow(r.offset(0, 4), 3, 18, withAlpha(black, 0.5f));
        vec4 top = theme::mix(velvet, velvetBright, t * 0.8f);
        vec4 bot = theme::mix(velvetDeep, velvet, t * 0.6f);
        top = theme::mix(top, velvetDeep, p * 0.5f);
        gfx::fillV(r, top, bot, 2.0f);
        gfx::fillV(Rect(r.x, r.y, r.w, r.h * 0.5f), vec4(1, 0.85f, 0.7f, 0.05f + 0.03f * t), vec4(1, 0.85f, 0.7f, 0.0f), 2.0f);
        gfx::stroke(r, withAlpha(gold, 0.55f + 0.4f * t), 0.0f, 2.0f);
        gfx::stroke(r.inset(3.0f), withAlpha(gold, 0.16f + 0.1f * t), 0.0f, 1.0f);
        st.color = theme::mix(ivory, goldBright, t);
    } else if (kind == ButtonKind::Secondary) {
        gfx::fill(r, vec4(0, 0, 0, 0.28f), 2.0f);
        gfx::fillV(r, withAlpha(gold, 0.07f * t), withAlpha(gold, 0.03f * t), 2.0f);
        gfx::stroke(r, withAlpha(gold, 0.32f + 0.5f * t), 0.0f, 2.0f);
        st.color = theme::mix(ivoryDim, goldBright, t);
    } else {
        st.face = font::FACE_ITALIC;
        st.size = kSmall;
        st.tracking = 0.0f;
        st.color = theme::mix(muted, goldBright, t);
        st.size = gfx::fitSize(shown, st, r.w - 12.0f, 0.75f);
        float w = gfx::textWidth(shown, st);
        float base = centerBaseline(r, st);
        gfx::fillH(Rect(r.cx() - w * 0.5f, gfx::snap(base + 5.0f), w, gfx::px()), withAlpha(gold, 0.6f * t), withAlpha(gold, 0.1f * t));
    }
    if (kind != ButtonKind::Quiet) {
        st.size = gfx::fitSize(shown, st, r.w - 2.0f * kButtonLabelPad, kButtonMinScale);
        shown = elideToFit(shown, st, r.w - 2.0f * kButtonLabelPad);
    }
    gfx::text(shown, r.cx(), centerBaseline(r, st) + p, st);
    gfx::popAlpha();
    if (it.activated) sound(Sound::Click);
    return it.activated;
}

bool buttonLabelFits(const std::string& label, float width) {
    TextStyle st = buttonLabelStyle();
    st.size *= kButtonMinScale;
    return gfx::textWidth(displayText(label), st) <= width - 2.0f * kButtonLabelPad;
}

void disabledButton(const std::string& label, const Rect& r, ButtonKind kind, const std::string& why, const Rect& within) {
    button(label, r, kind, false);
    // An item of its own over the disabled one, for the tip only: hovered or focused, it shows a
    // faint frame (the focus stops there) and the reason; activating it does nothing.
    Item it = item(makeId(label + "##why"), r, ITEM_FOCUSABLE | ITEM_SILENT);
    if (it.hoverT > 0.01f) gfx::stroke(r, withAlpha(gold, 0.3f * it.hoverT), 0.0f, 1.5f);
    tooltip(why, within);
}

bool toggleRow(const std::string& label, bool& value, const Rect& r, bool enabled) {
    Item it = item(makeId(label), r, enabled ? ITEM_FOCUSABLE : ITEM_DISABLED);
    Anim& a = anim(it.id);
    bool changed = false;
    if (it.activated) {
        value = !value;
        changed = true;
        sound(Sound::Toggle);
    }
    if (a.firstFrame == frame()) a.v[3] = value ? 1.0f : 0.0f;
    a.v[3] = approach(a.v[3], value ? 1.0f : 0.0f, 18.0f);
    float k = a.v[3];
    rowHighlight(r, it.hoverT);
    TextStyle vs;
    vs.face = font::FACE_ITALIC;
    vs.size = kSmall;
    vs.align = endAlign();
    vs.color = theme::mix(muted, ivoryDim, k);
    const std::string on = i18n::tr("common.on"), off = i18n::tr("common.off");
    float pw = 54.0f, ph = 26.0f;
    rowLabel(label, r, pw + 16.0f + std::max(gfx::textWidth(on, vs), gfx::textWidth(off, vs)) + 24.0f, enabled);
    gfx::pushAlpha(enabled ? 1.0f : 0.4f);
    float pillX = r.r() - 22.0f - pw;
    Rect pill = flip(r, Rect(pillX, r.cy() - ph * 0.5f, pw, ph));
    gfx::fill(pill, theme::mix(vec4(0, 0, 0, 0.45f), vec4(0.42f, 0.32f, 0.16f, 0.95f), k), ph * 0.5f);
    gfx::stroke(pill, withAlpha(gold, 0.35f + 0.35f * std::max(k, it.hoverT)), 0.0f, ph * 0.5f);
    float kx = flipX(r, pillX + ph * 0.5f + k * (pw - ph));
    gfx::shadow(Rect(kx - 9.0f, pill.cy() - 8.0f, 18.0f, 18.0f), 9.0f, 4.0f, withAlpha(black, 0.5f));
    gfx::circle(vec2(kx, pill.cy()), 9.0f, theme::mix(ivoryDim, goldBright, k));
    gfx::text(value ? on : off, flipX(r, pillX - 16.0f), centerBaseline(r, vs), vs);
    gfx::popAlpha();
    return changed;
}

bool sliderRow(const std::string& label, float& value, float lo, float hi, float step,
               const std::function<std::string(float)>& format, const Rect& r, bool enabled) {
    Item it = item(makeId(label), r, enabled ? (ITEM_FOCUSABLE | ITEM_HORIZONTAL) : ITEM_DISABLED);
    Anim& a = anim(it.id);
    float old = value;
    float valueW = 120.0f;
    // Geometry in left-to-right terms, mirrored when drawn in a right-to-left UI (the low end of
    // the track is then on the right).
    float tx0 = r.x + r.w * 0.47f, tx1 = r.r() - 22.0f - valueW - 18.0f;
    Rect track = flip(r, Rect(tx0 - 10.0f, r.y, tx1 - tx0 + 20.0f, r.h));
    if (it.pressed && track.contains(mouse())) a.v[3] = 1.0f;
    if (!it.held) a.v[3] = 0.0f;
    if (a.v[3] > 0.5f && enabled) {
        float t = m::saturate((flipX(r, mouse().x) - tx0) / std::max(1.0f, tx1 - tx0));
        value = lo + t * (hi - lo);
        if (step > 0.0f) value = lo + std::round((value - lo) / step) * step;
    }
    float kstep = step > 0.0f ? step : (hi - lo) / 20.0f;
    float dir = rtl() ? -1.0f : 1.0f;  // the arrow keys move the knob the way they point
    if (it.left) value -= dir * kstep;
    if (it.right) value += dir * kstep;
    value = m::clamp(value, lo, hi);
    bool changed = std::fabs(value - old) > 1e-6f;
    if (changed) sound(Sound::Tick);

    rowHighlight(r, it.hoverT);
    rowLabel(label, r, r.r() - tx0 + 10.0f, enabled);
    gfx::pushAlpha(enabled ? 1.0f : 0.4f);
    float t = hi > lo ? (value - lo) / (hi - lo) : 0.0f;
    float y = r.cy();
    float kxl = tx0 + t * (tx1 - tx0);
    float kx = flipX(r, kxl);
    float px = gfx::px();
    gfx::fill(flip(r, Rect(tx0, gfx::snap(y) - px, tx1 - tx0, 2.0f * px)), withAlpha(gold, 0.22f));
    Rect done = flip(r, Rect(tx0, gfx::snap(y) - px, kxl - tx0, 2.0f * px));
    if (rtl()) gfx::fillH(done, withAlpha(goldBright, 0.95f), withAlpha(goldDeep, 0.9f));
    else gfx::fillH(done, withAlpha(goldDeep, 0.9f), withAlpha(goldBright, 0.95f));
    // Graduation ticks at both ends.
    gfx::vline(flipX(r, tx0), y - 6.0f, y + 6.0f, withAlpha(gold, 0.45f));
    gfx::vline(flipX(r, tx1), y - 6.0f, y + 6.0f, withAlpha(gold, 0.45f));
    float kr = 8.0f + 1.5f * it.hoverT + 1.5f * it.pressT;
    gfx::shadow(Rect(kx - 6, y - 4, 12, 12), 6, 8, withAlpha(black, 0.6f));
    gfx::diamond(vec2(kx, y), kr, theme::mix(gold, goldBright, it.hoverT));
    gfx::diamond(vec2(kx, y), kr * 0.4f, withAlpha(velvetDeep, 0.9f));
    TextStyle vs = valueStyle(enabled, it.hoverT);
    vs.align = endAlign();
    std::string vt = format ? format(value) : std::to_string(value);
    vs.size = gfx::fitSize(vt, vs, valueW + 12.0f);
    gfx::text(vt, flipX(r, r.r() - 22.0f), centerBaseline(r, vs), vs);
    gfx::popAlpha();
    return changed;
}

namespace {
void drawStepGlyph(vec2 c, bool plus, float t, bool enabled) {
    vec4 col = enabled ? theme::mix(gold, goldBright, t) : withAlpha(faint, 0.6f);
    gfx::circle(c, 15.0f, withAlpha(gold, (enabled ? 0.10f : 0.03f) + 0.12f * t));
    gfx::circle(c, 15.0f, withAlpha(col, enabled ? 0.55f + 0.4f * t : 0.3f), 1.0f);
    float px = gfx::px();
    float l = 6.0f;
    gfx::fill(Rect(c.x - l, gfx::snap(c.y) - px, 2 * l, 2 * px), col);
    if (plus) gfx::fill(Rect(gfx::snap(c.x) - px, c.y - l, 2 * px, 2 * l), col);
}
}  // namespace

bool stepperRow(const std::string& label, int& index, int count, const std::function<std::string(int)>& format,
                const Rect& r, bool enabled) {
    Item it = item(makeId(label), r, enabled ? (ITEM_FOCUSABLE | ITEM_HORIZONTAL) : ITEM_DISABLED);
    Anim& a = anim(it.id);
    int old = index;
    float valueW = 170.0f;
    float plusX = r.r() - 22.0f - 15.0f;
    float minusX = plusX - 30.0f - valueW;
    // Right-to-left: minus on the right, plus on the left.
    vec2 cm(flipX(r, minusX), r.cy()), cp(flipX(r, plusX), r.cy());
    Rect rm(cm.x - 22, r.y, 44, r.h), rp(cp.x - 22, r.y, 44, r.h);
    if (it.pressed) {
        a.v[3] = rm.contains(mouse()) ? -1.0f : rp.contains(mouse()) ? 1.0f : 0.0f;
        a.v[4] = 0.0f;
        if (a.v[3] != 0.0f) index += int(a.v[3]);
    }
    if (it.held && a.v[3] != 0.0f) {
        // Hold to repeat: after 0.4 s, accelerating.
        float ht = it.heldTime - 0.4f;
        if (ht > 0.0f) {
            int n = int(ht / 0.075f) + 1;
            while (a.v[4] < float(n)) {
                index += int(a.v[3]);
                a.v[4] += 1.0f;
            }
        }
    }
    if (!it.held) a.v[3] = 0.0f;
    int dir = rtl() ? -1 : 1;
    if (it.left) index -= dir;
    if (it.right) index += dir;
    index = std::clamp(index, 0, std::max(0, count - 1));
    bool changed = index != old;
    if (changed) sound(Sound::Tick);

    rowHighlight(r, it.hoverT);
    rowLabel(label, r, r.r() - minusX + 32.0f, enabled);
    bool hm = it.hovered && rm.contains(mouse()), hp = it.hovered && rp.contains(mouse());
    drawStepGlyph(cm, false, hm ? 1.0f : 0.0f, enabled && index > 0);
    drawStepGlyph(cp, true, hp ? 1.0f : 0.0f, enabled && index < count - 1);
    TextStyle vs = valueStyle(enabled, it.hoverT);
    vs.align = HAlign::Center;
    std::string vt = format ? format(index) : std::to_string(index);
    vs.size = gfx::fitSize(vt, vs, valueW - 6.0f);
    gfx::text(vt, flipX(r, (minusX + plusX) * 0.5f), centerBaseline(r, vs), vs);
    return changed;
}

bool selectorRow(const std::string& label, int& index, const std::vector<std::string>& options, const Rect& r,
                 bool enabled) {
    Item it = item(makeId(label), r, enabled ? (ITEM_FOCUSABLE | ITEM_HORIZONTAL) : ITEM_DISABLED);
    int n = int(options.size());
    int old = index;
    float valueW = 250.0f;
    Rect box = flip(r, Rect(r.r() - 22.0f - valueW, r.y, valueW, r.h));
    Rect rl(box.x - 8, r.y, 40, r.h), rr(box.r() - 32, r.y, 40, r.h);
    // The options run in the reading direction: in a right-to-left UI the left arrow goes forward.
    int step = rtl() ? -1 : 1;
    if (it.clicked) {
        if (rl.contains(mouse())) index -= step;
        else if (rr.contains(mouse())) index += step;
        else if (box.contains(mouse())) index = n > 0 ? (index + 1) % n : 0;
    }
    if (it.left) index -= step;
    if (it.right) index += step;
    index = std::clamp(index, 0, std::max(0, n - 1));
    bool changed = index != old;
    if (changed) sound(Sound::Tick);

    rowHighlight(r, it.hoverT);
    rowLabel(label, r, valueW + 24.0f, enabled);
    TextStyle as;
    as.face = font::FACE_TEXT;
    as.size = 34.0f;
    as.align = HAlign::Center;
    as.dir = 0;
    bool canPrev = enabled && index > 0, canNext = enabled && index < n - 1;
    bool canL = step > 0 ? canPrev : canNext, canR = step > 0 ? canNext : canPrev;
    bool hl = it.hovered && rl.contains(mouse()), hr = it.hovered && rr.contains(mouse());
    float ab = centerBaseline(r, as) - 3.0f;
    as.color = canL ? theme::mix(gold, goldBright, hl ? 1.0f : 0.0f) : withAlpha(faint, 0.5f);
    gfx::text("\xE2\x80\xB9", box.x + 12.0f, ab, as);
    as.color = canR ? theme::mix(gold, goldBright, hr ? 1.0f : 0.0f) : withAlpha(faint, 0.5f);
    gfx::text("\xE2\x80\xBA", box.r() - 12.0f, ab, as);
    TextStyle vs = valueStyle(enabled, it.hoverT);
    vs.align = HAlign::Center;
    if (index >= 0 && index < n) {
        const std::string& v = options[size_t(index)];
        vs.size = gfx::fitSize(v, vs, valueW - 62.0f, 0.6f);
        gfx::text(v, box.cx(), centerBaseline(r, vs), vs);
    }
    return changed;
}

bool editingText() { return c.editId != 0; }

namespace {
bool editField(const std::string& label, std::string& text, const Rect& r, int maxChars, const TextStyle* textStyle,
               bool enabled, uint32_t fieldFlags, const std::string& placeholder);
}  // namespace

bool textField(const std::string& label, std::string& text, const Rect& r, int maxChars, const TextStyle* textStyle,
               bool enabled) {
    return editField(label, text, r, maxChars, textStyle, enabled, 0u, std::string());
}

bool formField(const std::string& label, std::string& text, const Rect& r, int maxChars, uint32_t fieldFlags,
               const std::string& placeholder, bool enabled) {
    return editField(label, text, r, maxChars, nullptr, enabled, fieldFlags, placeholder);
}

namespace {
// The text as drawn: dots for a secret field.
std::string shownText(const std::string& text, bool secret) {
    if (!secret) return text;
    std::string dots;
    for (size_t i = 0, n = uni::decode(text).size(); i < n; ++i) dots += "\xE2\x80\xA2";
    return dots;
}

bool editField(const std::string& label, std::string& text, const Rect& r, int maxChars, const TextStyle* textStyle,
               bool enabled, uint32_t fieldFlags, const std::string& placeholder) {
    const bool secret = (fieldFlags & FIELD_SECRET) != 0, ltr = (fieldFlags & (FIELD_LTR | FIELD_SECRET)) != 0;
    Id id = makeId(label);
    Item it = item(id, r, enabled ? (ITEM_FOCUSABLE | ITEM_HORIZONTAL) : ITEM_DISABLED);
    const plat::Input& in = input();
    const std::string before = text;
    float boxW = std::min(480.0f, r.w * 0.5f);
    Rect box = flip(r, Rect(r.r() - 22.0f - boxW, r.y + 5.0f, boxW, r.h - 10.0f));
    float innerW = box.w - 30.0f;

    // Text layout: the text's own direction decides its alignment in the box (an Arabic name in
    // a French UI is right-aligned); an empty field follows the UI.
    auto styleFor = [&](const std::string& s) {
        TextStyle ts = textStyle ? *textStyle : valueStyle(enabled, 0.0f);
        if (!textStyle) ts.color = enabled ? ivory : withAlpha(muted, 0.8f);
        ts.align = HAlign::Left;
        ts.dir = ltr ? 0 : s.empty() ? (rtl() ? 1 : 0) : gfx::textDirection(s, ts);
        ts.size = gfx::fitSize(s, ts, innerW, 0.55f);
        return ts;
    };
    auto originX = [&](const TextStyle& ts, const std::string& s) {
        return ts.dir == 1 ? box.r() - 15.0f - gfx::textWidth(s, ts) : box.x + 15.0f;
    };

    bool editing = c.editId == id;
    bool started = false;
    if (!editing && enabled && it.activated) {
        c.editId = id;
        c.editOriginal = text;
        c.caret = int(uni::decode(text).size());
        c.caretTime = 0.0f;
        editing = started = true;
        sound(Sound::Toggle);
    }
    if (editing) {
        c.editSeen = true;
        c.caretTime += c.dt;
    }
    if (editing && !started) {
        bool end = false;
        if (!it.focused || (c.mPressed && !r.contains(mouse())) || in.keyPressed[plat::KEY_TAB]) end = true;
        if (it.activated && !it.clicked) end = true;  // Enter
        if (c.kBack && !c.backConsumed && c.blockDepth == 0) {  // Esc: restore, keep the page open
            c.backConsumed = true;
            text = c.editOriginal;
            end = true;
        }
        if (!end) {
            std::u32string u = uni::decode(text);
            int n = int(u.size());
            c.caret = std::clamp(c.caret, 0, n);
            bool modified = false, moved = false;
            // Left/Right move the caret the way they point: backwards in a right-to-left name.
            int visual = styleFor(shownText(text, secret)).dir == 1 ? -1 : 1;
            if (in.keyPressed[plat::KEY_LEFT]) { c.caret -= visual; moved = true; }
            if (in.keyPressed[plat::KEY_RIGHT]) { c.caret += visual; moved = true; }
            if (in.keyPressed[plat::KEY_HOME]) { c.caret = 0; moved = true; }
            if (in.keyPressed[plat::KEY_END]) { c.caret = n; moved = true; }
            c.caret = std::clamp(c.caret, 0, n);
            if (in.keyPressed[plat::KEY_BACKSPACE] && c.caret > 0) {
                u.erase(size_t(c.caret - 1), 1);
                --c.caret;
                modified = true;
            }
            if (in.keyPressed[plat::KEY_DELETE] && c.caret < int(u.size())) {
                u.erase(size_t(c.caret), 1);
                modified = true;
            }
            std::u32string typed;
            bool ctrl = in.keyDown[plat::KEY_LCTRL] || in.keyDown[plat::KEY_RCTRL];
            if (ctrl && in.keyPressed['V']) {
                for (char32_t ch : uni::decode(plat::clipboardText())) {
                    if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
                    if (ch >= 32 && ch != 127 && !(ch >= 0x80 && ch < 0xA0)) typed += ch;
                }
            } else {
                for (int i = 0; i < in.textCount; ++i)
                    if (in.text[i] >= 32 && in.text[i] != 127) typed += char32_t(in.text[i]);
            }
            int room = maxChars - int(u.size());
            if (!typed.empty() && room > 0) {
                if (int(typed.size()) > room) typed.resize(size_t(room));
                u.insert(size_t(c.caret), typed);
                c.caret += int(typed.size());
                modified = true;
            }
            if (modified) text = uni::encode(u);
            if (modified || moved) {
                c.caretTime = 0.0f;
                sound(Sound::Tick);
            }
        }
        if (end) {
            c.editId = 0;
            editing = false;
            sound(Sound::Confirm);
        }
    }
    // A click in the box places the caret (and starts the edit, above).
    const std::string shown = shownText(text, secret);
    if (editing && it.clicked && box.contains(mouse())) {
        TextStyle ts = styleFor(shown);
        c.caret = gfx::caretAt(shown, ts, mouse().x - originX(ts, shown));
        c.caretTime = 0.0f;
    }

    rowHighlight(r, editing ? 1.0f : it.hoverT);
    rowLabel(label, r, boxW + 24.0f, enabled);
    gfx::pushAlpha(enabled ? 1.0f : 0.4f);
    gfx::fill(box, vec4(0, 0, 0, editing ? 0.45f : 0.3f), 2.0f);
    gfx::stroke(box, withAlpha(gold, editing ? 0.85f : 0.25f + 0.4f * it.hoverT), 0.0f, 2.0f);
    TextStyle ts = styleFor(shown);
    float ox = originX(ts, shown);
    float base = centerBaseline(box, ts);
    gfx::pushClip(box.inset(3.0f));
    gfx::text(shown, ox, base, ts);
    if (text.empty() && !editing && !placeholder.empty()) {
        TextStyle ps = labelStyle(enabled);
        ps.face = font::FACE_ITALIC;
        ps.size = kSmall;
        ps.color = withAlpha(muted, enabled ? 0.9f : 0.5f);
        ps.align = rtl() && !ltr ? HAlign::Right : HAlign::Left;
        ps.size = gfx::fitSize(placeholder, ps, innerW, 0.6f);
        gfx::text(placeholder, ps.align == HAlign::Right ? box.r() - 15.0f : box.x + 15.0f, centerBaseline(box, ps), ps);
    }
    if (editing && std::fmod(c.caretTime, 1.0f) < 0.62f) {
        float cx = ox + gfx::caretOffset(shown, ts, c.caret);
        gfx::fill(Rect(gfx::snap(cx) - gfx::px(), box.y + 9.0f, std::max(2.0f * gfx::px(), 2.0f), box.h - 18.0f), goldBright);
    }
    gfx::popClip();
    gfx::popAlpha();
    return text != before;
}
}  // namespace

bool tabBar(const std::vector<std::string>& tabs, int& current, const Rect& r) {
    Item it = item(makeId("##tabs"), r, ITEM_FOCUSABLE | ITEM_HORIZONTAL);
    int n = int(tabs.size());
    int old = current;
    TextStyle st;
    st.face = font::FACE_TITLE;
    st.size = 21.0f;
    st.tracking = 0.2f;
    st.align = HAlign::Center;
    std::vector<float> widths(static_cast<size_t>(n), 0.0f);
    float gap = 54.0f, total = 0.0f;
    for (int i = 0; i < n; ++i) total += (widths[size_t(i)] = gfx::textWidth(tabs[size_t(i)], st));
    total += gap * float(std::max(0, n - 1));
    if (total > r.w - 20.0f && total > 0.0f) {  // long names (German, Russian): tighten, then shrink
        float k = (r.w - 20.0f) / total;
        gap *= std::max(k, 0.5f);
        st.size *= std::min(1.0f, std::max(0.7f, k));
        total = 0.0f;
        for (int i = 0; i < n; ++i) total += (widths[size_t(i)] = gfx::textWidth(tabs[size_t(i)], st));
        total += gap * float(std::max(0, n - 1));
    }
    float x = r.cx() - total * 0.5f;
    int hoverTab = -1;
    std::vector<Rect> rects;
    for (int i = 0; i < n; ++i) {
        // Right-to-left: the first tab is on the right.
        Rect tr = flip(r, Rect(x - gap * 0.5f, r.y, widths[size_t(i)] + gap, r.h));
        rects.push_back(tr);
        if (it.hovered && tr.contains(mouse())) hoverTab = i;
        x += widths[size_t(i)] + gap;
    }
    int step = rtl() ? -1 : 1;
    if (it.clicked && hoverTab >= 0) current = hoverTab;
    if (it.left) current -= step;
    if (it.right) current += step;
    if (!blocked() && c.kPgUp) current--;
    if (!blocked() && c.kPgDn) current++;
    current = std::clamp(current, 0, std::max(0, n - 1));
    bool changed = current != old;
    if (changed) sound(Sound::Tick);
    Anim& a = anim(it.id);
    if (a.firstFrame == frame()) a.v[3] = float(current);
    a.v[3] = approach(a.v[3], float(current), 14.0f);
    gfx::hlineFade(r.x, r.r(), r.b() - 1.0f, withAlpha(gold, 0.35f), 0.2f);
    for (int i = 0; i < n; ++i) {
        const Rect& tr = rects[size_t(i)];
        bool cur = i == current;
        float h = (i == hoverTab) ? 1.0f : 0.0f;
        st.color = cur ? goldBright : theme::mix(muted, ivory, h);
        gfx::text(tabs[size_t(i)], tr.cx(), centerBaseline(r, st), st);
    }
    // Sliding underline under the current tab.
    float fi = m::clamp(a.v[3], 0.0f, float(std::max(0, n - 1)));
    int i0 = int(std::floor(fi));
    int i1 = std::min(n - 1, i0 + 1);
    float f = fi - float(i0);
    if (n > 0) {
        float cx = m::lerp(rects[size_t(i0)].cx(), rects[size_t(i1)].cx(), f);
        float w = m::lerp(widths[size_t(i0)], widths[size_t(i1)], f) + 16.0f;
        gfx::fill(Rect(cx - w * 0.5f, gfx::snap(r.b() - 2.0f), w, 2.0f * gfx::px()), goldBright);
        gfx::diamond(vec2(cx, r.b() - 2.0f + gfx::px()), 3.5f, goldBright);
    }
    if (it.focused && keyboardMode()) gfx::stroke(r.inset(-2.0f), withAlpha(gold, 0.25f), 0.0f, 2.0f);
    return changed;
}

void beginInfoMarks() { c.infoMarks = true; }
void endInfoMarks() { c.infoMarks = false; }

float formLabel(const std::string& label, const Rect& r, float reserved, bool enabled) {
    c.last = LastItem();
    c.last.id = makeId(label);
    c.last.r = r;
    c.last.enabled = enabled;
    float w = rowLabel(label, r, reserved, enabled);
    return 22.0f + w + (c.infoMarks ? kInfoRoom : 16.0f);
}

namespace {
// Floating tip box: at the mouse, or under 'below' from its start side (fromStart) or its end
// side, above it when there is no room below. 'fade' 0..1. 'within' (when not empty, e.g. the
// page's panel): the tip stays inside it too; when it would cross its bottom edge, it goes above
// the item ('below') rather than above the mouse, so that the item stays readable.
void drawTip(const std::string& text, float fade, bool atMouse, const Rect& below, bool fromStart, const Rect& within = Rect()) {
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_TOP);
    TextStyle st;
    st.face = font::FACE_ITALIC;
    st.size = kCaption + 1.0f;
    st.color = withAlpha(ivory, fade);
    st.align = startAlign();
    float maxW = 440.0f;
    int lines = gfx::wrapLineCount(text, maxW, st);
    float lh = st.size * 1.3f;
    // More room at the end of right-to-left lines: Arabic final letters have tails that reach
    // past their advance.
    float w = std::min(maxW, gfx::wrapWidth(text, maxW, st)) + (rtl() ? 46.0f : 36.0f);
    float h = float(lines) * lh + 24.0f;
    vec2 view = gfx::viewSize();
    vec2 p = atMouse ? mouse() + vec2(rtl() ? -18.0f - w : 18.0f, 26.0f)
                     : vec2(fromStart != rtl() ? below.x : below.r() - w, below.b() + 6.0f);
    const bool bounded = within.w > 0.0f && within.h > 0.0f;
    const float left = bounded ? std::max(8.0f, within.x + 8.0f) : 8.0f;
    const float right = std::min(view.x, bounded ? within.r() : view.x) - 8.0f;
    p.x = m::clamp(p.x, left, std::max(left, right - w));
    if (bounded && p.y + h > std::min(view.y, within.b()) - 8.0f) p.y = below.y - h - 10.0f;
    else if (p.y + h > view.y - 8.0f) p.y = (atMouse ? mouse().y : below.y) - h - 10.0f;
    Rect r(p.x, p.y, w, h);
    gfx::shadow(r.offset(0, 6), 3, 24, withAlpha(black, 0.6f * fade));
    gfx::fillV(r, vec4(0.08f, 0.07f, 0.06f, 0.96f * fade), vec4(0.05f, 0.045f, 0.04f, 0.96f * fade), 2.0f);
    gfx::stroke(r, withAlpha(gold, 0.45f * fade), 0.0f, 2.0f);
    gfx::textWrapped(text, rtl() ? r.r() - 18.0f : r.x + 18.0f, r.y + 12.0f + st.size * 0.78f, maxW, st, lh);
    gfx::setLayer(prev);
}

// The circled "i" after the label of the last form row (before it in a right-to-left layout), and
// its tip while the mouse rests on the label or the mark (sooner on the mark), or after a moment
// of keyboard focus. The mark and the label hover also work on a disabled row (they say why it
// is off). One tip at a time: while the keyboard moves the focus (the mouse has not moved since),
// only the focused row's; otherwise only the one under the mouse.
void infoMark(const std::string& text) {
    const LastItem& li = c.last;
    const Rect& lb = li.label;
    vec2 centre(rtl() ? lb.x - kInfoOffset : lb.r() + kInfoOffset, lb.cy());
    Rect mark(centre.x - kInfoRadius - 5.0f, centre.y - kInfoRadius - 6.0f, 2.0f * kInfoRadius + 10.0f, 2.0f * kInfoRadius + 12.0f);
    c.marks.push_back({li.id, mark});
    float x0 = std::min(lb.x, mark.x), x1 = std::max(lb.r(), mark.r());
    Rect zone(x0, lb.y, x1 - x0, lb.h);
    bool canHover = c.blockDepth == 0 && c.mouseInWindow && !c.mDown && gfx::clipContains(c.mouse);
    bool onMark = canHover && mark.contains(c.mouse);
    bool hot = canHover && (onMark || zone.contains(c.mouse));
    bool hoverTip = hot && !c.kbMode;
    Anim& a = anim(li.id);
    a.v[5] = hoverTip ? a.v[5] + c.dt : 0.0f;
    bool keyboard = c.kbMode && c.blockDepth == 0 && li.id == c.focus && li.highlight;

    float h = hot ? 1.0f : keyboard ? 0.6f : 0.0f;
    gfx::pushAlpha(li.enabled ? 1.0f : 0.45f);
    vec4 ink = theme::mix(gold, goldBright, h);
    gfx::circle(centre, kInfoRadius, withAlpha(gold, 0.08f + 0.14f * h));
    gfx::circle(centre, kInfoRadius, withAlpha(ink, 0.55f + 0.4f * h), 1.0f);
    TextStyle is;
    is.face = font::FACE_ITALIC;
    is.size = 18.0f;
    is.color = ink;
    is.dir = 0;
    if (const font::Glyph* g = font::glyph(is.face, 'i')) {  // centred on its ink
        gfx::text("i", centre.x - (g->x0 + g->x1) * 0.5f * is.size, centre.y - (g->y0 + g->y1) * 0.5f * is.size, is);
    }
    gfx::popAlpha();

    float shown = hoverTip ? a.v[5] - (onMark ? 0.1f : 0.35f) : keyboard ? li.highlightTime - 0.7f : -1.0f;
    if (shown < 0.0f) return;
    drawTip(text, m::saturate(shown / 0.15f), hoverTip, lb, true);
}
}  // namespace

void tooltip(const std::string& text) { tooltip(text, Rect()); }

void tooltip(const std::string& text, const Rect& within) {
    if (text.empty()) return;
    if (c.infoMarks) {
        if (c.last.hasLabel) {
            infoMark(text);
        } else if (c.kbMode && c.last.id == c.focus && c.last.highlight && c.last.highlightTime >= 0.7f) {
            drawTip(text, m::saturate((c.last.highlightTime - 0.7f) / 0.15f), false, c.last.r, false, within);
        }
        return;
    }
    // One tip at a time, as for the info marks: the focused item's while the keyboard leads, else
    // the hovered one's.
    bool owner = c.kbMode ? c.last.id == c.focus : c.last.hovered;
    if (!owner || !c.last.highlight || c.last.highlightTime < 0.55f) return;
    drawTip(text, m::saturate((c.last.highlightTime - 0.55f) / 0.15f), !c.kbMode, c.last.r, false, within);
}

int confirmDialog(const char* idStr, const std::string& title, const std::string& message, const std::string& confirmLabel,
                  const std::string& cancelLabel, bool dangerous) {
    Id id = makeId(idStr);
    Anim& a = anim(id);
    bool first = a.firstFrame == frame();
    a.v[0] = first ? 0.0f : approach(a.v[0], 1.0f, 12.0f);
    float t = a.v[0];
    captureMouseAll();
    captureKeyboard();
    // Modal scope: items registered earlier this frame (the page below) leave the navigation.
    c.curList.clear();
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_MODAL);
    vec2 view = gfx::viewSize();
    gfx::fill(Rect(0, 0, view.x, view.y), vec4(0, 0, 0, 0.55f * t));
    gfx::pushAlpha(t);
    TextStyle ms;
    ms.face = font::FACE_TEXT;
    ms.size = kBody;
    ms.color = ivoryDim;
    ms.align = HAlign::Center;
    float w = 760.0f;
    int lines = gfx::wrapLineCount(message, w - 120.0f, ms);
    float h = 250.0f + float(lines) * ms.size * 1.35f;
    Rect r(view.x * 0.5f - w * 0.5f, view.y * 0.5f - h * 0.5f + (1.0f - t) * 12.0f, w, h);
    gfx::fill(r, vec4(0.035f, 0.03f, 0.027f, 1.0f), 3.0f);  // opaque: hide the page below
    panel(r);
    pageTitle(title, r.cx(), r.y + 72.0f);
    gfx::textWrapped(message, r.cx(), r.y + 142.0f, w - 120.0f, ms, ms.size * 1.35f);
    float bw = 250.0f, bh = 56.0f, gap = 30.0f;
    float by = r.b() - 44.0f - bh;
    pushId(idStr);
    // Cancel first in the reading direction.
    Rect rc = flip(r, Rect(r.cx() - gap * 0.5f - bw, by, bw, bh)), ro = flip(r, Rect(r.cx() + gap * 0.5f, by, bw, bh));
    Id cancelId = makeId(cancelLabel);
    if (first) setFocus(cancelId);
    setDefaultFocus(cancelId);
    int result = -1;
    if (button(cancelLabel, rc, ButtonKind::Secondary)) result = 0;
    if (button(confirmLabel, ro, dangerous ? ButtonKind::Primary : ButtonKind::Primary)) result = 1;
    popId();
    if (!first && result < 0 && consumeBack()) {
        result = 0;
        sound(Sound::Back);
    }
    if (result == 1) sound(Sound::Confirm);
    gfx::popAlpha();
    gfx::setLayer(prev);
    return result;
}

}  // namespace im
}  // namespace ui
