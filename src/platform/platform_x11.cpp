// Linux/X11 + GLX platform layer: the Linux desktop game (XWayland on a Wayland desktop), and
// headless screenshots under Xvfb with Mesa llvmpipe (export MESA_GL_VERSION_OVERRIDE=4.6
// MESA_GLSL_VERSION_OVERRIDE=460). The clipboard, text input, keys and fullscreen state are in
// x11_desktop.cpp.
#ifndef _WIN32
#include "platform.h"
#include "x11_desktop.h"
#include "../gl/gl_context.h"
#include "../core/embedded.h"
#include "../core/image.h"
#include "../core/log.h"
#include "../net/net_sys.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>

typedef GLXContext (*PFN_glXCreateContextAttribsARB)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
typedef void (*PFN_glXSwapIntervalEXT)(Display*, GLXDrawable, int);

namespace plat {
namespace {
Display* g_dpy;
Window g_win;
GLXContext g_ctx;
Atom g_wmProtocols, g_wmDelete;
int g_width, g_height;
int g_windowedW, g_windowedH;  // the windowed size last asked for
bool g_quit, g_focus = true, g_captured, g_cursorVisible = true;
Input g_input;
timespec g_t0;
Cursor g_blankCursor;
float g_lastX, g_lastY;
Time g_lastTime;  // of the last input event: what the clipboard requests name (ICCCM)
x11::Clipboard g_clipboard;
x11::Keyboard g_keyboard;
x11::Fullscreen g_fullscreen;

void noteTime(const XEvent& e) {
    switch (e.type) {
        case KeyPress: case KeyRelease: g_lastTime = e.xkey.time; break;
        case ButtonPress: case ButtonRelease: g_lastTime = e.xbutton.time; break;
        case MotionNotify: g_lastTime = e.xmotion.time; break;
        case EnterNotify: case LeaveNotify: g_lastTime = e.xcrossing.time; break;
        case PropertyNotify: g_lastTime = e.xproperty.time; break;
        default: break;
    }
}
void* getProc(const char* name) { return (void*)glXGetProcAddressARB((const GLubyte*)name); }

// What desktops show of the window before it is mapped: WM_CLASS "scacelith" / "Scacelith" (the
// launcher res/linux/scacelith.desktop has StartupWMClass=Scacelith), WM_CLIENT_MACHINE (which
// _NET_WM_PID requires), the title in UTF-8 and the icon (res/icons/scacelith.ico, embedded).
void setIdentity(const char* title) {
    XClassHint hint{const_cast<char*>("scacelith"), const_cast<char*>("Scacelith")};
    XSetWMProperties(g_dpy, g_win, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &hint);
    const Atom utf8 = XInternAtom(g_dpy, "UTF8_STRING", False);
    XChangeProperty(g_dpy, g_win, XInternAtom(g_dpy, "_NET_WM_NAME", False), utf8, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(title), int(std::strlen(title)));
    const long pid = long(getpid());
    XChangeProperty(g_dpy, g_win, XInternAtom(g_dpy, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&pid), 1);
    if (const embedded::File* f = embedded::find("res/icons/scacelith.ico")) {
        const std::vector<uint32_t> px = image::icoImages(f->data, f->size);
        // Format 32 is an array of long, 64 bits on LP64 (143 KB sent: under the 256 KB request limit).
        const std::vector<unsigned long> icon(px.begin(), px.end());
        if (!icon.empty())
            XChangeProperty(g_dpy, g_win, XInternAtom(g_dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(icon.data()), int(icon.size()));
    }
}

}  // namespace

bool init(const WindowDesc& desc) {
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    // The input method's locale (LC_CTYPE only), before the display is opened.
    x11::setTextLocale();
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
    // PropertyChangeMask: the clipboard's transfers and the window manager's fullscreen state.
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask | FocusChangeMask | EnterWindowMask | LeaveWindowMask |
                     PropertyChangeMask;
    g_width = g_windowedW = desc.width;
    g_height = g_windowedH = desc.height;
    // Created at the windowed size, which the window manager restores when it leaves fullscreen.
    g_win = XCreateWindow(g_dpy, root, 0, 0, unsigned(g_width), unsigned(g_height), 0, vi->depth, InputOutput,
                          vi->visual, CWColormap | CWEventMask, &swa);
    XFree(vi);
    XStoreName(g_dpy, g_win, desc.title);
    g_wmProtocols = XInternAtom(g_dpy, "WM_PROTOCOLS", False);
    g_wmDelete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &g_wmDelete, 1);
    setIdentity(desc.title);
    g_clipboard.attach(g_dpy, g_win);
    g_fullscreen.attach(g_dpy, g_win);
    if (desc.mode == DisplayMode::Borderless) {
        g_fullscreen.start();
        // Without a window manager the window covers the screen at once; with one, its
        // ConfigureNotify gives the size.
        if (!g_fullscreen.managed()) {
            g_width = DisplayWidth(g_dpy, screen);
            g_height = DisplayHeight(g_dpy, screen);
        }
    }
    g_keyboard.open(g_dpy, g_win);
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
    // Copied text stays on the clipboard once the game has quit when a clipboard manager runs.
    g_clipboard.handOver(1000);
    g_keyboard.close();
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
        noteTime(e);
        // The input method's events, the keys and the typed text; then the clipboard's requests, the
        // window manager's fullscreen state and the focus (which releases the keys held when lost).
        if (g_keyboard.event(e, g_input) || g_clipboard.event(e) || g_fullscreen.event(e) ||
            x11::focusEvent(e, g_win, g_keyboard, g_input, g_focus))
            continue;
        switch (e.type) {
            case ClientMessage:
                if (e.xclient.message_type == g_wmProtocols && Atom(e.xclient.data.l[0]) == g_wmDelete) g_quit = true;
                break;
            case ConfigureNotify:
                g_width = e.xconfigure.width;
                g_height = e.xconfigure.height;
                break;
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
                    case 1: x11::setButton(g_input, MOUSE_LEFT, down); break;
                    case 2: x11::setButton(g_input, MOUSE_MIDDLE, down); break;
                    case 3: x11::setButton(g_input, MOUSE_RIGHT, down); break;
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
void setDisplayMode(DisplayMode mode, int w, int h) {
    // Fullscreen is the window manager's _NET_WM_STATE_FULLSCREEN (x11_desktop.h): its monitor,
    // without decorations, and the windowed geometry back when it ends.
    const bool full = mode == DisplayMode::Borderless;
    // Unchanged (Options applied for another setting): the window keeps its size, as on Windows,
    // and a state the player changed through the window manager stays.
    if (full == g_fullscreen.wanted() && (full || (w == g_windowedW && h == g_windowedH))) return;
    if (!full) {
        g_windowedW = w;
        g_windowedH = h;
    }
    g_fullscreen.set(full, g_windowedW, g_windowedH);
}
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

// The core library's folders (net::sys), so both layers agree: $XDG_CONFIG_HOME/scacelith/ (by default
// ~/.config/scacelith/) is private (0700).
std::string exeDirectory() { return net::sys::exeDirectory(); }
std::string userDataDirectory() { return net::sys::userDataDirectory(); }
std::string appDataDirectory() { return net::sys::appDataDirectory(); }
void messageBox(const char* title, const char* text, bool) { LOGE("%s: %s", title, text); }
uint64_t randomSeed() {
    timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return uint64_t(t.tv_nsec) * 2654435761ULL ^ uint64_t(t.tv_sec) ^ (uint64_t(getpid()) << 32);
}
std::string clipboardText() { return g_dpy ? g_clipboard.text(g_lastTime) : std::string(); }
bool setClipboardText(const std::string& utf8, bool sensitive) {
    return g_dpy && g_clipboard.setText(utf8, sensitive, g_lastTime);
}
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
    char* argv[] = {const_cast<char*>("xdg-open"), const_cast<char*>(arg.c_str()), nullptr};
    // Not posix_spawnp itself: the file manager gets SIGPIPE's default action back (the game
    // ignores that signal, main.cpp).
    const pid_t pid = net::sys::spawnProgram(argv);
    if (pid < 0) {
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
