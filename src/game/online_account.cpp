// The account pages' data (see online_account.h).
#include "online_account.h"
#include "../chess/chess.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "game_archive.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>

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
    case Kind::GifResult:
        // For the GifSaver. The GIF routes need the token: any 401 refused it (the network layer
        // has forgotten it: "invalid_token" from the server), and "not_logged_in" means none is saved.
        if (!e.ok && (e.error == "invalid_token" || e.error == "not_logged_in")) signedIn = false;
        break;
    case Kind::PgnResult:
    case Kind::EmailChangeResult:
    case Kind::AccountExportResult: break;  // for the page that asked
    default: return false;
    }
    // The token was refused: the network layer has forgotten it.
    if (!e.ok && e.error == "unauthorized") signedIn = false;
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

// ---- Animated GIFs ------------------------------------------------------------------------------------

std::string gifFileName(std::time_t started, const std::string& white, const std::string& black, uint64_t gameId) {
    std::tm tm{};
#ifdef _WIN32
    const bool have = started > 0 && localtime_s(&tm, &started) == 0;
#else
    const bool have = started > 0 && localtime_r(&started, &tm) != nullptr;
#endif
    char stamp[64] = "0000-00-00_000000";
    if (have)
        std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                      tm.tm_min, tm.tm_sec);
    std::string name = std::string(stamp) + "_" + archive::sanitizeName(white) + "-vs-" + archive::sanitizeName(black);
    if (gameId) name += "_" + std::to_string(gameId);
    return name + ".gif";
}

std::time_t pgnLocalTime(const std::string& date, const std::string& time, std::time_t fallback) {
    auto digits = [](const std::string& s, size_t at, size_t n, int& out) {
        if (s.size() < at + n) return false;
        out = 0;
        for (size_t i = at; i < at + n; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            out = out * 10 + (s[i] - '0');
        }
        return true;
    };
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!digits(date, 0, 4, y) || !digits(date, 5, 2, mo) || !digits(date, 8, 2, d) || mo < 1 || mo > 12 || d < 1 || d > 31)
        return fallback;
    if (digits(time, 0, 2, h) && time.size() >= 5 && time[2] == ':' && digits(time, 3, 2, mi)) {
        if (time.size() >= 8 && time[5] == ':') digits(time, 6, 2, se);
    } else {
        h = mi = se = 0;
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = std::clamp(h, 0, 23);
    tm.tm_min = std::clamp(mi, 0, 59);
    tm.tm_sec = std::clamp(se, 0, 59);
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    return t == std::time_t(-1) ? fallback : t;
}

std::string waitText(int seconds) {
    const int s = std::max(1, seconds);
    if (s < 60) return i18n::trn("gif.wait.seconds", s);
    if (s >= 300) return i18n::trn("gif.wait.minutes", (s + 59) / 60);
    if (s % 60 == 0) return i18n::trn("gif.wait.minutes", s / 60);
    return i18n::trf("gif.wait.both", {i18n::trn("gif.wait.minutes", s / 60), i18n::trn("gif.wait.seconds", s % 60)});
}

GifSaver::~GifSaver() {
    if (job_.valid()) job_.wait();
}

bool GifSaver::begin(const std::string& owner, uint64_t gameId, const std::string& folder, const std::string& fileName) {
    if (busy()) return false;
    stage_ = Stage::Rendering;
    owner_ = owner;
    gameId_ = gameId;
    folder_ = folder;
    fileName_ = fileName;
    path_.clear();
    error_.clear();
    retryAfterSec_ = 0;
    return true;
}

bool GifSaver::finish(const net::Event& e) {
    if (e.kind != Kind::GifResult || stage_ != Stage::Rendering || e.gameId != gameId_) return false;
    if (!e.ok) {
        stage_ = Stage::Failed;
        error_ = e.error.empty() ? std::string("server_error") : e.error;
        retryAfterSec_ = e.retryAfterSec;
        LOGI("online: no GIF for %s: %s", owner_.c_str(), error_.c_str());
        return true;
    }
    stage_ = Stage::Writing;
    job_ = std::async(std::launch::async, [folder = folder_, name = fileName_, bytes = e.text]() {
        try {
            return archive::saveFile(folder, name, bytes);
        } catch (const std::exception& ex) {   // out of memory, a thread refused
            archive::SaveResult r;
            r.error = ex.what();
            return r;
        }
    });
    return true;
}

bool GifSaver::poll(bool wait) {
    if (stage_ != Stage::Writing || !job_.valid()) return false;
    if (!wait && job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    const archive::SaveResult r = job_.get();
    if (r.ok) {
        stage_ = Stage::Saved;
        path_ = r.path;
        LOGI("online: GIF saved as %s", r.path.c_str());
    } else {
        stage_ = Stage::Failed;
        error_ = "write_failed";
        LOGW("online: the GIF could not be written: %s", r.error.c_str());
    }
    return true;
}

void GifSaver::clear() {
    if (job_.valid()) job_.wait();
    job_ = std::future<archive::SaveResult>();
    stage_ = Stage::Idle;
    owner_.clear();
    path_.clear();
    error_.clear();
    gameId_ = 0;
    retryAfterSec_ = 0;
}

}  // namespace game
