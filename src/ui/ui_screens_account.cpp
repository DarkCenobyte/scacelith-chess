// The account API's pages of "Play Online" (ui_online_pages.h), opened from the account page:
//   - game history: the player's games on this server, newest first, ten a page (Previous / Next,
//     "page 2 of 5"), filtered by kind (all, rated, casual) and result (won, lost, drawn); each row
//     shows the local date and time, the colour played, the opponent and their rating, the result
//     from the player's side (coloured), the time control, rated or casual, and the rating change;
//   - a game of the history: the players with their ratings and changes, the result and how the
//     game ended in words, date, time control, the moves in figurine notation with the clocks
//     (scrollable); Save to saved games (the server's PGN read as untrusted input and saved once
//     per game and server: game::archive::saveServerGame, off the UI thread), Replay (saves it
//     first when needed, then replays the file like the Saved games page: MenuAction::StartReplay),
//     Report opponent while the server accepts a report of that game (the report dialog of the
//     online games);
//   - signed-in devices: each session of the account (its client's label or "Unknown device",
//     signed in, last active, "This device"), Sign out per other device, Sign out everywhere;
//   - change of e-mail: the new address, the password and, with two-factor on, a code (a recovery
//     code works too); the link sent to the new address, or the address changed at once on a
//     server without e-mail confirmation;
//   - download my data: the JSON document of the server written (never over a file) to
//     <app data>/account/<host>_<user>_<date>.json, its path shown with Open folder;
//   - delete the account: what it means (ratings and games stay under an anonymous name,
//     "deleted#<id>"; cannot be undone), the user name typed to confirm, the password (and code).
// The answers of the server arrive on any page: accountPump() takes them every frame. Same look
// as the other online pages; mirrored with im::flip / im::flipX in a right-to-left language.
#include "ui_online_pages.h"
#include "ui.h"
#include "ui_draw.h"
#include "ui_internal.h"
#include "ui_online.h"
#include "ui_screens_game.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "../coach/catalog.h"
#include "../core/log.h"
#include "../game/game_archive.h"
#include "../game/game_saving.h"
#include "../game/online_account.h"
#include "../game/online_session.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include "../platform/platform.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <future>

namespace ui {

using gfx::HAlign;
using gfx::Rect;
using gfx::TextStyle;
using m::vec2;
using m::vec4;
using namespace theme;

namespace detail {
namespace onl {

namespace {

using Kind = net::Event::Kind;
namespace archive = game::archive;

const vec4 kWon(0.47f, 0.74f, 0.44f, 1.0f);   // the green of "online" in the server line
const char* const kDot = "  \xC2\xB7  ";
const char* const kEllipsis = "\xE2\x80\xA6";

// ---- Texts ------------------------------------------------------------------------------------------
std::string num(long long v) { return std::to_string(v); }

bool localTime(int64_t ms, std::tm& out) {
    const std::time_t t = std::time_t(ms / 1000);
#ifdef _WIN32
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}
// "2026-09-27  21:47" (local time), left to right whatever the language.
std::string dateTimeText(int64_t ms) {
    std::tm tm{};
    if (ms <= 0 || !localTime(ms, tm)) return "\xE2\x80\x94";
    char buf[48];
    std::strftime(buf, sizeof buf, "%Y-%m-%d  %H:%M", &tm);
    return i18n::ltr(buf);
}
std::string dateText(int64_t ms) {
    std::tm tm{};
    if (ms <= 0 || !localTime(ms, tm)) return "\xE2\x80\x94";
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return i18n::ltr(buf);
}

// s cut at the end (whole characters, "…" added) to fit maxWidth in st.
std::string elide(const std::string& s, const TextStyle& st, float maxWidth) {
    if (gfx::textWidth(s, st) <= maxWidth) return s;
    std::u32string cps = uni::decode(s);
    size_t lo = 0, hi = cps.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (gfx::textWidth(uni::encode(cps.substr(0, mid)) + kEllipsis, st) <= maxWidth) lo = mid;
        else hi = mid - 1;
    }
    return uni::encode(cps.substr(0, lo)) + kEllipsis;
}
// Fits s: a little smaller first, then cut.
void fitOrElide(std::string& s, TextStyle& st, float maxWidth, float minScale = 0.85f) {
    st.size = gfx::fitSize(s, st, maxWidth, minScale);
    s = elide(s, st, maxWidth);
}

std::string outcomeText(game::Outcome o) {
    switch (o) {
    case game::Outcome::Win: return T("online.history.won");
    case game::Outcome::Loss: return T("online.history.lost");
    case game::Outcome::Draw: return T("online.history.drawn");
    case game::Outcome::Aborted: return T("online.history.aborted");
    default: return "\xE2\x80\x94";
    }
}
vec4 outcomeColor(game::Outcome o) {
    switch (o) {
    case game::Outcome::Win: return kWon;
    case game::Outcome::Loss: return danger;
    case game::Outcome::Draw: return goldBright;
    default: return muted;
    }
}
// "+9", "−12", "±0".
std::string diffText(int d) {
    if (d > 0) return i18n::ltr("+" + num(d));
    if (d < 0) return i18n::ltr("\xE2\x88\x92" + num(-d));
    return i18n::ltr("\xC2\xB1" "0");
}
vec4 diffColor(int d) { return d > 0 ? kWon : d < 0 ? danger : muted; }
std::string tcText(const net::GameSummary& g) { return i18n::ltr(game::timeControlLabel(g.baseMs, g.incMs)); }
// "1–0", "0–1", "½–½", "*".
std::string resultText(const std::string& r) {
    if (r == "1-0") return "1\xE2\x80\x93" "0";
    if (r == "0-1") return "0\xE2\x80\x93" "1";
    if (r == "1/2-1/2") return "\xC2\xBD\xE2\x80\x93\xC2\xBD";
    return "*";
}
// How the game ended, in words ("" when unknown). The online reasons are worded for the game over
// card of the player who stayed; a game the player left says so.
std::string reasonWords(const net::GameSummary& g) {
    if (g.reason == 20 && game::outcomeOf(g) == game::Outcome::Loss) return T("online.game.you_left");
    const std::string key = game::saving::onlineEndKey(g.reason);
    return key.empty() || !i18n::has(key.c_str()) ? std::string() : T(key.c_str());
}
// A clock: "4:59", "1:02:31", "0:07.3" under ten seconds.
std::string clockText(int64_t ms) {
    if (ms < 0) return std::string();
    char buf[32];
    const int64_t s = ms / 1000;
    if (ms < 10000) std::snprintf(buf, sizeof buf, "0:%02lld.%lld", (long long)s, (long long)(ms / 100 % 10));
    else if (s >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
    else std::snprintf(buf, sizeof buf, "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    return i18n::ltr(buf);
}
std::string errorText(const net::Event& e) { return game::onlineErrorText(e.error, e.retryAfterSec); }

// A piece colour marker: a small square, ivory or ebony.
void colourMark(vec2 c, int colour, float size = 15.0f) {
    Rect r(c.x - size * 0.5f, c.y - size * 0.5f, size, size);
    if (colour == 0) {
        gfx::fill(r, vec4(0.90f, 0.87f, 0.80f, 1.0f), 2.0f);
    } else {
        gfx::fill(r, vec4(0.07f, 0.06f, 0.055f, 1.0f), 2.0f);
        gfx::stroke(r, withAlpha(ivory, 0.55f), 2.0f, 1.2f);
    }
}

// Scroll of a clipped area by the wheel over it (and PageUp / PageDown when 'keys').
float wheelScroll(float& scroll, float& target, const Rect& area, float contentH, float step, bool opened, bool keys) {
    float maxScroll = std::max(0.0f, contentH - area.h);
    if (area.contains(im::mouse()) && im::wheel() != 0.0f) target -= im::wheel() * step;
    if (keys && im::keyPressed(plat::KEY_PAGEDOWN)) target += area.h * 0.8f;
    if (keys && im::keyPressed(plat::KEY_PAGEUP)) target -= area.h * 0.8f;
    target = m::clamp(target, 0.0f, maxScroll);
    scroll = opened ? target : std::min(im::approach(scroll, target, 16.0f), maxScroll);
    return scroll;
}
// Scroll bar on the end side and fades at the edges (panel colour), as the saved games.
void scrollDecor(const Rect& area, float scroll, float contentH) {
    float maxScroll = contentH - area.h;
    if (maxScroll <= 0.5f) return;
    float bh = std::max(24.0f, area.h * area.h / contentH);
    Rect bar = im::flip(area, Rect(area.r() + 10.0f, area.y + (area.h - bh) * (scroll / maxScroll), 2.0f, bh));
    gfx::fill(bar, withAlpha(gold, 0.35f), 1.0f);
    vec4 pc(0.05f, 0.043f, 0.039f, 0.95f), pz(0.05f, 0.043f, 0.039f, 0.0f);
    if (scroll > 0.5f) gfx::fillV(Rect(area.x, area.y, area.w, 22.0f), pc, pz);
    if (scroll < maxScroll - 0.5f) gfx::fillV(Rect(area.x, area.b() - 22.0f, area.w, 22.0f), pz, pc);
}

// A centred message in an area: a heading and a wrapped text under it.
void message(const Rect& area, const std::string& head, const std::string& text, vec4 headColor) {
    TextStyle hs = style(font::FACE_TITLE, 24.0f, headColor, HAlign::Center, 0.14f);
    TextStyle ts = style(font::FACE_ITALIC, 23.0f, ivoryDim, HAlign::Center);
    const float lineH = 32.0f, w = std::min(area.w - 40.0f, 860.0f);
    int lines = text.empty() ? 0 : gfx::wrapLineCount(text, w, ts);
    float h = 34.0f + float(lines) * lineH;
    float y = area.y + std::max(40.0f, (area.h - h) * 0.42f);
    const std::string& hd = head;  // written in capitals in the language files
    hs.size = gfx::fitSize(hd, hs, w);
    gfx::text(hd, area.cx(), y, hs);
    gfx::diamond(vec2(area.cx(), y + 26.0f), 3.5f, withAlpha(gold, 0.7f));
    if (!text.empty()) gfx::textWrapped(text, area.cx(), y + 68.0f, w, ts, lineH);
}

// Error (red) or note (ivory) centred under a form; returns the height used.
float messageLine(const Rect& p, float y, const std::string& error, const std::string& note) {
    if (!error.empty()) return paragraph(error, p, y, p.w - 200.0f, danger, kSmall + 1.0f);
    if (!note.empty()) return paragraph(note, p, y, p.w - 200.0f, ivoryDim, kSmall + 1.0f);
    return 0.0f;
}

// ---- State ------------------------------------------------------------------------------------------
enum class Save { Unknown, Checking, NotSaved, Downloading, Writing, Saved, Failed };

struct State {
    // history
    int kindFilter = 0;      // 0 all, 1 rated, 2 casual
    int resultFilter = 0;    // 0 all, 1 won, 2 lost, 3 drawn
    bool reloadHistory = true;
    // a game: saved games, replay, report
    std::string saveFolder;  // the saved games (the menu's LibrarySetup)
    uint64_t saveId = 0;     // the game the save state is about
    Save save = Save::Unknown;
    std::future<archive::ServerSaveResult> saveJob;
    std::string savedPath;
    int savedIndex = 0;
    bool replayWanted = false;
    bool reportOpen = false;
    int reportCategory = 0;
    std::string reportComment;
    std::vector<uint64_t> reported;
    float movesScroll = 0.0f, movesTarget = 0.0f;
    // devices
    bool reloadDevices = true;
    int64_t revoking = 0;
    bool confirmAll = false;
    // forms
    std::string newEmail, password, code, confirmName;
    std::string error, note;
    std::string sentTo;              // e-mail change: the link went to this address
    std::future<archive::SaveResult> exportJob;
    bool exportWriting = false;
    std::string exportPath;          // the data export: the file written
};
State& st() {
    static State s;
    return s;
}
void clearSecrets() {
    State& s = st();
    s.password.clear();
    s.code.clear();
}

bool ready(const std::future<archive::ServerSaveResult>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}
bool ready(const std::future<archive::SaveResult>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

archive::ServerGame serverGameOf(const net::GameDetails& g) {
    archive::ServerGame sg;
    sg.server = ses().endpoint().origin();
    sg.gameId = g.id;
    if (g.status != 0) sg.endKey = game::saving::onlineEndKey(g.reason);
    return sg;
}

// Saved games: looks (pgnText empty) or writes, off the UI thread.
void startSave(const std::string& folder, const net::GameDetails& g, const std::string& pgnText) {
    State& s = st();
    s.save = pgnText.empty() ? Save::Checking : Save::Writing;
    const archive::ServerGame sg = serverGameOf(g);
    s.saveJob = std::async(std::launch::async, [folder, sg, pgnText]() {
        try {
            return archive::saveServerGame(folder, sg, pgnText);
        } catch (const std::exception& ex) {
            archive::ServerSaveResult r;
            r.status = archive::ServerSaveStatus::Failed;
            r.error = ex.what();
            return r;
        }
    });
}

net::GamesFilter filterOf(const State& s) {
    net::GamesFilter f;
    f.rated = s.kindFilter == 1 ? 1 : s.kindFilter == 2 ? 0 : -1;
    static const char* const results[] = {"", "win", "loss", "draw"};
    f.result = results[std::clamp(s.resultFilter, 0, 3)];
    return f;
}

bool mfaOn() { return ses().account().mfaEnabled; }

// The re-authentication fields of the account changes: the password, and a code with two-factor on.
void reauthFields(const Rect& p, float& y, float inset = 160.0f) {
    State& s = st();
    im::formField(L("online.field.password"), s.password, formRow(p, y, inset), 128, im::FIELD_SECRET);
    if (mfaOn())
        im::formField(L("online.field.code"), s.code, formRow(p, y, inset), 14, im::FIELD_LTR, T("online.field.code.hint"));
}
bool reauthFilled() {
    const State& s = st();
    return !s.password.empty() && (!mfaOn() || trim(s.code).size() >= 6);
}
std::string reauthCode() { return mfaOn() ? trim(st().code) : std::string(); }

// ---- Game history -------------------------------------------------------------------------------------
AccountNav pageHistory(float t, bool fresh) {
    State& s = st();
    game::OnlineSession& se = ses();
    const game::AccountData& data = se.accountData();
    const game::HistoryPager& h = data.history;
    if (fresh && (s.reloadHistory || (!h.loaded() && !h.waiting()))) {
        se.loadHistory(filterOf(s));
        s.reloadHistory = false;
    }
    AccountNav nav = AccountNav::Stay;
    Rect p = beginPage(t, 1640.0f, 1000.0f, T("online.history.title"));
    serverLine(p, true);
    im::pushId("history");
    const float pad = 64.0f, x0 = p.x + pad, cw = p.w - 2.0f * pad;
    const float top = p.y + 140.0f;

    // Filters.
    {
        const float fw = (cw - 40.0f) * 0.5f;
        int kind = s.kindFilter, result = s.resultFilter;
        bool changed = im::selectorRow(L("online.history.filter.games"), kind,
                                       {T("online.history.filter.all"), T("online.rated"), T("online.casual")},
                                       im::flip(p, Rect(x0, top, fw, 50.0f)));
        changed |= im::selectorRow(L("online.history.filter.result"), result,
                                   {T("online.history.filter.all"), T("online.history.filter.won"), T("online.history.filter.lost"),
                                    T("online.history.filter.drawn")},
                                   im::flip(p, Rect(x0 + fw + 40.0f, top, fw, 50.0f)));
        if (changed && (kind != s.kindFilter || result != s.resultFilter)) {
            s.kindFilter = kind;
            s.resultFilter = result;
            se.loadHistory(filterOf(s));
        }
    }

    // Columns (fractions of the width, from the start side).
    const float cDate = 0.0f, cMark = 0.152f, cOpp = 0.172f, cResult = 0.45f, cTc = 0.565f, cKind = 0.705f, cRating = 0.81f;
    const Rect cols(x0, 0, cw, 0);
    auto at = [&](float frac) { return im::flipX(cols, x0 + cw * frac); };
    const float headY = top + 102.0f;
    {
        TextStyle hs = style(font::FACE_TITLE, 16.0f, withAlpha(gold, 0.85f), im::startAlign(), 0.16f);
        const struct { float x; const char* key; float w; } heads[] = {
            {cDate, "online.history.col.date", cMark - cDate}, {cOpp, "online.history.col.opponent", cResult - cOpp},
            {cResult, "online.history.col.result", cTc - cResult}, {cTc, "online.history.col.tc", cKind - cTc},
            {cKind, "online.history.col.kind", cRating - cKind}, {cRating, "online.history.col.rating", 0.97f - cRating},
        };
        for (const auto& hd : heads) {
            TextStyle ts = hs;
            const std::string label = T(hd.key);
            ts.size = gfx::fitSize(label, ts, cw * hd.w - 14.0f, 0.7f);
            gfx::text(label, at(hd.x), headY, ts);
        }
        gfx::hlineFade(x0, x0 + cw, headY + 14.0f, withAlpha(gold, 0.3f), 0.15f);
    }
    const float rowH = 54.0f;
    const Rect list(x0, headY + 24.0f, cw, rowH * float(game::HistoryPager::kPageSize));
    const net::GamesPage& page = h.page();
    const bool filtered = s.kindFilter != 0 || s.resultFilter != 0;
    if (!h.loaded()) {
        if (!h.error().empty()) {
            const bool missing = h.error() == "not_found" || h.error() == "not_implemented";
            message(list, T(missing ? "online.history.unavailable" : "online.history.error"),
                    missing ? std::string() : game::onlineErrorText(h.error()), danger);
            if (!missing && linkButton("online.retry", list.cx(), list.y + list.h * 0.42f + 150.0f)) se.historyReload();
        } else {
            spinner(vec2(list.cx(), list.y + 120.0f), 16.0f);
        }
    } else if (page.games.empty()) {
        message(list, T(filtered ? "online.history.empty_filter.title" : "online.history.empty.title"),
                T(filtered ? "online.history.empty_filter" : "online.history.empty"), goldBright);
    } else {
        gfx::pushAlpha(h.waiting() ? 0.45f : 1.0f);
        for (size_t i = 0; i < page.games.size(); ++i) {
            const net::GameSummary& g = page.games[i];
            const Rect r(x0, list.y + float(i) * rowH, cw, rowH - 6.0f);
            im::Item it = im::item(im::makeId(std::to_string(g.id)), r, h.waiting() ? im::ITEM_DISABLED : im::ITEM_FOCUSABLE);
            if (i % 2 == 1) gfx::fill(r, withAlpha(gold, 0.03f), 2.0f);
            gfx::fill(r, withAlpha(gold, 0.07f * it.hoverT), 2.0f);
            if (it.hoverT > 0.01f) gfx::stroke(r, withAlpha(gold, 0.45f * it.hoverT), 0.0f, 1.5f);
            const float by = r.cy() + 8.0f;
            // Date.
            TextStyle ds = style(font::FACE_TEXT, 21.0f, ivoryDim, im::startAlign());
            std::string date = dateTimeText(g.endedAtMs);
            ds.size = gfx::fitSize(date, ds, cw * (cMark - cDate) - 12.0f, 0.75f);
            gfx::text(date, at(cDate + 0.004f), by, ds);
            // Colour, opponent and rating.
            const int you = g.you == 1 ? 1 : 0;
            colourMark(vec2(at(cMark + 0.006f), r.cy()), you);
            const net::GameSide& opp = you == 0 ? g.black : g.white;
            TextStyle ns = style(font::FACE_TEXT, 24.0f, opp.name.compare(0, 8, "deleted#") == 0 ? ivoryDim : ivory, im::startAlign());
            std::string rating = opp.rating > 0 ? i18n::ltr(num(opp.rating)) : std::string();
            TextStyle rs = style(font::FACE_TEXT, 21.0f, muted, im::startAlign());
            const float ratingW = rating.empty() ? 0.0f : gfx::textWidth(rating, rs) + 14.0f;
            std::string name = opp.name;
            fitOrElide(name, ns, cw * (cResult - cOpp) - 20.0f - ratingW);
            gfx::text(name, at(cOpp), by, ns);
            if (!rating.empty()) gfx::text(rating, at(cOpp) + (im::rtl() ? -1.0f : 1.0f) * (gfx::textWidth(name, ns) + 12.0f), by, rs);
            // Result.
            const game::Outcome o = game::outcomeOf(g);
            TextStyle os = style(font::FACE_TEXT, 23.0f, outcomeColor(o), im::startAlign());
            std::string out = outcomeText(o);
            fitOrElide(out, os, cw * (cTc - cResult) - 12.0f, 0.75f);
            gfx::text(out, at(cResult), by, os);
            // Time control, rated or casual.
            TextStyle ts = style(font::FACE_TEXT, 22.0f, ivoryDim, im::startAlign());
            gfx::text(tcText(g), at(cTc), by, ts);
            std::string kind = T(g.rated ? "online.rated" : "online.casual");
            TextStyle ks = style(font::FACE_ITALIC, 21.0f, g.rated ? ivoryDim : muted, im::startAlign());
            fitOrElide(kind, ks, cw * (cRating - cKind) - 12.0f, 0.75f);
            gfx::text(kind, at(cKind), by, ks);
            // Rating change: "+9" and the rating after it.
            const net::GameSide& me = you == 0 ? g.white : g.black;
            if (me.ratingChanged) {
                TextStyle cs = style(font::FACE_TEXT, 23.0f, diffColor(me.ratingDiff), im::startAlign());
                const std::string d = diffText(me.ratingDiff);
                gfx::text(d, at(cRating), by, cs);
                TextStyle as = style(font::FACE_TEXT, 21.0f, muted, im::startAlign());
                gfx::text(i18n::ltr(num(me.ratingAfter)), at(cRating) + (im::rtl() ? -1.0f : 1.0f) * (gfx::textWidth(d, cs) + 12.0f), by, as);
            } else {
                TextStyle cs = style(font::FACE_TEXT, 22.0f, faint, im::startAlign());
                gfx::text("\xE2\x80\x94", at(cRating), by, cs);
            }
            gfx::diamond(vec2(at(0.988f), r.cy()), 3.5f, withAlpha(gold, 0.35f + 0.5f * it.hoverT));
            if (it.activated && !h.waiting()) {
                im::sound(Sound::Open);
                se.openGame(g.id);
                nav = AccountNav::Game;
            }
        }
        gfx::popAlpha();
        if (h.waiting()) spinner(vec2(list.cx(), list.y + list.h * 0.5f), 16.0f);
    }

    // Under the list: the count and the page, or the error of a page that did not come.
    const float infoY = list.b() + 40.0f;
    if (h.loaded() && !h.error().empty()) {
        TextStyle es = style(font::FACE_ITALIC, kSmall, danger, HAlign::Center);
        std::string line = game::onlineErrorText(h.error());
        es.size = gfx::fitSize(line, es, cw - 300.0f, 0.75f);
        gfx::text(line, p.cx(), infoY, es);
        if (linkButton("online.retry", p.cx(), infoY + 14.0f)) se.historyReload();
    } else if (h.loaded() && !page.games.empty()) {
        TextStyle is = style(font::FACE_ITALIC, kSmall, ivoryDim, HAlign::Center);
        std::string line = i18n::trf("online.history.page", {num(h.pageIndex() + 1), num(h.pageCount())}) + kDot +
                           i18n::trn("online.history.total", std::max(0, page.total));
        is.size = gfx::fitSize(line, is, cw, 0.75f);
        gfx::text(line, p.cx(), infoY, is);
    }

    // Footer: Back, then Previous / Next on the end side.
    footerRule(p);
    bool back = backButton(p);
    const float bw = 220.0f;
    const Rect next = im::flip(p, Rect(p.r() - 60.0f - bw, footerY(p), bw, kBtnH));
    const Rect prev = im::flip(p, Rect(p.r() - 60.0f - 2.0f * bw - 20.0f, footerY(p), bw, kBtnH));
    if (im::button(L("online.history.previous"), prev, im::ButtonKind::Secondary, h.hasPrevious() && !h.waiting())) se.historyPrevious();
    if (im::button(L("online.history.next"), next, im::ButtonKind::Secondary, h.hasNext() && !h.waiting())) se.historyNext();
    im::popId();
    endPage();
    if (nav == AccountNav::Stay && (back || im::consumeBack())) nav = AccountNav::Account;
    return nav;
}

// ---- A game of the history -------------------------------------------------------------------------
// One player: colour mark, name ("you" after it), rating and its change.
float playerBlock(const net::GameSide& side, int colour, bool you, const Rect& col, float y) {
    colourMark(vec2(im::flipX(col, col.x + 9.0f), y - 9.0f), colour, 17.0f);
    TextStyle ns = style(font::FACE_TEXT, 30.0f, side.name.compare(0, 8, "deleted#") == 0 ? ivoryDim : ivory, im::startAlign());
    std::string name = side.name;
    TextStyle ys = style(font::FACE_TITLE, 15.0f, gold, im::startAlign(), 0.16f);
    const std::string youTag = T("online.game.you");
    const float tagW = you ? gfx::textWidth(youTag, ys) + 34.0f : 0.0f;
    fitOrElide(name, ns, col.w - 40.0f - tagW);
    const float nx = col.x + 36.0f;
    gfx::text(name, im::flipX(col, nx), y, ns);
    if (you) {
        const float tx = nx + gfx::textWidth(name, ns) + 16.0f;
        gfx::text(youTag, im::flipX(col, tx), y - 3.0f, ys);
    }
    std::string line;
    if (side.rating > 0 && side.ratingChanged)
        line = i18n::ltr(num(side.rating) + " \xE2\x86\x92 " + num(side.ratingAfter));
    else if (side.rating > 0)
        line = i18n::ltr(num(side.rating));
    TextStyle rs = style(font::FACE_TEXT, 22.0f, ivoryDim, im::startAlign());
    if (!line.empty()) {
        gfx::text(line, im::flipX(col, nx), y + 32.0f, rs);
        if (side.ratingChanged) {
            TextStyle cs = style(font::FACE_TEXT, 22.0f, diffColor(side.ratingDiff), im::startAlign());
            gfx::text("(" + diffText(side.ratingDiff) + ")", im::flipX(col, nx + gfx::textWidth(line, rs) + 12.0f), y + 32.0f, cs);
        }
    } else {
        TextStyle us = style(font::FACE_ITALIC, 21.0f, muted, im::startAlign());
        gfx::text(T("online.game.unrated"), im::flipX(col, nx), y + 32.0f, us);
    }
    return 78.0f;
}

AccountNav pageGame(float t, bool fresh, LibrarySetup* library, MenuAction& act) {
    State& s = st();
    game::OnlineSession& se = ses();
    const game::AccountData& data = se.accountData();
    const bool loaded = data.gameLoaded && data.game.id == data.gameWanted;
    const net::GameDetails& g = data.game;
    const bool canSave = library && !library->folder.empty();
    if (fresh) {
        s.movesScroll = s.movesTarget = 0.0f;
        s.reportOpen = false;
        if (!loaded && data.gameWanted && !se.busy(Kind::GameDetailsResult)) se.openGame(data.gameWanted);
    }
    // Is it in the saved games already?
    if (loaded && canSave && s.saveId != g.id && !s.saveJob.valid()) {
        s.saveId = g.id;
        s.savedPath.clear();
        s.error.clear();
        s.note.clear();
        s.replayWanted = false;
        startSave(library->folder, g, std::string());
    }
    if (loaded && s.save == Save::Saved && s.replayWanted && s.saveId == g.id) {
        s.replayWanted = false;
        library->replay.path = s.savedPath;
        library->replay.game = s.savedIndex;
        LOGI("online: replay of server game %llu (%s)", (unsigned long long)g.id, s.savedPath.c_str());
        act = MenuAction::StartReplay;
    }

    AccountNav nav = AccountNav::Stay;
    if (s.reportOpen) im::pushBlock();
    Rect p = beginPage(t, 1640.0f, 1000.0f, T("online.game.title"));
    serverLine(p, true);
    im::pushId("game");
    const float pad = 70.0f, gap = 80.0f;
    const float leftW = std::floor((p.w - 2.0f * pad - gap) * 0.43f), rightW = p.w - 2.0f * pad - gap - leftW;
    const float lx = im::flip(p, Rect(p.x + pad, 0, leftW, 0)).x, rx = im::flip(p, Rect(p.x + pad + leftW + gap, 0, rightW, 0)).x;
    const Rect lcol(lx, 0, leftW, 0), rcol(rx, 0, rightW, 0);
    const float top = p.y + 150.0f, bottom = footerY(p) - 40.0f;
    bool save = false, replay = false, report = false;

    if (!loaded) {
        Rect area(p.x + pad, top, p.w - 2.0f * pad, bottom - top);
        if (!data.gameError.empty())
            message(area, T(data.gameError == "not_found" ? "online.game.not_found" : "online.game.error"),
                    data.gameError == "not_found" ? std::string() : game::onlineErrorText(data.gameError), danger);
        else
            spinner(vec2(area.cx(), area.y + 160.0f), 16.0f);
    } else {
        gfx::vline(im::flipX(p, p.x + pad + leftW + gap * 0.5f), top, bottom, withAlpha(gold, 0.12f));
        // ---- Players, result, details, actions.
        im::sectionLabel(T("online.game.players"), lx, top + 8.0f, leftW);
        float y = top + 70.0f;
        y += playerBlock(g.white, 0, g.you == 0, lcol, y);
        y += playerBlock(g.black, 1, g.you == 1, lcol, y);
        y += 14.0f;
        gfx::hlineFade(lx, lx + leftW, y - 22.0f, withAlpha(gold, 0.2f), 0.2f);
        const game::Outcome o = game::outcomeOf(g);
        {
            TextStyle res = style(font::FACE_TITLE, 34.0f, goldBright, im::startAlign(), 0.06f);
            const std::string rt = i18n::ltr(resultText(g.result));
            gfx::text(rt, im::flipX(lcol, lx), y + 14.0f, res);
            const float rw = gfx::textWidth(rt, res) + 22.0f;
            TextStyle os = style(font::FACE_TEXT, 28.0f, outcomeColor(o), im::startAlign());
            std::string words = o == game::Outcome::Win    ? T("online.game.you_won")
                                : o == game::Outcome::Loss ? T("online.game.you_lost")
                                : o == game::Outcome::Draw ? T("online.game.draw")
                                                           : T("online.history.aborted");
            fitOrElide(words, os, leftW - rw, 0.75f);
            gfx::text(words, im::flipX(lcol, lx + rw), y + 12.0f, os);
            std::string why = reasonWords(g);
            if (!why.empty()) {
                TextStyle ws = style(font::FACE_ITALIC, 23.0f, ivoryDim, im::startAlign());
                fitOrElide(why, ws, leftW, 0.75f);
                gfx::text(why, im::flipX(lcol, lx), y + 50.0f, ws);
            }
            y += 92.0f;
        }
        infoLine(T("online.game.date"), dateTimeText(g.startedAtMs), lcol, y);
        y += 74.0f;
        std::string tc = tcText(g) + kDot + T(g.rated ? "online.rated" : "online.casual");
        infoLine(T("online.game.time_control"), tc, lcol, y);
        y += 82.0f;
        // Actions.
        const float bh = 52.0f, bstep = 62.0f;
        const bool hasMoves = !g.moves.empty();
        const bool working = s.save == Save::Checking || s.save == Save::Downloading || s.save == Save::Writing;
        if (canSave) {
            const bool saved = s.save == Save::Saved && s.saveId == g.id;
            Rect b(lx, y, leftW, bh);
            save = im::button(L(saved ? "online.game.saved_button" : "online.game.save"), b, im::ButtonKind::Secondary,
                              hasMoves && !saved && !working);
            if (working && !s.replayWanted) spinner(vec2(im::flipX(lcol, lx + leftW + 30.0f), b.cy()), 10.0f);
            y += bstep;
            Rect rb(lx, y, leftW, bh);
            replay = im::button(L("online.game.replay"), rb, im::ButtonKind::Primary, hasMoves && !working);
            if (working && s.replayWanted) spinner(vec2(im::flipX(lcol, lx + leftW + 30.0f), rb.cy()), 10.0f);
            y += bstep;
        }
        const bool reported = std::find(s.reported.begin(), s.reported.end(), g.id) != s.reported.end();
        if (g.reportable && !reported) {
            report = im::button(L("online.report.button"), Rect(lx, y, leftW, bh), im::ButtonKind::Quiet);
            y += bstep;
        }
        if (!s.error.empty() || !s.note.empty()) {
            TextStyle ms = style(font::FACE_ITALIC, kSmall, s.error.empty() ? ivoryDim : danger, im::startAlign());
            gfx::textWrapped(s.error.empty() ? s.note : s.error, im::flipX(lcol, lx), y + 18.0f, leftW, ms, 29.0f);
        }

        // ---- The moves, a full move a row: number, White's move and clock, Black's move and clock.
        im::sectionLabel(T("online.game.moves"), rx, top + 8.0f, rightW);
        {
            TextStyle cs = style(font::FACE_ITALIC, 21.0f, ivoryDim, im::endAlign());
            std::string count = i18n::trn("online.game.move_count", (g.plies + 1) / 2);
            gfx::text(count, im::flipX(rcol, rx + rightW), top + 8.0f, cs);
        }
        bool complete = true;
        const std::vector<game::MoveLine> lines = game::gameMoves(g, &complete);
        const Rect area(rx, top + 40.0f, rightW, bottom - top - 40.0f);
        if (lines.empty()) {
            TextStyle es = style(font::FACE_ITALIC, 22.0f, muted, im::startAlign());
            gfx::text(T("online.game.no_moves"), im::flipX(rcol, rx + 2.0f), area.y + 40.0f, es);
        } else {
            const float rowH = 38.0f;
            const int rows = int(lines.size() + 1) / 2;
            const float contentH = float(rows) * rowH + 20.0f;
            const float scroll = wheelScroll(s.movesScroll, s.movesTarget, area, contentH, rowH * 3.0f, fresh, true);
            gfx::pushClip(area);
            // Moves are left to right whatever the language (like the saved games).
            const float xNum = rx + 8.0f, xW = rx + rightW * 0.13f, xWc = rx + rightW * 0.47f, xB = rx + rightW * 0.56f,
                        xBc = rx + rightW * 0.97f;
            TextStyle ns = style(font::FACE_TEXT, 22.0f, muted, HAlign::Left);
            TextStyle ms = style(font::FACE_TEXT, 25.0f, ivory, HAlign::Left);
            TextStyle ks = style(font::FACE_TEXT, 20.0f, withAlpha(ivoryDim, 0.85f), HAlign::Right);
            ns.dir = ms.dir = ks.dir = 0;
            for (int r = 0; r < rows; ++r) {
                const float ry = area.y + 6.0f + float(r) * rowH - scroll;
                if (ry + rowH < area.y || ry > area.b()) continue;
                if (r % 2 == 1) gfx::fill(Rect(rx, ry, rightW, rowH), withAlpha(gold, 0.03f));
                const float by = ry + rowH * 0.5f + 8.0f;
                gfx::text(num(r + 1) + ".", xNum, by, ns);
                const game::MoveLine& w = lines[size_t(2 * r)];
                gfx::text(coach::figurineSan(w.san), xW, by, ms);
                gfx::text(clockText(w.clockMs), xWc, by, ks);
                if (size_t(2 * r + 1) < lines.size()) {
                    const game::MoveLine& b = lines[size_t(2 * r + 1)];
                    gfx::text(coach::figurineSan(b.san), xB, by, ms);
                    gfx::text(clockText(b.clockMs), xBc, by, ks);
                }
            }
            gfx::popClip();
            scrollDecor(area, scroll, contentH);
        }
        if (!complete) {
            TextStyle es = style(font::FACE_ITALIC, 19.0f, danger, im::startAlign());
            gfx::text(T("online.game.moves_incomplete"), im::flipX(rcol, rx), bottom + 22.0f, es);
        }
    }
    footerRule(p);
    bool back = backButton(p);
    im::popId();
    endPage();
    if (s.reportOpen) im::popBlock();

    // Actions.
    if (loaded && (save || replay) && canSave) {
        s.error.clear();
        s.note.clear();
        if (replay) s.replayWanted = true;
        if (s.save == Save::Saved && s.saveId == g.id) {
            // Replay of a saved game: next frame (above).
        } else if (s.save != Save::Checking && s.save != Save::Downloading && s.save != Save::Writing) {
            se.api().downloadPgn(g.id);
            se.expect(Kind::PgnResult);
            s.save = Save::Downloading;
        }
    }
    if (report) {
        s.reportOpen = true;
        s.reportCategory = 0;
        s.reportComment.clear();
    }
    if (s.reportOpen) {
        int r = reportDialog(s.reportCategory, s.reportComment);
        if (r == 1 && loaded) {
            static const char* const cats[] = {"cheating", "abuse", "other"};
            const net::GameSide& opp = g.you == 0 ? g.black : g.white;
            se.api().report(g.id, opp.name, cats[std::clamp(s.reportCategory, 0, 2)], s.reportComment);
            se.expect(Kind::ReportResult);
            s.reported.push_back(g.id);
        }
        if (r >= 0) s.reportOpen = false;
        return AccountNav::Stay;
    }
    if (back || im::consumeBack()) nav = AccountNav::History;
    return nav;
}

// ---- Signed-in devices -------------------------------------------------------------------------------
AccountNav pageDevices(float t, bool fresh, std::string& navNote) {
    State& s = st();
    game::OnlineSession& se = ses();
    const game::AccountData& data = se.accountData();
    if (fresh && (s.reloadDevices || !data.sessionsLoaded)) {
        se.loadSessions();
        s.reloadDevices = false;
    }
    AccountNav nav = AccountNav::Stay;
    if (s.confirmAll) im::pushBlock();
    Rect p = beginPage(t, 1320.0f, 940.0f, T("online.devices.title"));
    serverLine(p, true);
    im::pushId("devices");
    float y = p.y + 150.0f;
    y += paragraph(T("online.devices.lead"), p, y, p.w - 240.0f, ivoryDim, kSmall + 1.0f) + 12.0f;
    const float pad = 90.0f, x0 = p.x + pad, w = p.w - 2.0f * pad;
    const Rect area(x0, y, w, footerY(p) - 70.0f - y);
    if (!data.sessionsLoaded) {
        if (!data.sessionsError.empty()) {
            message(area, T("online.devices.error"), game::onlineErrorText(data.sessionsError), danger);
            if (linkButton("online.retry", area.cx(), area.y + area.h * 0.42f + 150.0f)) se.loadSessions();
        } else {
            spinner(vec2(area.cx(), area.y + 80.0f), 16.0f);
        }
    } else {
        const float rowH = 96.0f;
        const float contentH = float(data.sessions.size()) * rowH;
        static float scroll = 0.0f, target = 0.0f;
        wheelScroll(scroll, target, area, contentH, rowH, fresh, false);
        gfx::pushClip(area);
        const Rect rowCol(x0, 0, w, 0);
        const float bw = 200.0f;
        for (size_t i = 0; i < data.sessions.size(); ++i) {
            const net::SessionInfo& d = data.sessions[i];
            const Rect r(x0, area.y + float(i) * rowH - scroll, w, rowH - 12.0f);
            if (r.b() < area.y || r.y > area.b()) continue;
            gfx::fill(r, vec4(0, 0, 0, 0.22f), 2.0f);
            gfx::stroke(r, withAlpha(gold, d.current ? 0.45f : 0.16f), 0.0f, d.current ? 1.6f : 1.0f);
            // Label (or "Unknown device"), "This device".
            const bool unknown = d.clientLabel.empty();
            std::string label = unknown ? T("online.devices.unknown") : i18n::ltr(d.clientLabel);
            TextStyle ls = style(unknown ? font::FACE_ITALIC : font::FACE_TEXT, 26.0f, unknown ? ivoryDim : ivory, im::startAlign());
            std::string tag = d.current ? T("online.devices.this") : std::string();
            TextStyle ts = style(font::FACE_TITLE, 15.0f, gold, im::startAlign(), 0.16f);
            const float tagW = tag.empty() ? 0.0f : gfx::textWidth(tag, ts) + 30.0f;
            fitOrElide(label, ls, w - 60.0f - bw - tagW);
            const float lx = x0 + 26.0f;
            gfx::text(label, im::flipX(rowCol, lx), r.y + 38.0f, ls);
            if (!tag.empty()) {
                const float tx = lx + gfx::textWidth(label, ls) + 18.0f;
                gfx::diamond(vec2(im::flipX(rowCol, tx + 4.0f), r.y + 30.0f), 3.0f, gold);
                gfx::text(tag, im::flipX(rowCol, tx + 16.0f), r.y + 36.0f, ts);
            }
            std::string when = i18n::trf("online.devices.signed_in", {dateText(d.createdAtMs)}) + kDot +
                               (d.current ? T("online.devices.active_now") : i18n::trf("online.devices.last_active", {dateTimeText(d.lastSeenAtMs)}));
            TextStyle ws = style(font::FACE_ITALIC, 20.0f, muted, im::startAlign());
            fitOrElide(when, ws, w - 60.0f - bw, 0.8f);
            gfx::text(when, im::flipX(rowCol, lx), r.y + 70.0f, ws);
            if (!d.current) {
                im::pushId(int(i));
                const Rect b = im::flip(rowCol, Rect(r.r() - 22.0f - bw, r.cy() - 25.0f, bw, 50.0f));
                const bool busy = s.revoking == d.id;
                if (im::button(L("online.devices.sign_out"), b, im::ButtonKind::Secondary, !busy && s.revoking == 0)) {
                    s.revoking = d.id;
                    s.error.clear();
                    s.note.clear();
                    se.revokeSession(d.id);
                }
                // Before the button on its start side (b is mirrored already: flipped once, here
                // from the unmirrored x).
                if (busy) spinner(vec2(im::flipX(rowCol, r.r() - 22.0f - bw - 30.0f), b.cy()), 10.0f);
                im::popId();
            }
        }
        gfx::popClip();
        scrollDecor(area, scroll, contentH);
    }
    messageLine(p, footerY(p) - 52.0f, s.error, s.note);
    footerRule(p);
    bool back = backButton(p);
    const Rect allR = im::flip(p, Rect(p.r() - 60.0f - 320.0f, footerY(p), 320.0f, kBtnH));
    bool all = im::button(L("online.account.sign_out_all"), allR, im::ButtonKind::Primary);
    im::tooltip(T("online.account.sign_out_all.help"));
    im::popId();
    endPage();
    if (s.confirmAll) im::popBlock();
    if (all) s.confirmAll = true;
    if (s.confirmAll) {
        int r = im::confirmDialog("##online.devices.all", T("online.devices.all.title"), T("online.account.sign_out_all.help"),
                                  T("online.account.sign_out_all"), T("common.cancel"), true);
        if (r == 1) {
            se.signOut(true);
            clearSecrets();
            navNote = T("online.account.signed_out_all");
            nav = AccountNav::SignIn;
        }
        if (r >= 0) s.confirmAll = false;
        return nav;
    }
    if (back || im::consumeBack()) nav = AccountNav::Account;
    return nav;
}

// ---- Change of e-mail ------------------------------------------------------------------------------
AccountNav pageEmail(float t) {
    State& s = st();
    game::OnlineSession& se = ses();
    const net::AccountInfo& a = se.account();
    const bool busy = se.busy(Kind::EmailChangeResult);
    Rect p = beginPage(t, 1040.0f, 720.0f, T("online.email.title"));
    serverLine(p, true);
    im::pushId("email");
    float y = p.y + 160.0f;
    bool send = false, done = false;
    if (!s.sentTo.empty()) {
        // The link went to the new address.
        y += 30.0f;
        gfx::diamond(vec2(p.cx(), y - 8.0f), 5.0f, withAlpha(gold, 0.8f));
        y += 40.0f;
        y += paragraph(i18n::trf("online.email.sent", {i18n::ltr(s.sentTo)}), p, y, p.w - 220.0f, ivory, kBody, font::FACE_TEXT) + 18.0f;
        paragraph(T("online.email.sent.hint"), p, y, p.w - 220.0f, muted, kSmall);
        footerRule(p);
        done = primaryButton(p, "online.email.done", true);
    } else {
        std::string current = a.email.empty() ? std::string("\xE2\x80\x94") : i18n::ltr(a.email);
        y += paragraph(i18n::trf("online.email.current", {current}), p, y, p.w - 220.0f, ivoryDim, kSmall + 2.0f) + 6.0f;
        if (!a.pendingEmail.empty())
            y += paragraph(i18n::trf("online.account.pending_email", {i18n::ltr(a.pendingEmail)}), p, y, p.w - 220.0f, gold, kSmall) + 6.0f;
        y += 14.0f;
        if (!a.hasPassword) {
            y += paragraph(game::onlineErrorText("password_not_set"), p, y + 20.0f, p.w - 220.0f, danger, kSmall + 1.0f);
        } else {
            im::formField(L("online.field.new_email"), s.newEmail, formRow(p, y, 160.0f), 254, im::FIELD_LTR, "name@example.org");
            reauthFields(p, y);
            messageLine(p, y + 22.0f, s.error, s.note);
        }
        footerRule(p);
        const std::string addr = trim(s.newEmail);
        send = primaryButton(p, "online.email.button", a.hasPassword && addr.find('@') != std::string::npos && reauthFilled(), busy);
    }
    bool back = backButton(p);
    im::popId();
    endPage();
    if (send) {
        s.error.clear();
        s.note.clear();
        se.api().changeEmail(trim(s.newEmail), s.password, reauthCode());
        se.expect(Kind::EmailChangeResult);
    }
    if (done || back || im::consumeBack()) {
        clearSecrets();
        return AccountNav::Account;
    }
    return AccountNav::Stay;
}

// ---- Download my data -------------------------------------------------------------------------------
AccountNav pageExport(float t) {
    State& s = st();
    game::OnlineSession& se = ses();
    const net::AccountInfo& a = se.account();
    const bool busy = se.busy(Kind::AccountExportResult) || s.exportWriting;
    Rect p = beginPage(t, 1040.0f, 700.0f, T("online.export.title"));
    serverLine(p, true);
    im::pushId("export");
    float y = p.y + 160.0f;
    bool go = false;
    if (!s.exportPath.empty()) {
        // Written: where, and its folder.
        y += 30.0f;
        gfx::diamond(vec2(p.cx(), y - 8.0f), 5.0f, withAlpha(gold, 0.8f));
        y += 40.0f;
        y += paragraph(T("online.export.saved"), p, y, p.w - 220.0f, ivory, kBody, font::FACE_TEXT) + 22.0f;
        TextStyle ps = style(font::FACE_TEXT, 22.0f, goldBright, HAlign::Center);
        ps.dir = 0;
        std::string path = s.exportPath;
        fitOrElide(path, ps, p.w - 160.0f, 0.7f);
        gfx::text(path, p.cx(), y + 6.0f, ps);
        y += 44.0f;
        const float bw = 260.0f;
        if (im::button(L("library.open_folder"), Rect(p.cx() - bw * 0.5f, y, bw, 52.0f), im::ButtonKind::Secondary)) {
            std::string folder = s.exportPath;
            const size_t cut = folder.find_last_of("/\\");
            if (cut != std::string::npos) folder.erase(cut + 1);
            if (!plat::openInFileManager(folder)) notify(T("library.open_failed"));
        }
    } else {
        y += paragraph(T("online.export.lead"), p, y, p.w - 220.0f, ivoryDim, kSmall + 2.0f) + 22.0f;
        if (!a.hasPassword) {
            paragraph(game::onlineErrorText("password_not_set"), p, y + 20.0f, p.w - 220.0f, danger, kSmall + 1.0f);
        } else {
            reauthFields(p, y);
            messageLine(p, y + 22.0f, s.error, s.note);
        }
    }
    footerRule(p);
    bool back = backButton(p);
    if (s.exportPath.empty() && a.hasPassword) go = primaryButton(p, "online.export.button", reauthFilled(), busy);
    im::popId();
    endPage();
    if (go) {
        s.error.clear();
        s.note.clear();
        se.api().exportAccount(s.password, reauthCode());
        se.expect(Kind::AccountExportResult);
    }
    if (back || im::consumeBack()) {
        clearSecrets();
        return AccountNav::Account;
    }
    return AccountNav::Stay;
}

// ---- Delete the account ------------------------------------------------------------------------------
AccountNav pageDelete(float t) {
    State& s = st();
    game::OnlineSession& se = ses();
    const net::AccountInfo& a = se.account();
    const bool busy = se.busy(Kind::AccountDeleted);
    Rect p = beginPage(t, 1100.0f, 820.0f, T("online.delete.title"));
    serverLine(p, true);
    im::pushId("delete");
    float y = p.y + 160.0f;
    y += paragraph(T("online.delete.warning"), p, y, p.w - 220.0f, danger, kSmall + 3.0f, font::FACE_TEXT) + 12.0f;
    y += paragraph(i18n::trf("online.delete.games", {i18n::ltr("deleted#" + num(a.userId))}), p, y, p.w - 220.0f, ivoryDim, kSmall + 1.0f) + 22.0f;
    bool go = false;
    if (!a.hasPassword) {
        paragraph(game::onlineErrorText("password_not_set"), p, y + 20.0f, p.w - 220.0f, danger, kSmall + 1.0f);
    } else {
        y += paragraph(T("online.delete.confirm_name"), p, y, p.w - 220.0f, ivoryDim, kSmall + 1.0f) + 8.0f;
        im::formField(L("online.field.username"), s.confirmName, formRow(p, y, 160.0f), 40, im::FIELD_LTR, a.username);
        reauthFields(p, y);
        messageLine(p, y + 22.0f, s.error, s.note);
    }
    footerRule(p);
    bool back = backButton(p);
    if (a.hasPassword) go = primaryButton(p, "online.delete.button", trim(s.confirmName) == a.username && reauthFilled(), busy);
    im::popId();
    endPage();
    if (go) {
        s.error.clear();
        se.api().deleteAccount(s.password, reauthCode());
        se.expect(Kind::AccountDeleted);
    }
    if (back || im::consumeBack()) {
        clearSecrets();
        s.confirmName.clear();
        return AccountNav::Account;
    }
    return AccountNav::Stay;
}

}  // namespace

// ==== The account pages' entry points ==================================================================
void accountReset(AccountPage page) {
    State& s = st();
    s.error.clear();
    s.note.clear();
    clearSecrets();
    switch (page) {
    case AccountPage::History: s.reloadHistory = true; break;
    case AccountPage::Devices:
        s.reloadDevices = true;
        s.revoking = 0;
        s.confirmAll = false;
        break;
    case AccountPage::Email:
        s.newEmail.clear();
        s.sentTo.clear();
        break;
    case AccountPage::Export: s.exportPath.clear(); break;
    case AccountPage::Delete: s.confirmName.clear(); break;
    case AccountPage::Game: break;
    }
}

AccountNav accountPump(const AccountPage* current, std::string& note, std::string& error) {
    State& s = st();
    game::OnlineSession& se = ses();
    net::Event e;
    AccountNav nav = AccountNav::Stay;
    auto on = [&](AccountPage p) { return current && *current == p; };
    // The pages read these from the session (accountData). A token refused on the way signs out
    // (the session forgot it): the sign-in page says so.
    for (Kind k : {Kind::GamesResult, Kind::GameDetailsResult, Kind::SessionsResult, Kind::ReportResult})
        if (se.take(k, e) && !e.ok && e.error == "unauthorized") error = errorText(e);
    if (se.take(Kind::SessionRevoked, e)) {
        s.revoking = 0;
        if (e.ok || e.error == "not_found") {
            if (on(AccountPage::Devices)) s.note = T("online.devices.signed_out");
        } else if (e.error != "unauthorized") {
            if (on(AccountPage::Devices)) s.error = errorText(e);
            else notify(errorText(e), 4.0f);
        }
    }
    if (se.take(Kind::PreferencesResult, e) && !e.ok && e.error != "unauthorized") notify(errorText(e), 4.0f);
    if (se.take(Kind::PgnResult, e) && s.save == Save::Downloading) {
        const game::AccountData& data = se.accountData();
        if (e.ok && data.gameLoaded && data.game.id == s.saveId && e.gameId == s.saveId) {
            startSave(s.saveFolder, data.game, e.text);
        } else {
            s.save = Save::Failed;
            s.replayWanted = false;
            s.error = e.ok ? T("online.game.save_failed") : errorText(e);
        }
    }
    if (se.take(Kind::EmailChangeResult, e)) {
        if (e.ok && e.status == "email_changed") {
            clearSecrets();
            note = T("online.email.changed");
            if (on(AccountPage::Email)) nav = AccountNav::Account;
            else notify(note, 4.0f);
        } else if (e.ok) {
            s.sentTo = trim(s.newEmail);
            clearSecrets();
            if (!on(AccountPage::Email)) notify(i18n::trf("online.email.sent", {i18n::ltr(s.sentTo)}), 6.0f);
        } else if (e.error != "unauthorized") {
            s.error = errorText(e);
        }
    }
    if (se.take(Kind::AccountExportResult, e)) {
        if (e.ok) {
            clearSecrets();
            const std::string folder = plat::appDataDirectory() + "account/";
            const std::string name = game::exportFileName(se.endpoint().host, se.account().username, std::time(nullptr));
            const std::string text = e.text;
            s.exportWriting = true;
            s.exportJob = std::async(std::launch::async, [folder, name, text]() {
                try {
                    return archive::saveFile(folder, name, text);
                } catch (const std::exception& ex) {
                    archive::SaveResult r;
                    r.error = ex.what();
                    return r;
                }
            });
        } else if (e.error != "unauthorized") {
            s.error = errorText(e);
        }
    }
    if (se.take(Kind::AccountDeleted, e)) {
        if (e.ok) {
            LOGI("online: the account was deleted");
            clearSecrets();
            s.confirmName.clear();
            note = T("online.delete.done");
            nav = AccountNav::SignIn;
        } else if (e.error != "unauthorized") {
            s.error = errorText(e);
        }
    }
    // Work done off the UI thread.
    if (ready(s.saveJob)) {
        const archive::ServerSaveResult r = s.saveJob.get();
        const bool looking = s.save == Save::Checking;
        switch (r.status) {
        case archive::ServerSaveStatus::Saved:
        case archive::ServerSaveStatus::AlreadySaved:
            s.save = Save::Saved;
            s.savedPath = r.path;
            s.savedIndex = r.index;
            if (!looking && !s.replayWanted)
                s.note = T(r.status == archive::ServerSaveStatus::Saved ? "online.game.saved" : "online.game.already_saved");
            if (!looking) LOGI("online: server game saved in %s", r.path.c_str());
            break;
        case archive::ServerSaveStatus::NeedsText: s.save = Save::NotSaved; break;
        case archive::ServerSaveStatus::Invalid:
            s.save = Save::Failed;
            s.replayWanted = false;
            s.error = T("online.game.invalid");
            LOGW("online: the server's PGN cannot be read: %s", r.error.c_str());
            break;
        default:
            s.save = Save::Failed;
            s.replayWanted = false;
            s.error = T("online.game.save_failed");
            LOGW("online: the game could not be saved: %s", r.error.c_str());
            break;
        }
    }
    if (ready(s.exportJob)) {
        const archive::SaveResult r = s.exportJob.get();
        s.exportWriting = false;
        if (r.ok) {
            s.exportPath = r.path;
            LOGI("online: account data saved in %s", r.path.c_str());
            if (!on(AccountPage::Export)) notify(T("online.export.saved"), 4.0f);
        } else {
            s.error = T("online.export.failed");
            LOGW("online: the account data could not be written: %s", r.error.c_str());
        }
    }
    if (!se.signedIn() && error.empty() && nav == AccountNav::Stay && current) error = game::onlineErrorText("unauthorized");
    return nav;
}

AccountNav accountPage(AccountPage page, float t, bool fresh, LibrarySetup* library, MenuAction& act, std::string& note) {
    State& s = st();
    if (library) s.saveFolder = library->folder;
    switch (page) {
    case AccountPage::History: return pageHistory(t, fresh);
    case AccountPage::Game: return pageGame(t, fresh, library, act);
    case AccountPage::Devices: return pageDevices(t, fresh, note);
    case AccountPage::Email: return pageEmail(t);
    case AccountPage::Export: return pageExport(t);
    case AccountPage::Delete: return pageDelete(t);
    }
    return AccountNav::Stay;
}

bool accountDebugOpen(const std::string& sub, AccountPage& page) {
    static const struct { const char* name; AccountPage page; } names[] = {
        {"history", AccountPage::History}, {"game", AccountPage::Game},     {"devices", AccountPage::Devices},
        {"email", AccountPage::Email},     {"email-sent", AccountPage::Email}, {"export", AccountPage::Export},
        {"export-done", AccountPage::Export}, {"delete", AccountPage::Delete},
    };
    for (const auto& n : names) {
        if (sub != n.name) continue;
        page = n.page;
        accountReset(page);
        State& s = st();
        game::OnlineSession& se = ses();
        net::Event e;
        // The data, fetched from the fake server (its clock only moves here).
        if (page == AccountPage::History || page == AccountPage::Game) {
            se.loadHistory(net::GamesFilter());
            se.runMock(1000.0);
            se.take(Kind::GamesResult, e);
            s.reloadHistory = false;
            const net::GamesPage& pg = se.accountData().history.page();
            if (page == AccountPage::Game && !pg.games.empty()) {
                // A decisive game with a rating change and plenty of moves, if the page has one.
                uint64_t id = pg.games[0].id;
                for (const net::GameSummary& g : pg.games)
                    if (g.rated && g.plies >= 30 && (g.status == 1 || g.status == 2)) {
                        id = g.id;
                        break;
                    }
                se.openGame(id);
                se.runMock(1000.0);
                se.take(Kind::GameDetailsResult, e);
            }
        }
        if (page == AccountPage::Devices) {
            se.loadSessions();
            se.runMock(1000.0);
            se.take(Kind::SessionsResult, e);
            s.reloadDevices = false;
        }
        if (sub == "email") s.newEmail = "magnus.t@example.net";
        if (sub == "email-sent") s.sentTo = "magnus.t@example.net";
        if (sub == "export-done")
            s.exportPath = plat::appDataDirectory() + "account/" +
                           game::exportFileName(se.endpoint().host, se.account().username, std::time_t(se.nowMs() / 1000.0));
        if (sub == "delete") s.confirmName = se.account().username;
        return true;
    }
    return false;
}

}  // namespace onl
}  // namespace detail
}  // namespace ui
