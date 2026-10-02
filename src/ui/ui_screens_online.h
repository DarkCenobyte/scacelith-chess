// Internal to src/ui: the online pages (ui_screens_online.cpp, ui_online_hud.cpp) and the hooks
// they share with ui_screens.cpp.
#pragma once
#include "ui.h"
#include "ui_draw.h"
#include "../game/settings.h"
#include <string>

namespace ui {
namespace detail {

// ---- Implemented in ui_screens_online.cpp, called from ui_screens.cpp ----------------------------
// "Play Online" page of the title menu: sign-in and account pages (game history, devices, e-mail,
// data export, deletion), matchmaking, challenges, private games and the direct match. t = page
// appearance (0..1), opened = first frame on the page. Sets 'back' when the player leaves it.
// library: the saved games folder of the menu (nullptr: a game of the history can be neither saved
// nor replayed); StartReplay when a game of the history is replayed (library->replay names it).
MenuAction onlinePage(LibrarySetup* library, float t, bool opened, bool& back);
// Every frame of the main menu, after the page: challenge cards (any page, once signed in) and
// the ping indicator (on the online page).
void onlineMenuOverlay(bool onOnlinePage);
// A game announced by the server or the direct match starts from the online page: the menu
// comes back to it after the game.
bool onlineGameStarting();
// Options > Online: the rows of the tab from y (advanced past them) in the column [rx, rx + rw].
void onlineOptionsRows(game::Settings& s, float rx, float rw, float& y);
void copyOnlineOptions(game::Settings& dst, const game::Settings& src);
bool sameOnlineOptions(const game::Settings& a, const game::Settings& b);
// The server settings changed (applied): the online session selects the new server.
bool onlineServerChanged(const game::Settings& before, const game::Settings& after);

// ---- Implemented in ui_screens.cpp, used by ui_screens_online.cpp ---------------------------------
// Opens Options on a tab; closing it comes back to the online page.
void openOptionsOnTab(int tab);
constexpr int kOnlineOptionsTab = 5;   // Display, Graphics, Audio, Gameplay, Player, Online, Controls

// ---- ui_qr.cpp / ui_clipboard.cpp ----------------------------------------------------------------
// QR code of 'text' (byte mode, error correction M) drawn dark on light paper in 'r' (square,
// with its quiet zone). False when the text does not fit a QR code.
bool drawQrCode(const std::string& text, const gfx::Rect& r);
// Forgets the last code drawn (the two-factor secret is in its text).
void clearQrCache();
// Puts UTF-8 text on the system clipboard (Windows). False where unavailable (X11 test builds).
// 'sensitive' (recovery codes) keeps it out of Windows' clipboard history and cloud clipboard.
bool setClipboardText(const std::string& text, bool sensitive = false);

}  // namespace detail

namespace debug {
// Viewer: opens the online page on a sub-page ("signin", "register", "check-email", "forgot",
// "mfa", "account", "password", "mfa-setup", "mfa-off", "recovery", "play", "search",
// "challenge", "private", "direct", "direct-host", "direct-wait", "direct-join", "noserver";
// the account API's pages: "history", "game", "game-gif", "game-gif-making", "game-saving",
// "game-saved", "devices", "email", "email-sent", "export", "export-done", "delete").
void openOnlinePage(const std::string& sub);
}  // namespace debug

}  // namespace ui
