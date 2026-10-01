#include "download.h"
#include "crypto.h"
#include "net_sys.h"
#include "../core/log.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

namespace net {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

bool startsWithNoCase(const std::string& s, const char* prefix) {
    size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && lower(s.substr(0, n)) == prefix;
}

// Removes "." and ".." segments of a path (RFC 3986 5.2.4); the query is left alone.
std::string normalisePath(const std::string& pathAndQuery) {
    size_t q = pathAndQuery.find('?');
    std::string path = pathAndQuery.substr(0, q), query = q == std::string::npos ? std::string() : pathAndQuery.substr(q);
    std::vector<std::string> out;
    size_t pos = 1;
    bool trailing = false;
    while (pos <= path.size()) {
        size_t e = path.find('/', pos);
        std::string seg = path.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        trailing = false;
        if (seg == ".") {
            trailing = true;
        } else if (seg == "..") {
            if (!out.empty()) out.pop_back();
            trailing = true;
        } else {
            out.push_back(seg);
        }
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    std::string r;
    for (const std::string& s : out) r += "/" + s;
    if (r.empty() || trailing) r += "/";
    return r + query;
}

// The bytes of a path that can go in a request line: controls and spaces refused, other bytes
// above ASCII percent-encoded.
bool cleanPath(const std::string& in, std::string& out) {
    static const char hexd[] = "0123456789ABCDEF";
    out.clear();
    for (unsigned char c : in) {
        if (c <= 0x20 || c == 0x7f) return false;
        if (c >= 0x80) {
            out += '%';
            out += hexd[c >> 4];
            out += hexd[c & 15];
        } else {
            out += char(c);
        }
    }
    return true;
}

bool validHost(const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    if (isIpLiteral(h)) return true;
    for (unsigned char c : h)
        if (!std::isalnum(c) && c != '-' && c != '.' && c != '_') return false;
    return true;
}

bool redirectStatus(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

// "bytes 100-199/1000": first byte, and total (0 when "*" or absent). False when malformed.
bool parseContentRange(const std::string& v, uint64_t& first, uint64_t& total) {
    std::string s = lower(v);
    if (s.compare(0, 6, "bytes ") != 0) return false;
    const char* p = s.c_str() + 6;
    while (*p == ' ') ++p;
    char* end = nullptr;
    first = std::strtoull(p, &end, 10);
    if (end == p || *end != '-') return false;
    p = end + 1;
    std::strtoull(p, &end, 10);
    if (end == p || *end != '/') return false;
    p = end + 1;
    total = 0;
    if (*p == '*') return true;
    total = std::strtoull(p, &end, 10);
    return end != p;
}

// Waits up to ms, returning early (false) when cancelled.
bool pause(int ms, CancelToken* cancel) {
    for (int t = 0; t < ms; t += 50) {
        if (cancel && cancel->cancelled()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !(cancel && cancel->cancelled());
}

bool retryable(const std::string& error) { return error == "network" || error == "timeout" || error == "truncated"; }

}  // namespace

bool parseUrl(const std::string& text, Url& out) {
    Url u;
    size_t at;
    if (startsWithNoCase(text, "https://")) {
        u.tls = true;
        u.port = 443;
        at = 8;
    } else if (startsWithNoCase(text, "http://")) {
        u.tls = false;
        u.port = 80;
        at = 7;
    } else {
        return false;
    }
    size_t end = text.find_first_of("/?#", at);
    std::string authority = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (authority.find('@') != std::string::npos) return false;   // user information: never
    std::string host, port;
    if (!authority.empty() && authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        host = authority.substr(1, close - 1);
        if (!isIpLiteral(host) || host.find(':') == std::string::npos) return false;
        std::string rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') return false;
            port = rest.substr(1);
        }
    } else {
        size_t colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) port = authority.substr(colon + 1);
    }
    host = lower(host);
    if (!validHost(host)) return false;
    if (!port.empty() || (!authority.empty() && authority.back() == ':')) {
        if (port.empty() || port.size() > 5) return false;
        for (char c : port)
            if (c < '0' || c > '9') return false;
        int p = std::atoi(port.c_str());
        if (p < 1 || p > 65535) return false;
        u.port = uint16_t(p);
    }
    u.host = host;
    std::string rest = end == std::string::npos ? std::string() : text.substr(end);
    size_t hash = rest.find('#');
    if (hash != std::string::npos) rest.resize(hash);
    if (rest.empty() || rest[0] == '?') rest = "/" + rest;
    if (!cleanPath(rest, u.path)) return false;
    u.path = normalisePath(u.path);
    out = u;
    return true;
}

bool resolveLocation(const Url& base, const std::string& location, Url& out) {
    size_t a = location.find_first_not_of(" \t"), b = location.find_last_not_of(" \t");
    if (a == std::string::npos) return false;
    std::string loc = location.substr(a, b - a + 1);
    size_t scheme = loc.find("://");
    if (scheme != std::string::npos && loc.find_first_of("/?#") > scheme) return parseUrl(loc, out);
    if (loc.compare(0, 2, "//") == 0) return parseUrl((base.tls ? "https:" : "http:") + loc, out);
    size_t hash = loc.find('#');
    if (hash != std::string::npos) loc.resize(hash);
    Url u = base;
    std::string path;
    if (!loc.empty() && loc[0] == '/') {
        path = loc;
    } else {
        std::string dir = base.path.substr(0, base.path.find('?'));
        dir = dir.substr(0, dir.find_last_of('/') + 1);
        if (dir.empty()) dir = "/";
        path = loc.empty() ? base.path : (loc[0] == '?' ? base.path.substr(0, base.path.find('?')) + loc : dir + loc);
    }
    if (!cleanPath(path, u.path)) return false;
    u.path = normalisePath(u.path);
    out = u;
    return true;
}

std::string urlText(const Url& u) {
    return std::string(u.tls ? "https://" : "http://") + hostHeader(u.host, u.port, u.tls) + u.path;
}

std::string fileSha256(const std::string& path, const std::atomic<bool>* cancel, const std::function<void(uint64_t)>& onProgress) {
    std::FILE* f = sys::openFile(path, "rb");
    if (!f) return std::string();
    crypto::Sha256Stream h;
    std::vector<char> buf(1 << 20);
    size_t n;
    uint64_t done = 0;
    bool ok = true;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        if (cancel && cancel->load()) { ok = false; break; }
        h.update(buf.data(), n);
        done += n;
        if (onProgress) onProgress(done);
    }
    if (std::ferror(f)) ok = false;
    std::fclose(f);
    return ok ? crypto::hex(h.finish()) : std::string();
}

DownloadResult download(const DownloadRequest& rq, CancelToken* cancel) {
    DownloadResult res;
    Url start;
    if (!parseUrl(rq.url, start)) {
        res.error = "url";
        res.detail = rq.url;
        return res;
    }
    if (!start.tls && !isLoopbackHost(start.host)) {
        res.error = "insecure";
        return res;
    }
    const uint64_t cap = rq.maxBytes ? rq.maxBytes : rq.expectedSize ? rq.expectedSize : (uint64_t(1) << 31);
    const std::string part = rq.path + ".part";
    crypto::Sha256Stream hash;
    uint64_t have = 0;

    // The .part of an earlier attempt: its bytes are hashed again, then the transfer goes on from
    // its end. Anything unusable is discarded.
    uint64_t partSize = 0;
    if (rq.resume && sys::fileSize(part, partSize) && partSize > 0 && partSize <= cap) {
        std::FILE* f = sys::openFile(part, "rb");
        std::vector<char> buf(1 << 20);
        size_t n;
        uint64_t read = 0;
        while (f && (n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
            hash.update(buf.data(), n);
            read += n;
        }
        if (f) std::fclose(f);
        if (read == partSize) {
            have = partSize;
            res.resumed = true;
        } else {
            hash.reset();
        }
    }
    if (have == 0) sys::removeFile(part);

    // Size and digest checks, then the rename. On failure the .part goes (it cannot be continued).
    auto finish = [&](DownloadResult& r) {
        r.bytes = have;
        r.sha256 = crypto::hex(hash.finish());
        if (rq.expectedSize && have != rq.expectedSize) {
            r.error = "size";
            r.detail = std::to_string(have) + " bytes, expected " + std::to_string(rq.expectedSize);
        } else if (!rq.sha256.empty() && lower(rq.sha256) != r.sha256) {
            r.error = "hash";
            r.detail = "SHA-256 " + r.sha256 + ", expected " + lower(rq.sha256);
        } else if (!sys::renameFile(part, rq.path)) {
            r.error = "io";
            r.detail = "cannot rename " + part;
            return;
        } else {
            r.ok = true;
            r.error.clear();
            return;
        }
        sys::removeFile(part);
    };
    if (rq.expectedSize && have == rq.expectedSize) {   // complete already: only the checks remain
        finish(res);
        if (res.ok || res.error == "io") return res;
        have = 0;
        res = DownloadResult();
    }

    bool restartedOnce = false;
    int retriesLeft = rq.retries;
    for (;;) {
        if (cancel && cancel->cancelled()) {
            res.error = "cancelled";
            break;
        }
        // One request (with its redirects).
        Url u = start;
        int redirects = 0;
        std::FILE* out = nullptr;
        std::string failure, failDetail;
        uint64_t receivedNow = 0, total = rq.expectedSize;
        bool restart = false, complete = false;
        HttpResponse resp;
        for (;;) {
            HttpRequest h;
            h.method = "GET";
            h.host = u.host;
            h.port = u.port;
            h.tls = u.tls;
            h.path = u.path;
            h.timeoutMs = rq.timeoutMs;
            h.maxResponseBytes = size_t(cap) + 1;   // the exact cap is checked here (an answer may start over)
            h.headers.emplace_back("User-Agent", rq.userAgent);
            if (have > 0) h.headers.emplace_back("Range", "bytes=" + std::to_string(have) + "-");
            Url next;
            bool redirected = false;
            auto onHead = [&](const HttpHead& head) {
                res.status = head.status;
                if (redirectStatus(head.status)) {
                    std::string loc = head.get("location");
                    if (loc.empty() || ++redirects > rq.maxRedirects) {
                        failure = "redirect";
                        failDetail = loc.empty() ? "redirect without a Location" : "too many redirects";
                    } else if (!resolveLocation(u, loc, next)) {
                        failure = "redirect";
                        failDetail = "bad Location " + loc;
                    } else if (!next.tls && (u.tls || !isLoopbackHost(next.host))) {
                        failure = "insecure";
                        failDetail = "redirect to " + urlText(next);
                    } else {
                        redirected = true;
                    }
                    return false;
                }
                if (head.status == 416 && have > 0 && !restartedOnce) {   // the .part does not fit this file
                    restart = true;
                    return false;
                }
                if (head.status != 200 && head.status != 206) {
                    failure = "http";
                    failDetail = "HTTP " + std::to_string(head.status) + " from " + u.host;
                    return false;
                }
                std::string cl = head.get("content-length");
                uint64_t length = cl.empty() ? 0 : std::strtoull(cl.c_str(), nullptr, 10);
                bool append = false;
                if (head.status == 206) {
                    uint64_t first = 0, whole = 0;
                    if (have == 0 || !parseContentRange(head.get("content-range"), first, whole) || first != have) {
                        if (restartedOnce) {
                            failure = "http";
                            failDetail = "unusable partial answer from " + u.host;
                        } else {
                            restart = true;
                        }
                        return false;
                    }
                    if (whole && rq.expectedSize && whole != rq.expectedSize) {
                        failure = "size";
                        failDetail = "the server has " + std::to_string(whole) + " bytes";
                        return false;
                    }
                    if (whole) total = whole;
                    append = true;
                } else {
                    if (have > 0) {   // the server ignored the Range: from the start
                        hash.reset();
                        have = 0;
                        res.resumed = false;
                    }
                    if (!cl.empty()) {
                        if (rq.expectedSize && length != rq.expectedSize) {
                            failure = "size";
                            failDetail = "the server has " + std::to_string(length) + " bytes";
                            return false;
                        }
                        total = length;
                    }
                }
                if (!cl.empty() && have + length > cap) {
                    failure = "too_large";
                    return false;
                }
                out = sys::openFile(part, append ? "ab" : "wb");
                if (!out) {
                    failure = "io";
                    failDetail = "cannot write " + part;
                    return false;
                }
                res.host = u.host;
                if (rq.onProgress) rq.onProgress(have, total);
                return true;
            };
            auto onBody = [&](const char* p, size_t n) {
                if (have + n > cap) {
                    failure = "too_large";
                    return false;
                }
                if (std::fwrite(p, 1, n, out) != n) {
                    failure = "io";
                    failDetail = "cannot write " + part;
                    return false;
                }
                hash.update(p, n);
                have += n;
                receivedNow += n;
                if (rq.onProgress) rq.onProgress(have, total);
                return true;
            };
            httpStream(h, onHead, onBody, resp, cancel);
            if (redirected && failure.empty() && resp.error.empty()) {
                LOGI("download: %s redirects to %s", u.host.c_str(), next.host.c_str());
                u = next;
                continue;
            }
            complete = out && failure.empty() && resp.error.empty();
            break;
        }
        res.redirects = redirects;
        if (out && std::fclose(out) != 0 && failure.empty()) {
            failure = "io";
            failDetail = "cannot write " + part;
            complete = false;
        }
        if (complete) {
            bool resumed = res.resumed;
            finish(res);
            // A continued file that does not hash right may have begun as another version of
            // the file (an old .part): once more from the start.
            if (!res.ok && res.error == "hash" && resumed && !restartedOnce) {
                LOGW("download: the continued file from %s does not match, starting over", u.host.c_str());
                restartedOnce = true;
                have = 0;
                res = DownloadResult();
                continue;
            }
            break;
        }
        if (restart) {
            LOGI("download: %s cannot continue the partial file, starting over", u.host.c_str());
            restartedOnce = true;
            hash.reset();
            have = 0;
            res.resumed = false;
            sys::removeFile(part);
            continue;
        }
        std::string error = !failure.empty() ? failure : !resp.error.empty() ? resp.error : "network";
        if (error == "aborted") error = "network";
        // A transfer that broke off after some data is continued from where it stopped.
        if (failure.empty() && retryable(error) && receivedNow > 0 && retriesLeft > 0 && rq.resume) {
            --retriesLeft;
            LOGW("download: transfer from %s broke off at %llu bytes (%s), continuing", u.host.c_str(), (unsigned long long)have,
                 error.c_str());
            res.resumed = true;
            if (pause(rq.retryDelayMs, cancel)) continue;
            error = "cancelled";
        }
        res.error = cancel && cancel->cancelled() ? "cancelled" : error;
        res.detail = !failDetail.empty() ? failDetail : resp.detail;
        res.bytes = have;
        if (!rq.resume || error == "too_large" || error == "size") sys::removeFile(part);
        break;
    }
    return res;
}

}  // namespace net
