// The desktop side of the X11 layer: see x11_desktop.h.
#if !defined(_WIN32) && !defined(__APPLE__)
#include "x11_desktop.h"
#include "../core/log.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <langinfo.h>
#include <poll.h>
#include <strings.h>
#include <time.h>
#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <vector>

namespace plat {
namespace x11 {
namespace {

// ---- Helpers ----------------------------------------------------------------------------------
int g_trapped;  // the first error code an ErrorTrap caught, 0 for none
int trapHandler(Display*, XErrorEvent* e) {
    if (!g_trapped) g_trapped = e->error_code ? e->error_code : -1;
    return 0;
}

double nowMs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) * 1e3 + double(t.tv_nsec) * 1e-6;
}

// Waits for an event the predicate takes, until 'deadline' (nowMs()); the other events stay queued,
// in their order, for the events loop.
bool waitFor(Display* dpy, XEvent& ev, Bool (*pred)(Display*, XEvent*, XPointer), XPointer arg, double deadline) {
    for (;;) {
        // Flushes the requests, then reads what the server has sent.
        if (XCheckIfEvent(dpy, &ev, pred, arg)) return true;
        const double left = deadline - nowMs();
        if (left <= 0) return false;
        pollfd p{ConnectionNumber(dpy), POLLIN, 0};
        poll(&p, 1, int(left) + 1);
    }
}

// X server times are 32-bit milliseconds that wrap around: whether a comes at or after b.
bool notBefore(Time a, Time b) { return int32_t(uint32_t(a) - uint32_t(b)) >= 0; }

void putUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += char(cp);
    } else if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xF0 | (cp >> 18));
        s += char(0x80 | ((cp >> 12) & 0x3F));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
}

// The code point at s[i], i moved past it: U+FFFD for a malformed sequence (one byte consumed),
// an overlong form, a surrogate or a value beyond U+10FFFF.
uint32_t nextUtf8(const char* s, size_t n, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int len = 0;
    uint32_t cp = 0, min = 0;
    if (c < 0x80) {
        ++i;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        len = 2, cp = c & 0x1F, min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3, cp = c & 0x0F, min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4, cp = c & 0x07, min = 0x10000;
    } else {
        ++i;
        return 0xFFFD;
    }
    if (n - i < size_t(len)) {
        ++i;
        return 0xFFFD;
    }
    for (int k = 1; k < len; ++k) {
        const unsigned char d = static_cast<unsigned char>(s[i + size_t(k)]);
        if ((d & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (d & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) {
        ++i;
        return 0xFFFD;
    }
    i += size_t(len);
    return cp;
}

// Valid UTF-8, up to the first NUL.
std::string cleanUtf8(const char* s, size_t n) {
    n = std::find(s, s + n, '\0') - s;
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n;) putUtf8(out, nextUtf8(s, n, i));
    return out;
}

// ICCCM STRING: ISO Latin-1 (up to the first NUL).
std::string latin1ToUtf8(const char* s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n && s[i]; ++i) putUtf8(out, static_cast<unsigned char>(s[i]));
    return out;
}
// '?' for what Latin-1 lacks; 'all' false when there was any.
std::string utf8ToLatin1(const std::string& s, bool* all = nullptr) {
    std::string out;
    if (all) *all = true;
    for (size_t i = 0; i < s.size();) {
        const uint32_t cp = nextUtf8(s.data(), s.size(), i);
        if (cp > 0xFF && all) *all = false;
        out += cp <= 0xFF ? char(cp) : '?';
    }
    return out;
}

void wipe(std::string& s) {
    volatile char* p = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
    s.shrink_to_fit();
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
        case XK_Tab: case XK_ISO_Left_Tab: return KEY_TAB;
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
        case XK_Alt_R: case XK_ISO_Level3_Shift: return KEY_RALT;  // AltGr, as Windows reports it
    }
    return KEY_UNKNOWN;
}

// The letters and digits of a US keyboard by XKB key name (the row, then the place in the row).
struct Row { const char* name; const char* keys; };
constexpr Row kRows[] = {{"AE", "1234567890"}, {"AD", "QWERTYUIOP"}, {"AC", "ASDFGHJKL"}, {"AB", "ZXCVBNM"}};
// The same keys by evdev keycode, for a server without the XKB extension.
constexpr int kEvdevFirst[] = {10, 24, 38, 52};

// A keysym of a script other than Latin (Cyrillic, Greek, Arabic, Hebrew, Thai...): legacy keysyms
// above Latin-1 below the function keys, or a Unicode keysym.
bool otherScript(KeySym ks) { return (ks > 0xFF && ks < 0xFE00) || (ks & 0xFF000000UL) == 0x01000000UL; }

}  // namespace

// ---- Input state ------------------------------------------------------------------------------
void setKey(Input& in, int key, bool down) {
    if (key <= 0 || key >= KEY_COUNT) return;
    if (down) in.keyPressed[key] = true;
    else if (in.keyDown[key]) in.keyReleased[key] = true;
    in.keyDown[key] = down;
}

void setButton(Input& in, int b, bool down) {
    if (b < 0 || b >= MOUSE_BUTTON_COUNT) return;
    if (down && !in.mouseDown[b]) in.mousePressed[b] = true;
    if (!down && in.mouseDown[b]) in.mouseReleased[b] = true;
    in.mouseDown[b] = down;
}

void releaseAll(Input& in) {
    for (int k = 0; k < KEY_COUNT; ++k)
        if (in.keyDown[k]) setKey(in, k, false);
    for (int b = 0; b < MOUSE_BUTTON_COUNT; ++b) setButton(in, b, false);
}

void addText(Input& in, uint32_t cp) {
    if (cp < 32 || cp == 127 || (cp >= 0x80 && cp < 0xA0) || (cp >= 0xD800 && cp < 0xE000) || cp > 0x10FFFF) return;
    if (in.textCount < int(sizeof(in.text) / sizeof(in.text[0]))) in.text[in.textCount++] = cp;
}

// ---- Errors -----------------------------------------------------------------------------------
ErrorTrap::ErrorTrap(Display* dpy) : dpy_(dpy) {
    XSync(dpy_, False);
    g_trapped = 0;
    previous_ = XSetErrorHandler(trapHandler);
}

ErrorTrap::~ErrorTrap() {
    XSync(dpy_, False);
    XSetErrorHandler(previous_);
}

bool ErrorTrap::failed() {
    XSync(dpy_, False);
    return g_trapped != 0;
}

// ---- Clipboard --------------------------------------------------------------------------------
namespace {
struct Wanted {
    Window win;
    Atom selection, property;
};
Bool isNotify(Display*, XEvent* e, XPointer p) {
    const Wanted& w = *reinterpret_cast<const Wanted*>(p);
    return e->type == SelectionNotify && e->xselection.requestor == w.win && e->xselection.selection == w.selection;
}
Bool isNewValue(Display*, XEvent* e, XPointer p) {
    const Wanted& w = *reinterpret_cast<const Wanted*>(p);
    return e->type == PropertyNotify && e->xproperty.window == w.win && e->xproperty.atom == w.property &&
           e->xproperty.state == PropertyNewValue;
}
// The clipboard manager's answer, its requests for the text and the loss of the clipboard.
Bool isHandOver(Display*, XEvent* e, XPointer p) {
    const Wanted& w = *reinterpret_cast<const Wanted*>(p);
    return (e->type == SelectionNotify && e->xselection.requestor == w.win && e->xselection.selection == w.selection) ||
           (e->type == SelectionRequest && e->xselectionrequest.owner == w.win) ||
           (e->type == SelectionClear && e->xselectionclear.window == w.win);
}
}  // namespace

Clipboard::~Clipboard() { forget(); }

void Clipboard::attach(Display* dpy, Window win) {
    dpy_ = dpy;
    win_ = win;
    const char* names[] = {"CLIPBOARD", "TARGETS", "TIMESTAMP", "MULTIPLE", "ATOM_PAIR", "SAVE_TARGETS", "NULL",
                           "UTF8_STRING", "text/plain;charset=utf-8", "SCACELITH_TIME", "TEXT", "INCR",
                           "SCACELITH_SELECTION", "x-kde-passwordManagerHint", "CLIPBOARD_MANAGER"};
    constexpr int n = int(sizeof(names) / sizeof(names[0]));
    Atom a[n];
    XInternAtoms(dpy, const_cast<char**>(names), n, False, a);
    Atom* dst[n] = {&clipboard_, &targets_, &timestamp_, &multiple_, &atomPair_, &saveTargets_, &null_, &utf8_,
                    &plainUtf8_, &stamp_, &text_, &incr_, &property_, &passwordHint_, &manager_};
    for (int i = 0; i < n; ++i) *dst[i] = a[i];
    // One ChangeProperty request carries the text: its size is bounded by the server's request size
    // (BIG-REQUESTS when available), less the request's header.
    long units = XExtendedMaxRequestSize(dpy);
    if (units <= 0) units = XMaxRequestSize(dpy);
    maxProperty_ = std::min(kMaxBytes, size_t(units) * 4 - 256);
}

Time Clipboard::serverTime() {
    // Appending nothing changes nothing, but the server stamps the PropertyNotify it sends.
    static const unsigned char none = 0;
    XChangeProperty(dpy_, win_, stamp_, XA_INTEGER, 8, PropModeAppend, &none, 0);
    Wanted w{win_, None, stamp_};
    XEvent ev;
    if (waitFor(dpy_, ev, isNewValue, reinterpret_cast<XPointer>(&w), nowMs() + kTimeoutMs)) return ev.xproperty.time;
    return CurrentTime;
}

std::string Clipboard::text(Time time) {
    if (!dpy_) return {};
    const Window owner = XGetSelectionOwner(dpy_, clipboard_);
    if (owner == None) return {};
    if (owner == win_) return owner_ ? owned_ : std::string();
    for (Atom target : {utf8_, plainUtf8_, Atom(XA_STRING)}) {
        std::string out;
        switch (convert(target, time, out)) {
            case Result::Ok: return out;
            case Result::Refused: break;   // the next target
            case Result::Timeout: return {};  // no second wait for an owner that does not answer
        }
    }
    return {};
}

Clipboard::Result Clipboard::convert(Atom target, Time time, std::string& out) {
    const double start = nowMs();
    Wanted w{win_, clipboard_, property_};
    XDeleteProperty(dpy_, win_, property_);
    XConvertSelection(dpy_, clipboard_, target, property_, win_, time);
    XEvent ev;
    if (!waitFor(dpy_, ev, isNotify, reinterpret_cast<XPointer>(&w), start + kTimeoutMs)) {
        LOGW("clipboard: its owner did not answer within %d ms", kTimeoutMs);
        return Result::Timeout;
    }
    if (ev.xselection.property == None) return Result::Refused;
    // The owner wrote the property before it answered: that PropertyNotify is queued already, and
    // an INCR transfer waits for the next ones.
    while (XCheckIfEvent(dpy_, &ev, isNewValue, reinterpret_cast<XPointer>(&w))) {
    }
    Atom type = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    // Read and deleted (when read in full), at most kMaxBytes and one long more to tell what is longer.
    const long longs = long(kMaxBytes / 4 + 1);
    if (XGetWindowProperty(dpy_, win_, property_, 0, longs, True, AnyPropertyType, &type, &format, &count, &after,
                           &data) != Success)
        return Result::Refused;
    std::string bytes;
    Atom textType = type;
    if (type == incr_) {
        // INCR: deleting the property asked for the first part; each part comes as a new value of the
        // property, and a part of length 0 ends the transfer.
        if (data) XFree(data);
        for (;;) {
            const double deadline = std::min(nowMs() + kTimeoutMs, start + kTransferMs);
            if (!waitFor(dpy_, ev, isNewValue, reinterpret_cast<XPointer>(&w), deadline)) {
                LOGW("clipboard: a transfer in parts stopped (%zu bytes received)", bytes.size());
                XDeleteProperty(dpy_, win_, property_);
                return Result::Timeout;
            }
            data = nullptr;
            if (XGetWindowProperty(dpy_, win_, property_, 0, longs, True, AnyPropertyType, &type, &format, &count, &after,
                                   &data) != Success) {
                return Result::Timeout;
            }
            const bool end = count == 0;
            const bool bad = !end && (format != 8 || after != 0 || bytes.size() + count > kMaxBytes);
            if (!end && !bad) {
                bytes.append(reinterpret_cast<const char*>(data), count);
                textType = type;
            }
            if (data) XFree(data);
            if (end) break;
            if (bad) {
                // Over the limit: the owner gets no more deletions and gives up.
                LOGW("clipboard: more than %zu bytes, not read", kMaxBytes);
                XDeleteProperty(dpy_, win_, property_);
                return Result::Timeout;
            }
        }
    } else {
        const bool tooLong = after != 0;
        if (!tooLong && format == 8 && data) bytes.assign(reinterpret_cast<const char*>(data), count);
        if (data) XFree(data);
        if (tooLong) {
            LOGW("clipboard: more than %zu bytes, not read", kMaxBytes);
            XDeleteProperty(dpy_, win_, property_);
            return Result::Timeout;
        }
        if (format != 8) return Result::Refused;
    }
    out = textType == XA_STRING ? latin1ToUtf8(bytes.data(), bytes.size()) : cleanUtf8(bytes.data(), bytes.size());
    return Result::Ok;
}

bool Clipboard::setText(const std::string& utf8, bool sensitive, Time time) {
    if (!dpy_) return false;
    std::string text = cleanUtf8(utf8.data(), utf8.size());
    if (text.size() > maxProperty_) {
        LOGW("clipboard: %zu bytes, more than the %zu one copy carries", text.size(), maxProperty_);
        wipe(text);
        return false;
    }
    if (time == CurrentTime) time = serverTime();
    forget();
    XSetSelectionOwner(dpy_, clipboard_, win_, time);
    if (XGetSelectionOwner(dpy_, clipboard_) != win_) {
        wipe(text);
        return false;
    }
    owned_ = std::move(text);
    sensitive_ = sensitive;
    ownedSince_ = time;
    owner_ = true;
    return true;
}

void Clipboard::forget() {
    wipe(owned_);
    owner_ = sensitive_ = false;
}

bool Clipboard::event(const XEvent& e) {
    if (!dpy_) return false;
    if (e.type == SelectionClear && e.xselectionclear.window == win_) {
        // Another client owns the clipboard now.
        if (e.xselectionclear.selection == clipboard_) forget();
        return true;
    }
    if (e.type == SelectionRequest && e.xselectionrequest.owner == win_) {
        serve(e.xselectionrequest);
        return true;
    }
    return false;
}

void Clipboard::serve(const XSelectionRequestEvent& r) {
    XEvent reply{};
    XSelectionEvent& s = reply.xselection;
    s.type = SelectionNotify;
    s.display = r.display;
    s.requestor = r.requestor;
    s.selection = r.selection;
    s.target = r.target;
    s.time = r.time;
    s.property = None;
    // A request from before the window owned the clipboard is refused (ICCCM); an obsolete client
    // names no property, and the target is then the property.
    const bool current = owner_ && r.selection == clipboard_ && (r.time == CurrentTime || notBefore(r.time, ownedSince_));
    const Atom property = r.property != None ? r.property : r.target;
    ErrorTrap trap(dpy_);  // the requestor's window may be gone already
    if (current) {
        if (r.target == multiple_) {
            if (r.property != None && answerMultiple(r.requestor, r.property)) s.property = r.property;
        } else if (answer(r.requestor, r.target, property)) {
            s.property = property;
        }
    }
    XSendEvent(dpy_, r.requestor, False, NoEventMask, &reply);
    if (trap.failed()) LOGD("clipboard: the requesting window went away");
}

bool Clipboard::answer(Window requestor, Atom target, Atom property) {
    auto put8 = [&](Atom type, const std::string& bytes) {
        XChangeProperty(dpy_, requestor, property, type, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()));
        return true;
    };
    if (target == targets_) {
        std::vector<Atom> list{targets_, timestamp_, multiple_, saveTargets_, utf8_, plainUtf8_, text_, XA_STRING};
        if (sensitive_) list.push_back(passwordHint_);
        XChangeProperty(dpy_, requestor, property, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(list.data()), int(list.size()));
        return true;
    }
    if (target == timestamp_) {
        const long t = long(ownedSince_);
        XChangeProperty(dpy_, requestor, property, XA_INTEGER, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&t), 1);
        return true;
    }
    if (target == saveTargets_) {
        // A side-effect target with nothing to do (ICCCM): clipboard managers ask whether it is there.
        XChangeProperty(dpy_, requestor, property, null_, 32, PropModeReplace, nullptr, 0);
        return true;
    }
    if (target == utf8_ || target == plainUtf8_) return put8(target, owned_);
    if (target == text_) {
        // The owner picks the encoding: STRING when Latin-1 has every character.
        bool latin1 = false;
        const std::string l = utf8ToLatin1(owned_, &latin1);
        return latin1 ? put8(XA_STRING, l) : put8(utf8_, owned_);
    }
    if (target == XA_STRING) return put8(XA_STRING, utf8ToLatin1(owned_));
    if (target == passwordHint_ && sensitive_) return put8(XA_STRING, "secret");
    return false;
}

bool Clipboard::answerMultiple(Window requestor, Atom property) {
    // The property holds (target, property) pairs; each one refused becomes (target, None).
    Atom type = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy_, requestor, property, 0, 1024, False, AnyPropertyType, &type, &format, &count, &after,
                           &data) != Success || !data)
        return false;
    if (format != 32 || count < 2) {
        XFree(data);
        return false;
    }
    unsigned long* pairs = reinterpret_cast<unsigned long*>(data);
    for (unsigned long i = 0; i + 1 < count; i += 2)
        if (pairs[i] == multiple_ || pairs[i + 1] == None || !answer(requestor, Atom(pairs[i]), Atom(pairs[i + 1])))
            pairs[i + 1] = None;
    XChangeProperty(dpy_, requestor, property, type, 32, PropModeReplace, data, int(count & ~1UL));
    XFree(data);
    return true;
}

void Clipboard::handOver(int timeoutMs) {
    if (!dpy_ || !owner_ || sensitive_ || XGetSelectionOwner(dpy_, clipboard_) != win_) return;
    if (XGetSelectionOwner(dpy_, manager_) == None) return;
    XConvertSelection(dpy_, manager_, saveTargets_, None, win_, CurrentTime);
    Wanted w{win_, manager_, None};
    const double deadline = nowMs() + timeoutMs;
    XEvent ev;
    while (waitFor(dpy_, ev, isHandOver, reinterpret_cast<XPointer>(&w), deadline)) {
        if (ev.type == SelectionNotify) return;  // the manager has its copy (or declined)
        event(ev);
    }
    LOGW("clipboard: the clipboard manager did not take the text within %d ms", timeoutMs);
}

// ---- Keyboard ---------------------------------------------------------------------------------
bool setTextLocale() {
    auto utf8 = []() {
        const char* cs = nl_langinfo(CODESET);
        return cs && (strcasecmp(cs, "UTF-8") == 0 || strcasecmp(cs, "utf8") == 0);
    };
    if (!std::setlocale(LC_CTYPE, "") || !utf8()) {
        if (!std::setlocale(LC_CTYPE, "C.UTF-8") && !std::setlocale(LC_CTYPE, "en_US.UTF-8") &&
            !std::setlocale(LC_CTYPE, ""))
            std::setlocale(LC_CTYPE, "C");
    }
    return XSupportsLocale() == True;
}

Keyboard::~Keyboard() { close(); }

void Keyboard::open(Display* dpy, Window win) {
    dpy_ = dpy;
    win_ = win;
    // Held keys repeat as presses alone, without a release before each (the release tells when a
    // key goes up).
    Bool detectable = False;
    XkbSetDetectableAutoRepeat(dpy_, True, &detectable);
    readPositions();
    openInputMethod();
}

void Keyboard::close() {
    closeInputMethod();
    if (waiting_ && dpy_)
        XUnregisterIMInstantiateCallback(dpy_, nullptr, nullptr, nullptr, instantiated, reinterpret_cast<XPointer>(this));
    waiting_ = false;
}

void Keyboard::openInputMethod() {
    if (!XSupportsLocale()) {
        LOGW("X11: the locale %s is not supported: text input without an input method", std::setlocale(LC_CTYPE, nullptr));
        return;
    }
    XSetLocaleModifiers("");  // XMODIFIERS
    im_ = XOpenIM(dpy_, nullptr, nullptr, nullptr);
    if (im_) {
        if (waiting_)
            XUnregisterIMInstantiateCallback(dpy_, nullptr, nullptr, nullptr, instantiated, reinterpret_cast<XPointer>(this));
        waiting_ = false;
    } else {
        // The session's input method is not running (yet): opened when it starts; Xlib's own
        // composes the dead keys meanwhile.
        if (!waiting_)
            waiting_ = XRegisterIMInstantiateCallback(dpy_, nullptr, nullptr, nullptr, instantiated,
                                                      reinterpret_cast<XPointer>(this)) == True;
        XSetLocaleModifiers("@im=none");
        im_ = XOpenIM(dpy_, nullptr, nullptr, nullptr);
        XSetLocaleModifiers("");
    }
    if (!im_) {
        LOGW("X11: no input method: text input without composition");
        return;
    }
    XIMCallback gone{reinterpret_cast<XPointer>(this), destroyed};
    XSetIMValues(im_, XNDestroyCallback, &gone, nullptr);
    // The input method shows its pre-edit and status in windows of its own ("root" style), or has
    // none to show (Xlib's own).
    XIMStyles* styles = nullptr;
    XIMStyle style = 0;
    if (!XGetIMValues(im_, XNQueryInputStyle, &styles, nullptr) && styles) {
        for (XIMStyle want : {XIMStyle(XIMPreeditNothing | XIMStatusNothing), XIMStyle(XIMPreeditNone | XIMStatusNone)}) {
            for (unsigned short i = 0; i < styles->count_styles && !style; ++i)
                if (styles->supported_styles[i] == want) style = want;
            if (style) break;
        }
        XFree(styles);
    }
    if (style) ic_ = XCreateIC(im_, XNInputStyle, style, XNClientWindow, win_, XNFocusWindow, win_, nullptr);
    if (!ic_) {
        LOGW("X11: the input method has no usable input context: text input without composition");
        XCloseIM(im_);
        im_ = nullptr;
        return;
    }
    // The events the input method reads on the window (key releases for some).
    unsigned long filter = 0;
    XGetICValues(ic_, XNFilterEvents, &filter, nullptr);
    XWindowAttributes wa{};
    if (filter && XGetWindowAttributes(dpy_, win_, &wa)) XSelectInput(dpy_, win_, wa.your_event_mask | long(filter));
    if (focused_) XSetICFocus(ic_);
}

void Keyboard::closeInputMethod() {
    if (ic_) XDestroyIC(ic_);
    if (im_) XCloseIM(im_);
    ic_ = nullptr;
    im_ = nullptr;
}

void Keyboard::instantiated(Display*, XPointer self, XPointer) {
    // The session's input method started: it replaces Xlib's own at the next event.
    reinterpret_cast<Keyboard*>(self)->reopen_ = true;
}

void Keyboard::destroyed(XIM, XPointer self, XPointer) {
    // The input method went away (its server stopped): Xlib has freed it and its contexts.
    Keyboard* k = reinterpret_cast<Keyboard*>(self);
    k->ic_ = nullptr;
    k->im_ = nullptr;
    k->reopen_ = true;
}

void Keyboard::focus(bool focused) {
    focused_ = focused;
    // The releases of the keys held now go to another window: the next press of each key starts
    // afresh, in the layout of then.
    if (!focused) std::memset(keyOf_, 0, sizeof(keyOf_));
    if (!ic_) return;
    if (focused) XSetICFocus(ic_);
    else XUnsetICFocus(ic_);
}

void Keyboard::readPositions() {
    std::memset(position_, 0, sizeof(position_));
    XkbDescPtr desc = XkbGetMap(dpy_, 0, XkbUseCoreKbd);
    if (desc && XkbGetNames(dpy_, XkbKeyNamesMask, desc) == Success && desc->names && desc->names->keys) {
        for (int kc = desc->min_key_code; kc <= desc->max_key_code && kc < 256; ++kc) {
            const char* name = desc->names->keys[kc].name;  // four characters, not terminated
            for (const Row& row : kRows) {
                if (std::strncmp(name, row.name, 2) != 0) continue;
                const int place = (name[2] - '0') * 10 + (name[3] - '0');
                if (name[2] >= '0' && name[2] <= '9' && name[3] >= '0' && name[3] <= '9' && place >= 1 &&
                    place <= int(std::strlen(row.keys)))
                    position_[kc] = row.keys[place - 1];
            }
        }
    } else {
        for (size_t r = 0; r < sizeof(kRows) / sizeof(kRows[0]); ++r)
            for (size_t i = 0; kRows[r].keys[i]; ++i) position_[kEvdevFirst[r] + int(i)] = kRows[r].keys[i];
    }
    if (desc) XkbFreeKeyboard(desc, 0, True);
}

int Keyboard::shortcutKey(unsigned keycode, unsigned state) const {
    if (keycode == 0 || keycode > 255 || !dpy_) return KEY_UNKNOWN;
    const int group = int(XkbGroupForCoreState(state));
    KeySym ks = XkbKeycodeToKeysym(dpy_, KeyCode(keycode), group, 0);
    if (ks == NoSymbol && group) ks = XkbKeycodeToKeysym(dpy_, KeyCode(keycode), 0, 0);
    const int key = mapKeysym(ks);
    if (key != KEY_UNKNOWN) return key;
    // Windows' virtual-key codes: the digit row is 1..0 on every layout, and the letters of another
    // script are the US letters at their place.
    const char pos = position_[keycode];
    if (pos >= '0' && pos <= '9') return pos;
    return pos && otherScript(ks) ? int(pos) : int(KEY_UNKNOWN);
}

bool Keyboard::event(XEvent& e, Input& in) {
    if (reopen_ && dpy_) {
        reopen_ = false;
        closeInputMethod();
        openInputMethod();
    }
    const bool isKey = e.type == KeyPress || e.type == KeyRelease;
    const unsigned keycode = isKey ? e.xkey.keycode : 0;  // some input methods clear it in XFilterEvent
    const bool filtered = XFilterEvent(&e, None) == True;
    if (e.type == MappingNotify) {
        if (e.xmapping.request == MappingKeyboard || e.xmapping.request == MappingModifier) {
            XRefreshKeyboardMapping(&e.xmapping);
            readPositions();
        }
        return true;
    }
    if (!isKey) return filtered;
    if (e.type == KeyPress) {
        if (keycode > 0 && keycode < 256) {
            // An input method that does not use a key sends it back: the same time tells it apart.
            const uint32_t t = uint32_t(e.xkey.time), last = pressTime_[keycode];
            const bool again = ic_ && t != 0 && last != 0 && !(t - last != 0 && t - last < 0x80000000u);
            if (!again) {
                pressTime_[keycode] = t;
                // An auto-repeat of a key still down reports the key its first press did, though
                // the layout may have changed since: the release then lets that key go.
                if (!keyOf_[keycode] || !in.keyDown[keyOf_[keycode]])
                    keyOf_[keycode] = shortcutKey(keycode, e.xkey.state);
                setKey(in, keyOf_[keycode], true);
            }
        }
        if (!filtered) typed(e.xkey, in);
        return true;
    }
    // A release followed by a press at the same time is an auto-repeat (when the server cannot
    // leave the release out): the key stays down.
    if (XEventsQueued(dpy_, QueuedAfterReading)) {
        XEvent next;
        XPeekEvent(dpy_, &next);
        if (next.type == KeyPress && next.xkey.time == e.xkey.time && next.xkey.keycode == keycode) return true;
    }
    if (keycode > 0 && keycode < 256) {
        // The key its press reported, whatever the layout or the modifiers are now.
        const int key = keyOf_[keycode] ? keyOf_[keycode] : shortcutKey(keycode, e.xkey.state);
        keyOf_[keycode] = 0;
        setKey(in, key, false);
    }
    return true;
}

void Keyboard::typed(XKeyEvent& ev, Input& in) {
    // Shortcuts (Ctrl+V, Alt+...) type nothing, as on Windows; a text the input method commits
    // (keycode 0) always counts.
    const bool shortcut = ev.keycode != 0 && (ev.state & (ControlMask | Mod1Mask));
    if (ic_) {
        char buf[256];
        KeySym ks = NoSymbol;
        Status status = 0;
        int n = Xutf8LookupString(ic_, &ev, buf, int(sizeof(buf)), &ks, &status);
        std::string big;
        const char* s = buf;
        if (status == XBufferOverflow) {
            if (n <= 0 || n > kMaxCommit) return;  // far more than one frame's text holds
            big.resize(size_t(n));
            n = Xutf8LookupString(ic_, &ev, &big[0], n, &ks, &status);
            s = big.data();
        }
        if (shortcut || (status != XLookupChars && status != XLookupBoth) || n <= 0) return;
        for (size_t i = 0; i < size_t(n);) addText(in, nextUtf8(s, size_t(n), i));
        return;
    }
    // Without an input method: Latin-1 keysyms are their code point, and 0x01xxxxxx keysyms carry
    // one (xdotool type).
    char txt[16] = {};
    KeySym ks = NoSymbol;
    const int len = XLookupString(&ev, txt, int(sizeof(txt)), &ks, nullptr);
    uint32_t cp = 0;
    if ((ks >= 0x20 && ks <= 0x7E) || (ks >= 0xA0 && ks <= 0xFF)) cp = uint32_t(ks);
    else if ((ks & 0xFF000000UL) == 0x01000000UL) cp = uint32_t(ks & 0x00FFFFFFUL);
    if (len == 1 && static_cast<unsigned char>(txt[0]) >= 32 && static_cast<unsigned char>(txt[0]) < 127)
        cp = static_cast<unsigned char>(txt[0]);
    if (!shortcut && cp) addText(in, cp);
}

bool focusEvent(const XEvent& e, Window win, Keyboard& keyboard, Input& in, bool& focused) {
    if ((e.type != FocusIn && e.type != FocusOut) || e.xfocus.window != win) return false;
    // NotifyPointer: the focus is on the root window and the pointer is over this one, which is no
    // change of this window's focus.
    if (e.xfocus.detail == NotifyPointer) return true;
    focused = e.type == FocusIn;
    keyboard.focus(focused);
    if (!focused) releaseAll(in);
    return true;
}

// ---- Fullscreen -------------------------------------------------------------------------------
void Fullscreen::attach(Display* dpy, Window win) {
    dpy_ = dpy;
    win_ = win;
    state_ = XInternAtom(dpy, "_NET_WM_STATE", False);
    fullscreen_ = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    motifHints_ = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
}

bool Fullscreen::windowManagerSupportsFullscreen(Display* dpy) {
    const Window root = DefaultRootWindow(dpy);
    const Atom check = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    auto checkWindow = [&](Window w, Window& out) {
        Atom type = None;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        bool ok = XGetWindowProperty(dpy, w, check, 0, 1, False, XA_WINDOW, &type, &format, &count, &after, &data) ==
                      Success &&
                  data && format == 32 && count == 1;
        if (ok) out = Window(reinterpret_cast<unsigned long*>(data)[0]);
        if (data) XFree(data);
        return ok;
    };
    // The window manager's check window names itself; one left behind by a window manager that
    // has exited is gone (BadWindow) or names nothing.
    Window wm = None, self = None;
    if (!checkWindow(root, wm) || wm == None) return false;
    {
        ErrorTrap trap(dpy);
        const bool ok = checkWindow(wm, self);
        if (trap.failed() || !ok || self != wm) return false;
    }
    Atom type = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    const Atom supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
    const Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    const Atom fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    bool hasState = false, hasFullscreen = false;
    if (XGetWindowProperty(dpy, root, supported, 0, 4096, False, XA_ATOM, &type, &format, &count, &after, &data) ==
            Success &&
        data && format == 32) {
        const unsigned long* atoms = reinterpret_cast<const unsigned long*>(data);
        for (unsigned long i = 0; i < count; ++i) {
            hasState |= atoms[i] == state;
            hasFullscreen |= atoms[i] == fullscreen;
        }
    }
    if (data) XFree(data);
    return hasState && hasFullscreen;
}

void Fullscreen::start() {
    wanted_ = true;
    managed_ = windowManagerSupportsFullscreen(dpy_);
    pendingSize_ = false;
    if (managed_) {
        // Read by the window manager when it maps the window.
        XChangeProperty(dpy_, win_, state_, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&fullscreen_), 1);
        active_ = true;
        return;
    }
    decorate(false);
    XMoveResizeWindow(dpy_, win_, 0, 0, unsigned(DisplayWidth(dpy_, DefaultScreen(dpy_))),
                      unsigned(DisplayHeight(dpy_, DefaultScreen(dpy_))));
    active_ = true;
}

void Fullscreen::set(bool on, int w, int h) {
    wanted_ = on;
    managed_ = windowManagerSupportsFullscreen(dpy_);
    if (!managed_) {
        // No window manager to ask: the window covers the screen, or takes its windowed size back.
        pendingSize_ = false;
        decorate(!on);
        if (on) {
            XMoveResizeWindow(dpy_, win_, 0, 0, unsigned(DisplayWidth(dpy_, DefaultScreen(dpy_))),
                              unsigned(DisplayHeight(dpy_, DefaultScreen(dpy_))));
            XRaiseWindow(dpy_, win_);
        } else {
            XResizeWindow(dpy_, win_, unsigned(std::max(w, 1)), unsigned(std::max(h, 1)));
        }
        active_ = on;
        XFlush(dpy_);
        return;
    }
    if (on) {
        pendingSize_ = false;
        ask(true);
        return;
    }
    // The window manager restores the geometry the window had before; the windowed size asked for
    // follows once it has left fullscreen.
    pendingW_ = std::max(w, 1);
    pendingH_ = std::max(h, 1);
    if (active_) {
        pendingSize_ = true;
        ask(false);
    } else {
        pendingSize_ = false;
        XResizeWindow(dpy_, win_, unsigned(pendingW_), unsigned(pendingH_));
        XFlush(dpy_);
    }
}

void Fullscreen::ask(bool on) {
    // A window the window manager does not manage yet (never mapped, withdrawn): it reads the
    // property when it maps the window. A managed one: a message to the root window (EWMH).
    const Atom wmState = XInternAtom(dpy_, "WM_STATE", False);
    Atom type = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    bool managedNow = false;
    if (XGetWindowProperty(dpy_, win_, wmState, 0, 2, False, wmState, &type, &format, &count, &after, &data) == Success &&
        data && format == 32 && count >= 1)
        managedNow = reinterpret_cast<const unsigned long*>(data)[0] != WithdrawnState;
    if (data) XFree(data);
    if (!managedNow) {
        XChangeProperty(dpy_, win_, state_, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&fullscreen_),
                        on ? 1 : 0);
        active_ = on;
        if (!on && pendingSize_) {
            pendingSize_ = false;
            XResizeWindow(dpy_, win_, unsigned(pendingW_), unsigned(pendingH_));
        }
        XFlush(dpy_);
        return;
    }
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win_;
    ev.xclient.message_type = state_;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = on ? 1 : 0;  // _NET_WM_STATE_ADD, _NET_WM_STATE_REMOVE
    ev.xclient.data.l[1] = long(fullscreen_);
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 1;  // from an application
    XSendEvent(dpy_, DefaultRootWindow(dpy_), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy_);
}

void Fullscreen::decorate(bool on) {
    // Motif hints, which window managers without EWMH fullscreen read: decorations or none.
    const unsigned long hints[5] = {2 /* MWM_HINTS_DECORATIONS */, 0, on ? 1UL : 0UL, 0, 0};
    XChangeProperty(dpy_, win_, motifHints_, motifHints_, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(hints), 5);
}

void Fullscreen::readState() {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    bool on = false;
    if (XGetWindowProperty(dpy_, win_, state_, 0, 64, False, XA_ATOM, &type, &format, &count, &after, &data) == Success &&
        data && format == 32) {
        const unsigned long* atoms = reinterpret_cast<const unsigned long*>(data);
        for (unsigned long i = 0; i < count; ++i) on |= atoms[i] == fullscreen_;
    }
    if (data) XFree(data);
    const bool left = active_ && !on;
    active_ = on;
    if (left && pendingSize_) {
        pendingSize_ = false;
        XResizeWindow(dpy_, win_, unsigned(pendingW_), unsigned(pendingH_));
        XFlush(dpy_);
    }
}

bool Fullscreen::event(const XEvent& e) {
    if (!dpy_ || e.type != PropertyNotify || e.xproperty.window != win_ || e.xproperty.atom != state_) return false;
    if (managed_) readState();
    return true;
}

}  // namespace x11
}  // namespace plat
#endif
