// Small loopback servers for the *_gaps_tests of the network modules: a TCP server on 127.0.0.1
// that hands each connection to the test's handler on a thread of its own, and the few pieces of
// HTTP/1.1 and WebSocket (RFC 6455) a handler needs to play a server byte by byte: a request head,
// a header, the upgrade's accept key, server frames of any length, the client's masked frames.
// Unlike tests/http_fake.h (one scripted answer per request), the handler writes what it wants:
// malformed answers, frames that break the protocol, a WebSocket that stays open while the test
// sends messages on it. Only loopback peers reach it (it listens on 127.0.0.1 alone).
#pragma once
#include "net/crypto.h"
#include "net/socket_util.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gaps {

using Clock = std::chrono::steady_clock;

// One accepted connection (a non-blocking socket, waited on with PollSet). The handler reads and
// writes it on its thread; the test thread may write too (sendAll is serialized).
class Conn {
public:
    Conn(net::sock::Handle h, const std::atomic<bool>& stop) : h_(h), stop_(stop) {}
    net::sock::Handle handle() const { return h_; }
    std::string in;          // bytes read and not consumed yet
    bool peerClosed = false; // the peer closed its side (or the connection failed)
    bool stopping() const { return stop_.load(); }

    // Appends what arrives within ms to 'in'. False once the peer has closed, on an error, at the
    // timeout or when the server stops.
    bool readMore(int ms = 5000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            if (stop_.load()) return false;
            uint8_t buf[16384];
            bool closed = false;
            const int r = net::sock::recvSome(h_, buf, sizeof buf, closed);
            if (r > 0) {
                in.append(reinterpret_cast<const char*>(buf), size_t(r));
                return true;
            }
            if (r < 0 || closed) {
                peerClosed = true;
                return false;
            }
            if (Clock::now() >= end) return false;
            net::sock::PollSet ps;
            ps.add(h_, true, false);
            ps.wait(50);
        }
    }
    // Reads until 'in' holds at least n bytes.
    bool need(size_t n, int ms = 5000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (in.size() < n) {
            const int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count());
            if (left <= 0 || !readMore(left)) return false;
        }
        return true;
    }
    // The request head up to its empty line, taken out of 'in' (the empty line too).
    bool readHead(std::string& head, int ms = 5000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        size_t e;
        while ((e = in.find("\r\n\r\n")) == std::string::npos) {
            const int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count());
            if (left <= 0 || !readMore(left)) return false;
        }
        head = in.substr(0, e);
        in.erase(0, e + 4);
        return true;
    }
    bool sendAll(const std::string& s) {
        std::lock_guard<std::mutex> lk(sendMu_);
        const auto end = Clock::now() + std::chrono::seconds(10);
        size_t off = 0;
        while (off < s.size()) {
            const int r = net::sock::sendSome(h_, reinterpret_cast<const uint8_t*>(s.data()) + off, s.size() - off);
            if (r < 0) return false;
            if (r == 0) {
                if (Clock::now() > end || stop_.load()) return false;
                net::sock::PollSet ps;
                ps.add(h_, false, true);
                ps.wait(50);
                continue;
            }
            off += size_t(r);
        }
        return true;
    }
    void shutdownSend() { net::sock::shutdownSend(h_); }
    // Waits until the peer closes its side (or ms pass), reading and dropping what it sends.
    void drainUntilClosed(int ms = 3000) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end && readMore(int(std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count())))
            in.clear();
    }

private:
    net::sock::Handle h_;
    const std::atomic<bool>& stop_;
    std::mutex sendMu_;
};

// Listens on 127.0.0.1 (a free port); every connection goes to the handler on a thread of its own
// and is closed when the handler returns. The destructor stops the handlers (their reads fail) and
// joins them.
class TcpServer {
public:
    using Handler = std::function<void(Conn&)>;
    explicit TcpServer(Handler h) : handler_(std::move(h)) {
        std::string err;
        listener_ = net::sock::listenLoopbackV4(port_, err);
        if (listener_ != net::sock::kInvalid) thread_ = std::thread([this] { run(); });
    }
    ~TcpServer() {
        stop_.store(true);
        waker_.wake();
        if (thread_.joinable()) thread_.join();
        for (std::thread& t : workers_) t.join();
        net::sock::closeSocket(listener_);
    }
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool ok() const { return listener_ != net::sock::kInvalid && port_ != 0; }
    uint16_t port() const { return port_; }
    int connections() const { return accepted_.load(); }

private:
    Handler handler_;
    net::sock::Handle listener_ = net::sock::kInvalid;
    uint16_t port_ = 0;
    net::sock::Waker waker_;
    std::atomic<bool> stop_{false};
    std::atomic<int> accepted_{0};
    std::thread thread_;
    std::vector<std::thread> workers_;   // run()'s only

    void run() {
        while (!stop_.load()) {
            net::sock::PollSet ps;
            ps.add(listener_, true, false);
            if (waker_.valid()) ps.add(waker_.handle(), true, false);
            ps.wait(200);
            if (waker_.valid() && ps.readable(waker_.handle())) waker_.drain();
            if (stop_.load()) break;
            if (!ps.readable(listener_)) continue;
            net::sock::Handle c = net::sock::acceptOne(listener_, nullptr, true);
            if (c == net::sock::kInvalid) continue;
            ++accepted_;
            workers_.emplace_back([this, c] {
                {
                    Conn conn(c, stop_);
                    handler_(conn);
                }
                net::sock::closeSocket(c);
            });
        }
    }
};

inline std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// A header's value in a request or answer head (name in any case; "" when absent).
inline std::string headerOf(const std::string& head, const std::string& name) {
    const std::string lower = lowerAscii(head);
    size_t p = lower.find("\r\n" + lowerAscii(name) + ":");
    if (p == std::string::npos) return std::string();
    p += name.size() + 3;
    size_t e = head.find("\r\n", p);
    std::string v = head.substr(p, e == std::string::npos ? std::string::npos : e - p);
    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
    return v;
}

// "GET /api/v1/info HTTP/1.1" -> "/api/v1/info"; the method in 'method' when given.
inline std::string requestPath(const std::string& head, std::string* method = nullptr) {
    const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return std::string();
    if (method) *method = head.substr(0, sp1);
    return head.substr(sp1 + 1, sp2 - sp1 - 1);
}

// Reads the body of a request whose head was read (Content-Length bytes) from c.in.
inline bool readBody(Conn& c, const std::string& head, std::string& body) {
    const size_t want = size_t(std::strtoull(headerOf(head, "content-length").c_str(), nullptr, 10));
    if (!c.need(want)) return false;
    body = c.in.substr(0, want);
    c.in.erase(0, want);
    return true;
}

// A whole HTTP answer with a body (Content-Length, Connection: close).
inline std::string httpAnswer(int status, const std::string& body, const std::string& extraHeaders = std::string(),
                              const char* type = "application/json") {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
}

// Sec-WebSocket-Accept for a Sec-WebSocket-Key.
inline std::string wsAccept(const std::string& key) {
    const std::string k = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const net::crypto::Sha1 d = net::crypto::sha1(k.data(), k.size());
    return net::crypto::base64(d.data(), d.size());
}

// The 101 answer to an upgrade request head; 'extra' adds header lines ("Name: value\r\n").
inline std::string wsUpgradeAnswer(const std::string& requestHead, const std::string& subprotocol, const std::string& extra = std::string()) {
    std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                    wsAccept(headerOf(requestHead, "sec-websocket-key")) + "\r\n";
    if (!subprotocol.empty()) r += "Sec-WebSocket-Protocol: " + subprotocol + "\r\n";
    return r + extra + "\r\n";
}

// A server frame (never masked), with the 7-, 16- or 64-bit length its size needs. 'firstByte'
// overrides FIN/RSV/opcode when not negative.
inline std::string serverFrame(int opcode, const std::string& payload, bool fin = true, int firstByte = -1) {
    std::string f;
    f += char(firstByte >= 0 ? firstByte : (fin ? 0x80 : 0x00) | opcode);
    const size_t n = payload.size();
    if (n < 126) {
        f += char(n);
    } else if (n < 65536) {
        f += char(126);
        f += char(n >> 8);
        f += char(n);
    } else {
        f += char(127);
        for (int i = 7; i >= 0; --i) f += char(uint64_t(n) >> (8 * i));
    }
    return f + payload;
}

// Takes one whole client frame (masked, as clients send them) from the front of 'in': its opcode,
// FIN bit and unmasked payload. 0 when it is not whole yet, -1 when it is not masked, 1 when taken.
inline int takeClientFrame(std::string& in, int& opcode, std::string& payload, bool* finOut = nullptr) {
    if (in.size() < 2) return 0;
    const uint8_t b1 = uint8_t(in[1]);
    if (!(b1 & 0x80)) return -1;
    uint64_t len = b1 & 0x7F;
    size_t hdr = 2;
    if (len == 126) {
        if (in.size() < 4) return 0;
        len = uint64_t(uint8_t(in[2])) << 8 | uint8_t(in[3]);
        hdr = 4;
    } else if (len == 127) {
        if (in.size() < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; ++i) len = len << 8 | uint8_t(in[2 + size_t(i)]);
        hdr = 10;
    }
    if (in.size() < hdr + 4 + len) return 0;
    opcode = uint8_t(in[0]) & 0x0F;
    if (finOut) *finOut = (uint8_t(in[0]) & 0x80) != 0;
    const char* mask = in.data() + hdr;
    payload.assign(in, hdr + 4, size_t(len));
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(uint8_t(payload[i]) ^ uint8_t(mask[i & 3]));
    in.erase(0, hdr + 4 + size_t(len));
    return 1;
}

// One client frame from c.in, waiting up to ms for it. False when none came, or when it is not
// masked.
inline bool readClientFrame(Conn& c, int& opcode, std::string& payload, int ms = 5000, bool* finOut = nullptr) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        const int r = takeClientFrame(c.in, opcode, payload, finOut);
        if (r != 0) return r > 0;
        const int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count());
        if (left <= 0 || !c.readMore(left)) return false;
    }
}

// The two bytes of a close frame's code.
inline std::string closePayload(uint16_t code, const std::string& reason = std::string()) {
    std::string p;
    p += char(code >> 8);
    p += char(code & 0xFF);
    return p + reason;
}

// Polls until cond() holds or ms pass.
inline bool eventually(const std::function<bool()>& cond, int ms = 5000) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (!cond()) {
        if (Clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

}  // namespace gaps
