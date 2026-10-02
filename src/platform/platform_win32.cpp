#ifdef _WIN32
#include "platform.h"
#include "../gl/gl46.h"
#include "../gl/gl_context.h"
#include "../core/log.h"

#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cwchar>

// WGL_ARB_create_context / WGL_ARB_pixel_format / WGL_EXT_swap_control tokens.
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define WGL_CONTEXT_DEBUG_BIT_ARB 0x0001
#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#define WGL_DOUBLE_BUFFER_ARB 0x2011
#define WGL_PIXEL_TYPE_ARB 0x2013
#define WGL_TYPE_RGBA_ARB 0x202B
#define WGL_COLOR_BITS_ARB 0x2014
#define WGL_ALPHA_BITS_ARB 0x201B
#define WGL_DEPTH_BITS_ARB 0x2022
#define WGL_STENCIL_BITS_ARB 0x2023
#define WGL_ACCELERATION_ARB 0x2003
#define WGL_FULL_ACCELERATION_ARB 0x2027

typedef HGLRC(WINAPI* PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);
typedef BOOL(WINAPI* PFN_wglChoosePixelFormatARB)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
typedef BOOL(WINAPI* PFN_wglSwapIntervalEXT)(int);

namespace plat {
namespace {
HINSTANCE g_inst;
HWND g_hwnd;
HDC g_hdc;
HGLRC g_glrc;
HMODULE g_opengl32;
int g_width = 0, g_height = 0;
bool g_quit = false;
bool g_focus = true;
bool g_cursorVisible = true;
bool g_captured = false;
DisplayMode g_mode = DisplayMode::Windowed;
int g_windowedW = 1600, g_windowedH = 900;
Input g_input;
wchar_t g_highSurrogate = 0;  // first half of a UTF-16 pair waiting for its second WM_CHAR
LARGE_INTEGER g_freq, g_t0;
PFN_wglSwapIntervalEXT g_swapInterval;
POINT g_captureCenter;
bool g_leaveTracked = false;  // a WM_MOUSELEAVE is asked for (TrackMouseEvent)

int mapVK(WPARAM vk, LPARAM lp) {
    if (vk >= 'A' && vk <= 'Z') return int(vk);
    if (vk >= '0' && vk <= '9') return int(vk);
    bool ext = (lp >> 24) & 1;
    switch (vk) {
        case VK_SPACE: return KEY_SPACE;
        case VK_ESCAPE: return KEY_ESCAPE;
        case VK_RETURN: return KEY_ENTER;
        case VK_TAB: return KEY_TAB;
        case VK_BACK: return KEY_BACKSPACE;
        case VK_DELETE: return KEY_DELETE;
        case VK_LEFT: return KEY_LEFT;
        case VK_RIGHT: return KEY_RIGHT;
        case VK_UP: return KEY_UP;
        case VK_DOWN: return KEY_DOWN;
        case VK_HOME: return KEY_HOME;
        case VK_END: return KEY_END;
        case VK_PRIOR: return KEY_PAGEUP;
        case VK_NEXT: return KEY_PAGEDOWN;
        case VK_SHIFT: return (((lp >> 16) & 0xFF) == 0x36) ? KEY_RSHIFT : KEY_LSHIFT;
        case VK_CONTROL: return ext ? KEY_RCTRL : KEY_LCTRL;
        case VK_MENU: return ext ? KEY_RALT : KEY_LALT;
        default:
            if (vk >= VK_F1 && vk <= VK_F12) return KEY_F1 + int(vk - VK_F1);
            return KEY_UNKNOWN;
    }
}

void setKey(int k, bool down) {
    if (k <= 0 || k >= KEY_COUNT) return;
    if (down) g_input.keyPressed[k] = true;
    else if (g_input.keyDown[k]) g_input.keyReleased[k] = true;
    g_input.keyDown[k] = down;
}
void setButton(int b, bool down) {
    if (down && !g_input.mouseDown[b]) g_input.mousePressed[b] = true;
    if (!down && g_input.mouseDown[b]) g_input.mouseReleased[b] = true;
    g_input.mouseDown[b] = down;
}
// A button went up (wp = WM_xBUTTONUP's key state): the window keeps the pointer (SetCapture)
// while another button is still held.
void releaseCapture(WPARAM wp) {
    if (!(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) ReleaseCapture();
}

void applyCursor() {
    // ShowCursor keeps a counter; drive it to the wanted state.
    bool want = g_cursorVisible && !g_captured;
    CURSORINFO ci{sizeof(ci)};
    GetCursorInfo(&ci);
    bool shown = (ci.flags & CURSOR_SHOWING) != 0;
    if (want && !shown) while (ShowCursor(TRUE) < 0) {}
    if (!want && shown) while (ShowCursor(FALSE) >= 0) {}
}

bool inClientArea(int x, int y) { return x >= 0 && y >= 0 && x < g_width && y < g_height; }

// The pointer stands over the client area, and no other window is in front of it there.
bool pointerOverClient() {
    POINT p;
    if (!GetCursorPos(&p) || WindowFromPoint(p) != g_hwnd || !ScreenToClient(g_hwnd, &p)) return false;
    return inClientArea(p.x, p.y);
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE: g_quit = true; return 0;
        case WM_SIZE:
            g_width = LOWORD(lp);
            g_height = HIWORD(lp);
            return 0;
        case WM_SETFOCUS: g_focus = true; return 0;
        case WM_KILLFOCUS:
            g_focus = false;
            for (int k = 0; k < KEY_COUNT; ++k) if (g_input.keyDown[k]) setKey(k, false);
            for (int b = 0; b < MOUSE_BUTTON_COUNT; ++b) setButton(b, false);
            return 0;
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) { g_quit = true; return 0; }
            setKey(mapVK(wp, lp), true);
            return 0;
        case WM_KEYUP: case WM_SYSKEYUP: setKey(mapVK(wp, lp), false); return 0;
        case WM_CHAR: {
            // UTF-16 code units: typed characters, dead-key compositions and IME results (the
            // default handling of WM_IME_CHAR posts them here). Characters outside the BMP (CJK
            // extensions, emoji) arrive as two messages.
            uint32_t cp = uint32_t(wp);
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                g_highSurrogate = wchar_t(cp);
                return 0;
            }
            if (cp >= 0xDC00 && cp <= 0xDFFF) {
                if (!g_highSurrogate) return 0;
                cp = 0x10000 + ((uint32_t(g_highSurrogate) - 0xD800) << 10) + (cp - 0xDC00);
            }
            g_highSurrogate = 0;
            if (cp >= 32 && cp != 127 && g_input.textCount < int(sizeof(g_input.text) / sizeof(g_input.text[0])))
                g_input.text[g_input.textCount++] = cp;
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = short(LOWORD(lp)), y = short(HIWORD(lp));
            g_input.mouseX = float(x);
            g_input.mouseY = float(y);
            // With a button down the window keeps the pointer (SetCapture): moves go on outside.
            g_input.mouseInWindow = inClientArea(x, y);
            if (!g_leaveTracked) {
                // The last move inside can be anywhere near the edge the pointer leaves by (at the
                // top, in the look-up band): only WM_MOUSELEAVE says it has gone.
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                g_leaveTracked = TrackMouseEvent(&tme) != FALSE;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            // The pointer has left the client area: for the frame, another window or the desktop.
            // One that comes while it is still over the client area changes nothing; the next
            // move asks again.
            g_leaveTracked = false;
            g_input.mouseInWindow = pointerOverClient();
            return 0;
        case WM_LBUTTONDOWN: SetCapture(h); setButton(MOUSE_LEFT, true); return 0;
        case WM_LBUTTONUP: releaseCapture(wp); setButton(MOUSE_LEFT, false); return 0;
        case WM_RBUTTONDOWN: SetCapture(h); setButton(MOUSE_RIGHT, true); return 0;
        case WM_RBUTTONUP: releaseCapture(wp); setButton(MOUSE_RIGHT, false); return 0;
        case WM_MBUTTONDOWN: SetCapture(h); setButton(MOUSE_MIDDLE, true); return 0;
        case WM_MBUTTONUP: releaseCapture(wp); setButton(MOUSE_MIDDLE, false); return 0;
        case WM_MOUSEWHEEL: g_input.wheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA; return 0;
        case WM_INPUT: {
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_input.mouseDX += float(ri.data.mouse.lLastX);
                g_input.mouseDY += float(ri.data.mouse.lLastY);
            }
            break;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(g_cursorVisible && !g_captured ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
                return TRUE;
            }
            break;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // no ALT menu beep
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void* getProc(const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 || p == (void*)0x3 || p == (void*)-1)
        p = (void*)GetProcAddress(g_opengl32, name);
    return p;
}

RECT windowRectFor(DisplayMode mode, int w, int h, DWORD& style) {
    RECT r;
    if (mode == DisplayMode::Borderless) {
        HMONITOR mon = MonitorFromWindow(g_hwnd ? g_hwnd : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfo(mon, &mi);
        style = WS_POPUP | WS_VISIBLE;
        r = mi.rcMonitor;
    } else {
        style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        r = {0, 0, w, h};
        AdjustWindowRect(&r, style, FALSE);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        int x = (sw - ww) / 2, y = (sh - wh) / 2;
        r = {x, y, x + ww, y + wh};
    }
    return r;
}
}  // namespace

bool init(const WindowDesc& desc) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    timeBeginPeriod(1);

    // Per-monitor DPI awareness so the window is not bitmap-scaled.
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL(WINAPI * SetDpiCtx)(HANDLE);
        if (auto fn = (SetDpiCtx)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
            fn((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    }

    g_inst = GetModuleHandleW(nullptr);
    g_opengl32 = LoadLibraryW(L"opengl32.dll");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(g_inst, MAKEINTRESOURCE(1));
    wc.lpszClassName = L"ScacelithWindow";
    RegisterClassExW(&wc);

    // Dummy context to obtain wglCreateContextAttribsARB.
    HWND dummy = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr, g_inst, nullptr);
    HDC ddc = GetDC(dummy);
    PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1};
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    SetPixelFormat(ddc, ChoosePixelFormat(ddc, &pfd), &pfd);
    HGLRC drc = wglCreateContext(ddc);
    wglMakeCurrent(ddc, drc);
    auto createContextAttribs = (PFN_wglCreateContextAttribsARB)(void*)wglGetProcAddress("wglCreateContextAttribsARB");
    auto choosePixelFormat = (PFN_wglChoosePixelFormatARB)(void*)wglGetProcAddress("wglChoosePixelFormatARB");
    g_swapInterval = (PFN_wglSwapIntervalEXT)(void*)wglGetProcAddress("wglSwapIntervalEXT");
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(drc);
    ReleaseDC(dummy, ddc);
    DestroyWindow(dummy);
    if (!createContextAttribs || !choosePixelFormat) {
        messageBox("Scacelith", "This graphics driver does not support modern OpenGL contexts.\nOpenGL 4.6 is required.\n"
                                "Please update your graphics driver.");
        return false;
    }

    g_mode = desc.mode;
    g_windowedW = desc.width;
    g_windowedH = desc.height;
    DWORD style;
    RECT r = windowRectFor(desc.mode, desc.width, desc.height, style);
    if (desc.hidden) style &= ~WS_VISIBLE;
    wchar_t wtitle[256];
    MultiByteToWideChar(CP_UTF8, 0, desc.title, -1, wtitle, 256);
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, wtitle, style, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, g_inst, nullptr);
    g_hdc = GetDC(g_hwnd);

    const int pfAttribs[] = {WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_DOUBLE_BUFFER_ARB, 1,
                             WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB, WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
                             WGL_COLOR_BITS_ARB, 24, WGL_ALPHA_BITS_ARB, 8, WGL_DEPTH_BITS_ARB, 0, WGL_STENCIL_BITS_ARB, 0, 0};
    int format = 0;
    UINT count = 0;
    if (!choosePixelFormat(g_hdc, pfAttribs, nullptr, 1, &format, &count) || count == 0) {
        messageBox("Scacelith", "No suitable pixel format.\nPlease update your graphics driver.");
        return false;
    }
    DescribePixelFormat(g_hdc, format, sizeof(pfd), &pfd);
    SetPixelFormat(g_hdc, format, &pfd);

    const int ctxAttribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 6,
                              WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                              WGL_CONTEXT_FLAGS_ARB, desc.debugContext ? WGL_CONTEXT_DEBUG_BIT_ARB : 0, 0};
    g_glrc = createContextAttribs(g_hdc, nullptr, ctxAttribs);
    if (!g_glrc) {
        messageBox("Scacelith", "Could not create an OpenGL 4.6 core context.\nPlease update your graphics driver.");
        return false;
    }
    wglMakeCurrent(g_hdc, g_glrc);
    const char* missing = nullptr;
    int nMissing = gl46::load(getProc, &missing);
    if (nMissing) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "The OpenGL driver is missing %d required 4.6 functions (first: %s).\n"
                      "Please update your graphics driver.", nMissing, missing);
        messageBox("Scacelith", buf);
        return false;
    }
    gl46::afterContextCreated(desc.debugContext);
    setVsync(desc.vsync);

    RAWINPUTDEVICE rid{0x01, 0x02, 0, g_hwnd};  // generic desktop / mouse
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    RECT cr;
    GetClientRect(g_hwnd, &cr);
    g_width = cr.right - cr.left;
    g_height = cr.bottom - cr.top;
    if (!desc.hidden) {
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
    }
    return true;
}

void shutdown() {
    if (g_glrc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(g_glrc); }
    if (g_hwnd) { ReleaseDC(g_hwnd, g_hdc); DestroyWindow(g_hwnd); }
    g_glrc = nullptr;
    g_hwnd = nullptr;
    timeEndPeriod(1);
}

bool pumpEvents() {
    for (int k = 0; k < KEY_COUNT; ++k) g_input.keyPressed[k] = g_input.keyReleased[k] = false;
    for (int b = 0; b < MOUSE_BUTTON_COUNT; ++b) g_input.mousePressed[b] = g_input.mouseReleased[b] = false;
    g_input.mouseDX = g_input.mouseDY = 0;
    g_input.wheel = 0;
    g_input.textCount = 0;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) g_quit = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_captured && g_focus) {
        SetCursorPos(g_captureCenter.x, g_captureCenter.y);
    }
    return !g_quit;
}

void swapBuffers() { SwapBuffers(g_hdc); }
void setVsync(bool on) { if (g_swapInterval) g_swapInterval(on ? 1 : 0); }

void setDisplayMode(DisplayMode mode, int w, int h) {
    // Unchanged (Options applied for another setting): the window stays where the player moved,
    // resized or maximised it.
    if (mode == g_mode && (mode == DisplayMode::Borderless || (w == g_windowedW && h == g_windowedH))) return;
    g_mode = mode;
    if (mode == DisplayMode::Windowed) { g_windowedW = w; g_windowedH = h; }
    DWORD style;
    RECT r = windowRectFor(mode, g_windowedW, g_windowedH, style);
    SetWindowLongPtrW(g_hwnd, GWL_STYLE, style);
    SetWindowPos(g_hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

int width() { return g_width; }
int height() { return g_height; }
bool hasFocus() { return g_focus; }

double time() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
}
void sleepMs(int ms) { Sleep(DWORD(ms)); }

const Input& input() { return g_input; }
void setCursorVisible(bool v) { g_cursorVisible = v; applyCursor(); }
void setMouseCaptured(bool c) {
    if (c == g_captured) return;
    g_captured = c;
    if (c) {
        GetCursorPos(&g_captureCenter);
        RECT r;
        GetClientRect(g_hwnd, &r);
        POINT tl{r.left, r.top}, br{r.right, r.bottom};
        ClientToScreen(g_hwnd, &tl);
        ClientToScreen(g_hwnd, &br);
        RECT clip{tl.x, tl.y, br.x, br.y};
        ClipCursor(&clip);
    } else {
        ClipCursor(nullptr);
    }
    applyCursor();
}

std::string exeDirectory() {
    wchar_t w[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, w, MAX_PATH);
    char buf[MAX_PATH * 3];
    int len = WideCharToMultiByte(CP_UTF8, 0, w, int(n), buf, int(sizeof(buf)) - 1, nullptr, nullptr);
    buf[len > 0 ? len : 0] = 0;
    std::string s(buf);
    size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? std::string(".\\") : s.substr(0, p + 1);
}

std::string userDataDirectory() {
    wchar_t w[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, w))) {
        std::wstring dir = std::wstring(w) + L"\\scacelith";
        CreateDirectoryW(dir.c_str(), nullptr);
        char buf[MAX_PATH * 3];
        int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
        if (n > 0) return std::string(buf) + "\\";
    }
    return exeDirectory();
}

// The same folder as userDataDirectory() on Windows (Roaming application data).
std::string appDataDirectory() { return userDataDirectory(); }

void messageBox(const char* title, const char* text) {
    wchar_t wt[256], wx[2048];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wt, 256);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wx, 2048);
    MessageBoxW(g_hwnd, wx, wt, MB_OK | MB_ICONERROR);
}

bool openClipboard(void* owner) {
    for (int attempt = 0;; ++attempt) {
        if (OpenClipboard(static_cast<HWND>(owner))) return true;
        if (attempt == 4) return false;
        Sleep(5);
    }
}

std::string clipboardText() {
    std::string out;
    if (!openClipboard(g_hwnd)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            // Up to the first NUL within the block (another program may have left none), and a
            // million characters at most: the text field keeps a few dozen.
            size_t cap = GlobalSize(h) / sizeof(wchar_t);
            int len = int(std::min<size_t>(wcsnlen(w, cap), size_t(1) << 20));
            int n = len > 0 ? WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr) : 0;
            if (n > 0) {
                out.resize(size_t(n));
                WideCharToMultiByte(CP_UTF8, 0, w, len, &out[0], n, nullptr, nullptr);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

std::string systemLanguage() {
    // The display language of Windows (not the regional format), mapped by hand to the tags the
    // game understands (no dependency on LCIDToLocaleName).
    LANGID id = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(id)) {
    case LANG_ENGLISH: return "en";
    case LANG_FRENCH: return "fr";
    case LANG_GERMAN: return "de";
    case LANG_SPANISH: return "es";
    case LANG_UKRAINIAN: return "uk";
    case LANG_ARABIC: return "ar";
    case LANG_RUSSIAN: return "ru";
    case LANG_JAPANESE: return "ja";
    case LANG_CHINESE: {
        int sub = SUBLANGID(id);
        bool traditional = sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU;
        return traditional ? "zh-TW" : "zh-CN";
    }
    default: return "";
    }
}

std::vector<std::string> commandLine() {
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return args;
    for (int i = 1; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string a;
        if (n > 1) {
            a.resize(size_t(n));
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &a[0], n, nullptr, nullptr);
            a.resize(size_t(n - 1));
        }
        args.push_back(a);
    }
    LocalFree(wargv);
    return args;
}

uint64_t randomSeed() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart) ^ (uint64_t(GetCurrentProcessId()) << 32) ^ uint64_t(GetTickCount64() * 2654435761ULL);
}

// ---- Saved games ---------------------------------------------------------------------------------
bool openInFileManager(const std::string& path) {
    if (path.empty()) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    HINSTANCE r = ShellExecuteW(g_hwnd, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) > 32) return true;
    LOGW("could not open %s in the file manager (%d)", path.c_str(), int(reinterpret_cast<INT_PTR>(r)));
    return false;
}

}  // namespace plat
#endif
