// The Google sign-in's loopback redirect (RFC 8252 section 7.3): Google sends the browser back to
// the game itself, http://127.0.0.1:<port>/oauth2/google/<origin tag>?code=..&state=.., and the
// game hands the code to the server it started the sign-in with (POST /auth/sso/google/finish,
// the server's docs/API.md). A link to Google's page sent by someone else therefore leads
// nowhere: the code lands on the victim's own computer, where nothing waits for it (or a sign-in
// of the game whose state it does not match).
//
// LoopbackRedirect listens on 127.0.0.1 only (sock::listenLoopbackV4), on a port bound before the
// sign-in starts, and serves one strict GET: the expected path, the Host header 127.0.0.1:<port>
// (no DNS rebinding), the state of the sign-in (compared in constant time). A request whose state
// does not match gets the 'foreign' page and the listener goes on waiting; the redirect with the
// code (or Google's error) gets its page, closes the listener at once and calls done, once. The
// pages hold no script and load nothing; they never say the sign-in succeeded (the game shows the
// outcome). Nothing of a request is ever logged but its status and its peer: the cookies of the
// other web applications of 127.0.0.1 arrive here too.
//
// Origin tag: the server the game talks to is named in the redirect path, so that a hostile
// community server that relays the game's sign-in to another server cannot hand back that
// server's Google page (its redirect path names the other server: the game refuses it before
// opening the browser, and Google refuses the code if it is rewritten).
//
// Engine-free (one worker thread 'net-sso' per sign-in, no UI): the online client
// (net/online_client.cpp) uses it on net-http; tests/sso_loopback_tests.cpp checks it.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace net {

// The texts of the pages the browser shows, in the game's language (made on the game thread:
// i18n is never used by the network threads).
struct SsoBrowserPage {
    std::string lang = "en";          // the html lang attribute
    bool rtl = false;
    std::string title, doneHeading, done, cancelledHeading, cancelled, foreignHeading, foreign;
};

struct RedirectResult {
    enum class Kind {
        Code,            // Google's redirect with a code: code, state, iss ("" when absent)
        ProviderError,   // Google's redirect with error= (access_denied: the player cancelled)
        Expired          // nothing came before the deadline
    } kind = Kind::Expired;
    std::string code, state, iss, error;
};

// The origin tag of a server, the frozen contract of the game and the server: the first 22
// characters of base64url(SHA-256("scacelith-sso-origin-v1\n" + origin)), origin =
// ServerEndpoint::origin() ("play.scacelith.example:443"; the server derives the same from
// SERVER_PUBLIC_HOST and PUBLIC_API_PORT).
std::string ssoOriginTag(const std::string& origin);
// "/oauth2/google/" + ssoOriginTag(origin): the path of the redirect URI.
std::string ssoRedirectPath(const std::string& origin);

class LoopbackRedirect {
public:
    LoopbackRedirect();
    ~LoopbackRedirect();              // cancel()
    LoopbackRedirect(const LoopbackRedirect&) = delete;
    LoopbackRedirect& operator=(const LoopbackRedirect&) = delete;

    // Binds 127.0.0.1:0 and listens; err = the socket error when it fails.
    bool open(std::string& err);
    uint16_t port() const;            // 0 before open()
    // Starts the worker: it serves expectedPath until a redirect carrying expectedState, or until
    // deadlineSteadyMs (sock::steadyMs()), then calls done once, from its own thread. done must
    // not call cancel(), nor wait for what calls it.
    void start(std::string expectedPath, std::string expectedState, int64_t deadlineSteadyMs, SsoBrowserPage page,
               std::function<void(const RedirectResult&)> done);
    // Stops listening and joins the worker; once it returns, done is never called. Idempotent.
    void cancel();

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// ---- the pure parts, for the tests ----
namespace loopback {

constexpr size_t kMaxHead = 64 * 1024;       // a longer request head is answered 431
constexpr size_t kMaxTarget = 4096;
constexpr size_t kMaxCode = 2048;
constexpr size_t kMaxIss = 256;
constexpr size_t kMaxError = 64;

// The redirect's parameters (unknown ones are ignored).
struct Request {
    bool hasCode = false, hasState = false, hasIss = false, hasError = false;
    std::string code, state, iss, error;
};

// Strict percent-decoding of a query component: %XX with two hex digits ('+' is a space); false
// for a bad or truncated escape and for %00.
bool percentDecode(const std::string& in, std::string& out);
// "a=1&b=2" -> {{"a","1"},{"b","2"}} decoded; false when a part does not decode.
bool queryParams(const std::string& query, std::vector<std::pair<std::string, std::string>>& out);

// Checks a request head (up to and with its blank line) sent to 127.0.0.1:port: 200 when it is
// "GET <expectedPath>?<query> HTTP/1.1" (or 1.0) with the Host 127.0.0.1:<port> and a well-formed
// query (out holds it; the state is not checked here), otherwise the status to answer: 400 (a bad
// request line, target, Host or query), 404 (another path), 405 (another method) or 431 (a head
// longer than kMaxHead).
int parseRequestHead(const std::string& head, uint16_t port, const std::string& expectedPath, Request& out);

enum class Page { Done, Cancelled, Foreign };
// The HTML page (texts HTML-escaped, lang and dir of the page's language).
std::string renderPage(const SsoBrowserPage& page, Page which);
// A whole HTTP/1.1 answer: the security headers (no-store, nosniff, no-referrer, a CSP that
// allows nothing but inline style and data: images, no CORS), Connection: close. contentType ""
// = text/plain with an empty body.
std::string response(int status, const std::string& contentType, const std::string& body);

}  // namespace loopback
}  // namespace net
