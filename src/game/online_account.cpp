// The account pages' data (see online_account.h).
#include "online_account.h"
#include "../chess/chess.h"
#include "game_archive.h"
#include <algorithm>
#include <cstdio>

namespace game {

using Kind = net::Event::Kind;

// ---- History pages ------------------------------------------------------------------------------------

uint64_t HistoryPager::restart(const net::GamesFilter& filter) {
    filter_ = filter;
    page_ = net::GamesPage();
    cursors_.clear();
    index_ = 0;
    loaded_ = false;
    error_.clear();
    waiting_ = true;
    wantBefore_ = 0;
    wantIndex_ = 0;
    return 0;
}

bool HistoryPager::next(uint64_t& before) {
    if (waiting_ || !hasNext()) return false;
    waiting_ = true;
    wantBefore_ = before = page_.next;
    wantIndex_ = index_ + 1;
    error_.clear();
    return true;
}

bool HistoryPager::previous(uint64_t& before) {
    if (waiting_ || !hasPrevious() || size_t(index_ - 1) >= cursors_.size()) return false;
    waiting_ = true;
    wantBefore_ = before = cursors_[size_t(index_ - 1)];
    wantIndex_ = index_ - 1;
    error_.clear();
    return true;
}

uint64_t HistoryPager::reload() {
    waiting_ = true;
    wantIndex_ = loaded_ ? index_ : 0;
    wantBefore_ = loaded_ && size_t(index_) < cursors_.size() ? cursors_[size_t(index_)] : 0;
    error_.clear();
    return wantBefore_;
}

bool HistoryPager::accept(const net::GamesPage& page) {
    if (!waiting_ || page.before != wantBefore_) return false;
    waiting_ = false;
    page_ = page;
    index_ = wantIndex_;
    if (cursors_.size() < size_t(index_) + 1) cursors_.resize(size_t(index_) + 1, 0);
    cursors_[size_t(index_)] = page.before;
    cursors_.resize(size_t(index_) + 1);  // the pages after it are found again from its cursor
    loaded_ = true;
    error_.clear();
    return true;
}

void HistoryPager::fail(const std::string& error) {
    waiting_ = false;
    error_ = error.empty() ? std::string("server_error") : error;
}

void HistoryPager::clear() {
    *this = HistoryPager();
}

int HistoryPager::pageCount() const {
    if (!loaded_) return 1;
    const int byTotal = (std::max(0, page_.total) + kPageSize - 1) / kPageSize;
    // The total may lag behind the pages (a game finished meanwhile): never fewer than seen.
    return std::max({1, byTotal, index_ + (page_.next ? 2 : 1)});
}

// ---- Routing --------------------------------------------------------------------------------------------

void AccountData::clear() {
    *this = AccountData();
}

bool AccountData::apply(const net::Event& e, net::AccountInfo& account, bool& signedIn) {
    switch (e.kind) {
    case Kind::GamesResult:
        if (e.ok) history.accept(e.gamesPage);
        else history.fail(e.error);
        break;
    case Kind::GameDetailsResult:
        if (e.ok && e.gameDetails.id == gameWanted) {
            game = e.gameDetails;
            gameLoaded = true;
            gameError.clear();
        } else if (!e.ok) {
            gameError = e.error;
        }
        break;
    case Kind::SessionsResult:
        if (e.ok) {
            sessions = e.sessions;
            // The current device first, then the most recently active.
            std::stable_sort(sessions.begin(), sessions.end(), [](const net::SessionInfo& a, const net::SessionInfo& b) {
                if (a.current != b.current) return a.current;
                return a.lastSeenAtMs > b.lastSeenAtMs;
            });
            sessionsLoaded = true;
            sessionsError.clear();
        } else {
            sessionsError = e.error;
        }
        break;
    case Kind::SessionRevoked:
        // Gone already (not_found) is what was asked for too.
        if (e.ok || e.error == "not_found")
            sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [&](const net::SessionInfo& s) { return s.id == e.sessionId; }),
                           sessions.end());
        break;
    case Kind::PreferencesResult:
        if (e.ok) account.acceptChallenges = e.account.acceptChallenges;
        break;
    case Kind::AccountDeleted:
        if (e.ok) {
            signedIn = false;
            account = net::AccountInfo();
            clear();
        }
        break;
    case Kind::PgnResult:
    case Kind::EmailChangeResult:
    case Kind::AccountExportResult: break;  // for the page that asked
    default: return false;
    }
    // The token was refused (or none is saved): the network layer has forgotten it. A public read
    // asked again without it is answered (ok) and says so with sessionLost.
    if (e.sessionLost || (!e.ok && e.error == "unauthorized")) signedIn = false;
    return true;
}

// ---- Texts and moves ---------------------------------------------------------------------------------

Outcome outcomeOf(const net::GameSummary& g) {
    switch (g.status) {
    case 1:
    case 2:
        if (g.you != 0 && g.you != 1) return Outcome::None;
        return (g.status == 1) == (g.you == 0) ? Outcome::Win : Outcome::Loss;
    case 3: return Outcome::Draw;
    case 4: return Outcome::Aborted;
    default: return Outcome::None;
    }
}

std::vector<MoveLine> gameMoves(const net::GameDetails& g, bool* complete) {
    std::vector<MoveLine> out;
    out.reserve(g.moves.size());
    chess::Position pos;
    bool ok = true;
    for (const net::GameDetails::Ply& p : g.moves) {
        chess::Move mv;
        if (p.move != 0)
            mv = pos.findLegal(chess::Square(net::moveFrom(p.move)), chess::Square(net::moveTo(p.move)),
                               chess::PieceType(net::movePromo(p.move)));
        if (!mv.valid() && !p.uci.empty()) mv = pos.parseUCI(p.uci);
        if (!mv.valid()) {
            ok = false;
            break;
        }
        MoveLine line;
        line.san = pos.toSAN(mv);
        line.clockMs = p.clockMs;
        line.spentMs = p.spentMs;
        out.push_back(line);
        pos.makeMove(mv);
    }
    if (complete) *complete = ok;
    return out;
}

std::string timeControlLabel(int64_t baseMs, int64_t incMs) {
    const int64_t base = std::max<int64_t>(0, baseMs / 1000), inc = std::max<int64_t>(0, incMs / 1000);
    char buf[48];
    if (base % 60 == 0) std::snprintf(buf, sizeof buf, "%lld+%lld", (long long)(base / 60), (long long)inc);
    else std::snprintf(buf, sizeof buf, "%lld:%02lld+%lld", (long long)(base / 60), (long long)(base % 60), (long long)inc);
    return buf;
}

std::string exportFileName(const std::string& host, const std::string& username, std::time_t when) {
    std::tm tm{};
#ifdef _WIN32
    const bool have = localtime_s(&tm, &when) == 0;
#else
    const bool have = localtime_r(&when, &tm) != nullptr;
#endif
    char date[32] = "0000-00-00";
    if (have) std::snprintf(date, sizeof date, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return archive::sanitizeName(host, 64) + "_" + archive::sanitizeName(username, 40) + "_" + date + ".json";
}

}  // namespace game
