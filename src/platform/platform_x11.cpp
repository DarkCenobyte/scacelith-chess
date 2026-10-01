// Linux/X11 + GLX platform layer. Development and test target only (headless screenshots under
// Xvfb with Mesa llvmpipe: export MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460).
#ifndef _WIN32
#include "platform.h"
#include "../gl/gl_context.h"
#include "../core/log.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/glx.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <thread>
extern char** environ;

typedef GLXContext (*PFN_glXCreateContextAttribsARB)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
typedef void (*PFN_glXSwapIntervalEXT)(Display*, GLXDrawable, int);

namespace plat {
namespace {
Display* g_dpy;
Window g_win;
GLXContext g_ctx;
Atom g_wmDelete;
int g_width, g_height;
bool g_quit, g_focus = true, g_captured, g_cursorVisible = true;
Input g_input;
timespec g_t0;
Cursor g_blankCursor;
float g_lastX, g_lastY;

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
int mapKeysym(KeySym ks) {
    if (ks >= XK_a && ks <= XK_z) return int('A' + (ks - XK_a));
    if (ks >= XK_A && ks <= XK_Z) return int('A' + (ks - XK_A));
    if (ks >= XK_0 && ks <= XK_9) return int('0' + (ks - XK_0));
    if (ks >= XK_F1 && ks <= XK_F12) return KEY_F1 + int(ks - XK_F1);
    switch (ks) {
        case XK_space: return KEY_SPACE;
        case XK_Escape: return KEY_ESCAPE;
        case XK_Return: case XK_KP_Enter: return KEY_ENTER;
        case XK_Tab: return KEY_TAB;
        case XK_BackSpace: return KEY_BACKSPACE;
        case XK_Delete: return KEY_DELETE;
        case XK_Left: return KEY_LEFT;
        case XK_Right: return KEY_RIGHT;
        case XK_Up: return KEY_UP;
        case XK_Down: return KEY_DOWN;
        case XK_Home: return KEY_HOME;
        case XK_End: return KEY_END;
        case XK_Page_Up: return KEY_PAGEUP;
        case XK_Page_Down: return KEY_PAGEDOWN;
        case XK_Shift_L: return KEY_LSHIFT;
        case XK_Shift_R: return KEY_RSHIFT;
        case XK_Control_L: return KEY_LCTRL;
        case XK_Control_R: return KEY_RCTRL;
        case XK_Alt_L: return KEY_LALT;
        case XK_Alt_R: return KEY_RALT;
    }
    return KEY_UNKNOWN;
}
void* getProc(const char* name) { return (void*)glXGetProcAddressARB((const GLubyte*)name); }

}  // namespace

bool init(const WindowDesc& desc) {
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) { LOGE("cannot open X display (is DISPLAY set / Xvfb running?)"); return false; }
    int screen = DefaultScreen(g_dpy);
    static const int fbAttribs[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                                    GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
                                    GLX_DOUBLEBUFFER, True, None};
    int n = 0;
    GLXFBConfig* cfgs = glXChooseFBConfig(g_dpy, screen, fbAttribs, &n);
    if (!cfgs || n == 0) { LOGE("no GLX framebuffer config"); return false; }
    GLXFBConfig cfg = cfgs[0];
    XFree(cfgs);
    XVisualInfo* vi = glXGetVisualFromFBConfig(g_dpy, cfg);
    Window root = RootWindow(g_dpy, screen);
    XSetWindowAttributes swa{};
    swa.colormap = XCreateColormap(g_dpy, root, vi->visual, AllocNone);
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask | FocusChangeMask | EnterWindowMask | LeaveWindowMask;
    g_width = desc.width;
    g_height = desc.height;
    if (desc.mode == DisplayMode::Borderless) {
        g_width = DisplayWidth(g_dpy, screen);
        g_height = DisplayHeight(g_dpy, screen);
    }
    g_win = XCreateWindow(g_dpy, root, 0, 0, g_width, g_height, 0, vi->depth, InputOutput, vi->visual,
                          CWColormap | CWEventMask, &swa);
    XFree(vi);
    XStoreName(g_dpy, g_win, desc.title);
    g_wmDelete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &g_wmDelete, 1);
    if (!desc.hidden) XMapWindow(g_dpy, g_win);

    auto createCtx = (PFN_glXCreateContextAttribsARB)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    int ctxAttribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, 6,
                        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                        GLX_CONTEXT_FLAGS_ARB, desc.debugContext ? GLX_CONTEXT_DEBUG_BIT_ARB : 0, None};
    g_ctx = createCtx ? createCtx(g_dpy, cfg, nullptr, True, ctxAttribs) : nullptr;
    if (!g_ctx) { LOGE("cannot create a GL 4.6 core context (set MESA_GL_VERSION_OVERRIDE=4.6 for llvmpipe)"); return false; }
    glXMakeCurrent(g_dpy, g_win, g_ctx);
    const char* missing = nullptr;
    int nMissing = gl46::load(getProc, &missing);
    if (nMissing) LOGW("%d GL entry points missing (first: %s)", nMissing, missing);
    gl46::afterContextCreated(desc.debugContext);
    setVsync(desc.vsync);

    char data[1] = {0};
    Pixmap blank = XCreateBitmapFromData(g_dpy, g_win, data, 1, 1);
    XColor dummy{};
    g_blankCursor = XCreatePixmapCursor(g_dpy, blank, blank, &dummy, &dummy, 0, 0);
    XFreePixmap(g_dpy, blank);
    XFlush(g_dpy);
    return true;
}

void shutdown() {
    if (!g_dpy) return;
    glXMakeCurrent(g_dpy, None, nullptr);
    if (g_ctx) glXDestroyContext(g_dpy, g_ctx);
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);
    g_dpy = nullptr;
}

bool pumpEvents() {
    for (int k = 0; k < KEY_COUNT; ++k) g_input.keyPressed[k] = g_input.keyReleased[k] = false;
    for (int b = 0; b < MOUSE_BUTTON_COUNT; ++b) g_input.mousePressed[b] = g_input.mouseReleased[b] = false;
    g_input.mouseDX = g_input.mouseDY = 0;
    g_input.wheel = 0;
    g_input.textCount = 0;
    while (XPending(g_dpy)) {
        XEvent e;
        XNextEvent(g_dpy, &e);
        switch (e.type) {
            case ClientMessage:
                if ((Atom)e.xclient.data.l[0] == g_wmDelete) g_quit = true;
                break;
            case ConfigureNotify:
                g_width = e.xconfigure.width;
                g_height = e.xconfigure.height;
                break;
            case FocusIn: g_focus = true; break;
            case FocusOut: g_focus = false; break;
            case KeyPress: case KeyRelease: {
                bool down = e.type == KeyPress;
                if (!down && XEventsQueued(g_dpy, QueuedAfterReading)) {  // swallow auto-repeat release
                    XEvent nx;
                    XPeekEvent(g_dpy, &nx);
                    if (nx.type == KeyPress && nx.xkey.time == e.xkey.time && nx.xkey.keycode == e.xkey.keycode) break;
                }
                char txt[8] = {};
                KeySym ks = 0;
                int len = XLookupString(&e.xkey, txt, sizeof(txt), &ks, nullptr);
                setKey(mapKeysym(ks), down);
                // Basic text input (no input method): Latin-1 keysyms are their codepoint and
                // 0x01xxxxxx keysyms carry a Unicode codepoint (other layouts, xdotool type).
                uint32_t cp = 0;
                if ((ks >= 0x20 && ks <= 0x7E) || (ks >= 0xA0 && ks <= 0xFF)) cp = uint32_t(ks);
                else if ((ks & 0xFF000000UL) == 0x01000000UL) cp = uint32_t(ks & 0x00FFFFFFUL);
                if (len == 1 && (unsigned char)txt[0] >= 32 && (unsigned char)txt[0] < 127) cp = (unsigned char)txt[0];
                if (e.xkey.state & (ControlMask | Mod1Mask)) cp = 0;  // shortcuts (Ctrl+V) type nothing
                if (down && cp >= 32 && cp != 127 && g_input.textCount < int(sizeof(g_input.text) / sizeof(g_input.text[0])))
                    g_input.text[g_input.textCount++] = cp;
                break;
            }
            case MotionNotify: {
                float x = float(e.xmotion.x), y = float(e.xmotion.y);
                if (g_captured) {
                    float cx = float(g_width / 2), cy = float(g_height / 2);
                    if (x != cx || y != cy) { g_input.mouseDX += x - cx; g_input.mouseDY += y - cy; }
                } else {
                    g_input.mouseDX += x - g_lastX;
                    g_input.mouseDY += y - g_lastY;
                    g_input.mouseX = x;
                    g_input.mouseY = y;
                }
                g_lastX = x;
                g_lastY = y;
                break;
            }
            case ButtonPress: case ButtonRelease: {
                bool down = e.type == ButtonPress;
                switch (e.xbutton.button) {
                    case 1: setButton(MOUSE_LEFT, down); break;
                    case 2: setButton(MOUSE_MIDDLE, down); break;
                    case 3: setButton(MOUSE_RIGHT, down); break;
                    case 4: if (down) g_input.wheel += 1; break;
                    case 5: if (down) g_input.wheel -= 1; break;
                }
                break;
            }
            case EnterNotify: g_input.mouseInWindow = true; break;
            case LeaveNotify: g_input.mouseInWindow = false; break;
        }
    }
    if (g_captured && g_focus) XWarpPointer(g_dpy, None, g_win, 0, 0, 0, 0, g_width / 2, g_height / 2);
    return !g_quit;
}

void swapBuffers() { glXSwapBuffers(g_dpy, g_win); }
void setVsync(bool on) {
    auto fn = (PFN_glXSwapIntervalEXT)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT");
    if (fn) fn(g_dpy, g_win, on ? 1 : 0);
}
void setDisplayMode(DisplayMode, int w, int h) {
    XResizeWindow(g_dpy, g_win, w, h);
}
void setTitle(const char* t) { XStoreName(g_dpy, g_win, t); }
int width() { return g_width; }
int height() { return g_height; }
bool hasFocus() { return g_focus; }

double time() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec - g_t0.tv_sec) + double(t.tv_nsec - g_t0.tv_nsec) * 1e-9;
}
void sleepMs(int ms) { usleep(useconds_t(ms) * 1000); }

const Input& input() { return g_input; }
void setCursorVisible(bool v) {
    g_cursorVisible = v;
    if (v && !g_captured) XUndefineCursor(g_dpy, g_win);
    else XDefineCursor(g_dpy, g_win, g_blankCursor);
}
void setMouseCaptured(bool c) {
    g_captured = c;
    setCursorVisible(g_cursorVisible);
}

std::string exeDirectory() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "./";
    buf[n] = 0;
    std::string s(buf);
    return s.substr(0, s.find_last_of('/') + 1);
}
std::string userDataDirectory() {
    const char* home = getenv("HOME");
    if (!home) return exeDirectory();
    std::string d = std::string(home) + "/.config/scacelith/";
    mkdir((std::string(home) + "/.config").c_str(), 0755);
    mkdir(d.c_str(), 0755);
    return d;
}
void messageBox(const char* title, const char* text) { LOGE("%s: %s", title, text); }
uint64_t randomSeed() {
    timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return uint64_t(t.tv_nsec) * 2654435761ULL ^ uint64_t(t.tv_sec) ^ (uint64_t(getpid()) << 32);
}
std::string clipboardText() { return ""; }  // the X11 layer serves tests and screenshots only
std::string systemLanguage() {
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* v = getenv(var);
        if (v && *v) return std::strcmp(v, "C") == 0 || std::strcmp(v, "POSIX") == 0 ? std::string() : std::string(v);
    }
    return "";
}
std::vector<std::string> commandLine() {
    std::vector<std::string> args;
    FILE* f = std::fopen("/proc/self/cmdline", "rb");
    if (!f) return args;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    std::fclose(f);
    size_t p = all.find('\0');  // skip the program name
    while (p != std::string::npos && p + 1 < all.size()) {
        size_t q = all.find('\0', p + 1);
        args.push_back(all.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1));
        p = q;
    }
    return args;
}

// ---- Saved games ---------------------------------------------------------------------------------
bool openInFileManager(const std::string& path) {
    if (path.empty()) return false;
    // A relative path starting with '-' would read as an option.
    const std::string arg = path[0] == '-' ? "./" + path : path;
    pid_t pid;
    char* argv[] = {const_cast<char*>("xdg-open"), const_cast<char*>(arg.c_str()), nullptr};
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) != 0) {
        LOGW("could not start xdg-open for %s", path.c_str());
        return false;
    }
    // xdg-open may wait for the file manager: reaped on a thread of its own, so the menu never waits
    // and no zombie stays behind.
    std::thread([pid]() {
        int status = 0;
        waitpid(pid, &status, 0);
    }).detach();
    return true;
}
}  // namespace plat
#endif
