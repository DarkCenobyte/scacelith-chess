// Platform-independent parts of transport.h.
#include "transport.h"
#include "crypto.h"
#include <cctype>

namespace net {

// The abort action runs under mu_: setAbort(nullptr), when the operation ends, waits for one that
// is running, which therefore never outlives the socket or handle it closes.
void CancelToken::cancel() {
    std::lock_guard<std::mutex> lk(mu_);
    cancelled_.store(true);
    if (abort_) abort_();
}

void CancelToken::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    cancelled_.store(false);
    abort_ = nullptr;
}

bool CancelToken::hasAbort() {
    std::lock_guard<std::mutex> lk(mu_);
    return bool(abort_);
}

void CancelToken::setAbort(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mu_);
    abort_ = std::move(fn);
    if (cancelled_.load() && abort_) abort_();
}

static std::string lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = char(std::tolower((unsigned char)c));
    return r;
}

std::string HttpHead::get(const std::string& lowerName) const {
    for (auto& h : headers)
        if (h.first == lowerName) return h.second;
    return std::string();
}

bool isLoopbackHost(const std::string& host) {
    std::string h = lower(host);
    return h == "localhost" || h == "127.0.0.1" || h == "::1";
}

static bool isIPv4(const std::string& h) {
    int parts = 0, digits = 0, value = 0;
    for (size_t i = 0; i <= h.size(); ++i) {
        if (i == h.size() || h[i] == '.') {
            if (digits == 0 || value > 255) return false;
            ++parts;
            digits = value = 0;
            continue;
        }
        if (!std::isdigit((unsigned char)h[i]) || ++digits > 3) return false;
        value = value * 10 + (h[i] - '0');
    }
    return parts == 4;
}

static bool isIPv6(const std::string& h) {
    if (h.size() < 2 || h.size() > 45 || h.find(':') == std::string::npos) return false;
    int groups = 0;
    bool compressed = false;
    size_t i = 0;
    if (h.compare(0, 2, "::") == 0) { compressed = true; i = 2; }
    while (i < h.size()) {
        size_t j = i;
        while (j < h.size() && std::isxdigit((unsigned char)h[j])) ++j;
        if (j < h.size() && h[j] == '.') {           // trailing IPv4 part
            if (!isIPv4(h.substr(i))) return false;
            groups += 2;
            i = h.size();
            break;
        }
        if (j == i || j - i > 4) return false;
        ++groups;
        i = j;
        if (i == h.size()) break;
        if (h[i] != ':') return false;
        ++i;
        if (i < h.size() && h[i] == ':') {
            if (compressed) return false;
            compressed = true;
            ++i;
        } else if (i == h.size()) {
            return false;                            // trailing single ':'
        }
    }
    return compressed ? groups <= 7 : groups == 8;
}

bool isIpLiteral(const std::string& host) { return isIPv4(host) || isIPv6(host); }

std::string hostHeader(const std::string& host, uint16_t port, bool tls) {
    std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    if ((tls && port == 443) || (!tls && port == 80)) return h;
    return h + ":" + std::to_string(port);
}

PinCheck pinCheckAtSend(const std::string& pin, const std::string& leaf, bool noTlsYet) {
    if (leaf.empty()) return noTlsYet ? PinCheck::Later : PinCheck::Mismatch;
    return crypto::constantTimeEqual(leaf, pin) ? PinCheck::Match : PinCheck::Mismatch;
}

}  // namespace net
