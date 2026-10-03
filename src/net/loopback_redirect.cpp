#include "loopback_redirect.h"
#include "crypto.h"
#include "socket_util.h"
#include "../core/log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <thread>

namespace net {

std::string ssoOriginTag(const std::string& origin) {
    crypto::Sha256 d = crypto::sha256("scacelith-sso-origin-v1\n" + origin);
    return crypto::base64url(d.data(), d.size()).substr(0, 22);
}

std::string ssoRedirectPath(const std::string& origin) { return "/oauth2/google/" + ssoOriginTag(origin); }

// ---------------------------------------------------------------------------------------------
// The pure parts
// ---------------------------------------------------------------------------------------------

namespace loopback {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool allIn(const std::string& s, char lo, char hi) {
    return std::all_of(s.begin(), s.end(), [lo, hi](char c) { return c >= lo && c <= hi; });
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

std::string escapeHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

const char* reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 431: return "Request Header Fields Too Large";
    default: return "Error";
    }
}

}  // namespace

bool percentDecode(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%') {
            if (i + 2 >= in.size()) return false;
            int hi = hexValue(in[i + 1]), lo = hexValue(in[i + 2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return false;
            out += char(hi << 4 | lo);
            i += 2;
        } else {
            out += c;
        }
    }
    return true;
}

bool queryParams(const std::string& query, std::vector<std::pair<std::string, std::string>>& out) {
    out.clear();
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string part = query.substr(pos, amp - pos);
        pos = amp + 1;
        if (part.empty()) continue;
        size_t eq = part.find('=');
        std::string name, value;
        if (!percentDecode(part.substr(0, eq), name)) return false;
        if (eq != std::string::npos && !percentDecode(part.substr(eq + 1), value)) return false;
        out.emplace_back(std::move(name), std::move(value));
    }
    return true;
}

int parseRequestHead(const std::string& head, uint16_t port, const std::string& expectedPath, Request& out) {
    out = Request();
    if (head.size() > kMaxHead) return 431;
    size_t eol = head.find("\r\n");
    if (eol == std::string::npos) return 400;
    // "GET <target> HTTP/1.1": single spaces, the method in capitals (another method: 405).
    const std::string line = head.substr(0, eol);
    size_t sp1 = line.find(' '), sp2 = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos || line.find(' ', sp2 + 1) != std::string::npos) return 400;
    const std::string method = line.substr(0, sp1), target = line.substr(sp1 + 1, sp2 - sp1 - 1), version = line.substr(sp2 + 1);
    if (method.empty() || !allIn(method, 'A', 'Z')) return 400;
    if (version != "HTTP/1.1" && version != "HTTP/1.0") return 400;
    if (method != "GET") return 405;
    // Origin-form only (no absolute URI), printable, no fragment.
    if (target.empty() || target[0] != '/' || target.size() > kMaxTarget || !allIn(target, 0x21, 0x7e) ||
        target.find('#') != std::string::npos)
        return 400;
    // One Host header, exactly this listener (a DNS name pointing here, rebinding, is refused).
    int hosts = 0;
    for (size_t pos = eol + 2; pos < head.size();) {
        size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) return 400;
        if (e == pos) break;   // the blank line
        const std::string h = head.substr(pos, e - pos);
        pos = e + 2;
        size_t colon = h.find(':');
        if (colon == std::string::npos || colon == 0 || h[0] == ' ' || h[0] == '\t') return 400;
        if (!iequals(h.substr(0, colon), "host")) continue;
        size_t a = h.find_first_not_of(" \t", colon + 1), b = h.find_last_not_of(" \t");
        std::string value = a == std::string::npos ? std::string() : h.substr(a, b - a + 1);
        if (++hosts > 1 || !iequals(value, "127.0.0.1:" + std::to_string(port))) return 400;
    }
    if (hosts != 1) return 400;
    size_t q = target.find('?');
    if (target.substr(0, q) != expectedPath) return 404;
    std::vector<std::pair<std::string, std::string>> params;
    if (q != std::string::npos && !queryParams(target.substr(q + 1), params)) return 400;
    for (auto& p : params) {
        bool* has = p.first == "code" ? &out.hasCode : p.first == "state" ? &out.hasState
                  : p.first == "iss"  ? &out.hasIss  : p.first == "error" ? &out.hasError : nullptr;
        if (!has) continue;                    // scope, authuser, prompt, hd...
        if (*has) return 400;                  // twice
        *has = true;
        std::string& v = p.first == "code" ? out.code : p.first == "state" ? out.state : p.first == "iss" ? out.iss : out.error;
        v = std::move(p.second);
    }
    if (out.hasCode && (out.code.empty() || out.code.size() > kMaxCode || !allIn(out.code, 0x21, 0x7e))) return 400;
    if (out.hasIss && (out.iss.empty() || out.iss.size() > kMaxIss || !allIn(out.iss, 0x21, 0x7e))) return 400;
    if (out.hasError && (out.error.empty() || out.error.size() > kMaxError ||
                         !std::all_of(out.error.begin(), out.error.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; })))
        return 400;
    return 200;
}

std::string renderPage(const SsoBrowserPage& page, Page which) {
    const std::string& heading = which == Page::Done ? page.doneHeading : which == Page::Cancelled ? page.cancelledHeading : page.foreignHeading;
    const std::string& text = which == Page::Done ? page.done : which == Page::Cancelled ? page.cancelled : page.foreign;
    std::string html = "<!DOCTYPE html>\n<html lang=\"" + escapeHtml(page.lang) + "\" dir=\"" + (page.rtl ? "rtl" : "ltr") + "\">\n<head>\n";
    html += "<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    html += "<title>" + escapeHtml(page.title) + "</title>\n<link rel=\"icon\" href=\"data:,\">\n";
    html += "<style>\n"
            "html{color-scheme:dark}\n"
            "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#15171c;"
            "color:#e9e6df;font:17px/1.55 system-ui,-apple-system,\"Segoe UI\",Roboto,\"Noto Sans\",\"Helvetica Neue\",Arial,sans-serif}\n"
            "main{box-sizing:border-box;max-width:36rem;margin:24px;padding:32px 36px;background:#1e2128;border:1px solid #2e323b;"
            "border-radius:12px}\n"
            "h1{margin:0 0 14px;font-size:1.45rem;font-weight:600;color:#f3d9a4}\n"
            "p{margin:0;color:#c9c5bc}\n"
            "</style>\n</head>\n<body>\n<main>\n";
    html += "<h1>" + escapeHtml(heading) + "</h1>\n<p>" + escapeHtml(text) + "</p>\n</main>\n</body>\n</html>\n";
    return html;
}

std::string response(int status, const std::string& contentType, const std::string& body) {
    std::string r = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
    r += "Content-Type: " + (contentType.empty() ? std::string("text/plain; charset=utf-8") : contentType) + "\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Cache-Control: no-store\r\n"
         "Referrer-Policy: no-referrer\r\n"
         "X-Content-Type-Options: nosniff\r\n"
         "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; img-src data:; base-uri 'none'; "
         "form-action 'none'; frame-ancestors 'none'\r\n";
    if (status == 405) r += "Allow: GET\r\n";
    r += "Connection: close\r\n\r\n";
    return r + body;
}

}  // namespace loopback

// ---------------------------------------------------------------------------------------------
// LoopbackRedirect (worker 'net-sso')
// ---------------------------------------------------------------------------------------------

namespace {

constexpr size_t kMaxConnections = 8;        // browsers open speculative sockets beside the real one
constexpr int64_t kHeadMs = 5000;            // to send the whole request head
constexpr int64_t kSendMs = 2000;            // to take the answer
constexpr int64_t kLingerMs = 300;           // after the answer, for the browser to close first
constexpr int kMaxLogged = 16;               // "redirect received" lines per sign-in

}  // namespace

struct LoopbackRedirect::Impl {
    sock::Handle listener = sock::kInvalid;
    uint16_t port = 0;
    sock::Waker waker;
    std::thread thread;
    std::atomic<bool> stop{false};
    // done runs under doneMu, and cancel() raises 'cancelled' under it: once cancel() has it, done
    // has either run or never will.
    std::mutex doneMu;
    bool cancelled = false;
    std::function<void(const RedirectResult&)> done;

    std::string path, state;
    int64_t deadline = 0;
    SsoBrowserPage page;
    int logged = 0;

    struct Conn {
        sock::Handle h = sock::kInvalid;
        std::string peer;
        int64_t deadline = 0;
        bool received = false;               // a byte came (never evicted for a newcomer then)
        std::string in, out;
        bool answering = false, halfClosed = false;
        bool final = false;                  // the redirect that ends the sign-in
    };
    std::vector<Conn> conns;

    void closeConn(Conn& c) {
        sock::closeSocket(c.h);
        c.h = sock::kInvalid;
    }

    void closeListener() {
        sock::closeSocket(listener);
        listener = sock::kInvalid;
    }

    void acceptAll(int64_t now) {
        for (;;) {
            sock::Endpoint peer;
            sock::Handle h = sock::acceptOne(listener, &peer, true);
            if (h == sock::kInvalid) return;
            if (peer.ip().compare(0, 4, "127.") != 0) {
                sock::closeSocket(h);
                continue;
            }
            if (conns.size() >= kMaxConnections) {
                // Every slot taken: the oldest connection that has said nothing makes room (an idle
                // preconnect, or a local process holding the slots); when all have spoken, the
                // newcomer goes.
                auto idle = std::find_if(conns.begin(), conns.end(), [](const Conn& c) { return !c.received && !c.answering; });
                if (idle == conns.end()) {
                    sock::closeSocket(h);
                    continue;
                }
                closeConn(*idle);
                conns.erase(idle);
            }
            Conn c;
            c.h = h;
            c.peer = peer.toString();
            c.deadline = now + kHeadMs;
            conns.push_back(std::move(c));
        }
    }

    // A whole head (or too much of one) arrived: the answer, and the end of the sign-in when it is
    // the redirect that carries its state.
    void answer(Conn& c, int64_t now, RedirectResult& result, bool& finished) {
        size_t end = c.in.find("\r\n\r\n");
        if (end != std::string::npos) c.in.resize(end + 4);   // a GET has no body
        loopback::Request req;
        int status = loopback::parseRequestHead(c.in, port, path, req);
        const std::string html = "text/html; charset=utf-8";
        if (status != 200) {
            c.out = loopback::response(status, std::string(), std::string());
        } else if (!req.hasState || !crypto::constantTimeEqual(req.state, state) || (!req.hasCode && !req.hasError)) {
            // Not the sign-in this game waits for (a link someone sent, another tab): it goes on.
            status = 400;
            c.out = loopback::response(status, html, loopback::renderPage(page, loopback::Page::Foreign));
        } else {
            result = RedirectResult();
            result.state = req.state;
            if (req.hasError) {
                result.kind = RedirectResult::Kind::ProviderError;
                result.error = req.error;
            } else {
                result.kind = RedirectResult::Kind::Code;
                result.code = req.code;
                result.iss = req.iss;
            }
            c.out = loopback::response(status, html, loopback::renderPage(page, req.hasError ? loopback::Page::Cancelled : loopback::Page::Done));
            c.final = true;
            finished = true;
            // Nothing more is served: a reload is refused at once.
            closeListener();
            for (Conn& o : conns)
                if (&o != &c) closeConn(o);
        }
        if (logged < kMaxLogged) {
            ++logged;
            LOGI("sso: redirect received (%d) from %s", status, c.peer.c_str());
        }
        c.in.clear();
        c.answering = true;
        c.deadline = now + kSendMs;
    }

    // Reads, writes and closes one connection as far as it can go now; false once it is closed.
    bool service(Conn& c, int64_t now, const sock::PollSet& ps, RedirectResult& result, bool& finished) {
        if (c.h == sock::kInvalid) return false;
        if (!c.answering && ps.readable(c.h)) {
            uint8_t buf[4096];
            for (;;) {
                bool closed = false;
                int r = sock::recvSome(c.h, buf, sizeof buf, closed);
                if (r < 0) {
                    closeConn(c);
                    return false;
                }
                if (r == 0) break;
                c.received = true;
                c.in.append(reinterpret_cast<const char*>(buf), size_t(r));
                if (c.in.find("\r\n\r\n") != std::string::npos || c.in.size() > loopback::kMaxHead) {
                    answer(c, now, result, finished);
                    break;
                }
            }
        }
        if (c.answering && !c.halfClosed) {
            while (!c.out.empty()) {
                int r = sock::sendSome(c.h, reinterpret_cast<const uint8_t*>(c.out.data()), c.out.size());
                if (r < 0) {
                    closeConn(c);
                    return false;
                }
                if (r == 0) break;
                c.out.erase(0, size_t(r));
            }
            if (c.out.empty()) {
                sock::shutdownSend(c.h);
                c.halfClosed = true;
                c.deadline = std::min(c.deadline, now + kLingerMs);
            }
        }
        if (c.halfClosed) {
            // Until the browser closes: closing with its bytes unread could reset the connection
            // before it has read the page.
            uint8_t buf[1024];
            for (int i = 0; i < 64; ++i) {
                bool closed = false;
                int r = sock::recvSome(c.h, buf, sizeof buf, closed);
                if (r < 0) {
                    closeConn(c);
                    return false;
                }
                if (r == 0) break;
            }
        }
        if (now >= c.deadline) {
            closeConn(c);
            return false;
        }
        return true;
    }

    void run() {
        RedirectResult result;
        bool finished = false;
        while (!stop) {
            int64_t now = sock::steadyMs();
            if (!finished && now >= deadline) break;   // Expired
            int64_t wait = 1000;
            auto until = [&](int64_t t) { wait = std::min(wait, std::max<int64_t>(0, t - now)); };
            if (!finished) until(deadline);
            for (const Conn& c : conns) until(c.deadline);
            sock::PollSet ps;
            ps.add(waker.handle(), true, false);
            if (listener != sock::kInvalid) ps.add(listener, true, false);
            for (const Conn& c : conns) ps.add(c.h, !c.answering || c.halfClosed, c.answering && !c.halfClosed);
            ps.wait(int(wait));
            if (ps.readable(waker.handle())) waker.drain();
            if (stop) break;
            now = sock::steadyMs();
            if (listener != sock::kInvalid && ps.readable(listener)) acceptAll(now);
            bool finalOpen = false;
            for (size_t i = 0; i < conns.size();) {
                if (service(conns[i], now, ps, result, finished)) {
                    finalOpen = finalOpen || conns[i].final;
                    ++i;
                } else {
                    conns.erase(conns.begin() + long(i));
                }
            }
            // The page of the redirect is out (or its time is up): the sign-in's result goes.
            if (finished && !finalOpen) break;
        }
        closeListener();
        for (Conn& c : conns) closeConn(c);
        conns.clear();
        if (stop) return;
        if (!finished) result = RedirectResult();   // Expired
        std::lock_guard<std::mutex> lk(doneMu);
        if (!cancelled && done) done(result);
    }
};

LoopbackRedirect::LoopbackRedirect() : impl_(std::make_unique<Impl>()) {}

LoopbackRedirect::~LoopbackRedirect() { cancel(); }

bool LoopbackRedirect::open(std::string& err) {
    Impl& d = *impl_;
    if (d.listener != sock::kInvalid) return true;
    d.listener = sock::listenLoopbackV4(d.port, err);
    if (d.listener == sock::kInvalid) {
        d.port = 0;
        return false;
    }
    if (!d.waker.valid()) {
        err = "waker";
        d.closeListener();
        d.port = 0;
        return false;
    }
    return true;
}

uint16_t LoopbackRedirect::port() const { return impl_->port; }

void LoopbackRedirect::start(std::string expectedPath, std::string expectedState, int64_t deadlineSteadyMs, SsoBrowserPage page,
                             std::function<void(const RedirectResult&)> done) {
    Impl& d = *impl_;
    if (d.thread.joinable() || d.stop) return;
    d.path = std::move(expectedPath);
    d.state = std::move(expectedState);
    d.deadline = deadlineSteadyMs;
    d.page = std::move(page);
    d.done = std::move(done);
    d.thread = std::thread([&d] { d.run(); });
}

void LoopbackRedirect::cancel() {
    Impl& d = *impl_;
    {
        std::lock_guard<std::mutex> lk(d.doneMu);
        d.cancelled = true;
    }
    d.stop = true;
    d.waker.wake();
    if (d.thread.joinable()) d.thread.join();
    d.closeListener();
}

}  // namespace net
