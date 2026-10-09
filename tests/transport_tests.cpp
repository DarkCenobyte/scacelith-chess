// What the transports owe the process around them (net/transport.h): on Linux no write of the
// TLS transport may raise SIGPIPE, whatever the disposition of that signal (a child process whose
// SIGPIPE is left to its default action, death, talks to a local TLS server that resets or closes
// the connection under it, or cancels its own exchange before OpenSSL's last write); and a name
// that takes long to resolve holds up neither a timeout nor a cancellation (a stand-in for
// getaddrinfo that answers after a second: net/socket_util.h).
#include "test.h"
#include "net/credential_store.h"
#include "net/crypto.h"
#include "net/direct_match.h"
#include "net/net_sys.h"
#include "net/online_client.h"
#include "net/socket_util.h"
#include "net/transport.h"

#ifndef _WIN32
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

int msSince(Clock::time_point t0) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}

}  // namespace

#ifndef _WIN32

// =============================================================================================
// SIGPIPE: a local TLS server, the client in a child process
// =============================================================================================

namespace {

// The server's TLS context: a self-signed P-256 certificate for 127.0.0.1 made for the run, and
// its pin (the client accepts the unknown issuer of a pinned server).
struct TlsServer {
    SSL_CTX* ctx = nullptr;
    std::string pin;
    int listener = -1;
    uint16_t port = 0;

    ~TlsServer() {
        if (listener >= 0) ::close(listener);
        SSL_CTX_free(ctx);
    }

    bool start() {
        EVP_PKEY* key = EVP_EC_gen("P-256");
        X509* cert = X509_new();
        bool ok = key && cert && X509_set_version(cert, 2) && ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) &&
                  X509_gmtime_adj(X509_getm_notBefore(cert), -3600) && X509_gmtime_adj(X509_getm_notAfter(cert), 86400) &&
                  X509_set_pubkey(cert, key);
        if (ok) {
            X509_NAME* name = X509_get_subject_name(cert);
            ok = X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) &&
                 X509_set_issuer_name(cert, name);
        }
        if (ok) {
            X509V3_CTX v3;
            X509V3_set_ctx_nodb(&v3);
            X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
            X509_EXTENSION* san = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, "IP:127.0.0.1,DNS:localhost");
            ok = san && X509_add_ext(cert, san, -1);
            X509_EXTENSION_free(san);
        }
        ok = ok && X509_sign(cert, key, EVP_sha256()) > 0;
        if (ok) {
            unsigned char* der = nullptr;
            int len = i2d_X509(cert, &der);
            ok = len > 0;
            if (ok) pin = net::crypto::hex(net::crypto::sha256(der, size_t(len)));
            OPENSSL_free(der);
        }
        ctx = ok ? SSL_CTX_new(TLS_server_method()) : nullptr;
        ok = ctx && SSL_CTX_use_certificate(ctx, cert) == 1 && SSL_CTX_use_PrivateKey(ctx, key) == 1;
        if (ok) SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);   // no alert answers a bare FIN
        X509_free(cert);
        EVP_PKEY_free(key);
        if (!ok) return false;
        listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(a);
        if (listener < 0 || ::bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(listener, 4) != 0 ||
            ::getsockname(listener, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return false;
        port = ntohs(a.sin_port);
        return true;
    }
};

// One accepted TLS connection on the server's side (blocking, 5 s timeouts).
struct Peer {
    int fd = -1;
    SSL* ssl = nullptr;
    ~Peer() { drop(false); }

    bool accept(const TlsServer& srv) {
        pollfd p{srv.listener, POLLIN, 0};
        if (::poll(&p, 1, 5000) != 1) return false;
        fd = ::accept4(srv.listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) return false;
        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        ssl = SSL_new(srv.ctx);
        return ssl && SSL_set_fd(ssl, fd) == 1 && SSL_accept(ssl) == 1;
    }
    // The request head, up to its empty line.
    bool readHead(std::string& head) {
        char c;
        while (head.find("\r\n\r\n") == std::string::npos) {
            if (SSL_read(ssl, &c, 1) != 1 || head.size() > 16384) return false;
            head += c;
        }
        return true;
    }
    bool write(const std::string& s) { return SSL_write(ssl, s.data(), int(s.size())) == int(s.size()); }
    bool readSome() {
        char buf[256];
        return SSL_read(ssl, buf, sizeof(buf)) > 0;
    }
    // Waits until the client has gone (its close_notify, its FIN, or the timeout).
    void drain() {
        char buf[256];
        while (SSL_read(ssl, buf, sizeof(buf)) > 0) {}
    }
    // Closes; reset: with a TCP RST (SO_LINGER 0) instead of a FIN.
    void drop(bool reset) {
        if (fd < 0) return;
        if (reset) {
            linger l{1, 0};
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
        }
        SSL_free(ssl);
        ssl = nullptr;
        ::close(fd);
        fd = -1;
    }
};

std::string headerOf(const std::string& head, const std::string& name) {
    std::string lower;
    for (char c : head) lower += char(std::tolower((unsigned char)c));
    size_t p = lower.find("\r\n" + name + ":");
    if (p == std::string::npos) return std::string();
    p += name.size() + 3;
    size_t e = head.find("\r\n", p);
    std::string v = head.substr(p, e - p);
    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
    return v;
}

enum class Scenario {
    WsReset,              // after the upgrade: the server resets the connection (RST)
    WsCloseNotify,        // the server ends TLS (close_notify), then its side of TCP (FIN)
    WsCloseNotifyReset,   // close_notify, then RST
    StreamAbort,          // the client cancels its download from its last piece of body
};

const char kSubprotocol[] = "scacelith.rt1";

// The client's side, in the child: 0 when it went through, another code for a step that failed
// (a death by SIGPIPE shows in the wait status instead).
int runClient(Scenario sc, uint16_t port, const std::string& pin) {
    if (sc == Scenario::StreamAbort) {
        net::HttpRequest r;
        r.host = "127.0.0.1";
        r.port = port;
        r.pinnedSha256 = pin;
        r.path = "/file";
        r.timeoutMs = 5000;
        net::CancelToken cancel;
        net::HttpResponse resp;
        size_t got = 0;
        net::httpStream(
            r, [](const net::HttpHead& h) { return h.status == 200; },
            [&](const char*, size_t n) {
                got += n;
                if (got >= 5) cancel.cancel();   // shuts the socket down: OpenSSL's close_notify comes after
                return true;
            },
            resp, &cancel);
        if (got != 5) return 10;
        return 0;
    }
    net::WsParams p;
    p.host = "127.0.0.1";
    p.port = port;
    p.pinnedSha256 = pin;
    p.subprotocol = kSubprotocol;
    p.timeoutMs = 5000;
    std::string err;
    net::WsAnswer answer;
    std::unique_ptr<net::WebSocket> ws = net::wsConnect(p, err, answer);
    if (!ws) return 11;
    if (!ws->send({1, 2, 3})) return 12;   // the server waits for it before it closes
    uint16_t code = 0;
    std::string reason;
    const auto t0 = Clock::now();
    while (!ws->closed(code, reason) && msSince(t0) < 5000) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!ws->closed(code, reason)) return 13;
    for (int i = 0; i < 3; ++i) ws->send({4, 5, 6});   // refused (closed): never written
    ws->close(1000);
    ws.reset();
    return 0;
}

// The server's side, in the test's own process. A connection it does not reset stays open until
// the child has ended (peer is the caller's): the client's own shutdown, not the server's reset,
// decides what its last write meets.
bool runServer(Scenario sc, const TlsServer& srv, Peer& peer) {
    std::string head;
    if (!peer.accept(srv) || !peer.readHead(head)) return false;
    if (sc == Scenario::StreamAbort) {
        // Then silent: no alert of its own when the client's FIN comes.
        return peer.write("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
    }
    const std::string key = headerOf(head, "sec-websocket-key");
    const std::string accepted = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const net::crypto::Sha1 sha = net::crypto::sha1(accepted.data(), accepted.size());
    if (!peer.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                    net::crypto::base64(sha.data(), sha.size()) + "\r\nSec-WebSocket-Protocol: " + kSubprotocol + "\r\n\r\n"))
        return false;
    if (!peer.readSome()) return false;   // the client's first message: it is connected
    if (sc == Scenario::WsReset) {
        peer.drop(true);
        return true;
    }
    SSL_shutdown(peer.ssl);
    if (sc == Scenario::WsCloseNotifyReset) {
        peer.drop(true);
        return true;
    }
    ::shutdown(peer.fd, SHUT_WR);
    peer.drain();
    return true;
}

// Runs a scenario with the client in a child process whose SIGPIPE has its default action (and is
// not blocked): only the transport keeps it alive. The child also checks that the transport left
// that disposition as it was.
void checkNoSigpipe(Scenario sc) {
    TlsServer srv;
    REQUIRE(srv.start());
    // The server's own writes to a client that died must not end the test run.
    struct sigaction ignore {}, old{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, &old);
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(SIGPIPE, &dfl, nullptr);
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
        ::close(srv.listener);
        int code = runClient(sc, srv.port, srv.pin);
        struct sigaction now {};
        sigaction(SIGPIPE, nullptr, &now);
        if (code == 0 && now.sa_handler != SIG_DFL) code = 20;   // the transport changed it
        std::fflush(nullptr);
        ::_exit(code);
    }
    if (pid < 0) {
        sigaction(SIGPIPE, &old, nullptr);
        REQUIRE(pid > 0);
    }
    Peer peer;
    const bool served = runServer(sc, srv, peer);
    int status = 0;
    pid_t done = 0;
    const auto t0 = Clock::now();
    while ((done = ::waitpid(pid, &status, WNOHANG)) == 0 && msSince(t0) < 10000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (done == 0) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        std::fprintf(stderr, "  the client did not finish\n");
    }
    sigaction(SIGPIPE, &old, nullptr);
    CHECK(served);
    if (WIFSIGNALED(status)) std::fprintf(stderr, "  the client was killed by signal %d\n", WTERMSIG(status));
    CHECK(done == pid);
    CHECK(!WIFSIGNALED(status));
    CHECK(WIFEXITED(status));
    CHECK_EQ(WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0);
}

}  // namespace

// The server resets the connection under a live WebSocket: the client's I/O thread reads the
// reset, then OpenSSL's close_notify goes to a socket that can no longer send.
TEST(net_sigpipe_ws_reset) { checkNoSigpipe(Scenario::WsReset); }

// The server ends TLS and TCP in order (close_notify, FIN), or close_notify then RST: the client
// answers with its close_notify, and its later sends are refused.
TEST(net_sigpipe_ws_close_notify) {
    checkNoSigpipe(Scenario::WsCloseNotify);
    checkNoSigpipe(Scenario::WsCloseNotifyReset);
}

// A download cancelled from its last piece of body: the cancellation shuts the socket down
// (shutdown(SHUT_RDWR)), and the exchange ends with OpenSSL's close_notify on that socket.
TEST(net_sigpipe_stream_abort) { checkNoSigpipe(Scenario::StreamAbort); }

// The programs the game starts (xdg-open, for the browser and the file manager) get SIGPIPE's
// default action back, although the game ignores that signal (main.cpp) and an ignored signal
// stays ignored across exec: a shell that sends itself SIGPIPE dies of it.
TEST(net_sigpipe_spawned_program_default) {
    struct sigaction ignore {}, old{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, &old);
    sigset_t pipeSet, oldMask;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    pthread_sigmask(SIG_UNBLOCK, &pipeSet, &oldMask);
    char* argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>("kill -s PIPE $$; exit 3"), nullptr};
    const int pid = net::sys::spawnProgram(argv);
    int status = 0;
    const bool reaped = pid > 0 && ::waitpid(pid, &status, 0) == pid;
    pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
    sigaction(SIGPIPE, &old, nullptr);
    REQUIRE(reaped);
    CHECK(WIFSIGNALED(status));
    CHECK_EQ(WIFSIGNALED(status) ? WTERMSIG(status) : (WIFEXITED(status) ? 100 + WEXITSTATUS(status) : -1), SIGPIPE);
    // A program that is not there: -1, nothing to reap.
    char* missing[] = {const_cast<char*>("scacelith-no-such-program"), nullptr};
    CHECK_EQ(net::sys::spawnProgram(missing), -1);
}

#endif  // !_WIN32

// =============================================================================================
// Name resolution: a slow DNS holds up neither a timeout nor a cancellation
// =============================================================================================

namespace {

std::atomic<int> g_slowLookups{0};

// Answers (nothing) after a second, as a DNS server that does not answer would after much longer.
bool slowLookup(const std::string&, uint16_t, bool, std::vector<net::sock::Endpoint>&, std::string& error) {
    ++g_slowLookups;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    error = "no answer";
    return false;
}

// The slow lookup in place of getaddrinfo while it lives. The lookups it started run on and end
// alone: each keeps the function it started with, and its own state.
struct SlowDns {
    SlowDns() { net::sock::Lookup::setForTests(slowLookup); }
    ~SlowDns() { net::sock::Lookup::setForTests(nullptr); }
};

// How long the wait of a cancelled or timed-out resolution may take at most (the lookup itself
// takes 1000 ms).
constexpr int kPromptMs = 400;

}  // namespace

TEST(net_resolve_lookup_bounds) {
    using R = net::sock::Lookup::Result;
    SlowDns dns;
    {
        net::sock::Lookup l("slow.test", 443, true);
        const auto t0 = Clock::now();
        CHECK(l.wait(50) == R::TimedOut);
        CHECK(msSince(t0) < kPromptMs);
    }
    {
        net::sock::Lookup l("slow.test", 443, true);
        std::thread stopper([&l] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            l.stop();
        });
        const auto t0 = Clock::now();
        CHECK(l.wait(5000) == R::Stopped);
        CHECK(msSince(t0) < kPromptMs);
        stopper.join();
    }
    {
        std::atomic<bool> flag{false};
        net::sock::Lookup l("slow.test", 443, true);
        std::thread raiser([&flag] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            flag = true;
        });
        const auto t0 = Clock::now();
        CHECK(l.wait(5000, [&flag] { return flag.load(); }) == R::Stopped);
        CHECK(msSince(t0) < kPromptMs);
        raiser.join();
    }
    // An address needs no lookup: it is there at once.
    const int before = g_slowLookups.load();
    net::sock::Lookup v4("127.0.0.1", 8443, true);
    CHECK(v4.wait(0) == R::Found);
    REQUIRE(v4.endpoints().size() == 1);
    CHECK_EQ(v4.endpoints()[0].toString(), std::string("127.0.0.1:8443"));
    net::sock::Lookup v6("::1", 8443, true);
    CHECK(v6.wait(0) == R::Found);
    CHECK_EQ(g_slowLookups.load(), before);
    // The slow one's answer, when waited for.
    net::sock::Lookup slow("slow.test", 443, true);
    CHECK(slow.wait(5000) == R::NotFound);
    CHECK_EQ(slow.error(), std::string("no answer"));
}

namespace {

std::atomic<bool> g_dnsGateOpen{false};

// Answers (nothing) once the test opens the gate: a resolver that hangs until then.
bool gatedLookup(const std::string&, uint16_t, bool, std::vector<net::sock::Endpoint>&, std::string& error) {
    while (!g_dnsGateOpen.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    error = "no answer";
    return false;
}

}  // namespace

// A resolver that never answers keeps a bounded number of threads: past the limit a lookup fails
// at once, and once the stuck ones end, lookups get threads again.
TEST(net_resolve_lookup_threads_bounded) {
    using R = net::sock::Lookup::Result;
    const std::string refused = "too many name lookups still running";
    g_dnsGateOpen = false;
    net::sock::Lookup::setForTests(gatedLookup);
    std::vector<std::unique_ptr<net::sock::Lookup>> stuck;
    bool capped = false;
    for (int i = 0; i < 64 && !capped; ++i) {
        auto l = std::make_unique<net::sock::Lookup>("stuck.test", 443, true);
        const auto t0 = Clock::now();
        if (l->wait(20) == R::NotFound) {
            CHECK_EQ(l->error(), refused);
            CHECK(msSince(t0) < kPromptMs);
            capped = true;
        }
        stuck.push_back(std::move(l));
    }
    CHECK(capped);
    CHECK(stuck.size() <= 16 + 1);   // at most 16 threads, then the refused one
    g_dnsGateOpen = true;
    for (auto& l : stuck) l->wait(5000);
    net::sock::Lookup::setForTests(nullptr);
    // The threads still ending (this test's, earlier tests' slow ones) give their places back.
    net::sock::Lookup::setForTests(slowLookup);
    bool started = false;
    for (auto t0 = Clock::now(); !started && msSince(t0) < 3000;) {
        net::sock::Lookup l("slow.test", 443, true);
        if (l.wait(5000) == R::NotFound && l.error() == "no answer") started = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    net::sock::Lookup::setForTests(nullptr);
    CHECK(started);
}

// getaddrinfo itself, for a name every system resolves.
TEST(net_resolve_localhost) {
    net::sock::Lookup l("localhost", 80, true);
    REQUIRE(l.wait(10000) == net::sock::Lookup::Result::Found);
    CHECK(!l.endpoints().empty());
    for (const net::sock::Endpoint& e : l.endpoints()) CHECK_EQ(e.port(), uint16_t(80));
}

// A direct match joined by name: leaving, or the game quitting (the DirectMatch destroyed, which
// waits for its threads), does not wait for the name.
TEST(net_resolve_direct_guest_leaves_at_once) {
    SlowDns dns;
    const auto t0 = Clock::now();
    {
        net::DirectMatch d;
        d.join("slow.test", 47100, "ABCD-EFGH-JKMN", "Guest");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(d.state() == net::DirectMatch::State::Connecting);
    }
    CHECK(msSince(t0) < 30 + kPromptMs);
}

#ifndef _WIN32
// The Linux transport: the resolution counts in the connection's deadline, and a cancellation
// ends it at once (HTTPS, a streamed download, a WebSocket).
TEST(net_resolve_transport_bounds) {
    SlowDns dns;
    net::HttpRequest r;
    r.host = "slow.test";
    r.timeoutMs = 50;
    net::HttpResponse resp;
    auto t0 = Clock::now();
    net::httpRequest(r, resp);
    CHECK_EQ(resp.error, std::string("timeout"));
    CHECK(msSince(t0) < kPromptMs);

    r.timeoutMs = 5000;
    auto cancelSoon = [](net::CancelToken& c) {
        return std::thread([&c] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            c.cancel();
        });
    };
    {
        net::CancelToken cancel;
        std::thread t = cancelSoon(cancel);
        t0 = Clock::now();
        net::httpRequest(r, resp, &cancel);
        t.join();
        CHECK_EQ(resp.error, std::string("cancelled"));
        CHECK(msSince(t0) < kPromptMs);
    }
    {
        net::CancelToken cancel;
        std::thread t = cancelSoon(cancel);
        t0 = Clock::now();
        net::httpStream(r, [](const net::HttpHead&) { return true; }, [](const char*, size_t) { return true; }, resp, &cancel);
        t.join();
        CHECK_EQ(resp.error, std::string("cancelled"));
        CHECK(msSince(t0) < kPromptMs);
    }
    {
        net::CancelToken cancel;
        net::WsParams p;
        p.host = "slow.test";
        p.subprotocol = "scacelith.rt1";
        p.timeoutMs = 5000;
        std::string err;
        net::WsAnswer answer;
        std::thread t = cancelSoon(cancel);
        t0 = Clock::now();
        CHECK(!net::wsConnect(p, err, answer, &cancel));
        t.join();
        CHECK_EQ(err, std::string("cancelled"));
        CHECK(msSince(t0) < kPromptMs);
    }
    {
        net::CancelToken cancel;
        cancel.cancel();   // before it starts: no lookup at all
        const int before = g_slowLookups.load();
        net::httpRequest(r, resp, &cancel);
        CHECK_EQ(resp.error, std::string("cancelled"));
        CHECK_EQ(g_slowLookups.load(), before);
    }
}

// The online client quits at once while its HTTPS thread and its realtime thread both wait for a
// name (the game's exit destroys it, which cancels and joins its threads).
TEST(net_resolve_online_client_quits_at_once) {
    SlowDns dns;
    const std::string path = net::sys::exeDirectory() + "net-test-resolve.credentials";
    net::sys::removeFile(path);
    net::ServerEndpoint ep;
    ep.host = "slow.test";
    {
        net::CredentialStore s(path);
        net::Credential c;
        c.origin = ep.origin();
        c.username = "alice";
        c.token = "sct_" + std::string(43, 'R');
        CHECK(s.put(c));
    }
    const int before = g_slowLookups.load();
    auto t0 = Clock::now();
    {
        net::OnlineClient c;
        c.setCredentialsFile(path);
        c.setServer(ep);
        c.fetchServerInfo();   // net-http
        c.connect();           // net-rt: /info first
        while (g_slowLookups.load() < before + 2 && msSince(t0) < 2000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(g_slowLookups.load() >= before + 2);   // both threads are in a lookup
        t0 = Clock::now();
    }
    CHECK(msSince(t0) < kPromptMs);
    net::sys::removeFile(path);
}
#endif
