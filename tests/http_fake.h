// A small scripted HTTP/1.1 server on the loopback interface for the download tests (plain HTTP
// is allowed to loopback hosts only, net/transport.h). One thread, one connection at a time; the
// handler decides every answer, so a test can play a file host: redirects, Range, a connection
// cut in the middle of a body, a slow or silent server.
#pragma once
#include "net/socket_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fakehttp {

struct Request {
    std::string method, path;
    std::vector<std::pair<std::string, std::string>> headers;   // names lower-case
    std::string get(const std::string& name) const {
        for (auto& h : headers)
            if (h.first == name) return h.second;
        return std::string();
    }
    bool has(const std::string& name) const {
        for (auto& h : headers)
            if (h.first == name) return true;
        return false;
    }
};

struct Reply {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool chunked = false;           // Transfer-Encoding: chunked
    bool noLength = false;          // neither Content-Length nor chunked: the body ends with the connection
    size_t cutAfter = SIZE_MAX;     // close the connection after this many bytes of the body
    int pieceDelayMs = 0;           // pause between 16 KiB pieces of the body (a slow server)
    int silenceMs = 0;              // say nothing for this long before answering
};

// Answer for a file as a CDN gives it: the whole file (200) or, for "Range: bytes=N-", the rest
// from N (206 with Content-Range); 416 when N is past the end.
inline Reply fileReply(const std::string& data, const Request& r, bool honourRange = true) {
    Reply rep;
    std::string range = r.get("range");
    if (honourRange && range.compare(0, 6, "bytes=") == 0) {
        size_t from = size_t(std::strtoull(range.c_str() + 6, nullptr, 10));
        if (from >= data.size()) {
            rep.status = 416;
            rep.headers.emplace_back("Content-Range", "bytes */" + std::to_string(data.size()));
            return rep;
        }
        rep.status = 206;
        rep.headers.emplace_back("Content-Range",
                                 "bytes " + std::to_string(from) + "-" + std::to_string(data.size() - 1) + "/" + std::to_string(data.size()));
        rep.body = data.substr(from);
        return rep;
    }
    rep.body = data;
    rep.headers.emplace_back("Accept-Ranges", "bytes");
    return rep;
}

class Server {
public:
    using Handler = std::function<Reply(const Request&)>;

    explicit Server(Handler h) : handler_(std::move(h)) {
        bool dual = false;
        std::string err;
        listener_ = net::sock::listenTcp(0, dual, err);
        net::sock::Endpoint ep;
        if (listener_ == net::sock::kInvalid || !net::sock::localEndpoint(listener_, ep)) return;
        port_ = ep.port();
        thread_ = std::thread([this] { run(); });
    }
    ~Server() {
        stop_ = true;
        waker_.wake();
        if (thread_.joinable()) thread_.join();
        net::sock::closeSocket(listener_);
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool ok() const { return port_ != 0; }
    uint16_t port() const { return port_; }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
    }
    void setHandler(Handler h) {
        std::lock_guard<std::mutex> lk(mu_);
        handler_ = std::move(h);
    }

private:
    Handler handler_;
    net::sock::Handle listener_ = net::sock::kInvalid;
    net::sock::Waker waker_;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::vector<Request> requests_;

    void run() {
        while (!stop_) {
            net::sock::PollSet ps;
            ps.add(listener_, true, false);
            if (waker_.valid()) ps.add(waker_.handle(), true, false);
            ps.wait(200);
            if (waker_.valid() && ps.readable(waker_.handle())) waker_.drain();
            if (stop_) break;
            if (!ps.readable(listener_)) continue;
            net::sock::Handle c = net::sock::acceptOne(listener_, nullptr);
            if (c == net::sock::kInvalid) continue;
            serve(c);
            net::sock::closeSocket(c);
        }
    }

    // Waits until the socket is readable/writable (or 'ms' passed, or the server stops).
    bool waitFor(net::sock::Handle c, bool write, int ms) {
        net::sock::PollSet ps;
        ps.add(c, !write, write);
        return ps.wait(ms) > 0 && !stop_;
    }

    bool sendAll(net::sock::Handle c, const char* p, size_t n) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (n) {
            int r = net::sock::sendSome(c, reinterpret_cast<const uint8_t*>(p), n);
            if (r < 0) return false;
            if (r == 0) {
                if (std::chrono::steady_clock::now() > deadline || !waitFor(c, true, 100)) {
                    if (stop_ || std::chrono::steady_clock::now() > deadline) return false;
                }
                continue;
            }
            p += r;
            n -= size_t(r);
        }
        return true;
    }

    void serve(net::sock::Handle c) {
        std::string raw;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (raw.find("\r\n\r\n") == std::string::npos) {
            if (std::chrono::steady_clock::now() > deadline || stop_) return;
            uint8_t buf[4096];
            bool closed = false;
            int r = net::sock::recvSome(c, buf, sizeof buf, closed);
            if (r > 0) raw.append(reinterpret_cast<char*>(buf), size_t(r));
            else if (closed || r < 0) return;
            else waitFor(c, false, 100);
        }
        Request req;
        size_t eol = raw.find("\r\n");
        std::string line = raw.substr(0, eol);
        size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
        req.method = line.substr(0, sp1);
        req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
        size_t pos = eol + 2, end = raw.find("\r\n\r\n");
        while (pos < end) {
            size_t e = raw.find("\r\n", pos);
            std::string h = raw.substr(pos, e - pos);
            size_t colon = h.find(':');
            if (colon != std::string::npos) {
                std::string name = h.substr(0, colon), value = h.substr(colon + 1);
                for (char& ch : name) ch = char(std::tolower((unsigned char)ch));
                size_t a = value.find_first_not_of(' ');
                req.headers.emplace_back(name, a == std::string::npos ? std::string() : value.substr(a));
            }
            pos = e + 2;
        }
        Handler h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            requests_.push_back(req);
            h = handler_;
        }
        Reply rep = h(req);
        for (int t = 0; t < rep.silenceMs && !stop_; t += 20) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::string head = "HTTP/1.1 " + std::to_string(rep.status) + " Status\r\n";
        for (auto& kv : rep.headers) head += kv.first + ": " + kv.second + "\r\n";
        if (rep.chunked) head += "Transfer-Encoding: chunked\r\n";
        else if (!rep.noLength) head += "Content-Length: " + std::to_string(rep.body.size()) + "\r\n";
        head += "Connection: close\r\n\r\n";
        if (!sendAll(c, head.data(), head.size())) return;
        size_t limit = std::min(rep.cutAfter, rep.body.size()), sent = 0;
        while (sent < limit) {
            size_t k = std::min<size_t>(16384, limit - sent);
            std::string piece = rep.body.substr(sent, k);
            if (rep.chunked) {
                char size[32];
                std::snprintf(size, sizeof size, "%zx\r\n", k);
                piece = size + piece + "\r\n";
            }
            if (!sendAll(c, piece.data(), piece.size())) return;
            sent += k;
            for (int t = 0; t < rep.pieceDelayMs && !stop_; t += 10) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (rep.chunked && sent == rep.body.size()) sendAll(c, "0\r\n\r\n", 5);
        net::sock::shutdownSend(c);
        // Let the client read everything before the socket closes (a reset could drop the end).
        for (int i = 0; i < 20; ++i) {
            uint8_t buf[256];
            bool closed = false;
            int r = net::sock::recvSome(c, buf, sizeof buf, closed);
            if (closed || r < 0) break;
            if (r == 0) waitFor(c, false, 50);
        }
    }
};

}  // namespace fakehttp
