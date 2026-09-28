// Copy to the system clipboard (the direct-match invitation, recovery codes). Windows only: the
// X11 layer serves tests and screenshots, where copying reports that it is unavailable.
#include "ui_screens_online.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
#endif

namespace ui {
namespace detail {

bool setClipboardText(const std::string& text) {
#ifdef _WIN32
    // CRLF line breaks and UTF-16, as CF_UNICODETEXT expects.
    std::string crlf;
    for (char c : text) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, crlf.c_str(), int(crlf.size()), nullptr, 0);
    if (n < 0) return false;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (size_t(n) + 1) * sizeof(wchar_t));
    if (!mem) return false;
    wchar_t* dst = static_cast<wchar_t*>(GlobalLock(mem));
    if (!dst) {
        GlobalFree(mem);
        return false;
    }
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, crlf.c_str(), int(crlf.size()), dst, n);
    dst[n] = 0;
    GlobalUnlock(mem);
    // The clipboard needs an owner window for SetClipboardData to succeed after EmptyClipboard.
    HWND owner = GetActiveWindow();
    if (!owner) owner = GetForegroundWindow();
    if (!OpenClipboard(owner)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);  // on success the clipboard owns the memory
    return ok;
#else
    (void)text;
    return false;
#endif
}

}  // namespace detail
}  // namespace ui
