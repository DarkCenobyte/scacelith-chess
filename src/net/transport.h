// Transports of the online client: HTTPS requests and a binary WebSocket client.
//   Windows:  WinHTTP (transport_win32.cpp): OS TLS stack and trust store, system proxy,
//             WinHTTP WebSocket API.
//   Linux:    OpenSSL (transport_openssl.cpp, development and test builds) with a minimal
//             HTTP/1.1 and RFC 6455 client.
//
// Security rules applied here, whatever the caller asks:
//   - TLS certificates are validated by the OS trust store (Linux: OpenSSL default paths),
//     including the host name. With a pin (hex SHA-256 of the leaf certificate's DER), an
//     unknown issuer is tolerated for that connection only and the leaf must match the pin;
//     no header or body given by the caller (token, password) reaches a server that fails the
//     pin (OpenSSL: checked right after the handshake; WinHTTP: see transport_win32.cpp, which
//     also says what an active attacker can still get under Wine).
//   - Plain HTTP / WS only for loopback hosts (localhost, 127.0.0.1, ::1).
//   - HTTP redirects are never followed (a 3xx comes back as it is), cookies are not kept.
//     (net/download.h follows the redirects of public file hosts itself, with its own rules.)
//
// Every function blocks and is called from the client's network threads only. A CancelToken
// lets another thread abort a blocking call (shutdown).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace net {

class CancelToken {
public:
    void cancel();                            // idempotent; runs the registered abort action
    bool cancelled() const { return cancelled_.load(); }
    void reset();
    // The blocking operation in progress registers how to abort it (closing its handle or
    // socket); cleared with setAbort(nullptr) when it ends (AbortGuard below does both). Runs at
    // once when already cancelled. The action runs under the token's lock (it must neither block
    // nor call the token): setAbort(nullptr) returns once a running one has finished.
    void setAbort(std::function<void()> fn);
    bool hasAbort();                          // an abort action is registered (an operation runs)

private:
    std::mutex mu_;
    std::function<void()> abort_;
    std::atomic<bool> cancelled_{false};
};

// The abort action of one blocking operation, registered on its token for as long as the guard
// lives (clear() takes it back earlier). Taken back also when an exception leaves the operation
// (a std::bad_alloc while a large answer grows): the token never keeps an action on the
// operation's socket or handle once they are gone.
class AbortGuard {
public:
    AbortGuard(CancelToken* token, std::function<void()> fn) : token_(token) {
        if (token_) token_->setAbort(std::move(fn));
    }
    ~AbortGuard() { clear(); }
    AbortGuard(const AbortGuard&) = delete;
    AbortGuard& operator=(const AbortGuard&) = delete;
    void clear() {
        if (token_) token_->setAbort(nullptr);
        token_ = nullptr;
    }

private:
    CancelToken* token_;
};

struct HttpRequest {
    std::string method = "GET";
    std::string host;                         // DNS name or IP literal (IPv6 without brackets)
    uint16_t port = 443;
    bool tls = true;                          // false: loopback development servers only
    std::string pinnedSha256;                 // hex, lower-case; "" = trust store only
    std::string path = "/";                   // "/api/v1/info"
    std::string body;                         // JSON; sent with Content-Type application/json
    std::string accept = "application/json";  // the Accept header (a PGN download asks for its type)
    std::vector<std::pair<std::string, std::string>> headers;   // e.g. Authorization
    // Windows (WinHTTP): for each of resolve, connect, send and receive. Linux: one deadline for
    // the connection and its TLS handshake, then one for the whole exchange (httpStream: for each
    // write and each read).
    int timeoutMs = 15000;
    size_t maxResponseBytes = 1 << 20;
};

struct HttpResponse {
    int status = 0;                           // 0 when no HTTP response was received
    std::string body;
    std::string retryAfter;                   // Retry-After header, if any
    // "" when a response arrived; otherwise "network", "tls", "certificate", "timeout",
    // "cancelled", "insecure" (plain HTTP to a non-loopback host), "too_large", "unavailable".
    std::string error;
    std::string detail;                       // diagnostic text for the log
};

void httpRequest(const HttpRequest& req, HttpResponse& resp, CancelToken* cancel = nullptr);

// ---- Streamed responses (downloads: net/download.h) ----
// The head of a response: status and headers (names lower-case, in the order received).
struct HttpHead {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string get(const std::string& lowerName) const;   // first value, "" when absent
};

// One exchange whose body is handed over piece by piece as it arrives, never held whole in
// memory. onHead receives the status and headers (return false to stop there: a redirect, an
// error status); then onBody receives each piece of the body (return false to abort). Same rules
// as httpRequest: redirects are never followed, cookies are not kept, plain HTTP only to loopback
// hosts. req.headers are sent as given (a User-Agent there replaces the default one); req.body
// must be empty. req.timeoutMs bounds the connection and each wait for data (an idle timeout: a
// large body takes as long as it needs while data keeps coming). req.maxResponseBytes caps the
// body. resp.status is set once a head arrived; resp.error is "" when the body arrived complete
// (or onHead stopped the exchange), else as for httpRequest, "aborted" when onBody returned
// false, "truncated" when the connection ended before the announced end of the body.
void httpStream(const HttpRequest& req, const std::function<bool(const HttpHead&)>& onHead,
                const std::function<bool(const char* data, size_t n)>& onBody, HttpResponse& resp,
                CancelToken* cancel = nullptr);

struct WsParams {
    std::string host;
    uint16_t port = 443;
    bool tls = true;
    std::string pinnedSha256;
    std::string path = "/ws";
    std::string subprotocol;                  // required, and required in the answer
    int timeoutMs = 10000;                    // handshake
    size_t maxMessageBytes = 1 << 20;
    // Called from the transport's own thread when a message arrived or the connection closed.
    std::function<void()> onActivity;
};

// A connected WebSocket. Received binary messages queue up inside (the transport reads on its
// own thread); text messages are a protocol violation (close 1003).
class WebSocket {
public:
    virtual ~WebSocket() = default;
    virtual bool send(const std::vector<uint8_t>& msg) = 0;     // one binary message
    virtual bool receive(std::vector<uint8_t>& msg) = 0;        // false when none is waiting
    // True once closed; code 1006 when the connection dropped without a close frame.
    virtual bool closed(uint16_t& code, std::string& reason) const = 0;
    // Closing handshake (bounded wait, about one second at most), then frees everything.
    virtual void close(uint16_t code = 1000) = 0;
    // The Scacelith-Server-Id header of the server's 101 answer ("" when there was none): the
    // client compares it with the server its saved session belongs to before sending Hello.
    const std::string& serverId() const { return serverId_; }

protected:
    std::string serverId_;
};

// Connects (TCP, TLS, HTTP upgrade). nullptr on failure with error as for HttpResponse, or
// "http_<status>" when the server answered without upgrading, or "subprotocol".
std::unique_ptr<WebSocket> wsConnect(const WsParams& p, std::string& error, int& httpStatus, CancelToken* cancel = nullptr);

bool transportAvailable();                    // false when the WinHTTP session or the OpenSSL context fails
bool isLoopbackHost(const std::string& host); // localhost, 127.0.0.1, ::1 (any case)
bool isIpLiteral(const std::string& host);
// "host" or "[v6]" followed by ":port" unless it is the scheme's default port.
std::string hostHeader(const std::string& host, uint16_t port, bool tls);

}  // namespace net
