// The UI of the ui_*_tests.cpp files driven frame by frame, without a scene: the "ui" viewer scene
// of a shared headless App (headless::shared, uitest::app()) gives the null OpenGL and ui::init()
// (the font atlas), then a Driver draws the test's own frames between ui::beginFrame and
// ui::endFrame with scripted input (ui::im::setInputOverride), on a 1920 x 1080 canvas (one
// physical pixel per reference pixel):
//
//   uitest::Driver d;
//   ui::MenuAction last = ui::MenuAction::None;
//   d.screen = [&] { ui::MenuAction a = ui::mainMenu(setup, watch, coach, library); if (a != ui::MenuAction::None) last = a; };
//   d.frames(3);
//   REQUIRE(d.click("New Game"));           // a widget of the last frame, found by its label
//   d.press(plat::KEY_ESCAPE);
//
// The App's own frames are not run while a Driver draws: the viewer's page stays as it was.
// Every Driver starts the UI from the title page state (ui::detail::screensReset) after a frame
// without UI (the widgets appear again), and gives the platform's input back when it goes.
#pragma once
#include "headless.h"
#include "null_gl.h"
#include "platform/platform.h"
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui/ui_widgets.h"
#include <functional>
#include <string>
#include <vector>

namespace uitest {

// The App every ui_*_tests.cpp file shares: the "ui" viewer scene on its title page (one start
// for all the files when they run one after the other).
inline headless::App* app() { return headless::shared({"--scene", "ui"}); }

class Driver {
public:
    plat::Input in;                    // the input of the next frame (per-frame fields cleared after it)
    std::function<void()> screen;      // what each frame draws
    float dt = 1.0f / 60.0f;
    int width = 1920, height = 1080;

    Driver() {
        ui::im::setInputOverride(&in);
        in.mouseX = in.mouseY = 1.0f;  // a corner nothing is drawn on
        frame();                       // nothing drawn: every widget appears again afterwards
        ui::detail::screensReset();
        ui::im::setKeyboardMode(false);
    }
    ~Driver() {
        screen = nullptr;
        frame();
        ui::im::setInputOverride(nullptr);
    }
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    // One frame of 'screen' (nothing when it is empty) with the input set so far.
    void frame() {
        ui::beginFrame(width, height, dt);
        if (screen) screen();
        ui::endFrame();
        for (int k = 0; k < plat::KEY_COUNT; ++k) in.keyPressed[k] = in.keyReleased[k] = false;
        for (int b = 0; b < plat::MOUSE_BUTTON_COUNT; ++b) in.mousePressed[b] = in.mouseReleased[b] = false;
        in.wheel = 0.0f;
        in.textCount = 0;
        in.mouseDX = in.mouseDY = 0.0f;
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) frame();
    }
    // Frames for 'seconds' of UI time.
    void wait(float seconds) { frames(int(seconds / dt + 0.5f)); }
    // The draw calls one frame makes (null GL: one per layer and clip rectangle used).
    uint64_t drawsOfFrame() {
        const uint64_t before = nullgl::stats().draws;
        frame();
        return nullgl::stats().draws - before;
    }

    // ---- Mouse (reference pixels) ------------------------------------------------------------
    void move(float x, float y) {
        in.mouseDX = x - in.mouseX;
        in.mouseDY = y - in.mouseY;
        in.mouseX = x;
        in.mouseY = y;
        in.mouseInWindow = true;
        frame();
    }
    // Move there, press, release: three frames (the release frame activates).
    void clickAt(float x, float y) {
        move(x, y);
        in.mouseDown[plat::MOUSE_LEFT] = true;
        in.mousePressed[plat::MOUSE_LEFT] = true;
        frame();
        in.mouseDown[plat::MOUSE_LEFT] = false;
        in.mouseReleased[plat::MOUSE_LEFT] = true;
        frame();
    }
    void pressAt(float x, float y) {   // move, press and keep it held
        move(x, y);
        in.mouseDown[plat::MOUSE_LEFT] = true;
        in.mousePressed[plat::MOUSE_LEFT] = true;
        frame();
    }
    void release() {
        in.mouseDown[plat::MOUSE_LEFT] = false;
        in.mouseReleased[plat::MOUSE_LEFT] = true;
        frame();
    }
    void wheel(float notches) {
        in.wheel = notches;
        frame();
    }

    // ---- Keyboard ----------------------------------------------------------------------------
    // One frame with the key pressed (and held, with the modifiers given), then released.
    void press(int key, bool shift = false, bool ctrl = false) {
        in.keyDown[plat::KEY_LSHIFT] = shift;
        in.keyDown[plat::KEY_LCTRL] = ctrl;
        in.keyDown[key] = true;
        in.keyPressed[key] = true;
        frame();
        in.keyDown[key] = false;
        in.keyReleased[key] = true;
        in.keyDown[plat::KEY_LSHIFT] = in.keyDown[plat::KEY_LCTRL] = false;
    }
    // The text typed in one frame (UTF-8 in, UTF-32 delivered as the platform layers do).
    void type(const std::string& utf8) {
        in.textCount = 0;
        for (size_t i = 0; i < utf8.size() && in.textCount < 64;) {
            const unsigned char c = static_cast<unsigned char>(utf8[i]);
            const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
            for (int k = 1; k < n && i + size_t(k) < utf8.size(); ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + size_t(k)]) & 0x3Fu);
            in.text[in.textCount++] = cp;
            i += size_t(n);
        }
        frame();
    }

    // ---- The widgets of the last frame -------------------------------------------------------
    const std::vector<ui::im::TracedItem>& items() const { return ui::im::tracedItems(); }
    const ui::im::TracedItem* find(const std::string& label) const {
        for (const auto& it : items())
            if (it.label == label) return &it;
        return nullptr;
    }
    bool has(const std::string& label) const { return find(label) != nullptr; }
    int count(const std::string& label) const {
        int n = 0;
        for (const auto& it : items())
            if (it.label == label) ++n;
        return n;
    }
    // Clicks the centre of the first widget of the last frame showing 'label', then runs one more
    // frame (the page the click opened is drawn). False when there is none.
    bool click(const std::string& label) {
        const ui::im::TracedItem* it = find(label);
        if (!it) return false;
        const ui::gfx::Rect r = it->r;   // a copy: the next frame replaces the list
        clickAt(r.cx(), r.cy());
        frame();
        return true;
    }
    // Moves the mouse over the first widget showing 'label' (false when there is none).
    bool hover(const std::string& label) {
        const ui::im::TracedItem* it = find(label);
        if (!it) return false;
        const ui::gfx::Rect r = it->r;
        move(r.cx(), r.cy());
        return true;
    }
    // Frames until a widget shows 'label' (false after maxFrames).
    bool waitFor(const std::string& label, int maxFrames = 120) {
        for (int i = 0; i < maxFrames; ++i) {
            if (has(label)) return true;
            frame();
        }
        return has(label);
    }
    std::vector<std::string> labels() const {
        std::vector<std::string> out;
        for (const auto& it : items())
            if (!it.label.empty()) out.push_back(it.label);
        return out;
    }
};

// Records the UI sounds while it lives (ui::setSoundCallback); none are played afterwards.
struct Sounds {
    std::vector<ui::Sound> played;
    Sounds() {
        ui::setSoundCallback([this](ui::Sound s) { played.push_back(s); });
    }
    ~Sounds() { ui::setSoundCallback(nullptr); }
    int count(ui::Sound s) const {
        int n = 0;
        for (ui::Sound p : played)
            if (p == s) ++n;
        return n;
    }
};

}  // namespace uitest
