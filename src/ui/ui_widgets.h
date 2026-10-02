// Immediate-mode widget core: ids, hover/press/focus with mouse and keyboard (spatial arrow-key
// navigation, Enter/Space to activate, Esc = back), small animations, input blocking for modal
// layers, sound hooks, and the Scacelith-styled widgets used by the screens. Reference pixels.
#pragma once
#include "ui.h"
#include "ui_draw.h"
#include "../platform/platform.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ui {
namespace im {

using gfx::Rect;
using m::vec2;
using m::vec4;
typedef uint32_t Id;

// ---- Frame / input ------------------------------------------------------------------------------
void beginFrame(float dt);
void endFrame();
float dt();
double time();
uint64_t frame();
vec2 mouse();
bool keyboardMode();          // last navigation came from the keyboard (show focus highlight)
bool keyPressed(int key);     // plat key pressed this frame and input not blocked
float wheel();                // mouse wheel notches this frame (0 while blocked)
bool consumeBack();           // Esc pressed this frame (returns true once)
bool consumeNavigation(int* dx, int* dy);  // arrow keys this frame, for custom handling

// Input blocking: while the depth is > 0 items draw but do not react (content under a dialog).
void pushBlock();
void popBlock();
bool blocked();
// Marks the frame as using the mouse / keyboard (feeds ui::wantsMouse / wantsKeyboard).
void captureMouseAll();
void captureMouseRect(const Rect& r);
void captureKeyboard();
bool mouseCapturedLastFrame();
bool keyboardCapturedLastFrame();
void setKeyboardMode(bool on);  // debug / viewer: show the focus highlight
void setMouseOverride(bool on, vec2 pos = vec2(0, 0));  // debug / viewer: fake mouse position (reference px)
void setInputOverride(const plat::Input* in);           // debug / viewer: scripted input (nullptr = platform)

// ---- Ids & animation state -----------------------------------------------------------------------
// A label may carry its id after "##": "Apply##apply" is drawn "Apply" and identified by "apply"
// alone, so translated labels keep their id (focus, animations) across a language change.
Id makeId(const char* s);
Id makeId(const std::string& s);
std::string displayText(const std::string& label);  // the part before "##"
Id makeId(int i);
void pushId(const char* s);
void pushId(int i);
void popId();

struct Anim {
    float v[6] = {0, 0, 0, 0, 0, 0};
    uint64_t lastFrame = 0;
    uint64_t firstFrame = 0;
};
Anim& anim(Id id);
bool appearing(Id id);  // true on the first frame an id is used after an absence
float approach(float current, float target, float rate);  // frame-rate independent easing

// ---- Focus ------------------------------------------------------------------------------------------
void setFocus(Id id);
// Focus to give when the current focus is not among this frame's focusable items.
void setDefaultFocus(Id id);

// ---- Items ------------------------------------------------------------------------------------------
enum ItemFlags : uint32_t {
    ITEM_FOCUSABLE = 1u << 0,
    ITEM_DISABLED = 1u << 1,
    ITEM_HORIZONTAL = 1u << 2,  // consumes Left/Right while focused (sliders, steppers, selectors)
    ITEM_SILENT = 1u << 3,      // no hover sound
    ITEM_MOUSE_ONLY = 1u << 4,  // never takes keyboard focus (HUD buttons: Space/Enter belong to the game)
};

// ---- Reading direction ------------------------------------------------------------------------------
// Right-to-left UI language (Arabic): rows are mirrored (label on the right, control on the left),
// text blocks start on the right, horizontal keys follow the visual direction.
bool rtl();
gfx::HAlign startAlign();                       // Left, or Right in a right-to-left UI
gfx::HAlign endAlign();
float flipX(const Rect& area, float x);         // x mirrored inside 'area' in a right-to-left UI
Rect flip(const Rect& area, const Rect& r);     // r mirrored inside 'area' in a right-to-left UI
struct Item {
    Id id = 0;
    bool hovered = false;     // mouse over
    bool held = false;        // mouse button held after pressing on it
    bool pressed = false;     // mouse pressed on it this frame
    bool clicked = false;     // mouse released on it
    bool activated = false;   // clicked or Enter/Space while focused
    bool focused = false;
    bool highlight = false;   // hovered, or focused in keyboard mode
    bool left = false, right = false;  // arrow keys while focused (ITEM_HORIZONTAL)
    float hoverT = 0.0f;      // animated 0..1 highlight
    float pressT = 0.0f;      // animated 0..1 press
    float heldTime = 0.0f;
};
Item item(Id id, const Rect& r, uint32_t flags = ITEM_FOCUSABLE);
void sound(Sound s);
void setSoundCallback(std::function<void(Sound)> cb);

// ---- Styled widgets -----------------------------------------------------------------------------
enum class ButtonKind { Primary, Secondary, Quiet };
// Large Cinzel entry of the title / pause menus (text only, gold rule on hover).
bool menuEntry(const std::string& label, const Rect& r, bool enabled = true, gfx::HAlign align = gfx::HAlign::Left);
// The label of a Primary or Secondary button shrinks to fit its frame (to 62 % of its size at
// most), and is cut ("…") when it still does not fit.
bool button(const std::string& label, const Rect& r, ButtonKind kind = ButtonKind::Secondary, bool enabled = true,
            uint32_t extraFlags = 0);
// Whether a Primary or Secondary button 'width' wide shows this label whole (shrunk or not).
bool buttonLabelFits(const std::string& label, float width);
// A button that is off for a reason: drawn like a disabled button, but the mouse resting on it (or
// the keyboard focus, which stops on it) shows 'why' as its tooltip, kept inside 'within' when
// given (the page's panel: above the button near its bottom edge). Never activates.
void disabledButton(const std::string& label, const Rect& r, ButtonKind kind, const std::string& why, const Rect& within = Rect());
// Form rows: label on the left, control on the right. Return true when the value changed.
bool toggleRow(const std::string& label, bool& value, const Rect& r, bool enabled = true);
bool sliderRow(const std::string& label, float& value, float lo, float hi, float step,
               const std::function<std::string(float)>& format, const Rect& r, bool enabled = true);
bool stepperRow(const std::string& label, int& index, int count, const std::function<std::string(int)>& format,
                const Rect& r, bool enabled = true);
bool selectorRow(const std::string& label, int& index, const std::vector<std::string>& options, const Rect& r,
                 bool enabled = true);
// Single-line text input row (label, then an edit box). Click the box or press Enter on the row to
// type; Enter or Tab keeps the text, Esc restores it, and moving the focus away ends the edit.
// Editing: typed Unicode text (IME results included), caret by mouse, Left/Right (visual
// order), Home/End, Backspace/Delete, Ctrl+V. maxChars counts characters (codepoints).
// textStyle (optional) draws the text, e.g. in a handwriting; the box is sized from the row.
// Returns true on the frame the text changed.
bool textField(const std::string& label, std::string& text, const Rect& r, int maxChars = 24,
               const gfx::TextStyle* textStyle = nullptr, bool enabled = true);
// Form fields of the online pages: FIELD_SECRET shows the text as dots (passwords, codes that
// must stay private), FIELD_LTR keeps it left to right whatever the UI (addresses, e-mails,
// codes); the placeholder shows in an empty field that is not being edited.
enum FieldFlags : uint32_t {
    FIELD_SECRET = 1u << 0,
    FIELD_LTR = 1u << 1,
};
bool formField(const std::string& label, std::string& text, const Rect& r, int maxChars, uint32_t fieldFlags,
               const std::string& placeholder = "", bool enabled = true);
bool editingText();  // a text field has the keyboard (Space and letters type text)
// Tab bar: Left/Right while focused, PageUp/PageDown anywhere. Returns true when changed.
bool tabBar(const std::vector<std::string>& tabs, int& current, const Rect& r);
// Hover/focus tooltip for the previous item (a floating tip; an info mark between
// beginInfoMarks() and endInfoMarks()). One tip shows at a time: the focused item's while the
// keyboard leads (arrows pressed since the mouse last moved), else the hovered item's. With
// 'within' (a page's panel), the floating tip stays inside it as well as inside the window.
void tooltip(const std::string& text);
void tooltip(const std::string& text, const Rect& within);
// Info marks (settings pages: Options, hosting a direct match). Between these calls, a tooltip()
// that follows a form row (or a formLabel()) puts a small circled "i" after the row's label, and
// its text shows while the mouse rests on the label or on the mark, or once the row has had the
// keyboard focus for a moment (then no tip follows the resting mouse); a click on the mark does
// not operate the row. A tooltip() that follows any other item shows for the keyboard focus only.
// Form row labels leave room for the mark.
void beginInfoMarks();
void endInfoMarks();
// Label of a custom form row, drawn at the start of r like the labels of the form rows (shrunk to
// leave 'reserved' units free at the end of the row); a tooltip() right after it gives it an info
// mark. Returns the width it takes from the start side of r, the mark and a gap included.
float formLabel(const std::string& label, const Rect& r, float reserved, bool enabled = true);

// Decorations.
void panel(const Rect& r, float alpha = 1.0f);
void ornamentRule(float cx, float y, float halfWidth, float alpha = 1.0f);
void pageTitle(const std::string& title, float cx, float y);
void sectionLabel(const std::string& text, float x, float y, float width);
void rowHighlight(const Rect& r, float t);

// Modal confirmation. Returns -1 while open, 1 = confirmed, 0 = cancelled (Esc / cancel button).
// 'dangerous' marks destructive actions; it does not change the look (the confirm button is Primary).
int confirmDialog(const char* idStr, const std::string& title, const std::string& message, const std::string& confirmLabel,
                  const std::string& cancelLabel, bool dangerous);

}  // namespace im
}  // namespace ui
