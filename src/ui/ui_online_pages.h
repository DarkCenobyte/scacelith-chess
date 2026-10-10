// Internal to src/ui: what the pages of "Play Online" share. The page chrome and the small helpers
// are defined in ui_screens_online.cpp (sign-in, account, play and direct match pages); the
// account API's pages (game history, a game of the history, signed-in devices, change of e-mail,
// data export, deletion of the account) live in ui_screens_account.cpp and are driven from the
// online page's dispatcher, which owns the navigation between sub-pages.
#pragma once
#include "ui.h"
#include "ui_draw.h"
#include "ui_theme.h"
#include "../game/online_session.h"
#include <string>

namespace ui {
namespace detail {
namespace onl {

// ---- ui_screens_online.cpp: helpers and page chrome ---------------------------------------------
gfx::TextStyle style(int face, float size, m::vec4 color, gfx::HAlign align = gfx::HAlign::Left, float tracking = 0.0f);
std::string T(const char* key);                  // i18n::tr
std::string L(const char* key);                  // a button label with its key as id ("Text##key")
game::OnlineSession& ses();
std::string trim(const std::string& s);
// Rotating dots (a request in flight).
void spinner(m::vec2 c, float r = 12.0f, float alpha = 1.0f);
// Centered wrapped paragraph from 'y' (baseline of the first line); returns the height used.
float paragraph(const std::string& s, const gfx::Rect& p, float y, float width, m::vec4 color = theme::ivoryDim,
                float size = theme::kBody, int face = font::FACE_ITALIC);
// The panel of a sub-page (dimmed hall, panel, title), w x h at most; endPage() after it.
// fitTitle: a title longer than the panel is drawn smaller, inside its frame.
gfx::Rect beginPage(float t, float w, float h, const std::string& title, bool fitTitle = false);
void endPage();
// The server's name and the connection state in the panel's top corner (start side).
void serverLine(const gfx::Rect& p, bool showConnection);
// A form row of the panel at y (advanced to the next row), 'inset' from both sides.
gfx::Rect formRow(const gfx::Rect& p, float& y, float inset = 90.0f);
// The footer: Back on the start side, the primary action on the end side, a rule above them.
// The primary action is the page's submit button (Enter in its last text field, im::ITEM_SUBMIT)
// unless 'submit' is false (another button of the page sends what its field holds).
constexpr float kBtnW = 260.0f, kBtnH = 56.0f;
float footerY(const gfx::Rect& p);
bool backButton(const gfx::Rect& p, const char* key = "common.back", bool enabled = true);
bool primaryButton(const gfx::Rect& p, const char* key, bool enabled, bool busy = false, float width = kBtnW,
                   bool submit = true);
void footerRule(const gfx::Rect& p);
// A quiet link-like button centered at cx.
bool linkButton(const char* key, float cx, float y, bool enabled = true);
// Label / value line of the account page (label above, value under it).
void infoLine(const std::string& label, const std::string& value, const gfx::Rect& col, float y, m::vec4 valueColor = theme::ivory);
// Sign out everywhere (the account page, signed-in devices): the sign-in page tells the server's
// answer when it comes, a refusal included (the other computers then stay signed in); a failure
// that keeps this computer's session goes back to the account page with it, to try again.
void signOutEverywhere();

// ---- ui_screens_account.cpp: helpers shared with the saved games (ui_library.cpp) ---------------
// Scroll of a clipped area by the wheel over it (and PageUp / PageDown when 'keys'); returns the offset.
float wheelScroll(float& scroll, float& target, const gfx::Rect& area, float contentH, float step, bool opened, bool keys = false);
// Scroll bar on the end side and fades at the edges (panel colour) of a scrolled area.
void scrollDecor(const gfx::Rect& area, float scroll, float contentH);
// The end of s that fits maxWidth in st ("…" and the end): for a path, whose file's name matters
// more than the folder's.
std::string elideStart(const std::string& s, const gfx::TextStyle& st, float maxWidth);

// ---- ui_screens_account.cpp: the account API's pages --------------------------------------------
enum class AccountPage { History, Game, Devices, Email, Export, Delete };

// Where the online page goes after a frame of an account page.
enum class AccountNav {
    Stay,
    Account,      // back to the account page
    History,      // back to the history (from a game)
    Game,         // a game of the history opened (accountData().gameWanted)
    SignIn        // signed out (the account was deleted): the sign-in page
};

// One frame of an account page. t = appearance, fresh = first frame on it. 'library' is the saved
// games folder of the menu (nullptr / empty folder: no Save nor Replay); Replay fills
// library->replay and sets 'act' to StartReplay. note: a line for the page navigated to.
AccountNav accountPage(AccountPage page, float t, bool fresh, LibrarySetup* library, MenuAction& act, std::string& note);
// Every frame of the online page, before its sub-page: takes the answers the account pages wait
// for (they arrive on any page). current: the account page shown, nullptr on the others. SignIn
// after the account was deleted, with its note; 'error' set when the server refused the session.
AccountNav accountPump(const AccountPage* current, std::string& note, std::string& error);
// Entering the account pages from the account page (forms and messages emptied).
void accountReset(AccountPage page);
// Viewer: the page in a given state ("history", "game", "game-gif" (its GIF saved), "game-gif-making",
// "game-saving" (its PGN on the way), "game-saved" (its PGN written to the saved games), "devices",
// "email", "email-sent", "export", "export-done", "delete"), its data fetched from the in-process
// fake server.
bool accountDebugOpen(const std::string& sub, AccountPage& page);

}  // namespace onl
}  // namespace detail
}  // namespace ui
