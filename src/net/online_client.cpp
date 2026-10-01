// OnlineClient: two network threads behind a command/event interface.
//
//   net-http  HTTPS API calls (account, login, SSO polling, proof of work), one at a time.
//   net-rt    the realtime WebSocket: connection, Hello/Welcome, heartbeats, reconnection with
//             backoff, decoding of the server's messages into Events and the OnlineGame copy.
//
// Pacing, chosen for a server that may hold thousands of idle players on a small machine:
//   - The client's own Ping (ping indicator, server clock offset) follows Welcome.clientPingMs
//     (the server's CLIENT_PING_INTERVAL_MS, 10 s by default): one at once after Welcome and
//     kPingBurst more about a second apart, so both values are right quickly, then one per
//     interval. Liveness does not wait for it: the server's own Ping comes about every
//     Welcome.heartbeatMs (a quarter second later at worst), and when nothing at all came for 1.5
//     heartbeats (7.5 s at least) the client sends one Ping at once as a probe; the connection is
//     dead after two heartbeats (10 s at least) with nothing received.
//   - Automatic reconnections follow reconnectDelayMs() (online_client.h): full jitter, a long
//     wait when the server is full, a spread first attempt after a shutdown, and 8 s at most
//     between attempts while a game is in progress (unless the server gave a Retry-After): the
//     server's reconnection grace is short, at least RECONNECT_GRACE_MIN_MS (15 s by default),
//     and RECOVERY_GRACE_MS (90 s by default) for the games it restores after a restart. For 10
//     minutes after losing a connection that had reached Welcome they reuse the /api/v1/info
//     answer it was made with (one TLS handshake instead of two), after a shutdown as after a
//     crash, so the reconnection wave of a restart costs one handshake per player; the server id
//     of the 101 answer is checked against the saved session before Hello (a reinstall), and a
//     refused upgrade (404, 426: another path or subprotocol) makes the next attempt read /info
//     again. A connect() asked by the player always reads /info again and never waits for the
//     backoff.
//   - Gestures (sendGesture, net/gesture.h) follow Welcome.gestureRate / gestureBurst, the
//     server's relay bucket: only the latest one waits, and it goes when a token of a bucket one
//     message smaller than the server's allows (none when the rate is 0): the server then drops
//     none unless a stall of the link delivers more than its burst at once (gestureSendCapacity).
//     A Gesture made while the connection is down, or for another game than the one of the last
//     GameSnapshot, is dropped: the next one carries the whole state again.
//
// Keeping the realtime connection on its own thread means a slow HTTPS call (or a proof of
// work) never delays the answer to a server Ping or the sending of a move. The game thread only
// pushes commands (lambdas) and drains Events with poll(); the credential store has its own
// lock. Tokens are read from the store for the origin a command was issued for, and every
// request of a command goes to that origin only.
#include "online_client.h"
#include "credential_store.h"
#include "crypto.h"
#include "json.h"
#include "net_sys.h"
#include "protocol_gen.h"
#include "transport.h"
#include "../core/log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <thread>

#ifndef SCACELITH_VERSION_STRING
#define SCACELITH_VERSION_STRING "0.1.0"
#endif
#ifndef SCACELITH_OFFICIAL_SERVER
#define SCACELITH_OFFICIAL_SERVER ""
#endif
// The official server when the build does not name another one: HTTPS API (/api/v1) and the
// WebSocket (/ws) share port 44664.
#define SCACELITH_DEFAULT_OFFICIAL_SERVER "caissa.scacelith.com:44664"

namespace net {

using Clock = std::chrono::steady_clock;
namespace pr = net::proto;

// ---------------------------------------------------------------------------------------------
// Endpoint helpers
// ---------------------------------------------------------------------------------------------

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// "AB:CD:..." or "abcd..." -> 64 lower-case hex digits, "" when not a SHA-256.
std::string normalizePin(const std::string& pin) {
    std::string out;
    for (char c : pin) {
        if (c == ':' || c == ' ') continue;
        if (!std::isxdigit((unsigned char)c)) return std::string();
        out += char(std::tolower((unsigned char)c));
    }
    return out.size() == 64 ? out : std::string();
}

bool validHostName(const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    if (isIpLiteral(h)) return true;
    size_t label = 0;
    for (size_t i = 0; i < h.size(); ++i) {
        char c = h[i];
        if (c == '.') {
            if (label == 0 || h[i - 1] == '-') return false;
            label = 0;
            continue;
        }
        if (!(std::isalnum((unsigned char)c) || c == '-')) return false;
        if (label == 0 && c == '-') return false;
        if (++label > 63) return false;
    }
    return label > 0 && h.back() != '-';
}

std::string clientString() {
#ifdef _WIN32
    return std::string("Scacelith/") + SCACELITH_VERSION_STRING + " win64";
#else
    return std::string("Scacelith/") + SCACELITH_VERSION_STRING + " linux";
#endif
}

// Epoch milliseconds from a monotonic clock anchored once (immune to wall-clock jumps).
double localEpochMs() {
    static const auto steady0 = Clock::now();
    static const double wall0 =
        std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
    return wall0 + std::chrono::duration<double, std::milli>(Clock::now() - steady0).count();
}

bool plausibleToken(const std::string& t) {
    if (t.size() < 16 || t.size() > 160) return false;
    for (char c : t)
        if (c <= ' ' || c > '~') return false;
    return true;
}

constexpr int kPingBurst = 3;                   // quick pings after the one sent at Welcome
constexpr int kPingBurstGapMs = 1100;           // the server answers one Ping per 950 ms at most
constexpr size_t kOffsetSamples = 8;            // clock offset: lowest round trip of the last 8
constexpr auto kOffsetMaxAge = std::chrono::minutes(5);   // ...taken in the last 5 minutes
constexpr auto kInfoReuse = std::chrono::minutes(10);     // /info answer reused on reconnection
constexpr uint16_t kCloseServerFull = 4006;     // 4000 + ErrorCode::ServerFull (no CloseCode entry)

// Milliseconds of the monotonic clock (the Gesture bucket's time).
double steadyMs() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }

// Random numbers for the reconnection jitter, seeded from the OS generator so that clients never
// share a sequence (they would come back together).
double jitterUniform() {
    static thread_local std::mt19937_64 rng = [] {
        uint64_t seed[2] = {0, 0};
        if (!crypto::randomBytes(seed, sizeof seed)) {
            seed[0] = uint64_t(std::random_device{}()) << 32 ^ std::random_device{}();
            seed[1] = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
        }
        std::seed_seq seq{uint32_t(seed[0]), uint32_t(seed[0] >> 32), uint32_t(seed[1]), uint32_t(seed[1] >> 32)};
        return std::mt19937_64(seq);
    }();
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng);
}

}  // namespace

std::string ServerEndpoint::origin() const {
    std::string h = lower(host);
    if (h.find(':') != std::string::npos) h = "[" + h + "]";
    return h + ":" + std::to_string(apiPort);
}

bool ServerEndpoint::valid() const {
    if (!validHostName(host) || apiPort == 0) return false;
    if (insecureDev && !isLoopbackHost(host)) return false;
    if (!pinnedSha256.empty() && normalizePin(pinnedSha256).empty()) return false;
    return true;
}

ServerEndpoint officialServer() {
    ServerEndpoint ep;
    ep.host.clear();
    std::string s = SCACELITH_OFFICIAL_SERVER;
    if (s.empty()) s = SCACELITH_DEFAULT_OFFICIAL_SERVER;
    if (s == "none") return ep;
    std::string rest;
    if (s[0] == '[') {                          // "[v6]:api:ws"
        size_t e = s.find(']');
        if (e == std::string::npos) return ep;
        ep.host = s.substr(1, e - 1);
        rest = s.substr(e + 1);
    } else {
        size_t c = s.find(':');
        ep.host = s.substr(0, c);
        rest = c == std::string::npos ? std::string() : s.substr(c);
    }
    ep.apiPort = 443;
    ep.wsPort = 0;
    if (!rest.empty() && rest[0] == ':') {
        rest.erase(0, 1);
        size_t c = rest.find(':');
        ep.apiPort = uint16_t(std::atoi(rest.substr(0, c).c_str()));
        if (c != std::string::npos) ep.wsPort = uint16_t(std::atoi(rest.substr(c + 1).c_str()));
    }
    ep.wsPort = ep.effectiveWsPort();           // one port for both unless given
    ep.host = lower(ep.host);
    ep.pinnedSha256.clear();
    ep.insecureDev = false;
    if (!ep.valid()) {
        LOGW("net: invalid official server '%s' in this build", s.c_str());
        return ServerEndpoint();
    }
    return ep;
}

uint32_t positionDigest(const std::string& fen) {
    // First four whitespace-separated fields joined by single spaces.
    uint32_t h = 0x811c9dc5u;
    int field = 0;
    size_t i = 0, n = fen.size();
    while (i < n && std::isspace((unsigned char)fen[i])) ++i;
    while (i < n && field < 4) {
        if (field > 0) { h ^= uint8_t(' '); h *= 0x01000193u; }
        while (i < n && !std::isspace((unsigned char)fen[i])) {
            h ^= uint8_t(fen[i++]);
            h *= 0x01000193u;
        }
        ++field;
        while (i < n && std::isspace((unsigned char)fen[i])) ++i;
    }
    return h;
}

uint32_t reconnectDelayMs(int attempt, RetryCause cause, double u, bool gameInProgress, uint32_t retryAfterMs) {
    if (!(u >= 0.0)) u = 0.0;                   // NaN too
    if (u > 1.0) u = 1.0;
    attempt = std::max(attempt, 0);
    auto uniform = [u](double lo, double hi) { return lo + u * (hi - lo); };
    // A game in progress must not be lost on the reconnection grace: 8 s at most between attempts.
    const double cap = gameInProgress ? 8000.0 : 30000.0;
    double ms;
    if (cause == RetryCause::Shutdown && attempt == 0) {
        ms = gameInProgress ? uniform(1000.0, 8000.0) : uniform(5000.0, 35000.0);
    } else if (cause == RetryCause::ServerFull && !gameInProgress) {
        ms = uniform(60000.0, 120000.0);
    } else {
        ms = uniform(500.0, std::min(cap, 2000.0 * std::pow(2.0, std::min(attempt, 16))));
    }
    if (retryAfterMs > 0) {
        double ra = std::min(double(retryAfterMs), 600000.0);
        ms = std::max(ms, std::min(ra * (1.0 + 0.5 * u), 600000.0));
    }
    return uint32_t(ms);
}

uint32_t clientPingIntervalMs(uint32_t announced) {
    return announced == 0 ? 10000u : std::clamp<uint32_t>(announced, 1000u, 60000u);
}

// ---------------------------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------------------------

struct OnlineClient::Impl {
    // ---- game thread ----
    ServerEndpoint ep;
    OnlineGame view;
    bool hasView = false;

    // ---- shared ----
    std::mutex mu;
    std::condition_variable httpCv, rtCv;
    std::deque<std::function<void()>> httpQ, rtQ;
    std::deque<Event> events;
    bool stopping = false, rtWake = false;
    // The latest Gesture of the game thread, until net-rt sends or drops it (flushGesture).
    struct GestureOut { bool pending = false; uint64_t game = 0; Gesture g; } gestureOut;
    std::atomic<bool> stopFlag{false};
    std::atomic<int> connState{int(ConnState::Offline)};
    std::atomic<int> ping{-1};
    std::atomic<double> clockOffset{0.0};
    std::atomic<uint32_t> connectGen{0};
    CredentialStore creds;
    CancelToken httpCancel, rtCancel;
    std::thread httpThread, rtThread;

    // ---- net-http state ----
    std::string mfaToken, mfaOrigin;
    struct Sso {
        bool active = false;
        ServerEndpoint ep;
        std::string attemptId, verifier;
        Clock::time_point nextPoll, expires;
        int pollMs = 2000;
    } sso;
    std::string ssoTicket, ssoTicketOrigin;

    // ---- net-rt state ----
    struct Rt {
        bool wanted = false;
        ServerEndpoint ep;
        int attempt = 0;
        Clock::time_point nextAttempt{}, connectedAt{}, lastRecv{}, nextPing{};
        Clock::time_point probedAt{};                     // lastRecv when the silence probe went
        std::unique_ptr<WebSocket> ws;
        bool welcomed = false;
        uint32_t seq = 0;
        uint32_t heartbeatMs = 0;
        uint32_t pingEveryMs = 10000;                     // clientPingIntervalMs(Welcome.clientPingMs)
        int pingBurst = 0;                                // quick pings still to send after Welcome
        uint32_t pingNonce = 0;
        struct Sent { uint32_t nonce = 0; Clock::time_point at{}; } sent[8];
        struct Sample { double rtt = 0, offset = 0; Clock::time_point at{}; };
        std::deque<Sample> samples;
        double rttEma = -1;
        Clock::time_point lastPong{};
        bool haveOffset = false;
        int lastFatal = 0;                                // ErrorCode of the last fatal Error
        bool shutdownNotice = false;                      // Notice{ServerShutdown} on this connection
        bool restarting = false;                          // lost to a shutdown, no Welcome or 503 since
        // The /api/v1/info answer the last connection attempt used. proven: a connection built on
        // it reached Welcome; at: when it was read, or when such a connection last ended.
        struct Info {
            bool valid = false, proven = false, insecure = false;
            std::string origin, wsPath;
            Clock::time_point at{};
        } info;
        double banUntil = 0;
        OnlineGame game;
        uint32_t lastGseq = 0;
        struct Pending { uint64_t game = 0; int ply = -1; uint16_t move = 0; } pending;
        GestureBucket gestures;                           // Welcome.gestureRate / gestureBurst
    } rt;

    Impl() {
        httpThread = std::thread([this] { httpLoop(); });
        rtThread = std::thread([this] { rtLoop(); });
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(mu);
            stopping = true;
        }
        stopFlag.store(true);
        httpCancel.cancel();
        rtCancel.cancel();
        httpCv.notify_all();
        rtCv.notify_all();
        if (httpThread.joinable()) httpThread.join();
        if (rtThread.joinable()) rtThread.join();
    }

    void post(Event ev) {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(std::move(ev));
    }
    // An OpponentGesture replaces the one of the same game still waiting to be polled (only the
    // latest state matters), so the queue holds at most one per game.
    void postGesture(Event ev) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto it = events.begin(); it != events.end(); ++it) {
            if (it->kind == Event::Kind::OpponentGesture && it->gameId == ev.gameId) {
                events.erase(it);
                break;
            }
        }
        events.push_back(std::move(ev));
    }
    void http(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(mu);
            httpQ.push_back(std::move(fn));
        }
        httpCv.notify_one();
    }
    void realtime(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(mu);
            rtQ.push_back(std::move(fn));
        }
        rtCv.notify_one();
    }
    void wakeRt() {
        {
            std::lock_guard<std::mutex> lk(mu);
            rtWake = true;
        }
        rtCv.notify_one();
    }

    // The pin in effect for an endpoint: the one given, else the one saved for its origin.
    std::string effectivePin(const ServerEndpoint& e) {
        std::string p = normalizePin(e.pinnedSha256);
        if (!p.empty()) return p;
        Credential c;
        return creds.get(e.origin(), c) ? c.pinnedSha256 : std::string();
    }

    // =========================================================================================
    // HTTPS API (net-http thread; connect() also uses it from net-rt for /info)
    // =========================================================================================

    struct Api {
        int status = 0;
        json::Value body;
        std::string error;       // "" on 2xx
        int retryAfter = 0;
        bool ok() const { return error.empty(); }
    };

    Api api(const ServerEndpoint& e, const std::string& method, const std::string& path, const json::Value* body,
            bool auth, CancelToken& cancel) {
        Api out;
        if (!e.valid()) { out.error = "invalid_server"; return out; }
        HttpRequest req;
        req.method = method;
        req.host = e.host;
        req.port = e.apiPort;
        req.tls = !e.insecureDev;
        req.pinnedSha256 = req.tls ? effectivePin(e) : std::string();
        req.path = "/api/v1" + path;
        if (auth) {
            Credential c;
            if (!creds.get(e.origin(), c) || c.token.empty()) { out.error = "not_logged_in"; return out; }
            req.headers.emplace_back("Authorization", "Bearer " + c.token);
        }
        json::Value payload = body ? *body : json::Value();
        for (int round = 0; round < 2; ++round) {
            req.body = body ? payload.dump() : std::string();
            HttpResponse resp;
            httpRequest(req, resp, &cancel);
            out = Api();
            if (!resp.error.empty()) {
                out.error = resp.error;
                if (!resp.detail.empty()) LOGW("net: %s %s: %s (%s)", method.c_str(), path.c_str(), resp.error.c_str(), resp.detail.c_str());
                return out;
            }
            out.status = resp.status;
            if (!resp.body.empty()) {
                std::string err;
                if (!json::parse(resp.body, out.body, &err) && resp.status >= 200 && resp.status < 300) {
                    LOGW("net: %s %s: invalid JSON (%s)", method.c_str(), path.c_str(), err.c_str());
                    out.error = "bad_response";
                    return out;
                }
            }
            out.retryAfter = int(out.body["retryAfter"].asInt(std::atoi(resp.retryAfter.c_str())));
            if (resp.status >= 200 && resp.status < 300) return out;
            out.error = out.body["error"].asString("http_" + std::to_string(resp.status));
            // Proof of work (DESIGN 8): solve and repeat the same request once.
            if (resp.status == 428 && round == 0 && body && payload.isObject() && out.body["pow"].isObject()) {
                std::string challenge = out.body["pow"]["challenge"].asString();
                int bits = int(out.body["pow"]["bits"].asInt(-1));
                if (challenge.empty() || challenge.size() > 512 || bits < 0) return out;
                if (bits > crypto::kPowMaxBits) { out.error = "pow_too_hard"; return out; }
                std::string nonce;
                crypto::PowStats st;
                std::atomic<bool>* stop = &stopFlag;
                if (!crypto::powSolve(challenge, bits, nonce, stop, &st)) {
                    out.error = stopFlag.load() ? "cancelled" : "pow_failed";
                    return out;
                }
                LOGI("net: proof of work %d bits: %llu hashes in %.2f s", bits, (unsigned long long)st.hashes, st.seconds);
                json::Value pow = json::Value::object();
                pow.set("challenge", challenge);
                pow.set("nonce", nonce);
                payload.set("pow", pow);
                continue;
            }
            return out;
        }
        return out;
    }

    static void fillError(Event& ev, const Api& a) {
        ev.ok = a.ok();
        ev.error = a.error;
        ev.retryAfterSec = a.retryAfter;
    }

    static RatingInfo parseRating(const json::Value& r) {
        RatingInfo ri;
        ri.category = r["category"].asString(r["id"].asString());
        ri.rating = int(r["rating"].asInt(1500));
        ri.games = int(r["games"].asInt(0));
        ri.wins = int(r["wins"].asInt(0));
        ri.draws = int(r["draws"].asInt(0));
        ri.losses = int(r["losses"].asInt(0));
        ri.peak = int(r["peak"].asInt(ri.rating));
        ri.provisional = r["provisional"].asBool(ri.games < 20);
        return ri;
    }

    // /account/me ({ user, ratings, sanctions, ban }) or a login answer's user object.
    static AccountInfo parseAccount(const json::Value& v) {
        const json::Value& u = v["user"].isObject() ? v["user"] : v;
        AccountInfo a;
        a.userId = uint32_t(u["id"].asInt(u["userId"].asInt(0)));
        a.username = u["username"].asString();
        a.email = u["email"].asString();
        a.emailVerified = u["emailVerified"].asBool();
        a.mfaEnabled = u["mfaEnabled"].asBool();
        a.googleLinked = u["googleLinked"].asBool();
        const json::Value& ratings = v["ratings"].isArray() ? v["ratings"] : u["ratings"];
        for (const json::Value& r : ratings.items()) a.ratings.push_back(parseRating(r));
        if (v["ban"].isObject()) a.bannedUntilMs = v["ban"]["until"].isNumber() ? v["ban"]["until"].asInt() : INT64_MAX;
        return a;
    }

    static ServerInfo parseInfo(const json::Value& v) {
        ServerInfo i;
        i.name = v["name"].asString();
        i.serverId = v["serverId"].asString();
        i.motd = v["motd"].asString();
        const json::Value& p = v["protocol"];
        i.protocolMin = int(p["min"].asInt(0));
        i.protocolMax = int(p["max"].asInt(0));
        if (p["schema"].isNumber()) {
            i.schemaHash = uint32_t(p["schema"].asInt(0));
        } else {
            std::string s = p["schema"].asString();
            if (s.compare(0, 2, "0x") == 0) s.erase(0, 2);
            i.schemaHash = uint32_t(std::strtoul(s.c_str(), nullptr, 16));
        }
        i.compatible = int(pr::kProtocolVersion) >= i.protocolMin && int(pr::kProtocolVersion) <= i.protocolMax &&
                       i.schemaHash == pr::kSchemaHash && p["subprotocol"].asString(pr::kWsSubprotocol) == pr::kWsSubprotocol;
        int64_t ws = v["wsPort"].asInt(0);
        i.wsPort = ws > 0 && ws < 65536 ? uint16_t(ws) : 0;
        const json::Value& reg = v["registration"];
        i.registrationOpen = reg.isBool() ? reg.asBool() : reg.asString() == "open";
        i.emailVerification = v["emailVerification"].asBool();
        i.googleSso = v["sso"]["google"].asBool();
        i.powRegisterBits = int(v["pow"]["register"].asInt(0));
        for (const json::Value& c : v["categories"].items()) {
            Category cat;
            cat.id = c["id"].asString();
            cat.baseSec = int(c["baseSec"].asInt(0));
            cat.incSec = int(c["incSec"].asInt(0));
            if (!cat.id.empty()) i.categories.push_back(cat);
        }
        return i;
    }

    // Fetches /info and applies the per-origin identity rule: a saved session whose server id
    // differs from the one announced now belongs to another server: it is dropped, never sent.
    Api fetchInfo(const ServerEndpoint& e, ServerInfo& info, CancelToken& cancel) {
        Api a = api(e, "GET", "/info", nullptr, false, cancel);
        if (!a.ok()) return a;
        info = parseInfo(a.body);
        Credential c;
        if (!info.serverId.empty() && creds.get(e.origin(), c) && !c.token.empty() && !c.serverId.empty() && c.serverId != info.serverId) {
            LOGW("net: %s announces another server id; its saved session is discarded", e.origin().c_str());
            creds.clearToken(e.origin());
        }
        return a;
    }

    // A login answer: { token, expiresAt, user } or { mfaRequired, mfaToken }.
    void finishLogin(const ServerEndpoint& e, const Api& a, Event& ev) {
        fillError(ev, a);
        if (!a.ok()) {
            if (a.body["until"].isNumber()) ev.account.bannedUntilMs = a.body["until"].asInt();
            return;
        }
        if (a.body["mfaRequired"].asBool()) {
            mfaToken = a.body["mfaToken"].asString();
            mfaOrigin = e.origin();
            ev.ok = false;
            ev.mfaRequired = true;
            ev.error = "mfa_required";
            return;
        }
        std::string token = a.body["token"].asString();
        if (!plausibleToken(token)) {
            ev.ok = false;
            ev.error = "bad_response";
            return;
        }
        ev.account = parseAccount(a.body);
        Credential c;
        creds.get(e.origin(), c);   // keeps the saved server id / pin when present
        c.origin = e.origin();
        c.username = ev.account.username.empty() ? c.username : ev.account.username;
        c.token = token;
        std::string pin = normalizePin(e.pinnedSha256);
        if (!pin.empty()) c.pinnedSha256 = pin;
        ServerInfo info;
        if (fetchInfo(e, info, httpCancel).ok() && !info.serverId.empty()) c.serverId = info.serverId;
        if (!creds.put(c)) LOGW("net: the session could not be saved (%s)", creds.path().c_str());
        mfaToken.clear();
        ev.ok = true;
        ev.error.clear();
    }

    void ssoFinished() {
        sso = Sso();
    }

    void ssoPollOnce() {
        Event ev;
        ev.kind = Event::Kind::LoginResult;
        if (Clock::now() >= sso.expires) {
            ev.error = "sso_expired";
            ssoFinished();
            post(ev);
            return;
        }
        json::Value b = json::Value::object();
        b.set("attemptId", sso.attemptId);
        b.set("codeVerifier", sso.verifier);
        b.set("clientLabel", clientString());
        ServerEndpoint e = sso.ep;
        Api a = api(e, "POST", "/auth/sso/google/poll", &b, false, httpCancel);
        if (!sso.active) return;
        if (a.ok() && a.body["status"].asString() == "pending") {
            sso.nextPoll = Clock::now() + std::chrono::milliseconds(sso.pollMs);
            return;
        }
        if (!a.ok() && a.status == 0 && a.error != "cancelled") {
            // Transport trouble: keep polling until the attempt expires.
            sso.nextPoll = Clock::now() + std::chrono::milliseconds(sso.pollMs * 2);
            return;
        }
        ssoFinished();
        if (a.ok() && a.body["needsUsername"].asBool()) {
            ssoTicket = a.body["ssoTicket"].asString();
            ssoTicketOrigin = e.origin();
            Event n;
            n.kind = Event::Kind::SsoNeedsUsername;
            n.ok = true;
            n.account.username = a.body["suggestedUsername"].asString();
            post(n);
            return;
        }
        finishLogin(e, a, ev);
        post(ev);
    }

    void httpLoop() {
        for (;;) {
            std::function<void()> cmd;
            {
                std::unique_lock<std::mutex> lk(mu);
                auto ready = [&] { return stopping || !httpQ.empty() || (sso.active && Clock::now() >= sso.nextPoll); };
                if (sso.active) httpCv.wait_until(lk, sso.nextPoll, ready);
                else httpCv.wait(lk, ready);
                if (stopping) return;
                if (!httpQ.empty()) {
                    cmd = std::move(httpQ.front());
                    httpQ.pop_front();
                }
            }
            if (cmd) cmd();
            else if (sso.active && Clock::now() >= sso.nextPoll) ssoPollOnce();
        }
    }

    // =========================================================================================
    // Realtime (net-rt thread)
    // =========================================================================================

    void setState(ConnState s, const std::string& error = std::string()) {
        int prev = connState.exchange(int(s));
        if (prev == int(s) && error.empty()) return;
        Event ev;
        ev.kind = Event::Kind::ConnectionChanged;
        ev.state = s;
        ev.ok = s == ConnState::Online;
        ev.error = error;
        if (s == ConnState::Banned) {
            ev.noticeCode = int(pr::NoticeCode::Banned);
            ev.noticeArg = rt.banUntil;
        }
        post(ev);
    }

    template <class M> bool send(M& m) {
        if (!rt.ws) return false;
        m.seq = ++rt.seq;
        std::vector<uint8_t> buf;
        pr::encode(m, buf);
        return rt.ws->send(buf);
    }

    // Commands that need the Welcome first; others answer "offline" or "invalid_request".
    template <class M> void sendGame(M m) {
        if (!rt.ws || !rt.welcomed) { localError("offline"); return; }
        if (!pr::valid(m)) { localError("invalid_request"); return; }
        send(m);
    }

    void localError(const char* what) {
        Event ev;
        ev.kind = Event::Kind::ServerError;
        ev.error = what;
        post(ev);
    }

    void dropSocket(uint16_t code) {
        if (rt.ws) {
            std::unique_ptr<WebSocket> ws = std::move(rt.ws);
            ws->close(code);
        }
        rt.welcomed = false;
        rt.samples.clear();
        rt.rttEma = -1;
        ping.store(-1);
    }

    void scheduleRetry(RetryCause why, uint32_t retryAfterMs = 0) {
        const bool inGame = rt.game.id != 0 && rt.game.status == int(pr::GameStatus::Ongoing);
        uint32_t delay = reconnectDelayMs(rt.attempt, why, jitterUniform(), inGame, retryAfterMs);
        ++rt.attempt;
        rt.nextAttempt = Clock::now() + std::chrono::milliseconds(delay);
        LOGI("net: next connection attempt in %.1f s", delay / 1000.0);
        setState(ConnState::Reconnecting);
    }

    // Every outcome that stops the automatic reconnection (incompatible, unauthorized, certificate,
    // banned, replaced...) also forgets the /info answer: the next connect() reads it again.
    void stopWanting(ConnState s, const std::string& error) {
        rt.wanted = false;
        rt.info = Rt::Info();
        rt.restarting = false;
        setState(s, error);
    }

    // The last /info answer serves an automatic attempt when a connection built on it reached
    // Welcome, for the same endpoint, and it was read or last in use less than 10 minutes ago
    // (a Hello accepted since then proved the protocol, a working upgrade the path). After a
    // shutdown too, so that the reconnection wave of a restart costs one TLS handshake per player,
    // as after a crash. What a restart can change is caught without it: another server (a
    // reinstall) by the server id of the 101 answer, before Hello (tryConnect); another path or
    // subprotocol by the upgrade's 404 or 426, after which /info is read again; another protocol
    // by Hello (close 4002, Incompatible).
    bool infoReusable(const ServerEndpoint& e) const {
        return rt.info.valid && rt.info.proven && rt.info.origin == e.origin() &&
               rt.info.insecure == e.insecureDev && Clock::now() - rt.info.at < kInfoReuse;
    }

    void tryConnect() {
        uint32_t gen = connectGen.load();
        rtCancel.reset();
        const ServerEndpoint e = rt.ep;
        if (!e.valid()) { stopWanting(ConnState::Offline, "invalid_server"); return; }
        if (connState.load() != int(ConnState::Reconnecting)) setState(ConnState::Connecting);
        Credential c;
        if (!creds.get(e.origin(), c) || c.token.empty()) { stopWanting(ConnState::Unauthorized, "not_logged_in"); return; }
        if (!plausibleToken(c.token)) { creds.clearToken(e.origin()); stopWanting(ConnState::Unauthorized, "not_logged_in"); return; }

        // /api/v1/info (compatibility, wsPath): read again, unless an automatic reconnection can
        // reuse the answer a connection reached Welcome with (infoReusable).
        const bool reused = infoReusable(e);
        if (!reused) {
            rt.info = Rt::Info();
            ServerInfo info;
            Api a = fetchInfo(e, info, rtCancel);
            if (gen != connectGen.load() || stopFlag.load()) return;
            if (!a.ok()) {
                if (a.error == "certificate" || a.error == "insecure" || a.error == "unavailable" || a.error == "invalid_server") {
                    stopWanting(ConnState::Offline, a.error);
                } else {
                    scheduleRetry(RetryCause::Failure, uint32_t(std::clamp(a.retryAfter, 0, 600)) * 1000u);
                }
                return;
            }
            if (!info.compatible) { stopWanting(ConnState::Incompatible, "incompatible"); return; }
            if (!creds.get(e.origin(), c) || c.token.empty()) { stopWanting(ConnState::Unauthorized, "server_changed"); return; }
            std::string path = a.body["wsPath"].asString("/ws");
            bool pathOk = !path.empty() && path[0] == '/' && path.size() < 128;
            for (char ch : path) pathOk = pathOk && ch > ' ' && ch <= '~';
            rt.info.valid = true;
            rt.info.origin = e.origin();
            rt.info.insecure = e.insecureDev;
            rt.info.wsPath = pathOk ? path : "/ws";
            rt.info.at = Clock::now();
        }

        WsParams p;
        p.host = e.host;                               // the WebSocket always goes to the API's host
        p.port = e.effectiveWsPort();                  // left empty: the API's port
        p.tls = !e.insecureDev;
        p.pinnedSha256 = p.tls ? effectivePin(e) : std::string();
        p.path = rt.info.wsPath;
        p.subprotocol = pr::kWsSubprotocol;
        p.maxMessageBytes = 256 * 1024;
        p.onActivity = [this] { wakeRt(); };
        std::string err;
        int httpStatus = 0;
        std::unique_ptr<WebSocket> ws = wsConnect(p, err, httpStatus, &rtCancel);
        if (gen != connectGen.load() || stopFlag.load()) {
            if (ws) ws->close(1001);
            return;
        }
        if (!ws) {
            LOGW("net: websocket %s:%u failed: %s", p.host.c_str(), p.port, err.c_str());
            // A 4xx other than 429 (404, 426...) is this server refusing the request as made. A 5xx
            // is a server (or its reverse proxy: 502, 504) that cannot answer now: retried like a
            // network failure, with the same /info answer.
            const bool refused = httpStatus >= 400 && httpStatus < 500 && httpStatus != 429;
            if (err == "certificate" || err == "insecure" || err == "unavailable") {
                stopWanting(ConnState::Offline, err);
            } else if (reused && (err == "subprotocol" || refused)) {
                // The server may have changed since its /info was read: read it again next time.
                rt.info = Rt::Info();
                scheduleRetry(RetryCause::Failure);
            } else if (err == "subprotocol") {
                stopWanting(ConnState::Offline, err);
            } else if (httpStatus == 503) {
                // Full, unless it is the first 503 since a shutdown (a draining server refuses
                // upgrades with 503): later ones mean that the server came back full.
                const bool restart = rt.restarting;
                rt.restarting = false;
                scheduleRetry(restart ? RetryCause::Failure : RetryCause::ServerFull);
            } else {
                scheduleRetry(RetryCause::Failure);
            }
            return;
        }
        // The per-origin identity rule of fetchInfo, for the server that answered this upgrade (a
        // reused /info answer was read from the server that was there before).
        if (!ws->serverId().empty() && !c.serverId.empty() && ws->serverId() != c.serverId) {
            LOGW("net: %s announces another server id; its saved session is discarded", e.origin().c_str());
            ws->close(1000);
            creds.clearToken(e.origin());
            stopWanting(ConnState::Unauthorized, "server_changed");
            return;
        }
        rt.ws = std::move(ws);
        rt.seq = 0;
        rt.welcomed = false;
        rt.lastFatal = 0;
        rt.shutdownNotice = false;
        rt.connectedAt = rt.lastRecv = Clock::now();
        rt.probedAt = Clock::time_point{};
        pr::Hello h;
        h.proto = pr::kProtocolVersion;
        h.schema = pr::kSchemaHash;
        h.client = clientString();
        h.token = c.token;
        send(h);
    }

    // One client Ping; the next one follows the burst after Welcome, then Welcome.clientPingMs.
    void sendPing() {
        pr::C_Ping m;
        m.nonce = ++rt.pingNonce;
        rt.sent[m.nonce % 8] = {m.nonce, Clock::now()};
        send(m);
        uint32_t next = rt.pingBurst > 0 ? uint32_t(kPingBurstGapMs) : rt.pingEveryMs;
        if (rt.pingBurst > 0) --rt.pingBurst;
        rt.nextPing = Clock::now() + std::chrono::milliseconds(next);
    }

    void onPong(const pr::S_Pong& m) {
        const Rt::Sent& s = rt.sent[m.nonce % 8];
        if (s.nonce != m.nonce || s.nonce == 0) return;
        Clock::time_point now = Clock::now();
        double rtt = std::chrono::duration<double, std::milli>(now - s.at).count();
        if (rtt < 0 || rtt > 60000) return;
        // Smoothed round trip: a quarter per sample at the burst's pace, half once samples are 10 s
        // or more apart, so the indicator follows a lasting change within a sample or two whatever
        // the interval (the weight grows with the time since the previous sample).
        double gap = std::chrono::duration<double, std::milli>(now - rt.lastPong).count();
        double w = std::clamp(gap / 20000.0, 0.25, 0.5);
        rt.rttEma = rt.rttEma < 0 ? rtt : rt.rttEma + w * (rtt - rt.rttEma);
        rt.lastPong = now;
        ping.store(int(std::lround(rt.rttEma)));
        // Clock offset: the server stamped its clock about rtt/2 before we received the Pong.
        // Keep the sample with the lowest round trip (least queueing noise) among the last 8 of the
        // last 5 minutes (with pings a minute apart, older ones would carry the clocks' drift).
        double offset = m.serverTime + rtt * 0.5 - localEpochMs();
        rt.samples.push_back({rtt, offset, now});
        while (rt.samples.size() > kOffsetSamples || (rt.samples.size() > 1 && now - rt.samples.front().at > kOffsetMaxAge))
            rt.samples.pop_front();
        auto best = std::min_element(rt.samples.begin(), rt.samples.end(),
                                     [](const Rt::Sample& a, const Rt::Sample& b) { return a.rtt < b.rtt; });
        clockOffset.store(best->offset);
        rt.haveOffset = true;
    }

    static OnlineGame fromSnapshot(const pr::GameSnapshot& s) {
        OnlineGame g;
        g.id = s.game;
        g.category = s.category;
        g.baseMs = s.baseMs;
        g.incMs = s.incMs;
        g.rated = s.rated;
        g.white = {s.white.userId, s.white.name, s.white.rating, s.white.provisional};
        g.black = {s.black.userId, s.black.name, s.black.rating, s.black.provisional};
        g.you = int(s.you);
        for (const pr::MoveRec& m : s.moves) g.moves.push_back({m.move, m.spentMs, m.clockMs});
        g.running = int(s.running);
        g.whiteMs = s.whiteMs;
        g.blackMs = s.blackMs;
        g.serverTimeMs = s.serverTime;
        g.drawOfferBy = int(s.drawOffer);
        g.status = int(s.status);
        g.reason = int(s.reason);
        g.whiteConnected = s.whiteConnected;
        g.blackConnected = s.blackConnected;
        g.graceMs = s.graceMs;
        g.firstMoveMs = s.firstMoveMs;
        g.rematchBy = int(s.rematch);
        g.autoPress = s.autoPress;
        return g;
    }

    void resync(uint64_t gameId) {
        if (!rt.welcomed || gameId == 0) return;
        pr::Resync r;
        r.game = gameId;
        send(r);
    }

    Event gameEvent(Event::Kind k) {
        Event ev;
        ev.kind = k;
        ev.ok = true;
        ev.game = rt.game;
        ev.gameId = rt.game.id;
        return ev;
    }

    void handleMessage(const std::vector<uint8_t>& b) {
        pr::MsgType t;
        const uint8_t* p = b.data();
        size_t n = b.size();
        if (!pr::peekType(p, n, t) || pr::isClientType(uint8_t(t))) {
            LOGW("net: ignoring a frame of unknown type (%u bytes)", unsigned(n));
            return;
        }
        rt.lastRecv = Clock::now();
        auto bad = [&] { LOGW("net: malformed %s from the server (%u bytes)", pr::messageName(t), unsigned(n)); };
        switch (t) {
        case pr::MsgType::Welcome: {
            pr::Welcome m;
            if (!pr::decode(p, n, m)) return bad();
            rt.welcomed = true;
            rt.attempt = 0;
            rt.heartbeatMs = m.heartbeatMs;
            rt.pingEveryMs = clientPingIntervalMs(m.clientPingMs);
            rt.pingBurst = kPingBurst;
            rt.restarting = false;
            rt.gestures.reset(steadyMs(), m.gestureRate, gestureSendCapacity(m.gestureBurst));
            if (rt.info.valid) rt.info.proven = true;
            if (!rt.haveOffset) clockOffset.store(m.serverTime - localEpochMs());
            setState(ConnState::Online);
            Event ev;
            ev.kind = Event::Kind::Welcome;
            ev.ok = true;
            ev.account.userId = m.userId;
            ev.account.username = m.username;
            ev.serverName = m.serverName;
            ev.gameId = m.activeGame;
            post(ev);
            sendPing();
            // The game we showed ended while we were away: ask for its final state.
            if (m.activeGame == 0 && rt.game.id != 0 && rt.game.status == int(pr::GameStatus::Ongoing)) resync(rt.game.id);
            break;
        }
        case pr::MsgType::Error: {
            pr::Error m;
            if (!pr::decode(p, n, m)) return bad();
            if (m.fatal) rt.lastFatal = int(m.code);
            Event ev;
            ev.kind = Event::Kind::ServerError;
            ev.code = int(m.code);
            ev.fatal = m.fatal;
            ev.gameId = m.game;
            ev.error = pr::enumName(m.code);
            post(ev);
            break;
        }
        case pr::MsgType::S_Ping: {
            pr::S_Ping m;
            if (!pr::decode(p, n, m)) return bad();
            pr::C_Pong r;
            r.nonce = m.nonce;
            send(r);
            break;
        }
        case pr::MsgType::S_Pong: {
            pr::S_Pong m;
            if (!pr::decode(p, n, m)) return bad();
            onPong(m);
            break;
        }
        case pr::MsgType::Ack:
            break;
        case pr::MsgType::Notice: {
            pr::Notice m;
            if (!pr::decode(p, n, m)) return bad();
            if (m.code == pr::NoticeCode::Banned) rt.banUntil = m.arg;
            if (m.code == pr::NoticeCode::ServerShutdown) rt.shutdownNotice = true;
            if (m.code == pr::NoticeCode::SessionRevoked) creds.clearToken(rt.ep.origin());
            if (m.code == pr::NoticeCode::ReplacedByNewConnection) rt.lastFatal = int(pr::ErrorCode::Replaced);
            Event ev;
            ev.kind = Event::Kind::Notice;
            ev.ok = true;
            ev.noticeCode = int(m.code);
            ev.noticeArg = m.arg;
            post(ev);
            break;
        }
        case pr::MsgType::QueueStatus: {
            pr::QueueStatus m;
            if (!pr::decode(p, n, m)) return bad();
            Event ev;
            ev.kind = Event::Kind::QueueStatus;
            ev.ok = true;
            ev.queueCategory = m.category;
            ev.queueRated = m.rated;
            ev.queueState = int(m.state);
            ev.queueWaitMs = m.waitMs;
            ev.queueWindow = m.window;
            ev.queued = m.queued;
            post(ev);
            break;
        }
        case pr::MsgType::ChallengeReceived: {
            pr::ChallengeReceived m;
            if (!pr::decode(p, n, m)) return bad();
            Event ev;
            ev.kind = Event::Kind::ChallengeReceived;
            ev.ok = true;
            ev.challengeId = m.id;
            ev.challenger = {m.from.userId, m.from.name, m.from.rating, m.from.provisional};
            ev.challengeBaseSec = m.baseSec;
            ev.challengeIncSec = m.incSec;
            ev.challengeRated = m.rated;
            ev.challengeColor = int(m.yourColor);
            ev.challengeExpiresMs = m.expiresMs;
            post(ev);
            break;
        }
        case pr::MsgType::ChallengeStatus: {
            pr::ChallengeStatus m;
            if (!pr::decode(p, n, m)) return bad();
            Event ev;
            ev.kind = Event::Kind::ChallengeStatus;
            ev.ok = true;
            ev.challengeId = m.id;
            ev.challengeState = int(m.state);
            ev.challengeTarget = m.target;
            ev.challengeCode = m.code;
            ev.challengeBaseSec = m.baseSec;
            ev.challengeIncSec = m.incSec;
            ev.challengeRated = m.rated;
            post(ev);
            break;
        }
        case pr::MsgType::GameSnapshot: {
            pr::GameSnapshot m;
            if (!pr::decode(p, n, m)) return bad();
            rt.game = fromSnapshot(m);
            rt.lastGseq = m.gseq;
            if (rt.pending.game == m.game && rt.pending.ply < int(rt.game.moves.size())) rt.pending = Rt::Pending();
            post(gameEvent(Event::Kind::GameSnapshot));
            break;
        }
        case pr::MsgType::MoveMade: {
            pr::MoveMade m;
            if (!pr::decode(p, n, m)) return bad();
            OnlineGame& g = rt.game;
            if (m.game != g.id) { resync(m.game); return; }
            if (m.ply < g.moves.size()) return;                     // already known (resent)
            if (m.ply > g.moves.size()) { resync(m.game); return; } // missed something
            int mover = m.ply & 1;
            g.moves.push_back({m.move, m.spentMs, mover == 0 ? m.whiteMs : m.blackMs});
            g.whiteMs = m.whiteMs;
            g.blackMs = m.blackMs;
            g.serverTimeMs = m.serverTime;
            g.firstMoveMs = m.firstMoveMs;
            // Plies 0 and 1 do not run the clocks (DESIGN 6.1): White's clock starts after ply 1.
            g.running = g.status != int(pr::GameStatus::Ongoing) || m.ply == 0 ? 2 : (m.ply + 1) & 1;
            if (m.drawOffer) g.drawOfferBy = mover;
            else if (g.drawOfferBy == (mover ^ 1)) g.drawOfferBy = 2;   // a move declines the opponent's offer
            rt.lastGseq = std::max(rt.lastGseq, m.gseq);
            Event ev = gameEvent(Event::Kind::MoveMade);
            ev.ply = m.ply;
            ev.move = m.move;
            ev.flags = m.flags;
            ev.spentMs = m.spentMs;
            ev.color = mover;
            bool matchesPending = rt.pending.game == m.game && rt.pending.ply == int(m.ply) && rt.pending.move == m.move;
            ev.mine = matchesPending || g.you == mover;
            if (matchesPending) rt.pending = Rt::Pending();
            post(ev);
            break;
        }
        case pr::MsgType::MoveRejected: {
            pr::MoveRejected m;
            if (!pr::decode(p, n, m)) return bad();
            if (rt.pending.game == m.game && rt.pending.ply == int(m.ply)) rt.pending = Rt::Pending();
            Event ev = gameEvent(Event::Kind::MoveRejected);
            ev.gameId = m.game;
            ev.ply = m.ply;
            ev.move = m.move;
            ev.code = int(m.code);
            ev.error = pr::enumName(m.code);
            post(ev);
            break;
        }
        case pr::MsgType::GameEvent: {
            pr::GameEvent m;
            if (!pr::decode(p, n, m)) return bad();
            OnlineGame& g = rt.game;
            if (m.game == g.id) {
                int c = int(m.color);
                switch (m.kind) {
                case pr::GameEventKind::DrawOffered: g.drawOfferBy = c; break;
                case pr::GameEventKind::DrawDeclined: g.drawOfferBy = 2; break;
                case pr::GameEventKind::PlayerDisconnected:
                    if (c == 0) g.whiteConnected = false;
                    if (c == 1) g.blackConnected = false;
                    g.graceMs = m.arg;
                    break;
                case pr::GameEventKind::PlayerReconnected:
                    if (c == 0) g.whiteConnected = true;
                    if (c == 1) g.blackConnected = true;
                    break;
                case pr::GameEventKind::RematchOffered: g.rematchBy = c; break;
                case pr::GameEventKind::RematchDeclined: g.rematchBy = 2; break;
                default: break;
                }
                rt.lastGseq = std::max(rt.lastGseq, m.gseq);
            }
            Event ev = gameEvent(Event::Kind::GameEvent);
            ev.gameId = m.game;
            ev.gameEventKind = int(m.kind);
            ev.color = int(m.color);
            ev.arg = m.arg;
            post(ev);
            break;
        }
        case pr::MsgType::GameEnd: {
            pr::GameEnd m;
            if (!pr::decode(p, n, m)) return bad();
            OnlineGame& g = rt.game;
            if (m.game == g.id) {
                g.status = int(m.status);
                g.reason = int(m.reason);
                g.whiteMs = m.whiteMs;
                g.blackMs = m.blackMs;
                g.serverTimeMs = m.serverTime;
                g.running = 2;
                g.drawOfferBy = 2;
                rt.lastGseq = std::max(rt.lastGseq, m.gseq);
            }
            Event ev = gameEvent(Event::Kind::GameEnd);
            ev.gameId = m.game;
            post(ev);
            break;
        }
        case pr::MsgType::RatingUpdate: {
            pr::RatingUpdate m;
            if (!pr::decode(p, n, m)) return bad();
            Event ev = gameEvent(Event::Kind::RatingUpdate);
            ev.gameId = m.game;
            ev.queueCategory = m.category;
            ev.ratingWhite = {m.white.before, m.white.after, int(m.white.games), m.white.provisional};
            ev.ratingBlack = {m.black.before, m.black.after, int(m.black.games), m.black.provisional};
            post(ev);
            break;
        }
        case pr::MsgType::S_Gesture: {
            pr::S_Gesture m;
            if (!pr::decode(p, n, m)) return bad();
            if (m.game == 0 || m.game != rt.game.id) break;   // not the game shown
            Event ev;
            ev.kind = Event::Kind::OpponentGesture;
            ev.ok = true;
            ev.gameId = m.game;
            ev.gesture = gestureFromWire(m);
            postGesture(std::move(ev));
            break;
        }
        default:
            break;
        }
    }

    // The game thread's latest Gesture: sent when its bucket has a token, otherwise left for
    // later (nextRtDeadline wakes the loop then). Dropped when it cannot go: not Online (it is
    // never kept for a reconnection), no relay on this server, or not the current game.
    void flushGesture() {
        GestureOut out;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!gestureOut.pending) return;
            const bool usable = rt.ws && rt.welcomed && rt.gestures.enabled() && gestureOut.game != 0 && gestureOut.game == rt.game.id;
            if (usable && !rt.gestures.take(steadyMs())) return;
            out = gestureOut;
            gestureOut.pending = false;
            if (!usable) return;
        }
        pr::C_Gesture m;
        m.game = out.game;
        gestureToWire(out.g, m);
        send(m);
    }

    void onClosed(uint16_t code, const std::string& reason) {
        LOGI("net: realtime connection closed (%u %s)", code, reason.c_str());
        const bool wasOnline = rt.welcomed;
        dropSocket(1000);
        if (wasOnline && rt.info.proven) rt.info.at = Clock::now();   // the /info answer worked until now
        int fatal = rt.lastFatal;
        if (code == pr::CloseCode::UnsupportedProtocol || fatal == int(pr::ErrorCode::UnsupportedProtocol)) {
            stopWanting(ConnState::Incompatible, "incompatible");
        } else if (code == pr::CloseCode::Unauthorized || fatal == int(pr::ErrorCode::Unauthorized)) {
            creds.clearToken(rt.ep.origin());
            stopWanting(ConnState::Unauthorized, "unauthorized");
        } else if (fatal == int(pr::ErrorCode::EmailUnverified)) {
            stopWanting(ConnState::Unauthorized, "email_unverified");
        } else if (code == pr::CloseCode::Banned || fatal == int(pr::ErrorCode::Banned)) {
            stopWanting(ConnState::Banned, "banned");
        } else if (code == pr::CloseCode::Replaced || fatal == int(pr::ErrorCode::Replaced)) {
            // Another client of this account took over: fighting back would loop forever.
            stopWanting(ConnState::Offline, "replaced");
        } else if (code == pr::CloseCode::CheatDetected || fatal == int(pr::ErrorCode::CheatDetected)) {
            // The server's fair-play checks stopped this client: the player decides what comes next.
            stopWanting(ConnState::Offline, "cheat_detected");
        } else if (rt.wanted) {
            RetryCause why = RetryCause::Failure;
            if (code == kCloseServerFull || fatal == int(pr::ErrorCode::ServerFull)) {
                why = RetryCause::ServerFull;
            } else if (code == pr::CloseCode::ShuttingDown || fatal == int(pr::ErrorCode::ShuttingDown) || rt.shutdownNotice) {
                why = RetryCause::Shutdown;
                rt.restarting = true;
            }
            scheduleRetry(why);
        } else {
            setState(ConnState::Offline);
        }
    }

    Clock::time_point nextRtDeadline() const {
        Clock::time_point t = Clock::now() + std::chrono::seconds(5);
        if (rt.wanted && !rt.ws) t = std::min(t, rt.nextAttempt);
        if (rt.ws && rt.welcomed) t = std::min(t, rt.nextPing);
        if (rt.ws) t = std::min(t, Clock::now() + std::chrono::milliseconds(500));
        if (rt.ws && rt.welcomed && gestureOut.pending) {   // called with mu held
            double now = steadyMs();
            double wait = std::ceil(rt.gestures.readyAtMs(now) - now);
            t = std::min(t, Clock::now() + std::chrono::milliseconds(int64_t(wait)));
        }
        return t;
    }

    void rtLoop() {
        for (;;) {
            std::deque<std::function<void()>> cmds;
            {
                std::unique_lock<std::mutex> lk(mu);
                rtCv.wait_until(lk, nextRtDeadline(), [&] { return stopping || !rtQ.empty() || rtWake; });
                if (stopping) break;
                cmds.swap(rtQ);
                rtWake = false;
            }
            for (auto& c : cmds) c();
            // Before reading: a Gesture made while the connection was down never goes after the
            // Welcome that may be waiting.
            flushGesture();
            if (rt.ws) {
                std::vector<uint8_t> msg;
                while (rt.ws && rt.ws->receive(msg)) handleMessage(msg);
                uint16_t code;
                std::string reason;
                if (rt.ws && rt.ws->closed(code, reason)) {
                    // Messages that arrived just before the close still count.
                    while (rt.ws->receive(msg)) handleMessage(msg);
                    onClosed(code, reason);
                }
            }
            Clock::time_point now = Clock::now();
            if (rt.ws) {
                // Liveness: the server pings about every heartbeatMs. When nothing came for 1.5
                // heartbeats, one Ping of ours asks for an answer at once (a heartbeat the server
                // sent late, or skipped, then costs nothing); the connection is dead at two.
                uint32_t silence = std::max<uint32_t>(10000, std::min<uint32_t>(rt.heartbeatMs, 60000) * 2);
                auto quiet = now - rt.lastRecv;
                if (!rt.welcomed && now - rt.connectedAt > std::chrono::seconds(10)) {
                    LOGW("net: no Welcome within 10 s");
                    onClosed(1006, "hello timeout");
                } else if (quiet > std::chrono::milliseconds(silence)) {
                    LOGW("net: the server has been silent for %u ms", silence);
                    onClosed(1006, "heartbeat timeout");
                } else if (rt.welcomed && quiet > std::chrono::milliseconds(silence / 4 * 3) && rt.probedAt != rt.lastRecv) {
                    rt.probedAt = rt.lastRecv;
                    sendPing();
                } else if (rt.welcomed && now >= rt.nextPing) {
                    sendPing();
                }
            } else if (rt.wanted && now >= rt.nextAttempt) {
                tryConnect();
            }
        }
        dropSocket(1001);
    }
};

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

OnlineClient::OnlineClient() : impl_(std::make_unique<Impl>()) {
    ServerEndpoint e = officialServer();
    impl_->ep = e;
    Impl* d = impl_.get();
    d->realtime([d, e] { d->rt.ep = e; });
}

OnlineClient::~OnlineClient() = default;

void OnlineClient::setCredentialsFile(const std::string& path) { impl_->creds.setPath(path); }

void OnlineClient::setServer(const ServerEndpoint& ep) {
    ServerEndpoint e = ep;
    e.host = lower(e.host);
    if (!e.host.empty() && e.host.front() == '[' && e.host.back() == ']') e.host = e.host.substr(1, e.host.size() - 2);
    std::string pin = normalizePin(e.pinnedSha256);
    if (!pin.empty()) e.pinnedSha256 = pin;
    bool originChanged = e.origin() != impl_->ep.origin() || e.insecureDev != impl_->ep.insecureDev;
    impl_->ep = e;
    Impl* d = impl_.get();
    if (originChanged) {
        // Nothing of the previous server survives: connection, game view, SSO, MFA step.
        d->connectGen.fetch_add(1);
        d->rtCancel.cancel();
        d->realtime([d, e] {
            d->rt.wanted = false;
            d->dropSocket(1000);
            d->rt.ep = e;
            d->rt.info = Impl::Rt::Info();
            d->rt.restarting = false;
            d->rt.game = OnlineGame();
            d->rt.pending = Impl::Rt::Pending();
            d->rt.banUntil = 0;
            d->setState(ConnState::Offline);
        });
        d->http([d] {
            d->ssoFinished();
            d->mfaToken.clear();
            d->ssoTicket.clear();
        });
        d->view = OnlineGame();
        d->hasView = false;
    } else {
        d->realtime([d, e] { d->rt.ep = e; });
    }
}

const ServerEndpoint& OnlineClient::server() const { return impl_->ep; }

void OnlineClient::fetchServerInfo() {
    Impl* d = impl_.get();
    ServerEndpoint e = d->ep;
    d->http([d, e] {
        Event ev;
        ev.kind = Event::Kind::ServerInfoResult;
        Impl::Api a = d->fetchInfo(e, ev.info, d->httpCancel);
        Impl::fillError(ev, a);
        if (a.ok() && !ev.info.compatible) {
            ev.ok = false;
            ev.error = "incompatible";
        }
        d->post(ev);
    });
}

bool OnlineClient::hasSavedSession() const { return impl_->ep.valid() && impl_->creds.hasToken(impl_->ep.origin()); }

std::string OnlineClient::savedUsername() const { return impl_->creds.username(impl_->ep.origin()); }

void OnlineClient::registerAccount(const std::string& username, const std::string& email, const std::string& password) {
    Impl* d = impl_.get();
    ServerEndpoint e = d->ep;
    d->http([d, e, username, email, password] {
        json::Value b = json::Value::object();
        b.set("username", username);
        b.set("email", email);
        b.set("password", password);
        Impl::Api a = d->api(e, "POST", "/auth/register", &b, false, d->httpCancel);
        Event ev;
        ev.kind = Event::Kind::RegisterResult;
        Impl::fillError(ev, a);
        ev.account.username = username;
        ev.account.emailVerified = a.ok() && a.body["status"].asString() == "ready";
        if (a.ok()) {
            Credential c;
            d->creds.get(e.origin(), c);
            if (c.token.empty()) {
                c.origin = e.origin();
                c.username = username;
                d->creds.put(c);
            }
        }
        d->post(ev);
    });
}

void OnlineClient::login(const std::string& usernameOrEmail, const std::string& password) {
    Impl* d = impl_.get();
    ServerEndpoint e = d->ep;
    d->http([d, e, usernameOrEmail, password] {
        json::Value b = json::Value::object();
        b.set("login", usernameOrEmail);
        b.set("password", password);
        b.set("clientLabel", clientString());
        Impl::Api a = d->api(e, "POST", "/auth/login", &b, false, d->httpCancel);
        Event ev;
        ev.kind = Event::Kind::LoginResult;
        d->finishLogin(e, a, ev);
        d->post(ev);
    });
}

void OnlineClient::loginMfa(const std::string& code) {
    Impl* d = impl_.get();
    ServerEndpoint e = d->ep;
    d->http([d, e, code] {
        Event ev;
        ev.kind = Event::Kind::LoginResult;
        if (d->mfaToken.empty() || d->mfaOrigin != e.origin()) {
            ev.error = "invalid_mfa_token";
            d->post(ev);
            return;
        }
        json::Value b = json::Value::object();
        b.set("mfaToken", d->mfaToken);
        bool digits = code.size() == 6 && std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; });
        b.set(digits ? "code" : "recoveryCode", code);
        Impl::Api a = d->api(e, "POST", "/auth/login/mfa", &b, false, d->httpCancel);
        d->finishLogin(e, a, ev);
        d->post(ev);
    });
}

void OnlineClient::startGoogleSso() {
    Impl* d = impl_.get();
    ServerEndpoint e = d->ep;
    d->http([d, e] {
        d->ssoFinished();
        Event ev;
        ev.kind = Event::Kind::SsoBrowserOpened;
        crypto::Pkce pkce;
        if (!crypto::makePkce(pkce)) {
            ev.error = "random";
            d->post(ev);
            return;
        }
        json::Value b = json::Value::object();
        b.set("codeChallenge", pkce.challenge);
        b.set("codeChallengeMethod", "S256");
        Impl::Api a = d->api(e, "POST", "/auth/sso/google/start", &b, false, d->httpCancel);
        Impl::fillError(ev, a);
        if (!a.ok()) { d->post(ev); return; }
        std::string url = a.body["authUrl"].asString();
        // Only an https page is handed to the shell (a hostile server must not start programs).
        bool urlOk = url.compare(0, 8, "https://") == 0 && url.size() < 4096;
        for (char c : url) urlOk = urlOk && c > ' ' && c <= '~' && c != '"' && c != '\\';
        if (!urlOk || a.body["attemptId"].asString().empty()) {
            ev.ok = false;
            ev.error = "bad_response";
            d->post(ev);
            return;
        }
        if (!sys::openBrowser(url)) {
            ev.ok = false;
            ev.error = "browser";
            d->post(ev);
            return;
        }
        d->sso.active = true;
        d->sso.ep = e;
        d->sso.attemptId = a.body["attemptId"].asString();
        d->sso.verifier = pkce.verifier;
        d->sso.pollMs = int(std::clamp<int64_t>(a.body["pollMs"].asInt(2000), 1000, 10000));
        int64_t expires = std::clamp<int64_t>(a.body["expiresIn"].asInt(600), 30, 1800);
        d->sso.nextPoll = Clock::now() + std::chrono::milliseconds(d->sso.pollMs);
        d->sso.expires = Clock::now() + std::chrono::seconds(expires);
        ev.ok = true;
        d->post(ev);
    });
}

void OnlineClient::completeSso(const std::string& username) {
    Impl* d = impl_.get();
    ServerEndpoint e = d->ep;
    d->http([d, e, username] {
        Event ev;
        ev.kind = Event::Kind::LoginResult;
        if (d->ssoTicket.empty() || d->ssoTicketOrigin != e.origin()) {
            ev.error = "sso_expired";
            d->post(ev);
            return;
        }
        json::Value b = json::Value::object();
        b.set("ssoTicket", d->ssoTicket);
        b.set("username", username);
        b.set("clientLabel", clientString());
        Impl::Api a = d->api(e, "POST", "/auth/sso/complete", &b, false, d->httpCancel);
        // A taken or invalid name keeps the ticket for another try.
        if (a.ok()) d->ssoTicket.clear();
        d->finishLogin(e, a, ev);
        d->post(ev);
    });
}

void OnlineClient::cancelSso() {
    Impl* d = impl_.get();
    d->http([d] {
        bool was = d->sso.active;
        d->ssoFinished();
        d->ssoTicket.clear();
        if (was) {
            Event ev;
            ev.kind = Event::Kind::LoginResult;
            ev.error = "cancelled";
            d->post(ev);
        }
    });
}

void OnlineClient::logout(bool allSessions) {
    Impl* d = impl_.get();
    ServerEndpoint e = d->ep;
    d->connectGen.fetch_add(1);
    d->realtime([d] {
        d->rt.wanted = false;
        d->dropSocket(1000);
        d->setState(ConnState::Offline);
    });
    d->http([d, e, allSessions] {
        json::Value b = json::Value::object();
        Impl::Api a = d->api(e, "POST", allSessions ? "/auth/logout-all" : "/auth/logout", &b, true, d->httpCancel);
        d->creds.clearToken(e.origin());   // gone locally whatever the server said
        Event ev;
        ev.kind = Event::Kind::LogoutResult;
        Impl::fillError(ev, a);
        if (a.status == 401 || a.error == "not_logged_in") {
            ev.ok = true;
            ev.error.clear();
        }
        d->post(ev);
    });
}

void OnlineClient::fetchAccount() {
    Impl* d = impl_.get();
    ServerEndpoint e = d->ep;
    d->http([d, e] {
        Impl::Api a = d->api(e, "GET", "/account/me", nullptr, true, d->httpCancel);
        if (a.status == 401) d->creds.clearToken(e.origin());   // expired or revoked session
        Event ev;
        ev.kind = Event::Kind::AccountResult;
        Impl::fillError(ev, a);
        if (a.ok()) ev.account = Impl::parseAccount(a.body);
        d->post(ev);
    });
}

namespace {
// Fire-and-report helper for the simple POST endpoints.
void simplePost(OnlineClient::Impl* d, const ServerEndpoint& e, Event::Kind kind, const std::string& path, json::Value body, bool auth,
                std::function<void(const json::Value&, Event&)> fill = nullptr) {
    d->http([d, e, kind, path, body, auth, fill] {
        OnlineClient::Impl::Api a = d->api(e, "POST", path, &body, auth, d->httpCancel);
        Event ev;
        ev.kind = kind;
        OnlineClient::Impl::fillError(ev, a);
        if (a.ok() && fill) fill(a.body, ev);
        d->post(ev);
    });
}

json::Value obj(std::initializer_list<std::pair<const char*, json::Value>> kv) {
    json::Value o = json::Value::object();
    for (auto& p : kv) o.set(p.first, p.second);
    return o;
}

void readCodes(const json::Value& b, Event& ev) {
    for (const json::Value& c : b["recoveryCodes"].items()) ev.recoveryCodes.push_back(c.asString());
}
}  // namespace

void OnlineClient::resendVerification(const std::string& email) {
    simplePost(impl_.get(), impl_->ep, Event::Kind::VerificationResent, "/auth/verify-email/resend", obj({{"email", email}}), false);
}

void OnlineClient::forgotPassword(const std::string& email) {
    simplePost(impl_.get(), impl_->ep, Event::Kind::PasswordResetRequested, "/auth/password/forgot", obj({{"email", email}}), false);
}

void OnlineClient::changePassword(const std::string& current, const std::string& next) {
    simplePost(impl_.get(), impl_->ep, Event::Kind::PasswordChanged, "/account/password",
               obj({{"currentPassword", current}, {"newPassword", next}}), true);
}

void OnlineClient::mfaSetup(const std::string& password) {
    simplePost(impl_.get(), impl_->ep, Event::Kind::MfaSetupResult, "/account/mfa/totp/setup", obj({{"password", password}}), true,
               [](const json::Value& b, Event& ev) {
                   ev.mfaSecret = b["secret"].asString();
                   ev.mfaUri = b["uri"].asString();
               });
}

void OnlineClient::mfaEnable(const std::string& code) {
    simplePost(impl_.get(), impl_->ep, Event::Kind::MfaEnableResult, "/account/mfa/totp/enable", obj({{"code", code}}), true, readCodes);
}

void OnlineClient::mfaDisable(const std::string& password, const std::string& codeOrRecovery) {
    bool digits = codeOrRecovery.size() == 6 &&
                  std::all_of(codeOrRecovery.begin(), codeOrRecovery.end(), [](char c) { return c >= '0' && c <= '9'; });
    simplePost(impl_.get(), impl_->ep, Event::Kind::MfaDisableResult, "/account/mfa/totp/disable",
               obj({{"password", password}, {digits ? "code" : "recoveryCode", codeOrRecovery}}), true);
}

void OnlineClient::regenerateRecoveryCodes(const std::string& password, const std::string& code) {
    simplePost(impl_.get(), impl_->ep, Event::Kind::RecoveryCodesResult, "/account/mfa/recovery-codes",
               obj({{"password", password}, {"code", code}}), true, readCodes);
}

void OnlineClient::report(uint64_t gameId, const std::string& username, const std::string& category, const std::string& comment) {
    simplePost(impl_.get(), impl_->ep, Event::Kind::ReportResult, "/reports",
               obj({{"gameId", json::Value(gameId)}, {"reported", username}, {"category", category}, {"comment", comment}}), true);
}

// ---- account API ----
// CONTRACT STUBS (replaced by the net work package): each answers its result event with
// error "not_implemented".
namespace {
void notImplemented(OnlineClient::Impl* d, Event::Kind kind) {
    d->http([d, kind] {
        Event ev;
        ev.kind = kind;
        ev.error = "not_implemented";
        d->post(ev);
    });
}
}  // namespace

void OnlineClient::fetchMyGames(uint64_t, int, const GamesFilter&) { notImplemented(impl_.get(), Event::Kind::GamesResult); }
void OnlineClient::fetchGame(uint64_t) { notImplemented(impl_.get(), Event::Kind::GameDetailsResult); }
void OnlineClient::downloadPgn(uint64_t) { notImplemented(impl_.get(), Event::Kind::PgnResult); }
void OnlineClient::fetchSessions() { notImplemented(impl_.get(), Event::Kind::SessionsResult); }
void OnlineClient::revokeSession(int64_t) { notImplemented(impl_.get(), Event::Kind::SessionRevoked); }
void OnlineClient::setAcceptChallenges(bool) { notImplemented(impl_.get(), Event::Kind::PreferencesResult); }
void OnlineClient::changeEmail(const std::string&, const std::string&, const std::string&) {
    notImplemented(impl_.get(), Event::Kind::EmailChangeResult);
}
void OnlineClient::exportAccount(const std::string&, const std::string&) { notImplemented(impl_.get(), Event::Kind::AccountExportResult); }
void OnlineClient::deleteAccount(const std::string&, const std::string&) { notImplemented(impl_.get(), Event::Kind::AccountDeleted); }

// ---- realtime ----

void OnlineClient::connect() {
    Impl* d = impl_.get();
    ServerEndpoint e = d->ep;
    d->realtime([d, e] {
        d->rt.ep = e;
        d->rt.wanted = true;
        if (!d->rt.ws) {
            // Asked by the player: at once (never behind the backoff), with a fresh /info. A 503
            // now means a full server, whatever happened before.
            d->rt.attempt = 0;
            d->rt.nextAttempt = Clock::now();
            d->rt.info = Impl::Rt::Info();
            d->rt.restarting = false;
        }
    });
}

void OnlineClient::disconnect() {
    Impl* d = impl_.get();
    d->connectGen.fetch_add(1);
    d->rtCancel.cancel();
    d->realtime([d] {
        d->rt.wanted = false;
        d->dropSocket(1000);
        d->setState(ConnState::Offline);
    });
}

ConnState OnlineClient::state() const { return ConnState(impl_->connState.load()); }
int OnlineClient::pingMs() const { return impl_->ping.load(); }
double OnlineClient::serverNowMs() const { return localEpochMs() + impl_->clockOffset.load(); }

void OnlineClient::joinQueue(const std::string& category, bool rated) {
    Impl* d = impl_.get();
    d->realtime([d, category, rated] {
        pr::QueueJoin m;
        m.category = category;
        m.rated = rated;
        d->sendGame(m);
    });
}

void OnlineClient::leaveQueue() {
    Impl* d = impl_.get();
    d->realtime([d] { d->sendGame(pr::QueueLeave()); });
}

void OnlineClient::challenge(const std::string& username, int baseSec, int incSec, bool rated, int colorPref) {
    Impl* d = impl_.get();
    d->realtime([d, username, baseSec, incSec, rated, colorPref] {
        if (username.empty() || baseSec < 0 || baseSec > 65535 || incSec < 0 || incSec > 255 || colorPref < 0 || colorPref > 2) {
            d->localError("invalid_request");
            return;
        }
        pr::ChallengeCreate m;
        m.target = username;
        m.baseSec = uint16_t(baseSec);
        m.incSec = uint8_t(incSec);
        m.rated = rated;
        m.color = pr::ColorPref(colorPref);
        d->sendGame(m);
    });
}

void OnlineClient::createPrivateGame(int baseSec, int incSec, bool rated, int colorPref) {
    Impl* d = impl_.get();
    d->realtime([d, baseSec, incSec, rated, colorPref] {
        if (baseSec < 0 || baseSec > 65535 || incSec < 0 || incSec > 255 || colorPref < 0 || colorPref > 2) {
            d->localError("invalid_request");
            return;
        }
        pr::ChallengeCreate m;
        m.baseSec = uint16_t(baseSec);
        m.incSec = uint8_t(incSec);
        m.rated = rated;
        m.color = pr::ColorPref(colorPref);
        d->sendGame(m);
    });
}

void OnlineClient::joinPrivateGame(const std::string& code) {
    Impl* d = impl_.get();
    std::string c;
    for (char ch : code)
        if (ch != '-' && ch != ' ') c += char(std::toupper((unsigned char)ch));
    d->realtime([d, c] {
        pr::ChallengeJoinCode m;
        m.code = c;
        d->sendGame(m);
    });
}

void OnlineClient::acceptChallenge(uint32_t id) {
    Impl* d = impl_.get();
    d->realtime([d, id] {
        pr::ChallengeAccept m;
        m.id = id;
        d->sendGame(m);
    });
}

void OnlineClient::declineChallenge(uint32_t id) {
    Impl* d = impl_.get();
    d->realtime([d, id] {
        pr::ChallengeDecline m;
        m.id = id;
        d->sendGame(m);
    });
}

void OnlineClient::cancelChallenge(uint32_t id) {
    Impl* d = impl_.get();
    d->realtime([d, id] {
        pr::ChallengeCancel m;
        m.id = id;
        d->sendGame(m);
    });
}

void OnlineClient::sendMove(uint64_t gameId, int ply, uint16_t move, const std::string& fen, uint32_t thinkMs, bool drawOffer) {
    Impl* d = impl_.get();
    uint32_t posHash = positionDigest(fen);
    d->realtime([d, gameId, ply, move, posHash, thinkMs, drawOffer] {
        if (ply < 0 || ply > 0xFFFF) { d->localError("invalid_request"); return; }
        pr::Move m;
        m.game = gameId;
        m.ply = uint16_t(ply);
        m.move = move;
        m.posHash = posHash;
        m.thinkMs = thinkMs;
        m.drawOffer = drawOffer;
        if (!d->rt.ws || !d->rt.welcomed) { d->localError("offline"); return; }
        if (!pr::valid(m)) { d->localError("invalid_request"); return; }
        d->rt.pending = {gameId, ply, move};
        d->send(m);
    });
}

namespace {
template <class M> void gameCommand(OnlineClient::Impl* d, uint64_t gameId) {
    d->realtime([d, gameId] {
        M m;
        m.game = gameId;
        d->sendGame(m);
    });
}
}  // namespace

void OnlineClient::resign(uint64_t gameId) { gameCommand<pr::Resign>(impl_.get(), gameId); }
void OnlineClient::offerDraw(uint64_t gameId) { gameCommand<pr::DrawOffer>(impl_.get(), gameId); }
void OnlineClient::claimDraw(uint64_t gameId) { gameCommand<pr::DrawClaim>(impl_.get(), gameId); }
void OnlineClient::abortGame(uint64_t gameId) { gameCommand<pr::Abort>(impl_.get(), gameId); }
void OnlineClient::requestResync(uint64_t gameId) { gameCommand<pr::Resync>(impl_.get(), gameId); }

void OnlineClient::answerDraw(uint64_t gameId, bool accept) {
    Impl* d = impl_.get();
    d->realtime([d, gameId, accept] {
        pr::DrawAnswer m;
        m.game = gameId;
        m.accept = accept;
        d->sendGame(m);
    });
}

void OnlineClient::rematch(uint64_t gameId, bool accept) {
    Impl* d = impl_.get();
    d->realtime([d, gameId, accept] {
        pr::Rematch m;
        m.game = gameId;
        m.accept = accept;
        d->sendGame(m);
    });
}

void OnlineClient::sendGesture(uint64_t gameId, const Gesture& g) {
    Impl* d = impl_.get();
    if (d->connState.load() != int(ConnState::Online)) return;   // nothing kept for a reconnection
    bool wake;
    {
        std::lock_guard<std::mutex> lk(d->mu);
        // One already waiting has woken net-rt, which looks again when its bucket allows.
        wake = !d->gestureOut.pending;
        d->gestureOut.pending = true;
        d->gestureOut.game = gameId;
        d->gestureOut.g = g;
        if (wake) d->rtWake = true;
    }
    if (wake) d->rtCv.notify_one();
}

const OnlineGame* OnlineClient::currentGame() const { return impl_->hasView ? &impl_->view : nullptr; }

bool OnlineClient::poll(Event& out) {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->events.empty()) return false;
        out = std::move(impl_->events.front());
        impl_->events.pop_front();
    }
    switch (out.kind) {
    case Event::Kind::GameSnapshot:
    case Event::Kind::MoveMade:
    case Event::Kind::MoveRejected:
    case Event::Kind::GameEvent:
    case Event::Kind::GameEnd:
    case Event::Kind::RatingUpdate:
        if (out.game.id != 0) {
            impl_->view = out.game;
            impl_->hasView = true;
        }
        break;
    default:
        break;
    }
    return true;
}

OnlineClient& onlineClient() {
    static OnlineClient client;
    return client;
}

}  // namespace net
