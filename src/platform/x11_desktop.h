// The desktop side of the X11 layer (platform_x11.cpp): the CLIPBOARD selection, text typed through
// the input method, the keys of the shortcuts, focus loss and the window manager's fullscreen
// state. Apart from the GL context, so that tests/x11_desktop_tests.cpp runs it against a real X
// server (Xvfb, with and without a window manager).
#pragma once
#if !defined(_WIN32) && !defined(__APPLE__)
#include "platform.h"

#include <X11/Xlib.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace plat {
namespace x11 {

// ---- Input state ------------------------------------------------------------------------------
void setKey(Input& in, int key, bool down);
void setButton(Input& in, int button, bool down);
// Focus loss: every key and button held is released (keyReleased this frame), as the Windows layer
// does at WM_KILLFOCUS. The window sees no release that happens while another window has the
// keyboard, and the observer camera reads keyDown directly.
void releaseAll(Input& in);
// Appends a typed character to in.text: control characters (C0, DEL, C1) and what no longer fits
// are left out, as on Windows (WM_CHAR).
void addText(Input& in, uint32_t cp);

// ---- Errors -----------------------------------------------------------------------------------
// Catches the X errors of the requests made while it lives (Xlib's default handler ends the
// process): a window of another client may be gone by the time a request names it.
class ErrorTrap {
public:
    explicit ErrorTrap(Display* dpy);
    ~ErrorTrap();
    ErrorTrap(const ErrorTrap&) = delete;
    ErrorTrap& operator=(const ErrorTrap&) = delete;
    bool failed();  // an error since the trap was set (waits for the server's answers)

private:
    Display* dpy_;
    int (*previous_)(Display*, XErrorEvent*);
};

// ---- Clipboard (ICCCM CLIPBOARD selection) ----------------------------------------------------
// Reading asks the selection's owner for UTF8_STRING, then text/plain;charset=utf-8, then STRING
// (Latin-1), and takes INCR transfers. Writing makes the window the owner; the events loop passes
// SelectionRequest and SelectionClear to event(), which serves TARGETS, TIMESTAMP, MULTIPLE,
// SAVE_TARGETS, UTF8_STRING, text/plain;charset=utf-8, TEXT and STRING. The window must select
// PropertyChangeMask.
class Clipboard {
public:
    static constexpr size_t kMaxBytes = size_t(4) << 20;  // read or written at most (Windows: a million characters)
    static constexpr int kTimeoutMs = 1000;    // for each answer of the owner (a hung owner: the paste gives up)
    static constexpr int kTransferMs = 3000;   // for a whole transfer
    ~Clipboard();
    void attach(Display* dpy, Window win);
    // The text, valid UTF-8 up to its first NUL; "" when the clipboard is empty, holds no text,
    // holds more than kMaxBytes or its owner does not answer in time. 'time' is the event that
    // asked for it (CurrentTime: the server's time).
    std::string text(Time time);
    // Owns the clipboard with this text. 'sensitive' (recovery codes): the targets add
    // x-kde-passwordManagerHint "secret", so that clipboard managers (Klipper, CopyQ...) keep it out
    // of their history, and the text is not handed over to the clipboard manager at exit. False
    // when it is over what one property can carry (kMaxBytes at most) or the server refused.
    bool setText(const std::string& utf8, bool sensitive, Time time);
    bool owns() const { return owner_; }
    // A SelectionRequest or SelectionClear for this window: served or noted, true. False otherwise.
    bool event(const XEvent& e);
    // At exit: the clipboard manager (freedesktop.org ClipboardManager, CLIPBOARD_MANAGER) takes a
    // copy of non-sensitive text, which then stays on the clipboard. Waits timeoutMs at most.
    void handOver(int timeoutMs);

private:
    enum class Result { Ok, Refused, Timeout };
    Result convert(Atom target, Time time, std::string& out);
    void serve(const XSelectionRequestEvent& r);
    bool answer(Window requestor, Atom target, Atom property);
    bool answerMultiple(Window requestor, Atom property);
    void forget();
    Time serverTime();

    Display* dpy_ = nullptr;
    Window win_ = 0;
    Atom clipboard_ = 0, targets_ = 0, timestamp_ = 0, multiple_ = 0, atomPair_ = 0, saveTargets_ = 0, null_ = 0;
    Atom utf8_ = 0, plainUtf8_ = 0, stamp_ = 0, text_ = 0, incr_ = 0, property_ = 0, passwordHint_ = 0, manager_ = 0;
    size_t maxProperty_ = 0;      // the bytes one ChangeProperty request carries
    std::string owned_;           // the text while the window owns the clipboard
    bool owner_ = false, sensitive_ = false;
    Time ownedSince_ = 0;
};

// ---- Keyboard ---------------------------------------------------------------------------------
// LC_CTYPE for the input method: the user's locale when it is UTF-8, else C.UTF-8 (whose compose
// table is en_US.UTF-8's: dead keys and Compose sequences for every script). Only LC_CTYPE
// changes. True when Xlib supports the locale that results.
bool setTextLocale();

class Keyboard {
public:
    ~Keyboard();
    // Opens the input method of the session (XMODIFIERS: IBus, Fcitx...), else Xlib's own, which
    // composes dead keys and Compose sequences, and an input context on 'win' (adding the events
    // it needs to the window's mask). Without one the text comes from XLookupString (Latin-1 and
    // Unicode keysyms only). An input method that starts later is opened then.
    void open(Display* dpy, Window win);
    void close();
    bool hasInputContext() const { return ic_ != nullptr; }
    void focus(bool focused);
    // Every event goes through here first: the input method's (XFilterEvent), key presses and
    // releases (the keys and the typed text), keyboard mapping changes. True when it was one of
    // those, with nothing left to do with it.
    bool event(XEvent& e, Input& in);
    // The key a physical key stands for in shortcuts: the layout's unshifted keysym in the active
    // group (AZERTY's A key is A), else its position on a US keyboard for letters and digits (the
    // V key of a Russian or Arabic layout is still V, AZERTY's & key is 1), as Windows' virtual-key
    // codes do.
    int shortcutKey(unsigned keycode, unsigned state) const;

private:
    void openInputMethod();
    void closeInputMethod();
    void readPositions();
    void typed(XKeyEvent& ev, Input& in);
    static void instantiated(Display* dpy, XPointer self, XPointer);
    static void destroyed(XIM im, XPointer self, XPointer);

    static constexpr int kMaxCommit = 4096;  // the bytes one commit of the input method may carry

    Display* dpy_ = nullptr;
    Window win_ = 0;
    XIM im_ = nullptr;
    XIC ic_ = nullptr;
    bool focused_ = false;
    bool waiting_ = false;  // for the session's input method to start (Xlib's own composes meanwhile)
    bool reopen_ = false;   // the input method went away, or the session's started: opened again at the next event
    int keyOf_[256] = {};          // the key each keycode's press reported (its release releases it)
    uint32_t pressTime_[256] = {}; // the last press reported per keycode (input methods send some twice)
    char position_[256] = {};      // keycode -> US letter or digit at that position, 0 for none
};

// FocusIn or FocusOut of 'win': 'focused' follows it, and so does the input method. At focus
// loss every key and button held goes up (releaseAll): those released while another window has the
// keyboard (Alt+Tab, a window manager's grab) never reach this one, and a held right button ends
// the mouse capture with it. True when it was one of them.
bool focusEvent(const XEvent& e, Window win, Keyboard& keyboard, Input& in, bool& focused);

// ---- Fullscreen (EWMH _NET_WM_STATE_FULLSCREEN) -----------------------------------------------
// With a window manager that supports it, the window manager makes the window cover its monitor
// without decorations, and restores the window's geometry when it leaves. Without one (Xvfb, a
// bare X server), the window covers the screen itself. The window must select PropertyChangeMask.
class Fullscreen {
public:
    void attach(Display* dpy, Window win);
    // Before the window is mapped: fullscreen from the start (the window manager reads _NET_WM_STATE
    // when it maps the window).
    void start();
    // Fullscreen, or the windowed size w x h (applied once the window manager has left fullscreen).
    void set(bool on, int w, int h);
    // A PropertyNotify of _NET_WM_STATE on the window: active() follows it. True when it was one.
    bool event(const XEvent& e);
    bool wanted() const { return wanted_; }   // the state last asked for
    bool active() const { return active_; }   // the state the window manager reports
    bool managed() const { return managed_; } // a window manager with EWMH fullscreen, at the last request
    // A running window manager that supports _NET_WM_STATE_FULLSCREEN (_NET_SUPPORTING_WM_CHECK, _NET_SUPPORTED).
    static bool windowManagerSupportsFullscreen(Display* dpy);

private:
    void ask(bool on);
    void decorate(bool on);
    void readState();

    Display* dpy_ = nullptr;
    Window win_ = 0;
    Atom state_ = 0, fullscreen_ = 0, motifHints_ = 0;
    bool wanted_ = false, active_ = false, managed_ = false, pendingSize_ = false;
    int pendingW_ = 0, pendingH_ = 0;
};

}  // namespace x11
}  // namespace plat
#endif
