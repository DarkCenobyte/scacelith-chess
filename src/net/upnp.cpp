#include "upnp.h"
#include "socket_util.h"
#include "core/log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace net {
namespace upnp {

namespace {

// Any LAN host can answer an M-SEARCH: real networks have one to three gateways, and every answer
// kept costs an HTTP fetch.
constexpr size_t kMaxSsdpAnswers = 16;     // distinct LOCATIONs
constexpr int kMaxDatagramsPerPoll = 64;   // read from one socket before the deadline is checked again

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool startsWithNoCase(const std::string& s, const char* prefix) {
    size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)prefix[i])) return false;
    return true;
}

// Lower-cased scheme of an absolute "scheme://..." URL (RFC 3986 3.1: a letter, then letters,
// digits, '+', '-' or '.'; case-insensitive), or "" for a relative reference.
std::string schemeOf(const std::string& url) {
    size_t sep = url.find("://");
    if (sep == std::string::npos || sep == 0 || !std::isalpha((unsigned char)url[0])) return "";
    for (size_t i = 1; i < sep; ++i) {
        unsigned char c = (unsigned char)url[i];
        if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return "";
    }
    return lower(url.substr(0, sep));
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&apos;"; break;
        default: o += c;
        }
    }
    return o;
}

std::string xmlUnescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") o += '&';
        else if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            long v = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? std::strtol(ent.c_str() + 2, nullptr, 16)
                                                                           : std::strtol(ent.c_str() + 1, nullptr, 10);
            if (v > 0 && v < 128) o += char(v);   // the values we read are ASCII (URLs, numbers, addresses)
        } else {
            o += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return o;
}

std::string stripComments(const std::string& xml) {
    std::string o;
    size_t pos = 0;
    for (;;) {
        size_t a = xml.find("<!--", pos);
        if (a == std::string::npos) { o += xml.substr(pos); break; }
        o += xml.substr(pos, a - pos);
        size_t b = xml.find("-->", a + 4);
        if (b == std::string::npos) break;
        pos = b + 3;
    }
    return o;
}

std::string localName(const std::string& tag) {
    size_t c = tag.find(':');
    return c == std::string::npos ? tag : tag.substr(c + 1);
}

// Reads the tag name starting at xml[p] (just after '<' or '</').
std::string tagNameAt(const std::string& xml, size_t p) {
    size_t e = p;
    while (e < xml.size() && !std::isspace((unsigned char)xml[e]) && xml[e] != '>' && xml[e] != '/') ++e;
    return xml.substr(p, e - p);
}

// Calls fn(inner text) for every element whose local name is 'name', in document order
// (outermost first; nested elements of the same name are part of their parent's inner text).
template <class Fn>
void forEachElement(const std::string& xml, const std::string& name, Fn fn) {
    size_t pos = 0;
    const size_t n = xml.size();
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t p = pos + 1;
        if (p >= n) break;
        if (xml[p] == '/' || xml[p] == '?' || xml[p] == '!') { pos = p; continue; }
        std::string tag = tagNameAt(xml, p);
        size_t gt = xml.find('>', p);
        if (gt == std::string::npos) break;
        if (localName(tag) != name) { pos = gt + 1; continue; }
        if (xml[gt - 1] == '/') { fn(std::string()); pos = gt + 1; continue; }
        // Matching close tag, counting nested elements of the same local name.
        int depth = 1;
        size_t q = gt + 1, close = std::string::npos, closeEnd = std::string::npos;
        while (depth > 0) {
            size_t lt = xml.find('<', q);
            if (lt == std::string::npos || lt + 1 >= n) break;
            bool closing = xml[lt + 1] == '/';
            std::string t = tagNameAt(xml, lt + (closing ? 2 : 1));
            size_t g = xml.find('>', lt);
            if (g == std::string::npos) break;
            if (localName(t) == name && xml[lt + 1] != '!' && xml[lt + 1] != '?') {
                if (closing) {
                    if (--depth == 0) { close = lt; closeEnd = g; }
                } else if (xml[g - 1] != '/') {
                    ++depth;
                }
            }
            q = g + 1;
        }
        if (close == std::string::npos) break;
        fn(xml.substr(gt + 1, close - gt - 1));
        pos = closeEnd + 1;
    }
}

// ---- tiny HTTP/1.1 client (plain HTTP to the LAN) --------------------------------------------

struct HttpResponse {
    int status = 0;
    std::string headers;   // lower-cased header block
    std::string body;
};

bool dechunk(const std::string& in, std::string& out, bool& complete) {
    out.clear();
    complete = false;
    size_t pos = 0;
    for (;;) {
        size_t eol = in.find("\r\n", pos);
        if (eol == std::string::npos) return true;
        std::string sizeLine = in.substr(pos, eol - pos);
        size_t semi = sizeLine.find(';');
        if (semi != std::string::npos) sizeLine.resize(semi);
        sizeLine = trim(sizeLine);
        if (sizeLine.empty() || sizeLine.size() > 8) return false;
        char* end = nullptr;
        unsigned long sz = std::strtoul(sizeLine.c_str(), &end, 16);
        if (!end || *end) return false;
        pos = eol + 2;
        if (sz == 0) { complete = true; return true; }
        if (in.size() < pos + sz + 2) return true;
        out.append(in, pos, sz);
        pos += sz + 2;
    }
}

std::string headerValue(const std::string& lowerHeaders, const char* name) {
    std::string key = std::string("\n") + name + ":";
    size_t p = lowerHeaders.find(key);
    if (p == std::string::npos) return "";
    size_t e = lowerHeaders.find('\n', p + key.size());
    return trim(lowerHeaders.substr(p + key.size(), e == std::string::npos ? std::string::npos : e - p - key.size()));
}

// cfg.cancel stops the exchange, or only until the whole request is sent when finishOnceSent (the
// router may have acted on it: its answer says what it did).
bool httpExchange(const Config& cfg, const std::string& host, uint16_t port, const std::string& request, HttpResponse& resp,
                  std::string* localIp, Error& err, bool finishOnceSent = false) {
    sock::Endpoint ep;
    if (!sock::Endpoint::parse(host, port, ep) || !ep.isV4()) { err.text = "bad_url"; return false; }
    const int64_t deadline = sock::steadyMs() + cfg.httpTimeoutMs;
    std::string cerr;
    sock::Handle h = sock::connectWithTimeout(ep, cfg.httpTimeoutMs, cerr, cfg.cancel);
    if (h == sock::kInvalid) { err.text = cerr; return false; }
    if (localIp) {
        sock::Endpoint le;
        if (sock::localEndpoint(h, le)) *localIp = le.ip();
    }
    struct Closer { sock::Handle h; ~Closer() { sock::closeSocket(h); } } closer{h};

    size_t sent = 0;
    std::string raw;
    bool closed = false;
    size_t headerEnd = std::string::npos;
    for (;;) {
        if (cfg.cancel && cfg.cancel->load() && !(finishOnceSent && sent == request.size())) { err.text = "cancelled"; return false; }
        int64_t left = deadline - sock::steadyMs();
        if (left <= 0) { err.text = "timeout"; return false; }
        sock::PollSet ps;
        ps.add(h, true, sent < request.size());
        if (ps.wait(int(std::min<int64_t>(left, 100))) < 0) { err.text = "network"; return false; }
        if (sent < request.size() && ps.writable(h)) {
            int r = sock::sendSome(h, reinterpret_cast<const uint8_t*>(request.data()) + sent, request.size() - sent);
            if (r < 0) { err.text = "reset"; return false; }
            sent += size_t(r);
        }
        if (ps.readable(h)) {
            uint8_t buf[4096];
            for (;;) {
                int r = sock::recvSome(h, buf, sizeof buf, closed);
                if (r > 0) {
                    raw.append(reinterpret_cast<char*>(buf), size_t(r));
                    if (raw.size() > cfg.maxHttpBytes) { err.text = "too_large"; return false; }
                    continue;
                }
                if (r < 0 && !closed) { err.text = "reset"; return false; }
                break;
            }
        }
        if (headerEnd == std::string::npos) headerEnd = raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            std::string hdr = "\n" + lower(raw.substr(0, headerEnd)) + "\n";
            std::string te = headerValue(hdr, "transfer-encoding");
            std::string cl = headerValue(hdr, "content-length");
            std::string rest = raw.substr(headerEnd + 4);
            bool done = closed;
            std::string body = rest;
            if (te.find("chunked") != std::string::npos) {
                bool complete = false;
                if (!dechunk(rest, body, complete)) { err.text = "bad_http"; return false; }
                done = complete || closed;
                if (closed && !complete) { err.text = "truncated"; return false; }
            } else if (!cl.empty()) {
                size_t want = size_t(std::strtoul(cl.c_str(), nullptr, 10));
                if (want > cfg.maxHttpBytes) { err.text = "too_large"; return false; }
                if (rest.size() >= want) { body = rest.substr(0, want); done = true; }
                else if (closed) { err.text = "truncated"; return false; }
            }
            if (done) {
                // Status line: "HTTP/1.1 200 OK".
                size_t sp = raw.find(' ');
                if (!startsWithNoCase(raw, "HTTP/") || sp == std::string::npos || sp > headerEnd) { err.text = "bad_http"; return false; }
                resp.status = std::atoi(raw.c_str() + sp + 1);
                resp.headers = hdr;
                resp.body = body;
                err.httpStatus = resp.status;
                return true;
            }
        } else if (closed) {
            err.text = "bad_http";
            return false;
        }
    }
}

bool httpGet(const Config& cfg, const std::string& url, HttpResponse& resp, std::string* localIp, Error& err) {
    std::string host, path;
    uint16_t port = 0;
    if (!splitHttpUrl(url, host, port, path)) { err.text = "bad_url"; return false; }
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                      "\r\nConnection: close\r\nUser-Agent: Scacelith UPnP/1.1\r\n\r\n";
    return httpExchange(cfg, host, port, req, resp, localIp, err);
}

bool isMulticastV4(const std::string& ip) {
    int a = std::atoi(ip.c_str());
    return a >= 224 && a <= 239;
}

bool hostOfUrl(const std::string& url, std::string& host) {
    std::string path;
    uint16_t port = 0;
    return splitHttpUrl(url, host, port, path);
}

}  // namespace

// ---- parsing helpers ----------------------------------------------------------------------------

bool xmlFind(const std::string& xml, const std::string& name, std::string& text) {
    bool found = false;
    forEachElement(xml, name, [&](const std::string& inner) {
        if (!found) { text = xmlUnescape(trim(inner)); found = true; }
    });
    return found;
}

bool parseSsdpResponse(const std::string& text, SsdpResponse& out) {
    out = SsdpResponse();
    if (!startsWithNoCase(text, "HTTP/1.1 200") && !startsWithNoCase(text, "HTTP/1.0 200")) return false;
    size_t pos = text.find('\n');
    while (pos != std::string::npos && pos + 1 < text.size()) {
        size_t e = text.find('\n', pos + 1);
        std::string line = trim(text.substr(pos + 1, e == std::string::npos ? std::string::npos : e - pos - 1));
        pos = e;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = lower(trim(line.substr(0, colon)));
        std::string val = trim(line.substr(colon + 1));
        if (key == "location") out.location = val;
        else if (key == "server") out.server = val;
        else if (key == "usn") out.usn = val;
    }
    return !out.location.empty();
}

bool parseDescription(const std::string& xmlIn, Description& out) {
    out = Description();
    std::string xml = stripComments(xmlIn);
    bool hasDevice = false;
    forEachElement(xml, "device", [&](const std::string&) { hasDevice = true; });
    if (!hasDevice) return false;
    xmlFind(xml, "URLBase", out.urlBase);
    xmlFind(xml, "deviceType", out.deviceType);
    xmlFind(xml, "friendlyName", out.friendlyName);
    forEachElement(xml, "service", [&](const std::string& inner) {
        Service s;
        xmlFind(inner, "serviceType", s.serviceType);
        xmlFind(inner, "controlURL", s.controlUrl);
        if (!s.serviceType.empty() && !s.controlUrl.empty()) out.services.push_back(s);
    });
    return true;
}

bool parseSoapFault(const std::string& xml, int& code, std::string& description) {
    std::string c;
    if (!xmlFind(xml, "errorCode", c) || c.empty()) return false;
    code = std::atoi(c.c_str());
    description.clear();
    xmlFind(xml, "errorDescription", description);
    return code != 0;
}

bool splitHttpUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
    if (schemeOf(url) != "http") return false;
    // The path is copied into the request line ("GET <path> HTTP/1.1"): a space, CR or LF from the
    // network (a bare CR survives the SSDP line split, &#13;&#10; the XML unescaping) would forge
    // the request. RFC 3986 allows none of them in a URL.
    if (std::any_of(url.begin(), url.end(), [](char ch) { return (unsigned char)ch <= ' ' || ch == '\x7f'; })) return false;
    std::string rest = url.substr(7);   // after "http://"
    size_t slash = rest.find('/');
    std::string auth = rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = auth.rfind(':');
    host = auth.substr(0, colon);
    port = 80;
    if (colon != std::string::npos) {
        std::string p = auth.substr(colon + 1);
        if (p.empty() || p.size() > 5 || !std::all_of(p.begin(), p.end(), [](char ch) { return std::isdigit((unsigned char)ch); }))
            return false;
        long v = std::strtol(p.c_str(), nullptr, 10);
        if (v <= 0 || v > 65535) return false;
        port = uint16_t(v);
    }
    sock::Endpoint ep;
    return !host.empty() && sock::Endpoint::parse(host, port, ep) && ep.isV4();
}

std::string resolveUrl(const std::string& base, const std::string& ref) {
    if (!schemeOf(ref).empty()) return ref;   // absolute: splitHttpUrl() then refuses all but http
    size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return ref;
    size_t pathStart = base.find('/', schemeEnd + 3);
    std::string origin = base.substr(0, pathStart);
    if (!ref.empty() && ref[0] == '/') return origin + ref;
    std::string path = pathStart == std::string::npos ? "/" : base.substr(pathStart);
    size_t q = path.find_first_of("?#");
    if (q != std::string::npos) path.resize(q);
    path.resize(path.rfind('/') + 1);
    return origin + path + ref;
}

const Service* pickService(const Description& d) {
    static const char* prefs[] = {
        "urn:schemas-upnp-org:service:WANIPConnection:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };
    for (const char* p : prefs)
        for (auto& s : d.services)
            if (s.serviceType == p) return &s;
    return nullptr;
}

bool cgnatSuspected(const std::string& ip) {
    sock::Endpoint ep;
    if (ip.empty() || !sock::Endpoint::parse(ip, 0, ep) || !ep.isV4()) return true;
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return true;
    if (a == 10) return true;                               // RFC 1918
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 100 && b >= 64 && b <= 127) return true;       // RFC 6598 shared address space (CGNAT)
    if (a == 169 && b == 254) return true;                  // link-local
    if (a == 127 || a == 0) return true;                    // loopback, unspecified
    if (a == 192 && b == 0 && c == 0) return true;          // IETF protocol assignments (DS-Lite B4 192.0.0.0/29)
    if (a == 198 && (b == 18 || b == 19)) return true;      // benchmarking
    if (a >= 224) return true;                              // multicast, reserved, broadcast
    return false;
}

// ---- Client -------------------------------------------------------------------------------------

Client::Client(const Config& cfg) : cfg_(cfg) {}

bool Client::discover(Gateway& out, Error& err) {
    err = Error();
    out = Gateway();
    sock::startup();
    sock::Endpoint target;
    if (!sock::Endpoint::parse(cfg_.discoveryAddress, cfg_.discoveryPort, target) || !target.isV4()) {
        err.text = "bad_discovery_address";
        return false;
    }
    const bool multicast = isMulticastV4(cfg_.discoveryAddress);
    static const char* searchTargets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
    };
    std::vector<std::string> queries;
    for (const char* st : searchTargets)
        queries.push_back("M-SEARCH * HTTP/1.1\r\nHOST: " + cfg_.discoveryAddress + ":" + std::to_string(cfg_.discoveryPort) +
                          "\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: " + st + "\r\n\r\n");

    // One socket per IPv4 interface: a multicast query leaves through the interface its socket
    // is bound to, and routers only answer on their LAN side.
    std::vector<sock::Handle> socks;
    std::vector<std::string> ifaces;
    if (multicast) ifaces = sock::localAddresses(true, false);
    if (ifaces.empty()) ifaces.push_back("");
    for (auto& ip : ifaces) {
        sock::Handle h = sock::openUdpV4();
        if (h == sock::kInvalid) continue;
        sock::Endpoint local = sock::Endpoint::anyV4(0);
        if (!ip.empty() && !sock::Endpoint::parse(ip, 0, local)) local = sock::Endpoint::anyV4(0);
        if (!sock::bindTo(h, local, false)) { sock::closeSocket(h); continue; }
        if (multicast) {
            if (!ip.empty()) sock::setMulticastIf(h, local);
            sock::setMulticastTtl(h, 2);
        }
        socks.push_back(h);
    }
    struct Closer { std::vector<sock::Handle>& v; ~Closer() { for (auto h : v) sock::closeSocket(h); } } closer{socks};
    if (socks.empty()) { err.text = "network"; return false; }
    auto sendQueries = [&] {
        for (auto h : socks)
            for (auto& q : queries) sock::sendTo(h, reinterpret_cast<const uint8_t*>(q.data()), q.size(), target);
    };
    sendQueries();

    std::vector<SsdpResponse> answers;
    const int64_t start = sock::steadyMs();
    int64_t deadline = start + cfg_.discoveryMs;
    bool resent = false;
    while (!cancelled()) {
        int64_t now = sock::steadyMs();
        if (now >= deadline) break;
        if (!resent && answers.empty() && now - start >= cfg_.discoveryMs / 3) {
            sendQueries();   // UDP may be lost: ask once more
            resent = true;
        }
        sock::PollSet ps;
        for (auto h : socks) ps.add(h, true, false);
        if (ps.wait(int(std::min<int64_t>(deadline - now, 100))) < 0) break;
        for (auto h : socks) {
            if (!ps.readable(h)) continue;
            uint8_t buf[2048];
            sock::Endpoint from;
            int r, n = 0;
            while (n++ < kMaxDatagramsPerPoll && (r = sock::recvFrom(h, buf, sizeof buf - 1, from)) > 0) {
                SsdpResponse resp;
                if (!parseSsdpResponse(std::string(reinterpret_cast<char*>(buf), size_t(r)), resp)) continue;
                resp.from = from.ip();
                // The description must come from the device that answered (no third-party fetch).
                std::string locHost;
                if (!hostOfUrl(resp.location, locHost) || locHost != resp.from) {
                    LOGD("upnp: ignoring SSDP answer from %s with LOCATION %s", resp.from.c_str(), resp.location.c_str());
                    continue;
                }
                bool dup = false;
                for (auto& a : answers) dup = dup || a.location == resp.location;
                if (dup || answers.size() >= kMaxSsdpAnswers) continue;
                answers.push_back(resp);
                if (answers.size() == 1) deadline = std::min(deadline, sock::steadyMs() + cfg_.settleMs);
            }
        }
    }
    if (cancelled()) { err.text = "cancelled"; return false; }
    if (answers.empty()) { err.text = "no_gateway"; return false; }

    // Descriptions -> candidate WAN connection services.
    std::vector<Gateway> candidates;
    for (auto& a : answers) {
        if (cancelled()) break;
        HttpResponse resp;
        Error e;
        std::string localIp;
        if (!httpGet(cfg_, a.location, resp, &localIp, e) || resp.status != 200) {
            LOGW("upnp: description %s: %s (HTTP %d)", a.location.c_str(), e.text.c_str(), resp.status);
            continue;
        }
        Description d;
        if (!parseDescription(resp.body, d)) continue;
        const Service* svc = pickService(d);
        if (!svc) continue;
        Gateway g;
        g.server = a.server;
        g.friendlyName = d.friendlyName;
        g.serviceType = svc->serviceType;
        g.controlUrl = resolveUrl(d.urlBase.empty() ? a.location : d.urlBase, svc->controlUrl);
        g.localAddress = localIp;
        std::string descHost;
        hostOfUrl(a.location, descHost);
        if (!splitHttpUrl(g.controlUrl, g.host, g.port, g.path) || g.host != descHost) {
            LOGW("upnp: control URL %s refused (not on %s)", g.controlUrl.c_str(), descHost.c_str());
            continue;
        }
        candidates.push_back(g);
    }
    if (cancelled()) { err.text = "cancelled"; return false; }
    if (candidates.empty()) { err.text = "no_wan_service"; return false; }

    // Several WAN connections (or devices) may exist: prefer one that reports an address.
    for (auto& g : candidates) {
        std::string ip;
        Error e;
        if (getExternalIp(g, ip, e) && !ip.empty() && ip != "0.0.0.0") {
            g.externalIp = ip;
            out = g;
            return true;
        }
    }
    out = candidates.front();
    return true;
}

bool Client::soap(const Gateway& gw, const char* action, const std::vector<std::pair<std::string, std::string>>& args,
                  std::string& body, Error& err, bool finishOnceSent) {
    err = Error();
    std::string xml = "<?xml version=\"1.0\"?>\r\n"
                      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:";
    xml += action;
    xml += " xmlns:u=\"" + xmlEscape(gw.serviceType) + "\">";
    for (auto& a : args) xml += "<" + a.first + ">" + xmlEscape(a.second) + "</" + a.first + ">";
    xml += "</u:";
    xml += action;
    xml += "></s:Body></s:Envelope>\r\n";
    std::string req = "POST " + gw.path + " HTTP/1.1\r\nHost: " + gw.host + ":" + std::to_string(gw.port) +
                      "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + gw.serviceType + "#" + action +
                      "\"\r\nContent-Length: " + std::to_string(xml.size()) + "\r\nConnection: close\r\n\r\n" + xml;
    HttpResponse resp;
    if (!httpExchange(cfg_, gw.host, gw.port, req, resp, nullptr, err, finishOnceSent)) return false;
    body = resp.body;
    if (resp.status == 200) return true;
    int code = 0;
    std::string desc;
    if (parseSoapFault(resp.body, code, desc)) {
        err.upnpCode = code;
        err.text = std::to_string(code) + (desc.empty() ? "" : " " + desc);
    } else {
        err.text = "http " + std::to_string(resp.status);
    }
    return false;
}

bool Client::getExternalIp(const Gateway& gw, std::string& ip, Error& err) {
    std::string body;
    if (!soap(gw, "GetExternalIPAddress", {}, body, err)) return false;
    ip.clear();
    if (!xmlFind(body, "NewExternalIPAddress", ip)) { err.text = "bad_response"; return false; }
    return true;
}

bool Client::addPortMapping(const Gateway& gw, const Mapping& m, Error& err) {
    std::string client = m.internalClient.empty() ? gw.localAddress : m.internalClient;
    if (client.empty()) { err = Error(); err.text = "no_local_address"; return false; }
    std::string body;
    return soap(gw, "AddPortMapping",
                {{"NewRemoteHost", ""},
                 {"NewExternalPort", std::to_string(m.externalPort)},
                 {"NewProtocol", "TCP"},
                 {"NewInternalPort", std::to_string(m.internalPort)},
                 {"NewInternalClient", client},
                 {"NewEnabled", "1"},
                 {"NewPortMappingDescription", m.description},
                 {"NewLeaseDuration", std::to_string(m.leaseSec)}},
                body, err, true);   // awaited once sent, despite a cancel: a mapping made is known (and deleted)
}

bool Client::deletePortMapping(const Gateway& gw, uint16_t externalPort, Error& err) {
    std::string body;
    return soap(gw, "DeletePortMapping",
                {{"NewRemoteHost", ""}, {"NewExternalPort", std::to_string(externalPort)}, {"NewProtocol", "TCP"}}, body, err);
}

bool Client::mapPort(const Gateway& gw, uint16_t port, const std::function<bool(uint16_t)>& claimLocalPort, Mapping& out,
                     Error& err) {
    Mapping m;
    m.externalPort = m.internalPort = port;
    m.leaseSec = kDefaultLeaseSec;
    m.internalClient = gw.localAddress;
    for (int attempt = 0; attempt < kMaxMappingTries && !cancelled(); ++attempt) {
        if (addPortMapping(gw, m, err)) {
            out = m;
            return true;
        }
        if (cancelled()) break;   // cancelled meanwhile: no other port is claimed or asked
        if (err.upnpCode == 725 && m.leaseSec != 0) {
            m.leaseSec = 0;   // OnlyPermanentLeasesSupported: the mapping is deleted when the match ends
            continue;
        }
        if (err.upnpCode == 718) {
            // ConflictInMappingEntry: another machine has this external port; move to the next one
            // that is also free here (internal and external ports stay equal).
            bool found = false;
            uint16_t p = m.externalPort;
            for (int k = 0; k < 32 && !found; ++k) {
                p = p >= 65535 ? 1024 : uint16_t(p + 1);
                found = !claimLocalPort || claimLocalPort(p);
            }
            if (!found) return false;
            m.externalPort = m.internalPort = p;
            continue;
        }
        return false;
    }
    if (cancelled()) err.text = "cancelled";
    else if (err.text.empty()) err.text = "too_many_attempts";
    return false;
}

bool Client::renew(const Gateway& gw, const Mapping& m, Error& err) {
    if (m.leaseSec == 0) return true;
    return addPortMapping(gw, m, err);
}

void RenewalSchedule::mapped(int64_t now, uint32_t leaseSec) {
    leaseSec_ = leaseSec;
    renewAt_ = now + kRenewEveryMs;
    leaseEnd_ = now + int64_t(leaseSec) * 1000;
    lost_ = false;
}

void RenewalSchedule::started(int64_t now) {
    start_ = now;
    renewAt_ = now + kRenewEveryMs;
}

bool RenewalSchedule::finished(int64_t now, bool ok) {
    if (!ok) {
        if (now < leaseEnd_) renewAt_ = now + kRenewRetryMs;
        return false;
    }
    leaseEnd_ = start_ + int64_t(leaseSec_) * 1000;   // counted from the renewal's request
    const bool back = lost_;
    lost_ = false;
    return back;
}

bool RenewalSchedule::lapsed(int64_t now) {
    if (!leaseSec_ || lost_ || now < leaseEnd_) return false;
    lost_ = true;
    return true;
}

}  // namespace upnp
}  // namespace net

