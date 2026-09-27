// Platform layer: one window with an OpenGL 4.6 core context, input, timing and paths.
// Implementations: platform_win32.cpp (the shipping target) and platform_x11.cpp (Linux, used
// for automated tests and headless screenshots under Xvfb).
#pragma once
#include <cstdint>
#include <string>

namespace plat {

enum class DisplayMode { Windowed, Borderless };

struct WindowDesc {
    const char* title = "Scacelith";
    int width = 1600, height = 900;  // client size in windowed mode
    DisplayMode mode = DisplayMode::Windowed;
    bool vsync = true;
    bool debugContext = false;       // GL debug output (KHR_debug)
    bool hidden = false;             // create without showing (tests)
};

bool init(const WindowDesc& desc);   // creates window + GL 4.6 core context and loads GL
void shutdown();

// Processes pending OS events and refreshes the input state. Returns false once the user asked
// to close the window.
bool pumpEvents();
void swapBuffers();
void setVsync(bool on);
void setDisplayMode(DisplayMode mode, int width, int height);
void setTitle(const char* title);
int width();   // current framebuffer size in pixels
int height();
bool hasFocus();

// Seconds since init, high resolution, monotonic.
double time();
void sleepMs(int ms);

// ---- Input -----------------------------------------------------------------------------------
enum Key : int {
    KEY_UNKNOWN = 0,
    KEY_A = 'A', KEY_Z = 'Z',  // letters use their uppercase ASCII codes
    KEY_0 = '0', KEY_9 = '9',  // digits use ASCII codes
    KEY_SPACE = ' ',
    KEY_ESCAPE = 256, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_DELETE,
    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_COUNT
};
enum MouseButton { MOUSE_LEFT = 0, MOUSE_RIGHT = 1, MOUSE_MIDDLE = 2, MOUSE_BUTTON_COUNT = 3 };

struct Input {
    bool keyDown[KEY_COUNT] = {};
    bool keyPressed[KEY_COUNT] = {};   // went down this frame (includes auto-repeat)
    bool keyReleased[KEY_COUNT] = {};
    bool mouseDown[MOUSE_BUTTON_COUNT] = {};
    bool mousePressed[MOUSE_BUTTON_COUNT] = {};
    bool mouseReleased[MOUSE_BUTTON_COUNT] = {};
    float mouseX = 0, mouseY = 0;        // pixels, origin top-left
    float mouseDX = 0, mouseDY = 0;      // raw motion this frame (pixels / mickeys)
    float wheel = 0;                     // notches this frame, + = away from user
    uint32_t text[32] = {};              // UTF-32 characters typed this frame
    int textCount = 0;
    bool mouseInWindow = true;
};
const Input& input();
void setCursorVisible(bool visible);
// When captured the cursor is hidden and locked in place; only mouseDX/DY change.
void setMouseCaptured(bool captured);

// ---- Paths & misc ----------------------------------------------------------------------------
std::string exeDirectory();      // with trailing separator
std::string userDataDirectory(); // writable (e.g. %APPDATA%/Scacelith/), with trailing separator
void messageBox(const char* title, const char* text);
uint64_t randomSeed();           // non-deterministic seed from the OS

}  // namespace plat
