// Windows transport: WinHTTP (synchronous handles, one session for the process).
//   - TLS by the OS (Schannel), certificates checked against the Windows trust store, system
//     proxy settings (automatic proxy on Windows 8.1+, the default proxy otherwise).
//   - Redirects and cookies disabled on every request.
//   - Pinned servers: SECURITY_FLAG_IGNORE_UNKNOWN_CA for that request only, and the leaf
//     certificate (WINHTTP_OPTION_SERVER_CERT_CONTEXT, CERT_SHA256_HASH_PROP_ID) is compared
//     with the pin in the WINHTTP_CALLBACK_STATUS_SENDING_REQUEST notification, after the TLS
//     handshake and before the request is written; a mismatch closes the request handle there,
//     which aborts the send (the approach of .NET's WinHttpHandler). The pin is checked again
//     once the response has arrived. Wine's WinHTTP sends the request anyway after that close,
//     so a pinned request that carries anything (Authorization header, body) is preceded by a
//     "HEAD /" probe without either: a server that fails the pin never receives the secret, and
//     the real request normally reuses the probe's verified keep-alive connection.
//   - WebSocket: WinHttpWebSocketCompleteUpgrade / Send / Receive / Shutdown. A reader thread
//     owned by the socket object blocks in WinHttpWebSocketReceive (WinHTTP allows one send and
//     one receive in flight at the same time) and queues complete binary messages.
#ifdef _WIN32
#include "transport.h"
#include "crypto.h"
#include "net_sys.h"
#include "../core/log.h"

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <thread>

namespace net {

namespace {

using sys::widen;

std::string narrow(const wchar_t* w, size_t len) {
    if (!len) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, int(len), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, int(len), &s[0], n, nullptr, nullptr);
    return s;
}

// The process-wide WinHTTP session (thread-safe; connections are pooled per host).
HINTERNET session() {
    static HINTERNET s = [] {
        HINTERNET h = WinHttpOpen(L"Scacelith", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h) h = WinHttpOpen(L"Scacelith", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h) {
            LOGE("net: WinHttpOpen failed (%lu)", GetLastError());
            return h;
        }
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;   // Windows before 11 / Server 2022
            WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(h, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));
        return h;
    }();
    return s;
}

// A WinHTTP handle that another thread may close to cancel a blocking call.
class Handle {
public:
    explicit Handle(HINTERNET h = nullptr) : h_(h) {}
    ~Handle() { close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HINTERNET get() const { return h_.load(); }
    void reset(HINTERNET h) { close(); h_.store(h); }
    HINTERNET release() { return h_.exchange(nullptr); }
    void close() {
        HINTERNET h = h_.exchange(nullptr);
        if (h) WinHttpCloseHandle(h);
    }

private:
    std::atomic<HINTERNET> h_;
};

std::string leafSha256(HINTERNET request) {
    PCCERT_CONTEXT cert = nullptr;
    DWORD size = sizeof(cert);
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &size) || !cert) return std::string();
    BYTE hash[32];
    DWORD len = sizeof(hash);
    std::string out;
    if (CertGetCertificateContextProperty(cert, CERT_SHA256_HASH_PROP_ID, hash, &len) && len == 32)
        out = crypto::hex(hash, 32);
    else
        out = crypto::hex(crypto::sha256(cert->pbCertEncoded, cert->cbCertEncoded));
    CertFreeCertificateContext(cert);
    return out;
}

struct RequestContext {
    std::string pin;
    Handle* request = nullptr;
    std::atomic<bool> pinMismatch{false};
    std::atomic<DWORD> secureFlags{0};
};

void CALLBACK statusCallback(HINTERNET h, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD len) {
    RequestContext* c = reinterpret_cast<RequestContext*>(ctx);
    if (!c) return;
    if (status == WINHTTP_CALLBACK_STATUS_SECURE_FAILURE && info && len >= sizeof(DWORD)) {
        c->secureFlags.store(*static_cast<DWORD*>(info));
    } else if (status == WINHTTP_CALLBACK_STATUS_SENDING_REQUEST && !c->pin.empty()) {
        // Every time: WinHTTP may send the request more than once (again on another connection).
        std::string got = leafSha256(h);
        if (got.empty() || !crypto::constantTimeEqual(got, c->pin)) {
            LOGW("net: pinned certificate mismatch (server presents %s)", got.empty() ? "?" : got.c_str());
            c->pinMismatch.store(true);
            c->request->close();   // abort before the request (headers, token, body) is written
        }
    }
}

std::string mapError(DWORD e, const RequestContext& c) {
    if (c.pinMismatch.load()) return "certificate";
    switch (e) {
    case ERROR_WINHTTP_TIMEOUT: return "timeout";
    case 10060: return "timeout";   // WSAETIMEDOUT, which Wine's WinHTTP passes on
    case ERROR_WINHTTP_SECURE_FAILURE:
        return (c.secureFlags.load() & WINHTTP_CALLBACK_STATUS_FLAG_SECURITY_CHANNEL_ERROR) ? "tls" : "certificate";
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        return "certificate";
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        return "tls";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "cancelled";
    default: return "network";
    }
}

// Opens connect + request handles with the security options; false with resp-style error.
bool openRequest(const std::string& host, uint16_t port, bool tls, const std::string& method, const std::string& path,
                 const std::string& pin, int timeoutMs, Handle& conn, Handle& req, std::string& error, std::string& detail) {
    if (!session()) { error = "unavailable"; return false; }
    conn.reset(WinHttpConnect(session(), widen(host).c_str(), port, 0));
    if (!conn.get()) { error = "network"; detail = "WinHttpConnect " + std::to_string(GetLastError()); return false; }
    req.reset(WinHttpOpenRequest(conn.get(), widen(method).c_str(), widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, tls ? WINHTTP_FLAG_SECURE : 0));
    if (!req.get()) { error = "network"; detail = "WinHttpOpenRequest " + std::to_string(GetLastError()); return false; }
    WinHttpSetTimeouts(req.get(), timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    // The wait for the response headers has its own timeout (90 s by default), which some
    // WinHTTP implementations (Wine's) apply instead of the receive timeout.
    DWORD headersTimeout = DWORD(timeoutMs);
    WinHttpSetOption(req.get(), WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &headersTimeout, sizeof(headersTimeout));
    DWORD features = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(req.get(), WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features))) {
        features = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(req.get(), WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features))) {
            DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            if (!WinHttpSetOption(req.get(), WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never))) {
                error = "network";
                detail = "cannot disable redirects";
                return false;
            }
        }
    }
    if (tls && !pin.empty()) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
        WinHttpSetOption(req.get(), WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }
    WinHttpSetStatusCallback(req.get(), statusCallback, WINHTTP_CALLBACK_FLAG_SECURE_FAILURE | WINHTTP_CALLBACK_FLAG_SEND_REQUEST, 0);
    return true;
}

// Sends and waits for the response headers, with the pin checks. The body (may be empty) is
// sent with the request.
bool exchange(Handle& req, RequestContext& ctx, const std::wstring& headers, const std::string& body, std::string& error, std::string& detail) {
    BOOL ok = WinHttpSendRequest(req.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), DWORD(-1L),
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), DWORD(body.size()), DWORD(body.size()),
                                 reinterpret_cast<DWORD_PTR>(&ctx));
    if (ok) ok = WinHttpReceiveResponse(req.get(), nullptr);
    if (!ok) {
        DWORD e = GetLastError();
        error = mapError(e, ctx);
        detail = "WinHTTP error " + std::to_string(e);
        return false;
    }
    if (!ctx.pin.empty()) {
        // Defence in depth: the leaf must match the pin (also when no SENDING_REQUEST came).
        std::string got = leafSha256(req.get());
        if (got.empty() || !crypto::constantTimeEqual(got, ctx.pin)) {
            error = "certificate";
            detail = "pinned certificate mismatch";
            return false;
        }
    }
    return true;
}

DWORD queryStatus(HINTERNET req) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    return status;
}

std::string queryHeader(HINTERNET req, DWORD info, const wchar_t* name) {
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (!WinHttpQueryHeaders(req, info, name ? name : WINHTTP_HEADER_NAME_BY_INDEX, buf, &size, WINHTTP_NO_HEADER_INDEX))
        return std::string();
    return narrow(buf, size / sizeof(wchar_t));
}

// ---- WebSocket ----

class WinHttpWebSocket final : public WebSocket {
public:
    WinHttpWebSocket(HINTERNET conn, HINTERNET ws, const WsParams& p, std::string serverId)
        : conn_(conn), ws_(ws), maxBytes_(p.maxMessageBytes), onActivity_(p.onActivity) {
        serverId_ = std::move(serverId);
        reader_ = std::thread([this] { run(); });
    }
    ~WinHttpWebSocket() override { close(1001); }

    bool send(const std::vector<uint8_t>& msg) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (closed_ || closeSent_) return false;
        }
        std::lock_guard<std::mutex> sl(sendMu_);
        DWORD e = WinHttpWebSocketSend(ws_.get(), WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                       msg.empty() ? nullptr : (PVOID)msg.data(), DWORD(msg.size()));
        if (e != NO_ERROR) {
            markClosed(1006, "send failed");
            return false;
        }
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
        if (!reader_.joinable()) return;
        bool shutdown = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!closed_ && !closeSent_) closeSent_ = shutdown = true;
        }
        if (shutdown) {
            std::lock_guard<std::mutex> sl(sendMu_);
            WinHttpWebSocketShutdown(ws_.get(), code, nullptr, 0);   // our close frame; the answer ends the reader
        }
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::seconds(1), [this] { return closed_; });
        }
        ws_.close();   // cancels a receive still in progress
        reader_.join();
        conn_.close();
        markClosed(code, "closed");
    }

private:
    Handle conn_, ws_;
    size_t maxBytes_;
    std::function<void()> onActivity_;
    std::thread reader_;
    mutable std::mutex mu_;
    std::mutex sendMu_;                   // one send (or shutdown) at a time
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> inq_;
    bool closed_ = false, closeSent_ = false;
    uint16_t closeCode_ = 1006;
    std::string closeReason_;

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

    void shutdownWith(uint16_t code) {
        bool send = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!closeSent_) closeSent_ = send = true;
        }
        if (send) {
            std::lock_guard<std::mutex> sl(sendMu_);
            WinHttpWebSocketShutdown(ws_.get(), code, nullptr, 0);
        }
    }

    void run() {
        std::vector<uint8_t> buf(64 * 1024), msg;
        for (;;) {
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            HINTERNET ws = ws_.get();
            if (!ws) { markClosed(1006, "closed"); return; }
            DWORD e = WinHttpWebSocketReceive(ws, buf.data(), DWORD(buf.size()), &read, &type);
            if (e != NO_ERROR) {
                markClosed(1006, "connection lost (" + std::to_string(e) + ")");
                return;
            }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                USHORT status = 1005;
                BYTE reason[128];
                DWORD reasonLen = 0;
                WinHttpWebSocketQueryCloseStatus(ws, &status, reason, sizeof(reason), &reasonLen);
                shutdownWith(status == 1005 ? 1000 : status);   // answer the server's close frame
                markClosed(status, std::string(reinterpret_cast<char*>(reason), reasonLen));
                return;
            }
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
                shutdownWith(1003);
                markClosed(1003, "text message");
                return;
            }
            if (msg.size() + read > maxBytes_) {
                shutdownWith(1009);
                markClosed(1009, "message too big");
                return;
            }
            msg.insert(msg.end(), buf.begin(), buf.begin() + read);
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    inq_.push_back(std::move(msg));
                }
                msg.clear();
                if (onActivity_) onActivity_();
            }
        }
    }
};

}  // namespace

bool transportAvailable() { return session() != nullptr; }

namespace {

void perform(const HttpRequest& r, HttpResponse& resp, CancelToken* cancel) {
    resp = HttpResponse();
    Handle conn, req;
    if (!openRequest(r.host, r.port, r.tls, r.method, r.path, r.tls ? r.pinnedSha256 : std::string(), r.timeoutMs, conn, req,
                     resp.error, resp.detail))
        return;
    RequestContext ctx;
    ctx.pin = r.tls ? r.pinnedSha256 : std::string();
    ctx.request = &req;
    AbortGuard abortGuard(cancel, [&req] { req.close(); });

    std::wstring headers = L"Accept: " + widen(r.accept) + L"\r\n";
    bool hasBody = !r.body.empty() || r.method == "POST" || r.method == "PUT";
    if (hasBody) headers += L"Content-Type: application/json\r\n";
    for (auto& h : r.headers) headers += widen(h.first) + L": " + widen(h.second) + L"\r\n";

    bool ok = exchange(req, ctx, headers, r.body, resp.error, resp.detail);
    if (ok) {
        resp.status = int(queryStatus(req.get()));
        resp.retryAfter = queryHeader(req.get(), WINHTTP_QUERY_RETRY_AFTER, nullptr);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req.get(), &avail)) {
                DWORD e = GetLastError();
                resp.error = mapError(e, ctx);
                resp.detail = "read " + std::to_string(e);
                ok = false;
                break;
            }
            if (avail == 0) break;
            if (resp.body.size() + avail > r.maxResponseBytes) {
                resp.error = "too_large";
                ok = false;
                break;
            }
            size_t at = resp.body.size();
            resp.body.resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req.get(), &resp.body[at], avail, &got)) {
                DWORD e = GetLastError();
                resp.error = mapError(e, ctx);
                ok = false;
                break;
            }
            resp.body.resize(at + got);
            if (got == 0) break;
        }
    }
    abortGuard.clear();
    if (cancel && cancel->cancelled()) { resp.error = "cancelled"; ok = false; }
    if (!ok) {
        resp.status = 0;
        resp.body.clear();
    }
}

}  // namespace

void httpRequest(const HttpRequest& r, HttpResponse& resp, CancelToken* cancel) {
    resp = HttpResponse();
    if (!r.tls && !isLoopbackHost(r.host)) {
        resp.error = "insecure";
        return;
    }
    if (r.tls && !r.pinnedSha256.empty() && (!r.body.empty() || !r.headers.empty())) {
        HttpRequest probe;
        probe.method = "HEAD";
        probe.host = r.host;
        probe.port = r.port;
        probe.pinnedSha256 = r.pinnedSha256;
        probe.path = "/";
        probe.timeoutMs = r.timeoutMs;
        probe.maxResponseBytes = 64 * 1024;
        perform(probe, resp, cancel);
        if (!resp.error.empty()) return;   // any HTTP status will do: the pin held
        resp = HttpResponse();
    }
    perform(r, resp, cancel);
}

// ---- Streamed responses ----

namespace {

// Every header of the response (WINHTTP_QUERY_RAW_HEADERS_CRLF), names lower-case.
void queryAllHeaders(HINTERNET req, HttpHead& head) {
    DWORD size = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return;
    std::wstring w(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &w[0], &size, WINHTTP_NO_HEADER_INDEX))
        return;
    std::string all = narrow(w.data(), size / sizeof(wchar_t));
    size_t pos = all.find("\r\n");   // after the status line
    while (pos != std::string::npos && pos + 2 < all.size()) {
        pos += 2;
        size_t e = all.find("\r\n", pos);
        std::string line = all.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        size_t c = line.find(':');
        if (c != std::string::npos) {
            std::string name = line.substr(0, c), value = line.substr(c + 1);
            for (char& ch : name) ch = char(std::tolower((unsigned char)ch));
            size_t a = value.find_first_not_of(" \t"), b = value.find_last_not_of(" \t");
            head.headers.emplace_back(name, a == std::string::npos ? std::string() : value.substr(a, b - a + 1));
        }
        pos = e;
    }
}

}  // namespace

void httpStream(const HttpRequest& r, const std::function<bool(const HttpHead&)>& onHead,
                const std::function<bool(const char*, size_t)>& onBody, HttpResponse& resp, CancelToken* cancel) {
    resp = HttpResponse();
    if (!r.tls && !isLoopbackHost(r.host)) {
        resp.error = "insecure";
        return;
    }
    Handle conn, req;
    std::string pin = r.tls ? r.pinnedSha256 : std::string();
    if (!openRequest(r.host, r.port, r.tls, r.method, r.path, pin, r.timeoutMs, conn, req, resp.error, resp.detail)) return;
    RequestContext ctx;
    ctx.pin = pin;
    ctx.request = &req;
    AbortGuard abortGuard(cancel, [&req] { req.close(); });

    // The session's agent ("Scacelith") is only added when the request has none.
    std::wstring headers = L"Accept: */*\r\n";
    for (auto& h : r.headers) headers += widen(h.first) + L": " + widen(h.second) + L"\r\n";
    std::string err;
    bool ok = exchange(req, ctx, headers, std::string(), resp.error, resp.detail);
    if (ok) {
        HttpHead head;
        head.status = int(queryStatus(req.get()));
        queryAllHeaders(req.get(), head);
        resp.status = head.status;
        resp.retryAfter = head.get("retry-after");
        bool wantBody = onHead(head) && r.method != "HEAD" && head.status != 204 && head.status != 304;
        // WinHTTP removes the chunked coding itself; a sized body is checked for truncation here.
        std::string cl = head.get("content-length");
        bool sized = !cl.empty() && head.get("transfer-encoding").empty();
        uint64_t announced = sized ? std::strtoull(cl.c_str(), nullptr, 10) : 0, total = 0;
        if (wantBody && sized && announced > r.maxResponseBytes) { err = "too_large"; ok = false; wantBody = false; }
        std::vector<char> buf(64 * 1024);
        while (wantBody) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req.get(), &avail)) {
                DWORD e = GetLastError();
                resp.error = mapError(e, ctx);
                resp.detail = "read " + std::to_string(e);
                ok = false;
                break;
            }
            if (avail == 0) {
                if (sized && total < announced) { err = "truncated"; ok = false; }
                break;
            }
            DWORD got = 0;
            if (!WinHttpReadData(req.get(), buf.data(), std::min<DWORD>(avail, DWORD(buf.size())), &got)) {
                DWORD e = GetLastError();
                resp.error = mapError(e, ctx);
                resp.detail = "read " + std::to_string(e);
                ok = false;
                break;
            }
            if (got == 0) {
                if (sized && total < announced) { err = "truncated"; ok = false; }
                break;
            }
            total += got;
            if (total > r.maxResponseBytes) { err = "too_large"; ok = false; break; }
            if (!onBody(buf.data(), got)) { err = "aborted"; ok = false; break; }
        }
    }
    abortGuard.clear();
    if (cancel && cancel->cancelled()) { err = "cancelled"; ok = false; }
    if (!ok && !err.empty()) resp.error = err;
    if (!ok && resp.error.empty()) resp.error = "network";
}

std::unique_ptr<WebSocket> wsConnect(const WsParams& p, std::string& error, int& httpStatus, CancelToken* cancel) {
    error.clear();
    httpStatus = 0;
    if (!p.tls && !isLoopbackHost(p.host)) {
        error = "insecure";
        return nullptr;
    }
    Handle conn, req;
    std::string detail;
    std::string pin = p.tls ? p.pinnedSha256 : std::string();
    if (!openRequest(p.host, p.port, p.tls, "GET", p.path, pin, p.timeoutMs, conn, req, error, detail)) return nullptr;
    if (!WinHttpSetOption(req.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        error = "unavailable";   // Windows 7 has no WinHTTP WebSocket
        return nullptr;
    }
    RequestContext ctx;
    ctx.pin = pin;
    ctx.request = &req;
    AbortGuard abortGuard(cancel, [&req] { req.close(); });
    std::wstring headers = L"Sec-WebSocket-Protocol: " + widen(p.subprotocol) + L"\r\n";
    bool ok = exchange(req, ctx, headers, std::string(), error, detail);
    abortGuard.clear();
    if (cancel && cancel->cancelled()) { error = "cancelled"; ok = false; }
    if (!ok) {
        if (!detail.empty()) LOGW("net: websocket connect to %s:%u: %s", p.host.c_str(), p.port, detail.c_str());
        return nullptr;
    }
    httpStatus = int(queryStatus(req.get()));
    if (httpStatus != 101) {
        error = "http_" + std::to_string(httpStatus);
        return nullptr;
    }
    if (queryHeader(req.get(), WINHTTP_QUERY_CUSTOM, L"Sec-WebSocket-Protocol") != p.subprotocol) {
        error = "subprotocol";
        return nullptr;
    }
    // Read before WinHttpWebSocketCompleteUpgrade: the request handle is closed afterwards.
    std::string serverId = queryHeader(req.get(), WINHTTP_QUERY_CUSTOM, L"Scacelith-Server-Id");
    // Idle periods are normal on a WebSocket: no receive timeout (the client's own heartbeat
    // watchdog detects a dead connection).
    DWORD infinite = 0;
    WinHttpSetOption(req.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &infinite, sizeof(infinite));
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req.get(), 0);
    if (!ws) {
        error = "network";
        LOGW("net: WinHttpWebSocketCompleteUpgrade failed (%lu)", GetLastError());
        return nullptr;
    }
    WinHttpSetOption(ws, WINHTTP_OPTION_RECEIVE_TIMEOUT, &infinite, sizeof(infinite));
    req.close();
    // The socket object owns the connect handle from now on.
    return std::make_unique<WinHttpWebSocket>(conn.release(), ws, p, std::move(serverId));
}

}  // namespace net

#endif
