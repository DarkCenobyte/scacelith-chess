// The OpenSSL transport of Linux and macOS (src/net/transport_openssl.cpp) in the cases the other
// tests leave out, in the test's own process (tests/transport_tests.cpp runs its TLS clients in
// child processes):
//   - TLS on a loopback server with a self-signed certificate made for the run: HTTPS with the
//     server's pin, by address and by name; a pin that does not match; no pin (the unknown
//     issuer refused); a server that does not speak TLS; a record that breaks the TLS stream;
//     a WebSocket and a streamed answer over TLS; the online client against that server (no pin:
//     "certificate", with the pin: its /info, then a WebSocket port whose certificate is another);
//   - the WebSocket frames a client meets on a plain loopback connection: 16- and 64-bit
//     lengths, a message in fragments, a ping, and the frames that end it (a continuation with no
//     message, an unknown opcode, a text message); a client message of each length form; an
//     upgrade answer with a wrong accept key;
//   - HTTP answers: a head that never ends, a body that runs to the end of the connection, a
//     request body larger than the socket's buffers, a connection reset under the request, a
//     streamed answer with a broken chunked coding, plain HTTP to another host;
//   - the socket helpers of net/socket_util.h: error names, IPv6 endpoints (IPv4-mapped too),
//     the multicast options of a UDP socket.
// Windows has its own transport (WinHTTP, transport_win32.cpp): the TLS and frame cases are not run
// there.
#include "test.h"
#include "headless.h"
#include "net_gaps_helpers.h"
#include "net/credential_store.h"
#include "net/crypto.h"
#include "net/online_client.h"
#include "net/protocol_gen.h"
#include "net/socket_util.h"
#include "net/transport.h"

#ifndef _WIN32
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

#ifndef _WIN32

namespace {

const char kSub[] = "scacelith.test1";

// A self-signed P-256 certificate for 127.0.0.1 and localhost, made for the run, in a server
// context; pin: the hex SHA-256 of its DER form.
struct TlsIdentity {
    SSL_CTX* ctx = nullptr;
    std::string pin;
    TlsIdentity() {
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
            const int len = i2d_X509(cert, &der);
            ok = len > 0;
            if (ok) pin = net::crypto::hex(net::crypto::sha256(der, size_t(len)));
            OPENSSL_free(der);
        }
        ctx = ok ? SSL_CTX_new(TLS_server_method()) : nullptr;
        if (ctx && (SSL_CTX_use_certificate(ctx, cert) != 1 || SSL_CTX_use_PrivateKey(ctx, key) != 1)) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        if (ctx) SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
        X509_free(cert);
        EVP_PKEY_free(key);
    }
    ~TlsIdentity() { SSL_CTX_free(ctx); }
    TlsIdentity(const TlsIdentity&) = delete;
    TlsIdentity& operator=(const TlsIdentity&) = delete;
};

// One accepted TLS connection on the server's side (blocking, 3 s timeouts).
struct TlsPeer {
    int fd = -1;
    SSL* ssl = nullptr;
    std::string in;   // read and not consumed

    bool readMore() {
        char buf[16384];
        const int r = SSL_read(ssl, buf, sizeof buf);
        if (r <= 0) return false;
        in.append(buf, size_t(r));
        return true;
    }
    bool readHead(std::string& head) {
        size_t e;
        while ((e = in.find("\r\n\r\n")) == std::string::npos)
            if (in.size() > 65536 || !readMore()) return false;
        head = in.substr(0, e);
        in.erase(0, e + 4);
        return true;
    }
    bool write(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            const int r = SSL_write(ssl, s.data() + off, int(s.size() - off));
            if (r <= 0) return false;
            off += size_t(r);
        }
        return true;
    }
    bool frame(int& op, std::string& payload) {
        for (;;) {
            const int r = gaps::takeClientFrame(in, op, payload);
            if (r != 0) return r > 0;
            if (!readMore()) return false;
        }
    }
};

// A TLS server on 127.0.0.1 with its own identity: each connection is accepted (TLS handshake
// included) and handed to the handler on a thread of its own.
class TlsLoopback {
public:
    using Handler = std::function<void(TlsPeer&)>;
    explicit TlsLoopback(Handler h) : handler_(std::move(h)) {
        if (!id_.ctx) return;
#ifdef __APPLE__
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ >= 0) fcntl(listener_, F_SETFD, FD_CLOEXEC);
#else
        listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(a);
        if (listener_ < 0 || ::bind(listener_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(listener_, 8) != 0 ||
            ::getsockname(listener_, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return;
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { run(); });
    }
    ~TlsLoopback() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        for (std::thread& t : workers_) t.join();
        if (listener_ >= 0) ::close(listener_);
    }
    TlsLoopback(const TlsLoopback&) = delete;
    TlsLoopback& operator=(const TlsLoopback&) = delete;

    bool ok() const { return port_ != 0; }
    uint16_t port() const { return port_; }
    const std::string& pin() const { return id_.pin; }
    int handshakes() const { return handshakes_.load(); }

private:
    TlsIdentity id_;
    Handler handler_;
    int listener_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> handshakes_{0};
    std::thread thread_;
    std::vector<std::thread> workers_;

    void run() {
        while (!stop_.load()) {
            pollfd p{listener_, POLLIN, 0};
            if (::poll(&p, 1, 100) != 1) continue;
            const int fd = ::accept(listener_, nullptr, nullptr);
            if (fd < 0) continue;
            workers_.emplace_back([this, fd] {
                timeval tv{3, 0};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                TlsPeer peer;
                peer.fd = fd;
                peer.ssl = SSL_new(id_.ctx);
                if (peer.ssl && SSL_set_fd(peer.ssl, fd) == 1 && SSL_accept(peer.ssl) == 1) {
                    ++handshakes_;
                    handler_(peer);
                    SSL_shutdown(peer.ssl);
                }
                SSL_free(peer.ssl);
                ::close(fd);
            });
        }
    }
};

// SIGPIPE ignored while a test runs: the servers' writes to a client that has gone must not end
// the run (the client side, the code under test, never raises it).
struct NoSigpipe {
    struct sigaction old {};
    NoSigpipe() {
        struct sigaction ignore {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        sigaction(SIGPIPE, &ignore, &old);
    }
    ~NoSigpipe() { sigaction(SIGPIPE, &old, nullptr); }
};

net::HttpRequest tlsGet(uint16_t port, const std::string& pin, const std::string& host = "127.0.0.1") {
    net::HttpRequest r;
    r.host = host;
    r.port = port;
    r.tls = true;
    r.pinnedSha256 = pin;
    r.path = "/hello";
    r.timeoutMs = 5000;
    return r;
}

// Answers one HTTP request with a small body, then waits for the client's close.
void helloHandler(TlsPeer& p) {
    std::string head;
    if (!p.readHead(head)) return;
    p.write(gaps::httpAnswer(200, "hello over tls", "Retry-After: 7\r\n", "text/plain"));
}

}  // namespace

// HTTPS to a pinned self-signed server, by address and by name; the pin of another certificate,
// and no pin at all, are refused as "certificate".
TEST(transport_gaps_tls_pinned_and_refused) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    NoSigpipe sigpipe;
    TlsLoopback srv(helloHandler);
    REQUIRE(srv.ok());

    net::HttpResponse resp;
    net::httpRequest(tlsGet(srv.port(), srv.pin()), resp);
    CHECK_EQ(resp.error, std::string(""));
    CHECK_EQ(resp.status, 200);
    CHECK_EQ(resp.body, std::string("hello over tls"));
    CHECK_EQ(resp.retryAfter, std::string("7"));

    // By name: the name goes in the SNI and is checked against the certificate.
    net::httpRequest(tlsGet(srv.port(), srv.pin(), "localhost"), resp);
    CHECK_EQ(resp.error, std::string(""));
    CHECK_EQ(resp.body, std::string("hello over tls"));

    const int before = srv.handshakes();
    net::httpRequest(tlsGet(srv.port(), std::string(64, '0')), resp);
    CHECK_EQ(resp.error, std::string("certificate"));
    CHECK(resp.detail.find("pinned certificate mismatch") != std::string::npos);
    CHECK(resp.detail.find(srv.pin()) != std::string::npos);   // what the server presents, for the log
    CHECK_EQ(resp.status, 0);

    // No pin: a self-signed certificate is no issuer the system trusts.
    net::httpRequest(tlsGet(srv.port(), ""), resp);
    CHECK_EQ(resp.error, std::string("certificate"));
    CHECK(!resp.detail.empty());
    CHECK(resp.body.empty());
    CHECK(srv.handshakes() >= before + 1);   // the pinned mismatch completed its handshake
}

// A server that answers in plain text where TLS is expected, and one that breaks the TLS stream
// after the handshake (bytes that are no TLS record): "tls" and "network", with OpenSSL's reason.
TEST(transport_gaps_tls_broken_streams) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    NoSigpipe sigpipe;
    gaps::TcpServer plain([](gaps::Conn& c) {
        c.readMore(2000);   // the ClientHello
        c.sendAll("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
        c.drainUntilClosed(2000);
    });
    REQUIRE(plain.ok());
    net::HttpResponse resp;
    net::httpRequest(tlsGet(plain.port(), "ab" + std::string(62, 'c')), resp);
    CHECK_EQ(resp.error, std::string("tls"));
    CHECK(!resp.detail.empty());

    TlsLoopback srv([](TlsPeer& p) {
        std::string head;
        if (!p.readHead(head)) return;
        const char junk[] = "\x17\x03\x03\x00\x05garbage that is not a record";
        (void)!::send(p.fd, junk, sizeof junk - 1, MSG_NOSIGNAL);
        char buf[64];
        while (::recv(p.fd, buf, sizeof buf, 0) > 0) {
        }
    });
    REQUIRE(srv.ok());
    net::httpRequest(tlsGet(srv.port(), srv.pin()), resp);
    CHECK_EQ(resp.error, std::string("network"));
    CHECK(!resp.detail.empty());
    CHECK_EQ(resp.status, 0);
}

// A WebSocket over TLS: the upgrade, messages both ways (a client message of each length form), the
// server's close; and a streamed answer in chunks over TLS.
TEST(transport_gaps_tls_websocket_and_stream) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    NoSigpipe sigpipe;
    std::atomic<int> echoed{0};
    TlsLoopback srv([&](TlsPeer& p) {
        std::string head;
        if (!p.readHead(head)) return;
        if (gaps::requestPath(head) == "/stream") {
            p.write("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
            return;
        }
        if (!p.write(gaps::wsUpgradeAnswer(head, kSub))) return;
        int op = 0;
        std::string payload;
        while (p.frame(op, payload)) {
            if (op == 0x8) {
                p.write(gaps::serverFrame(0x8, payload.substr(0, 2)));
                return;
            }
            if (op != 0x2) continue;
            p.write(gaps::serverFrame(0x2, payload));   // echoed back
            if (++echoed == 3) p.write(gaps::serverFrame(0x8, gaps::closePayload(4000, "done")));
        }
    });
    REQUIRE(srv.ok());

    net::WsParams wp;
    wp.host = "127.0.0.1";
    wp.port = srv.port();
    wp.pinnedSha256 = srv.pin();
    wp.subprotocol = kSub;
    wp.timeoutMs = 5000;
    std::string err;
    net::WsAnswer answer;
    std::unique_ptr<net::WebSocket> ws = net::wsConnect(wp, err, answer);
    REQUIRE(ws != nullptr);
    CHECK_EQ(answer.status, 101);
    const std::vector<uint8_t> small(10, 1), mid(300, 2), big(70000, 3);   // 7-, 16- and 64-bit lengths
    CHECK(ws->send(small));
    CHECK(ws->send(mid));
    CHECK(ws->send(big));
    std::vector<std::vector<uint8_t>> got;
    uint16_t code = 0;
    std::string reason;
    CHECK(gaps::eventually([&] {
        std::vector<uint8_t> m;
        while (ws->receive(m)) got.push_back(m);
        return ws->closed(code, reason);
    }));
    REQUIRE(got.size() == 3);
    CHECK(got[0] == small);
    CHECK(got[1] == mid);
    CHECK(got[2] == big);
    CHECK_EQ(code, uint16_t(4000));
    CHECK_EQ(reason, std::string("done"));
    CHECK(!ws->send(small));   // closed: refused
    ws->close(1000);
    ws.reset();

    net::HttpRequest rq = tlsGet(srv.port(), srv.pin());
    rq.path = "/stream";
    std::string body;
    net::HttpResponse resp;
    net::httpStream(
        rq, [](const net::HttpHead& h) { return h.status == 200; },
        [&](const char* p, size_t n) {
            body.append(p, n);
            return true;
        },
        resp);
    CHECK_EQ(resp.error, std::string(""));
    CHECK_EQ(body, std::string("hello world"));
}

// The online client against a TLS server: without the pin the self-signed certificate is refused
// (/info, and a connection, which then stops for good); with it /info answers, and a WebSocket port
// whose certificate is another one stops the connection as "certificate".
TEST(transport_gaps_online_client_over_tls) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    NoSigpipe sigpipe;
    TlsLoopback ws([](TlsPeer& p) {   // the other certificate: never reached past the handshake
        std::string head;
        p.readHead(head);
    });
    REQUIRE(ws.ok());
    const uint16_t wsPort = ws.port();
    TlsLoopback api([&](TlsPeer& p) {
        std::string head;
        if (!p.readHead(head)) return;
        char info[512];
        std::snprintf(info, sizeof info,
                      "{\"name\":\"Tls\",\"serverId\":\"srv-tls\",\"protocol\":{\"min\":%u,\"max\":%u},\"wsPort\":%u,\"wsPath\":\"/ws\"}",
                      unsigned(pr::kProtocolVersion), unsigned(pr::kProtocolVersion), unsigned(wsPort));
        p.write(gaps::httpAnswer(200, info));
    });
    REQUIRE(api.ok());

    headless::TempDir dir("gaps-tls-client");
    std::error_code ec;
    std::filesystem::permissions(std::filesystem::u8path(dir.path), std::filesystem::perms::owner_all, ec);
    const std::string credPath = dir.file("tls.credentials");
    net::ServerEndpoint ep;
    ep.host = "127.0.0.1";
    ep.apiPort = api.port();
    {
        net::CredentialStore s(credPath);
        net::Credential cr;
        cr.origin = ep.origin();
        cr.username = "alice";
        cr.token = "sct_" + std::string(43, 'T');
        s.put(cr);
    }
    auto waitFor = [](net::OnlineClient& c, net::Event::Kind k, net::Event& out, const std::function<bool(const net::Event&)>& pred) {
        return gaps::eventually([&] {
            net::Event ev;
            while (c.poll(ev))
                if (ev.kind == k && (!pred || pred(ev))) {
                    out = ev;
                    return true;
                }
            return false;
        }, 10000);
    };
    using K = net::Event::Kind;
    net::Event ev;
    {
        net::OnlineClient c;
        c.setCredentialsFile(credPath);
        c.setServer(ep);
        c.fetchServerInfo();
        REQUIRE(waitFor(c, K::ServerInfoResult, ev, nullptr));
        CHECK(!ev.ok);
        CHECK_EQ(ev.error, std::string("certificate"));
        c.connect();
        REQUIRE(waitFor(c, K::ConnectionChanged, ev, [](const net::Event& e) { return e.state == net::ConnState::Offline; }));
        CHECK_EQ(ev.error, std::string("certificate"));

        ep.pinnedSha256 = api.pin();
        ep.wsPort = wsPort;
        c.setServer(ep);
        c.fetchServerInfo();
        REQUIRE(waitFor(c, K::ServerInfoResult, ev, nullptr));
        CHECK(ev.ok);
        CHECK_EQ(ev.info.name, std::string("Tls"));
        CHECK_EQ(int(ev.info.wsPort), int(wsPort));
        c.connect();
        REQUIRE(waitFor(c, K::ConnectionChanged, ev, [](const net::Event& e) { return e.state == net::ConnState::Offline; }));
        CHECK_EQ(ev.error, std::string("certificate"));
    }
}

// =============================================================================================
// WebSocket frames and HTTP answers on a plain loopback connection
// =============================================================================================

namespace {

// A WebSocket server whose frames after the upgrade are the test's: 'script' writes them, after
// reading what it needs of the client's.
std::unique_ptr<net::WebSocket> connectTo(uint16_t port, std::string& err, size_t maxBytes = 1 << 20) {
    net::WsParams p;
    p.host = "127.0.0.1";
    p.port = port;
    p.tls = false;
    p.subprotocol = kSub;
    p.timeoutMs = 5000;
    p.maxMessageBytes = maxBytes;
    net::WsAnswer answer;
    return net::wsConnect(p, err, answer);
}

// Waits until the socket is closed; the messages received before go to 'got'.
bool closedWith(net::WebSocket& ws, uint16_t& code, std::string& reason, std::vector<std::vector<uint8_t>>* got = nullptr) {
    return gaps::eventually([&] {
        std::vector<uint8_t> m;
        while (ws.receive(m))
            if (got) got->push_back(m);
        return ws.closed(code, reason);
    });
}

struct FrameServer {
    std::function<void(gaps::Conn&)> script;
    std::atomic<int> clientClose{-1};   // the code of the client's close frame
    std::atomic<int> pongs{0};
    std::string pongPayload;
    std::mutex mu;
    gaps::TcpServer tcp;
    explicit FrameServer(std::function<void(gaps::Conn&)> s)
        : script(std::move(s)), tcp([this](gaps::Conn& c) {
              std::string head;
              if (!c.readHead(head) || !c.sendAll(gaps::wsUpgradeAnswer(head, kSub))) return;
              script(c);
              // What the client sends until it closes: its pongs and its close frame.
              int op = 0;
              std::string payload;
              while (gaps::readClientFrame(c, op, payload, 2000)) {
                  if (op == 0xA) {
                      std::lock_guard<std::mutex> lk(mu);
                      pongPayload = payload;
                      ++pongs;
                  }
                  if (op == 0x8) {
                      clientClose = payload.size() >= 2 ? (uint8_t(payload[0]) << 8 | uint8_t(payload[1])) : 1005;
                      c.sendAll(gaps::serverFrame(0x8, payload.substr(0, 2)));
                      break;
                  }
              }
          }) {}
};

}  // namespace

// Frames of every length form, a message in fragments with a ping between them (answered with its
// payload), then the server's close.
TEST(transport_gaps_ws_frames_delivered) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    const std::string mid(300, 'm'), big(70000, 'b');
    FrameServer srv([&](gaps::Conn& c) {
        std::string all = gaps::serverFrame(0x2, mid) + gaps::serverFrame(0x2, big);
        // Sent in two pieces cut inside the 64-bit length: the parser waits for the rest.
        const size_t cut = gaps::serverFrame(0x2, mid).size() + 5;
        c.sendAll(all.substr(0, cut));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        c.sendAll(all.substr(cut));
        // A 16-bit length cut after its first byte, too.
        const std::string third = gaps::serverFrame(0x2, std::string(200, 't'));
        c.sendAll(third.substr(0, 3));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        c.sendAll(third.substr(3));
        c.sendAll(gaps::serverFrame(0x2, "frag-", false));
        c.sendAll(gaps::serverFrame(0x9, "are you there"));
        c.sendAll(gaps::serverFrame(0x0, "ment", true));
        c.sendAll(gaps::serverFrame(0xA, "unasked pong"));   // ignored
    });
    REQUIRE(srv.tcp.ok());
    std::string err;
    std::unique_ptr<net::WebSocket> ws = connectTo(srv.tcp.port(), err);
    REQUIRE(ws != nullptr);
    std::vector<std::vector<uint8_t>> got;
    CHECK(gaps::eventually([&] {
        std::vector<uint8_t> m;
        while (ws->receive(m)) got.push_back(m);
        return got.size() >= 4;
    }));
    REQUIRE(got.size() == 4);
    CHECK_EQ(std::string(got[0].begin(), got[0].end()), mid);
    CHECK_EQ(std::string(got[1].begin(), got[1].end()), big);
    CHECK_EQ(got[2].size(), size_t(200));
    CHECK_EQ(std::string(got[3].begin(), got[3].end()), std::string("frag-ment"));
    CHECK(gaps::eventually([&] { return srv.pongs.load() == 1; }));
    {
        std::lock_guard<std::mutex> lk(srv.mu);
        CHECK_EQ(srv.pongPayload, std::string("are you there"));
    }
    ws->close(1000);
    CHECK_EQ(srv.clientClose.load(), 1000);
}

// Frames the client refuses: it closes with the protocol's code (and says so to the server).
TEST(transport_gaps_ws_frames_refused) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    struct Case {
        const char* what;
        std::string frames;
        uint16_t code;
    };
    const Case cases[] = {
        {"continuation without a message", gaps::serverFrame(0x0, "orphan"), 1002},
        {"unknown opcode", gaps::serverFrame(0x3, "?"), 1002},
        {"text message", gaps::serverFrame(0x1, "text"), 1003},
        {"text message in fragments", gaps::serverFrame(0x1, "te", false) + gaps::serverFrame(0x0, "xt"), 1003},
    };
    for (const Case& k : cases) {
        FrameServer srv([&](gaps::Conn& c) { c.sendAll(k.frames); });
        REQUIRE(srv.tcp.ok());
        std::string err;
        std::unique_ptr<net::WebSocket> ws = connectTo(srv.tcp.port(), err);
        REQUIRE(ws != nullptr);
        uint16_t code = 0;
        std::string reason;
        std::vector<std::vector<uint8_t>> got;
        const bool closed = closedWith(*ws, code, reason, &got);
        if (!closed || code != k.code) std::fprintf(stderr, "  case: %s\n", k.what);
        CHECK(closed);
        CHECK_EQ(code, k.code);
        CHECK(!reason.empty());
        CHECK(got.empty());
        CHECK(gaps::eventually([&] { return srv.clientClose.load() == int(k.code); }, 3000));
        ws->close(1000);
    }
}

// An upgrade whose Sec-WebSocket-Accept is not the key's: refused before any frame.
TEST(transport_gaps_ws_bad_accept_key) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    gaps::TcpServer srv([](gaps::Conn& c) {
        std::string head;
        if (!c.readHead(head)) return;
        c.sendAll("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                  gaps::wsAccept("another key") + "\r\nSec-WebSocket-Protocol: " + kSub + "\r\n\r\n");
        c.drainUntilClosed(1000);
    });
    REQUIRE(srv.ok());
    std::string err;
    std::unique_ptr<net::WebSocket> ws = connectTo(srv.port(), err);
    CHECK(ws == nullptr);
    CHECK_EQ(err, std::string("network"));
}

// HTTP answers: a head that never ends (refused past 64 KiB), a body that runs to the end of the
// connection, a request body larger than the socket's buffers sent to a server that reads it
// late, a connection reset while the request is sent, a broken chunked coding in a stream, and
// plain HTTP to another host (refused before any connection).
TEST(transport_gaps_http_answers) {
    if (!net::transportAvailable()) SKIP("transport unavailable");
    std::atomic<size_t> bodyBytes{0};
    gaps::TcpServer srv([&](gaps::Conn& c) {
        std::string head;
        if (!c.readHead(head)) return;
        const std::string path = gaps::requestPath(head);
        if (path == "/endless-head") {
            std::string h = "HTTP/1.1 200 OK\r\n";
            for (int i = 0; i < 1200; ++i) h += "X-Filler-" + std::to_string(i) + ": " + std::string(60, 'f') + "\r\n";
            c.sendAll(h);
            c.drainUntilClosed(2000);
        } else if (path == "/to-the-end") {
            c.sendAll("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nall of it");
            c.shutdownSend();
            c.drainUntilClosed(2000);
        } else if (path == "/upload") {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));   // the client's buffers fill
            std::string body;
            if (!gaps::readBody(c, head, body)) return;
            bodyBytes = body.size();
            c.sendAll(gaps::httpAnswer(200, "{\"stored\":true}"));
        } else if (path == "/reset") {
            linger l{1, 0};   // a reset instead of a close
            setsockopt(c.handle(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l);
        } else if (path == "/bad-chunks") {
            c.sendAll("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX7\r\n");
            c.drainUntilClosed(2000);
        }
    });
    REQUIRE(srv.ok());
    auto request = [&](const std::string& path) {
        net::HttpRequest r;
        r.host = "127.0.0.1";
        r.port = srv.port();
        r.tls = false;
        r.path = path;
        r.timeoutMs = 5000;
        return r;
    };
    net::HttpResponse resp;
    net::httpRequest(request("/endless-head"), resp);
    CHECK_EQ(resp.error, std::string("network"));
    CHECK_EQ(resp.detail, std::string("response header too large"));

    net::httpRequest(request("/to-the-end"), resp);
    CHECK_EQ(resp.error, std::string(""));
    CHECK_EQ(resp.status, 200);
    CHECK_EQ(resp.body, std::string("all of it"));

    net::HttpRequest up = request("/upload");
    up.method = "POST";
    up.body.assign(size_t(24) << 20, 'u');
    net::httpRequest(up, resp);
    CHECK_EQ(resp.error, std::string(""));
    CHECK_EQ(resp.status, 200);
    CHECK_EQ(bodyBytes.load(), up.body.size());

    net::HttpRequest reset = request("/reset");
    reset.method = "POST";
    reset.body.assign(size_t(8) << 20, 'r');
    net::httpRequest(reset, resp);
    CHECK_EQ(resp.error, std::string("network"));
    CHECK_EQ(resp.status, 0);

    std::string streamed;
    net::httpStream(
        request("/bad-chunks"), [](const net::HttpHead&) { return true; },
        [&](const char* p, size_t n) {
            streamed.append(p, n);
            return true;
        },
        resp);
    CHECK_EQ(resp.error, std::string("network"));
    CHECK_EQ(resp.detail, std::string("bad chunked body"));
    CHECK_EQ(streamed, std::string("hello"));   // the whole chunk before the break was delivered

    // Plain HTTP leaves the machine for nobody: refused before any connection.
    const int before = srv.connections();
    net::HttpRequest away = request("/x");
    away.host = "192.0.2.1";
    net::httpStream(away, [](const net::HttpHead&) { return true; }, [](const char*, size_t) { return true; }, resp);
    CHECK_EQ(resp.error, std::string("insecure"));
    net::httpRequest(away, resp);
    CHECK_EQ(resp.error, std::string("insecure"));
    CHECK_EQ(srv.connections(), before);
}

#endif  // !_WIN32

// The socket helpers: error names, IPv6 endpoints (an IPv4-mapped one shows as IPv4), the
// wildcard IPv6 endpoint, and the multicast options of a UDP socket (an IPv6 endpoint is no
// IPv4 interface).
TEST(transport_gaps_socket_helpers) {
    using namespace net::sock;
#ifndef _WIN32
    CHECK_EQ(errorName(ETIMEDOUT), std::string("timeout"));
    CHECK_EQ(errorName(EHOSTUNREACH), std::string("unreachable"));
    CHECK_EQ(errorName(ENETUNREACH), std::string("unreachable"));
    CHECK_EQ(errorName(EADDRNOTAVAIL), std::string("unreachable"));
    CHECK_EQ(errorName(EMFILE), std::string("network"));
#endif
    Endpoint e;
    REQUIRE(Endpoint::parse("2001:db8::7", 47100, e));
    CHECK(e.isV6());
    CHECK(!e.isV4());
    CHECK_EQ(int(e.port()), 47100);
    CHECK_EQ(e.ip(), std::string("2001:db8::7"));
    CHECK_EQ(e.toString(), std::string("[2001:db8::7]:47100"));
    e.setPort(443);
    CHECK_EQ(e.toString(), std::string("[2001:db8::7]:443"));
    REQUIRE(Endpoint::parse("::ffff:192.0.2.33", 80, e));
    CHECK(e.isV6());
    CHECK_EQ(e.ip(), std::string("192.0.2.33"));
    const Endpoint any = Endpoint::anyV6(5000);
    CHECK(any.isV6());
    CHECK_EQ(int(any.port()), 5000);
    CHECK_EQ(any.ip(), std::string("::"));

    Handle h = openUdpV4();
    REQUIRE(h != kInvalid);
    Endpoint lo;
    REQUIRE(Endpoint::parse("127.0.0.1", 0, lo));
    CHECK(setMulticastIf(h, lo));
    CHECK(!setMulticastIf(h, any));   // IPv4 interfaces only
    CHECK(setMulticastTtl(h, 2));
    closeSocket(h);
}
