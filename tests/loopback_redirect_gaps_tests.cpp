// The Google sign-in's loopback redirect (net/loopback_redirect.h), the paths
// tests/sso_loopback_tests.cpp leaves out: an answer of a status the pages never use, the five
// characters a page escapes, and the listener with its eight slots all taken by connections that
// have each sent part of a request: a newcomer is closed at once, the others are still served.
#include "test.h"
#include "net/loopback_redirect.h"
#include "net/socket_util.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace net;

namespace {

const std::string kPath = ssoRedirectPath("play.scacelith.example:443");
const std::string kState = "S7qLm0x_Rf-2hTzYw9KbVnPcD4eGaU1oJiXsQ8yZt3A";

SsoBrowserPage page() {
    SsoBrowserPage p;
    p.title = "Sign-in";
    p.doneHeading = "Back to the game";
    p.done = "Return to the game.";
    p.cancelledHeading = "Cancelled";
    p.cancelled = "Cancelled.";
    p.foreignHeading = "Not for you";
    p.foreign = "This page does not match.";
    return p;
}

sock::Handle connectTo(uint16_t port) {
    sock::Endpoint ep;
    if (!sock::Endpoint::parse("127.0.0.1", port, ep)) return sock::kInvalid;
    std::string err;
    return sock::connectWithTimeout(ep, 2000, err);
}

bool sendAll(sock::Handle h, const std::string& s) {
    size_t off = 0;
    const int64_t end = sock::steadyMs() + 3000;
    while (off < s.size() && sock::steadyMs() < end) {
        const int r = sock::sendSome(h, reinterpret_cast<const uint8_t*>(s.data()) + off, s.size() - off);
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

// Everything the other side sends until it closes (or 'ms' passed); closed says which.
std::string readAll(sock::Handle h, int ms, bool& closed) {
    std::string out;
    const int64_t end = sock::steadyMs() + ms;
    closed = false;
    while (sock::steadyMs() < end) {
        sock::PollSet ps;
        ps.add(h, true, false);
        ps.wait(20);
        uint8_t buf[4096];
        bool peerClosed = false;
        const int r = sock::recvSome(h, buf, sizeof buf, peerClosed);
        if (r > 0) {
            out.append(reinterpret_cast<char*>(buf), size_t(r));
        } else if (r < 0) {
            closed = true;
            break;
        }
    }
    return out;
}

}  // namespace

TEST(loopback_redirect_gaps_other_status_and_escapes) {
    const std::string a = loopback::response(500, "", "");
    CHECK_EQ(a.compare(0, 20, "HTTP/1.1 500 Error\r\n"), 0);
    CHECK(a.find("\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 0\r\n") != std::string::npos);
    CHECK(a.find("Connection: close\r\n\r\n") == a.size() - 21);   // no body
    CHECK(loopback::response(503, "text/plain", "x").compare(0, 19, "HTTP/1.1 503 Error\r") == 0);
    SsoBrowserPage p = page();
    p.done = "Tom & Jerry's <\"game\">";
    const std::string html = loopback::renderPage(p, loopback::Page::Done);
    CHECK(html.find("Tom &amp; Jerry&#39;s &lt;&quot;game&quot;&gt;") != std::string::npos);
    CHECK(html.find("Jerry's") == std::string::npos);
}

TEST(loopback_redirect_gaps_slots_taken_by_partial_requests) {
    LoopbackRedirect l;
    std::string err;
    REQUIRE(l.open(err));
    const uint16_t port = l.port();
    std::mutex m;
    std::condition_variable cv;
    std::vector<RedirectResult> results;
    l.start(kPath, kState, sock::steadyMs() + 20000, page(), [&](const RedirectResult& r) {
        std::lock_guard<std::mutex> lk(m);
        results.push_back(r);
        cv.notify_all();
    });
    // Eight connections, each with the start of a request (the slots all hold one that spoke).
    const std::string head = "GET " + kPath + "?state=" + kState + "&code=4%2F0AbC HTTP/1.1\r\nHost: 127.0.0.1:" +
                             std::to_string(port) + "\r\n\r\n";
    std::vector<sock::Handle> spoke;
    for (int i = 0; i < 8; ++i) {
        spoke.push_back(connectTo(port));
        CHECK(spoke.back() != sock::kInvalid);
        CHECK(sendAll(spoke.back(), head.substr(0, 4)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the listener has read them all
    // A ninth: closed at once, unanswered.
    sock::Handle late = connectTo(port);
    CHECK(late != sock::kInvalid);
    bool closed = false;
    CHECK(readAll(late, 2000, closed).empty());
    CHECK(closed);
    sock::closeSocket(late);
    // The first one still gets its page once its request is whole.
    CHECK(sendAll(spoke[0], head.substr(4)));
    const std::string ans = readAll(spoke[0], 3000, closed);
    CHECK(ans.compare(0, 15, "HTTP/1.1 200 OK") == 0);
    CHECK(ans.find("Back to the game") != std::string::npos);
    {
        std::unique_lock<std::mutex> lk(m);
        CHECK(cv.wait_for(lk, std::chrono::seconds(3), [&] { return !results.empty(); }));
        CHECK(results.size() == 1 && results[0].kind == RedirectResult::Kind::Code && results[0].code == "4/0AbC");
    }
    for (sock::Handle h : spoke) sock::closeSocket(h);
    l.cancel();
}
