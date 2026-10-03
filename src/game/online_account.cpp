// The account pages' data (see online_account.h).
#include "online_account.h"
#include "../chess/chess.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "game_archive.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>

namespace game {

using Kind = net::Event::Kind;

// ---- Answers awaited ------------------------------------------------------------------------------------

void ServerAnswers::setServer(const std::string& origin) {
    const int pgn = int(Kind::PgnResult);
    const auto awaited = pending_.find(pgn);
    const int pgnPending = awaited == pending_.end() ? 0 : awaited->second;
    const auto arrived = results_.find(pgn);
    net::Event pgnResult;
    const bool pgnArrived = arrived != results_.end();
    if (pgnArrived) pgnResult = std::move(arrived->second);
    origin_ = origin;
    results_.clear();
    pending_.clear();
    if (pgnPending > 0) pending_[pgn] = pgnPending;
    if (pgnArrived) results_[pgn] = std::move(pgnResult);
}

void ServerAnswers::expect(Kind k) {
    pending_[int(k)]++;
    results_.erase(int(k));
}

bool ServerAnswers::busy(Kind k) const {
    auto it = pending_.find(int(k));
    return it != pending_.end() && it->second > 0;
}

bool ServerAnswers::take(Kind k, net::Event& out) {
    auto it = results_.find(int(k));
    if (it == results_.end()) return false;
    out = std::move(it->second);
    results_.erase(it);
    return true;
}

bool ServerAnswers::keep(const net::Event& e) { return keep(net::Event(e)); }

bool ServerAnswers::keep(net::Event&& e) {
    if (foreign(e) && e.kind != Kind::PgnResult) return false;
    const int k = int(e.kind);
    auto it = pending_.find(k);
    if (it != pending_.end() && it->second > 0) it->second--;
    results_[k] = std::move(e);
    return true;
}

// ---- History pages ------------------------------------------------------------------------------------

namespace {
bool sameFilter(const net::GamesFilter& a, const net::GamesFilter& b) {
    return a.category == b.category && a.rated == b.rated && a.result == b.result;
}
}  // namespace

uint64_t HistoryPager::restart(const net::GamesFilter& filter) {
    filter_ = filter;
    page_ = net::GamesPage();
    cursors_.clear();
    index_ = 0;
    loaded_ = false;
    error_.clear();
    retryAfter_ = 0;
    waiting_ = true;
    wantBefore_ = 0;
    wantIndex_ = 0;
    sent();
    return 0;
}

bool HistoryPager::next(uint64_t& before) {
    if (waiting_ || !hasNext()) return false;
    waiting_ = true;
    wantBefore_ = before = page_.next;
    wantIndex_ = index_ + 1;
    error_.clear();
    retryAfter_ = 0;
    sent();
    return true;
}

bool HistoryPager::previous(uint64_t& before) {
    if (waiting_ || !hasPrevious() || size_t(index_ - 1) >= cursors_.size()) return false;
    waiting_ = true;
    wantBefore_ = before = cursors_[size_t(index_ - 1)];
    wantIndex_ = index_ - 1;
    error_.clear();
    retryAfter_ = 0;
    sent();
    return true;
}

uint64_t HistoryPager::reload() {
    waiting_ = true;
    wantIndex_ = loaded_ ? index_ : 0;
    wantBefore_ = loaded_ && size_t(index_) < cursors_.size() ? cursors_[size_t(index_)] : 0;
    error_.clear();
    retryAfter_ = 0;
    sent();
    return wantBefore_;
}

void HistoryPager::sent() {
    // Every request gets one answer; a bound all the same, should one never come.
    if (inFlight_.size() >= 32) inFlight_.erase(inFlight_.begin());
    inFlight_.push_back(Request{wantBefore_, filter_});
}

bool HistoryPager::answered(const net::GamesPage& request) {
    for (auto it = inFlight_.begin(); it != inFlight_.end(); ++it)
        if (it->before == request.before && sameFilter(it->filter, request.filter)) {
            inFlight_.erase(it);
            break;
        }
    return waiting_ && request.before == wantBefore_ && sameFilter(request.filter, filter_);
}

bool HistoryPager::accept(const net::GamesPage& page) {
    if (!answered(page)) return false;
    waiting_ = false;
    page_ = page;
    index_ = wantIndex_;
    if (cursors_.size() < size_t(index_) + 1) cursors_.resize(size_t(index_) + 1, 0);
    cursors_[size_t(index_)] = page.before;
    cursors_.resize(size_t(index_) + 1);  // the pages after it are found again from its cursor
    loaded_ = true;
    error_.clear();
    retryAfter_ = 0;
    return true;
}

void HistoryPager::fail(const net::GamesPage& request, const std::string& error, int retryAfterSec) {
    if (!answered(request)) return;
    // The same request asked again meanwhile (the page opened again): its answer is awaited.
    for (const Request& r : inFlight_)
        if (r.before == wantBefore_ && sameFilter(r.filter, filter_)) return;
    waiting_ = false;
    error_ = error.empty() ? std::string("server_error") : error;
    retryAfter_ = std::max(0, retryAfterSec);
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
        else history.fail(e.gamesPage, e.error, e.retryAfterSec);
        break;
    case Kind::GameDetailsResult:
        if (e.ok && e.gameDetails.id == gameWanted) {
            game = e.gameDetails;
            gameLoaded = true;
            gameError.clear();
            gameRetryAfter = 0;
        } else if (!e.ok && e.gameId == gameWanted) {
            gameError = e.error;   // not the failure of a game left meanwhile
            gameRetryAfter = std::max(0, e.retryAfterSec);
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
            sessionsRetryAfter = 0;
        } else {
            sessionsError = e.error;
            sessionsRetryAfter = std::max(0, e.retryAfterSec);
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
    case Kind::GifResult:   // for the GifSaver (the GIF routes need the session: refused, see below)
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

// ---- Saving a game of the history ---------------------------------------------------------------------

void GameSaveState::opened(bool job) {
    replayWanted = false;
    if (job || save == Save::Downloading) return;
    saveId = 0;
    save = Save::Unknown;
    savedPath.clear();
}

bool GameSaveState::lookupDue(uint64_t gameId, bool job) {
    if (saveId == gameId || job || save == Save::Downloading) return false;
    saveId = gameId;
    save = Save::Checking;
    savedPath.clear();
    replayWanted = false;
    return true;
}

bool GameSaveState::request(const archive::ServerGame& g, bool replay) {
    if (replay) replayWanted = true;
    if (save == Save::Saved && saveId == g.gameId) return false;  // a replay: it starts next frame
    if (save == Save::Checking || save == Save::Downloading || save == Save::Writing) return false;
    saveId = g.gameId;
    saveGame = g;
    save = Save::Downloading;
    return true;
}

bool GameSaveState::pgnArrived(const net::Event& e) const {
    return save == Save::Downloading && e.ok && e.gameId == saveId && saveGame.gameId == saveId;
}

bool GameSaveState::replayDue(uint64_t gameId) {
    if (save != Save::Saved || !replayWanted || saveId != gameId) return false;
    replayWanted = false;
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

std::string localTimeText(double epochMs) {
    const std::time_t t = std::time_t(epochMs / 1000.0), now = std::time(nullptr);
    std::tm when{}, today{};
    char buf[64] = "";
    if (archive::localTime(t, when)) {
        // Two separate results: std::localtime would hand out one shared buffer for both.
        bool sameDay = archive::localTime(now, today) && today.tm_yday == when.tm_yday && today.tm_year == when.tm_year;
        if (sameDay) std::snprintf(buf, sizeof buf, "%02d:%02d", when.tm_hour, when.tm_min);
        else std::snprintf(buf, sizeof buf, "%02d.%02d.%04d %02d:%02d", when.tm_mday, when.tm_mon + 1, when.tm_year + 1900, when.tm_hour, when.tm_min);
    }
    return i18n::ltr(buf);
}

std::string durationText(double ms) {
    long long s = std::max(0LL, (long long)std::ceil(ms / 1000.0));
    char buf[32];
    if (s >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(buf, sizeof buf, "%lld:%02lld", s / 60, s % 60);
    return i18n::ltr(buf);
}

std::string onlineErrorText(const std::string& code, int retryAfterSec, int64_t bannedUntilMs) {
    if (code.empty()) return "";
    if (code == "rate_limited") {
        if (retryAfterSec > 0) return i18n::trf("online.err.rate_limited_for", {durationText(retryAfterSec * 1000.0)});
        return i18n::tr("online.err.rate_limited");
    }
    if (code == "server_busy") {
        // Too many password checks at once on the server (its hash queue is full): not the player's fault.
        if (retryAfterSec > 0) return i18n::trf("online.err.server_busy_for", {durationText(retryAfterSec * 1000.0)});
        return i18n::tr("online.err.server_busy");
    }
    if (code == "too_many_attempts") {
        if (retryAfterSec > 0) return i18n::trf("online.err.too_many_attempts_for", {durationText(retryAfterSec * 1000.0)});
        return i18n::tr("online.err.too_many_attempts");
    }
    if (code == "banned") {
        if (bannedUntilMs > 0) return i18n::trf("online.err.banned_until", {localTimeText(double(bannedUntilMs))});
        return i18n::tr("online.err.banned");
    }
    static const char* known[] = {"invalid_credentials", "email_unverified", "network", "tls", "certificate", "incompatible",
                                  "unauthorized", "username_taken", "email_taken", "invalid_username", "invalid_email",
                                  "weak_password", "invalid_code", "expired", "registration_closed", "sso_cancelled",
                                  "server_error", "timeout", "offline", "invalid_password", "mfa_code_required",
                                  "password_not_set", "same_email", "not_found", "invalid_response"};
    for (const char* k : known)
        if (code == k) return i18n::tr(std::string("online.err.") + k);
    return i18n::trf("online.err.other", {code});
}

std::string exportFileName(const std::string& host, const std::string& username, std::time_t when) {
    std::tm tm{};
    const bool have = archive::localTime(when, tm);
    char date[32] = "0000-00-00";
    if (have) std::snprintf(date, sizeof date, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return archive::sanitizeName(host, 64) + "_" + archive::sanitizeName(username, 40) + "_" + date + ".json";
}

// ---- Animated GIFs ------------------------------------------------------------------------------------

std::string gifFileName(std::time_t started, const std::string& white, const std::string& black, uint64_t gameId) {
    std::tm tm{};
    const bool have = started > 0 && archive::localTime(started, tm);
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

std::time_t pgnGameStart(const std::string& date, const std::string& time, const std::string& utcDate, const std::string& utcTime,
                         std::time_t fallback) {
    using archive::digitsAt;
    const bool hasTime = digitsAt(time, 0, 2) >= 0 && time.size() >= 5 && time[2] == ':' && digitsAt(time, 3, 2) >= 0;
    if (hasTime) return pgnLocalTime(date, time, fallback);
    const int y = digitsAt(utcDate, 0, 4), mo = digitsAt(utcDate, 5, 2), d = digitsAt(utcDate, 8, 2);
    const int h = digitsAt(utcTime, 0, 2), mi = digitsAt(utcTime, 3, 2), se = digitsAt(utcTime, 6, 2);
    if (y >= 1970 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h >= 0 && h <= 23 && mi >= 0 && mi <= 59 && se >= 0 && se <= 60 &&
        utcTime.size() >= 8 && utcTime[2] == ':' && utcTime[5] == ':')
        return std::time_t(archive::daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se);
    return pgnLocalTime(date, std::string(), fallback);
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

bool GifSaver::finish(net::Event&& e) {
    if (e.kind != Kind::GifResult || stage_ != Stage::Rendering || e.gameId != gameId_) return false;
    if (!e.ok) {
        stage_ = Stage::Failed;
        error_ = e.error.empty() ? std::string("server_error") : e.error;
        retryAfterSec_ = e.retryAfterSec;
        LOGI("online: no GIF for %s: %s", owner_.c_str(), error_.c_str());
        return true;
    }
    // The bytes (up to 16 MiB) move to the write thread: no copy on the game's frame.
    try {
        job_ = std::async(std::launch::async, [folder = folder_, name = fileName_, bytes = std::move(e.text)]() {
            try {
                return archive::saveFile(folder, name, bytes);
            } catch (const std::exception& ex) {   // out of memory
                archive::SaveResult r;
                r.error = ex.what();
                return r;
            }
        });
    } catch (const std::exception& ex) {   // no memory or no thread to start the write
        stage_ = Stage::Failed;
        error_ = "write_failed";
        LOGW("online: the GIF could not be written: %s", ex.what());
        return true;
    }
    stage_ = Stage::Writing;
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
