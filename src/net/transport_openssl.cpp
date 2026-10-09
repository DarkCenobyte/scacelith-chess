// Linux transport: TCP + OpenSSL, a minimal HTTP/1.1 client (Connection: close, Content-Length or
// chunked bodies) and an RFC 6455 WebSocket client (masked binary frames, fragmentation,
// ping/pong, closing handshake) whose socket lives on one I/O thread per connection. Sockets are
// non-blocking; every wait is a poll() with a deadline and a wake-up pipe, so cancel() and close()
// never hang. Nothing written to a socket raises SIGPIPE (send with MSG_NOSIGNAL, and the same for
// OpenSSL's writes: noSignalWriteBio), whatever the process does with that signal.
#if !defined(_WIN32) && defined(SCACELITH_HAS_OPENSSL)
#include "transport.h"
#include "crypto.h"
#include "socket_util.h"
#include "../core/log.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace net {

namespace {

using Clock = std::chrono::steady_clock;

struct Deadline {
    Clock::time_point end;
    explicit Deadline(int ms) : end(Clock::now() + std::chrono::milliseconds(ms)) {}
    int remaining() const {
        auto r = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
        return r < 0 ? 0 : int(r);
    }
};

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string sslErrors() {
    std::string s;
    unsigned long e;
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        if (!s.empty()) s += "; ";
        s += buf;
    }
    return s;
}

bool unknownIssuerError(int err) {
    return err == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT || err == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN ||
           err == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY || err == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT ||
           err == X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE || err == X509_V_ERR_CERT_UNTRUSTED;
}

int verifyCallback(int ok, X509_STORE_CTX* st);

// OpenSSL's socket BIO writes with write(2): on a socket that can no longer send (reset by the
// peer, or shut down by abortSocket()) the kernel raises SIGPIPE, whose default action ends the
// process. Every byte OpenSSL writes (the handshake, SSL_write, the close_notify of SSL_shutdown,
// a key update answered inside SSL_read) goes through this BIO instead, which sends with
// MSG_NOSIGNAL: such a write fails with EPIPE, whatever the process does with SIGPIPE. Reads keep
// OpenSSL's socket BIO. The descriptor is the BIO's data; the BIO never closes it.
int noSignalWrite(BIO* b, const char* data, int n) {
    BIO_clear_retry_flags(b);
    const int fd = int(reinterpret_cast<intptr_t>(BIO_get_data(b)));
    ssize_t r;
    do { r = ::send(fd, data, size_t(n), MSG_NOSIGNAL); } while (r < 0 && errno == EINTR);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) BIO_set_retry_write(b);
    return int(r);
}

long noSignalCtrl(BIO* b, int cmd, long, void* ptr) {
    switch (cmd) {
    case BIO_CTRL_FLUSH: return 1;
    case BIO_C_GET_FD: {
        const int fd = int(reinterpret_cast<intptr_t>(BIO_get_data(b)));
        if (ptr) *static_cast<int*>(ptr) = fd;
        return fd;
    }
    default: return 0;
    }
}

BIO* noSignalWriteBio(int fd) {
    static BIO_METHOD* const method = [] {
        BIO_METHOD* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK | BIO_TYPE_DESCRIPTOR, "socket send (no SIGPIPE)");
        if (m && (!BIO_meth_set_write(m, noSignalWrite) || !BIO_meth_set_ctrl(m, noSignalCtrl))) {
            BIO_meth_free(m);
            m = nullptr;
        }
        return m;
    }();
    BIO* b = method ? BIO_new(method) : nullptr;
    if (!b) return nullptr;
    BIO_set_data(b, reinterpret_cast<void*>(intptr_t(fd)));
    BIO_set_init(b, 1);
    return b;
}

// One client context for the process: system trust store, TLS 1.2+.
SSL_CTX* clientContext() {
    static SSL_CTX* ctx = [] {
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        if (!c) return c;
        SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
        SSL_CTX_set_default_verify_paths(c);
        SSL_CTX_set_verify(c, SSL_VERIFY_PEER, verifyCallback);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
        SSL_CTX_set_options(c, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
        return c;
    }();
    return ctx;
}

// A TCP connection, optionally with TLS. Non-blocking socket.
class Stream {
public:
    bool pinned = false;
    std::string error, detail;

    ~Stream() { closeNow(); }

    bool open(const std::string& host, uint16_t port, bool tls, const std::string& pin, int timeoutMs, CancelToken* cancel) {
        Deadline dl(timeoutMs);
        cancel_ = cancel;
        AbortGuard abortGuard(cancel, [this] { abortSocket(); });
        bool ok = connectTcp(host, port, dl) && (!tls || handshake(host, pin, dl));
        abortGuard.clear();
        if (!ok && cancel && cancel->cancelled()) error = "cancelled";
        return ok;
    }

    int fd() const { return fd_.load(); }
    bool hasPending() const { return ssl_ && SSL_pending(ssl_) > 0; }

    // >0 bytes, 0 end of stream, -1 error, -2 would block (wantWrite: wait for POLLOUT).
    int readSome(void* buf, size_t n, bool& wantWrite) {
        wantWrite = false;
        if (ssl_) {
            ERR_clear_error();
            int r = SSL_read(ssl_, buf, int(n));
            if (r > 0) return r;
            int e = SSL_get_error(ssl_, r);
            if (e == SSL_ERROR_WANT_READ) return -2;
            if (e == SSL_ERROR_WANT_WRITE) { wantWrite = true; return -2; }
            if (e == SSL_ERROR_ZERO_RETURN || (e == SSL_ERROR_SYSCALL && errno == 0)) return 0;
            detail = sslErrors();
            return -1;
        }
        ssize_t r = ::recv(fd(), buf, n, 0);
        if (r > 0) return int(r);
        if (r == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -2;
        detail = std::strerror(errno);
        return -1;
    }

    // >0 bytes written, -1 error, -2 would block (wantRead: wait for POLLIN).
    int writeSome(const void* buf, size_t n, bool& wantRead) {
        wantRead = false;
        if (ssl_) {
            ERR_clear_error();
            int r = SSL_write(ssl_, buf, int(n));
            if (r > 0) return r;
            int e = SSL_get_error(ssl_, r);
            if (e == SSL_ERROR_WANT_WRITE) return -2;
            if (e == SSL_ERROR_WANT_READ) { wantRead = true; return -2; }
            detail = sslErrors();
            return -1;
        }
        ssize_t r = ::send(fd(), buf, n, MSG_NOSIGNAL);
        if (r >= 0) return int(r);
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return -2;
        detail = std::strerror(errno);
        return -1;
    }

    bool writeAll(const void* data, size_t n, const Deadline& dl) {
        const char* p = static_cast<const char*>(data);
        while (n) {
            bool wantRead;
            int r = writeSome(p, n, wantRead);
            if (r > 0) { p += r; n -= size_t(r); continue; }
            if (r == -1) { error = "network"; return false; }
            if (!waitFor(wantRead ? POLLIN : POLLOUT, dl)) return false;
        }
        return true;
    }

    // >0 bytes, 0 end of stream, -1 failure (error set).
    int readBlocking(void* buf, size_t n, const Deadline& dl) {
        for (;;) {
            bool wantWrite;
            int r = readSome(buf, n, wantWrite);
            if (r >= 0) return r;
            if (r == -1) { error = "network"; return -1; }
            if (!waitFor(wantWrite ? POLLOUT : POLLIN, dl)) return -1;
        }
    }

    // Unblocks a poll() or a name resolution in progress from another thread.
    void abortSocket() {
        {
            std::lock_guard<std::mutex> lk(lookupMu_);
            if (lookup_) lookup_->stop();
        }
        int f = fd_.load();
        if (f >= 0) ::shutdown(f, SHUT_RDWR);
    }

    void closeNow() {
        if (ssl_) {
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        int f = fd_.exchange(-1);
        if (f >= 0) ::close(f);
    }

    void shutdownTls() {
        if (ssl_) SSL_shutdown(ssl_);   // best effort close_notify
    }

private:
    std::atomic<int> fd_{-1};
    SSL* ssl_ = nullptr;
    CancelToken* cancel_ = nullptr;
    std::mutex lookupMu_;
    sock::Lookup* lookup_ = nullptr;      // the name resolution open() waits for (abortSocket stops it)

    bool waitFor(short events, const Deadline& dl) {
        if (cancel_ && cancel_->cancelled()) { error = "cancelled"; return false; }
        pollfd p{fd(), events, 0};
        int r;
        do { r = ::poll(&p, 1, dl.remaining()); } while (r < 0 && errno == EINTR);
        if (cancel_ && cancel_->cancelled()) { error = "cancelled"; return false; }
        if (r == 0) { error = "timeout"; return false; }
        if (r < 0) { error = "network"; detail = std::strerror(errno); return false; }
        return true;
    }

    // The name is resolved within the connection's deadline and given up on cancel (sock::Lookup:
    // getaddrinfo itself cannot be interrupted), then each address is tried in turn.
    bool connectTcp(const std::string& host, uint16_t port, const Deadline& dl) {
        if (cancel_ && cancel_->cancelled()) { error = "cancelled"; return false; }
        sock::Lookup lookup(host, port, true);
        {
            std::lock_guard<std::mutex> lk(lookupMu_);
            lookup_ = &lookup;
        }
        const sock::Lookup::Result found = lookup.wait(dl.remaining(), [this] { return cancel_ && cancel_->cancelled(); });
        {
            std::lock_guard<std::mutex> lk(lookupMu_);
            lookup_ = nullptr;
        }
        if (found != sock::Lookup::Result::Found) {
            error = found == sock::Lookup::Result::TimedOut ? "timeout" : found == sock::Lookup::Result::Stopped ? "cancelled" : "network";
            detail = found == sock::Lookup::Result::NotFound ? "resolve: " + lookup.error() : std::string("resolve: ") + error;
            return false;
        }
        bool ok = false;
        for (const sock::Endpoint& a : lookup.endpoints()) {
            int f = ::socket(a.family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
            if (f < 0) continue;
            fd_.store(f);
            int one = 1;
            setsockopt(f, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            int r = ::connect(f, reinterpret_cast<const sockaddr*>(a.storage), socklen_t(a.len));
            if (r != 0 && errno == EINPROGRESS) {
                if (waitFor(POLLOUT, dl)) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(f, SOL_SOCKET, SO_ERROR, &err, &len);
                    r = err == 0 ? 0 : -1;
                    if (err) detail = std::strerror(err);
                } else {
                    r = -1;
                }
            } else if (r != 0) {
                detail = std::strerror(errno);
            }
            if (r == 0) { ok = true; break; }
            fd_.store(-1);
            ::close(f);
            if (error == "timeout" || error == "cancelled") break;
        }
        if (!ok && error.empty()) error = "network";
        return ok;
    }

    bool handshake(const std::string& host, const std::string& pin, const Deadline& dl) {
        SSL_CTX* ctx = clientContext();
        if (!ctx || !(ssl_ = SSL_new(ctx))) { error = "tls"; detail = sslErrors(); return false; }
        pinned = !pin.empty();
        SSL_set_app_data(ssl_, this);
        BIO* rbio = BIO_new_socket(fd(), BIO_NOCLOSE);
        BIO* wbio = noSignalWriteBio(fd());
        if (!rbio || !wbio) {
            BIO_free(rbio);
            BIO_free(wbio);
            error = "tls";
            detail = sslErrors();
            return false;
        }
        SSL_set_bio(ssl_, rbio, wbio);   // owned by ssl_ from now on
        SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        if (isIpLiteral(host)) {
            X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl_), host.c_str());
        } else {
            SSL_set_tlsext_host_name(ssl_, host.c_str());
            SSL_set1_host(ssl_, host.c_str());
        }
        for (;;) {
            ERR_clear_error();
            int r = SSL_connect(ssl_);
            if (r == 1) break;
            int e = SSL_get_error(ssl_, r);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                if (!waitFor(e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, dl)) return false;
                continue;
            }
            long v = SSL_get_verify_result(ssl_);
            if (v != X509_V_OK) {
                error = "certificate";
                detail = X509_verify_cert_error_string(v);
            } else {
                error = "tls";
                detail = sslErrors();
            }
            return false;
        }
        if (pinned) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
            X509* cert = SSL_get1_peer_certificate(ssl_);
#else
            X509* cert = SSL_get_peer_certificate(ssl_);
#endif
            std::string got;
            if (cert) {
                unsigned char* der = nullptr;
                int len = i2d_X509(cert, &der);
                if (len > 0) got = crypto::hex(crypto::sha256(der, size_t(len)));
                OPENSSL_free(der);
                X509_free(cert);
            }
            if (got.empty() || !crypto::constantTimeEqual(got, pin)) {
                error = "certificate";
                detail = "pinned certificate mismatch (server presents " + got + ")";
                return false;
            }
        } else if (SSL_get_verify_result(ssl_) != X509_V_OK) {
            error = "certificate";
            return false;
        }
        return true;
    }
};

int verifyCallback(int ok, X509_STORE_CTX* st) {
    if (ok) return 1;
    SSL* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(st, SSL_get_ex_data_X509_STORE_CTX_idx()));
    Stream* s = ssl ? static_cast<Stream*>(SSL_get_app_data(ssl)) : nullptr;
    // With a pin, an issuer outside the trust store is fine: the leaf is compared to the pin
    // right after the handshake. Every other failure (host name, dates...) still counts.
    return s && s->pinned && unknownIssuerError(X509_STORE_CTX_get_error(st)) ? 1 : 0;
}

// ---- HTTP/1.1 ----

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool parseHead(const std::string& head, HttpHead& out) {
    size_t eol = head.find("\r\n");
    std::string line = head.substr(0, eol);
    if (line.compare(0, 5, "HTTP/") != 0) return false;
    size_t sp = line.find(' ');
    if (sp == std::string::npos || sp + 4 > line.size()) return false;
    out.status = std::atoi(line.c_str() + sp + 1);
    if (out.status < 100 || out.status > 999) return false;
    size_t pos = eol == std::string::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) e = head.size();
        std::string h = head.substr(pos, e - pos);
        size_t c = h.find(':');
        if (c != std::string::npos) out.headers.emplace_back(lower(trim(h.substr(0, c))), trim(h.substr(c + 1)));
        pos = e + 2;
    }
    return true;
}

// Decodes the chunks of a chunked body that are whole from 'pos' on, as more of it arrives: their
// data goes to the end of 'out' and 'pos' moves past them (it stays at the start of a chunk that
// is not whole yet). False when malformed; otherwise 'complete' tells whether the last chunk (size
// 0) and the line end after it have arrived.
bool dechunk(const std::string& in, size_t& pos, std::string& out, bool& complete) {
    complete = false;
    for (;;) {
        size_t e = in.find("\r\n", pos);
        if (e == std::string::npos) return true;
        std::string sizeLine = in.substr(pos, e - pos);
        size_t semi = sizeLine.find(';');
        if (semi != std::string::npos) sizeLine.resize(semi);
        sizeLine = trim(sizeLine);
        if (sizeLine.empty() || sizeLine.size() > 8) return false;
        char* end = nullptr;
        unsigned long n = std::strtoul(sizeLine.c_str(), &end, 16);
        if (*end) return false;
        size_t data = e + 2;
        if (n == 0) {
            // Trailers end with an empty line.
            complete = in.find("\r\n", data) != std::string::npos;
            return true;
        }
        if (in.size() < data + n + 2) return true;
        if (in.compare(data + n, 2, "\r\n") != 0) return false;
        out.append(in, data, n);
        pos = data + n + 2;
    }
}

// ---- WebSocket ----

constexpr char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

class OpenSslWebSocket final : public WebSocket {
public:
    OpenSslWebSocket(std::unique_ptr<Stream> s, const WsParams& p, std::string initial, std::string serverId)
        : s_(std::move(s)), maxBytes_(p.maxMessageBytes), onActivity_(p.onActivity), in_(std::move(initial)) {
        serverId_ = std::move(serverId);
        if (::pipe2(wake_, O_NONBLOCK | O_CLOEXEC) != 0) wake_[0] = wake_[1] = -1;
        io_ = std::thread([this] { run(); });
    }
    ~OpenSslWebSocket() override {
        close(1001);
        if (wake_[0] >= 0) { ::close(wake_[0]); ::close(wake_[1]); }
    }

    bool send(const std::vector<uint8_t>& msg) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_ || closeSent_) return false;
        appendFrame(0x2, msg.data(), msg.size());
        wake();
        return true;
    }

    bool receive(std::vector<uint8_t>& msg) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (inq_.empty()) return false;
        msg = std::move(inq_.front());
        inq_.pop_front();
        return true;
    }

    bool closed(uint16_t& code, std::string& reason) const override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!closed_) return false;
        code = closeCode_;
        reason = closeReason_;
        return true;
    }

    void close(uint16_t code) override {
        std::unique_lock<std::mutex> lk(mu_);
        if (!io_.joinable()) return;
        if (!closed_ && !closeSent_) {
            uint8_t payload[2] = {uint8_t(code >> 8), uint8_t(code)};
            appendFrame(0x8, payload, 2);
            closeSent_ = true;
            sentCode_ = code;
            wake();
        }
        // The server answers with its close frame; do not wait for it more than a second.
        cv_.wait_for(lk, std::chrono::seconds(1), [this] { return closed_; });
        stop_ = true;
        wake();
        lk.unlock();
        io_.join();
    }

private:
    std::unique_ptr<Stream> s_;
    size_t maxBytes_;
    std::function<void()> onActivity_;
    std::thread io_;
    int wake_[2] = {-1, -1};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::string out_;                     // encoded frames waiting for the I/O thread
    std::deque<std::vector<uint8_t>> inq_;
    bool closed_ = false, closeSent_ = false, stop_ = false;
    uint16_t closeCode_ = 1006, sentCode_ = 1000;
    std::string closeReason_;

    // I/O thread state.
    std::string in_;                      // raw bytes not yet parsed
    std::vector<uint8_t> msg_;            // fragments of the current message
    int msgOp_ = -1;

    void wake() {
        if (wake_[1] >= 0) {
            char c = 1;
            (void)!::write(wake_[1], &c, 1);
        }
    }

    // Client frames are always masked (RFC 6455 5.3). Called with mu_ held.
    void appendFrame(int opcode, const uint8_t* data, size_t n) {
        std::string f;
        f += char(0x80 | opcode);
        if (n < 126) {
            f += char(0x80 | n);
        } else if (n < 65536) {
            f += char(0x80 | 126);
            f += char(n >> 8);
            f += char(n);
        } else {
            f += char(0x80 | 127);
            for (int i = 7; i >= 0; --i) f += char(uint64_t(n) >> (8 * i));
        }
        uint8_t mask[4];
        crypto::randomBytes(mask, 4);
        f.append(reinterpret_cast<char*>(mask), 4);
        size_t at = f.size();
        f.append(reinterpret_cast<const char*>(data), n);
        for (size_t i = 0; i < n; ++i) f[at + i] = char(uint8_t(f[at + i]) ^ mask[i & 3]);
        out_ += f;
    }

    void markClosed(uint16_t code, const std::string& reason) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (closed_) return;
            closed_ = true;
            closeCode_ = code;
            closeReason_ = reason;
        }
        cv_.notify_all();
        if (onActivity_) onActivity_();
    }

    // Protocol error on our side: send a close frame with the code, then stop.
    void fail(uint16_t code, const char* why) {
        LOGW("net: websocket closed by the client (%u): %s", code, why);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!closeSent_) {
                uint8_t payload[2] = {uint8_t(code >> 8), uint8_t(code)};
                appendFrame(0x8, payload, 2);
                closeSent_ = true;
            }
        }
        markClosed(code, why);
    }

    // Parses complete frames from in_. Returns false once the connection is finished.
    bool parseFrames() {
        size_t pos = 0;
        bool alive = true;
        while (alive) {
            size_t avail = in_.size() - pos;
            if (avail < 2) break;
            const uint8_t* b = reinterpret_cast<const uint8_t*>(in_.data() + pos);
            bool fin = b[0] & 0x80;
            int op = b[0] & 0x0F;
            if (b[0] & 0x70) { fail(1002, "reserved bits set"); return false; }
            if (b[1] & 0x80) { fail(1002, "masked server frame"); return false; }
            uint64_t len = b[1] & 0x7F;
            size_t hdr = 2;
            if (len == 126) {
                if (avail < 4) break;
                len = uint64_t(b[2]) << 8 | b[3];
                hdr = 4;
            } else if (len == 127) {
                if (avail < 10) break;
                len = 0;
                for (int i = 0; i < 8; ++i) len = len << 8 | b[2 + i];
                hdr = 10;
            }
            bool control = op & 0x8;
            if (control && (!fin || len > 125)) { fail(1002, "bad control frame"); return false; }
            if (len > maxBytes_ || (!control && msg_.size() + len > maxBytes_)) { fail(1009, "message too big"); return false; }
            if (avail < hdr + len) break;
            const uint8_t* payload = b + hdr;
            size_t n = size_t(len);
            pos += hdr + n;
            switch (op) {
            case 0x0:
                if (msgOp_ < 0) { fail(1002, "unexpected continuation"); return false; }
                msg_.insert(msg_.end(), payload, payload + n);
                if (fin) alive = deliver();
                break;
            case 0x1:
            case 0x2:
                if (msgOp_ >= 0) { fail(1002, "interleaved message"); return false; }
                msgOp_ = op;
                msg_.assign(payload, payload + n);
                if (fin) alive = deliver();
                break;
            case 0x8: {
                uint16_t code = n >= 2 ? uint16_t(payload[0] << 8 | payload[1]) : 1005;
                std::string reason = n > 2 ? std::string(reinterpret_cast<const char*>(payload + 2), n - 2) : std::string();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (!closeSent_) {
                        uint8_t echo[2] = {uint8_t(code >> 8), uint8_t(code)};
                        appendFrame(0x8, echo, code == 1005 ? 0 : 2);
                        closeSent_ = true;
                    }
                }
                markClosed(code, reason);
                alive = false;
                break;
            }
            case 0x9: {
                std::lock_guard<std::mutex> lk(mu_);
                if (!closeSent_) appendFrame(0xA, payload, n);
                break;
            }
            case 0xA:
                break;
            default:
                fail(1002, "unknown opcode");
                return false;
            }
        }
        in_.erase(0, pos);
        return alive;
    }

    bool deliver() {
        if (msgOp_ == 0x1) {
            fail(1003, "text message");
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            inq_.push_back(std::move(msg_));
        }
        msg_.clear();
        msgOp_ = -1;
        if (onActivity_) onActivity_();
        return true;
    }

    void run() {
        std::string wbuf;
        bool readWantsWrite = false, writeWantsRead = false, finished = !parseFrames();
        Clock::time_point closeDeadline{};
        char buf[16384];
        for (;;) {
            bool stop;
            {
                std::lock_guard<std::mutex> lk(mu_);
                wbuf += out_;
                out_.clear();
                stop = stop_;
                if (closeSent_ && closeDeadline == Clock::time_point{}) closeDeadline = Clock::now() + std::chrono::seconds(1);
            }
            if (stop) break;
            // Write what is pending.
            while (!wbuf.empty()) {
                int r = s_->writeSome(wbuf.data(), wbuf.size(), writeWantsRead);
                if (r > 0) { wbuf.erase(0, size_t(r)); continue; }
                if (r == -1) { finished = true; wbuf.clear(); markClosed(1006, "write failed"); }
                break;
            }
            if (finished && wbuf.empty()) break;
            // Read what is available.
            while (!finished) {
                int r = s_->readSome(buf, sizeof(buf), readWantsWrite);
                if (r > 0) {
                    in_.append(buf, size_t(r));
                    if (!parseFrames()) finished = true;
                    continue;
                }
                if (r == 0 || r == -1) {
                    finished = true;
                    markClosed(1006, r == 0 ? "connection closed" : "read failed");
                }
                break;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!out_.empty()) continue;
            }
            if (finished && wbuf.empty()) break;
            if (s_->hasPending()) continue;
            int timeout = -1;
            if (closeDeadline != Clock::time_point{}) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(closeDeadline - Clock::now()).count();
                if (left <= 0) { markClosed(sentCode_, "close timeout"); break; }
                timeout = int(left);
            }
            pollfd p[2];
            p[0].fd = s_->fd();
            p[0].events = short((finished ? 0 : POLLIN) | (!wbuf.empty() || readWantsWrite ? POLLOUT : 0) | (writeWantsRead ? POLLIN : 0));
            p[0].revents = 0;
            p[1].fd = wake_[0];
            p[1].events = POLLIN;
            p[1].revents = 0;
            int r = ::poll(p, 2, timeout);
            if (r < 0 && errno != EINTR) { markClosed(1006, "poll failed"); break; }
            if (p[1].revents & POLLIN) {
                char drain[64];
                while (::read(wake_[0], drain, sizeof(drain)) > 0) {}
            }
        }
        s_->shutdownTls();
        s_->closeNow();
        markClosed(1006, "closed");
    }
};

}  // namespace

bool transportAvailable() { return clientContext() != nullptr; }

void httpRequest(const HttpRequest& req, HttpResponse& resp, CancelToken* cancel) {
    resp = HttpResponse();
    if (!req.tls && !isLoopbackHost(req.host)) {
        resp.error = "insecure";
        return;
    }
    Stream s;
    if (!s.open(req.host, req.port, req.tls, req.pinnedSha256, req.timeoutMs, cancel)) {
        resp.error = s.error;
        resp.detail = s.detail;
        return;
    }
    std::string head = req.method + " " + req.path + " HTTP/1.1\r\n";
    head += "Host: " + hostHeader(req.host, req.port, req.tls) + "\r\n";
    head += "User-Agent: Scacelith\r\nAccept: " + req.accept + "\r\nConnection: close\r\n";
    bool hasBody = !req.body.empty() || req.method == "POST" || req.method == "PUT";
    if (hasBody) head += "Content-Type: application/json\r\nContent-Length: " + std::to_string(req.body.size()) + "\r\n";
    for (auto& h : req.headers) head += h.first + ": " + h.second + "\r\n";
    head += "\r\n";

    AbortGuard abortGuard(cancel, [&s] { s.abortSocket(); });
    Deadline dl(req.timeoutMs);
    bool ok = s.writeAll(head.data(), head.size(), dl) && (req.body.empty() || s.writeAll(req.body.data(), req.body.size(), dl));
    std::string raw;
    HttpHead ph;
    size_t bodyAt = std::string::npos;
    bool chunked = false;
    std::string cl;
    size_t chunkAt = 0;   // the first chunk not decoded yet (chunked)
    std::string decoded;
    char buf[16384];
    while (ok) {
        if (bodyAt == std::string::npos) {
            size_t e = raw.find("\r\n\r\n");
            if (e != std::string::npos) {
                if (!parseHead(raw.substr(0, e), ph)) { s.error = "network"; s.detail = "bad HTTP response"; ok = false; break; }
                if (ph.status >= 100 && ph.status < 200) { raw.erase(0, e + 4); continue; }   // interim answer
                bodyAt = chunkAt = e + 4;
                chunked = lower(ph.get("transfer-encoding")).find("chunked") != std::string::npos;
                cl = ph.get("content-length");
            } else if (raw.size() > 65536) {
                s.error = "network"; s.detail = "response header too large"; ok = false; break;
            }
        }
        if (bodyAt != std::string::npos) {
            if (chunked) {
                bool complete;
                if (!dechunk(raw, chunkAt, decoded, complete)) { s.error = "network"; s.detail = "bad chunked body"; ok = false; break; }
                if (complete) { resp.body = std::move(decoded); break; }
            } else if (!cl.empty()) {
                size_t want = size_t(std::strtoull(cl.c_str(), nullptr, 10));
                if (want > req.maxResponseBytes) { s.error = "too_large"; ok = false; break; }
                if (raw.size() - bodyAt >= want) { resp.body = raw.substr(bodyAt, want); break; }
            }
            if (raw.size() - bodyAt > req.maxResponseBytes) { s.error = "too_large"; ok = false; break; }
        }
        int r = s.readBlocking(buf, sizeof(buf), dl);
        if (r < 0) { ok = false; break; }
        if (r == 0) {
            // End of stream: the body runs to the end when no length was given.
            if (bodyAt == std::string::npos) { s.error = "network"; s.detail = "connection closed before the response"; ok = false; break; }
            if (chunked || !cl.empty()) {
                s.error = "network"; s.detail = "truncated response"; ok = false; break;
            }
            resp.body = raw.substr(bodyAt);
            break;
        }
        raw.append(buf, size_t(r));
    }
    abortGuard.clear();
    if (!ok) {
        resp.error = cancel && cancel->cancelled() ? "cancelled" : (s.error.empty() ? "network" : s.error);
        resp.detail = s.detail;
        resp.body.clear();
        return;
    }
    resp.status = ph.status;
    resp.retryAfter = ph.get("retry-after");
    s.shutdownTls();
}

// ---- Streamed responses ----

namespace {

// The chunked transfer coding, decoded as the bytes come in.
class Dechunker {
public:
    // Consumes p[0..n); chunk data goes to 'out' (false from it: aborted). False when the coding
    // is malformed or 'out' aborted.
    bool feed(const char* p, size_t n, const std::function<bool(const char*, size_t)>& out, bool& aborted) {
        aborted = false;
        while (n) {
            if (state_ == Data) {
                size_t k = left_ < n ? size_t(left_) : n;
                if (!out(p, k)) { aborted = true; return false; }
                p += k; n -= k; left_ -= k;
                if (left_ == 0) state_ = DataEnd;
                continue;
            }
            if (state_ == Done) return true;   // anything after the trailers is ignored
            // Size, DataEnd and Trailer read lines.
            char c = *p++;
            --n;
            if (c != '\n') {
                if (line_.size() > 4096) return false;
                line_ += c;
                continue;
            }
            if (!line_.empty() && line_.back() == '\r') line_.pop_back();
            std::string line;
            line.swap(line_);
            if (state_ == DataEnd) {
                if (!line.empty()) return false;
                state_ = Size;
            } else if (state_ == Size) {
                size_t semi = line.find(';');
                if (semi != std::string::npos) line.resize(semi);
                line = trim(line);
                if (line.empty() || line.size() > 15) return false;
                char* end = nullptr;
                left_ = std::strtoull(line.c_str(), &end, 16);
                if (*end) return false;
                state_ = left_ ? Data : Trailer;
            } else if (state_ == Trailer && line.empty()) {
                state_ = Done;
            }
        }
        return true;
    }
    bool done() const { return state_ == Done; }

private:
    enum State { Size, Data, DataEnd, Trailer, Done };
    State state_ = Size;
    uint64_t left_ = 0;
    std::string line_;
};

}  // namespace

void httpStream(const HttpRequest& req, const std::function<bool(const HttpHead&)>& onHead,
                const std::function<bool(const char*, size_t)>& onBody, HttpResponse& resp, CancelToken* cancel) {
    resp = HttpResponse();
    if (!req.tls && !isLoopbackHost(req.host)) {
        resp.error = "insecure";
        return;
    }
    Stream s;
    if (!s.open(req.host, req.port, req.tls, req.pinnedSha256, req.timeoutMs, cancel)) {
        resp.error = s.error;
        resp.detail = s.detail;
        return;
    }
    std::string head = req.method + " " + req.path + " HTTP/1.1\r\n";
    head += "Host: " + hostHeader(req.host, req.port, req.tls) + "\r\n";
    bool agent = false;
    for (auto& h : req.headers) agent = agent || lower(h.first) == "user-agent";
    if (!agent) head += "User-Agent: Scacelith\r\n";
    head += "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    for (auto& h : req.headers) head += h.first + ": " + h.second + "\r\n";
    head += "\r\n";

    AbortGuard abortGuard(cancel, [&s] { s.abortSocket(); });
    std::string err;
    bool ok = s.writeAll(head.data(), head.size(), Deadline(req.timeoutMs));
    // The head (interim 1xx answers skipped); 'raw' keeps what came after it.
    std::string raw;
    HttpHead ph;
    std::vector<char> buf(64 * 1024);
    for (bool haveHead = false; ok && !haveHead;) {
        size_t e = raw.find("\r\n\r\n");
        if (e == std::string::npos) {
            if (raw.size() > 65536) { s.error = "network"; s.detail = "response header too large"; ok = false; break; }
            int r = s.readBlocking(buf.data(), buf.size(), Deadline(req.timeoutMs));
            if (r <= 0) {
                if (r == 0) { s.error = "network"; s.detail = "connection closed before the response"; }
                ok = false;
                break;
            }
            raw.append(buf.data(), size_t(r));
            continue;
        }
        ph = HttpHead();
        if (!parseHead(raw.substr(0, e), ph)) { s.error = "network"; s.detail = "bad HTTP response"; ok = false; break; }
        raw.erase(0, e + 4);
        haveHead = ph.status >= 200;
    }
    if (ok) {
        resp.status = ph.status;
        resp.retryAfter = ph.get("retry-after");
        bool wantBody = onHead(ph) && req.method != "HEAD" && ph.status != 204 && ph.status != 304;
        if (wantBody) {
            const bool chunked = lower(ph.get("transfer-encoding")).find("chunked") != std::string::npos;
            const std::string cl = ph.get("content-length");
            const bool sized = !chunked && !cl.empty();
            uint64_t left = sized ? std::strtoull(cl.c_str(), nullptr, 10) : 0;
            uint64_t total = 0;
            Dechunker dechunk;
            bool aborted = false;
            // Body bytes to the caller, with the size cap.
            auto deliver = [&](const char* p, size_t n) {
                total += n;
                if (total > req.maxResponseBytes) { err = "too_large"; return false; }
                if (!onBody(p, n)) { err = "aborted"; return false; }
                return true;
            };
            if (sized && left > req.maxResponseBytes) { err = "too_large"; ok = false; }
            bool finished = sized && left == 0;
            // 'raw' first, then the socket.
            const char* p = raw.data();
            size_t n = raw.size();
            while (ok && !finished) {
                if (n) {
                    if (chunked) {
                        if (!dechunk.feed(p, n, deliver, aborted)) {
                            if (err.empty()) { s.error = "network"; s.detail = "bad chunked body"; }
                            ok = false;
                            break;
                        }
                        finished = dechunk.done();
                    } else {
                        size_t k = sized && left < n ? size_t(left) : n;
                        if (!deliver(p, k)) { ok = false; break; }
                        if (sized) {
                            left -= k;
                            finished = left == 0;
                        }
                    }
                    if (finished) break;
                }
                int r = s.readBlocking(buf.data(), buf.size(), Deadline(req.timeoutMs));
                if (r < 0) { ok = false; break; }
                if (r == 0) {
                    // End of stream: complete only for a body that runs to the end of the connection.
                    if (chunked || sized) err = "truncated";
                    else finished = true;
                    ok = finished;
                    break;
                }
                p = buf.data();
                n = size_t(r);
            }
        }
    }
    abortGuard.clear();
    if (!ok) {
        resp.error = cancel && cancel->cancelled() ? "cancelled" : !err.empty() ? err : (s.error.empty() ? "network" : s.error);
        resp.detail = s.detail;
        return;
    }
    s.shutdownTls();
}

std::unique_ptr<WebSocket> wsConnect(const WsParams& p, std::string& error, WsAnswer& answer, CancelToken* cancel) {
    error.clear();
    answer = WsAnswer();
    if (!p.tls && !isLoopbackHost(p.host)) {
        error = "insecure";
        return nullptr;
    }
    auto s = std::make_unique<Stream>();
    if (!s->open(p.host, p.port, p.tls, p.pinnedSha256, p.timeoutMs, cancel)) {
        error = s->error;
        if (!s->detail.empty()) LOGW("net: websocket connect to %s:%u: %s", p.host.c_str(), p.port, s->detail.c_str());
        return nullptr;
    }
    uint8_t keyBytes[16];
    crypto::randomBytes(keyBytes, sizeof(keyBytes));
    std::string key = crypto::base64(keyBytes, sizeof(keyBytes));
    std::string head = "GET " + p.path + " HTTP/1.1\r\n";
    head += "Host: " + hostHeader(p.host, p.port, p.tls) + "\r\n";
    head += "User-Agent: Scacelith\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
    head += "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n";
    head += "Sec-WebSocket-Protocol: " + p.subprotocol + "\r\n\r\n";

    Stream* sp = s.get();
    AbortGuard abortGuard(cancel, [sp] { sp->abortSocket(); });
    Deadline dl(p.timeoutMs);
    std::string raw;
    size_t end = std::string::npos;
    bool ok = s->writeAll(head.data(), head.size(), dl);
    char buf[4096];
    while (ok && end == std::string::npos) {
        int r = s->readBlocking(buf, sizeof(buf), dl);
        if (r <= 0) { ok = false; if (r == 0) s->error = "network"; break; }
        raw.append(buf, size_t(r));
        end = raw.find("\r\n\r\n");
        if (end == std::string::npos && raw.size() > 16384) { ok = false; s->error = "network"; }
    }
    abortGuard.clear();
    if (!ok) {
        error = cancel && cancel->cancelled() ? "cancelled" : s->error;
        return nullptr;
    }
    HttpHead ph;
    if (!parseHead(raw.substr(0, end), ph)) { error = "network"; return nullptr; }
    answer.status = ph.status;
    answer.retryAfter = ph.get("retry-after");
    if (ph.status != 101) { error = "http_" + std::to_string(ph.status); return nullptr; }
    crypto::Sha1 acc = crypto::sha1((key + kWsGuid).data(), key.size() + sizeof(kWsGuid) - 1);
    if (lower(ph.get("upgrade")) != "websocket" || lower(ph.get("connection")).find("upgrade") == std::string::npos ||
        ph.get("sec-websocket-accept") != crypto::base64(acc.data(), acc.size())) {
        error = "network";
        LOGW("net: websocket handshake refused (bad upgrade answer)");
        return nullptr;
    }
    if (ph.get("sec-websocket-protocol") != p.subprotocol) { error = "subprotocol"; return nullptr; }
    if (!ph.get("sec-websocket-extensions").empty()) { error = "network"; return nullptr; }   // none was offered
    return std::make_unique<OpenSslWebSocket>(std::move(s), p, raw.substr(end + 4), ph.get("scacelith-server-id"));
}

}  // namespace net

#endif
