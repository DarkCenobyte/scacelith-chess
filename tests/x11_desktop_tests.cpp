// The desktop side of the X11 layer (src/platform/x11_desktop.h) against a real X server: the
// clipboard between clients (another client, in a thread of its own, copies, pastes or acts as the
// clipboard manager), text typed through Xlib's input method (dead keys, Compose, Cyrillic,
// Arabic, Greek), the keys of shortcuts on other layouts, focus loss, and fullscreen with and
// without a window manager.
// They change the server's keyboard map, clipboard and focus: they run only with
// SCACELITH_X11_TESTS=1 and DISPLAY on a test server. tools/test_x11.sh runs them under Xvfb twice,
// without a window manager (--require=x11) and with openbox (SCACELITH_X11_WM=1,
// --require=x11,x11-wm).
#if !defined(_WIN32) && !defined(__APPLE__)
#include "test.h"
#include "platform/x11_desktop.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <langinfo.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace x11 = plat::x11;

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

bool enabled() {
    const char* v = std::getenv("SCACELITH_X11_TESTS");
    return v && *v && std::strcmp(v, "0") != 0;
}

Display* openDisplay() {
    static const bool threads = XInitThreads() != 0;  // each client below has its own connection
    (void)threads;
    return XOpenDisplay(nullptr);
}

struct Connection {
    Display* dpy = openDisplay();
    ~Connection() {
        if (dpy) XCloseDisplay(dpy);
    }
};

#define X11_TEST_DISPLAY(NAME)                                                                              \
    if (!enabled())                                                                                         \
        SKIP_WITHOUT("x11", "needs SCACELITH_X11_TESTS=1 and DISPLAY on a test X server (tools/test_x11.sh): " \
                            "these tests change its keyboard map, clipboard and focus");                    \
    Connection NAME##_connection;                                                                           \
    REQUIRE(NAME##_connection.dpy != nullptr);                                                              \
    testing::uses("x11");                                                                                   \
    Display* NAME = NAME##_connection.dpy

Window makeWindow(Display* dpy, int w, int h) {
    XSetWindowAttributes a{};
    a.event_mask = KeyPressMask | KeyReleaseMask | StructureNotifyMask | FocusChangeMask | PropertyChangeMask;
    const Window win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, unsigned(w), unsigned(h), 0, CopyFromParent,
                                     InputOutput, CopyFromParent, CWEventMask, &a);
    XSync(dpy, False);
    return win;
}

// Maps the window and waits until it shows (its events stay queued).
bool mapAndWait(Display* dpy, Window win) {
    XMapWindow(dpy, win);
    const auto end = Clock::now() + 3s;
    while (Clock::now() < end) {
        XWindowAttributes wa{};
        if (XGetWindowAttributes(dpy, win, &wa) && wa.map_state == IsViewable) return true;
        usleep(2000);
    }
    return false;
}

std::string atomName(Display* dpy, Atom a) {
    if (a == None) return "None";
    char* n = XGetAtomName(dpy, a);
    std::string s = n ? n : "";
    if (n) XFree(n);
    return s;
}

Atom atom(Display* dpy, const char* name) { return XInternAtom(dpy, name, False); }

// The next event the predicate takes, within 'ms' (the others are dropped).
template <class F>
bool nextEvent(Display* dpy, XEvent& ev, F pred, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            if (pred(ev)) return true;
        }
        if (Clock::now() >= end) return false;
        usleep(500);
    }
}

// ---- Another client: what a text editor or a browser does with the clipboard -------------------
struct Converted {
    bool ok = false;
    std::string type, bytes;
    std::vector<std::string> atoms;
};

// Pastes: asks the CLIPBOARD's owner for 'target' (3 s at most).
Converted convert(Display* dpy, Window win, const char* target, const char* selection = "CLIPBOARD") {
    const Atom property = atom(dpy, "PEER_PROPERTY");
    XConvertSelection(dpy, atom(dpy, selection), atom(dpy, target), property, win, CurrentTime);
    XFlush(dpy);
    XEvent ev;
    Converted c;
    if (!nextEvent(dpy, ev, [&](const XEvent& e) { return e.type == SelectionNotify && e.xselection.requestor == win; }, 3000) ||
        ev.xselection.property == None)
        return c;
    Atom type = None;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    XGetWindowProperty(dpy, win, property, 0, 1 << 24, True, AnyPropertyType, &type, &format, &n, &after, &data);
    c.ok = true;
    c.type = atomName(dpy, type);
    if (format == 8 && data) c.bytes.assign(reinterpret_cast<const char*>(data), n);
    if (format == 32 && data && type == XA_ATOM)
        for (unsigned long i = 0; i < n; ++i) c.atoms.push_back(atomName(dpy, reinterpret_cast<unsigned long*>(data)[i]));
    if (data) XFree(data);
    return c;
}

bool has(const std::vector<std::string>& v, const char* s) {
    for (const std::string& x : v)
        if (x == s) return true;
    return false;
}

// Copies: owns the CLIPBOARD in a thread of its own and serves 'bytes' as 'type', in INCR parts of
// 'chunk' bytes when longer; 'mute' never answers.
struct Owner {
    std::string bytes, type = "UTF8_STRING";
    size_t chunk = size_t(1) << 22;
    bool mute = false;
    std::atomic<bool> ready{false}, stop{false};
    std::thread thread;

    void start() {
        thread = std::thread([this] { run(); });
        while (!ready) usleep(500);
    }
    ~Owner() {
        stop = true;
        if (thread.joinable()) thread.join();
    }
    void run() {
        Connection c;
        Display* dpy = c.dpy;
        const Window win = makeWindow(dpy, 10, 10);
        const Atom clipboard = atom(dpy, "CLIPBOARD"), targets = atom(dpy, "TARGETS"), incr = atom(dpy, "INCR");
        const Atom t = atom(dpy, type.c_str());
        XSetSelectionOwner(dpy, clipboard, win, CurrentTime);
        XSync(dpy, False);
        ready = true;
        struct Part {
            Window requestor;
            Atom property;
            size_t offset;
            bool done;
        };
        std::vector<Part> parts;
        while (!stop) {
            while (XPending(dpy)) {
                XEvent e;
                XNextEvent(dpy, &e);
                if (e.type == SelectionRequest && !mute) {
                    const XSelectionRequestEvent& r = e.xselectionrequest;
                    XEvent reply{};
                    XSelectionEvent& s = reply.xselection;
                    s.type = SelectionNotify;
                    s.requestor = r.requestor;
                    s.selection = r.selection;
                    s.target = r.target;
                    s.time = r.time;
                    s.property = r.property;
                    if (r.target == targets) {
                        const Atom list[] = {targets, t};
                        XChangeProperty(dpy, r.requestor, r.property, XA_ATOM, 32, PropModeReplace,
                                        reinterpret_cast<const unsigned char*>(list), 2);
                    } else if (r.target == t && bytes.size() <= chunk) {
                        XChangeProperty(dpy, r.requestor, r.property, t, 8, PropModeReplace,
                                        reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()));
                    } else if (r.target == t) {
                        XSelectInput(dpy, r.requestor, PropertyChangeMask);
                        const long size = long(bytes.size());
                        XChangeProperty(dpy, r.requestor, r.property, incr, 32, PropModeReplace,
                                        reinterpret_cast<const unsigned char*>(&size), 1);
                        parts.push_back({r.requestor, r.property, 0, false});
                    } else {
                        s.property = None;
                    }
                    XSendEvent(dpy, r.requestor, False, NoEventMask, &reply);
                    XFlush(dpy);
                } else if (e.type == PropertyNotify && e.xproperty.state == PropertyDelete) {
                    // The requestor read a part: the next one, then one of length 0.
                    for (Part& p : parts) {
                        if (p.done || p.requestor != e.xproperty.window || p.property != e.xproperty.atom) continue;
                        const size_t n = std::min(chunk, bytes.size() - p.offset);
                        XChangeProperty(dpy, p.requestor, p.property, t, 8, PropModeReplace,
                                        reinterpret_cast<const unsigned char*>(bytes.data() + p.offset), int(n));
                        p.offset += n;
                        p.done = n == 0;
                        XFlush(dpy);
                    }
                }
            }
            usleep(200);
        }
        XDestroyWindow(dpy, win);
    }
};

// Serves our side's clipboard requests until 'done' (5 s at most).
void serveUntil(Display* dpy, x11::Clipboard& cb, const std::atomic<bool>& done) {
    const auto end = Clock::now() + 5s;
    while (!done && Clock::now() < end) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            cb.event(e);
        }
        usleep(200);
    }
}

// What another client gets for each target of the game's clipboard.
std::vector<Converted> pastedByAnother(Display* dpy, x11::Clipboard& cb, const std::vector<const char*>& targets) {
    std::vector<Converted> out;
    std::atomic<bool> done{false};
    std::thread peer([&] {
        Connection c;
        const Window win = makeWindow(c.dpy, 10, 10);
        for (const char* t : targets) out.push_back(convert(c.dpy, win, t));
        XDestroyWindow(c.dpy, win);
        done = true;
    });
    serveUntil(dpy, cb, done);
    peer.join();
    return out;
}

// ---- Keyboard ---------------------------------------------------------------------------------
// LC_CTYPE and XMODIFIERS for Xlib's own input method (whatever the session runs), put back after.
struct TextLocale {
    std::string ctype = std::setlocale(LC_CTYPE, nullptr);
    std::string modifiers;
    bool hadModifiers = false;
    TextLocale() {
        if (const char* m = std::getenv("XMODIFIERS")) {
            hadModifiers = true;
            modifiers = m;
        }
        setenv("XMODIFIERS", "@im=none", 1);
    }
    ~TextLocale() {
        std::setlocale(LC_CTYPE, ctype.c_str());
        if (hadModifiers) setenv("XMODIFIERS", modifiers.c_str(), 1);
        else unsetenv("XMODIFIERS");
    }
};

// Keysyms given to keycodes for a test, the server's map put back after.
class KeymapChange {
public:
    explicit KeymapChange(Display* dpy) : dpy_(dpy) {}
    ~KeymapChange() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it)
            XChangeKeyboardMapping(dpy_, it->code, int(it->syms.size()), it->syms.data(), 1);
        XSync(dpy_, False);
    }
    void set(KeyCode code, KeySym lower, KeySym upper) {
        int per = 0;
        KeySym* syms = XGetKeyboardMapping(dpy_, code, 1, &per);
        saved_.push_back({code, std::vector<KeySym>(syms, syms + per)});
        XFree(syms);
        KeySym s[2] = {lower, upper};
        XChangeKeyboardMapping(dpy_, code, 2, s, 1);
        XSync(dpy_, False);
    }

private:
    struct Saved {
        KeyCode code;
        std::vector<KeySym> syms;
    };
    Display* dpy_;
    std::vector<Saved> saved_;
};

std::vector<KeyCode> freeKeycodes(Display* dpy) {
    int min = 0, max = 0, per = 0;
    XDisplayKeycodes(dpy, &min, &max);
    KeySym* syms = XGetKeyboardMapping(dpy, KeyCode(min), max - min + 1, &per);
    std::vector<KeyCode> out;
    for (int kc = min; kc <= max; ++kc) {
        bool none = true;
        for (int i = 0; i < per; ++i) none = none && syms[(kc - min) * per + i] == NoSymbol;
        if (none) out.push_back(KeyCode(kc));
    }
    XFree(syms);
    return out;
}

KeyCode keycodeNamed(Display* dpy, const char* name) {
    KeyCode found = 0;
    XkbDescPtr d = XkbGetMap(dpy, 0, XkbUseCoreKbd);
    if (d && XkbGetNames(dpy, XkbKeyNamesMask, d) == Success && d->names && d->names->keys)
        for (int kc = d->min_key_code; kc <= d->max_key_code && !found; ++kc)
            if (std::strncmp(d->names->keys[kc].name, name, XkbKeyNameLength) == 0) found = KeyCode(kc);
    if (d) XkbFreeKeyboard(d, 0, True);
    return found;
}

void sendKey(Display* dpy, Window win, KeyCode code, unsigned state, Time time, bool press) {
    XEvent ev{};
    XKeyEvent& k = ev.xkey;
    k.type = press ? KeyPress : KeyRelease;
    k.display = dpy;
    k.window = win;
    k.root = DefaultRootWindow(dpy);
    k.time = time;
    k.x = k.y = k.x_root = k.y_root = 1;
    k.state = state;
    k.keycode = code;
    k.same_screen = True;
    XSendEvent(dpy, win, False, press ? KeyPressMask : KeyReleaseMask, &ev);
}

// Every event through the keyboard, the composed ones put back by the input method included.
void pump(Display* dpy, x11::Keyboard& kb, plat::Input& in) {
    XSync(dpy, False);
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        kb.event(e, in);
    }
}

void newFrame(plat::Input& in) {
    for (int k = 0; k < plat::KEY_COUNT; ++k) in.keyPressed[k] = in.keyReleased[k] = false;
    for (int b = 0; b < plat::MOUSE_BUTTON_COUNT; ++b) in.mousePressed[b] = in.mouseReleased[b] = false;
    in.textCount = 0;
}

std::u32string typedText(const plat::Input& in) {
    std::u32string s;
    for (int i = 0; i < in.textCount; ++i) s += char32_t(in.text[i]);
    return s;
}

// A window with the keyboard and its input context, from Xlib's own input method.
struct Typing {
    Display* dpy;
    Window win;
    x11::Keyboard kb;
    plat::Input in;
    Time time = 1000;
    explicit Typing(Display* d) : dpy(d), win(makeWindow(d, 200, 100)) {}
    ~Typing() {
        kb.close();
        XDestroyWindow(dpy, win);
    }
    bool open() {
        if (!mapAndWait(dpy, win)) return false;
        kb.open(dpy, win);
        kb.focus(true);
        pump(dpy, kb, in);
        newFrame(in);
        return kb.hasInputContext();
    }
    // A key pressed and released with these modifiers: what it typed.
    std::u32string type(KeyCode code, unsigned state = 0) {
        newFrame(in);
        sendKey(dpy, win, code, state, time, true);
        sendKey(dpy, win, code, state, time + 5, false);
        time += 10;
        pump(dpy, kb, in);
        return typedText(in);
    }
};

// ---- Fullscreen -------------------------------------------------------------------------------
struct Geometry {
    int x = 0, y = 0, w = 0, h = 0;
};
// The size, and the position on the root window; on XWayland the compositor places the window's
// surface itself (weston keeps a fullscreen window's X position), so the position reads 0, 0 there.
Geometry geometry(Display* dpy, Window win) {
    XWindowAttributes wa{};
    XGetWindowAttributes(dpy, win, &wa);
    Geometry g;
    Window child = None;
    int op, ev, err;
    if (!XQueryExtension(dpy, "XWAYLAND", &op, &ev, &err))
        XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 0, 0, &g.x, &g.y, &child);
    g.w = wa.width;
    g.h = wa.height;
    return g;
}

// The window's events through 'fs' until 'done' holds (3 s at most).
template <class F>
bool settle(Display* dpy, x11::Fullscreen& fs, F done) {
    const auto end = Clock::now() + 3s;
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            fs.event(e);
        }
        if (done()) return true;
        if (Clock::now() >= end) return false;
        usleep(2000);
    }
}

// Whether a window manager with EWMH fullscreen runs; with SCACELITH_X11_WM=1 one is expected, and
// waited for (5 s at most) while it starts: until it maps a window, as openbox drops the map
// requests sent while it starts up.
bool windowManager(Display* dpy) {
    const char* v = std::getenv("SCACELITH_X11_WM");
    const auto end = Clock::now() + (v && *v && std::strcmp(v, "0") != 0 ? 5s : 0s);
    while (!x11::Fullscreen::windowManagerSupportsFullscreen(dpy)) {
        if (Clock::now() >= end) return false;
        usleep(20000);
    }
    const Window probe = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 16, 16, 0, 0, 0);
    bool mapped = false;
    while (!mapped && Clock::now() < end) {
        XMapWindow(dpy, probe);
        const auto retry = Clock::now() + 200ms;
        while (!mapped && Clock::now() < retry) {
            XWindowAttributes wa{};
            mapped = XGetWindowAttributes(dpy, probe, &wa) && wa.map_state == IsViewable;
            if (!mapped) usleep(5000);
        }
    }
    XDestroyWindow(dpy, probe);
    XSync(dpy, False);
    return true;
}

}  // namespace

// ---- Clipboard --------------------------------------------------------------------------------
TEST(x11_clipboard_copy_reaches_other_clients) {
    X11_TEST_DISPLAY(dpy);
    const Window win = makeWindow(dpy, 100, 100);
    x11::Clipboard cb;
    cb.attach(dpy, win);
    const std::string text = "Gr\xC3\xBC\xC3\x9F" "e \xE2\x99\x9E \xE6\xBC\xA2\nline two";  // Grüße ♞ 漢\nline two
    REQUIRE(cb.setText(text, false, CurrentTime));
    CHECK(cb.owns());
    const std::vector<Converted> got = pastedByAnother(
        dpy, cb, {"UTF8_STRING", "text/plain;charset=utf-8", "STRING", "TEXT", "TARGETS", "TIMESTAMP",
                  "x-kde-passwordManagerHint", "image/png"});
    REQUIRE(got.size() == 8u);
    CHECK(got[0].ok);
    CHECK_EQ(got[0].type, std::string("UTF8_STRING"));
    CHECK_EQ(got[0].bytes, text);
    CHECK_EQ(got[1].bytes, text);
    // STRING is Latin-1: what it lacks is '?'.
    CHECK_EQ(got[2].type, std::string("STRING"));
    CHECK_EQ(got[2].bytes, std::string("Gr\xFC\xDF" "e ? ?\nline two"));
    // TEXT: the owner's pick, UTF-8 here (not all of it is Latin-1).
    CHECK_EQ(got[3].type, std::string("UTF8_STRING"));
    CHECK_EQ(got[3].bytes, text);
    CHECK(has(got[4].atoms, "UTF8_STRING") && has(got[4].atoms, "STRING") && has(got[4].atoms, "TARGETS") &&
          has(got[4].atoms, "MULTIPLE") && has(got[4].atoms, "SAVE_TARGETS"));
    CHECK(!has(got[4].atoms, "x-kde-passwordManagerHint"));
    CHECK(got[5].ok);
    CHECK(!got[6].ok);  // not sensitive: no hint
    CHECK(!got[7].ok);  // not text
    // The game pastes its own text without asking the server.
    CHECK_EQ(cb.text(CurrentTime), text);
    // A copy of only Latin-1 is STRING for TEXT.
    REQUIRE(cb.setText("caf\xC3\xA9", false, CurrentTime));
    const std::vector<Converted> latin = pastedByAnother(dpy, cb, {"TEXT"});
    REQUIRE(latin.size() == 1u);
    CHECK_EQ(latin[0].type, std::string("STRING"));
    CHECK_EQ(latin[0].bytes, std::string("caf\xE9"));
    XDestroyWindow(dpy, win);
}

TEST(x11_clipboard_marks_sensitive_text_for_clipboard_managers) {
    X11_TEST_DISPLAY(dpy);
    const Window win = makeWindow(dpy, 100, 100);
    x11::Clipboard cb;
    cb.attach(dpy, win);
    REQUIRE(cb.setText("abcd-efgh\nijkl-mnop", true, CurrentTime));
    const std::vector<Converted> got = pastedByAnother(dpy, cb, {"TARGETS", "x-kde-passwordManagerHint", "UTF8_STRING"});
    REQUIRE(got.size() == 3u);
    CHECK(has(got[0].atoms, "x-kde-passwordManagerHint"));
    CHECK(got[1].ok);
    CHECK_EQ(got[1].bytes, std::string("secret"));
    // Still pasted where the player pastes it.
    CHECK_EQ(got[2].bytes, std::string("abcd-efgh\nijkl-mnop"));
    XDestroyWindow(dpy, win);
}

TEST(x11_clipboard_serves_multiple) {
    X11_TEST_DISPLAY(dpy);
    const Window win = makeWindow(dpy, 100, 100);
    x11::Clipboard cb;
    cb.attach(dpy, win);
    REQUIRE(cb.setText("e4 e5", false, CurrentTime));
    std::atomic<bool> done{false};
    std::string first, pairsAfter;
    bool answered = false;
    std::thread peer([&] {
        Connection c;
        Display* d = c.dpy;
        const Window w = makeWindow(d, 10, 10);
        const Atom p1 = atom(d, "PEER_ONE"), p2 = atom(d, "PEER_TWO"), multiple = atom(d, "PEER_MULTIPLE");
        const unsigned long pairs[4] = {atom(d, "UTF8_STRING"), p1, atom(d, "image/png"), p2};
        XChangeProperty(d, w, multiple, atom(d, "ATOM_PAIR"), 32, PropModeReplace, reinterpret_cast<const unsigned char*>(pairs), 4);
        XConvertSelection(d, atom(d, "CLIPBOARD"), atom(d, "MULTIPLE"), multiple, w, CurrentTime);
        XFlush(d);
        XEvent ev;
        if (nextEvent(d, ev, [&](const XEvent& e) { return e.type == SelectionNotify; }, 3000) && ev.xselection.property == multiple) {
            answered = true;
            Atom type;
            int format;
            unsigned long n, after;
            unsigned char* data = nullptr;
            XGetWindowProperty(d, w, p1, 0, 1024, True, AnyPropertyType, &type, &format, &n, &after, &data);
            if (data) {
                first.assign(reinterpret_cast<const char*>(data), n);
                XFree(data);
            }
            data = nullptr;
            XGetWindowProperty(d, w, multiple, 0, 16, True, AnyPropertyType, &type, &format, &n, &after, &data);
            if (data) {
                for (unsigned long i = 0; i < n; ++i) pairsAfter += atomName(d, reinterpret_cast<unsigned long*>(data)[i]) + ";";
                XFree(data);
            }
        }
        XDestroyWindow(d, w);
        done = true;
    });
    serveUntil(dpy, cb, done);
    peer.join();
    CHECK(answered);
    CHECK_EQ(first, std::string("e4 e5"));
    // The pair it could not convert names no property.
    CHECK_EQ(pairsAfter, std::string("UTF8_STRING;PEER_ONE;image/png;None;"));
    XDestroyWindow(dpy, win);
}

TEST(x11_clipboard_pastes_from_other_clients) {
    X11_TEST_DISPLAY(dpy);
    const Window win = makeWindow(dpy, 100, 100);
    x11::Clipboard cb;
    cb.attach(dpy, win);
    {
        Owner o;
        o.bytes = "1. e4 e5 2. Nf3 \xE2\x99\x9E\n";
        o.start();
        CHECK_EQ(cb.text(CurrentTime), o.bytes);
        CHECK(!cb.owns());
    }
    {
        // An owner that offers STRING only: Latin-1, as UTF-8 here.
        Owner o;
        o.type = "STRING";
        o.bytes = "caf\xE9";
        o.start();
        CHECK_EQ(cb.text(CurrentTime), std::string("caf\xC3\xA9"));
    }
    {
        // Malformed UTF-8 is replaced, and the text ends at its first NUL.
        Owner o;
        o.bytes = std::string("a\xFF" "b\xC3", 4) + std::string("\0tail", 5);
        o.start();
        CHECK_EQ(cb.text(CurrentTime), std::string("a\xEF\xBF\xBD" "b\xEF\xBF\xBD"));
    }
    {
        // 1.5 MB in INCR parts of 64 KB (a long PGN from a browser).
        Owner o;
        for (int i = 0; o.bytes.size() < 1500000; ++i) o.bytes += std::to_string(i) + ". e4 e5 ";
        o.chunk = 65536;
        o.start();
        const std::string got = cb.text(CurrentTime);
        CHECK_EQ(got.size(), o.bytes.size());
        CHECK(got == o.bytes);
    }
    {
        // Over the limit, in parts or at once: nothing.
        for (size_t chunk : {size_t(65536), size_t(1) << 23}) {
            Owner o;
            o.bytes.assign(x11::Clipboard::kMaxBytes + 1000, 'x');
            o.chunk = chunk;
            o.start();
            CHECK_EQ(cb.text(CurrentTime).size(), size_t(0));
        }
    }
    XDestroyWindow(dpy, win);
}

TEST(x11_clipboard_gives_up_on_an_owner_that_does_not_answer) {
    X11_TEST_DISPLAY(dpy);
    const Window win = makeWindow(dpy, 100, 100);
    x11::Clipboard cb;
    cb.attach(dpy, win);
    Owner o;
    o.bytes = "never sent";
    o.mute = true;
    o.start();
    const auto t0 = Clock::now();
    CHECK_EQ(cb.text(CurrentTime), std::string());
    const double s = std::chrono::duration<double>(Clock::now() - t0).count();
    // One wait of a second, not one per target.
    CHECK(s >= 0.9 && s < 2.0);
    XDestroyWindow(dpy, win);
}

TEST(x11_clipboard_lost_to_another_client) {
    X11_TEST_DISPLAY(dpy);
    const Window win = makeWindow(dpy, 100, 100);
    x11::Clipboard cb;
    cb.attach(dpy, win);
    REQUIRE(cb.setText("mine", false, CurrentTime));
    Owner o;
    o.bytes = "theirs";
    o.start();
    XSync(dpy, False);
    bool cleared = false;
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        cleared |= e.type == SelectionClear && cb.event(e);
    }
    CHECK(cleared);
    CHECK(!cb.owns());
    CHECK_EQ(cb.text(CurrentTime), std::string("theirs"));
    XDestroyWindow(dpy, win);
}

TEST(x11_clipboard_survives_a_requestor_that_went_away) {
    X11_TEST_DISPLAY(dpy);
    const Window win = makeWindow(dpy, 100, 100), gone = makeWindow(dpy, 10, 10);
    x11::Clipboard cb;
    cb.attach(dpy, win);
    REQUIRE(cb.setText("text", false, CurrentTime));
    XDestroyWindow(dpy, gone);
    XSync(dpy, False);
    // Xlib's default error handler would end the process on the BadWindow of this answer.
    XEvent e{};
    XSelectionRequestEvent& r = e.xselectionrequest;
    r.type = SelectionRequest;
    r.display = dpy;
    r.owner = win;
    r.requestor = gone;
    r.selection = atom(dpy, "CLIPBOARD");
    r.target = atom(dpy, "UTF8_STRING");
    r.property = atom(dpy, "PEER_PROPERTY");
    r.time = CurrentTime;
    CHECK(cb.event(e));
    CHECK(cb.owns());
    XDestroyWindow(dpy, win);
}

TEST(x11_clipboard_handed_over_to_the_clipboard_manager_at_exit) {
    X11_TEST_DISPLAY(dpy);
    for (bool sensitive : {false, true}) {
        const Window win = makeWindow(dpy, 100, 100);
        x11::Clipboard cb;
        cb.attach(dpy, win);
        REQUIRE(cb.setText(sensitive ? "recovery-code" : "copied", sensitive, CurrentTime));
        std::atomic<bool> ready{false}, asked{false};
        std::string saved;
        std::thread manager([&] {
            Connection c;
            Display* d = c.dpy;
            const Window w = makeWindow(d, 10, 10);
            XSetSelectionOwner(d, atom(d, "CLIPBOARD_MANAGER"), w, CurrentTime);
            XSync(d, False);
            ready = true;
            XEvent e;
            const Atom save = atom(d, "SAVE_TARGETS");
            if (nextEvent(d, e, [&](const XEvent& x) { return x.type == SelectionRequest && x.xselectionrequest.target == save; },
                          sensitive ? 500 : 3000)) {
                asked = true;
                const XSelectionRequestEvent req = e.xselectionrequest;
                // A copy of the text, as clipboard managers take it, then the answer.
                const Converted text = convert(d, w, "UTF8_STRING");
                saved = text.bytes;
                XEvent reply{};
                XSelectionEvent& s = reply.xselection;
                s.type = SelectionNotify;
                s.requestor = req.requestor;
                s.selection = req.selection;
                s.target = req.target;
                s.time = req.time;
                s.property = text.ok ? save : None;
                XSendEvent(d, req.requestor, False, NoEventMask, &reply);
                XFlush(d);
            }
            XDestroyWindow(d, w);
        });
        while (!ready) usleep(500);
        const auto t0 = Clock::now();
        cb.handOver(2000);
        const double s = std::chrono::duration<double>(Clock::now() - t0).count();
        manager.join();
        if (sensitive) {
            // Recovery codes leave the clipboard with the game.
            CHECK(!asked);
            CHECK(s < 0.1);
        } else {
            CHECK(asked);
            CHECK_EQ(saved, std::string("copied"));
            CHECK(s < 1.5);
        }
        XDestroyWindow(dpy, win);
    }
}

// ---- Keyboard ---------------------------------------------------------------------------------
TEST(x11_text_locale_is_utf8) {
    X11_TEST_DISPLAY(dpy);
    (void)dpy;
    TextLocale saved;
    const char* all = std::getenv("LC_ALL");
    const std::string oldAll = all ? all : "";
    setenv("LC_ALL", "C", 1);
    // The POSIX locale has no compose table for other scripts: C.UTF-8 takes its place.
    CHECK(x11::setTextLocale());
    const char* cs = nl_langinfo(CODESET);
    CHECK(cs && (std::strcmp(cs, "UTF-8") == 0 || std::strcmp(cs, "utf8") == 0));
    if (all) setenv("LC_ALL", oldAll.c_str(), 1);
    else unsetenv("LC_ALL");
}

TEST(x11_text_input_composes_and_types_every_script) {
    X11_TEST_DISPLAY(dpy);
    TextLocale saved;
    REQUIRE(x11::setTextLocale());
    const std::vector<KeyCode> codes = freeKeycodes(dpy);
    REQUIRE(codes.size() >= 8u);
    KeymapChange map(dpy);
    map.set(codes[0], XK_dead_circumflex, XK_dead_diaeresis);
    map.set(codes[1], XK_e, XK_E);
    map.set(codes[2], XK_Cyrillic_a, XK_Cyrillic_A);  // the legacy keysyms of XKB's layouts (0x6c1)
    map.set(codes[3], XK_Arabic_alef, XK_Arabic_alef);
    map.set(codes[4], XK_Greek_alpha, XK_Greek_ALPHA);
    map.set(codes[5], XK_Multi_key, XK_Multi_key);
    map.set(codes[6], XK_o, XK_O);
    map.set(codes[7], 0x100263A, 0x100263A);  // a Unicode keysym (U+263A)
    Typing t(dpy);
    REQUIRE(t.open());
    // French: ^ then e is ê, the dead key types nothing by itself.
    CHECK(t.type(codes[0]) == U"");
    CHECK(t.type(codes[1]) == U"ê");
    CHECK(t.type(codes[0], ShiftMask) == U"");
    CHECK(t.type(codes[1], ShiftMask) == U"Ë");
    // Russian, Arabic, Greek.
    CHECK(t.type(codes[2]) == U"а");
    CHECK(t.type(codes[2], ShiftMask) == U"А");
    CHECK(t.type(codes[3]) == U"ا");
    CHECK(t.type(codes[4]) == U"α");
    // Compose, o, e: œ.
    CHECK(t.type(codes[5]) == U"");
    CHECK(t.type(codes[6]) == U"");
    CHECK(t.type(codes[1]) == U"œ");
    CHECK(t.type(codes[7]) == U"☺");
    // Ctrl+key is a shortcut: no text.
    CHECK(t.type(codes[2], ControlMask) == U"");
    CHECK(t.type(codes[1], ControlMask) == U"");
    CHECK(t.in.keyPressed['E']);
}

TEST(x11_keys_reported_once_when_the_input_method_sends_them_back) {
    X11_TEST_DISPLAY(dpy);
    TextLocale saved;
    REQUIRE(x11::setTextLocale());
    Typing t(dpy);
    REQUIRE(t.open());
    const KeyCode e = XKeysymToKeycode(dpy, XK_e);
    REQUIRE(e != 0);
    // IBus and Fcitx take a key in XFilterEvent and send back the one they do not use, with the
    // same time: one press for the game (Backspace must not erase twice).
    sendKey(dpy, t.win, e, 0, 5000, true);
    pump(dpy, t.kb, t.in);
    CHECK(t.in.keyPressed['E']);
    newFrame(t.in);
    sendKey(dpy, t.win, e, 0, 5000, true);
    pump(dpy, t.kb, t.in);
    CHECK(!t.in.keyPressed['E']);
    CHECK(t.in.keyDown['E']);
    // Auto-repeat: a later press counts.
    newFrame(t.in);
    sendKey(dpy, t.win, e, 0, 5040, true);
    pump(dpy, t.kb, t.in);
    CHECK(t.in.keyPressed['E']);
    sendKey(dpy, t.win, e, 0, 5050, false);
    pump(dpy, t.kb, t.in);
    CHECK(!t.in.keyDown['E']);
}

TEST(x11_shortcut_keys_on_other_layouts) {
    X11_TEST_DISPLAY(dpy);
    TextLocale saved;
    REQUIRE(x11::setTextLocale());
    const KeyCode v = keycodeNamed(dpy, "AB04"), one = keycodeNamed(dpy, "AE01"), a = keycodeNamed(dpy, "AC01"),
                  m = keycodeNamed(dpy, "AB07");
    REQUIRE(v && one && a && m);
    KeymapChange map(dpy);
    map.set(v, XK_Cyrillic_em, XK_Cyrillic_EM);  // Russian: the V key types м
    map.set(one, XK_ampersand, XK_1);            // AZERTY's digit row
    map.set(a, XK_q, XK_Q);                      // AZERTY: Q where QWERTY has A
    map.set(m, XK_comma, XK_question);           // AZERTY: a comma where QWERTY has M
    Typing t(dpy);
    REQUIRE(t.open());
    CHECK_EQ(t.kb.shortcutKey(v, 0), int('V'));
    CHECK_EQ(t.kb.shortcutKey(one, 0), int('1'));
    CHECK_EQ(t.kb.shortcutKey(a, 0), int('Q'));
    CHECK_EQ(t.kb.shortcutKey(m, 0), int(plat::KEY_UNKNOWN));
    // Ctrl+V pastes on a Russian layout, and the key types м without Ctrl.
    CHECK(t.type(v, ControlMask) == U"");
    CHECK(t.in.keyPressed['V']);
    CHECK(t.type(v) == U"м");
    CHECK(t.type(one, ShiftMask) == U"1");
    CHECK(t.in.keyPressed['1']);
    // A key goes up as the key its press reported, though the layout changed in between.
    t.in = plat::Input{};
    sendKey(t.dpy, t.win, v, 0, t.time, true);
    pump(t.dpy, t.kb, t.in);
    CHECK(t.in.keyDown['V']);
    map.set(v, XK_b, XK_B);
    sendKey(t.dpy, t.win, v, 0, t.time + 5, false);
    pump(t.dpy, t.kb, t.in);
    CHECK(!t.in.keyDown['V']);
    CHECK(t.in.keyReleased['V']);
}

// ---- Focus ------------------------------------------------------------------------------------
TEST(x11_focus_loss_releases_held_keys_and_buttons) {
    X11_TEST_DISPLAY(dpy);
    TextLocale saved;
    x11::setTextLocale();
    Typing t(dpy);
    t.open();
    const Window other = makeWindow(dpy, 100, 100);
    REQUIRE(mapAndWait(dpy, other));
    bool focused = false;
    auto pumpFocus = [&](bool want) {
        const auto end = Clock::now() + 3s;
        while (Clock::now() < end) {
            XSync(dpy, False);
            while (XPending(dpy)) {
                XEvent e;
                XNextEvent(dpy, &e);
                if (!t.kb.event(e, t.in)) x11::focusEvent(e, t.win, t.kb, t.in, focused);
            }
            if (focused == want) return true;
            usleep(2000);
        }
        return false;
    };
    XSetInputFocus(dpy, t.win, RevertToParent, CurrentTime);
    REQUIRE(pumpFocus(true));
    // W held to walk in the observer view, Shift held, the right button held to look around.
    const KeyCode w = XKeysymToKeycode(dpy, XK_w), shift = XKeysymToKeycode(dpy, XK_Shift_L);
    sendKey(dpy, t.win, shift, 0, 7000, true);
    sendKey(dpy, t.win, w, ShiftMask, 7010, true);
    x11::setButton(t.in, plat::MOUSE_RIGHT, true);
    pump(dpy, t.kb, t.in);
    REQUIRE(t.in.keyDown['W'] && t.in.keyDown[plat::KEY_LSHIFT]);
    newFrame(t.in);
    // Alt+Tab: another window has the keyboard, and the releases go there.
    XSetInputFocus(dpy, other, RevertToParent, CurrentTime);
    REQUIRE(pumpFocus(false));
    CHECK(!t.in.keyDown['W']);
    CHECK(t.in.keyReleased['W']);
    CHECK(!t.in.keyDown[plat::KEY_LSHIFT]);
    CHECK(t.in.keyReleased[plat::KEY_LSHIFT]);
    CHECK(!t.in.mouseDown[plat::MOUSE_RIGHT]);
    CHECK(t.in.mouseReleased[plat::MOUSE_RIGHT]);
    XSetInputFocus(dpy, t.win, RevertToParent, CurrentTime);
    CHECK(pumpFocus(true));
    XDestroyWindow(dpy, other);
}

// ---- Fullscreen -------------------------------------------------------------------------------
TEST(x11_fullscreen_through_the_window_manager) {
    X11_TEST_DISPLAY(dpy);
    if (!windowManager(dpy))
        SKIP_WITHOUT("x11-wm", "no window manager with EWMH fullscreen (tools/test_x11.sh runs openbox)");
    testing::uses("x11-wm");
    const int sw = DisplayWidth(dpy, DefaultScreen(dpy)), sh = DisplayHeight(dpy, DefaultScreen(dpy));
    const Window win = makeWindow(dpy, 640, 400);
    x11::Fullscreen fs;
    fs.attach(dpy, win);
    REQUIRE(mapAndWait(dpy, win));
    REQUIRE(settle(dpy, fs, [&] { return geometry(dpy, win).w == 640; }));
    CHECK(!fs.active());
    // Windowed to fullscreen: the whole monitor, at its corner, no frame.
    fs.set(true, 640, 400);
    CHECK(fs.managed());
    CHECK(settle(dpy, fs, [&] {
        const Geometry g = geometry(dpy, win);
        return fs.active() && g.x == 0 && g.y == 0 && g.w == sw && g.h == sh;
    }));
    // Back to windowed, at the size of the options.
    fs.set(false, 800, 500);
    CHECK(settle(dpy, fs, [&] {
        const Geometry g = geometry(dpy, win);
        return !fs.active() && g.w == 800 && g.h == 500;
    }));
    // The player leaves fullscreen through the window manager: the state follows.
    fs.set(true, 800, 500);
    REQUIRE(settle(dpy, fs, [&] { return fs.active(); }));
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = atom(dpy, "_NET_WM_STATE");
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 0;
    ev.xclient.data.l[1] = long(atom(dpy, "_NET_WM_STATE_FULLSCREEN"));
    ev.xclient.data.l[3] = 2;  // from a pager
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    CHECK(settle(dpy, fs, [&] { return !fs.active() && geometry(dpy, win).w < sw; }));
    XDestroyWindow(dpy, win);

    // Fullscreen from the start, then windowed.
    const Window start = makeWindow(dpy, 700, 450);
    x11::Fullscreen fs2;
    fs2.attach(dpy, start);
    fs2.start();
    CHECK(fs2.managed());
    REQUIRE(mapAndWait(dpy, start));
    CHECK(settle(dpy, fs2, [&] {
        const Geometry g = geometry(dpy, start);
        return fs2.active() && g.x == 0 && g.y == 0 && g.w == sw && g.h == sh;
    }));
    fs2.set(false, 700, 450);
    CHECK(settle(dpy, fs2, [&] {
        const Geometry g = geometry(dpy, start);
        return !fs2.active() && g.w == 700 && g.h == 450;
    }));
    XDestroyWindow(dpy, start);
}

TEST(x11_fullscreen_without_a_window_manager) {
    X11_TEST_DISPLAY(dpy);
    if (windowManager(dpy)) SKIP("a window manager runs (tools/test_x11.sh runs this test before it starts one)");
    const int sw = DisplayWidth(dpy, DefaultScreen(dpy)), sh = DisplayHeight(dpy, DefaultScreen(dpy));
    const Window win = makeWindow(dpy, 640, 400);
    x11::Fullscreen fs;
    fs.attach(dpy, win);
    REQUIRE(mapAndWait(dpy, win));
    fs.set(true, 640, 400);
    CHECK(!fs.managed());
    CHECK(fs.active());
    CHECK(settle(dpy, fs, [&] {
        const Geometry g = geometry(dpy, win);
        return g.x == 0 && g.y == 0 && g.w == sw && g.h == sh;
    }));
    fs.set(false, 800, 500);
    CHECK(!fs.active());
    CHECK(settle(dpy, fs, [&] {
        const Geometry g = geometry(dpy, win);
        return g.w == 800 && g.h == 500;
    }));
    XDestroyWindow(dpy, win);
    const Window start = makeWindow(dpy, 700, 450);
    x11::Fullscreen fs2;
    fs2.attach(dpy, start);
    fs2.start();
    REQUIRE(mapAndWait(dpy, start));
    CHECK(settle(dpy, fs2, [&] {
        const Geometry g = geometry(dpy, start);
        return g.w == sw && g.h == sh;
    }));
    XDestroyWindow(dpy, start);
}
#endif
