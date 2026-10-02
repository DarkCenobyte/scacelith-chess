// "Play Online" page of the title menu and Options > Online. One page with sub-pages:
//   - no server configured (a build without an official server): community server settings or
//     a direct match;
//   - sign in (user name or e-mail + password, Google), the two-factor code step, create an
//     account (then "check your e-mail" with Resend), forgot password, Google first login
//     (choose a user name);
//   - account, in three columns: the account (user name, e-mail and an e-mail change waiting
//     for its link, two-factor, Google, the "Accept challenges" preference, sign out here /
//     everywhere), security and data (change password, change e-mail, two-factor setup (QR
//     code + key in groups of four + code, then 10 recovery codes shown once), turn it off, new
//     recovery codes, signed-in devices, download my data, delete the account) and the ratings
//     per time control (provisional "1500?", games, W/D/L); Game history in the footer. The
//     account API's pages (history, a game of it, devices, e-mail, export, deletion) are in
//     ui_screens_account.cpp, sharing this page's chrome through ui_online_pages.h;
//   - play: the server's rated categories with the player's ratings, rated or casual, Find
//     opponent (searching card: elapsed time, rating window, Cancel), challenge a player,
//     private game (create: a code to share / join with a code), matchmaking pause and ban notices;
//   - direct match (no server, no account, never rated): host (time control, colour, port, UPnP)
//     -> the invitation to read to a friend (address, port, code, LAN addresses, router status),
//     or join (address, port, code).
// The pages drive game::onlineSession() (the only code that talks to the network layer); the
// game starts by itself when the session announces one (the scene watches gameReady()).
#include "ui.h"
#include "ui_draw.h"
#include "ui_online.h"
#include "ui_online_pages.h"
#include "ui_screens_game.h"
#include "ui_screens_online.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../chess/chess.h"
#include "../game/online_session.h"
#include "../game/settings.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ui {

using gfx::HAlign;
using gfx::Rect;
using gfx::TextStyle;
using m::vec2;
using m::vec4;
using namespace theme;

// ---- Shared with the account pages (ui_online_pages.h) -----------------------------------------------
namespace detail {
namespace onl {

TextStyle style(int face, float size, vec4 color, HAlign align, float tracking) {
    TextStyle st;
    st.face = face;
    st.size = size;
    st.color = color;
    st.align = align;
    st.tracking = tracking;
    return st;
}
std::string T(const char* key) { return i18n::tr(key); }
std::string L(const char* key) { return std::string(i18n::tr(key)) + "##" + key; }
game::OnlineSession& ses() { return game::onlineSession(); }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Rotating dots (a request in flight).
void spinner(vec2 c, float r, float alpha) {
    float t = float(im::time());
    for (int i = 0; i < 8; ++i) {
        float a = float(i) / 8.0f * 6.2831853f;
        float phase = std::fmod(t * 1.3f - float(i) / 8.0f + 8.0f, 1.0f);
        gfx::diamond(vec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), 2.6f, withAlpha(gold, (0.2f + 0.8f * phase) * alpha));
    }
}

// Centered wrapped paragraph from 'y' (baseline of the first line); returns the height used.
float paragraph(const std::string& s, const Rect& p, float y, float width, vec4 color, float size, int face) {
    if (s.empty()) return 0.0f;
    TextStyle ts = style(face, size, color, HAlign::Center);
    float lh = size * 1.35f;
    int n = gfx::textWrapped(s, p.cx(), y, width, ts, lh);
    return float(n) * lh;
}

Rect beginPage(float t, float w, float h, const std::string& title) {
    vec2 v = gfx::viewSize();
    detail::dimBackground(t);
    w = std::min(w, v.x - 80.0f);
    h = std::min(h, v.y - 40.0f);
    Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f + (1.0f - t) * 14.0f, w, h);
    gfx::pushAlpha(t);
    im::panel(p);
    im::pageTitle(title, p.cx(), p.y + 80.0f);
    return p;
}
void endPage() { gfx::popAlpha(); }

// Server and connection in the panel's top corner (start side).
void serverLine(const Rect& p, bool showConnection) {
    game::OnlineSession& s = ses();
    std::string server = s.serverName();
    if (server.empty()) return;
    std::string line = i18n::ltr(server);
    const size_t named = line.size();
    vec4 dot = withAlpha(muted, 0.8f);
    if (showConnection) {
        switch (s.conn()) {
        case net::ConnState::Online: line += "  \xC2\xB7  " + T("online.conn.online"); dot = vec4(0.45f, 0.72f, 0.42f, 1.0f); break;
        case net::ConnState::Connecting: line += "  \xC2\xB7  " + T("online.conn.connecting"); dot = withAlpha(gold, 0.9f); break;
        case net::ConnState::Reconnecting: line += "  \xC2\xB7  " + T("online.conn.reconnecting"); dot = withAlpha(gold, 0.9f); break;
        case net::ConnState::Incompatible: line += "  \xC2\xB7  " + T("online.conn.incompatible"); dot = danger; break;
        default: line += "  \xC2\xB7  " + T("online.conn.offline"); break;
        }
    }
    TextStyle ts = style(font::FACE_ITALIC, kCaption, withAlpha(muted, 0.95f), im::startAlign());
    ts.size = gfx::fitSize(line, ts, p.w * 0.3f, 0.75f);
    if (gfx::textWidth(line, ts) > p.w * 0.3f + 0.5f) {  // a long server name is cut, not the connection
        const std::string state = line.substr(named);
        line = i18n::ltr(im::elideToFit(server, ts, p.w * 0.3f - gfx::textWidth(state, ts))) + state;
    }
    // Above the page title's height, so a long title never meets it.
    gfx::diamond(vec2(im::flipX(p, p.x + 40.0f), p.y + 31.0f), 3.5f, dot);
    gfx::text(line, im::flipX(p, p.x + 54.0f), p.y + 37.0f, ts);
}

Rect formRow(const Rect& p, float& y, float inset) {
    Rect r(p.x + inset, y, p.w - 2.0f * inset, 56.0f);
    y += 64.0f;
    return r;
}

float footerY(const Rect& p) { return p.b() - 48.0f - kBtnH; }
bool backButton(const Rect& p, const char* key) {
    return im::button(L(key), im::flip(p, Rect(p.x + 60.0f, footerY(p), kBtnW, kBtnH)), im::ButtonKind::Secondary);
}
bool primaryButton(const Rect& p, const char* key, bool enabled, bool busy) {
    Rect r = im::flip(p, Rect(p.r() - 60.0f - kBtnW, footerY(p), kBtnW, kBtnH));
    im::Id id = im::makeId(std::string("##") + key);
    bool hit = im::button(L(key), r, im::ButtonKind::Primary, enabled && !busy);
    if (busy) spinner(vec2(im::flipX(p, r.x - 34.0f), r.cy()));
    im::setDefaultFocus(id);
    return hit && enabled && !busy;
}
void footerRule(const Rect& p) {
    gfx::hlineFade(p.x + 40.0f, p.r() - 40.0f, footerY(p) - 26.0f, withAlpha(gold, 0.25f), 0.3f);
}

// A quiet link-like button, centered at cx.
bool linkButton(const char* key, float cx, float y, bool enabled) {
    TextStyle qs = style(font::FACE_ITALIC, kSmall, muted);
    float w = std::max(160.0f, gfx::textWidth(T(key), qs) + 40.0f);
    return im::button(L(key), Rect(cx - w * 0.5f, y, w, 44.0f), im::ButtonKind::Quiet, enabled);
}

// Label / value line of the account page.
void infoLine(const std::string& label, const std::string& value, const Rect& col, float y, vec4 valueColor) {
    TextStyle ls = style(font::FACE_TITLE, 17.0f, withAlpha(gold, 0.9f), im::startAlign(), 0.18f);
    gfx::text(label, im::flipX(col, col.x), y, ls);
    TextStyle vs = style(font::FACE_TEXT, kBody, valueColor, im::startAlign());
    vs.size = gfx::fitSize(value, vs, col.w - 10.0f, 0.7f);
    gfx::text(im::elideToFit(value, vs, col.w - 10.0f), im::flipX(col, col.x), y + 34.0f, vs);
}

}  // namespace onl
}  // namespace detail

namespace {

using namespace detail::onl;
using detail::baseTimeValues;
using detail::clockText;
using detail::nearestIndex;
using detail::spacedPlus;
using Kind = net::Event::Kind;

// ---- Helpers ---------------------------------------------------------------------------------------
float ease(float t) { return m::smootherstep(t); }
vec2 view() { return gfx::viewSize(); }

std::string tcLabel(int baseSec, int incSec) {
    std::string base = baseSec % 60 == 0 ? std::to_string(baseSec / 60) : [&] {
        char b[16];
        std::snprintf(b, sizeof b, "%d:%02d", baseSec / 60, baseSec % 60);
        return std::string(b);
    }();
    return base + "+" + std::to_string(incSec);
}
std::string ratingText(const net::RatingInfo* r) {
    if (!r) return "1500?";
    return std::to_string(r->rating) + (r->provisional ? "?" : "");
}
std::string digitsOnly(const std::string& s) {
    std::string r;
    for (char c : s)
        if (c >= '0' && c <= '9') r += c;
    return r;
}
std::string upperCode(const std::string& s) {
    std::string r;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-') r += char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}
// "ABCD EFGH IJKL ..." (the two-factor key, easier to type by hand).
std::string groupsOf4(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && i % 4 == 0) r += ' ';
        r += s[i];
    }
    return r;
}
// An address the way a player types it: IPv6 in brackets before ":port".
std::string hostPort(const std::string& host, int port) {
    bool v6 = host.find(':') != std::string::npos;
    return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}
bool isOfficialCategory(int baseSec, int incSec) {
    for (const net::Category& c : ses().info().categories)
        if (c.baseSec == baseSec && c.incSec == incSec) return true;
    return false;
}

// ---- State -----------------------------------------------------------------------------------------
enum class Sub {
    NoServer, SignIn, Mfa, Register, CheckEmail, Forgot, SsoWait, SsoName,
    Account, Password, MfaSetup, MfaOff, Recovery,
    History, Game, Devices, Email, Export, Delete,   // the account API's pages (ui_screens_account.cpp)
    Play, Challenge, Private,
    Direct, DirectHost, DirectWait, DirectJoin
};

// The account API's pages and their sub-pages here.
bool accountSub(Sub s, AccountPage& page) {
    switch (s) {
    case Sub::History: page = AccountPage::History; return true;
    case Sub::Game: page = AccountPage::Game; return true;
    case Sub::Devices: page = AccountPage::Devices; return true;
    case Sub::Email: page = AccountPage::Email; return true;
    case Sub::Export: page = AccountPage::Export; return true;
    case Sub::Delete: page = AccountPage::Delete; return true;
    default: return false;
    }
}
Sub subOf(AccountPage page) {
    switch (page) {
    case AccountPage::History: return Sub::History;
    case AccountPage::Game: return Sub::Game;
    case AccountPage::Devices: return Sub::Devices;
    case AccountPage::Email: return Sub::Email;
    case AccountPage::Export: return Sub::Export;
    case AccountPage::Delete: return Sub::Delete;
    }
    return Sub::Account;
}

bool needsAccount(Sub s) {
    AccountPage page;
    return s == Sub::Account || s == Sub::Password || s == Sub::MfaSetup || s == Sub::MfaOff || s == Sub::Recovery ||
           s == Sub::Play || s == Sub::Challenge || s == Sub::Private || accountSub(s, page);
}
bool isDirect(Sub s) { return s == Sub::Direct || s == Sub::DirectHost || s == Sub::DirectWait || s == Sub::DirectJoin; }

// Terms of a challenge or a private game.
struct Terms {
    int tc = 0;              // index into the server's categories, -1 = custom
    int baseSec = 600, incSec = 5;
    bool rated = true;
    int color = 0;           // 0 random, 1 White, 2 Black
};

struct State {
    Sub sub = Sub::SignIn;
    bool fresh = true;
    float t = 0.0f;
    Sub afterGame = Sub::SignIn;
    bool haveAfterGame = false;
    std::string forced;
    bool leave = false;
    // forms
    std::string user, email, password, password2, newPassword, code, ssoName, target, joinCode;
    std::string error, note;
    bool offerResend = false;
    std::string resendEmail;
    // two-factor
    int mfaStep = 0;
    std::string mfaSecret, mfaUri;
    std::vector<std::string> codes;
    // challenge / private game
    Terms terms;
    bool privateCreated = false;
    // direct match
    std::string directAddress, directPort, directCode, hostPortText;
    bool directJoining = false;
    float copiedAt = -100.0f;
    // Options > Online
    bool testShown = false, testOk = false;
    std::string testLine, testDetail;
};
State O;

void setSub(Sub s) {
    O.sub = s;
    O.fresh = true;
    O.t = 0.0f;
    O.error.clear();
    O.note.clear();
    O.offerResend = false;
}

void clearSecrets() {
    O.password.clear();
    O.password2.clear();
    O.newPassword.clear();
    O.code.clear();
}

// An account API page opened from the account page: its forms and messages start empty.
void openAccountPage(Sub sub) {
    AccountPage page;
    if (accountSub(sub, page)) accountReset(page);
    setSub(sub);
}

// ---- Page chrome -------------------------------------------------------------------------------------
// Error (red) or note (ivory) under a form, centered; returns the height used.
float messageLine(const Rect& p, float y) {
    if (!O.error.empty()) return paragraph(O.error, p, y, p.w - 200.0f, danger, kSmall + 1.0f);
    if (!O.note.empty()) return paragraph(O.note, p, y, p.w - 200.0f, ivoryDim, kSmall + 1.0f);
    return 0.0f;
}

// A large choice: title and a one-line description (Play page, direct match).
bool choiceRow(const char* key, const char* descKey, const Rect& r, bool enabled = true) {
    im::Item it = im::item(im::makeId(std::string("##") + key), r, enabled ? im::ITEM_FOCUSABLE : im::ITEM_DISABLED);
    gfx::pushAlpha(enabled ? 1.0f : 0.45f);
    gfx::fill(r, vec4(0, 0, 0, 0.22f), 2.0f);
    gfx::fill(r, withAlpha(gold, 0.07f * it.hoverT), 2.0f);
    gfx::stroke(r, withAlpha(gold, 0.16f + 0.45f * it.hoverT), 0.0f, 2.0f);
    TextStyle ts = style(font::FACE_TITLE, 23.0f, theme::mix(ivory, goldBright, it.hoverT), im::startAlign(), 0.12f);
    ts.size = gfx::fitSize(T(key), ts, r.w - 70.0f);
    gfx::text(T(key), im::flipX(r, r.x + 26.0f), r.y + 38.0f, ts);
    TextStyle ds = style(font::FACE_ITALIC, kSmall, ivoryDim, im::startAlign());
    ds.size = gfx::fitSize(T(descKey), ds, r.w - 70.0f, 0.75f);
    gfx::text(T(descKey), im::flipX(r, r.x + 26.0f), r.y + 70.0f, ds);
    gfx::diamond(vec2(im::flipX(r, r.r() - 30.0f), r.cy()), 4.0f, withAlpha(gold, 0.4f + 0.5f * it.hoverT));
    gfx::popAlpha();
    if (it.activated && enabled) im::sound(Sound::Open);
    return it.activated && enabled;
}

// Time control tile (the New Game page's look): label, and a second line (rating, category).
bool tcTile(int id, const Rect& r, const std::string& label, const std::string& sub, bool sel, bool enabled = true) {
    im::Item it = im::item(im::makeId(id), r, enabled ? im::ITEM_FOCUSABLE : im::ITEM_DISABLED);
    gfx::pushAlpha(enabled ? 1.0f : 0.45f);
    gfx::fill(r, vec4(0, 0, 0, 0.25f), 2.0f);
    if (sel) {
        gfx::fillV(r, withAlpha(gold, 0.16f), withAlpha(gold, 0.06f), 2.0f);
        gfx::stroke(r, withAlpha(gold, 0.7f), 0.0f, 2.0f);
    } else {
        gfx::fill(r, withAlpha(gold, 0.08f * it.hoverT), 2.0f);
        gfx::stroke(r, withAlpha(gold, 0.18f + 0.4f * it.hoverT), 0.0f, 2.0f);
    }
    TextStyle ls = style(font::FACE_TEXT, 27.0f, sel ? goldBright : theme::mix(ivory, goldBright, it.hoverT * 0.6f), HAlign::Center);
    ls.size = gfx::fitSize(label, ls, r.w - 12.0f);
    gfx::text(label, r.cx(), r.y + 32.0f, ls);
    TextStyle cs = style(font::FACE_ITALIC, 18.0f, sel ? gold : muted, HAlign::Center);
    cs.size = gfx::fitSize(sub, cs, r.w - 10.0f);
    gfx::text(sub, r.cx(), r.y + 54.0f, cs);
    gfx::popAlpha();
    bool hit = it.activated && enabled && !sel;
    if (hit) im::sound(Sound::Toggle);
    return hit;
}

// ---- Result handling (HTTPS answers of the pages) ----------------------------------------------------
void pumpResults() {
    game::OnlineSession& s = ses();
    net::Event e;
    if (s.take(Kind::LoginResult, e)) {
        if (e.ok) {
            clearSecrets();
            setSub(Sub::Play);
        } else if (e.mfaRequired) {
            O.code.clear();
            setSub(Sub::Mfa);
        } else {
            Sub back = O.sub == Sub::SsoName ? Sub::SsoName
                       : (O.sub == Sub::Mfa && e.error == "invalid_code") ? Sub::Mfa
                                                                           : Sub::SignIn;
            if (O.sub != back) setSub(back);
            O.error = game::onlineErrorText(e.error, e.retryAfterSec, e.account.bannedUntilMs);
            O.offerResend = e.error == "email_unverified";
        }
    }
    if (s.take(Kind::SsoBrowserOpened, e) && !e.ok) {
        setSub(Sub::SignIn);
        O.error = game::onlineErrorText(e.error, e.retryAfterSec);
    }
    if (s.take(Kind::SsoNeedsUsername, e)) {
        O.ssoName.clear();
        setSub(Sub::SsoName);
    }
    if (s.take(Kind::RegisterResult, e)) {
        if (e.ok) {
            O.password.clear();
            O.password2.clear();
            O.resendEmail = O.email;
            if (s.info().emailVerification || !s.infoKnown()) {
                setSub(Sub::CheckEmail);
            } else {
                setSub(Sub::SignIn);
                O.note = T("online.register.ready");
            }
        } else {
            O.error = game::onlineErrorText(e.error, e.retryAfterSec);
        }
    }
    if (s.take(Kind::VerificationResent, e)) {
        if (e.ok) O.note = T("online.check_email.resent"), O.error.clear();
        else O.error = game::onlineErrorText(e.error, e.retryAfterSec);
    }
    if (s.take(Kind::PasswordResetRequested, e)) {
        if (e.ok) O.note = T("online.forgot.sent"), O.error.clear();
        else O.error = game::onlineErrorText(e.error, e.retryAfterSec);
    }
    if (s.take(Kind::PasswordChanged, e)) {
        if (e.ok) {
            clearSecrets();
            setSub(Sub::Account);
            O.note = T("online.password.changed");
        } else {
            O.error = game::onlineErrorText(e.error, e.retryAfterSec);
        }
    }
    if (s.take(Kind::MfaSetupResult, e)) {
        if (e.ok) {
            O.mfaSecret = e.mfaSecret;
            O.mfaUri = e.mfaUri;
            O.mfaStep = 1;
            clearSecrets();
            O.error.clear();
        } else {
            O.error = game::onlineErrorText(e.error, e.retryAfterSec);
        }
    }
    if (s.take(Kind::MfaEnableResult, e)) {
        if (e.ok) {
            O.codes = e.recoveryCodes;
            O.mfaStep = 2;
            O.mfaSecret.clear();
            O.mfaUri.clear();
            clearSecrets();
            O.error.clear();
        } else {
            O.error = game::onlineErrorText(e.error, e.retryAfterSec);
        }
    }
    if (s.take(Kind::MfaDisableResult, e)) {
        if (e.ok) {
            clearSecrets();
            setSub(Sub::Account);
            O.note = T("online.mfa.off_done");
        } else {
            O.error = game::onlineErrorText(e.error, e.retryAfterSec);
        }
    }
    if (s.take(Kind::RecoveryCodesResult, e)) {
        if (e.ok) {
            O.codes = e.recoveryCodes;
            O.mfaStep = 2;
            clearSecrets();
            O.error.clear();
        } else {
            O.error = game::onlineErrorText(e.error, e.retryAfterSec);
        }
    }
    if (s.take(Kind::AccountResult, e) && !e.ok && e.error != "unauthorized" && O.sub == Sub::Account)
        O.error = game::onlineErrorText(e.error, e.retryAfterSec);
    s.take(Kind::LogoutResult, e);
}

// ---- Sub-pages: server, sign in ------------------------------------------------------------------------
void pageNoServer(float t) {
    Rect p = beginPage(t, 980.0f, 600.0f, T("online.title"));
    im::pushId("noserver");
    float y = p.y + 160.0f;
    y += paragraph(T("online.no_server.text"), p, y, p.w - 200.0f, ivoryDim) + 30.0f;
    float bw = 380.0f;
    if (im::button(L("online.no_server.settings"), Rect(p.cx() - bw * 0.5f, y, bw, kBtnH), im::ButtonKind::Primary))
        detail::openOptionsOnTab(detail::kOnlineOptionsTab);
    y += 80.0f;
    if (im::button(L("online.direct.button"), Rect(p.cx() - bw * 0.5f, y, bw, kBtnH), im::ButtonKind::Secondary)) {
        setSub(Sub::Direct);
    }
    footerRule(p);
    if (backButton(p)) O.leave = true;
    im::popId();
    endPage();
    if (im::consumeBack()) O.leave = true;
}

// Server not reachable / incompatible: a line and Try again. Returns true when the server can be used.
bool serverStatus(const Rect& p, float& y) {
    game::OnlineSession& s = ses();
    if (s.infoKnown() && !s.info().compatible) {
        y += paragraph(T("online.err.incompatible"), p, y, p.w - 200.0f, danger, kSmall + 1.0f) + 8.0f;
        return false;
    }
    if (!s.infoKnown() && !s.infoError().empty() && !s.busy(Kind::ServerInfoResult)) {
        y += paragraph(game::onlineErrorText(s.infoError()), p, y, p.w - 200.0f, danger, kSmall + 1.0f);
        if (linkButton("online.retry", p.cx(), y - 8.0f)) s.refreshInfo();
        y += 44.0f;
        return false;
    }
    return true;
}

void pageSignIn(float t) {
    game::OnlineSession& s = ses();
    Rect p = beginPage(t, 1000.0f, 820.0f, T("online.title"));
    serverLine(p, false);
    im::pushId("signin");
    float y = p.y + 150.0f;
    y += paragraph(i18n::trf("online.signin.lead", {i18n::ltr(s.serverName())}), p, y, p.w - 200.0f, ivoryDim, kSmall + 2.0f) + 14.0f;
    bool usable = serverStatus(p, y);
    bool busy = s.busy(Kind::LoginResult);
    im::formField(L("online.field.user"), O.user, formRow(p, y), 64, im::FIELD_LTR, T("online.field.user.hint"));
    im::formField(L("online.field.password"), O.password, formRow(p, y), 128, im::FIELD_SECRET);
    y += 8.0f;
    y += messageLine(p, y + 16.0f);
    if (O.offerResend && linkButton("online.check_email.resend", p.cx(), y + 2.0f, !s.busy(Kind::VerificationResent))) {
        std::string mail = O.user.find('@') != std::string::npos ? O.user : O.resendEmail;
        if (!mail.empty()) {
            s.api().resendVerification(mail);
            s.expect(Kind::VerificationResent);
        } else {
            O.resendEmail.clear();
            setSub(Sub::CheckEmail);
        }
    }
    y += O.offerResend ? 50.0f : 10.0f;
    // Other ways in.
    float lx = p.cx();
    bool sso = s.infoKnown() && s.info().googleSso;
    float linkY = std::max(y + 6.0f, footerY(p) - 150.0f);
    if (sso) {
        float gw = 380.0f;
        if (im::button(L("online.signin.google"), Rect(lx - gw * 0.5f, linkY, gw, 52.0f), im::ButtonKind::Secondary, usable && !busy)) {
            s.api().startGoogleSso();
            s.expect(Kind::SsoNeedsUsername);
            setSub(Sub::SsoWait);
        }
        linkY += 66.0f;
    }
    bool canRegister = !s.infoKnown() || s.info().registrationOpen;
    float third = (p.w - 200.0f) / 3.0f;
    if (linkButton("online.signin.register", p.x + 100.0f + third * 0.5f, linkY, canRegister && usable)) setSub(Sub::Register);
    if (linkButton("online.signin.forgot", p.cx(), linkY, usable)) {
        if (O.user.find('@') != std::string::npos) O.email = O.user;
        setSub(Sub::Forgot);
    }
    if (linkButton("online.direct.button", p.r() - 100.0f - third * 0.5f, linkY)) setSub(Sub::Direct);
    footerRule(p);
    if (backButton(p)) O.leave = true;
    if (primaryButton(p, "online.signin.button", usable && !trim(O.user).empty() && !O.password.empty(), busy)) {
        O.error.clear();
        O.note.clear();
        s.api().login(trim(O.user), O.password);
        s.expect(Kind::LoginResult);
    }
    im::popId();
    endPage();
    if (im::consumeBack()) O.leave = true;
}

void pageMfa(float t) {
    game::OnlineSession& s = ses();
    Rect p = beginPage(t, 940.0f, 600.0f, T("online.mfa.title"));
    serverLine(p, false);
    im::pushId("mfa");
    float y = p.y + 160.0f;
    y += paragraph(T("online.mfa.lead"), p, y, p.w - 200.0f, ivoryDim, kSmall + 2.0f) + 14.0f;
    bool busy = s.busy(Kind::LoginResult);
    im::formField(L("online.field.code"), O.code, formRow(p, y), 14, im::FIELD_LTR, T("online.field.code.hint"));
    messageLine(p, y + 20.0f);
    footerRule(p);
    bool back = backButton(p);
    if (primaryButton(p, "online.mfa.verify", trim(O.code).size() >= 6, busy)) {
        O.error.clear();
        s.api().loginMfa(trim(O.code));
        s.expect(Kind::LoginResult);
    }
    im::popId();
    endPage();
    if (back || im::consumeBack()) {
        O.code.clear();
        setSub(Sub::SignIn);
    }
}

void pageRegister(float t) {
    game::OnlineSession& s = ses();
    Rect p = beginPage(t, 1000.0f, 880.0f, T("online.register.title"));
    serverLine(p, false);
    im::pushId("register");
    float y = p.y + 150.0f;
    y += paragraph(i18n::trf("online.register.lead", {i18n::ltr(s.serverName())}), p, y, p.w - 200.0f, ivoryDim, kSmall + 2.0f) + 10.0f;
    bool busy = s.busy(Kind::RegisterResult);
    im::formField(L("online.field.username"), O.user, formRow(p, y), 24, im::FIELD_LTR, T("online.field.username.hint"));
    im::formField(L("online.field.email"), O.email, formRow(p, y), 128, im::FIELD_LTR);
    im::formField(L("online.field.password"), O.password, formRow(p, y), 128, im::FIELD_SECRET, T("online.field.password.hint"));
    im::formField(L("online.field.password2"), O.password2, formRow(p, y), 128, im::FIELD_SECRET);
    bool mismatch = !O.password2.empty() && O.password != O.password2;
    if (mismatch && O.error.empty()) paragraph(T("online.password.mismatch"), p, y + 20.0f, p.w - 200.0f, danger, kSmall + 1.0f);
    else messageLine(p, y + 20.0f);
    if (busy) paragraph(T("online.register.working"), p, y + 60.0f, p.w - 200.0f, muted, kCaption);
    footerRule(p);
    bool back = backButton(p);
    bool ready = !trim(O.user).empty() && O.email.find('@') != std::string::npos && !O.password.empty() && O.password == O.password2;
    if (primaryButton(p, "online.register.button", ready, busy)) {
        O.error.clear();
        s.api().registerAccount(trim(O.user), trim(O.email), O.password);
        s.expect(Kind::RegisterResult);
    }
    im::popId();
    endPage();
    if (back || im::consumeBack()) setSub(Sub::SignIn);
}

void pageCheckEmail(float t) {
    game::OnlineSession& s = ses();
    Rect p = beginPage(t, 940.0f, 600.0f, T("online.check_email.title"));
    im::pushId("checkemail");
    float y = p.y + 170.0f;
    std::string mail = O.resendEmail.empty() ? O.email : O.resendEmail;
    if (!mail.empty()) {
        y += paragraph(i18n::trf("online.check_email.text", {i18n::ltr(mail)}), p, y, p.w - 200.0f, ivory) + 20.0f;
    } else {
        y += paragraph(T("online.check_email.ask"), p, y, p.w - 200.0f, ivoryDim) + 10.0f;
        im::formField(L("online.field.email"), O.email, formRow(p, y), 128, im::FIELD_LTR);
    }
    bool busy = s.busy(Kind::VerificationResent);
    float bw = 330.0f;
    if (im::button(L("online.check_email.resend"), Rect(p.cx() - bw * 0.5f, y, bw, 52.0f), im::ButtonKind::Secondary,
                   !busy && (!mail.empty() || O.email.find('@') != std::string::npos))) {
        s.api().resendVerification(mail.empty() ? trim(O.email) : mail);
        s.expect(Kind::VerificationResent);
    }
    y += 90.0f;
    messageLine(p, y);
    footerRule(p);
    bool back = backButton(p);
    if (primaryButton(p, "online.signin.button", true)) {
        if (O.user.empty()) O.user = mail;
        setSub(Sub::SignIn);
    }
    im::popId();
    endPage();
    if (back || im::consumeBack()) setSub(Sub::SignIn);
}

void pageForgot(float t) {
    game::OnlineSession& s = ses();
    Rect p = beginPage(t, 940.0f, 600.0f, T("online.forgot.title"));
    serverLine(p, false);
    im::pushId("forgot");
    float y = p.y + 160.0f;
    y += paragraph(T("online.forgot.lead"), p, y, p.w - 200.0f, ivoryDim, kSmall + 2.0f) + 14.0f;
    bool busy = s.busy(Kind::PasswordResetRequested);
    im::formField(L("online.field.email"), O.email, formRow(p, y), 128, im::FIELD_LTR);
    messageLine(p, y + 20.0f);
    footerRule(p);
    bool back = backButton(p);
    if (primaryButton(p, "online.forgot.button", O.email.find('@') != std::string::npos, busy)) {
        O.error.clear();
        O.note.clear();
        s.api().forgotPassword(trim(O.email));
        s.expect(Kind::PasswordResetRequested);
    }
    im::popId();
    endPage();
    if (back || im::consumeBack()) setSub(Sub::SignIn);
}

void pageSsoWait(float t) {
    game::OnlineSession& s = ses();
    Rect p = beginPage(t, 900.0f, 520.0f, T("online.sso.title"));
    im::pushId("ssowait");
    float y = p.y + 170.0f;
    y += paragraph(T("online.sso.wait"), p, y, p.w - 200.0f, ivoryDim) + 30.0f;
    spinner(vec2(p.cx(), y + 10.0f), 16.0f);
    footerRule(p);
    bool cancel = backButton(p, "common.cancel");
    im::popId();
    endPage();
    if (cancel || im::consumeBack()) {
        s.api().cancelSso();
        setSub(Sub::SignIn);
    }
}

void pageSsoName(float t) {
    game::OnlineSession& s = ses();
    Rect p = beginPage(t, 940.0f, 600.0f, T("online.sso.name_title"));
    im::pushId("ssoname");
    float y = p.y + 160.0f;
    y += paragraph(T("online.sso.name_lead"), p, y, p.w - 200.0f, ivoryDim, kSmall + 2.0f) + 14.0f;
    bool busy = s.busy(Kind::LoginResult);
    im::formField(L("online.field.username"), O.ssoName, formRow(p, y), 24, im::FIELD_LTR, T("online.field.username.hint"));
    messageLine(p, y + 20.0f);
    footerRule(p);
    bool cancel = backButton(p, "common.cancel");
    if (primaryButton(p, "online.sso.continue", !trim(O.ssoName).empty(), busy)) {
        O.error.clear();
        s.api().completeSso(trim(O.ssoName));
        s.expect(Kind::LoginResult);
    }
    im::popId();
    endPage();
    if (cancel || im::consumeBack()) {
        s.api().cancelSso();
        setSub(Sub::SignIn);
    }
}

// ---- Sub-pages: account ----------------------------------------------------------------------------------
// An e-mail address that, followed by 'tail', fits maxWidth in st: whole when it does, else cut in
// the middle of its name ("jean-bapt…@example.com") so that its domain stays, or at its end when not
// even a letter of the name fits with the domain ('tail' is always kept).
std::string fitAddress(const std::string& email, const std::string& tail, const TextStyle& st, float maxWidth) {
    auto fits = [&](const std::string& address) { return gfx::textWidth(i18n::ltr(address) + tail, st) <= maxWidth + 0.5f; };
    if (fits(email)) return email;
    const size_t at = email.rfind('@');
    if (at != std::string::npos && at > 0) {
        const std::u32string name = uni::decode(email.substr(0, at));
        const std::string domain = "\xE2\x80\xA6" + email.substr(at);
        size_t lo = 0, hi = name.size();
        while (lo < hi) {  // the longest start of the name that fits
            size_t mid = (lo + hi + 1) / 2;
            if (fits(uni::encode(name.substr(0, mid)) + domain)) lo = mid;
            else hi = mid - 1;
        }
        if (lo > 0) return uni::encode(name.substr(0, lo)) + domain;
    }
    return im::elideToFit(email, st, maxWidth - gfx::textWidth(tail, st));
}

void pageAccount(float t) {
    game::OnlineSession& s = ses();
    const net::AccountInfo& a = s.account();
    if (O.fresh) {
        s.api().fetchAccount();
        s.expect(Kind::AccountResult);
    }
    Rect p = beginPage(t, 1720.0f, 960.0f, T("online.account.title"));
    serverLine(p, true);
    im::pushId("account");
    // Three columns from the start side: the account, what can be done with it, the ratings.
    const float pad = 70.0f, gap = 64.0f;
    const float inner = p.w - 2.0f * pad - 2.0f * gap;
    const float colW = std::floor(inner * 0.3f), actW = colW, col2W = inner - colW - actW;
    const float lx = im::flip(p, Rect(p.x + pad, 0, colW, 0)).x;
    const float ax = im::flip(p, Rect(p.x + pad + colW + gap, 0, actW, 0)).x;
    const float rx = im::flip(p, Rect(p.x + pad + colW + gap + actW + gap, 0, col2W, 0)).x;
    const float top = p.y + 150.0f;
    gfx::vline(im::flipX(p, p.x + pad + colW + gap * 0.5f), top, footerY(p) - 40.0f, withAlpha(gold, 0.12f));
    gfx::vline(im::flipX(p, p.x + pad + colW + gap + actW + gap * 0.5f), top, footerY(p) - 40.0f, withAlpha(gold, 0.12f));

    // Account column.
    Rect col(lx, top, colW, 0);
    im::sectionLabel(T("online.account.section"), lx, top + 8.0f, colW);
    float y = top + 56.0f;
    infoLine(T("online.account.username"), a.username.empty() ? "\xE2\x80\x94" : a.username, col, y);
    y += 72.0f;
    // A long address loses the middle of its name, not its domain or the tag after it (measured as
    // infoLine draws it at its smallest).
    const std::string unverified = a.email.empty() || a.emailVerified ? std::string() : "  " + T("online.account.unverified");
    const std::string mail = a.email.empty() ? std::string("\xE2\x80\x94")
                                             : i18n::ltr(fitAddress(a.email, unverified, style(font::FACE_TEXT, kBody * 0.7f, ivory), colW - 10.0f)) +
                                                   unverified;
    infoLine(T("online.account.email"), mail, col, y);
    y += 72.0f;
    if (!a.pendingEmail.empty()) {
        // An e-mail change waiting for its link.
        TextStyle ps = style(font::FACE_ITALIC, kCaption, gold, im::startAlign());
        // The address on a line of its own at most (a line breaks only at spaces).
        std::string line = i18n::trf("online.account.pending_email", {i18n::ltr(fitAddress(a.pendingEmail, std::string(), ps, colW - 10.0f))});
        int n = gfx::textWrapped(line, im::flipX(col, lx), y - 6.0f, colW - 10.0f, ps, 26.0f);
        y += float(n) * 26.0f + 8.0f;
    }
    infoLine(T("online.account.mfa"), T(a.mfaEnabled ? "online.account.mfa_on" : "online.account.mfa_off"), col, y,
             a.mfaEnabled ? ivory : ivoryDim);
    y += 72.0f;
    if (a.googleLinked) {
        infoLine(T("online.account.google"), T("online.account.google_linked"), col, y);
        y += 72.0f;
    }
    // Challenges by name (the server's preference).
    bool accept = a.acceptChallenges;
    if (im::toggleRow(L("online.account.accept_challenges"), accept, Rect(lx, y, colW, 54.0f), !s.busy(Kind::PreferencesResult)))
        s.setAcceptChallenges(accept);
    im::tooltip(T("online.account.accept_challenges.help"));
    y += 76.0f;
    const float bh = 50.0f, bstep = 58.0f;
    bool signOut = im::button(L("online.account.sign_out"), Rect(lx, y, colW, bh), im::ButtonKind::Quiet);
    y += bstep - 8.0f;
    bool signOutAll = im::button(L("online.account.sign_out_all"), Rect(lx, y, colW, bh), im::ButtonKind::Quiet);
    im::tooltip(T("online.account.sign_out_all.help"));

    // What can be done: security, devices, data.
    im::sectionLabel(T("online.account.manage"), ax, top + 8.0f, actW);
    Rect b(ax, top + 56.0f, actW, bh);
    auto next = [&]() {
        Rect r = b;
        b = b.offset(0, bstep);
        return r;
    };
    if (im::button(L("online.account.change_password"), next(), im::ButtonKind::Secondary)) {
        clearSecrets();
        setSub(Sub::Password);
    }
    if (im::button(L("online.account.change_email"), next(), im::ButtonKind::Secondary)) openAccountPage(Sub::Email);
    if (!a.mfaEnabled) {
        if (im::button(L("online.account.mfa_setup"), next(), im::ButtonKind::Secondary)) {
            clearSecrets();
            O.mfaStep = 0;
            setSub(Sub::MfaSetup);
        }
    } else {
        if (im::button(L("online.account.mfa_disable"), next(), im::ButtonKind::Secondary)) {
            clearSecrets();
            setSub(Sub::MfaOff);
        }
        if (im::button(L("online.account.recovery"), next(), im::ButtonKind::Secondary)) {
            clearSecrets();
            O.mfaStep = 0;
            setSub(Sub::Recovery);
        }
    }
    if (im::button(L("online.account.devices"), next(), im::ButtonKind::Secondary)) openAccountPage(Sub::Devices);
    if (im::button(L("online.account.export"), next(), im::ButtonKind::Secondary)) openAccountPage(Sub::Export);
    b = b.offset(0, 10.0f);
    if (im::button(L("online.account.delete"), next(), im::ButtonKind::Secondary)) openAccountPage(Sub::Delete);

    // Ratings column.
    im::sectionLabel(T("online.account.ratings"), rx, top + 8.0f, col2W);
    std::vector<std::string> cats;
    for (const net::Category& c : s.info().categories) cats.push_back(c.id);
    if (cats.empty())
        for (const net::RatingInfo& r : a.ratings) cats.push_back(r.category);
    Rect tcol(rx, top, col2W, 0);
    float cx[4] = {rx + 16.0f, rx + col2W * 0.36f, rx + col2W * 0.58f, rx + col2W * 0.80f};
    TextStyle hs = style(font::FACE_TITLE, 16.0f, withAlpha(gold, 0.85f), HAlign::Left, 0.18f);
    const char* heads[4] = {"online.account.col.tc", "online.account.col.rating", "online.account.col.games", "online.account.col.record"};
    float ty = top + 58.0f;
    for (int i = 0; i < 4; ++i) {
        TextStyle h = hs;
        h.align = i == 0 ? im::startAlign() : HAlign::Center;
        float x = i == 0 ? cx[0] : cx[i] + col2W * 0.1f;
        h.size = gfx::fitSize(T(heads[i]), h, i == 0 ? col2W * 0.34f : col2W * 0.21f, 0.7f);
        gfx::text(T(heads[i]), im::flipX(tcol, x), ty, h);
    }
    ty += 14.0f;
    gfx::hlineFade(rx, rx + col2W, ty, withAlpha(gold, 0.3f), 0.2f);
    float rowH = std::min(46.0f, (footerY(p) - 100.0f - ty) / float(std::max<size_t>(cats.size(), 1)));
    TextStyle vs = style(font::FACE_TEXT, 25.0f, ivory, HAlign::Center);
    for (size_t i = 0; i < cats.size(); ++i) {
        const net::RatingInfo* r = s.rating(cats[i]);
        float by = ty + rowH * float(i) + rowH * 0.5f + 9.0f;
        if (i % 2 == 1) gfx::fill(Rect(rx, ty + rowH * float(i), col2W, rowH), withAlpha(gold, 0.03f));
        TextStyle c0 = vs;
        c0.align = im::startAlign();
        gfx::text(i18n::ltr(spacedPlus(cats[i])), im::flipX(tcol, cx[0]), by, c0);
        TextStyle rs = vs;
        rs.color = r && !r->provisional ? goldBright : ivory;
        gfx::text(i18n::ltr(ratingText(r)), im::flipX(tcol, cx[1] + col2W * 0.1f), by, rs);
        TextStyle gs = vs;
        gs.color = ivoryDim;
        gfx::text(std::to_string(r ? r->games : 0), im::flipX(tcol, cx[2] + col2W * 0.1f), by, gs);
        std::string rec = r ? i18n::ltr(std::to_string(r->wins) + " / " + std::to_string(r->draws) + " / " + std::to_string(r->losses))
                            : i18n::ltr("0 / 0 / 0");
        gfx::text(rec, im::flipX(tcol, cx[3] + col2W * 0.1f), by, gs);
    }
    if (cats.empty()) spinner(vec2(rx + col2W * 0.5f, ty + 40.0f));
    TextStyle ns = style(font::FACE_ITALIC, kCaption, muted, im::startAlign());
    float noteY = ty + rowH * float(cats.size()) + 38.0f;
    gfx::textWrapped(T("online.account.provisional"), im::flipX(tcol, rx), noteY, col2W, ns, 26.0f);
    float my = footerY(p) - 50.0f;
    if (!O.error.empty() || !O.note.empty()) messageLine(p, my);
    footerRule(p);
    bool back = backButton(p);
    // The game history, from the ratings (end side).
    const Rect hb = im::flip(p, Rect(p.r() - 60.0f - 320.0f, footerY(p), 320.0f, kBtnH));
    bool history = im::button(L("online.account.history"), hb, im::ButtonKind::Primary);
    im::popId();
    endPage();
    if (history) {
        openAccountPage(Sub::History);
        return;
    }
    if (signOut || signOutAll) {
        s.signOut(signOutAll);
        clearSecrets();
        setSub(Sub::SignIn);
        O.note = T(signOutAll ? "online.account.signed_out_all" : "online.account.signed_out");
        return;
    }
    if (back || im::consumeBack()) setSub(Sub::Play);
}

void pagePassword(float t) {
    game::OnlineSession& s = ses();
    Rect p = beginPage(t, 980.0f, 700.0f, T("online.password.title"));
    serverLine(p, true);
    im::pushId("password");
    float y = p.y + 170.0f;
    bool busy = s.busy(Kind::PasswordChanged);
    im::formField(L("online.field.current_password"), O.password, formRow(p, y), 128, im::FIELD_SECRET);
    im::formField(L("online.field.new_password"), O.newPassword, formRow(p, y), 128, im::FIELD_SECRET, T("online.field.password.hint"));
    im::formField(L("online.field.password2"), O.password2, formRow(p, y), 128, im::FIELD_SECRET);
    bool mismatch = !O.password2.empty() && O.newPassword != O.password2;
    if (mismatch && O.error.empty()) paragraph(T("online.password.mismatch"), p, y + 20.0f, p.w - 200.0f, danger, kSmall + 1.0f);
    else messageLine(p, y + 20.0f);
    paragraph(T("online.password.other_sessions"), p, y + 70.0f, p.w - 200.0f, muted, kCaption);
    footerRule(p);
    bool back = backButton(p);
    if (primaryButton(p, "online.password.button", !O.password.empty() && !O.newPassword.empty() && O.newPassword == O.password2, busy)) {
        O.error.clear();
        s.api().changePassword(O.password, O.newPassword);
        s.expect(Kind::PasswordChanged);
    }
    im::popId();
    endPage();
    if (back || im::consumeBack()) {
        clearSecrets();
        setSub(Sub::Account);
    }
}

// The recovery codes, shown once (two-factor setup, new codes).
void recoveryCodes(const Rect& p, float y) {
    y += paragraph(T("online.recovery.lead"), p, y, p.w - 200.0f, ivoryDim, kSmall + 2.0f) + 16.0f;
    float colW = 300.0f;
    TextStyle cs = style(font::FACE_TEXT, 30.0f, goldBright, HAlign::Center, 0.08f);
    int n = int(O.codes.size());
    int rows = (n + 1) / 2;
    Rect box(p.cx() - colW - 30.0f, y - 10.0f, 2.0f * colW + 60.0f, float(rows) * 48.0f + 24.0f);
    gfx::fill(box, vec4(0, 0, 0, 0.3f), 3.0f);
    gfx::stroke(box, withAlpha(gold, 0.3f), 0.0f, 3.0f);
    for (int i = 0; i < n; ++i) {
        float x = p.cx() + (i < rows ? -colW * 0.5f - 15.0f : colW * 0.5f + 15.0f);
        float by = y + 30.0f + float(i % rows) * 48.0f;
        gfx::text(i18n::ltr(O.codes[size_t(i)]), x, by, cs);
    }
    float cy = box.b() + 20.0f;
    std::string all;
    for (const std::string& c : O.codes) all += c + "\n";
    bool copiedRecently = float(im::time()) - O.copiedAt < 2.5f;
    if (linkButton(copiedRecently ? "online.copied" : "online.copy", p.cx(), cy)) {
        if (detail::setClipboardText(all)) O.copiedAt = float(im::time());
        else notify(T("online.copy.unavailable"), 3.0f);
    }
}

void pageMfaSetup(float t) {
    game::OnlineSession& s = ses();
    Rect p = beginPage(t, 1200.0f, 900.0f, T(O.mfaStep == 2 ? "online.recovery.title" : "online.mfa_setup.title"));
    serverLine(p, true);
    im::pushId("mfasetup");
    im::pushId(O.mfaStep);
    bool back = false;
    if (O.mfaStep == 0) {
        float y = p.y + 170.0f;
        y += paragraph(T("online.mfa_setup.lead"), p, y, p.w - 260.0f, ivoryDim, kSmall + 2.0f) + 14.0f;
        im::formField(L("online.field.password"), O.password, formRow(p, y, 160.0f), 128, im::FIELD_SECRET);
        messageLine(p, y + 20.0f);
        footerRule(p);
        back = backButton(p);
        if (primaryButton(p, "online.continue", !O.password.empty(), s.busy(Kind::MfaSetupResult))) {
            O.error.clear();
            s.api().mfaSetup(O.password);
            s.expect(Kind::MfaSetupResult);
        }
    } else if (O.mfaStep == 1) {
        float top = p.y + 150.0f;
        float qs = 300.0f;
        float pad = 90.0f;
        Rect qr = im::flip(p, Rect(p.x + pad, top + 20.0f, qs, qs));
        if (!detail::drawQrCode(O.mfaUri, qr)) {
            gfx::stroke(qr, withAlpha(gold, 0.3f), 0.0f, 3.0f);
        }
        float tx = p.x + pad + qs + 60.0f, tw = p.r() - pad - tx;
        Rect col(tx, top, tw, 0);
        Rect colF = im::flip(p, col);
        TextStyle st = style(font::FACE_TEXT, kSmall + 2.0f, ivoryDim, im::startAlign());
        float y = top + 40.0f;
        int n = gfx::textWrapped(T("online.mfa_setup.scan"), im::flipX(colF, colF.x), y, tw, st, 31.0f);
        y += float(n) * 31.0f + 22.0f;
        TextStyle ks = style(font::FACE_TEXT, 30.0f, goldBright, im::startAlign(), 0.06f);
        std::string key = groupsOf4(O.mfaSecret);
        ks.size = gfx::fitSize(key, ks, tw, 0.6f);
        ks.dir = 0;
        gfx::text(i18n::ltr(key), im::flipX(colF, colF.x), y, ks);
        y += 56.0f;
        n = gfx::textWrapped(T("online.mfa_setup.enter"), im::flipX(colF, colF.x), y, tw, st, 31.0f);
        y += float(n) * 31.0f + 8.0f;
        im::formField(L("online.field.code6"), O.code, Rect(colF.x, y, tw, 56.0f), 8, im::FIELD_LTR, "123456");
        y += 80.0f;
        if (!O.error.empty()) {
            TextStyle es = style(font::FACE_ITALIC, kSmall + 1.0f, danger, im::startAlign());
            gfx::textWrapped(O.error, im::flipX(colF, colF.x), y, tw, es, 30.0f);
        }
        footerRule(p);
        back = backButton(p, "common.cancel");
        if (primaryButton(p, "online.mfa_setup.enable", digitsOnly(O.code).size() == 6, s.busy(Kind::MfaEnableResult))) {
            O.error.clear();
            s.api().mfaEnable(digitsOnly(O.code));
            s.expect(Kind::MfaEnableResult);
        }
    } else {
        recoveryCodes(p, p.y + 160.0f);
        footerRule(p);
        if (primaryButton(p, "online.recovery.saved", true)) {
            O.codes.clear();
            setSub(Sub::Account);
            O.note = T("online.mfa_setup.done");
        }
    }
    im::popId();
    im::popId();
    endPage();
    if (O.mfaStep < 2 && (back || im::consumeBack())) {
        clearSecrets();
        O.mfaSecret.clear();
        O.mfaUri.clear();
        setSub(Sub::Account);
    }
}

void pageMfaOff(float t, bool regenerate) {
    game::OnlineSession& s = ses();
    if (regenerate && O.mfaStep == 2) {
        Rect p = beginPage(t, 1200.0f, 900.0f, T("online.recovery.title"));
        im::pushId("recovery2");
        recoveryCodes(p, p.y + 160.0f);
        footerRule(p);
        if (primaryButton(p, "online.recovery.saved", true)) {
            O.codes.clear();
            setSub(Sub::Account);
        }
        im::popId();
        endPage();
        return;
    }
    Rect p = beginPage(t, 980.0f, 680.0f, T(regenerate ? "online.recovery.new_title" : "online.mfa_off.title"));
    serverLine(p, true);
    im::pushId(regenerate ? "recovery" : "mfaoff");
    float y = p.y + 160.0f;
    y += paragraph(T(regenerate ? "online.recovery.new_lead" : "online.mfa_off.lead"), p, y, p.w - 200.0f, ivoryDim, kSmall + 2.0f) + 14.0f;
    im::formField(L("online.field.password"), O.password, formRow(p, y), 128, im::FIELD_SECRET);
    im::formField(L(regenerate ? "online.field.code6" : "online.field.code"), O.code, formRow(p, y), 14, im::FIELD_LTR,
                  T(regenerate ? "online.field.code6.hint" : "online.field.code.hint"));
    messageLine(p, y + 20.0f);
    footerRule(p);
    bool back = backButton(p);
    Kind k = regenerate ? Kind::RecoveryCodesResult : Kind::MfaDisableResult;
    if (primaryButton(p, regenerate ? "online.recovery.button" : "online.mfa_off.button", !O.password.empty() && trim(O.code).size() >= 6,
                      s.busy(k))) {
        O.error.clear();
        if (regenerate) s.api().regenerateRecoveryCodes(O.password, trim(O.code));
        else s.api().mfaDisable(O.password, trim(O.code));
        s.expect(k);
    }
    im::popId();
    endPage();
    if (back || im::consumeBack()) {
        clearSecrets();
        setSub(Sub::Account);
    }
}

// ---- Sub-pages: play -------------------------------------------------------------------------------------
// Grid of the server's categories (a spinner while none is known); 'index' is the selected one,
// -1 = the "Custom" tile when 'custom'. Returns the height used.
float categoryGrid(const Rect& col, float y, int& index, bool custom, bool withRatings) {
    game::OnlineSession& s = ses();
    const std::vector<net::Category>& cats = s.info().categories;
    int n = int(cats.size()) + (custom ? 1 : 0);
    if (cats.empty()) {
        if (s.busy(Kind::ServerInfoResult) || !s.infoKnown()) spinner(vec2(col.x + col.w * 0.5f, y + 40.0f));
        return 80.0f;
    }
    int cols = 4;
    float gap = 12.0f, cw = (col.w - gap * float(cols - 1)) / float(cols), ch = 66.0f;
    im::pushId("cats");
    for (int i = 0; i < n; ++i) {
        bool isCustom = i == int(cats.size());
        Rect r = im::flip(col, Rect(col.x + float(i % cols) * (cw + gap), y + float(i / cols) * (ch + gap), cw, ch));
        bool sel = isCustom ? index < 0 : index == i;
        std::string label = isCustom ? T("tc.custom") : spacedPlus(cats[size_t(i)].id);
        std::string sub = isCustom ? T("online.casual_only") : withRatings ? ratingText(s.rating(cats[size_t(i)].id)) : std::string();
        if (!withRatings && !isCustom) sub = T(detail::tcCategoryKey(cats[size_t(i)].baseSec, cats[size_t(i)].incSec));
        if (tcTile(i, r, isCustom ? label : i18n::ltr(label), i18n::ltr(sub), sel)) index = isCustom ? -1 : i;
    }
    im::popId();
    return float((n + cols - 1) / cols) * (ch + gap);
}

int categoryIndex(const std::string& id) {
    const std::vector<net::Category>& cats = ses().info().categories;
    for (size_t i = 0; i < cats.size(); ++i)
        if (cats[i].id == id) return int(i);
    return 0;
}

// Searching card over the Play page. Returns true when cancelled.
bool searchCard() {
    game::OnlineSession& s = ses();
    const game::OnlineSession::Queue& q = s.queue();
    vec2 v = view();
    gfx::Layer prev = gfx::layer();
    gfx::setLayer(gfx::LAYER_MODAL);
    gfx::fill(Rect(0, 0, v.x, v.y), vec4(0, 0, 0, 0.35f));
    float w = 640.0f, h = 400.0f;
    Rect r(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f, w, h);
    gfx::fill(r, vec4(0.035f, 0.03f, 0.027f, 1.0f), 3.0f);
    im::panel(r);
    im::pageTitle(T("online.search.title"), r.cx(), r.y + 72.0f);
    std::string terms = spacedPlus(q.category) + "  \xC2\xB7  " + T(q.rated ? "online.rated" : "online.casual");
    TextStyle ts = style(font::FACE_TEXT, 30.0f, ivory, HAlign::Center);
    gfx::text(i18n::ltr(terms), r.cx(), r.y + 140.0f, ts);
    TextStyle es = style(font::FACE_TEXT, 58.0f, goldBright, HAlign::Center);
    gfx::text(game::durationText(std::max(0.0, s.nowMs() - q.sinceMs)), r.cx(), r.y + 216.0f, es);
    std::string detail;
    if (q.window > 0) detail = i18n::trf("online.search.window", {i18n::ltr("\xC2\xB1" + std::to_string(q.window))});
    if (q.queued > 0) {
        if (!detail.empty()) detail += "  \xC2\xB7  ";
        detail += i18n::trn("online.search.queued", q.queued, {std::to_string(q.queued)});
    }
    if (detail.empty()) detail = T("online.search.wait");
    TextStyle ds = style(font::FACE_ITALIC, kSmall, ivoryDim, HAlign::Center);
    gfx::text(detail, r.cx(), r.y + 262.0f, ds);
    spinner(vec2(r.cx() - 170.0f, r.y + 196.0f), 12.0f);
    spinner(vec2(r.cx() + 170.0f, r.y + 196.0f), 12.0f);
    float bw = 240.0f;
    im::Id cid = im::makeId("##online.search.cancel");
    bool cancel = im::button(T("common.cancel") + "##online.search.cancel", Rect(r.cx() - bw * 0.5f, r.b() - 44.0f - kBtnH, bw, kBtnH),
                             im::ButtonKind::Secondary);
    im::setDefaultFocus(cid);
    gfx::setLayer(prev);
    if (!cancel && im::consumeBack()) cancel = true;
    return cancel;
}

// Our challenge or private game, waiting for an answer.
void outgoingCard(const Rect& col, float y) {
    game::OnlineSession& s = ses();
    const game::OnlineSession::Outgoing& o = s.outgoing();
    Rect r(col.x, y, col.w, o.target.empty() ? 210.0f : 170.0f);
    gfx::fill(r, vec4(0, 0, 0, 0.3f), 3.0f);
    gfx::stroke(r, withAlpha(gold, 0.45f), 0.0f, 3.0f);
    TextStyle cap = style(font::FACE_TITLE, 17.0f, gold, im::startAlign(), 0.2f);
    std::string terms = i18n::ltr(spacedPlus(tcLabel(o.baseSec, o.incSec))) + "  \xC2\xB7  " + T(o.rated ? "online.rated" : "online.casual");
    TextStyle ts = style(font::FACE_ITALIC, kSmall, ivoryDim, im::startAlign());
    if (!o.target.empty()) {
        gfx::text(T("online.outgoing.challenge"), im::flipX(r, r.x + 24.0f), r.y + 36.0f, cap);
        TextStyle ns = style(font::FACE_TEXT, 30.0f, ivory, im::startAlign());
        gfx::text(o.target, im::flipX(r, r.x + 24.0f), r.y + 78.0f, ns);
        gfx::text(terms + "  \xC2\xB7  " + T("online.outgoing.waiting"), im::flipX(r, r.x + 24.0f), r.y + 112.0f, ts);
    } else {
        gfx::text(T("online.outgoing.private"), im::flipX(r, r.x + 24.0f), r.y + 36.0f, cap);
        if (o.code.empty()) {
            spinner(vec2(im::flipX(r, r.x + 50.0f), r.y + 80.0f));
        } else {
            TextStyle cs = style(font::FACE_TEXT, 52.0f, goldBright, im::startAlign(), 0.18f);
            cs.dir = 0;
            gfx::text(i18n::ltr(o.code), im::flipX(r, r.x + 24.0f), r.y + 100.0f, cs);
        }
        gfx::text(terms, im::flipX(r, r.x + 24.0f), r.y + 138.0f, ts);
        const std::string hint = T("online.outgoing.private_hint");
        TextStyle hs = ts;
        hs.size = gfx::fitSize(hint, hs, r.w - 48.0f, 0.7f);
        gfx::text(hint, im::flipX(r, r.x + 24.0f), r.y + 170.0f, hs);
    }
    float bw = 150.0f;
    if (im::button(L("common.cancel"), im::flip(r, Rect(r.r() - bw - 18.0f, r.y + 16.0f, bw, 44.0f)), im::ButtonKind::Quiet))
        s.cancelOutgoing();
}

void pagePlay(float t) {
    game::OnlineSession& s = ses();
    game::Settings& gs = game::settings();
    const std::vector<net::Category>& cats = s.info().categories;
    Rect p = beginPage(t, 1400.0f, 940.0f, T("online.title"));
    serverLine(p, true);
    bool searching = s.queue().searching;
    if (searching) im::pushBlock();
    im::pushId("play");
    // Account, top corner (end side).
    {
        std::string who = i18n::trf("online.play.signed_in", {s.account().username});
        TextStyle ws = style(font::FACE_ITALIC, kCaption, withAlpha(muted, 0.95f), im::endAlign());
        float bw = std::max(150.0f, gfx::textWidth(T("online.account.button"), style(font::FACE_ITALIC, kSmall, muted)) + 40.0f);
        Rect ab = im::flip(p, Rect(p.r() - 26.0f - bw, p.y + 12.0f, bw, 40.0f));
        if (im::button(L("online.account.button"), ab, im::ButtonKind::Quiet)) setSub(Sub::Account);
        ws.size = gfx::fitSize(who, ws, p.w * 0.25f, 0.7f);
        gfx::text(who, im::flipX(p, p.r() - 40.0f - bw), p.y + 37.0f, ws);
    }
    float pad = 64.0f, gap = 72.0f;
    float colW = (p.w - 2.0f * pad - gap) * 0.5f;
    float lx = im::flip(p, Rect(p.x + pad, 0, colW, 0)).x, rx = im::flip(p, Rect(p.x + pad + colW + gap, 0, colW, 0)).x;
    float top = p.y + 150.0f;
    gfx::vline(p.cx(), top, footerY(p) - 40.0f, withAlpha(gold, 0.12f));

    // Matchmaking column.
    im::sectionLabel(T("online.play.find_section"), lx, top + 8.0f, colW);
    int idx = categoryIndex(gs.onlineCategory);
    float y = top + 30.0f;
    float gh = categoryGrid(Rect(lx, y, colW, 0), y, idx, false, true);
    if (!cats.empty() && cats[size_t(idx)].id != gs.onlineCategory) gs.onlineCategory = cats[size_t(idx)].id;
    y += gh + 6.0f;
    int mode = gs.onlineRated ? 0 : 1;
    if (im::selectorRow(L("online.play.mode"), mode, {T("online.rated"), T("online.casual")}, Rect(lx, y, colW, 52.0f)))
        gs.onlineRated = mode == 0;
    im::tooltip(T("online.play.mode.help"));
    y += 66.0f;
    double now = s.nowMs();
    bool online = s.conn() == net::ConnState::Online;
    double banned = std::max(s.bannedUntilMs(), double(s.account().bannedUntilMs));
    bool cooldown = s.cooldownUntilMs() > now;
    bool isBanned = banned > now;
    im::Id findId = im::makeId("##online.play.find");
    if (im::button(L("online.play.find"), Rect(lx, y, colW, 60.0f), im::ButtonKind::Primary, online && !cooldown && !isBanned && !cats.empty())) {
        gs.save();
        s.findOpponent(gs.onlineCategory, gs.onlineRated);
    }
    im::setDefaultFocus(findId);
    y += 88.0f;
    TextStyle ns = style(font::FACE_ITALIC, kSmall, danger, HAlign::Center);
    std::string notice;
    if (isBanned) notice = i18n::trf("online.play.banned", {game::localTimeText(banned)});
    else if (cooldown) notice = i18n::trf("online.play.cooldown", {game::localTimeText(s.cooldownUntilMs())});
    else if (!online) notice = T(s.conn() == net::ConnState::Incompatible ? "online.err.incompatible" : "online.play.connecting");
    if (!notice.empty()) {
        if (online || s.conn() == net::ConnState::Incompatible) ns.color = danger;
        else ns.color = ivoryDim;
        gfx::textWrapped(notice, lx + colW * 0.5f, y, colW - 20.0f, ns, 28.0f);
    }
    if (!s.infoKnown() && !s.infoError().empty()) {
        float ey = y + 70.0f;
        TextStyle es = style(font::FACE_ITALIC, kSmall, danger, HAlign::Center);
        gfx::textWrapped(game::onlineErrorText(s.infoError()), lx + colW * 0.5f, ey, colW - 20.0f, es, 28.0f);
        if (linkButton("online.retry", lx + colW * 0.5f, ey + 20.0f, !s.busy(Kind::ServerInfoResult))) s.refreshInfo();
    }

    // Friends column.
    im::sectionLabel(T("online.play.friend_section"), rx, top + 8.0f, colW);
    float fy = top + 36.0f;
    bool busyOut = s.outgoing().active;
    if (choiceRow("online.play.challenge", "online.play.challenge.desc", Rect(rx, fy, colW, 96.0f), online && !busyOut)) {
        O.terms.tc = categoryIndex(gs.onlineCategory);
        O.terms.rated = gs.onlineRated;
        O.terms.color = gs.onlineColor;
        O.terms.baseSec = gs.onlineCustomBaseSeconds;
        O.terms.incSec = gs.onlineCustomIncrementSeconds;
        setSub(Sub::Challenge);
    }
    fy += 110.0f;
    if (choiceRow("online.play.private", "online.play.private.desc", Rect(rx, fy, colW, 96.0f), online && !busyOut)) {
        O.terms.tc = categoryIndex(gs.onlineCategory);
        O.terms.rated = gs.onlineRated;
        O.terms.color = gs.onlineColor;
        O.terms.baseSec = gs.onlineCustomBaseSeconds;
        O.terms.incSec = gs.onlineCustomIncrementSeconds;
        O.joinCode.clear();
        setSub(Sub::Private);
    }
    fy += 110.0f;
    if (choiceRow("online.play.direct", "online.play.direct.desc", Rect(rx, fy, colW, 96.0f))) setSub(Sub::Direct);
    fy += 124.0f;
    if (s.outgoing().active) outgoingCard(Rect(rx, fy, colW, 0), fy);

    footerRule(p);
    bool back = backButton(p);
    im::popId();
    if (searching) im::popBlock();
    endPage();
    if (searching) {
        if (searchCard()) s.cancelSearch();
        return;
    }
    if (back || im::consumeBack()) {
        s.cancelOutgoing();
        O.leave = true;
    }
}

// Terms column of a challenge / private game (time control, rated, colour).
void termsColumn(const Rect& col, float y) {
    game::OnlineSession& s = ses();
    Terms& tm = O.terms;
    im::sectionLabel(T("newgame.time_control"), col.x, y + 8.0f, col.w);
    y += 30.0f;
    int idx = tm.tc;
    y += categoryGrid(Rect(col.x, y, col.w, 0), y, idx, true, false) + 4.0f;
    tm.tc = idx;
    const std::vector<net::Category>& cats = s.info().categories;
    if (tm.tc >= int(cats.size())) tm.tc = cats.empty() ? -1 : 0;
    auto row = [&]() {
        Rect r(col.x, y, col.w, 46.0f);
        y += 50.0f;
        return r;
    };
    if (tm.tc < 0) {
        im::pushId("custom");
        auto& bv = baseTimeValues();
        int bi = nearestIndex(bv, tm.baseSec);
        if (im::stepperRow(L("tc.base_time"), bi, int(bv.size()), [&](int i) { return clockText(bv[size_t(i)]); }, row()))
            tm.baseSec = bv[size_t(bi)];
        int inc = std::clamp(tm.incSec, 0, 60);
        if (im::stepperRow(L("tc.increment"), inc, 61, [](int i) { return i18n::trf("tc.seconds", {i18n::ltr("+" + std::to_string(i))}); }, row()))
            tm.incSec = inc;
        im::popId();
    }
    int bs = tm.tc >= 0 ? cats[size_t(tm.tc)].baseSec : tm.baseSec, is = tm.tc >= 0 ? cats[size_t(tm.tc)].incSec : tm.incSec;
    bool canRate = isOfficialCategory(bs, is);
    bool rated = tm.rated && canRate;
    if (im::toggleRow(L("online.terms.rated"), rated, row(), canRate)) tm.rated = rated;
    im::tooltip(T("online.terms.rated.help"));
    im::selectorRow(L("online.terms.color"), tm.color, {T("online.color.random"), T("common.white"), T("common.black")}, row());
}

void termsValues(int& baseSec, int& incSec, bool& rated) {
    const std::vector<net::Category>& cats = ses().info().categories;
    const Terms& tm = O.terms;
    baseSec = tm.tc >= 0 && tm.tc < int(cats.size()) ? cats[size_t(tm.tc)].baseSec : tm.baseSec;
    incSec = tm.tc >= 0 && tm.tc < int(cats.size()) ? cats[size_t(tm.tc)].incSec : tm.incSec;
    rated = tm.rated && isOfficialCategory(baseSec, incSec);
}

void rememberTerms() {
    game::Settings& gs = game::settings();
    const std::vector<net::Category>& cats = ses().info().categories;
    if (O.terms.tc >= 0 && O.terms.tc < int(cats.size())) gs.onlineCategory = cats[size_t(O.terms.tc)].id;
    gs.onlineColor = O.terms.color;
    gs.onlineCustomBaseSeconds = O.terms.baseSec;
    gs.onlineCustomIncrementSeconds = O.terms.incSec;
    gs.save();
}

void pageChallenge(float t, bool privateGame) {
    game::OnlineSession& s = ses();
    Rect p = beginPage(t, 1400.0f, 940.0f, T(privateGame ? "online.private.title" : "online.challenge.title"));
    serverLine(p, true);
    im::pushId(privateGame ? "private" : "challenge");
    float pad = 64.0f, gap = 72.0f;
    float colW = (p.w - 2.0f * pad - gap) * 0.5f;
    float lx = im::flip(p, Rect(p.x + pad, 0, colW, 0)).x, rx = im::flip(p, Rect(p.x + pad + colW + gap, 0, colW, 0)).x;
    float top = p.y + 150.0f;
    gfx::vline(p.cx(), top, footerY(p) - 40.0f, withAlpha(gold, 0.12f));
    termsColumn(Rect(lx, top, colW, 0), top);
    bool online = s.conn() == net::ConnState::Online;
    int baseSec = 0, incSec = 0;
    bool rated = false;
    termsValues(baseSec, incSec, rated);
    bool send = false, join = false;
    if (!privateGame) {
        im::sectionLabel(T("online.challenge.opponent"), rx, top + 8.0f, colW);
        float y = top + 40.0f;
        im::formField(L("online.field.opponent"), O.target, Rect(rx, y, colW, 56.0f), 24, im::FIELD_LTR, T("online.field.opponent.hint"));
        y += 90.0f;
        TextStyle ns = style(font::FACE_ITALIC, kSmall, ivoryDim, im::startAlign());
        gfx::textWrapped(T("online.challenge.note"), im::flipX(Rect(rx, 0, colW, 0), rx), y, colW, ns, 29.0f);
    } else {
        im::sectionLabel(T("online.private.create_section"), rx, top + 8.0f, colW);
        float y = top + 36.0f;
        TextStyle ns = style(font::FACE_ITALIC, kSmall, ivoryDim, im::startAlign());
        int n = gfx::textWrapped(T("online.private.create_note"), im::flipX(Rect(rx, 0, colW, 0), rx), y + 20.0f, colW, ns, 29.0f);
        y += 20.0f + float(n) * 29.0f + 10.0f;
        if (im::button(L("online.private.create"), Rect(rx, y, colW, 58.0f), im::ButtonKind::Primary, online && !s.outgoing().active)) send = true;
        y += 110.0f;
        im::sectionLabel(T("online.private.join_section"), rx, y, colW);
        y += 30.0f;
        std::string code = O.joinCode;
        if (im::formField(L("online.field.private_code"), code, Rect(rx, y, colW, 56.0f), 12, im::FIELD_LTR, "ABC123")) O.joinCode = upperCode(code);
        y += 70.0f;
        if (im::button(L("online.private.join"), Rect(rx, y, colW, 56.0f), im::ButtonKind::Secondary, online && O.joinCode.size() >= 4)) join = true;
        if (!O.note.empty()) {
            TextStyle js = style(font::FACE_ITALIC, kSmall, ivoryDim, HAlign::Center);
            gfx::text(O.note, rx + colW * 0.5f, y + 96.0f, js);
            spinner(vec2(rx + colW * 0.5f - gfx::textWidth(O.note, js) * 0.5f - 28.0f, y + 88.0f));
        }
    }
    footerRule(p);
    bool back = backButton(p);
    if (!privateGame && primaryButton(p, "online.challenge.send", online && !trim(O.target).empty() && !s.outgoing().active)) send = true;
    if (!privateGame) {
        // Summary of the challenge beside the button.
        std::string summary = i18n::ltr(spacedPlus(tcLabel(baseSec, incSec))) + "  \xC2\xB7  " + T(rated ? "online.rated" : "online.casual");
        TextStyle ss = style(font::FACE_ITALIC, kSmall, ivoryDim, im::endAlign());
        gfx::text(summary, im::flipX(p, p.r() - 60.0f - kBtnW - 30.0f), footerY(p) + kBtnH * 0.5f + 7.0f, ss);
    }
    im::popId();
    endPage();
    if (send) {
        rememberTerms();
        if (privateGame) s.createPrivateGame(baseSec, incSec, rated, O.terms.color);
        else s.challenge(trim(O.target), baseSec, incSec, rated, O.terms.color);
        setSub(Sub::Play);
        return;
    }
    if (join) {
        s.joinPrivateGame(O.joinCode);
        O.note = T("online.private.joining");
        return;
    }
    if (back || im::consumeBack()) setSub(Sub::Play);
}

// ---- Sub-pages: direct match -------------------------------------------------------------------------
void pageDirect(float t) {
    Rect p = beginPage(t, 1100.0f, 720.0f, T("online.direct.title"));
    im::pushId("direct");
    float y = p.y + 150.0f;
    y += paragraph(T("online.direct.lead"), p, y, p.w - 220.0f, ivoryDim, kSmall + 2.0f) + 24.0f;
    float w = p.w - 240.0f;
    if (choiceRow("online.direct.host", "online.direct.host.desc", Rect(p.cx() - w * 0.5f, y, w, 100.0f))) {
        O.hostPortText = std::to_string(game::settings().directPort);
        setSub(Sub::DirectHost);
    }
    y += 120.0f;
    if (choiceRow("online.direct.join", "online.direct.join.desc", Rect(p.cx() - w * 0.5f, y, w, 100.0f))) {
        game::Settings& gs = game::settings();
        if (O.directAddress.empty()) O.directAddress = gs.directAddress;
        O.directPort = std::to_string(gs.directJoinPort);
        O.directJoining = false;
        setSub(Sub::DirectJoin);
    }
    footerRule(p);
    bool back = backButton(p);
    im::popId();
    endPage();
    if (back || im::consumeBack()) {
        game::OnlineSession& s = ses();
        if (!s.serverConfigured()) setSub(Sub::NoServer);
        else if (s.signedIn()) setSub(Sub::Play);
        else setSub(Sub::SignIn);
    }
}

void pageDirectHost(float t) {
    game::Settings& gs = game::settings();
    Rect p = beginPage(t, 1400.0f, 900.0f, T("online.direct.host_title"));
    im::pushId("directhost");
    im::beginInfoMarks();  // the host's choices carry their definitions as in Options
    float pad = 64.0f, gap = 72.0f;
    float colW = (p.w - 2.0f * pad - gap) * 0.5f;
    float lx = im::flip(p, Rect(p.x + pad, 0, colW, 0)).x, rx = im::flip(p, Rect(p.x + pad + colW + gap, 0, colW, 0)).x;
    float top = p.y + 150.0f;
    gfx::vline(p.cx(), top, footerY(p) - 40.0f, withAlpha(gold, 0.12f));

    // Time control: the presets without "Unlimited", and Custom.
    im::sectionLabel(T("newgame.time_control"), lx, top + 8.0f, colW);
    const std::vector<chess::TimeControl>& presets = chess::timeControlPresets();
    int n = int(presets.size());  // presets 1..n-1 + Custom
    int cols = 4;
    float cg = 12.0f, cw = (colW - cg * float(cols - 1)) / float(cols), ch = 66.0f;
    float gy = top + 30.0f;
    int sel = gs.directTimeControl;
    if (sel == 0 || sel >= n) sel = std::min(game::Settings::kDefaultDirectTimeControl, n - 1);
    im::pushId("tc");
    for (int i = 1; i <= n; ++i) {
        bool isCustom = i == n;
        int k = i - 1;
        Rect r = im::flip(Rect(lx, gy, colW, 0), Rect(lx + float(k % cols) * (cw + cg), gy + float(k / cols) * (ch + cg), cw, ch));
        bool on = isCustom ? sel < 0 : sel == i;
        std::string label = isCustom ? T("tc.custom") : i18n::ltr(spacedPlus(presets[size_t(i)].label()));
        std::string sub;
        if (isCustom) sub = T("tc.your_own");
        else sub = T(detail::tcCategoryKey(presets[size_t(i)].baseMs / 1000, presets[size_t(i)].incrementMs / 1000));
        if (tcTile(i, r, label, sub, on)) sel = isCustom ? -1 : i;
    }
    im::popId();
    gs.directTimeControl = sel;
    float y = gy + float((n + cols - 1) / cols) * (ch + cg) + 6.0f;
    auto row = [&](float x, float w) {
        Rect r(x, y, w, 46.0f);
        y += 50.0f;
        return r;
    };
    if (sel < 0) {
        im::pushId("custom");
        auto& bv = baseTimeValues();
        int bi = nearestIndex(bv, gs.directBaseSeconds);
        if (im::stepperRow(L("tc.base_time"), bi, int(bv.size()), [&](int i) { return clockText(bv[size_t(i)]); }, row(lx, colW)))
            gs.directBaseSeconds = bv[size_t(bi)];
        int inc = std::clamp(gs.directIncrementSeconds, 0, 60);
        if (im::stepperRow(L("tc.increment"), inc, 61, [](int i) { return i18n::trf("tc.seconds", {i18n::ltr("+" + std::to_string(i))}); },
                           row(lx, colW)))
            gs.directIncrementSeconds = inc;
        im::popId();
    }
    y += 6.0f;
    int color = std::clamp(gs.directColor, 0, 2);
    if (im::selectorRow(L("online.direct.your_color"), color, {T("online.color.random"), T("common.white"), T("common.black")}, row(lx, colW)))
        gs.directColor = color;
    // The robots press the clock by themselves (both players), or each player presses it.
    im::toggleRow(L("options.auto_press"), gs.directAutoPress, row(lx, colW));
    im::tooltip(T("online.direct.auto_press.help"));
    TextStyle us = style(font::FACE_ITALIC, kCaption, muted, im::startAlign());
    gfx::text(T("online.direct.unrated"), im::flipX(Rect(lx, 0, colW, 0), lx), y + 16.0f, us);

    // Connection column.
    im::sectionLabel(T("online.direct.connection"), rx, top + 8.0f, colW);
    float cy = top + 36.0f;
    if (O.hostPortText.empty()) O.hostPortText = std::to_string(gs.directPort);
    std::string portText = O.hostPortText;
    if (im::formField(L("online.direct.port"), portText, Rect(rx, cy, colW, 56.0f), 5, im::FIELD_LTR, "47100"))
        O.hostPortText = digitsOnly(portText);
    im::tooltip(T("online.direct.port.help"));
    cy += 64.0f;
    im::toggleRow(L("online.direct.upnp"), gs.directUpnp, Rect(rx, cy, colW, 56.0f));
    im::tooltip(T("online.direct.upnp.help"));
    cy += 80.0f;
    TextStyle hs = style(font::FACE_ITALIC, kSmall, ivoryDim, im::startAlign());
    gfx::textWrapped(T(gs.directUpnp ? "online.direct.host_note_upnp" : "online.direct.host_note_manual"), im::flipX(Rect(rx, 0, colW, 0), rx),
                     cy, colW, hs, 29.0f);
    int port = std::atoi(O.hostPortText.c_str());
    bool portOk = port >= 1024 && port <= 65535;
    if (!portOk) {
        TextStyle es = style(font::FACE_ITALIC, kSmall, danger, im::startAlign());
        gfx::text(T("online.direct.port_range"), im::flipX(Rect(rx, 0, colW, 0), rx), cy + 130.0f, es);
    }

    im::endInfoMarks();
    footerRule(p);
    bool back = backButton(p);
    bool host = primaryButton(p, "online.direct.host_button", portOk);
    im::popId();
    endPage();
    if (host) {
        gs.directPort = port;
        gs.save();
        net::DirectHostOptions opt;
        opt.port = uint16_t(port);
        opt.upnp = gs.directUpnp;
        if (sel > 0 && sel < n) {
            opt.baseSec = int(presets[size_t(sel)].baseMs / 1000);
            opt.incSec = int(presets[size_t(sel)].incrementMs / 1000);
        } else {
            opt.baseSec = gs.directBaseSeconds;
            opt.incSec = gs.directIncrementSeconds;
        }
        opt.hostColor = gs.directColor;
        opt.autoPress = gs.directAutoPress;
        const std::string& nm = gs.playerName;
        opt.playerName = nm.empty() || nm == "Human" ? std::string(T("player.default_name")) : nm;
        ses().hostDirect(opt);
        O.copiedAt = -100.0f;
        setSub(Sub::DirectWait);
        return;
    }
    if (back || im::consumeBack()) setSub(Sub::Direct);
}

std::string upnpLine(const net::UpnpStatus& u, int port) {
    using S = net::UpnpStatus::State;
    if (u.cgnatSuspected) return T("online.direct.upnp_cgnat");
    switch (u.state) {
    case S::Searching: return T("online.direct.upnp_searching");
    case S::Mapped:
        return i18n::trf("online.direct.upnp_mapped", {u.gatewayName.empty() ? T("online.direct.router") : u.gatewayName});
    case S::NoGateway: return i18n::trf("online.direct.upnp_none", {std::to_string(port)});
    case S::Failed: return i18n::trf("online.direct.upnp_failed", {std::to_string(port)});
    default: return i18n::trf("online.direct.upnp_off", {std::to_string(port)});
    }
}

void pageDirectWait(float t) {
    game::OnlineSession& s = ses();
    game::DirectApi& d = s.direct();
    using DS = net::DirectMatch::State;
    DS st = d.state();
    Rect p = beginPage(t, 1180.0f, 900.0f, T("online.direct.wait_title"));
    im::pushId("directwait");
    float y = p.y + 150.0f;
    bool cancel = false;
    if (st == DS::OpeningPort || st == DS::Idle) {
        y += paragraph(i18n::trf("online.direct.opening", {std::to_string(game::settings().directPort)}), p, y + 60.0f, p.w - 200.0f, ivoryDim);
        spinner(vec2(p.cx(), y + 130.0f), 16.0f);
    } else if (st == DS::Failed) {
        y += paragraph(game::directErrorText(d.lastError()), p, y + 60.0f, p.w - 200.0f, danger);
    } else {
        net::DirectInvite inv = d.invite();
        net::UpnpStatus up = d.upnp();
        TextStyle lead = style(font::FACE_ITALIC, kSmall + 2.0f, ivoryDim, HAlign::Center);
        gfx::text(T("online.direct.give"), p.cx(), y + 6.0f, lead);
        // Address and port.
        float bw = p.w - 200.0f;
        Rect box(p.cx() - bw * 0.5f, y + 30.0f, bw, 300.0f);
        gfx::fill(box, vec4(0, 0, 0, 0.3f), 3.0f);
        gfx::stroke(box, withAlpha(gold, 0.35f), 0.0f, 3.0f);
        float half = bw * 0.5f;
        TextStyle cap = style(font::FACE_TITLE, 17.0f, gold, HAlign::Center, 0.2f);
        TextStyle big = style(font::FACE_TEXT, 40.0f, ivory, HAlign::Center);
        big.dir = 0;
        std::string addr = inv.publicAddress.empty() ? std::string("\xE2\x80\x94") : inv.publicAddress;
        gfx::text(T("online.direct.cap_address"), box.x + half * 0.62f, box.y + 44.0f, cap);
        TextStyle ab = big;
        ab.size = gfx::fitSize(addr, big, half * 1.2f - 30.0f, 0.5f);
        gfx::text(i18n::ltr(addr), box.x + half * 0.62f, box.y + 96.0f, ab);
        gfx::text(T("online.direct.cap_port"), box.r() - half * 0.38f, box.y + 44.0f, cap);
        gfx::text(i18n::ltr(std::to_string(inv.port)), box.r() - half * 0.38f, box.y + 96.0f, big);
        gfx::hlineFade(box.x + 40.0f, box.r() - 40.0f, box.y + 128.0f, withAlpha(gold, 0.25f), 0.3f);
        gfx::text(T("online.direct.cap_code"), box.cx(), box.y + 170.0f, cap);
        TextStyle cs = style(font::FACE_TEXT, 64.0f, goldBright, HAlign::Center, 0.12f);
        cs.dir = 0;
        TextStyle glow = cs;
        glow.color = withAlpha(gold, 0.25f);
        glow.softness = 12.0f;
        glow.weight = 3.0f;
        gfx::text(i18n::ltr(inv.code), box.cx(), box.y + 246.0f, glow);
        gfx::text(i18n::ltr(inv.code), box.cx(), box.y + 246.0f, cs);
        // Copy "address:port code".
        bool copiedRecently = float(im::time()) - O.copiedAt < 2.5f;
        float cw = 170.0f;
        if (im::button(copiedRecently ? T("online.copied") + "##online.copy" : L("online.copy"),
                       im::flip(box, Rect(box.r() - cw - 16.0f, box.b() - 60.0f, cw, 44.0f)), im::ButtonKind::Quiet)) {
            std::string what = (inv.publicAddress.empty() ? std::string() : hostPort(inv.publicAddress, inv.port) + " ") + inv.code;
            if (detail::setClipboardText(what)) O.copiedAt = float(im::time());
            else notify(T("online.copy.unavailable"), 3.0f);
        }
        y = box.b() + 40.0f;
        TextStyle ss = style(font::FACE_ITALIC, kSmall, ivoryDim, HAlign::Center);
        if (!inv.lanAddresses.empty()) {
            std::string lan;
            for (const std::string& a : inv.lanAddresses) lan += (lan.empty() ? "" : ",  ") + a;
            std::string line = i18n::trf("online.direct.lan", {i18n::ltr(lan)});
            ss.size = gfx::fitSize(line, ss, p.w - 160.0f, 0.7f);
            gfx::text(line, p.cx(), y, ss);
            y += 36.0f;
        }
        TextStyle us = ss;
        us.size = kSmall;
        us.color = up.state == net::UpnpStatus::State::Mapped && !up.cgnatSuspected ? vec4(0.62f, 0.78f, 0.55f, 1.0f) : goldBright;
        std::string ul = upnpLine(up, inv.port);
        us.size = gfx::fitSize(ul, us, p.w - 160.0f, 0.7f);
        gfx::text(ul, p.cx(), y, us);
        y += 36.0f;
        TextStyle fs = ss;
        fs.color = muted;
        fs.size = gfx::fitSize(T("online.direct.firewall"), fs, p.w - 160.0f, 0.7f);
        gfx::text(T("online.direct.firewall"), p.cx(), y, fs);
        y += 56.0f;
        std::string wait = T(st == DS::Handshake ? "online.direct.connecting_friend" : st == DS::Playing ? "online.direct.starting"
                                                                                                    : "online.direct.waiting");
        TextStyle ws = style(font::FACE_ITALIC, kBody, ivory, HAlign::Center);
        gfx::text(wait, p.cx(), y, ws);
        float tw = gfx::textWidth(wait, ws);
        spinner(vec2(p.cx() - tw * 0.5f - 34.0f, y - 9.0f));
        spinner(vec2(p.cx() + tw * 0.5f + 34.0f, y - 9.0f));
    }
    footerRule(p);
    cancel = backButton(p, st == DS::Failed ? "common.back" : "common.cancel");
    im::popId();
    endPage();
    if (cancel || im::consumeBack()) {
        s.closeDirect();
        setSub(Sub::DirectHost);
    }
}

void pageDirectJoin(float t) {
    game::OnlineSession& s = ses();
    game::Settings& gs = game::settings();
    using DS = net::DirectMatch::State;
    DS st = O.directJoining ? s.direct().state() : DS::Idle;
    bool connecting = O.directJoining && (st == DS::Connecting || st == DS::Handshake || st == DS::Playing);
    if (O.directJoining && st == DS::Failed) {
        O.error = game::directErrorText(s.direct().lastError());
        O.directJoining = false;
        s.closeDirect();
    }
    Rect p = beginPage(t, 1000.0f, 760.0f, T("online.direct.join_title"));
    im::pushId("directjoin");
    float y = p.y + 150.0f;
    y += paragraph(T("online.direct.join_lead"), p, y, p.w - 200.0f, ivoryDim, kSmall + 2.0f) + 14.0f;
    if (connecting) im::pushBlock();
    im::formField(L("online.direct.address"), O.directAddress, formRow(p, y), 253, im::FIELD_LTR, T("online.direct.address.hint"));
    std::string port = O.directPort;
    if (im::formField(L("online.direct.port"), port, formRow(p, y), 5, im::FIELD_LTR, "47100")) O.directPort = digitsOnly(port);
    std::string code = O.directCode;
    if (im::formField(L("online.direct.code"), code, formRow(p, y), 16, im::FIELD_LTR, "XXXX-XXXX-XXXX")) O.directCode = upperCode(code);
    if (connecting) im::popBlock();
    y += 16.0f;
    if (connecting) {
        std::string line = i18n::trf(st == DS::Connecting ? "online.direct.connecting_to" : "online.direct.securing",
                                     {i18n::ltr(hostPort(trim(O.directAddress), std::atoi(O.directPort.c_str())))});
        TextStyle ws = style(font::FACE_ITALIC, kSmall + 2.0f, ivory, HAlign::Center);
        gfx::text(line, p.cx(), y + 10.0f, ws);
        spinner(vec2(p.cx() - gfx::textWidth(line, ws) * 0.5f - 30.0f, y + 1.0f));
    } else {
        messageLine(p, y + 10.0f);
    }
    footerRule(p);
    bool back = backButton(p, connecting ? "common.cancel" : "common.back");
    int portNum = std::atoi(O.directPort.c_str());
    std::string raw;
    for (char c : O.directCode)
        if (c != '-') raw += c;
    bool ready = !trim(O.directAddress).empty() && portNum > 0 && portNum <= 65535 && raw.size() == 12;
    if (primaryButton(p, "online.direct.join_button", ready && !connecting, connecting)) {
        gs.directAddress = trim(O.directAddress);
        gs.directJoinPort = portNum;
        gs.save();
        O.error.clear();
        s.joinDirect(trim(O.directAddress), uint16_t(portNum), O.directCode);
        O.directJoining = true;
    }
    im::popId();
    endPage();
    if (back || im::consumeBack()) {
        if (connecting) {
            s.closeDirect();
            O.directJoining = false;
        } else {
            setSub(Sub::Direct);
        }
    }
}

}  // namespace

// ==== Hooks ================================================================================================
namespace detail {

bool onlineGameStarting() { return game::onlineSession().gameReady(); }

MenuAction onlinePage(LibrarySetup* library, float t, bool opened, bool& back) {
    game::OnlineSession& s = ses();
    MenuAction act = MenuAction::None;
    if (opened) {
        O.leave = false;
        if (!O.forced.empty()) {
            // set by debug::openOnlinePage
        } else if (O.haveAfterGame) {
            setSub(O.afterGame);
        } else if (!s.serverConfigured()) {
            setSub(Sub::NoServer);
        } else {
            if (!s.signedIn()) s.resume();
            setSub(s.signedIn() ? Sub::Play : Sub::SignIn);
        }
        O.forced.clear();
        O.haveAfterGame = false;
        if (s.serverConfigured() && !s.infoKnown() && !s.busy(Kind::ServerInfoResult)) s.refreshInfo();
    }
    pumpResults();
    {
        // The answers of the account API (any page), before the check below: a deleted account
        // goes to the sign-in page with its note.
        AccountPage page;
        const bool onAccountPage = accountSub(O.sub, page);
        std::string note, error;
        AccountNav nav = accountPump(onAccountPage ? &page : nullptr, note, error);
        if (nav == AccountNav::SignIn) {
            clearSecrets();
            setSub(s.serverConfigured() ? Sub::SignIn : Sub::NoServer);
            O.note = note;
        } else if (nav == AccountNav::Account) {
            setSub(Sub::Account);
            O.note = note;
        }
        if (!error.empty()) O.error = error;
    }
    // A game is starting: come back to this page after it.
    if (s.gameReady()) {
        O.afterGame = isDirect(O.sub) ? Sub::Direct : s.signedIn() ? Sub::Play : Sub::SignIn;
        O.haveAfterGame = true;
        O.directJoining = false;
    }
    if (needsAccount(O.sub) && !s.signedIn() && !s.busy(Kind::LoginResult)) {
        std::string err = O.error;
        setSub(s.serverConfigured() ? Sub::SignIn : Sub::NoServer);
        O.error = err;
    }
    O.t = std::min(1.0f, O.t + im::dt() / 0.3f);
    float pt = std::min(t, ease(O.t));
    bool fresh = O.fresh;
    im::pushId("online");
    im::pushId(int(O.sub));
    switch (O.sub) {
    case Sub::NoServer: pageNoServer(pt); break;
    case Sub::SignIn: pageSignIn(pt); break;
    case Sub::Mfa: pageMfa(pt); break;
    case Sub::Register: pageRegister(pt); break;
    case Sub::CheckEmail: pageCheckEmail(pt); break;
    case Sub::Forgot: pageForgot(pt); break;
    case Sub::SsoWait: pageSsoWait(pt); break;
    case Sub::SsoName: pageSsoName(pt); break;
    case Sub::Account: pageAccount(pt); break;
    case Sub::Password: pagePassword(pt); break;
    case Sub::MfaSetup: pageMfaSetup(pt); break;
    case Sub::MfaOff: pageMfaOff(pt, false); break;
    case Sub::Recovery: pageMfaOff(pt, true); break;
    case Sub::History:
    case Sub::Game:
    case Sub::Devices:
    case Sub::Email:
    case Sub::Export:
    case Sub::Delete: {
        AccountPage page = AccountPage::History;
        accountSub(O.sub, page);
        std::string note;
        AccountNav nav = accountPage(page, pt, fresh, library, act, note);
        switch (nav) {
        case AccountNav::Account:
            setSub(Sub::Account);
            O.note = note;
            break;
        case AccountNav::History: setSub(Sub::History); break;
        case AccountNav::Game:
            accountReset(AccountPage::Game);
            setSub(Sub::Game);
            break;
        case AccountNav::SignIn:
            clearSecrets();
            setSub(Sub::SignIn);
            O.note = note;
            break;
        case AccountNav::Stay: break;
        }
        // A replay started from a game of the history: back to that game after it.
        if (act == MenuAction::StartReplay) {
            O.afterGame = Sub::Game;
            O.haveAfterGame = true;
        }
        break;
    }
    case Sub::Play: pagePlay(pt); break;
    case Sub::Challenge: pageChallenge(pt, false); break;
    case Sub::Private: pageChallenge(pt, true); break;
    case Sub::Direct: pageDirect(pt); break;
    case Sub::DirectHost: pageDirectHost(pt); break;
    case Sub::DirectWait: pageDirectWait(pt); break;
    case Sub::DirectJoin: pageDirectJoin(pt); break;
    }
    im::popId();
    im::popId();
    if (fresh && O.fresh) O.fresh = false;
    if (O.leave) {
        O.leave = false;
        s.cancelSearch();
        im::sound(Sound::Back);
        back = true;
    }
    return act;
}

void onlineMenuOverlay(bool onOnlinePage) {
    game::OnlineSession& s = ses();
    if (onOnlinePage && s.signedIn() && !isDirect(O.sub) && O.sub != Sub::NoServer)
        pingIndicator(s.pingMs(), s.conn() != net::ConnState::Online);
    if (s.signedIn()) onlineChallenges();
}

// ---- Options > Online ----------------------------------------------------------------------------------
void onlineOptionsRows(game::Settings& s, float rx, float rw, float& y) {
    game::OnlineSession& ses = game::onlineSession();
    bool inGame = ses.link() != nullptr;
    net::ServerEndpoint off = net::officialServer();
    bool official = off.valid();
    im::pushId("online");
    const float rh = 54.0f;
    auto row = [&](float h) {
        Rect r(rx, y, rw, h - 4.0f);
        y += h;
        return r;
    };
    // Server: its label, then two choices side by side (the official one first, with its address).
    {
        Rect r = row(64.0f);
        bool customNow = !official || s.onlineCustomServer;
        const std::string help = T("options.online.server.help");
        float cx = r.x + im::formLabel(L("options.online.server"), Rect(r.x, r.y, rw, 56.0f), rw * 0.72f);
        im::tooltip(help);
        float cw = r.r() - cx;
        float w1 = official ? (cw - 14.0f) * 0.64f : 0.0f, w2 = official ? cw - 14.0f - w1 : cw;
        struct Choice { std::string text; Rect r; bool sel, enabled; int id; };
        Choice cs[2] = {
            {official ? i18n::trf("options.online.official", {i18n::ltr(hostPort(off.host, off.apiPort))}) : std::string(),
             im::flip(r, Rect(cx, r.y, w1, 56.0f)), !customNow, official && !inGame, 0},
            {T("options.online.community"), im::flip(r, Rect(cx + (official ? w1 + 14.0f : 0.0f), r.y, w2, 56.0f)), customNow,
             official && !inGame, 1},
        };
        for (const Choice& c : cs) {
            if (c.text.empty()) continue;
            im::Item it = im::item(im::makeId(std::string("##options.online.server") + char('0' + c.id)), c.r,
                                   c.enabled ? im::ITEM_FOCUSABLE : im::ITEM_DISABLED);
            im::tooltip(help);
            gfx::fill(c.r, vec4(0, 0, 0, 0.25f), 2.0f);
            if (c.sel) {
                gfx::fillV(c.r, withAlpha(gold, 0.16f), withAlpha(gold, 0.06f), 2.0f);
                gfx::stroke(c.r, withAlpha(gold, 0.7f), 0.0f, 2.0f);
            } else {
                gfx::fill(c.r, withAlpha(gold, 0.08f * it.hoverT), 2.0f);
                gfx::stroke(c.r, withAlpha(gold, 0.18f + 0.4f * it.hoverT), 0.0f, 2.0f);
            }
            TextStyle ts = style(font::FACE_TEXT, kBody, c.sel ? goldBright : theme::mix(ivory, goldBright, it.hoverT * 0.6f), HAlign::Center);
            ts.size = gfx::fitSize(c.text, ts, c.r.w - 24.0f, 0.6f);
            gfx::text(c.text, c.r.cx(), c.r.cy() + gfx::capHeight(ts) * 0.5f, ts);
            if (it.activated && c.enabled && !c.sel) {
                s.onlineCustomServer = c.id == 1;
                O.testShown = false;
                im::sound(Sound::Toggle);
            }
        }
    }
    bool custom = !official || s.onlineCustomServer;
    TextStyle cs = style(font::FACE_ITALIC, kCaption, muted, im::startAlign());
    Rect area(rx, 0, rw, 0);
    if (custom) {
        bool en = !inGame;
        std::string host = s.onlineHost;
        if (im::formField(L("options.online.host"), host, row(rh), 253, im::FIELD_LTR, T("options.online.host.hint"), en)) {
            s.onlineHost = trim(host);
            O.testShown = false;
        }
        im::tooltip(T("options.online.host.help"));
        // The two ports side by side.
        Rect pr = row(rh);
        float half = (rw - 20.0f) * 0.5f;
        std::string api = s.onlineApiPort > 0 ? std::to_string(s.onlineApiPort) : "";
        if (im::formField(L("options.online.api_port"), api, im::flip(pr, Rect(pr.x, pr.y, half, pr.h)), 5, im::FIELD_LTR, "443", en)) {
            s.onlineApiPort = std::clamp(std::atoi(digitsOnly(api).c_str()), 0, 65535);
            O.testShown = false;
        }
        im::tooltip(T("options.online.api_port.help"));
        std::string ws = s.onlineWsPort > 0 ? std::to_string(s.onlineWsPort) : "";
        if (im::formField(L("options.online.ws_port"), ws, im::flip(pr, Rect(pr.x + half + 20.0f, pr.y, half, pr.h)), 5, im::FIELD_LTR,
                          T("options.online.ws_port.hint"), en)) {
            s.onlineWsPort = std::clamp(std::atoi(digitsOnly(ws).c_str()), 0, 65535);
            O.testShown = false;
        }
        im::tooltip(T("options.online.ws_port.help"));
        std::string pin = s.onlinePin;
        if (im::formField(L("options.online.pin"), pin, row(rh), 95, im::FIELD_LTR, T("options.online.pin.hint"), en)) {
            s.onlinePin = trim(pin);
            O.testShown = false;
        }
        im::tooltip(T("options.online.pin.help"));
        // 64 hexadecimal digits, colons and spaces allowed (the client normalises them).
        std::string hex;
        bool hexOk = true;
        for (char c : s.onlinePin) {
            if (c == ':' || c == ' ') continue;
            if (!std::isxdigit(static_cast<unsigned char>(c))) hexOk = false;
            hex += c;
        }
        if (!hex.empty() && (!hexOk || hex.size() != 64)) {
            TextStyle ws2 = cs;
            ws2.color = danger;
            ws2.size = gfx::fitSize(T("options.online.pin.invalid"), ws2, rw - 20.0f, 0.7f);
            gfx::text(T("options.online.pin.invalid"), im::flipX(area, rx + 10.0f), y + 6.0f, ws2);
            y += 30.0f;
        }
    }
    y += 14.0f;
    // Test connection (the values on the page, applied or not).
    net::ServerEndpoint ep;
    if (!custom) {
        ep = off;
    } else {
        ep.host = s.onlineHost;
        ep.apiPort = uint16_t(std::clamp(s.onlineApiPort, 0, 65535));
        ep.wsPort = s.onlineWsPort > 0 ? uint16_t(s.onlineWsPort) : ep.apiPort;
        ep.pinnedSha256 = s.onlinePin;
    }
    net::Event e;
    if (ses.takeTest(e)) {
        O.testShown = true;
        O.testOk = e.ok && e.info.compatible;
        if (e.ok) {
            O.testLine = i18n::trf(e.info.compatible ? "options.online.test_ok" : "options.online.test_incompatible",
                                   {e.info.name.empty() ? i18n::ltr(ep.host) : e.info.name});
            O.testDetail = e.info.motd;
        } else {
            O.testLine = game::onlineErrorText(e.error, e.retryAfterSec);
            O.testDetail.clear();
        }
    }
    Rect tr = row(64.0f);
    float bw = 280.0f;
    bool testing = ses.testing();
    if (im::button(L("options.online.test"), im::flip(tr, Rect(tr.x, tr.y + 2.0f, bw, 52.0f)), im::ButtonKind::Secondary,
                   ep.valid() && !testing && !inGame)) {
        O.testShown = false;
        ses.testServer(ep);
    }
    float tx = tr.x + bw + 26.0f, tw = rw - bw - 30.0f;
    Rect tcol(tx, 0, tw, 0);
    Rect tcolF = im::flip(tr, tcol);
    if (testing) {
        spinner(vec2(im::flipX(tr, tx + 16.0f), tr.y + 28.0f));
    } else if (O.testShown) {
        TextStyle rs = style(font::FACE_TEXT, kSmall, O.testOk ? vec4(0.62f, 0.78f, 0.55f, 1.0f) : danger, im::startAlign());
        rs.size = gfx::fitSize(O.testLine, rs, tw, 0.7f);
        gfx::text(im::elideToFit(O.testLine, rs, tw), im::flipX(tcolF, tcolF.x), tr.y + (O.testDetail.empty() ? 36.0f : 24.0f), rs);
        if (!O.testDetail.empty()) {
            TextStyle ms = style(font::FACE_ITALIC, kCaption, ivoryDim, im::startAlign());
            std::string motd = O.testDetail.substr(0, O.testDetail.find('\n'));
            ms.size = gfx::fitSize(motd, ms, tw, 0.7f);
            gfx::text(im::elideToFit(motd, ms, tw), im::flipX(tcolF, tcolF.x), tr.y + 52.0f, ms);
        }
    } else if (inGame) {
        TextStyle gs = style(font::FACE_ITALIC, kCaption, muted, im::startAlign());
        gs.size = gfx::fitSize(T("options.online.in_game"), gs, tw, 0.7f);
        gfx::text(T("options.online.in_game"), im::flipX(tcolF, tcolF.x), tr.y + 34.0f, gs);
    }
    im::popId();
}

void copyOnlineOptions(game::Settings& dst, const game::Settings& src) {
    dst.onlineCustomServer = src.onlineCustomServer;
    dst.onlineHost = trim(src.onlineHost);
    dst.onlineApiPort = src.onlineApiPort > 0 ? src.onlineApiPort : 443;
    dst.onlineWsPort = src.onlineWsPort;
    dst.onlinePin = trim(src.onlinePin);
}

bool sameOnlineOptions(const game::Settings& a, const game::Settings& b) {
    return a.onlineCustomServer == b.onlineCustomServer && trim(a.onlineHost) == trim(b.onlineHost) &&
           a.onlineApiPort == b.onlineApiPort && a.onlineWsPort == b.onlineWsPort && trim(a.onlinePin) == trim(b.onlinePin);
}

bool onlineServerChanged(const game::Settings& before, const game::Settings& after) { return !sameOnlineOptions(before, after); }

}  // namespace detail

namespace debug {
void openOnlinePage(const std::string& sub) {
    AccountPage page;
    if (accountDebugOpen(sub, page)) {
        setSub(subOf(page));
        O.forced = sub;
        return;
    }
    static const struct { const char* name; Sub sub; } names[] = {
        {"noserver", Sub::NoServer}, {"signin", Sub::SignIn},       {"mfa", Sub::Mfa},           {"register", Sub::Register},
        {"check-email", Sub::CheckEmail}, {"forgot", Sub::Forgot},  {"account", Sub::Account},   {"password", Sub::Password},
        {"mfa-setup", Sub::MfaSetup}, {"mfa-off", Sub::MfaOff},     {"recovery", Sub::Recovery}, {"play", Sub::Play},
        {"search", Sub::Play},        {"challenge", Sub::Challenge}, {"private", Sub::Private},  {"direct", Sub::Direct},
        {"direct-host", Sub::DirectHost}, {"direct-wait", Sub::DirectWait}, {"direct-join", Sub::DirectJoin},
    };
    for (const auto& n : names) {
        if (sub != n.name) continue;
        setSub(n.sub);
        O.forced = sub;
        game::OnlineSession& s = ses();
        game::Settings& gs = game::settings();
        if (n.sub == Sub::Challenge || n.sub == Sub::Private) {
            O.terms.tc = categoryIndex(gs.onlineCategory);
            O.terms.rated = gs.onlineRated;
            if (n.sub == Sub::Challenge) O.target = "Eleonora_V";
        }
        if (n.sub == Sub::DirectHost) O.hostPortText = std::to_string(gs.directPort);
        if (n.sub == Sub::DirectJoin) {
            O.directAddress = "203.0.113.47";
            O.directPort = "47100";
            O.directCode = "K7Q2-M9XH-3PTR";
        }
        if (n.sub == Sub::MfaSetup) {
            // Straight to the QR code step.
            s.api().mfaSetup("viewer-password");
            s.expect(Kind::MfaSetupResult);
            s.runMock(1000.0);
            pumpResults();
        }
        if (n.sub == Sub::Recovery) {
            s.api().regenerateRecoveryCodes("viewer-password", "123456");
            s.expect(Kind::RecoveryCodesResult);
            s.runMock(1000.0);
            pumpResults();
        }
        if (sub == "search") {
            s.findOpponent(gs.onlineCategory, true);
            s.runMock(600.0);
        }
        if (n.sub == Sub::DirectWait) {
            net::DirectHostOptions opt;
            opt.port = uint16_t(gs.directPort);
            opt.autoPress = gs.directAutoPress;
            opt.playerName = "Olivier";
            s.hostDirect(opt);
            s.runMock(1500.0);
        }
        return;
    }
}
}  // namespace debug

}  // namespace ui
