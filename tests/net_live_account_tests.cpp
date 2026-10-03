// Live check of the account API (opt-in): the game's net::OnlineClient against a real dedicated
// server, every call of the account API batch (dedicated-server/docs/API.md sections 5 to 11,
// net/online_client.h "account API" and "animated GIFs").
//
// dedicated-server/tools/live-cpp-check.js (part "account") starts the server (one shard, e-mail
// confirmation on, the mails read from its log transport), registers the accounts, plays the games
// of the C++ player's account through the realtime protocol, then runs, from the root of the source
// tree:
//   SCACELITH_NET_LIVE_ACCOUNT=host:port:<pin hex>:<control port> ./scacelith_tests net_live_account
// The control port is the harness's own small HTTP server on 127.0.0.1: the games it played
// (GET /state), the mails, the e-mail change link opened as a browser would, a session made to
// expire in the database, a session revoked from another device, a challenge sent by the rival,
// TOTP codes and the server's metrics.
//
// Each scenario prints "== <scenario>: ok" or "== <scenario>: FAILED (n checks)" so that the run
// reads as a report. The scenarios run in order on one account, which they finally delete:
//   the pin saved at sign-in (left out by fetchServerInfo(true): the certificate refused),
//   history (fetchMyGames: every game, filters, paging, errors), game details (fetchGame: own
//   games, another players' game, unknown ids), PGN (downloadPgn: tags in order, [%clk] / [%emt]
//   read back by chess::pgn against the record's clocks, the result and the termination), GIFs
//   (downloadGameGif / renderPgnGif decoded: header, screen size, frames = plies + 1, delays, the
//   per-account render quota answered 429 rate_limited with retryAfter, cache hits served past it),
//   preferences (setAcceptChallenges, with the rival's challenge refused then delivered again),
//   e-mail change (errors, the link mailed and confirmed), export (shape of the document),
//   sessions (fetchSessions, revokeSession of another device and of this one, a session revoked
//   elsewhere and an expired one: "unauthorized" with sessionLost, the token erased), two-factor
//   re-authentication (recovery code, authenticator code) and deleteAccount.
#include "test.h"
#include "chess/chess.h"
#include "chess/pgn.h"
#include "net/credential_store.h"
#include "net/json.h"
#include "net/online_client.h"
#include "net/protocol_gen.h"
#include "net/transport.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Kind = net::Event::Kind;
namespace json = net::json;

// One scenario of the report: its checks are those failing between its start and its end.
struct Scenario {
    std::string name;
    int before;
    explicit Scenario(std::string n) : name(std::move(n)), before(testing::g_failures) { std::fprintf(stderr, "  -- %s\n", name.c_str()); }
    ~Scenario() {
        int failed = testing::g_failures - before;
        if (failed) std::fprintf(stderr, "  == %s: FAILED (%d checks)\n", name.c_str(), failed);
        else std::fprintf(stderr, "  == %s: ok\n", name.c_str());
    }
};

std::string queryEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// The harness's control server (plain HTTP on the loopback).
struct Control {
    uint16_t port = 0;
    json::Value call(const std::string& method, const std::string& target, int* statusOut = nullptr,
                     const std::string& body = "{}") const {
        net::HttpRequest req;
        req.method = method;
        req.host = "127.0.0.1";
        req.port = port;
        req.tls = false;
        req.path = target;
        req.timeoutMs = 20000;
        if (method == "POST") req.body = body;
        net::HttpResponse resp;
        net::httpRequest(req, resp);
        if (statusOut) *statusOut = resp.status;
        json::Value v;
        if (!resp.error.empty() || !json::parse(resp.body, v)) {
            std::fprintf(stderr, "  control %s %s: %s %d\n", method.c_str(), target.c_str(), resp.error.c_str(), resp.status);
            return json::Value();
        }
        return v;
    }
};

// Drains the client's events until one of `kind` (matching pred) arrives; the others are kept in
// `seen` when given.
bool waitFor(net::OnlineClient& c, Kind kind, net::Event& out, int timeoutMs = 20000, std::vector<net::Event>* seen = nullptr,
             const std::function<bool(const net::Event&)>& pred = nullptr) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        net::Event ev;
        while (c.poll(ev)) {
            bool match = ev.kind == kind && (!pred || pred(ev));
            if (seen) seen->push_back(ev);
            if (match) {
                out = ev;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Sends a command and waits for its result.
net::Event ask(net::OnlineClient& c, Kind kind, const std::function<void()>& send, int timeoutMs = 20000,
               std::vector<net::Event>* seen = nullptr) {
    send();
    net::Event ev;
    if (!waitFor(c, kind, ev, timeoutMs, seen)) {
        std::fprintf(stderr, "  (no answer of kind %d within %d ms)\n", int(kind), timeoutMs);
        ev = net::Event();
        ev.kind = kind;
        ev.error = "no_event";
    }
    return ev;
}

void report(const char* what, const net::Event& ev) {
    std::fprintf(stderr, "  %s: ok=%d error='%s' retryAfter=%d sessionLost=%d\n", what, int(ev.ok), ev.error.c_str(), ev.retryAfterSec,
                 int(ev.sessionLost));
}

// ---- what the harness played ----
struct Expected {
    uint64_t id = 0;
    std::string color, outcome, category, result, termination;
    bool rated = false;
    int plies = 0, status = 0, reason = 0;
    int promoPly = -1;
    std::string promoUci, promoSan;
    int you() const { return color == "white" ? 0 : 1; }
};

uint64_t idOf(const json::Value& v) {
    if (v.isNumber()) return uint64_t(v.asNumber());
    return std::strtoull(v.asString().c_str(), nullptr, 10);
}

// ---- GIF decoding (GIF89a as the server writes it: a global palette, a first frame covering the
// screen, then sub-rectangles of what changed with a transparent index) ----
struct GifFrame {
    int x = 0, y = 0, w = 0, h = 0;
    int delayCs = 0;
    bool transparent = false;
    int disposal = 0;
};
struct Gif {
    std::string error;
    int w = 0, h = 0, colours = 0;
    bool loops = false;
    std::vector<GifFrame> frames;
};

bool lzwDecode(const std::string& data, int minCode, size_t expect, int colours) {
    if (minCode < 2 || minCode > 8) return false;
    const int clear = 1 << minCode, eoi = clear + 1;
    std::vector<int> prefix(4096, -1);
    std::vector<uint8_t> suffix(4096), first(4096);
    for (int i = 0; i < clear; ++i) suffix[size_t(i)] = first[size_t(i)] = uint8_t(i);
    int width = minCode + 1, next = eoi + 1, prev = -1;
    uint32_t acc = 0;
    int bits = 0;
    size_t at = 0, produced = 0;
    std::vector<uint8_t> stack;
    for (;;) {
        while (bits < width) {
            if (at >= data.size()) return false;   // the end-of-information code is missing
            acc |= uint32_t(uint8_t(data[at++])) << bits;
            bits += 8;
        }
        const int code = int(acc & ((1u << width) - 1));
        acc >>= width;
        bits -= width;
        if (code == clear) {
            width = minCode + 1;
            next = eoi + 1;
            prev = -1;
            continue;
        }
        if (code == eoi) break;
        if (prev < 0) {
            if (code >= clear) return false;
            if (code >= colours) return false;
            ++produced;
            prev = code;
            continue;
        }
        if (code > next || (code == next && next >= 4096)) return false;
        const uint8_t head = code < next ? first[size_t(code)] : first[size_t(prev)];
        if (next < 4096) {
            prefix[size_t(next)] = prev;
            suffix[size_t(next)] = head;
            first[size_t(next)] = first[size_t(prev)];
            ++next;
            if (next == (1 << width) && width < 12) ++width;
        }
        stack.clear();
        for (int k = code; k >= 0; k = prefix[size_t(k)]) {
            stack.push_back(suffix[size_t(k)]);
            if (stack.size() > 4096) return false;
        }
        for (uint8_t px : stack)
            if (int(px) >= colours) return false;
        produced += stack.size();
        prev = code;
    }
    return produced == expect;
}

Gif decodeGif(const std::string& s) {
    Gif g;
    size_t at = 0;
    auto fail = [&](const std::string& why) {
        g.error = why + " at byte " + std::to_string(at);
        return g;
    };
    auto byte = [&]() -> int { return at < s.size() ? uint8_t(s[at++]) : -1; };
    auto word = [&]() {
        const int lo = byte(), hi = byte();
        return lo < 0 || hi < 0 ? -1 : lo | hi << 8;
    };
    auto blocks = [&](std::string* into) {
        for (;;) {
            const int n = byte();
            if (n < 0 || at + size_t(n) > s.size()) return false;
            if (n == 0) return true;
            if (into) into->append(s, at, size_t(n));
            at += size_t(n);
        }
    };
    if (s.size() < 13 || s.compare(0, 6, "GIF89a") != 0) return fail("signature");
    at = 6;
    g.w = word();
    g.h = word();
    const int packed = byte();
    byte();   // background colour
    byte();   // aspect ratio
    if (g.w <= 0 || g.h <= 0) return fail("screen size");
    if (packed & 0x80) {
        g.colours = 2 << (packed & 7);
        at += size_t(g.colours) * 3;
    }
    GifFrame pending;
    for (;;) {
        const int b = byte();
        if (b == 0x3B) break;
        if (b == 0x21) {
            const int label = byte();
            std::string body;
            if (!blocks(&body)) return fail("extension");
            if (label == 0xF9) {
                if (body.size() != 4) return fail("graphic control extension");
                pending.disposal = (uint8_t(body[0]) >> 2) & 7;
                pending.transparent = uint8_t(body[0]) & 1;
                pending.delayCs = uint8_t(body[1]) | uint8_t(body[2]) << 8;
            }
            if (label == 0xFF && body.compare(0, 11, "NETSCAPE2.0") == 0) g.loops = true;
            continue;
        }
        if (b != 0x2C) return fail("unknown block " + std::to_string(b));
        GifFrame f = pending;
        pending = GifFrame();
        f.x = word();
        f.y = word();
        f.w = word();
        f.h = word();
        const int ipacked = byte();
        if (f.x < 0 || f.y < 0 || f.w <= 0 || f.h <= 0 || f.x + f.w > g.w || f.y + f.h > g.h) return fail("frame outside the screen");
        if (g.frames.empty() && (f.x != 0 || f.y != 0 || f.w != g.w || f.h != g.h)) return fail("first frame does not cover the screen");
        int colours = g.colours;
        if (ipacked & 0x80) {
            colours = 2 << (ipacked & 7);
            at += size_t(colours) * 3;
        }
        if (colours == 0) return fail("no palette");
        const int minCode = byte();
        std::string data;
        if (!blocks(&data)) return fail("image data");
        if (!lzwDecode(data, minCode, size_t(f.w) * size_t(f.h), colours)) return fail("LZW data of frame " + std::to_string(g.frames.size()));
        g.frames.push_back(f);
    }
    if (at != s.size()) return fail("bytes after the trailer");
    return g;
}

std::string fmtDate(int64_t ms, bool time) {
    std::time_t t = std::time_t(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    if (time) std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    else std::snprintf(buf, sizeof buf, "%04d.%02d.%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::string signedDiff(int d) { return (d >= 0 ? "+" : "") + std::to_string(d); }

size_t countOf(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}

}  // namespace

TEST(net_live_account_api) {
    const char* env = std::getenv("SCACELITH_NET_LIVE_ACCOUNT");
    if (!env) SKIP("SCACELITH_NET_LIVE_ACCOUNT not set");
    REQUIRE(net::transportAvailable());  // asked for, so it must not pass without running
    std::vector<std::string> f;
    {
        std::string s = env, cur;
        for (char ch : s) {
            if (ch == ':') {
                f.push_back(cur);
                cur.clear();
            } else {
                cur += ch;
            }
        }
        f.push_back(cur);
    }
    CHECK_EQ(int(f.size()), 4);
    if (f.size() != 4) return;
    Control ctl;
    ctl.port = uint16_t(std::atoi(f[3].c_str()));
    const json::Value state = ctl.call("GET", "/state");
    CHECK(state.isObject());
    if (!state.isObject()) return;
    const std::string user = state["user"].asString(), pass = state["password"].asString(), rival = state["rival"].asString();
    const std::string firstEmail = state["email"].asString();
    const uint32_t userId = uint32_t(state["userId"].asInt());
    const int renderQuota = int(state["gifUserRendersPerMin"].asInt(4));
    std::vector<Expected> games;
    for (const json::Value& g : state["games"].items()) {
        Expected e;
        e.id = idOf(g["id"]);
        e.color = g["color"].asString();
        e.outcome = g["outcome"].asString();
        e.category = g["category"].asString();
        e.result = g["result"].asString();
        e.termination = g["termination"].asString();
        e.rated = g["rated"].asBool();
        e.plies = int(g["plies"].asInt());
        e.status = int(g["status"].asInt());
        e.reason = int(g["reason"].asInt());
        if (g["promotion"].isObject()) {
            e.promoPly = int(g["promotion"]["ply"].asInt());
            e.promoUci = g["promotion"]["uci"].asString();
            e.promoSan = g["promotion"]["san"].asString();
        }
        games.push_back(e);
    }
    std::sort(games.begin(), games.end(), [](const Expected& a, const Expected& b) { return a.id > b.id; });   // newest first
    const uint64_t otherId = idOf(state["other"]["id"]);
    CHECK_EQ(int(games.size()), 6);
    auto find = [&](const char* outcome, const char* category = nullptr, int plies = -1) -> const Expected* {
        for (const Expected& e : games)
            if (e.outcome == outcome && (!category || e.category == category) && (plies < 0 || e.plies == plies)) return &e;
        return nullptr;
    };
    const Expected* gWin = find("win", "3+2", 5);       // rated, White, resignation
    const Expected* gLoss = find("loss");               // rated, Black
    const Expected* gDraw = find("draw");
    const Expected* gMate = find("win", "custom");      // 7+1, Black, checkmate
    const Expected* gPromo = find("win", "3+2", 9);     // b7xa8=Q
    const Expected* gAborted = find("aborted");
    CHECK(gWin && gLoss && gDraw && gMate && gPromo && gAborted);
    if (!gWin || !gLoss || !gDraw || !gMate || !gPromo || !gAborted) return;

    char credPath[256];
    std::snprintf(credPath, sizeof credPath, "scacelith-live-account-%d.credentials", int(std::time(nullptr) % 100000));
    net::OnlineClient c;
    c.setCredentialsFile(credPath);
    net::ServerEndpoint ep;
    ep.host = f[0];
    ep.apiPort = uint16_t(std::atoi(f[1].c_str()));
    ep.wsPort = 0;
    ep.pinnedSha256 = f[2];
    c.setServer(ep);
    const std::string origin = ep.origin();
    net::Event ev;

    net::Event info = ask(c, Kind::ServerInfoResult, [&] { c.fetchServerInfo(); });
    CHECK(info.ok);
    const std::string serverName = info.info.name;

    auto signIn = [&]() {
        net::Event l = ask(c, Kind::LoginResult, [&] { c.login(user, pass); });
        report("login", l);
        CHECK(l.ok);
        CHECK(c.hasSavedSession());
        return l;
    };

    // ---- sign-in and the account view (GET /account/me additions) ----
    {
        Scenario s("sign-in and account view (hasPassword, acceptChallenges, createdAt, lastLoginAt, pendingEmail)");
        net::Event l = signIn();
        if (!l.ok) {
            std::remove(credPath);
            return;
        }
        ev = ask(c, Kind::AccountResult, [&] { c.fetchAccount(); });
        CHECK(ev.ok);
        CHECK_EQ(ev.origin, origin);
        CHECK_EQ(ev.account.username, user);
        CHECK_EQ(ev.account.userId, userId);
        CHECK_EQ(ev.account.email, firstEmail);
        CHECK(ev.account.emailVerified);
        CHECK(ev.account.hasPassword);
        CHECK(ev.account.acceptChallenges);
        CHECK(ev.account.pendingEmail.empty());
        CHECK(ev.account.createdAtMs > 1700000000000LL);
        CHECK(ev.account.lastLoginAtMs >= ev.account.createdAtMs);
        CHECK(!ev.account.mfaEnabled);
        std::fprintf(stderr, "  account %u %s <%s>, created %lld, last login %lld, %zu rating record(s)\n", ev.account.userId,
                     ev.account.username.c_str(), ev.account.email.c_str(), (long long)ev.account.createdAtMs,
                     (long long)ev.account.lastLoginAtMs, ev.account.ratings.size());
    }
    {
        Scenario s("the pin saved at sign-in: used while the endpoint gives none, left out of one info request (Options' test of an emptied pin field)");
        CHECK(!net::CredentialStore(credPath).pin(origin).empty());
        net::ServerEndpoint bare = ep;
        bare.pinnedSha256.clear();
        c.setServer(bare);
        ev = ask(c, Kind::ServerInfoResult, [&] { c.fetchServerInfo(); });
        report("fetchServerInfo with the saved pin", ev);
        CHECK(ev.ok);
#ifdef _WIN32
        // WinHTTP would hand the next request the connection pooled by this one, whose certificate
        // the pin let through: past the server's keep-alive (5 s), it opens a new one.
        std::this_thread::sleep_for(std::chrono::milliseconds(6000));
#endif
        ev = ask(c, Kind::ServerInfoResult, [&] { c.fetchServerInfo(true); });
        report("fetchServerInfo without the saved pin", ev);
        CHECK(!ev.ok);
#ifdef _WIN32
        CHECK(ev.error == "certificate" || ev.error == "tls");   // Wine's WinHTTP: "tls" (net_tls_pinning_manual)
#else
        CHECK_EQ(ev.error, std::string("certificate"));
#endif
        CHECK(!net::CredentialStore(credPath).pin(origin).empty());   // nothing forgotten
        ev = ask(c, Kind::AccountResult, [&] { c.fetchAccount(); });
        report("fetchAccount with the saved pin", ev);
        CHECK(ev.ok);
        c.setServer(ep);
    }

    // ---- history: GET /account/games ----
    std::vector<net::GameSummary> all;
    {
        Scenario s("fetchMyGames: every game, newest first, summary fields");
        ev = ask(c, Kind::GamesResult, [&] { c.fetchMyGames(0, 50, net::GamesFilter()); });
        report("fetchMyGames", ev);
        CHECK(ev.ok);
        CHECK_EQ(ev.gamesPage.total, int(games.size()));
        CHECK_EQ(ev.gamesPage.next, uint64_t(0));
        CHECK_EQ(ev.gamesPage.games.size(), games.size());
        all = ev.gamesPage.games;
        for (size_t i = 0; i < all.size() && i < games.size(); ++i) {
            const net::GameSummary& g = all[i];
            const Expected& e = games[i];
            CHECK_EQ(g.id, e.id);
            CHECK_EQ(g.you, e.you());
            CHECK_EQ(g.category, e.category);
            CHECK_EQ(g.rated, e.rated);
            CHECK_EQ(g.result, e.result);
            CHECK_EQ(g.status, e.status);
            CHECK_EQ(g.reason, e.reason);
            CHECK_EQ(g.plies, e.plies);
            CHECK_EQ(g.baseMs, int64_t(e.category == "custom" ? 420000 : 180000));
            CHECK_EQ(g.incMs, int64_t(e.category == "custom" ? 1000 : 2000));
            CHECK_EQ(e.you() == 0 ? g.white.name : g.black.name, user);
            CHECK_EQ(e.you() == 0 ? g.black.name : g.white.name, rival);
            CHECK(g.startedAtMs > 0 && g.endedAtMs >= g.startedAtMs);
            CHECK(g.white.rating > 0 && g.black.rating > 0);
            // A rated game that changed the ratings has ratingAfter / ratingDiff; casual ones never.
            if (!e.rated) CHECK(!g.white.ratingChanged && !g.black.ratingChanged);
            if (g.white.ratingChanged) CHECK_EQ(g.white.ratingAfter - g.white.rating, g.white.ratingDiff);
            if (g.black.ratingChanged) CHECK_EQ(g.black.ratingAfter - g.black.rating, g.black.ratingDiff);
            std::fprintf(stderr, "  %llu %s %s %s %s plies %d result %s; white %s %d%s, black %s %d%s\n", (unsigned long long)g.id,
                         e.color.c_str(), e.outcome.c_str(), g.category.c_str(), g.rated ? "rated" : "casual", g.plies, g.result.c_str(),
                         g.white.name.c_str(), g.white.rating,
                         g.white.ratingChanged ? (" -> " + std::to_string(g.white.ratingAfter) + " (" + signedDiff(g.white.ratingDiff) + ")").c_str() : "",
                         g.black.name.c_str(), g.black.rating,
                         g.black.ratingChanged ? (" -> " + std::to_string(g.black.ratingAfter) + " (" + signedDiff(g.black.ratingDiff) + ")").c_str() : "");
        }
    }
    {
        Scenario s("fetchMyGames: filters (rated, result, category with '+', combined, invalid)");
        struct F {
            const char* label;
            net::GamesFilter filter;
            std::function<bool(const Expected&)> keep;
        };
        auto mk = [](const char* cat, int rated, const char* result) {
            net::GamesFilter g;
            g.category = cat;
            g.rated = rated;
            g.result = result;
            return g;
        };
        std::vector<F> filters = {
            {"rated", mk("", 1, ""), [](const Expected& e) { return e.rated; }},
            {"casual", mk("", 0, ""), [](const Expected& e) { return !e.rated; }},
            {"win", mk("", -1, "win"), [](const Expected& e) { return e.outcome == "win"; }},
            {"loss", mk("", -1, "loss"), [](const Expected& e) { return e.outcome == "loss"; }},
            {"draw", mk("", -1, "draw"), [](const Expected& e) { return e.outcome == "draw"; }},
            {"custom", mk("custom", -1, ""), [](const Expected& e) { return e.category == "custom"; }},
            {"3+2", mk("3+2", -1, ""), [](const Expected& e) { return e.category == "3+2"; }},
            {"rated win", mk("", 1, "win"), [](const Expected& e) { return e.rated && e.outcome == "win"; }},
            {"casual 3+2", mk("3+2", 0, ""), [](const Expected& e) { return !e.rated && e.category == "3+2"; }},
            {"casual 3+2 win", mk("3+2", 0, "win"), [](const Expected& e) { return !e.rated && e.category == "3+2" && e.outcome == "win"; }},
        };
        for (const F& fl : filters) {
            std::vector<uint64_t> want;
            for (const Expected& e : games)
                if (fl.keep(e)) want.push_back(e.id);
            ev = ask(c, Kind::GamesResult, [&] { c.fetchMyGames(0, 50, fl.filter); });
            std::vector<uint64_t> got;
            for (const net::GameSummary& g : ev.gamesPage.games) got.push_back(g.id);
            std::fprintf(stderr, "  filter %-15s ok=%d error='%s' total %d, %zu games (expected %zu)\n", fl.label, int(ev.ok), ev.error.c_str(),
                         ev.gamesPage.total, got.size(), want.size());
            CHECK(ev.ok);
            CHECK(got == want);
            CHECK_EQ(ev.gamesPage.total, int(want.size()));
            CHECK_EQ(ev.gamesPage.filter.category, fl.filter.category);
            CHECK_EQ(ev.gamesPage.filter.rated, fl.filter.rated);
            CHECK_EQ(ev.gamesPage.filter.result, fl.filter.result);
        }
        ev = ask(c, Kind::GamesResult, [&] { c.fetchMyGames(0, 10, mk("bogus", -1, "")); });
        report("filter category=bogus", ev);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("invalid_filter"));
        CHECK(ev.gamesPage.games.empty());
        CHECK_EQ(ev.gamesPage.filter.category, std::string("bogus"));
        ev = ask(c, Kind::GamesResult, [&] { c.fetchMyGames(0, 10, mk("", -1, "aborted")); });
        report("filter result=aborted", ev);
        CHECK_EQ(ev.error, std::string("invalid_filter"));
    }
    {
        Scenario s("fetchMyGames: paging (limit 2, then a filtered page of 3, a cursor past the oldest)");
        std::vector<uint64_t> got;
        uint64_t before = 0;
        int pages = 0;
        do {
            ev = ask(c, Kind::GamesResult, [&] { c.fetchMyGames(before, 2, net::GamesFilter()); });
            CHECK(ev.ok);
            CHECK_EQ(ev.gamesPage.before, before);
            CHECK_EQ(ev.gamesPage.total, int(games.size()));
            CHECK(ev.gamesPage.games.size() <= 2);
            for (const net::GameSummary& g : ev.gamesPage.games) got.push_back(g.id);
            std::fprintf(stderr, "  page %d before %llu: %zu games, next %llu\n", pages, (unsigned long long)before, ev.gamesPage.games.size(),
                         (unsigned long long)ev.gamesPage.next);
            if (!ev.ok) break;
            if (ev.gamesPage.next) CHECK_EQ(ev.gamesPage.next, ev.gamesPage.games.back().id);
            before = ev.gamesPage.next;
        } while (before && ++pages < 10);
        std::vector<uint64_t> want;
        for (const Expected& e : games) want.push_back(e.id);
        CHECK(got == want);
        CHECK_EQ(pages, 2);   // pages 0, 1, 2: three pages of two
        // Casual games only, three a page: 3 + 1.
        net::GamesFilter casual;
        casual.rated = 0;
        ev = ask(c, Kind::GamesResult, [&] { c.fetchMyGames(0, 3, casual); });
        CHECK(ev.ok);
        CHECK_EQ(ev.gamesPage.games.size(), size_t(3));
        CHECK_EQ(ev.gamesPage.total, 4);
        const uint64_t next = ev.gamesPage.next;
        CHECK(next != 0);
        ev = ask(c, Kind::GamesResult, [&] { c.fetchMyGames(next, 3, casual); });
        CHECK(ev.ok);
        CHECK_EQ(ev.gamesPage.games.size(), size_t(1));
        CHECK_EQ(ev.gamesPage.next, uint64_t(0));
        CHECK_EQ(ev.gamesPage.total, 4);
        // Past the oldest game: an empty last page; the total still counts every page.
        ev = ask(c, Kind::GamesResult, [&] { c.fetchMyGames(games.back().id, 20, net::GamesFilter()); });
        report("cursor = the oldest game", ev);
        CHECK(ev.ok);
        CHECK(ev.gamesPage.games.empty());
        CHECK_EQ(ev.gamesPage.next, uint64_t(0));
        CHECK_EQ(ev.gamesPage.total, int(games.size()));
    }

    // ---- game details: GET /games/:id ----
    std::vector<net::GameDetails> details(games.size());
    {
        Scenario s("fetchGame: own games (you, reportable, moves with clocks, promotion), the summary's fields");
        for (size_t i = 0; i < games.size(); ++i) {
            const Expected& e = games[i];
            ev = ask(c, Kind::GameDetailsResult, [&] { c.fetchGame(e.id); });
            CHECK(ev.ok);
            CHECK(!ev.sessionLost);
            CHECK_EQ(ev.gameId, e.id);
            const net::GameDetails& d = ev.gameDetails;
            details[i] = d;
            CHECK_EQ(d.id, e.id);
            CHECK_EQ(d.you, e.you());
            CHECK(d.reportable);
            CHECK_EQ(int(d.moves.size()), e.plies);
            CHECK_EQ(d.plies, e.plies);
            CHECK_EQ(d.result, e.result);
            CHECK_EQ(d.status, e.status);
            CHECK_EQ(d.reason, e.reason);
            CHECK_EQ(d.rematchOf, uint64_t(0));
            if (i < all.size()) {
                const net::GameSummary& g = all[i];
                CHECK_EQ(d.white.name, g.white.name);
                CHECK_EQ(d.black.name, g.black.name);
                CHECK_EQ(d.white.rating, g.white.rating);
                CHECK_EQ(d.white.ratingChanged, g.white.ratingChanged);
                CHECK_EQ(d.white.ratingDiff, g.white.ratingDiff);
                CHECK_EQ(d.black.ratingDiff, g.black.ratingDiff);
                CHECK_EQ(d.startedAtMs, g.startedAtMs);
                CHECK_EQ(d.endedAtMs, g.endedAtMs);
                CHECK_EQ(d.baseMs, g.baseMs);
                CHECK_EQ(d.incMs, g.incMs);
                CHECK_EQ(d.category, g.category);
            }
            // The moves replay in the client's own rules, clocks and spent times present.
            chess::Game mirror;
            bool legal = true;
            for (const net::GameDetails::Ply& p : d.moves) {
                CHECK(p.clockMs >= 0);
                CHECK(p.spentMs >= 0);
                chess::Move mv = mirror.position().findLegal(chess::Square(net::moveFrom(p.move)), chess::Square(net::moveTo(p.move)),
                                                              chess::PieceType(net::movePromo(p.move)));
                if (!mv.valid() || !mirror.play(mv)) {
                    legal = false;
                    break;
                }
            }
            CHECK(legal);
            if (e.reason == int(net::proto::EndReason::Checkmate)) CHECK(mirror.position().inCheck() && mirror.position().legalMoves().empty());
            if (e.promoPly >= 0 && e.promoPly < int(d.moves.size())) {
                const net::GameDetails::Ply& p = d.moves[size_t(e.promoPly)];
                CHECK_EQ(p.uci, e.promoUci);
                CHECK_EQ(net::moveFrom(p.move), 49);   // b7
                CHECK_EQ(net::moveTo(p.move), 56);     // a8
                CHECK_EQ(net::movePromo(p.move), int(chess::Queen));
            }
        }
    }
    {
        Scenario s("fetchGame: another players' game (public view), unknown and invalid ids");
        ev = ask(c, Kind::GameDetailsResult, [&] { c.fetchGame(otherId); });
        report("other game", ev);
        CHECK(ev.ok);
        CHECK_EQ(ev.gameDetails.id, otherId);
        CHECK_EQ(ev.gameDetails.you, 2);
        CHECK(!ev.gameDetails.reportable);
        CHECK_EQ(ev.gameDetails.white.name, rival);
        CHECK_EQ(ev.gameDetails.black.name, state["third"].asString());
        CHECK_EQ(int(ev.gameDetails.moves.size()), 2);
        CHECK_EQ(ev.gameDetails.result, std::string("1-0"));
        ev = ask(c, Kind::GameDetailsResult, [&] { c.fetchGame(9999999999999ULL); });
        report("unknown game", ev);
        CHECK_EQ(ev.error, std::string("not_found"));
        CHECK(c.hasSavedSession());
        ev = ask(c, Kind::GameDetailsResult, [&] { c.fetchGame(0); });
        CHECK_EQ(ev.error, std::string("invalid_game_id"));
    }
    {
        Scenario s("report() then fetchGame: reportable turns false");
        const size_t i = size_t(gLoss - games.data());
        ev = ask(c, Kind::ReportResult, [&] { c.report(gLoss->id, rival, "other", "live check report"); });
        report("report", ev);
        CHECK(ev.ok);
        ev = ask(c, Kind::GameDetailsResult, [&] { c.fetchGame(gLoss->id); });
        CHECK(ev.ok);
        CHECK(!ev.gameDetails.reportable);
        if (ev.ok) details[i].reportable = false;
    }

    // ---- PGN: GET /games/:id/pgn ----
    std::string promoPgn;
    {
        Scenario s("downloadPgn: tags in order, [%clk]/[%emt] read back against the record, result, termination");
        std::vector<const Expected*> pick = {gWin, gLoss, gDraw, gMate, gPromo, gAborted};
        for (const Expected* e : pick) {
            const net::GameDetails& d = details[size_t(e - games.data())];
            ev = ask(c, Kind::PgnResult, [&] { c.downloadPgn(e->id); });
            CHECK(ev.ok);
            CHECK_EQ(ev.gameId, e->id);
            const std::string& text = ev.text;
            if (e == gPromo) promoPgn = text;
            CHECK(text.find('\r') == std::string::npos);
            bool movetext = false, wrapped = true;
            size_t lineStart = 0;
            while (lineStart < text.size()) {
                size_t nl = text.find('\n', lineStart);
                if (nl == std::string::npos) nl = text.size();
                const std::string line = text.substr(lineStart, nl - lineStart);
                if (line.empty()) movetext = true;
                else if (movetext && line.size() >= 80) wrapped = false;
                lineStart = nl + 1;
            }
            CHECK(wrapped);
            CHECK_EQ(countOf(text, "[%clk "), size_t(e->plies));
            CHECK_EQ(countOf(text, "[%emt "), size_t(e->plies));
            chess::pgn::Result<chess::pgn::ParsedGame> parsed = chess::pgn::read(text);
            CHECK_EQ(parsed.games.size(), size_t(1));
            if (parsed.games.size() != 1) continue;
            const chess::pgn::ParsedGame& pg = parsed.games[0];
            if (!pg.ok()) std::fprintf(stderr, "  game %llu: %s\n", (unsigned long long)e->id, pg.error.text().c_str());
            CHECK(pg.ok());
            const chess::pgn::Record& r = pg.record;
            // The tags in the documented order.
            std::vector<std::string> names, want = {"Event", "Site", "Date", "Round", "White", "Black", "Result", "UTCDate", "UTCTime",
                                                    "WhiteElo", "BlackElo"};
            if (d.white.ratingChanged) {
                want.push_back("WhiteRatingDiff");
                want.push_back("BlackRatingDiff");
            }
            for (const char* t : {"TimeControl", "Termination", "PlyCount", "ScacelithGameId"}) want.push_back(t);
            for (const chess::pgn::Tag& t : r.tags) names.push_back(t.name);
            if (names != want) {
                std::string got;
                for (const std::string& n : names) got += n + " ";
                std::fprintf(stderr, "  game %llu tags: %s\n", (unsigned long long)e->id, got.c_str());
            }
            CHECK(names == want);
            const std::string category = e->category;
            const std::string event = r.tag("Event");
            CHECK_EQ(event.compare(0, serverName.size(), serverName), 0);
            CHECK(event.find(e->rated ? " rated " : " casual ") != std::string::npos);
            CHECK_EQ(r.tag("Site"), std::string("localhost"));
            CHECK_EQ(r.tag("Date"), fmtDate(d.startedAtMs, false));
            CHECK_EQ(r.tag("UTCDate"), fmtDate(d.startedAtMs, false));
            CHECK_EQ(r.tag("UTCTime"), fmtDate(d.startedAtMs, true));
            CHECK_EQ(r.tag("Round"), std::string("-"));
            CHECK_EQ(r.tag("White"), d.white.name);
            CHECK_EQ(r.tag("Black"), d.black.name);
            CHECK_EQ(r.tag("Result"), e->result);
            CHECK_EQ(r.result, e->result);
            CHECK_EQ(r.tag("WhiteElo"), d.white.rating > 0 ? std::to_string(d.white.rating) : std::string("-"));
            CHECK_EQ(r.tag("BlackElo"), d.black.rating > 0 ? std::to_string(d.black.rating) : std::string("-"));
            if (d.white.ratingChanged) {
                CHECK_EQ(r.tag("WhiteRatingDiff"), signedDiff(d.white.ratingDiff));
                CHECK_EQ(r.tag("BlackRatingDiff"), signedDiff(d.black.ratingDiff));
            }
            CHECK_EQ(r.tag("TimeControl"), std::to_string(d.baseMs / 1000) + "+" + std::to_string(d.incMs / 1000));
            CHECK_EQ(r.tag("Termination"), e->termination);
            CHECK_EQ(r.tag("PlyCount"), std::to_string(e->plies));
            CHECK_EQ(r.tag("ScacelithGameId"), std::to_string(e->id));
            // The moves and the clocks: the record's values in tenths, truncated.
            CHECK_EQ(r.plies.size(), d.moves.size());
            for (size_t k = 0; k < r.plies.size() && k < d.moves.size(); ++k) {
                const chess::pgn::Ply& p = r.plies[k];
                const net::GameDetails::Ply& q = d.moves[k];
                CHECK_EQ(int(p.move.from), net::moveFrom(q.move));
                CHECK_EQ(int(p.move.to), net::moveTo(q.move));
                CHECK_EQ(int(p.move.promotion), net::movePromo(q.move));
                CHECK_EQ(p.clockMs, q.clockMs / 100 * 100);
                CHECK_EQ(p.elapsedMs, q.spentMs / 100 * 100);
            }
            if (e->promoPly >= 0 && e->promoPly < int(r.plies.size())) CHECK_EQ(r.plies[size_t(e->promoPly)].san, e->promoSan);
            // The end in words after the last move (before the first one when there is none).
            const std::string words = r.plies.empty() ? r.comment : r.plies.back().comment;
            std::fprintf(stderr, "  game %llu: %zu bytes, Event \"%s\", %s %s, Termination \"%s\", end comment \"%s\"\n",
                         (unsigned long long)e->id, text.size(), event.c_str(), r.tag("WhiteElo").c_str(), r.tag("BlackElo").c_str(),
                         r.tag("Termination").c_str(), words.c_str());
            const char* expectWords = e->reason == int(net::proto::EndReason::Checkmate)     ? "Checkmate"
                                      : e->reason == int(net::proto::EndReason::Resignation) ? "Resignation"
                                      : e->reason == int(net::proto::EndReason::Agreement)   ? "Draw by agreement"
                                                                                               : "Game aborted";
            CHECK(words.find(expectWords) != std::string::npos);
        }
        // Another players' game, and a game that does not exist.
        ev = ask(c, Kind::PgnResult, [&] { c.downloadPgn(otherId); });
        CHECK(ev.ok);
        CHECK(ev.text.find("[ScacelithGameId \"" + std::to_string(otherId) + "\"]") != std::string::npos);
        ev = ask(c, Kind::PgnResult, [&] { c.downloadPgn(9999999999999ULL); });
        report("PGN of an unknown game", ev);
        CHECK_EQ(ev.error, std::string("not_found"));
        CHECK(ev.text.empty());
    }

    // ---- GIFs: GET /games/:id/gif, POST /gif ----
    {
        Scenario s("downloadGameGif / renderPgnGif: decoded, the render quota (429 + retryAfter), cache hits past it");
        int status = 0;
        const double hits0 = ctl.call("GET", "/metric?name=" + queryEncode("scacelith_gif_cache_total{result=\"hit\"}"))["value"].asNumber();
        const double renders0 = ctl.call("GET", "/metric?name=" + queryEncode("scacelith_gif_renders_total{result=\"ok\"}"))["value"].asNumber();
        auto gifOf = [&](uint64_t id, const net::GifOptions& o) { return ask(c, Kind::GifResult, [&] { c.downloadGameGif(id, o); }, 90000); };
        auto gifOfPgn = [&](const std::string& pgn, const net::GifOptions& o) {
            return ask(c, Kind::GifResult, [&] { c.renderPgnGif(pgn, o); }, 90000);
        };
        auto checkGif = [&](const char* what, const net::Event& g, int plies, int w, int h, int delayMs) {
            report(what, g);
            CHECK(g.ok);
            if (!g.ok) return;
            Gif gif = decodeGif(g.text);
            std::fprintf(stderr, "  %s: %zu bytes, %dx%d, %zu frames, delays", what, g.text.size(), gif.w, gif.h, gif.frames.size());
            for (const GifFrame& fr : gif.frames) std::fprintf(stderr, " %d", fr.delayCs);
            std::fprintf(stderr, "%s%s\n", gif.loops ? ", loops" : "", gif.error.empty() ? "" : (", ERROR " + gif.error).c_str());
            CHECK(gif.error.empty());
            CHECK_EQ(gif.w, w);
            CHECK_EQ(gif.h, h);
            CHECK(gif.loops);
            CHECK_EQ(int(gif.frames.size()), plies + 1);
            if (gif.frames.empty()) return;
            const int step = delayMs / 10;
            // The start position at least 1 s (the delay when longer); a game without a move has
            // only that frame, which is also the final position (held 3 s).
            if (plies > 0) CHECK_EQ(gif.frames.front().delayCs, std::max(100, step));
            else CHECK(gif.frames.front().delayCs >= 100);
            for (size_t k = 1; k + 1 < gif.frames.size(); ++k) CHECK_EQ(gif.frames[k].delayCs, step);
            if (gif.frames.size() > 1) CHECK_EQ(gif.frames.back().delayCs, 300);
            for (size_t k = 1; k < gif.frames.size(); ++k) CHECK(gif.frames[k].transparent);
        };
        net::GifOptions small;
        small.size = "small";
        small.delayMs = 250;
        // 1. A render.
        net::Event first = gifOf(gWin->id, small);
        CHECK_EQ(first.gameId, gWin->id);
        checkGif("GET small 250 ms (render 1)", first, gWin->plies, 284, 350, 250);
        // 2. The same picture: the worker's cache, byte for byte, no render.
        ev = gifOf(gWin->id, small);
        report("GET small 250 ms again (cache)", ev);
        CHECK(ev.ok);
        CHECK(ev.text == first.text);
        // 3. A PGN text: the promotion game as the server wrote it, large, Black at the bottom, no
        // coordinates.
        net::GifOptions large;
        large.size = "large";
        large.orientation = "black";
        large.delayMs = 800;
        large.coords = false;
        net::Event fromPgn = gifOfPgn(promoPgn, large);
        CHECK_EQ(fromPgn.gameId, uint64_t(0));
        checkGif("POST large black 800 ms no coords (render 2)", fromPgn, gPromo->plies, 600, 748, 800);
        // 4. The aborted game (no move): one frame.
        net::GifOptions medium;
        ev = gifOf(gAborted->id, medium);
        checkGif("GET medium, aborted game (render 3)", ev, 0, 424, 515, 500);
        // 5. The checkmate at the fastest pace.
        net::GifOptions fast;
        fast.size = "small";
        fast.delayMs = 100;
        fast.orientation = "black";
        ev = gifOf(gMate->id, fast);
        checkGif("GET small 100 ms black (render 4)", ev, gMate->plies, 284, 350, 100);
        // 6. The quota: GIF_USER_RENDERS_PER_MIN renders a minute per account.
        std::fprintf(stderr, "  render quota of the account: %d a minute\n", renderQuota);
        ev = gifOf(gDraw->id, small);
        report("GET a 5th GIF to render", ev);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("rate_limited"));
        CHECK(ev.retryAfterSec >= 1 && ev.retryAfterSec <= 60);
        CHECK(ev.text.empty());
        CHECK(!ev.sessionLost);
        CHECK(c.hasSavedSession());
        ev = gifOfPgn("1. e4 e5 2. Nf3 Nc6 *", small);
        report("POST a new PGN past the quota", ev);
        CHECK_EQ(ev.error, std::string("rate_limited"));
        CHECK(ev.retryAfterSec >= 1);
        // 7. Cache hits past the quota: not charged.
        ev = gifOf(gWin->id, small);
        report("GET the first GIF again past the quota (cache)", ev);
        CHECK(ev.ok);
        CHECK(ev.text == first.text);
        ev = gifOfPgn(promoPgn, large);
        report("POST the promotion PGN again past the quota (cache)", ev);
        CHECK(ev.ok);
        CHECK(ev.text == fromPgn.text);
        const double hits = ctl.call("GET", "/metric?name=" + queryEncode("scacelith_gif_cache_total{result=\"hit\"}"), &status)["value"].asNumber();
        const double renders = ctl.call("GET", "/metric?name=" + queryEncode("scacelith_gif_renders_total{result=\"ok\"}"))["value"].asNumber();
        std::fprintf(stderr, "  server metrics: %.0f renders, %.0f cache hits\n", renders - renders0, hits - hits0);
        CHECK_EQ(int(renders - renders0), 4);
        CHECK_EQ(int(hits - hits0), 3);
        // 8. Refusals before any render: options out of range, a PGN the server cannot read, a
        // game it does not have.
        net::GifOptions slow = small;
        slow.delayMs = 50;
        ev = gifOf(gWin->id, slow);
        report("GET delay 50 ms", ev);
        CHECK_EQ(ev.error, std::string("invalid_option"));
        net::GifOptions huge = small;
        huge.size = "huge";
        ev = gifOfPgn(promoPgn, huge);
        report("POST size huge", ev);
        CHECK_EQ(ev.error, std::string("invalid_option"));
        ev = gifOfPgn("1. e4 e5 2. Ke3 *", small);
        report("POST an illegal move", ev);
        CHECK_EQ(ev.error, std::string("invalid_pgn"));
        ev = gifOf(9999999999999ULL, small);
        report("GET an unknown game", ev);
        CHECK_EQ(ev.error, std::string("not_found"));
        ev = gifOf(0, small);
        CHECK_EQ(ev.error, std::string("invalid_game_id"));
        ev = gifOfPgn(std::string(net::OnlineClient::kGifMaxPgnBytes + 1, ' '), small);
        CHECK_EQ(ev.error, std::string("pgn_too_large"));
        CHECK(c.hasSavedSession());
    }

    // ---- preferences: PUT /account/preferences ----
    {
        Scenario s("setAcceptChallenges: none refuses the rival's challenge, all lets it through");
        json::Value ch = ctl.call("POST", "/challenge");
        std::fprintf(stderr, "  challenge before: %s\n", ch["result"].asString().c_str());
        CHECK_EQ(ch["result"].asString(), std::string("delivered"));
        ev = ask(c, Kind::PreferencesResult, [&] { c.setAcceptChallenges(false); });
        report("setAcceptChallenges(false)", ev);
        CHECK(ev.ok);
        CHECK(!ev.account.acceptChallenges);
        ev = ask(c, Kind::AccountResult, [&] { c.fetchAccount(); });
        CHECK(ev.ok);
        CHECK(!ev.account.acceptChallenges);
        ch = ctl.call("POST", "/challenge");
        std::fprintf(stderr, "  challenge with 'none': %s\n", ch["result"].asString().c_str());
        CHECK_EQ(ch["result"].asString(), std::string("UserUnavailable"));
        ev = ask(c, Kind::PreferencesResult, [&] { c.setAcceptChallenges(true); });
        report("setAcceptChallenges(true)", ev);
        CHECK(ev.ok);
        CHECK(ev.account.acceptChallenges);
        ev = ask(c, Kind::AccountResult, [&] { c.fetchAccount(); });
        CHECK(ev.account.acceptChallenges);
        ch = ctl.call("POST", "/challenge");
        std::fprintf(stderr, "  challenge with 'all': %s\n", ch["result"].asString().c_str());
        CHECK_EQ(ch["result"].asString(), std::string("delivered"));
    }

    // ---- e-mail change: POST /account/email ----
    const std::string newEmail = "cpp.new@example.org";
    {
        Scenario s("changeEmail: errors, the link mailed to the new address and confirmed, the notices to the old one");
        ev = ask(c, Kind::EmailChangeResult, [&] { c.changeEmail("not an address", pass, ""); });
        report("invalid address", ev);
        CHECK_EQ(ev.error, std::string("invalid_email"));
        ev = ask(c, Kind::EmailChangeResult, [&] { c.changeEmail(firstEmail, pass, ""); });
        report("same address", ev);
        CHECK_EQ(ev.error, std::string("same_email"));
        ev = ask(c, Kind::EmailChangeResult, [&] { c.changeEmail(newEmail, "wrong password 123", ""); });
        report("wrong password", ev);
        CHECK_EQ(ev.error, std::string("invalid_password"));
        CHECK(c.hasSavedSession());
        ev = ask(c, Kind::EmailChangeResult, [&] { c.changeEmail(newEmail, pass, ""); });
        report("change", ev);
        CHECK(ev.ok);
        CHECK_EQ(ev.status, std::string("verification_sent"));
        ev = ask(c, Kind::AccountResult, [&] { c.fetchAccount(); });
        CHECK_EQ(ev.account.pendingEmail, newEmail);
        CHECK_EQ(ev.account.email, firstEmail);
        json::Value link = ctl.call("GET", "/mail?to=" + queryEncode(newEmail) + "&subject=" + queryEncode("Confirm your new e-mail address"));
        std::fprintf(stderr, "  mail to %s: \"%s\"\n", newEmail.c_str(), link["subject"].asString().c_str());
        CHECK(link["text"].asString().find("/confirm-email-change?token=") != std::string::npos);
        json::Value notice = ctl.call("GET", "/mail?to=" + queryEncode(firstEmail) + "&subject=" + queryEncode("was requested"));
        std::fprintf(stderr, "  mail to %s: \"%s\"\n", firstEmail.c_str(), notice["subject"].asString().c_str());
        CHECK(notice["text"].asString().find("c***@example.org") != std::string::npos);
        CHECK(notice["text"].asString().find(newEmail) == std::string::npos);
        json::Value done = ctl.call("POST", "/confirm-email-change?to=" + queryEncode(newEmail));
        std::fprintf(stderr, "  link opened: GET %d, POST %d, changed %d\n", int(done["getStatus"].asInt()), int(done["status"].asInt()),
                     int(done["changed"].asBool()));
        CHECK(done["changed"].asBool());
        ev = ask(c, Kind::AccountResult, [&] { c.fetchAccount(); });
        CHECK(ev.ok);   // still signed in
        CHECK_EQ(ev.account.email, newEmail);
        CHECK(ev.account.pendingEmail.empty());
        CHECK(ev.account.emailVerified);
        json::Value changed = ctl.call("GET", "/mail?to=" + queryEncode(firstEmail) + "&subject=" + queryEncode("was changed"));
        std::fprintf(stderr, "  mail to %s: \"%s\"\n", firstEmail.c_str(), changed["subject"].asString().c_str());
        CHECK(changed["text"].asString().find("c***@example.org") != std::string::npos);
    }

    // ---- export: POST /account/export ----
    std::string currentLabel;
    {
        Scenario s("exportAccount: wrong password, then the document's shape");
        ev = ask(c, Kind::AccountExportResult, [&] { c.exportAccount("wrong password 123", ""); });
        report("wrong password", ev);
        CHECK_EQ(ev.error, std::string("invalid_password"));
        CHECK(ev.text.empty());
        ev = ask(c, Kind::AccountExportResult, [&] { c.exportAccount(pass, ""); }, 90000);
        report("export", ev);
        CHECK(ev.ok);
        json::Value doc;
        std::string err;
        CHECK(json::parse(ev.text, doc, &err));
        std::fprintf(stderr, "  export: %zu bytes\n", ev.text.size());
        CHECK_EQ(doc["format"].asString(), std::string("scacelith-account-export"));
        CHECK_EQ(doc["version"].asInt(), int64_t(1));
        CHECK(doc["exportedAt"].asInt() > 1700000000000LL);
        CHECK_EQ(doc["server"]["name"].asString(), serverName);
        CHECK_EQ(doc["server"]["host"].asString(), std::string("localhost"));
        CHECK(doc["notes"].isArray() && doc["notes"].size() >= 1 && doc["notes"][0].isString());
        const json::Value& a = doc["account"];
        CHECK_EQ(uint32_t(a["id"].asInt()), userId);
        CHECK_EQ(a["username"].asString(), user);
        CHECK_EQ(a["email"].asString(), newEmail);
        CHECK(a["emailVerified"].asBool());
        CHECK(a["pendingEmail"].isNull());
        CHECK(a["mfaEnabled"].isBool() && !a["mfaEnabled"].asBool());
        CHECK(a["googleLinked"].isBool() && !a["googleLinked"].asBool());
        CHECK(a.has("googleEmail") && a["googleEmail"].isNull());
        CHECK(a["hasPassword"].asBool());
        CHECK_EQ(a["acceptChallenges"].asString(), std::string("all"));
        CHECK(a["createdAt"].isNumber() && a["lastLoginAt"].isNumber());
        bool rated32 = false;
        for (const json::Value& r : doc["ratings"].items()) {
            CHECK(r["peak"].isNumber() && r["updatedAt"].isNumber() && r["rated"].isBool() && r["countedGames"].isNumber());
            if (r["category"].asString() == "3+2") rated32 = r["games"].asInt() == 2;
        }
        CHECK(rated32);
        CHECK(doc["ratingRefunds"].isArray());
        CHECK(doc["sessions"].isArray() && doc["sessions"].size() >= 2);
        for (const json::Value& se : doc["sessions"].items())
            CHECK(se["id"].isNumber() && se["createdAt"].isNumber() && se["expiresAt"].isNumber() && se.has("revokedAt") && se.has("clientLabel") &&
                  se.has("ip"));
        bool emailChanged = false, loggedIn = false, reauthFailed = false;
        for (const json::Value& se : doc["securityEvents"].items()) {
            const std::string k = se["kind"].asString();
            emailChanged = emailChanged || k == "email_changed";
            loggedIn = loggedIn || k == "login";
            reauthFailed = reauthFailed || k == "reauth_failed";
            CHECK(se["at"].isNumber() && se.has("ip") && se.has("detail"));
        }
        CHECK(emailChanged && loggedIn && reauthFailed);
        CHECK(doc["sanctions"].isArray());
        bool aborted = false;
        for (const json::Value& co : doc["conduct"].items()) aborted = aborted || co["kind"].asString() == "abort";
        CHECK(aborted);
        CHECK(doc["reportsFiled"].isArray() && doc["reportsFiled"].size() == 1);
        CHECK_EQ(idOf(doc["reportsFiled"][0]["gameId"]), gLoss->id);
        CHECK_EQ(doc["reportsFiled"][0]["reported"].asString(), rival);
        CHECK_EQ(doc["games"]["total"].asInt(), int64_t(games.size()));
        CHECK_EQ(doc["games"]["list"].size(), games.size());
        for (size_t i = 0; i < games.size() && i < doc["games"]["list"].size(); ++i) {
            const json::Value& g = doc["games"]["list"][i];
            CHECK_EQ(idOf(g["id"]), games[i].id);
            CHECK_EQ(g["outcome"].asString(), games[i].outcome);
        }
        // None of the secrets as a member, at any depth (the notes name them in plain words).
        std::vector<std::string> keys;
        std::function<void(const json::Value&)> walk = [&](const json::Value& v) {
            for (const auto& m : v.members()) {
                keys.push_back(m.first);
                walk(m.second);
            }
            for (const json::Value& item : v.items()) walk(item);
        };
        walk(doc);
        for (const std::string& k : keys) {
            std::string low;
            for (char ch : k) low += char(std::tolower(static_cast<unsigned char>(ch)));
            for (const char* secret : {"password", "secret", "hash", "recovery", "token", "integrity", "anomal"})
                if (low.find(secret) != std::string::npos && k != "hasPassword") {
                    std::fprintf(stderr, "  export member '%s'\n", k.c_str());
                    CHECK(false);
                }
        }
    }

    // ---- sessions: GET /auth/sessions, DELETE /auth/sessions/:id ----
    {
        Scenario s("fetchSessions / revokeSession of another device");
        ev = ask(c, Kind::SessionsResult, [&] { c.fetchSessions(); });
        report("fetchSessions", ev);
        CHECK(ev.ok);
        int current = 0;
        int64_t harness = 0;
        for (const net::SessionInfo& se : ev.sessions) {
            std::fprintf(stderr, "  session %lld '%s'%s created %lld last seen %lld expires %lld\n", (long long)se.id, se.clientLabel.c_str(),
                         se.current ? " (current)" : "", (long long)se.createdAtMs, (long long)se.lastSeenAtMs, (long long)se.expiresAtMs);
            CHECK(se.createdAtMs > 0 && se.lastSeenAtMs >= se.createdAtMs && se.expiresAtMs > se.lastSeenAtMs);
            if (se.current) {
                ++current;
                currentLabel = se.clientLabel;
            }
            if (se.clientLabel == state["harnessLabel"].asString()) harness = se.id;
        }
        CHECK_EQ(current, 1);
        CHECK(currentLabel.compare(0, 10, "Scacelith/") == 0);
        CHECK(harness != 0);
        ev = ask(c, Kind::SessionRevoked, [&] { c.revokeSession(harness); });
        report("revokeSession(the harness's)", ev);
        CHECK(ev.ok);
        CHECK_EQ(ev.sessionId, harness);
        CHECK(c.hasSavedSession());
        ev = ask(c, Kind::SessionsResult, [&] { c.fetchSessions(); });
        CHECK(ev.ok);
        for (const net::SessionInfo& se : ev.sessions) CHECK(se.id != harness);
        ev = ask(c, Kind::SessionRevoked, [&] { c.revokeSession(harness); });
        report("revokeSession(the same again)", ev);
        CHECK_EQ(ev.error, std::string("not_found"));
        CHECK(c.hasSavedSession());
    }
    {
        Scenario s("session revoked from another device: the next call is unauthorized with sessionLost, the token erased");
        ev = ask(c, Kind::SessionsResult, [&] { c.fetchSessions(); });
        int64_t mine = 0;
        for (const net::SessionInfo& se : ev.sessions)
            if (se.current) mine = se.id;
        json::Value r = ctl.call("POST", "/revoke-session?id=" + std::to_string(mine));
        std::fprintf(stderr, "  harness: DELETE /auth/sessions/%lld -> %d\n", (long long)mine, int(r["status"].asInt()));
        CHECK_EQ(r["status"].asInt(), int64_t(200));
        ev = ask(c, Kind::GamesResult, [&] { c.fetchMyGames(0, 5, net::GamesFilter()); });
        report("fetchMyGames after the revocation", ev);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("unauthorized"));
        CHECK(ev.sessionLost);
        CHECK(!c.hasSavedSession());
        CHECK_EQ(c.savedUsername(), user);
        ev = ask(c, Kind::AccountResult, [&] { c.fetchAccount(); });
        report("fetchAccount, signed out", ev);
        CHECK_EQ(ev.error, std::string("unauthorized"));
        CHECK(!ev.sessionLost);   // nothing sent
        ev = ask(c, Kind::GifResult, [&] { c.downloadGameGif(gWin->id, net::GifOptions()); });
        report("downloadGameGif, signed out", ev);
        CHECK_EQ(ev.error, std::string("unauthorized"));
        // The public reads still answer without a session.
        ev = ask(c, Kind::GameDetailsResult, [&] { c.fetchGame(gWin->id); });
        CHECK(ev.ok);
        CHECK_EQ(ev.gameDetails.you, 2);
        ev = ask(c, Kind::PgnResult, [&] { c.downloadPgn(gWin->id); });
        CHECK(ev.ok);
    }
    {
        Scenario s("revokeSession of this game's own session: signed out here, the realtime connection stopped");
        signIn();
        c.connect();
        std::vector<net::Event> seen;
        CHECK(waitFor(c, Kind::Welcome, ev, 15000, &seen));
        ev = ask(c, Kind::SessionsResult, [&] { c.fetchSessions(); });
        int64_t mine = 0;
        for (const net::SessionInfo& se : ev.sessions)
            if (se.current) mine = se.id;
        CHECK(mine != 0);
        seen.clear();
        ev = ask(c, Kind::SessionRevoked, [&] { c.revokeSession(mine); }, 20000, &seen);
        report("revokeSession(current)", ev);
        CHECK(ev.ok);
        CHECK_EQ(ev.sessionId, mine);
        CHECK(!c.hasSavedSession());
        net::Event st;
        bool offline = c.state() == net::ConnState::Offline ||
                       waitFor(c, Kind::ConnectionChanged, st, 5000, &seen, [](const net::Event& e) { return e.state == net::ConnState::Offline; });
        CHECK(offline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        bool reconnecting = false;
        while (c.poll(st))
            if (st.kind == Kind::ConnectionChanged && st.state != net::ConnState::Offline) reconnecting = true;
        CHECK(!reconnecting);
        CHECK(c.state() == net::ConnState::Offline);
        ev = ask(c, Kind::AccountResult, [&] { c.fetchAccount(); });
        report("fetchAccount after", ev);
        CHECK_EQ(ev.error, std::string("unauthorized"));
        CHECK(!ev.sessionLost);   // erased locally already: nothing sent
    }
    {
        Scenario s("expired session (GET /games/:id, optional auth): the public answer, sessionLost, the token erased");
        signIn();
        json::Value x = ctl.call("POST", "/expire-session");
        std::fprintf(stderr, "  harness: session %lld ('%s') expired in the database\n", (long long)x["sessionId"].asInt(),
                     x["clientLabel"].asString().c_str());
        CHECK(x["sessionId"].asInt() > 0);
        ev = ask(c, Kind::GameDetailsResult, [&] { c.fetchGame(gWin->id); });
        report("fetchGame with the expired token", ev);
        CHECK(ev.ok);
        CHECK(ev.error.empty());
        CHECK(ev.sessionLost);
        CHECK_EQ(ev.gameDetails.you, 2);
        CHECK(!ev.gameDetails.reportable);
        CHECK(!c.hasSavedSession());
    }
    {
        Scenario s("expired session (GET /account/me, required auth): 401 -> unauthorized, sessionLost, the token erased");
        signIn();
        json::Value x = ctl.call("POST", "/expire-session");
        CHECK(x["sessionId"].asInt() > 0);
        ev = ask(c, Kind::AccountResult, [&] { c.fetchAccount(); });
        report("fetchAccount with the expired token", ev);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("unauthorized"));
        CHECK(ev.sessionLost);
        CHECK(!c.hasSavedSession());
        signIn();
        json::Value y = ctl.call("POST", "/expire-session");
        CHECK(y["sessionId"].asInt() > 0);
        ev = ask(c, Kind::GifResult, [&] { c.downloadGameGif(gWin->id, net::GifOptions()); }, 90000);
        report("downloadGameGif with the expired token", ev);
        CHECK_EQ(ev.error, std::string("unauthorized"));
        CHECK(ev.sessionLost);
        CHECK(!c.hasSavedSession());
        // The PGN (optional session): answered without the token, the token erased.
        signIn();
        json::Value z = ctl.call("POST", "/expire-session");
        CHECK(z["sessionId"].asInt() > 0);
        ev = ask(c, Kind::PgnResult, [&] { c.downloadPgn(gWin->id); });
        report("downloadPgn with the expired token", ev);
        CHECK(ev.ok);
        CHECK(ev.sessionLost);
        CHECK(ev.text.find("[ScacelithGameId \"" + std::to_string(gWin->id) + "\"]") != std::string::npos);
        CHECK(!c.hasSavedSession());
        // The signed-in devices (required session).
        signIn();
        json::Value w = ctl.call("POST", "/expire-session");
        CHECK(w["sessionId"].asInt() > 0);
        ev = ask(c, Kind::SessionsResult, [&] { c.fetchSessions(); });
        report("fetchSessions with the expired token", ev);
        CHECK_EQ(ev.error, std::string("unauthorized"));
        CHECK(ev.sessionLost);
        CHECK(ev.sessions.empty());
        CHECK(!c.hasSavedSession());
    }

    // ---- two-factor re-authentication, then the deletion ----
    std::vector<std::string> recovery;
    std::string secret;
    {
        Scenario s("two-factor on: export without a code, with a recovery code; e-mail change with a recovery code");
        signIn();
        ev = ask(c, Kind::MfaSetupResult, [&] { c.mfaSetup(pass); });
        CHECK(ev.ok);
        secret = ev.mfaSecret;
        json::Value code = ctl.call("GET", "/totp?secret=" + queryEncode(secret));
        ev = ask(c, Kind::MfaEnableResult, [&] { c.mfaEnable(code["code"].asString()); });
        report("mfaEnable", ev);
        CHECK(ev.ok);
        recovery = ev.recoveryCodes;
        CHECK_EQ(recovery.size(), size_t(10));
        if (recovery.size() < 2) recovery.resize(2);
        ev = ask(c, Kind::AccountExportResult, [&] { c.exportAccount(pass, ""); }, 90000);
        report("export without a code", ev);
        CHECK_EQ(ev.error, std::string("mfa_code_required"));
        ev = ask(c, Kind::AccountExportResult, [&] { c.exportAccount(pass, recovery[0]); }, 90000);
        report("export with a recovery code", ev);
        CHECK(ev.ok);
        json::Value doc;
        CHECK(json::parse(ev.text, doc));
        CHECK(doc["account"]["mfaEnabled"].asBool());
        ev = ask(c, Kind::EmailChangeResult, [&] { c.changeEmail("cpp.third@example.org", pass, recovery[1]); });
        report("changeEmail with a recovery code", ev);
        CHECK(ev.ok);
        CHECK_EQ(ev.status, std::string("verification_sent"));
        ev = ask(c, Kind::EmailChangeResult, [&] { c.changeEmail("cpp.fourth@example.org", pass, recovery[1]); });
        report("changeEmail with the same recovery code again", ev);
        CHECK_EQ(ev.error, std::string("invalid_code"));
        ev = ask(c, Kind::AccountResult, [&] { c.fetchAccount(); });
        CHECK(ev.account.mfaEnabled);
        CHECK_EQ(ev.account.pendingEmail, std::string("cpp.third@example.org"));
    }
    {
        Scenario s("deleteAccount: wrong password, then with an authenticator code: signed out, realtime stopped, games anonymized");
        c.connect();
        CHECK(waitFor(c, Kind::Welcome, ev, 15000));
        std::vector<net::Event> seen;
        ev = ask(c, Kind::AccountDeleted, [&] { c.deleteAccount("wrong password 123", "000000"); }, 20000, &seen);
        report("deleteAccount(wrong password)", ev);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("invalid_password"));
        CHECK(c.hasSavedSession());
        // Closed for the request, open again once it was refused (its Welcome may come before or after the answer).
        net::Event welcome;
        const bool welcomed = std::any_of(seen.begin(), seen.end(), [](const net::Event& e) { return e.kind == Kind::Welcome; });
        CHECK(welcomed || waitFor(c, Kind::Welcome, welcome, 15000));
        CHECK(c.state() == net::ConnState::Online);
        json::Value code = ctl.call("GET", "/totp?secret=" + queryEncode(secret));
        seen.clear();
        ev = ask(c, Kind::AccountDeleted, [&] { c.deleteAccount(pass, code["code"].asString()); }, 20000, &seen);
        report("deleteAccount", ev);
        CHECK(ev.ok);
        CHECK(!c.hasSavedSession());
        CHECK(c.savedUsername().empty());
        net::Event st;
        bool offline = c.state() == net::ConnState::Offline ||
                       waitFor(c, Kind::ConnectionChanged, st, 5000, &seen, [](const net::Event& e) { return e.state == net::ConnState::Offline; });
        CHECK(offline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        bool reconnecting = false;
        while (c.poll(st))
            if (st.kind == Kind::ConnectionChanged && st.state != net::ConnState::Offline) reconnecting = true;
        CHECK(!reconnecting);
        ev = ask(c, Kind::LoginResult, [&] { c.login(user, pass); });
        report("login after the deletion", ev);
        CHECK(!ev.ok);
        CHECK(!c.hasSavedSession());
        ev = ask(c, Kind::GameDetailsResult, [&] { c.fetchGame(gWin->id); });
        CHECK(ev.ok);
        const std::string anon = "deleted#" + std::to_string(userId);
        std::fprintf(stderr, "  game %llu now: %s - %s\n", (unsigned long long)gWin->id, ev.gameDetails.white.name.c_str(),
                     ev.gameDetails.black.name.c_str());
        CHECK_EQ(ev.gameDetails.white.name, anon);
        ev = ask(c, Kind::PgnResult, [&] { c.downloadPgn(gWin->id); });
        CHECK(ev.ok);
        CHECK(ev.text.find("[White \"" + anon + "\"]") != std::string::npos);
    }
    std::remove(credPath);
}

// Other server settings: without e-mail confirmation (REQUIRE_EMAIL_VERIFICATION=false) the
// address changes at once (200 email_changed with the new address) and an address another account
// uses is refused (409 email_taken); with GIFs turned off (GIF_ENABLED=false) both GIF routes
// answer 404 gif_disabled. live-cpp-check.js runs it in its "game" part, on that part's server,
// after net_live_server_game registered the player and played its game:
//   SCACELITH_NET_LIVE_SETTINGS=host:port:<pin hex>:<username>:<password>:<an address in use>:<game id>
// The harness then finds the notice mailed to the former address in the server's log.
TEST(net_live_account_server_settings) {
    const char* env = std::getenv("SCACELITH_NET_LIVE_SETTINGS");
    if (!env) SKIP("SCACELITH_NET_LIVE_SETTINGS not set");
    REQUIRE(net::transportAvailable());  // asked for, so it must not pass without running
    std::vector<std::string> f;
    {
        std::string s = env, cur;
        for (char ch : s) {
            if (ch == ':') {
                f.push_back(cur);
                cur.clear();
            } else {
                cur += ch;
            }
        }
        f.push_back(cur);
    }
    CHECK_EQ(int(f.size()), 7);
    if (f.size() != 7) return;
    char credPath[256];
    std::snprintf(credPath, sizeof credPath, "scacelith-live-settings-%d.credentials", int(std::time(nullptr) % 100000));
    net::OnlineClient c;
    c.setCredentialsFile(credPath);
    net::ServerEndpoint ep;
    ep.host = f[0];
    ep.apiPort = uint16_t(std::atoi(f[1].c_str()));
    ep.pinnedSha256 = f[2];
    c.setServer(ep);
    const std::string user = f[3], pass = f[4], taken = f[5], next = user + ".changed@example.org";
    const uint64_t gameId = std::strtoull(f[6].c_str(), nullptr, 10);
    {
        Scenario s("changeEmail without e-mail confirmation: email_taken, then email_changed at once");
        net::Event ev = ask(c, Kind::LoginResult, [&] { c.login(user, pass); });
        report("login", ev);
        CHECK(ev.ok);
        const std::string before = ev.account.email;
        ev = ask(c, Kind::EmailChangeResult, [&] { c.changeEmail(taken, pass, ""); });
        report("an address in use", ev);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("email_taken"));
        ev = ask(c, Kind::EmailChangeResult, [&] { c.changeEmail(next, pass, ""); });
        report("change", ev);
        CHECK(ev.ok);
        CHECK_EQ(ev.status, std::string("email_changed"));
        CHECK_EQ(ev.account.email, next);
        ev = ask(c, Kind::AccountResult, [&] { c.fetchAccount(); });
        CHECK(ev.ok);
        std::fprintf(stderr, "  e-mail %s -> %s, pending '%s'\n", before.c_str(), ev.account.email.c_str(), ev.account.pendingEmail.c_str());
        CHECK_EQ(ev.account.email, next);
        CHECK(ev.account.pendingEmail.empty());
    }
    {
        Scenario s("GIFs turned off: gif_disabled for both routes, the session kept");
        net::GifOptions o;
        net::Event ev = ask(c, Kind::GifResult, [&] { c.downloadGameGif(gameId, o); }, 90000);
        report("downloadGameGif", ev);
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("gif_disabled"));
        CHECK_EQ(ev.gameId, gameId);
        ev = ask(c, Kind::GifResult, [&] { c.renderPgnGif("1. f3 e5 2. g4 Qh4# 0-1", o); }, 90000);
        report("renderPgnGif", ev);
        CHECK_EQ(ev.error, std::string("gif_disabled"));
        CHECK(!ev.sessionLost);
        CHECK(c.hasSavedSession());
        ev = ask(c, Kind::LogoutResult, [&] { c.logout(); });
        CHECK(ev.ok);
    }
    std::remove(credPath);
}

// ---- Google sign-in by loopback redirect (opt-in) -------------------------------------------------
// dedicated-server/tools/live-cpp-check.js (part "sso") serves the account API in its process with
// a fake Google (the provider's endpoints injected), e-mail confirmation on, and runs
//   SCACELITH_NET_LIVE_SSO=host:port:<control port> ./scacelith_tests net_live_sso
// over native TLS when it gives its certificate's SHA-256 in SCACELITH_NET_LIVE_SSO_PIN (the client
// pins it), else in plain HTTP on the loopback (a development client: insecureDev).
// The test is the game's browser: its opener keeps Google's URL; the control route
// GET /fake-authorize?url=<authUrl>&sub=<Google subject>&email=<address> answers what Google
// would (a 302 to the redirect URI with code, state and iss, its Location also as {"location"}),
// and the test GETs that address on the game's 127.0.0.1 listener. The other control routes:
// POST /seed-password-account {username, email, password, mfa}, GET /totp?username= (a code of an
// account seeded with mfa that the server has not seen used) and GET /links (the Google links
// stored: {"links": [{provider, subject, userId, username, email, createdAt}...]}).
namespace {

struct SsoBrowser {
    std::mutex mu;
    std::string url;
    std::string take() {
        std::lock_guard<std::mutex> lock(mu);
        std::string u = url;
        url.clear();
        return u;
    }
};

// Google's answer from the fake: the Location of its 302 (or of a JSON answer), "" when none.
std::string fakeAuthorize(const Control& ctl, const std::string& authUrl, const std::string& sub, const std::string& email) {
    net::HttpRequest req;
    req.host = "127.0.0.1";
    req.port = ctl.port;
    req.tls = false;
    req.path = "/fake-authorize?url=" + queryEncode(authUrl) + "&sub=" + queryEncode(sub) + "&email=" + queryEncode(email);
    req.timeoutMs = 20000;
    std::string location, body;
    net::HttpResponse resp;
    net::httpStream(req, [&](const net::HttpHead& h) {
        location = h.get("location");
        return location.empty();
    }, [&](const char* d, size_t n) {
        body.append(d, n);
        return body.size() < 65536;
    }, resp);
    if (location.empty()) {
        json::Value v;
        if (json::parse(body, v)) location = v["location"].asString();
    }
    if (location.empty()) std::fprintf(stderr, "  fake-authorize: %d %s %s\n", resp.status, resp.error.c_str(), body.substr(0, 200).c_str());
    return location;
}

// The browser following Google's redirect: a GET of "http://127.0.0.1:<port>/..." (on `port`
// instead when given: a link someone else got, opened against the game's listener).
int browserGet(const std::string& url, std::string* page = nullptr, uint16_t port = 0) {
    const std::string prefix = "http://127.0.0.1:";
    const size_t slash = url.find('/', prefix.size());
    if (url.compare(0, prefix.size(), prefix) != 0 || slash == std::string::npos) return -1;
    net::HttpRequest req;
    req.host = "127.0.0.1";
    req.port = port ? port : uint16_t(std::atoi(url.substr(prefix.size(), slash - prefix.size()).c_str()));
    req.tls = false;
    req.path = url.substr(slash);
    req.accept = "text/html";
    req.timeoutMs = 10000;
    net::HttpResponse resp;
    net::httpRequest(req, resp);
    if (page) *page = resp.body;
    return resp.status;
}

// The listener's port named by a URL: Google's (its redirect_uri, percent-encoded) or Google's
// redirect.
uint16_t listenerPort(const std::string& url) {
    for (const std::string key : {"127.0.0.1%3A", "127.0.0.1:"}) {
        const size_t at = url.find(key);
        if (at != std::string::npos) return uint16_t(std::atoi(url.c_str() + at + key.size()));
    }
    return 0;
}

// One Google sign-in of `c` up to the server's answer: the start, the fake Google for (sub,
// email), the browser's GET on the listener, then a LoginResult, SsoNeedsUsername or
// SsoNeedsPassword.
net::Event googleSignIn(net::OnlineClient& c, SsoBrowser& b, const Control& ctl, const std::string& sub, const std::string& email) {
    net::Event ev = ask(c, Kind::SsoBrowserOpened, [&] { c.startGoogleSso(net::SsoBrowserPage()); });
    report("start", ev);
    CHECK(ev.ok);
    if (!ev.ok) return ev;
    const std::string location = fakeAuthorize(ctl, b.take(), sub, email);
    CHECK(!location.empty());
    std::string page;
    const int status = browserGet(location, &page);
    CHECK_EQ(status, 200);
    CHECK(page.find("<html") != std::string::npos);
    net::Event code;
    CHECK(waitFor(c, Kind::SsoCodeReceived, code));
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < end) {
        while (c.poll(ev))
            if (ev.kind == Kind::LoginResult || ev.kind == Kind::SsoNeedsUsername || ev.kind == Kind::SsoNeedsPassword) return ev;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ev = net::Event();
    ev.error = "no_event";
    return ev;
}

// The Google links stored for a Google subject.
int linksOf(const Control& ctl, const std::string& sub) {
    const json::Value v = ctl.call("GET", "/links");
    int n = 0;
    for (const json::Value& r : v["links"].items()) n += r["subject"].asString() == sub;
    return n;
}

json::Value seedPasswordAccount(const Control& ctl, const std::string& username, const std::string& email,
                                const std::string& password, bool mfa) {
    json::Value b = json::Value::object();
    b.set("username", username);
    b.set("email", email);
    b.set("password", password);
    b.set("mfa", mfa);
    int status = 0;
    json::Value v = ctl.call("POST", "/seed-password-account", &status, b.dump());
    CHECK(status >= 200 && status < 300);
    return v;
}

}  // namespace

TEST(net_live_sso) {
    const char* env = std::getenv("SCACELITH_NET_LIVE_SSO");
    if (!env) SKIP("SCACELITH_NET_LIVE_SSO not set");
    REQUIRE(net::transportAvailable());  // asked for, so it must not pass without running
    std::vector<std::string> f;
    {
        std::string s = env, cur;
        for (char ch : s) {
            if (ch == ':') {
                f.push_back(cur);
                cur.clear();
            } else {
                cur += ch;
            }
        }
        f.push_back(cur);
    }
    CHECK_EQ(int(f.size()), 3);
    if (f.size() != 3) return;
    Control ctl;
    ctl.port = uint16_t(std::atoi(f[2].c_str()));
    const std::string run = std::to_string(int(std::time(nullptr) % 100000));
    char credPath[256];
    std::snprintf(credPath, sizeof credPath, "scacelith-live-sso-%s.credentials", run.c_str());
    net::ServerEndpoint ep;
    ep.host = f[0];
    ep.apiPort = uint16_t(std::atoi(f[1].c_str()));
    ep.wsPort = 0;
    const char* pin = std::getenv("SCACELITH_NET_LIVE_SSO_PIN");
    if (pin && *pin) ep.pinnedSha256 = pin;
    else ep.insecureDev = true;   // plain HTTP on the loopback
    SsoBrowser browser;
    auto opener = [&browser](const std::string& url) {
        std::lock_guard<std::mutex> lock(browser.mu);
        browser.url = url;
        return true;
    };
    net::OnlineClient c;
    c.setCredentialsFile(credPath);
    c.setServer(ep);
    c.setBrowserOpener(opener);
    net::Event ev = ask(c, Kind::ServerInfoResult, [&] { c.fetchServerInfo(); });
    CHECK(ev.ok);
    auto logout = [&] {
        net::Event l = ask(c, Kind::LogoutResult, [&] { c.logout(); });
        CHECK(l.ok);
    };

    const std::string newSub = "live-sso-new-" + run, newEmail = "sso.new." + run + "@example.org", newName = "SsoNew_" + run;
    {
        Scenario s("a new Google account: SsoNeedsUsername, then completeSso");
        ev = googleSignIn(c, browser, ctl, newSub, newEmail);
        CHECK(ev.kind == Kind::SsoNeedsUsername);
        std::fprintf(stderr, "  suggested '%s'\n", ev.account.username.c_str());
        ev = ask(c, Kind::LoginResult, [&] { c.completeSso(newName); });
        report("complete", ev);
        CHECK(ev.ok);
        CHECK_EQ(ev.account.username, newName);
        CHECK(ev.account.googleLinked);
        CHECK(!ev.account.hasPassword);
        CHECK_EQ(linksOf(ctl, newSub), 1);
        logout();
    }
    {
        Scenario s("a login by Google subject");
        ev = googleSignIn(c, browser, ctl, newSub, newEmail);
        report("login", ev);
        CHECK(ev.kind == Kind::LoginResult && ev.ok);
        CHECK_EQ(ev.account.username, newName);
        logout();
    }
    {
        Scenario s("link to a password account: a wrong password, the right one, then no password");
        const std::string sub = "live-sso-pw-" + run, email = "sso.pw." + run + "@example.org", user = "SsoPw_" + run;
        const std::string pass = "Correct horse " + run;
        seedPasswordAccount(ctl, user, email, pass, false);
        ev = googleSignIn(c, browser, ctl, sub, email);
        CHECK(ev.kind == Kind::SsoNeedsPassword);
        CHECK_EQ(ev.account.username, user);
        ev = ask(c, Kind::LoginResult, [&] { c.linkSso("not the password"); }, 60000);
        report("wrong password", ev);
        CHECK_EQ(ev.error, std::string("invalid_credentials"));
        CHECK_EQ(linksOf(ctl, sub), 0);
        ev = ask(c, Kind::LoginResult, [&] { c.linkSso(pass); }, 60000);
        report("password", ev);
        CHECK(ev.ok);
        CHECK_EQ(ev.account.username, user);
        CHECK_EQ(linksOf(ctl, sub), 1);
        logout();
        ev = googleSignIn(c, browser, ctl, sub, email);
        report("next sign-in", ev);
        CHECK(ev.kind == Kind::LoginResult && ev.ok);
        CHECK_EQ(ev.account.username, user);
        logout();
    }
    {
        Scenario s("link with two-factor: a wrong code links nothing, the right one links");
        const std::string sub = "live-sso-mfa-" + run, email = "sso.mfa." + run + "@example.org", user = "SsoMfa_" + run;
        const std::string pass = "Battery staple " + run;
        seedPasswordAccount(ctl, user, email, pass, true);
        ev = googleSignIn(c, browser, ctl, sub, email);
        CHECK(ev.kind == Kind::SsoNeedsPassword);
        ev = ask(c, Kind::LoginResult, [&] { c.linkSso(pass); }, 60000);
        report("password", ev);
        CHECK(!ev.ok && ev.mfaRequired);
        const std::string code = ctl.call("GET", "/totp?username=" + queryEncode(user))["code"].asString();
        CHECK(!code.empty());
        const std::string wrong = code == "000000" ? "000001" : "000000";
        ev = ask(c, Kind::LoginResult, [&] { c.loginMfa(wrong); });
        report("wrong code", ev);
        CHECK_EQ(ev.error, std::string("invalid_code"));
        CHECK_EQ(linksOf(ctl, sub), 0);
        ev = ask(c, Kind::LoginResult, [&] { c.loginMfa(code); });
        report("code", ev);
        CHECK(ev.ok);
        CHECK_EQ(ev.account.username, user);
        CHECK_EQ(linksOf(ctl, sub), 1);
        logout();
    }
    {
        Scenario s("another Google account with the address of a password-less account: sso_account_exists");
        ev = googleSignIn(c, browser, ctl, "live-sso-other-" + run, newEmail);
        report("sign-in", ev);
        CHECK(ev.kind == Kind::LoginResult && !ev.ok);
        CHECK_EQ(ev.error, std::string("sso_account_exists"));
    }
    {
        Scenario s("a stranger's Google link opened against the game's listener: the foreign page, then the game's own");
        SsoBrowser strangerBrowser;
        char strangerCred[256];
        std::snprintf(strangerCred, sizeof strangerCred, "scacelith-live-sso-stranger-%s.credentials", run.c_str());
        net::OnlineClient stranger;
        stranger.setCredentialsFile(strangerCred);
        stranger.setServer(ep);
        stranger.setBrowserOpener([&strangerBrowser](const std::string& url) {
            std::lock_guard<std::mutex> lock(strangerBrowser.mu);
            strangerBrowser.url = url;
            return true;
        });
        ev = ask(c, Kind::SsoBrowserOpened, [&] { c.startGoogleSso(net::SsoBrowserPage()); });
        CHECK(ev.ok);
        const std::string mine = browser.take();
        ev = ask(stranger, Kind::SsoBrowserOpened, [&] { stranger.startGoogleSso(net::SsoBrowserPage()); });
        CHECK(ev.ok);
        const std::string theirs = fakeAuthorize(ctl, strangerBrowser.take(), newSub, newEmail);
        CHECK(!theirs.empty());
        const uint16_t port = listenerPort(mine);
        CHECK(port != 0 && port != listenerPort(theirs));
        std::string page;
        CHECK_EQ(browserGet(theirs, &page, port), 400);
        CHECK(page.find("<html") != std::string::npos);
        const std::string location = fakeAuthorize(ctl, mine, newSub, newEmail);
        CHECK_EQ(browserGet(location, &page), 200);
        CHECK(waitFor(c, Kind::LoginResult, ev));
        report("own sign-in", ev);
        CHECK(ev.ok);
        CHECK_EQ(ev.account.username, newName);
        stranger.cancelSso();
        logout();
        std::remove(strangerCred);
    }
    std::remove(credPath);
}
