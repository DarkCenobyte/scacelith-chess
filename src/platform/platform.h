// Platform layer: one window with an OpenGL 4.6 core context, input, timing and paths.
// Implementations: platform_win32.cpp (the shipping target) and platform_x11.cpp (Linux, used
// for automated tests and headless screenshots under Xvfb).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

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

// Creates the window and a GL 4.6 core context, and loads GL. False on failure, with the reason in
// the log only: the caller tells the player (main.cpp).
bool init(const WindowDesc& desc);
void shutdown();

// Processes pending OS events and refreshes the input state. Returns false once the user asked
// to close the window.
bool pumpEvents();
void swapBuffers();
void setVsync(bool on);
void setDisplayMode(DisplayMode mode, int width, int height);
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
    // UTF-32 characters typed this frame (keyboard layouts, dead keys and IME results; control
    // characters are left out: use the keys).
    uint32_t text[64] = {};
    int textCount = 0;
    // The pointer is over the client area. False once it has left (mouseX/Y keep its last position
    // inside, maybe at an edge), and while a held button drags it outside (mouseX/Y follow it).
    bool mouseInWindow = true;
};
const Input& input();
void setCursorVisible(bool visible);
// When captured the cursor is hidden and locked in place; only mouseDX/DY change. On Windows, a
// mouse that reports positions (Remote Desktop, a tablet) moves the hidden cursor inside the
// window instead, so mouseX/Y change too; it is put back where the capture began when it ends.
void setMouseCaptured(bool captured);

// ---- Paths & misc ----------------------------------------------------------------------------
// The three folders are net::sys's (src/net/net_sys.h), the one implementation the core library
// uses too.
std::string exeDirectory();      // with trailing separator
std::string userDataDirectory(); // writable (e.g. %APPDATA%/scacelith/), with trailing separator
// The per-user folder of the game's data files, "scacelith", created if missing, with trailing
// separator; each kind of data has its subfolder there ("coach" = the coach's voice model).
// Windows: %APPDATA%\scacelith\ (Roaming). Linux: $XDG_DATA_HOME/scacelith/, by default
// ~/.local/share/scacelith/ (the settings stay in userDataDirectory(), ~/.config/scacelith/). A macOS port
// would use ~/Library/Application Support/scacelith/.
std::string appDataDirectory();
void messageBox(const char* title, const char* text, bool rtl = false);  // rtl: right-to-left text (Arabic)
uint64_t randomSeed();           // non-deterministic seed from the OS
// Text on the system clipboard as UTF-8 ("" when there is none; the X11 layer always returns "").
std::string clipboardText();
#ifdef _WIN32
// OpenClipboard for 'owner' (an HWND), tried up to 5 times 5 ms apart: a clipboard manager or the
// clipboard history may hold the clipboard for a moment. CloseClipboard() after a success.
bool openClipboard(void* owner);
#endif
// The user's interface language as a locale tag ("fr-FR", "zh-TW", "de_DE.UTF-8"), "" when
// unknown. Windows: GetUserDefaultUILanguage; X11: LC_ALL, LC_MESSAGES, LANG.
std::string systemLanguage();
// Command line arguments after the program name, UTF-8 (for modules that read an option before
// a scene exists, e.g. --lang).
std::vector<std::string> commandLine();

// ---- Saved games (the library page) -----------------------------------------------------------
// Shows a folder (or opens a file) in the system's file manager: ShellExecuteW "open" on Windows,
// xdg-open on Linux (started directly, no shell). Returns at once; false when it could not start.
bool openInFileManager(const std::string& path);

}  // namespace plat
