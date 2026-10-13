// The online client's calls and answers that tests/net_tests.cpp leaves out (net/online_client.h),
// against loopback servers (plain HTTP and WebSocket, the insecureDev mode):
//   - the account calls that post a form: registration (the session's user name kept for the
//     next sign-in), the e-mails for verification and a forgotten password, the password change,
//     two-factor setup / enable / disable (a code or a recovery code) and recovery codes, reports;
//     what each sends and what its answer fills in;
//   - answers that are not what a call expects: a sign-in answered with text that is not JSON or
//     with a token that cannot be one, steps asked for out of turn (a second factor or a Google
//     user name with no sign-in under way), a refused logout that still signs the game out, a
//     server that announces another server id (its saved session is dropped), a game record whose
//     ids are decimal text and whose promotions are a bishop and a rook;
//   - the realtime lobby and game messages the player sends (accept / decline a challenge, join a
//     private game by code, leave the queue, offer / answer / claim a draw, abort, resync, rematch)
//     and the server messages the client reads (a challenge received, every game event, the end,
//     the rating update), checked on the server's side and in the client's events;
//   - the refusals that end the realtime connection for good: another subprotocol at the upgrade,
//     another protocol version in Welcome, the close codes of an unverified e-mail address and of a
//     connection replaced by another one.
#include "test.h"
#include "headless.h"
#include "http_fake.h"
#include "net_gaps_helpers.h"
#include "net/credential_store.h"
#include "net/json.h"
#include "net/net_sys.h"
#include "net/online_client.h"
#include "net/protocol_gen.h"
#include "net/transport.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pr = net::proto;
using Kind = net::Event::Kind;
using net::json::Value;

namespace {

const std::string kToken = "sct_" + std::string(43, 'G');

fakehttp::Reply jsonAnswer(int status, const std::string& body) {
    fakehttp::Reply r;
    r.status = status;
    r.headers.emplace_back("Content-Type", "application/json");
    r.body = body;
    return r;
}

Value bodyOf(const fakehttp::Request& r) {
    Value v;
    net::json::parse(r.body, v);
    return v;
}

// Drains the client's events until one of `kind` (matching pred) comes; the others are dropped, or
// kept in 'seen' when given.
bool waitEvent(net::OnlineClient& c, Kind kind, net::Event& out, int timeoutMs = 10000,
               const std::function<bool(const net::Event&)>& pred = nullptr, std::vector<net::Event>* seen = nullptr) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        net::Event ev;
        while (c.poll(ev)) {
            if (seen) seen->push_back(ev);
            if (ev.kind == kind && (!pred || pred(ev))) {
                out = ev;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool waitState(net::OnlineClient& c, net::ConnState s, net::Event& out, int timeoutMs = 10000) {
    return waitEvent(c, Kind::ConnectionChanged, out, timeoutMs, [s](const net::Event& e) { return e.state == s; });
}

// A private folder (the credential store wants one only its owner reads) and its credential file,
// signed in as "alice" on 'origin' when a token is given.
struct Credentials {
    headless::TempDir dir;
    std::string path;
    explicit Credentials(const char* tag) : dir(tag), path(dir.file("gaps.credentials")) {
        std::error_code ec;
        std::filesystem::permissions(std::filesystem::u8path(dir.path), std::filesystem::perms::owner_all, ec);
    }
    void signIn(const std::string& origin, const std::string& serverId) const {
        net::CredentialStore s(path);
        net::Credential cr;
        cr.origin = origin;
        cr.username = "alice";
        cr.token = kToken;
        cr.serverId = serverId;
        s.put(cr);
    }
};

// A client on a scripted HTTP server (http_fake.h).
struct HttpRig {
    Credentials creds;
    fakehttp::Server srv;
    net::ServerEndpoint ep;
    std::unique_ptr<net::OnlineClient> c;

    HttpRig(const char* tag, fakehttp::Server::Handler h, bool signedIn) : creds(tag), srv(std::move(h)) {
        ep.host = "127.0.0.1";
        ep.apiPort = srv.port();
        ep.insecureDev = true;
        if (signedIn) creds.signIn(ep.origin(), "srv-gaps");
        c = std::make_unique<net::OnlineClient>();
        c->setCredentialsFile(creds.path);
        c->setServer(ep);
    }
    ~HttpRig() { c.reset(); }
    net::Event wait(Kind k) {
        net::Event ev;
        const bool got = waitEvent(*c, k, ev);
        CHECK(got);
        return ev;
    }
    fakehttp::Request last() const {
        std::vector<fakehttp::Request> r = srv.requests();
        return r.empty() ? fakehttp::Request() : r.back();
    }
    size_t count() const { return srv.requests().size(); }
};

}  // namespace

// Registration: the body, "ready" (no e-mail confirmation) against a verification mail, the user
// name kept for the next sign-in without a session, a refusal; then the two e-mail requests,
// which go without a session.
TEST(net_gaps_register_and_mail_requests) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    std::atomic<int> registerMode{0};
    HttpRig r("gaps-register", [&](const fakehttp::Request& q) {
        if (q.path == "/api/v1/auth/register") {
            if (registerMode.load() == 0) return jsonAnswer(201, R"({"status":"ready"})");
            if (registerMode.load() == 1) return jsonAnswer(201, R"({"status":"verification_sent"})");
            return jsonAnswer(409, R"({"error":"username_taken","message":"taken"})");
        }
        if (q.path == "/api/v1/auth/verify-email/resend" || q.path == "/api/v1/auth/password/forgot")
            return jsonAnswer(202, R"({"status":"sent"})");
        return jsonAnswer(404, R"({"error":"not_found"})");
    }, false);
    REQUIRE(r.srv.ok());
    CHECK(r.c->savedUsername().empty());

    r.c->registerAccount("bob", "b@example.org", "s3cret");
    net::Event ev = r.wait(Kind::RegisterResult);
    CHECK(ev.ok);
    CHECK(ev.account.emailVerified);   // "ready": no confirmation needed
    CHECK_EQ(ev.account.username, std::string("bob"));
    fakehttp::Request q = r.last();
    CHECK_EQ(q.method, std::string("POST"));
    Value b = bodyOf(q);
    CHECK_EQ(b["username"].asString(), std::string("bob"));
    CHECK_EQ(b["email"].asString(), std::string("b@example.org"));
    CHECK_EQ(b["password"].asString(), std::string("s3cret"));
    CHECK(!q.has("authorization"));
    // The name is kept for the sign-in page, without a session.
    CHECK(gaps::eventually([&] { return r.c->savedUsername() == "bob"; }));
    CHECK(!r.c->hasSavedSession());

    registerMode = 1;
    r.c->registerAccount("carol", "c@example.org", "pw");
    ev = r.wait(Kind::RegisterResult);
    CHECK(ev.ok);
    CHECK(!ev.account.emailVerified);   // a verification mail went out
    CHECK_EQ(r.c->savedUsername(), std::string("carol"));   // the latest account's name

    registerMode = 2;
    r.c->registerAccount("bob", "b@example.org", "pw");
    ev = r.wait(Kind::RegisterResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("username_taken"));
    CHECK(!ev.account.emailVerified);

    r.c->resendVerification("c@example.org");
    ev = r.wait(Kind::VerificationResent);
    CHECK(ev.ok);
    q = r.last();
    CHECK_EQ(q.path, std::string("/api/v1/auth/verify-email/resend"));
    CHECK_EQ(bodyOf(q)["email"].asString(), std::string("c@example.org"));
    CHECK(!q.has("authorization"));

    r.c->forgotPassword("b@example.org");
    ev = r.wait(Kind::PasswordResetRequested);
    CHECK(ev.ok);
    q = r.last();
    CHECK_EQ(q.path, std::string("/api/v1/auth/password/forgot"));
    CHECK_EQ(bodyOf(q)["email"].asString(), std::string("b@example.org"));
}

// The calls of a signed-in player: password change, two-factor setup, enable (its recovery codes),
// disable with a code or a recovery code, new recovery codes, and a report; a refusal fills in
// nothing. Without a session they are "unauthorized" and nothing is sent.
TEST(net_gaps_password_two_factor_and_report) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    std::atomic<bool> refuseEnable{false};
    HttpRig r("gaps-mfa", [&](const fakehttp::Request& q) {
        if (q.get("authorization") != "Bearer " + kToken) return jsonAnswer(401, R"({"error":"invalid_token"})");
        if (q.path == "/api/v1/account/password") return jsonAnswer(200, R"({"status":"changed"})");
        if (q.path == "/api/v1/account/mfa/totp/setup")
            return jsonAnswer(200, R"({"secret":"JBSWY3DPEHPK3PXP","uri":"otpauth://totp/Scacelith:alice?secret=JBSWY3DPEHPK3PXP"})");
        if (q.path == "/api/v1/account/mfa/totp/enable") {
            if (refuseEnable.load()) return jsonAnswer(400, R"({"error":"invalid_code","recoveryCodes":["zzzz-zzzz-zz"]})");
            return jsonAnswer(200, R"({"recoveryCodes":["abcd-efgh-12","ijkl-mnop-34"]})");
        }
        if (q.path == "/api/v1/account/mfa/totp/disable") return jsonAnswer(200, R"({"status":"disabled"})");
        if (q.path == "/api/v1/account/mfa/recovery-codes") return jsonAnswer(200, R"({"recoveryCodes":["qrst-uvwx-56"]})");
        if (q.path == "/api/v1/reports") return jsonAnswer(201, R"({"status":"received"})");
        return jsonAnswer(404, R"({"error":"not_found"})");
    }, true);
    REQUIRE(r.srv.ok());
    REQUIRE(r.c->hasSavedSession());

    r.c->changePassword("old pw", "new pw");
    net::Event ev = r.wait(Kind::PasswordChanged);
    CHECK(ev.ok);
    fakehttp::Request q = r.last();
    CHECK_EQ(q.path, std::string("/api/v1/account/password"));
    CHECK_EQ(q.get("authorization"), "Bearer " + kToken);
    CHECK_EQ(bodyOf(q)["currentPassword"].asString(), std::string("old pw"));
    CHECK_EQ(bodyOf(q)["newPassword"].asString(), std::string("new pw"));

    r.c->mfaSetup("pw");
    ev = r.wait(Kind::MfaSetupResult);
    CHECK(ev.ok);
    CHECK_EQ(ev.mfaSecret, std::string("JBSWY3DPEHPK3PXP"));
    CHECK(ev.mfaUri.compare(0, 15, "otpauth://totp/") == 0);
    CHECK_EQ(bodyOf(r.last())["password"].asString(), std::string("pw"));

    r.c->mfaEnable("123456");
    ev = r.wait(Kind::MfaEnableResult);
    CHECK(ev.ok);
    REQUIRE(ev.recoveryCodes.size() == 2);
    CHECK_EQ(ev.recoveryCodes[0], std::string("abcd-efgh-12"));
    CHECK_EQ(ev.recoveryCodes[1], std::string("ijkl-mnop-34"));
    CHECK_EQ(bodyOf(r.last())["code"].asString(), std::string("123456"));

    // A refusal: its error, and nothing read from its body.
    refuseEnable = true;
    r.c->mfaEnable("000000");
    ev = r.wait(Kind::MfaEnableResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("invalid_code"));
    CHECK(ev.recoveryCodes.empty());

    // Six digits go as "code", anything else as "recoveryCode".
    r.c->mfaDisable("pw", "654321");
    ev = r.wait(Kind::MfaDisableResult);
    CHECK(ev.ok);
    Value b = bodyOf(r.last());
    CHECK_EQ(b["password"].asString(), std::string("pw"));
    CHECK_EQ(b["code"].asString(), std::string("654321"));
    CHECK(!b.has("recoveryCode"));
    r.c->mfaDisable("pw", "abcd-efgh-12");
    ev = r.wait(Kind::MfaDisableResult);
    CHECK(ev.ok);
    b = bodyOf(r.last());
    CHECK_EQ(b["recoveryCode"].asString(), std::string("abcd-efgh-12"));
    CHECK(!b.has("code"));

    r.c->regenerateRecoveryCodes("pw", "111111");
    ev = r.wait(Kind::RecoveryCodesResult);
    CHECK(ev.ok);
    REQUIRE(ev.recoveryCodes.size() == 1);
    CHECK_EQ(ev.recoveryCodes[0], std::string("qrst-uvwx-56"));
    b = bodyOf(r.last());
    CHECK_EQ(r.last().path, std::string("/api/v1/account/mfa/recovery-codes"));
    CHECK_EQ(b["code"].asString(), std::string("111111"));

    r.c->report(812, "bob", "cheating", "every move instant");
    ev = r.wait(Kind::ReportResult);
    CHECK(ev.ok);
    b = bodyOf(r.last());
    CHECK_EQ(r.last().path, std::string("/api/v1/reports"));
    CHECK_EQ(b["gameId"].asInt(), int64_t(812));
    CHECK_EQ(b["reported"].asString(), std::string("bob"));
    CHECK_EQ(b["category"].asString(), std::string("cheating"));
    CHECK_EQ(b["comment"].asString(), std::string("every move instant"));

    // Signed out: refused here, nothing sent.
    r.c->logout();
    CHECK(r.wait(Kind::LogoutResult).kind == Kind::LogoutResult);
    const size_t before = r.count();
    r.c->changePassword("a", "b");
    ev = r.wait(Kind::PasswordChanged);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("unauthorized"));
    r.c->mfaSetup("pw");
    ev = r.wait(Kind::MfaSetupResult);
    CHECK_EQ(ev.error, std::string("unauthorized"));
    CHECK(ev.mfaSecret.empty());
    CHECK_EQ(r.count(), before);
}

// Sign-in answers the client cannot use, and steps asked for out of turn.
TEST(net_gaps_sign_in_answers_refused) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    std::atomic<int> loginMode{0};
    HttpRig r("gaps-login", [&](const fakehttp::Request& q) {
        if (q.path == "/api/v1/auth/login") {
            if (loginMode.load() == 0) return jsonAnswer(200, "<html>a captive portal</html>");
            return jsonAnswer(200, R"({"token":"short","user":{"id":8,"username":"mallory"}})");
        }
        if (q.path == "/api/v1/auth/logout") return jsonAnswer(401, R"({"error":"invalid_token"})");
        return jsonAnswer(404, R"({"error":"not_found"})");
    }, true);
    REQUIRE(r.srv.ok());
    REQUIRE(r.c->hasSavedSession());

    // A success whose body is not JSON: the session saved before stays.
    r.c->login("mallory", "pw");
    net::Event ev = r.wait(Kind::LoginResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("bad_response"));
    CHECK(r.c->hasSavedSession());

    // A token that cannot be one (too short): nothing saved.
    loginMode = 1;
    r.c->login("mallory", "pw");
    ev = r.wait(Kind::LoginResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("bad_response"));
    CHECK(r.c->hasSavedSession());
    CHECK_EQ(r.c->savedUsername(), std::string("alice"));

    // A second factor, or a Google user name, with no sign-in asking for it: refused here.
    const size_t before = r.count();
    r.c->loginMfa("123456");
    ev = r.wait(Kind::LoginResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("invalid_mfa_token"));
    r.c->completeSso("alice");
    ev = r.wait(Kind::LoginResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("sso_expired"));
    CHECK_EQ(r.count(), before);

    // A session the server no longer knows: the logout still signs the game out, and says so.
    r.c->logout();
    ev = r.wait(Kind::LogoutResult);
    CHECK(ev.ok);
    CHECK(ev.error.empty());
    CHECK(!r.c->hasSavedSession());
    CHECK_EQ(r.last().path, std::string("/api/v1/auth/logout"));
}

// /info announcing another server id than the one the session was saved with: that session
// belongs to another server and is dropped (the user name stays for the sign-in page).
TEST(net_gaps_info_from_another_server_drops_the_session) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    HttpRig r("gaps-info", [&](const fakehttp::Request& q) {
        if (q.path == "/api/v1/info")
            return jsonAnswer(200, "{\"name\":\"Reinstalled\",\"serverId\":\"srv-other\",\"protocol\":{\"min\":1,\"max\":1,\"schema\":\"0x1234abcd\"},"
                                   "\"registration\":false,\"categories\":[{\"id\":\"3+2\",\"baseSec\":180,\"incSec\":2},"
                                   "{\"id\":\"far-too-long\",\"baseSec\":60,\"incSec\":0}]}");
        return jsonAnswer(404, R"({"error":"not_found"})");
    }, true);
    REQUIRE(r.srv.ok());
    REQUIRE(r.c->hasSavedSession());
    r.c->fetchServerInfo();
    net::Event ev = r.wait(Kind::ServerInfoResult);
    CHECK(ev.ok);
    CHECK_EQ(ev.info.serverId, std::string("srv-other"));
    CHECK_EQ(ev.info.fingerprint, 0x1234abcdu);          // a schema written as hexadecimal text
    CHECK(!ev.info.registrationOpen);                     // a boolean instead of "open"
    CHECK_EQ(ev.info.categories.size(), size_t(1));       // an id the protocol cannot carry is left out
    CHECK(!r.c->hasSavedSession());
    CHECK_EQ(r.c->savedUsername(), std::string("alice"));
}

// A game record whose ids are decimal text, with promotions to a bishop and a rook; a PGN of game 0
// is refused without a request.
TEST(net_gaps_game_record_text_ids_and_promotions) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    HttpRig r("gaps-game", [&](const fakehttp::Request& q) {
        if (q.path == "/api/v1/games/812")
            return jsonAnswer(200, R"({"id":"812","category":"custom","rated":false,"timeControl":"300+3",
              "white":{"name":"alice","rating":1500},"black":{"name":"bob","rating":1510},"you":"white",
              "status":3,"reason":13,"result":"1/2-1/2","rematchOf":"700",
              "moves":[{"uci":"b7b8b"},{"uci":"g2g1r","spentMs":1200,"clockMs":299000}]})");
        return jsonAnswer(404, R"({"error":"not_found"})");
    }, true);
    REQUIRE(r.srv.ok());
    r.c->fetchGame(812);
    net::Event ev = r.wait(Kind::GameDetailsResult);
    CHECK(ev.ok);
    const net::GameDetails& d = ev.gameDetails;
    CHECK_EQ(d.id, uint64_t(812));
    CHECK_EQ(d.rematchOf, uint64_t(700));
    CHECK_EQ(d.baseMs, int64_t(300000));                  // from "timeControl"
    CHECK_EQ(d.incMs, int64_t(3000));
    CHECK_EQ(d.you, 0);
    CHECK_EQ(d.plies, 2);                                 // no "plies": the moves count
    REQUIRE(d.moves.size() == 2);
    CHECK_EQ(d.moves[0].move, net::packMove(49, 57, 3));  // b7-b8=B
    CHECK_EQ(d.moves[0].spentMs, int64_t(-1));
    CHECK_EQ(d.moves[1].move, net::packMove(14, 6, 4));   // g2-g1=R
    CHECK_EQ(d.moves[1].clockMs, int64_t(299000));

    const size_t before = r.count();
    r.c->downloadPgn(0);
    ev = r.wait(Kind::PgnResult);
    CHECK(!ev.ok);
    CHECK_EQ(ev.error, std::string("invalid_game_id"));
    CHECK_EQ(r.count(), before);
}

// =============================================================================================
// Realtime
// =============================================================================================

namespace {

// The dedicated server's /api/v1/info and /ws on 127.0.0.1: Hello is answered with Welcome (of the
// protocol version 'welcomeProto'), every later client message is kept, and the test sends the
// server's messages and close frames on the open WebSocket.
class RtServer {
public:
    std::atomic<uint16_t> welcomeProto{pr::kProtocolVersion};
    std::atomic<int> upgrades{0}, hellos{0};

    RtServer() : tcp_([this](gaps::Conn& c) { serve(c); }) {}
    bool ok() const { return tcp_.ok(); }
    uint16_t port() const { return tcp_.port(); }
    void setSubprotocol(const std::string& s) {
        std::lock_guard<std::mutex> lk(mu_);
        subprotocol_ = s;
    }

    // The client messages received after the Hellos, in order.
    std::vector<std::string> received() {
        std::lock_guard<std::mutex> lk(mu_);
        return got_;
    }
    // Waits for the next client message of M's type from index 'from' on; 'from' moves past it.
    template <class M> bool next(M& out, size_t& from, int ms = 5000) {
        const auto end = gaps::Clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            std::vector<std::string> got = received();
            for (size_t i = from; i < got.size(); ++i) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(got[i].data());
                pr::MsgType t;
                if (pr::peekType(p, got[i].size(), t) && t == M::kType) {
                    from = i + 1;
                    return pr::decode(p, got[i].size(), out);
                }
            }
            if (gaps::Clock::now() >= end) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    template <class M> bool send(const M& m) {
        std::vector<uint8_t> b;
        pr::encode(m, b);
        std::lock_guard<std::mutex> lk(mu_);
        return ws_ && ws_->sendAll(gaps::serverFrame(0x2, std::string(b.begin(), b.end())));
    }
    bool sendClose(uint16_t code) {
        std::lock_guard<std::mutex> lk(mu_);
        return ws_ && ws_->sendAll(gaps::serverFrame(0x8, gaps::closePayload(code)));
    }
    bool open() {
        std::lock_guard<std::mutex> lk(mu_);
        return ws_ != nullptr;
    }

private:
    std::mutex mu_;
    std::string subprotocol_ = pr::kWsSubprotocol;
    gaps::Conn* ws_ = nullptr;
    std::vector<std::string> got_;
    gaps::TcpServer tcp_;   // last: its handlers use the members above

    void serve(gaps::Conn& c) {
        std::string head;
        if (!c.readHead(head)) return;
        const std::string path = gaps::requestPath(head);
        if (path == "/api/v1/info") {
            char info[512];
            std::snprintf(info, sizeof info,
                          "{\"name\":\"Gaps\",\"serverId\":\"srv-gaps\",\"protocol\":{\"min\":%u,\"max\":%u,\"subprotocol\":\"%s\"},"
                          "\"wsPath\":\"/ws\",\"registration\":\"open\",\"categories\":[{\"id\":\"3+2\",\"baseSec\":180,\"incSec\":2}]}",
                          unsigned(pr::kProtocolVersion), unsigned(pr::kProtocolVersion), pr::kWsSubprotocol);
            c.sendAll(gaps::httpAnswer(200, info));
            return;
        }
        if (path != "/ws") {
            c.sendAll(gaps::httpAnswer(404, "{\"error\":\"not_found\"}"));
            return;
        }
        ++upgrades;
        std::string proto;
        {
            std::lock_guard<std::mutex> lk(mu_);
            proto = subprotocol_;
        }
        if (!c.sendAll(gaps::wsUpgradeAnswer(head, proto, "Scacelith-Server-Id: srv-gaps\r\n"))) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ws_ = &c;
        }
        for (;;) {
            int op = 0;
            std::string payload;
            if (!gaps::readClientFrame(c, op, payload, 200)) {
                if (c.peerClosed || c.stopping()) break;
                continue;
            }
            if (op == 0x8) {   // the client closes: answered, then the connection ends
                std::lock_guard<std::mutex> lk(mu_);
                c.sendAll(gaps::serverFrame(0x8, payload.substr(0, 2)));
                break;
            }
            if (op != 0x2 || payload.empty()) continue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.data());
            pr::MsgType t;
            if (!pr::peekType(p, payload.size(), t)) continue;
            if (t == pr::MsgType::Hello) {
                ++hellos;
                pr::Welcome w;
                w.proto = welcomeProto.load();
                w.minor = pr::kMinor;
                w.caps = pr::kCaps;
                w.serverTime = net::sock::epochMs();
                w.userId = 7;
                w.username = "alice";
                w.serverName = "Gaps";
                w.heartbeatMs = 15000;
                w.maxMsgPerSec = 20;
                w.msgBurst = 40;
                send(w);
                continue;
            }
            std::lock_guard<std::mutex> lk(mu_);
            got_.push_back(payload);
        }
        std::lock_guard<std::mutex> lk(mu_);
        ws_ = nullptr;
    }
};

// A client signed in on an RtServer.
struct RtRig {
    Credentials creds;
    RtServer srv;
    net::ServerEndpoint ep;
    std::unique_ptr<net::OnlineClient> c;
    explicit RtRig(const char* tag) : creds(tag) {
        ep.host = "127.0.0.1";
        ep.apiPort = srv.port();
        ep.insecureDev = true;
        creds.signIn(ep.origin(), "srv-gaps");
        c = std::make_unique<net::OnlineClient>();
        c->setCredentialsFile(creds.path);
        c->setServer(ep);
    }
    ~RtRig() { c.reset(); }
    bool online() {
        net::Event ev;
        c->connect();
        return waitEvent(*c, Kind::Welcome, ev) && gaps::eventually([&] { return srv.open(); });
    }
};

pr::GameSnapshot snapshot(uint64_t game, uint32_t gseq) {
    pr::GameSnapshot g;
    g.game = game;
    g.gseq = gseq;
    g.category = "3+2";
    g.baseMs = 180000;
    g.incMs = 2000;
    g.rated = true;
    g.white = {7, "alice", 1500, false};
    g.black = {9, "carol", 1610, true};
    g.you = pr::Color::White;
    g.running = pr::Color::None;
    g.whiteMs = g.blackMs = 180000;
    g.serverTime = net::sock::epochMs();
    g.drawOffer = pr::Color::None;
    g.rematch = pr::Color::None;
    g.whiteConnected = g.blackConnected = true;
    g.firstMoveMs = 30000;
    g.autoPress = true;
    return g;
}

pr::GameEvent gameEvent(uint64_t game, uint32_t gseq, pr::GameEventKind kind, pr::Color color, uint32_t arg = 0) {
    pr::GameEvent e;
    e.game = game;
    e.gseq = gseq;
    e.kind = kind;
    e.color = color;
    e.arg = arg;
    return e;
}

}  // namespace

// The lobby: a challenge received (its fields), accepted, declined; a private game joined by its
// code as the player types it; the queue left; challenge requests refused before they go.
TEST(net_gaps_realtime_lobby) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    RtRig r("gaps-lobby");
    REQUIRE(r.srv.ok());
    REQUIRE(r.online());
    CHECK_EQ(r.srv.hellos.load(), 1);

    pr::ChallengeReceived cr;
    cr.id = 5;
    cr.from = {9, "carol", 1610, true};
    cr.baseSec = 300;
    cr.incSec = 3;
    cr.rated = true;
    cr.yourColor = pr::ColorPref::Black;
    cr.expiresMs = 60000;
    REQUIRE(r.srv.send(cr));
    net::Event ev;
    REQUIRE(waitEvent(*r.c, Kind::ChallengeReceived, ev));
    CHECK(ev.ok);
    CHECK_EQ(ev.challengeId, 5u);
    CHECK_EQ(ev.challenger.userId, 9u);
    CHECK_EQ(ev.challenger.name, std::string("carol"));
    CHECK_EQ(ev.challenger.rating, 1610);
    CHECK(ev.challenger.provisional);
    CHECK_EQ(ev.challengeBaseSec, 300);
    CHECK_EQ(ev.challengeIncSec, 3);
    CHECK(ev.challengeRated);
    CHECK_EQ(ev.challengeColor, int(pr::ColorPref::Black));
    CHECK_EQ(ev.challengeExpiresMs, 60000u);

    size_t at = 0;
    r.c->acceptChallenge(5);
    pr::ChallengeAccept accept;
    CHECK(r.srv.next(accept, at));
    CHECK_EQ(accept.id, 5u);
    r.c->declineChallenge(6);
    pr::ChallengeDecline decline;
    CHECK(r.srv.next(decline, at));
    CHECK_EQ(decline.id, 6u);
    // The code as typed, with its dash and a space, in lower case: sent bare, in upper case.
    r.c->joinPrivateGame("k7q2-m9 xh");
    pr::ChallengeJoinCode join;
    CHECK(r.srv.next(join, at));
    CHECK_EQ(join.code, std::string("K7Q2M9XH"));
    r.c->leaveQueue();
    pr::QueueLeave leave;
    CHECK(r.srv.next(leave, at));
    CHECK(leave.seq > join.seq);   // numbered in order

    // Terms the protocol cannot carry: refused here, with the request's number.
    const uint32_t bad = r.c->challenge("", 300, 3, false, 0);
    CHECK(waitEvent(*r.c, Kind::ServerError, ev, 5000, [&](const net::Event& e) { return e.request == bad; }));
    CHECK_EQ(ev.error, std::string("invalid_request"));
    const uint32_t badColor = r.c->challenge("bob", 300, 3, false, 3);
    CHECK(waitEvent(*r.c, Kind::ServerError, ev, 5000, [&](const net::Event& e) { return e.request == badColor; }));
    CHECK_EQ(ev.error, std::string("invalid_request"));
    const uint32_t badPrivate = r.c->createPrivateGame(-1, 3, false, 0);
    CHECK(waitEvent(*r.c, Kind::ServerError, ev, 5000, [&](const net::Event& e) { return e.request == badPrivate; }));
    CHECK_EQ(ev.error, std::string("invalid_request"));
    const uint32_t badInc = r.c->createPrivateGame(300, 256, false, 0);
    CHECK(waitEvent(*r.c, Kind::ServerError, ev, 5000, [&](const net::Event& e) { return e.request == badInc; }));
    CHECK_EQ(ev.error, std::string("invalid_request"));
    // A code the protocol refuses (too short) is "invalid_request" too, and is not sent.
    r.c->joinPrivateGame("ab");
    CHECK(waitEvent(*r.c, Kind::ServerError, ev, 5000));
    CHECK_EQ(ev.error, std::string("invalid_request"));
    CHECK(!r.srv.next(join, at, 200));
}

// A game: every game event applied to the client's state in gseq order, the player's draw, abort,
// resync and rematch messages, the end and the rating update.
TEST(net_gaps_realtime_game_events_and_commands) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    RtRig r("gaps-game-rt");
    REQUIRE(r.srv.ok());
    REQUIRE(r.online());
    const uint64_t G = 90;
    REQUIRE(r.srv.send(snapshot(G, 1)));
    net::Event ev;
    REQUIRE(waitEvent(*r.c, Kind::GameSnapshot, ev));
    CHECK_EQ(ev.game.id, G);
    CHECK(ev.game.whiteConnected && ev.game.blackConnected);

    // Black's connection drops (30 s of grace), comes back.
    r.srv.send(gameEvent(G, 2, pr::GameEventKind::PlayerDisconnected, pr::Color::Black, 30000));
    REQUIRE(waitEvent(*r.c, Kind::GameEvent, ev));
    CHECK_EQ(ev.gameEventKind, int(pr::GameEventKind::PlayerDisconnected));
    CHECK_EQ(ev.color, 1);
    CHECK_EQ(ev.arg, 30000u);
    CHECK(!ev.game.blackConnected);
    CHECK(ev.game.whiteConnected);
    CHECK_EQ(ev.game.graceMs, 30000u);
    r.srv.send(gameEvent(G, 3, pr::GameEventKind::PlayerReconnected, pr::Color::Black));
    REQUIRE(waitEvent(*r.c, Kind::GameEvent, ev));
    CHECK(ev.game.blackConnected);
    // White's own drop as the opponent sees it, in the same way.
    r.srv.send(gameEvent(G, 4, pr::GameEventKind::PlayerDisconnected, pr::Color::White, 20000));
    REQUIRE(waitEvent(*r.c, Kind::GameEvent, ev));
    CHECK(!ev.game.whiteConnected);
    r.srv.send(gameEvent(G, 5, pr::GameEventKind::PlayerReconnected, pr::Color::White));
    REQUIRE(waitEvent(*r.c, Kind::GameEvent, ev));
    CHECK(ev.game.whiteConnected);

    // Black offers a draw, the player declines through the server.
    r.srv.send(gameEvent(G, 6, pr::GameEventKind::DrawOffered, pr::Color::Black));
    REQUIRE(waitEvent(*r.c, Kind::GameEvent, ev));
    CHECK_EQ(ev.game.drawOfferBy, 1);
    size_t at = 0;
    r.c->answerDraw(G, false);
    pr::DrawAnswer answer;
    CHECK(r.srv.next(answer, at));
    CHECK_EQ(answer.game, G);
    CHECK(!answer.accept);
    r.srv.send(gameEvent(G, 7, pr::GameEventKind::DrawDeclined, pr::Color::White));
    REQUIRE(waitEvent(*r.c, Kind::GameEvent, ev));
    CHECK_EQ(ev.gameEventKind, int(pr::GameEventKind::DrawDeclined));
    CHECK_EQ(ev.game.drawOfferBy, 2);
    // An event the state already holds (gseq 7 again) is not shown twice.
    r.srv.send(gameEvent(G, 7, pr::GameEventKind::DrawOffered, pr::Color::Black));

    r.c->offerDraw(G);
    pr::DrawOffer offer;
    CHECK(r.srv.next(offer, at));
    CHECK_EQ(offer.game, G);
    r.c->answerDraw(G, true);
    CHECK(r.srv.next(answer, at));
    CHECK(answer.accept);
    r.c->claimDraw(G);
    pr::DrawClaim claim;
    CHECK(r.srv.next(claim, at));
    CHECK_EQ(claim.game, G);
    r.c->abortGame(G);
    pr::Abort abort;
    CHECK(r.srv.next(abort, at));
    CHECK_EQ(abort.game, G);
    r.c->requestResync(G);
    pr::Resync resync;
    CHECK(r.srv.next(resync, at));
    CHECK_EQ(resync.game, G);
    CHECK(answer.seq < claim.seq && claim.seq < abort.seq && abort.seq < resync.seq);

    pr::GameEnd end;
    end.game = G;
    end.gseq = 8;
    end.status = pr::GameStatus::Draw;
    end.reason = pr::EndReason::Agreement;
    end.whiteMs = 170000;
    end.blackMs = 165000;
    end.serverTime = net::sock::epochMs();
    r.srv.send(end);
    std::vector<net::Event> seen;
    REQUIRE(waitEvent(*r.c, Kind::GameEnd, ev, 10000, nullptr, &seen));
    for (const net::Event& e : seen) CHECK(e.kind != Kind::GameEvent);   // the repeated gseq 7
    CHECK_EQ(ev.game.status, int(pr::GameStatus::Draw));
    CHECK_EQ(ev.game.reason, int(pr::EndReason::Agreement));
    CHECK_EQ(ev.game.running, 2);
    CHECK_EQ(ev.game.whiteMs, int64_t(170000));

    // The rematch: Black offers, the player declines, the server says so.
    r.srv.send(gameEvent(G, 9, pr::GameEventKind::RematchOffered, pr::Color::Black));
    REQUIRE(waitEvent(*r.c, Kind::GameEvent, ev));
    CHECK_EQ(ev.game.rematchBy, 1);
    r.c->rematch(G, false);
    pr::Rematch rematch;
    CHECK(r.srv.next(rematch, at));
    CHECK_EQ(rematch.game, G);
    CHECK(!rematch.accept);
    r.srv.send(gameEvent(G, 10, pr::GameEventKind::RematchDeclined, pr::Color::White));
    REQUIRE(waitEvent(*r.c, Kind::GameEvent, ev));
    CHECK_EQ(ev.gameEventKind, int(pr::GameEventKind::RematchDeclined));
    CHECK_EQ(ev.game.rematchBy, 2);

    pr::RatingUpdate ru;
    ru.game = G;
    ru.category = "3+2";
    ru.white = {1500, 1508, 12, false};
    ru.black = {1610, 1602, 9, true};
    r.srv.send(ru);
    REQUIRE(waitEvent(*r.c, Kind::RatingUpdate, ev));
    CHECK_EQ(ev.gameId, G);
    CHECK_EQ(ev.queueCategory, std::string("3+2"));
    CHECK_EQ(ev.ratingWhite.before, 1500);
    CHECK_EQ(ev.ratingWhite.after, 1508);
    CHECK_EQ(ev.ratingWhite.games, 12);
    CHECK(!ev.ratingWhite.provisional);
    CHECK_EQ(ev.ratingBlack.after, 1602);
    CHECK(ev.ratingBlack.provisional);

    // An event of another game reaches the player without touching the game shown.
    r.srv.send(gameEvent(G + 1, 1, pr::GameEventKind::RematchOffered, pr::Color::White));
    REQUIRE(waitEvent(*r.c, Kind::GameEvent, ev));
    CHECK_EQ(ev.gameId, G + 1);
    CHECK_EQ(ev.game.id, G);
    CHECK_EQ(ev.game.rematchBy, 2);
}

// Refusals that end the realtime connection for good (no reconnection): another subprotocol in
// the upgrade's answer, close 4011 (the e-mail address is not verified), close 4007 (another
// connection of the account took over), a Welcome of another protocol version.
TEST(net_gaps_realtime_refusals) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    RtRig r("gaps-refusals");
    REQUIRE(r.srv.ok());
    net::Event ev;

    r.srv.setSubprotocol("other.rt9");
    r.c->connect();
    REQUIRE(waitState(*r.c, net::ConnState::Offline, ev));
    CHECK_EQ(ev.error, std::string("subprotocol"));
    CHECK_EQ(r.srv.hellos.load(), 0);
    r.srv.setSubprotocol(pr::kWsSubprotocol);

    REQUIRE(r.online());
    r.srv.sendClose(pr::CloseCode::EmailUnverified);
    REQUIRE(waitState(*r.c, net::ConnState::Unauthorized, ev));
    CHECK_EQ(ev.error, std::string("email_unverified"));
    CHECK(r.c->hasSavedSession());   // the session is good: the address is what lacks

    REQUIRE(r.online());
    r.srv.sendClose(pr::CloseCode::Replaced);
    REQUIRE(waitState(*r.c, net::ConnState::Offline, ev));
    CHECK_EQ(ev.error, std::string("replaced"));
    const int upgrades = r.srv.upgrades.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(r.srv.upgrades.load(), upgrades);   // no fight with the other connection

    r.srv.welcomeProto = uint16_t(pr::kProtocolVersion + 1);
    r.c->connect();
    REQUIRE(waitState(*r.c, net::ConnState::Incompatible, ev));
    CHECK_EQ(ev.error, std::string("incompatible"));
    CHECK(r.c->state() == net::ConnState::Incompatible);
}
