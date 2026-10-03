// The Google sign-in's loopback redirect (net/loopback_redirect.h): the strict parser of the
// redirect (Google's real request shape, the request line, the target, the Host header, the query),
// the pages and their headers, the origin tag's contract vectors; then the listener on real
// sockets: bound to 127.0.0.1 only, one redirect served and the port closed after it, a request of
// another sign-in answered 'foreign' while it goes on waiting, Google's error, idle sockets of a
// browser's preconnect (one beside the request, the slots all taken), the deadline, and cancel()
// (before anything, and racing a request). The same file runs on Linux and under Wine
// (tools/test_win.sh sso_loopback).
#include "test.h"
#include "net/loopback_redirect.h"
#include "net/socket_util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace net;

namespace {

const std::string kTag = ssoOriginTag("play.scacelith.example:443");
const std::string kPath = "/oauth2/google/" + kTag;
const std::string kState = "S7qLm0x_Rf-2hTzYw9KbVnPcD4eGaU1oJiXsQ8yZt3A";   // 43 base64url

std::string get(const std::string& target, uint16_t port, const std::string& version = "HTTP/1.1") {
    return "GET " + target + " " + version + "\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nUser-Agent: test\r\n\r\n";
}

int parse(const std::string& head, uint16_t port, loopback::Request* out = nullptr) {
    loopback::Request r;
    int s = loopback::parseRequestHead(head, port, kPath, r);
    if (out) *out = r;
    return s;
}

SsoBrowserPage testPage() {
    SsoBrowserPage p;
    p.title = "Scacelith: Google sign-in";
    p.doneHeading = "Back to Scacelith";
    p.done = "Return to the game.";
    p.cancelledHeading = "Sign-in cancelled";
    p.cancelled = "Google sign-in was cancelled.";
    p.foreignHeading = "This page is not for you";
    p.foreign = "This page does not match.";
    return p;
}

// What the worker reports, for the game's side of the tests.
struct Outcome {
    std::mutex m;
    std::condition_variable cv;
    std::vector<RedirectResult> results;
    void add(const RedirectResult& r) {
        std::lock_guard<std::mutex> lk(m);
        results.push_back(r);
        cv.notify_all();
    }
    bool wait(int ms) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return !results.empty(); });
    }
    size_t count() {
        std::lock_guard<std::mutex> lk(m);
        return results.size();
    }
};

void startListener(LoopbackRedirect& l, Outcome& o, int64_t ms = 10000) {
    l.start(kPath, kState, sock::steadyMs() + ms, testPage(), [&o](const RedirectResult& r) { o.add(r); });
}

sock::Handle connectTo(uint16_t port) {
    sock::Endpoint ep;
    if (!sock::Endpoint::parse("127.0.0.1", port, ep)) return sock::kInvalid;
    std::string err;
    return sock::connectWithTimeout(ep, 2000, err);
}

bool sendAll(sock::Handle h, const std::string& s) {
    size_t off = 0;
    int64_t end = sock::steadyMs() + 3000;
    while (off < s.size() && sock::steadyMs() < end) {
        int r = sock::sendSome(h, reinterpret_cast<const uint8_t*>(s.data()) + off, s.size() - off);
        if (r < 0) return false;
        if (r == 0) {
            sock::PollSet ps;
            ps.add(h, false, true);
            ps.wait(50);
            continue;
        }
        off += size_t(r);
    }
    return off == s.size();
}

// Everything the other side sends until it closes (or 'ms' passed).
std::string readAll(sock::Handle h, int ms = 3000, bool* closedOut = nullptr) {
    std::string out;
    int64_t end = sock::steadyMs() + ms;
    bool closed = false;
    while (sock::steadyMs() < end) {
        sock::PollSet ps;
        ps.add(h, true, false);
        ps.wait(50);
        uint8_t buf[4096];
        int r = sock::recvSome(h, buf, sizeof buf, closed);
        if (r > 0) out.append(reinterpret_cast<char*>(buf), size_t(r));
        else if (r < 0) {
            closed = true;
            break;
        }
    }
    if (closedOut) *closedOut = closed;
    return out;
}

// One request on a new connection; the whole answer.
std::string request(uint16_t port, const std::string& head) {
    sock::Handle h = connectTo(port);
    if (h == sock::kInvalid) return std::string();
    std::string ans = sendAll(h, head) ? readAll(h) : std::string();
    sock::closeSocket(h);
    return ans;
}

std::string redirectTarget(const std::string& state, const std::string& rest = "&code=4%2F0AbCdEf-gh") {
    return kPath + "?state=" + state + rest;
}

}  // namespace

// ---- the parser ---------------------------------------------------------------------------------

TEST(sso_loopback_parse_google_shape) {
    const uint16_t port = 50123;
    loopback::Request r;
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&iss=https%3A%2F%2Faccounts.google.com&code=4%2F0AbCdEf-gh_ij"
                       "&scope=email+profile+openid+https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fuserinfo.email&authuser=0&prompt=consent",
                       port),
                   port, &r),
             200);
    CHECK(r.hasCode && r.hasState && r.hasIss && !r.hasError);
    CHECK_EQ(r.code, std::string("4/0AbCdEf-gh_ij"));
    CHECK_EQ(r.state, kState);
    CHECK_EQ(r.iss, std::string("https://accounts.google.com"));
    // HTTP/1.0, a Host in capitals, Google's error.
    CHECK_EQ(parse(get(redirectTarget(kState), port, "HTTP/1.0"), port), 200);
    CHECK_EQ(parse("GET " + redirectTarget(kState) + " HTTP/1.1\r\nHOST:  127.0.0.1:50123 \r\n\r\n", port), 200);
    CHECK_EQ(parse(get(kPath + "?error=access_denied&state=" + kState, port), port, &r), 200);
    CHECK(r.hasError && !r.hasCode);
    CHECK_EQ(r.error, std::string("access_denied"));
}

TEST(sso_loopback_parse_request_line) {
    const uint16_t port = 41000;
    const std::string t = redirectTarget(kState);
    CHECK_EQ(parse("get " + t + " HTTP/1.1\r\nHost: 127.0.0.1:41000\r\n\r\n", port), 400);
    CHECK_EQ(parse("POST " + t + " HTTP/1.1\r\nHost: 127.0.0.1:41000\r\n\r\n", port), 405);
    CHECK_EQ(parse("GET  " + t + " HTTP/1.1\r\nHost: 127.0.0.1:41000\r\n\r\n", port), 400);
    CHECK_EQ(parse("GET " + t + " HTTP/2\r\nHost: 127.0.0.1:41000\r\n\r\n", port), 400);
    CHECK_EQ(parse("GET " + t + "\r\nHost: 127.0.0.1:41000\r\n\r\n", port), 400);
    // Origin-form only, no fragment.
    CHECK_EQ(parse(get("http://127.0.0.1:41000" + t, port), port), 400);
    CHECK_EQ(parse(get(t + "#x", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=" + std::string(4100, 'a'), port), port), 400);
    // Another path.
    CHECK_EQ(parse(get(kPath + "/?state=" + kState + "&code=x", port), port), 404);
    CHECK_EQ(parse(get("/oauth2/google/" + ssoOriginTag("localhost:8443") + "?state=" + kState + "&code=x", port), port), 404);
    CHECK_EQ(parse(get("/favicon.ico", port), port), 404);
    CHECK_EQ(parse(get("/", port), port), 404);
}

TEST(sso_loopback_parse_query) {
    const uint16_t port = 41000;
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=a&code=b", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&state=" + kState + "&code=a", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=a&iss=x&iss=y", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&error=a&error=b", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=a%zzb", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=ab%4", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=ab%", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=a%00b", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=a%20b", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=a+b", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=a%01b", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=" + std::string(2048, 'c'), port), port), 200);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=" + std::string(2049, 'c'), port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&code=a&iss=" + std::string(257, 'i'), port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&error=Access_Denied", port), port), 400);
    CHECK_EQ(parse(get(kPath + "?state=" + kState + "&error=" + std::string(65, 'e'), port), port), 400);
    // Unknown parameters, even repeated or odd, are ignored.
    CHECK_EQ(parse(get(kPath + "?x=1&x=2&&flag&state=" + kState + "&code=a", port), port), 200);

    std::string s;
    CHECK(loopback::percentDecode("a%2Fb+c%7e", s));
    CHECK_EQ(s, std::string("a/b c~"));
    CHECK(!loopback::percentDecode("%G1", s));
    CHECK(!loopback::percentDecode("%1", s));
    CHECK(!loopback::percentDecode("%00", s));
    std::vector<std::pair<std::string, std::string>> q;
    CHECK(loopback::queryParams("a=1&b=&c&d=x%3Dy", q));
    CHECK_EQ(q.size(), size_t(4));
    if (q.size() == 4) {
        CHECK_EQ(q[0].second, std::string("1"));
        CHECK_EQ(q[2].first, std::string("c"));
        CHECK_EQ(q[3].second, std::string("x=y"));
    }
}

TEST(sso_loopback_parse_host) {
    const uint16_t port = 41000;
    const std::string line = "GET " + redirectTarget(kState) + " HTTP/1.1\r\n";
    CHECK_EQ(parse(line + "User-Agent: test\r\n\r\n", port), 400);
    CHECK_EQ(parse(line + "Host: localhost:41000\r\n\r\n", port), 400);
    CHECK_EQ(parse(line + "Host: evil.example:41000\r\n\r\n", port), 400);
    CHECK_EQ(parse(line + "Host: 127.0.0.1:41001\r\n\r\n", port), 400);
    CHECK_EQ(parse(line + "Host: 127.0.0.1\r\n\r\n", port), 400);
    CHECK_EQ(parse(line + "Host: 127.0.0.1:41000\r\nHost: 127.0.0.1:41000\r\n\r\n", port), 400);
    CHECK_EQ(parse(line + "Host 127.0.0.1:41000\r\n\r\n", port), 400);
    CHECK_EQ(parse(line + "Host: 127.0.0.1:41000\r\nCookie: a=b\r\n\r\n", port), 200);
    // A 64 KiB head (the cookies of other 127.0.0.1 applications fit under it).
    CHECK_EQ(parse(line + "Host: 127.0.0.1:41000\r\nCookie: " + std::string(60000, 'k') + "\r\n\r\n", port), 200);
    CHECK_EQ(parse(line + "Host: 127.0.0.1:41000\r\nCookie: " + std::string(64 * 1024, 'k') + "\r\n\r\n", port), 431);
}

TEST(sso_loopback_page_and_headers) {
    SsoBrowserPage p = testPage();
    p.doneHeading = "<script>alert(1)</script>";
    p.done = "Tom & \"Jerry\" 'x'";
    std::string html = loopback::renderPage(p, loopback::Page::Done);
    CHECK(html.find("<script>") == std::string::npos);
    CHECK(html.find("&lt;script&gt;alert(1)&lt;/script&gt;") != std::string::npos);
    CHECK(html.find("Tom &amp; &quot;Jerry&quot; &#39;x&#39;") != std::string::npos);
    CHECK(html.find("<html lang=\"en\" dir=\"ltr\">") != std::string::npos);
    CHECK(html.find("<link rel=\"icon\" href=\"data:,\">") != std::string::npos);
    CHECK(html.find("src=") == std::string::npos && html.find("http") == std::string::npos);   // loads nothing
    p.lang = "ar";
    p.rtl = true;
    html = loopback::renderPage(p, loopback::Page::Foreign);
    CHECK(html.find("<html lang=\"ar\" dir=\"rtl\">") != std::string::npos);
    CHECK(html.find(p.foreignHeading) != std::string::npos);
    CHECK(loopback::renderPage(p, loopback::Page::Cancelled).find(p.cancelled) != std::string::npos);

    std::string r = loopback::response(200, "text/html; charset=utf-8", "<p>x</p>");
    CHECK_EQ(r.compare(0, 17, "HTTP/1.1 200 OK\r\n"), 0);
    CHECK(r.find("\r\nContent-Type: text/html; charset=utf-8\r\n") != std::string::npos);
    CHECK(r.find("\r\nContent-Length: 8\r\n") != std::string::npos);
    CHECK(r.find("\r\nCache-Control: no-store\r\n") != std::string::npos);
    CHECK(r.find("\r\nX-Content-Type-Options: nosniff\r\n") != std::string::npos);
    CHECK(r.find("\r\nReferrer-Policy: no-referrer\r\n") != std::string::npos);
    CHECK(r.find("\r\nContent-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; img-src data:; base-uri 'none'; "
                 "form-action 'none'; frame-ancestors 'none'\r\n") != std::string::npos);
    CHECK(r.find("\r\nConnection: close\r\n\r\n<p>x</p>") != std::string::npos);
    CHECK(r.find("Access-Control") == std::string::npos);
    std::string nf = loopback::response(404, "", "");
    CHECK_EQ(nf.compare(0, 24, "HTTP/1.1 404 Not Found\r\n"), 0);
    CHECK(nf.find("\r\nContent-Type: text/plain; charset=utf-8\r\n") != std::string::npos);
    CHECK(nf.find("\r\nContent-Length: 0\r\n") != std::string::npos);
    CHECK(loopback::response(405, "", "").find("\r\nAllow: GET\r\n") != std::string::npos);
}

// The frozen contract's vectors (the server's tests assert the same ones).
TEST(sso_loopback_origin_tag_vectors) {
    CHECK_EQ(ssoOriginTag("play.scacelith.example:443"), std::string("IhcScoV7eDOzTEcSnqPUPt"));
    CHECK_EQ(ssoOriginTag("localhost:8443"), std::string("TFGx7zQ_8QlGZW5zpqznCr"));
    CHECK_EQ(ssoOriginTag("[::1]:8443"), std::string("XToJm0DG5PjciEVmZa9Cho"));
    CHECK_EQ(ssoOriginTag("127.0.0.1:50443"), std::string("3r653wM5ZjYsHcAJljmCwY"));
    CHECK_EQ(ssoRedirectPath("localhost:8443"), std::string("/oauth2/google/TFGx7zQ_8QlGZW5zpqznCr"));
}

// ---- the listener -------------------------------------------------------------------------------

TEST(sso_loopback_binds_loopback_only) {
    LoopbackRedirect l;
    CHECK_EQ(l.port(), uint16_t(0));
    std::string err;
    REQUIRE(l.open(err));
    const uint16_t port = l.port();
    CHECK(port >= 1024);
    sock::Handle h = connectTo(port);
    CHECK(h != sock::kInvalid);
    sock::closeSocket(h);
    // Not reachable through the machine's other addresses.
    for (const std::string& a : sock::localAddresses(true, false)) {
        sock::Endpoint ep;
        if (!sock::Endpoint::parse(a, port, ep)) continue;
        std::string e;
        sock::Handle o = sock::connectWithTimeout(ep, 1000, e);
        CHECK(o == sock::kInvalid);
        if (o != sock::kInvalid) {
            std::fprintf(stderr, "  reached through %s\n", a.c_str());
            sock::closeSocket(o);
        }
    }
}

TEST(sso_loopback_code_once_then_refused) {
    Outcome o;   // outlives the worker that reports to it
    LoopbackRedirect l;
    std::string err;
    REQUIRE(l.open(err));
    const uint16_t port = l.port();
    startListener(l, o);
    std::string ans = request(port, get(redirectTarget(kState, "&iss=https%3A%2F%2Faccounts.google.com&code=4%2F0AbC"), port));
    CHECK_EQ(ans.compare(0, 17, "HTTP/1.1 200 OK\r\n"), 0);
    CHECK(ans.find("<h1>Back to Scacelith</h1>") != std::string::npos);
    CHECK(o.wait(3000));
    REQUIRE(o.count() == 1);
    CHECK(o.results[0].kind == RedirectResult::Kind::Code);
    CHECK_EQ(o.results[0].code, std::string("4/0AbC"));
    CHECK_EQ(o.results[0].iss, std::string("https://accounts.google.com"));
    CHECK_EQ(o.results[0].state, kState);
    // The port is closed: a reload is refused, and done is not called again.
    sock::Handle h = connectTo(port);
    CHECK(h == sock::kInvalid);
    sock::closeSocket(h);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(o.count(), size_t(1));
    l.cancel();
    CHECK_EQ(o.count(), size_t(1));
}

TEST(sso_loopback_foreign_state_keeps_listening) {
    Outcome o;
    LoopbackRedirect l;
    std::string err;
    REQUIRE(l.open(err));
    const uint16_t port = l.port();
    startListener(l, o);
    std::string other = kState;
    other[5] = other[5] == 'x' ? 'y' : 'x';
    std::string ans = request(port, get(redirectTarget(other), port));
    CHECK_EQ(ans.compare(0, 26, "HTTP/1.1 400 Bad Request\r\n"), 0);
    CHECK(ans.find("<h1>This page is not for you</h1>") != std::string::npos);
    // No state, or the state without code nor error: 'foreign' too.
    CHECK(request(port, get(kPath + "?code=abc", port)).find("This page is not for you") != std::string::npos);
    CHECK(request(port, get(kPath + "?state=" + kState, port)).find("This page is not for you") != std::string::npos);
    // Another path, another Host: plain answers.
    CHECK_EQ(request(port, get("/favicon.ico", port)).compare(0, 22, "HTTP/1.1 404 Not Found"), 0);
    CHECK_EQ(request(port, "GET " + redirectTarget(kState) + " HTTP/1.1\r\nHost: localhost:" + std::to_string(port) + "\r\n\r\n")
                 .compare(0, 24, "HTTP/1.1 400 Bad Request"),
             0);
    CHECK_EQ(o.count(), size_t(0));
    ans = request(port, get(redirectTarget(kState), port));
    CHECK(ans.find("<h1>Back to Scacelith</h1>") != std::string::npos);
    CHECK(o.wait(3000));
    REQUIRE(o.count() == 1);
    CHECK(o.results[0].kind == RedirectResult::Kind::Code);
    CHECK_EQ(o.results[0].code, std::string("4/0AbCdEf-gh"));
}

TEST(sso_loopback_provider_error) {
    Outcome o;
    LoopbackRedirect l;
    std::string err;
    REQUIRE(l.open(err));
    const uint16_t port = l.port();
    startListener(l, o);
    std::string ans = request(port, get(kPath + "?error=access_denied&state=" + kState, port));
    CHECK_EQ(ans.compare(0, 17, "HTTP/1.1 200 OK\r\n"), 0);
    CHECK(ans.find("<h1>Sign-in cancelled</h1>") != std::string::npos);
    CHECK(o.wait(3000));
    REQUIRE(o.count() == 1);
    CHECK(o.results[0].kind == RedirectResult::Kind::ProviderError);
    CHECK_EQ(o.results[0].error, std::string("access_denied"));
    CHECK(o.results[0].code.empty());
}

// A browser's speculative socket stays idle beside the one that carries the request.
TEST(sso_loopback_idle_preconnect) {
    Outcome o;
    LoopbackRedirect l;
    std::string err;
    REQUIRE(l.open(err));
    const uint16_t port = l.port();
    startListener(l, o);
    sock::Handle idle = connectTo(port);
    CHECK(idle != sock::kInvalid);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int64_t t0 = sock::steadyMs();
    std::string ans = request(port, get(redirectTarget(kState), port));
    CHECK(ans.find("<h1>Back to Scacelith</h1>") != std::string::npos);
    CHECK(o.wait(3000));
    CHECK(sock::steadyMs() - t0 < 1000);
    sock::closeSocket(idle);
}

// Nine idle sockets: the oldest one makes room, and the real request still gets its page.
TEST(sso_loopback_idle_slots_evicted) {
    Outcome o;
    LoopbackRedirect l;
    std::string err;
    REQUIRE(l.open(err));
    const uint16_t port = l.port();
    startListener(l, o);
    std::vector<sock::Handle> idle;
    for (int i = 0; i < 9; ++i) {
        idle.push_back(connectTo(port));
        CHECK(idle.back() != sock::kInvalid);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));   // accepted in this order
    }
    bool closed = false;
    readAll(idle[0], 1000, &closed);
    CHECK(closed);   // evicted by the ninth
    std::string ans = request(port, get(redirectTarget(kState), port));
    CHECK(ans.find("<h1>Back to Scacelith</h1>") != std::string::npos);
    CHECK(o.wait(3000));
    for (sock::Handle h : idle) sock::closeSocket(h);
}

TEST(sso_loopback_deadline_expires) {
    Outcome o;
    LoopbackRedirect l;
    std::string err;
    REQUIRE(l.open(err));
    startListener(l, o, 300);
    CHECK(o.wait(3000));
    REQUIRE(o.count() == 1);
    CHECK(o.results[0].kind == RedirectResult::Kind::Expired);
    CHECK(connectTo(l.port()) == sock::kInvalid);
}

TEST(sso_loopback_cancel) {
    std::atomic<int> calls{0};
    {
        LoopbackRedirect l;
        std::string err;
        REQUIRE(l.open(err));
        l.start(kPath, kState, sock::steadyMs() + 10000, testPage(), [&](const RedirectResult&) { ++calls; });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint16_t port = l.port();
        l.cancel();
        l.cancel();
        CHECK(connectTo(port) == sock::kInvalid);
    }
    {
        LoopbackRedirect l;   // never started: the destructor closes it
        std::string err;
        CHECK(l.open(err));
    }
    {
        LoopbackRedirect l;   // started, destroyed without cancel()
        std::string err;
        REQUIRE(l.open(err));
        l.start(kPath, kState, sock::steadyMs() + 10000, testPage(), [&](const RedirectResult&) { ++calls; });
    }
    CHECK_EQ(calls.load(), 0);
}

// cancel() while a redirect is on its way: no crash, done at most once, and never after cancel().
TEST(sso_loopback_cancel_races_request) {
    for (int i = 0; i < 100; ++i) {
        std::atomic<int> calls{0}, after{0};
        std::atomic<bool> cancelled{false};
        LoopbackRedirect l;
        std::string err;
        REQUIRE(l.open(err));
        const uint16_t port = l.port();
        l.start(kPath, kState, sock::steadyMs() + 10000, testPage(), [&](const RedirectResult&) {
            ++calls;
            if (cancelled) ++after;
        });
        std::thread client([port] {
            sock::Handle h = connectTo(port);
            if (h == sock::kInvalid) return;
            if (sendAll(h, get(redirectTarget(kState), port))) readAll(h, 500);
            sock::closeSocket(h);
        });
        std::this_thread::sleep_for(std::chrono::microseconds(50 * (i % 20)));
        l.cancel();
        cancelled = true;
        client.join();
        CHECK(calls.load() <= 1);
        CHECK_EQ(after.load(), 0);
    }
}
