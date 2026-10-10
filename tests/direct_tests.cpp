// Direct match: UPnP client against a fake gateway on 127.0.0.1 (a cancel during AddPortMapping,
// at most 16 SSDP answers fetched), the carrier-grade NAT ranges and the lease renewal schedule,
// the secure channel (vectors and failure cases), the authority with synthetic time (names the
// protocol accepts, the 1200-ply cap) and full loopback matches between two DirectMatch
// instances, gestures included (both ways, paced, never replayed, the first one after a
// reconnection never dropped, outside the flood limit). A guest and a host written by hand check
// the limits: refusals and floods closed without stalling the host, a host flooding the guest
// dropped, messages out of sequence and connections logged at a bounded rate, a message of an
// unknown type refused, one more attempt after a close before the host's confirmation.
#include "test.h"
#include "chess/chess.h"
#include "core/log.h"
#include "net/direct_authority.h"
#include "net/direct_crypto.h"
#include "net/direct_match.h"
#include "net/protocol_gen.h"
#include "net/socket_util.h"
#include "net/upnp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace net;

namespace {

std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (; s[0] && s[1]; s += 2) v.push_back(uint8_t(nib(s[0]) << 4 | nib(s[1])));
    return v;
}

std::string toHex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}

// ---- fake Internet gateway: SSDP responder + HTTP/SOAP server on 127.0.0.1 ----------------------

struct FakeGateway {
    sock::Handle udp = sock::kInvalid, tcp = sock::kInvalid;
    uint16_t udpPort = 0, httpPort = 0;
    std::thread th;
    std::atomic<bool> stop{false};
    std::mutex m;
    // Behaviour. "%HTTP%" in the description is replaced by the HTTP port.
    std::string description;
    bool chunked = false;
    std::string externalIp = "203.0.113.7";
    std::deque<int> addResults;           // next AddPortMapping answers: 0 = success, else a UPnP error code
    int extraLocations = 0;               // more answers to each M-SEARCH, with LOCATIONs that are not found
    std::atomic<bool>* cancelOnAdd = nullptr;   // set once an AddPortMapping is received...
    int addDelayMs = 0;                   // ...and its answer comes this much later
    // What the client did.
    std::vector<std::string> searchTargets;
    struct Add { int extPort = 0, intPort = 0, lease = -1; std::string client, desc, proto; };
    std::vector<Add> adds;
    std::vector<int> deletes;
    std::vector<std::string> soapPaths;
    int getIpCalls = 0, httpGets = 0;

    ~FakeGateway() { shutdown(); }

    bool start() {
        udp = sock::openUdpV4();
        sock::Endpoint lo, ep;
        sock::Endpoint::parse("127.0.0.1", 0, lo);
        if (udp == sock::kInvalid || !sock::bindTo(udp, lo, false) || !sock::localEndpoint(udp, ep)) return false;
        udpPort = ep.port();
        bool dual = false;
        std::string err;
        tcp = sock::listenTcp(0, dual, err);
        if (tcp == sock::kInvalid || !sock::localEndpoint(tcp, ep)) return false;
        httpPort = ep.port();
        th = std::thread([this] { run(); });
        return true;
    }

    void shutdown() {
        stop = true;
        if (th.joinable()) th.join();
        sock::closeSocket(udp);
        sock::closeSocket(tcp);
        udp = tcp = sock::kInvalid;
    }

    upnp::Config clientConfig() const {
        upnp::Config c;
        c.discoveryAddress = "127.0.0.1";
        c.discoveryPort = udpPort;
        c.discoveryMs = 1500;
        c.settleMs = 50;
        c.httpTimeoutMs = 2000;
        return c;
    }

    void run() {
        while (!stop) {
            sock::PollSet ps;
            ps.add(udp, true, false);
            ps.add(tcp, true, false);
            if (ps.wait(50) <= 0) continue;
            if (ps.readable(udp)) {
                uint8_t buf[2048];
                sock::Endpoint from;
                int r;
                while ((r = sock::recvFrom(udp, buf, sizeof buf, from)) > 0) {
                    std::string q(reinterpret_cast<char*>(buf), size_t(r));
                    if (q.compare(0, 8, "M-SEARCH") != 0) continue;
                    size_t st = q.find("\r\nST: ");
                    std::string target = st == std::string::npos ? "" : q.substr(st + 6, q.find("\r\n", st + 6) - st - 6);
                    {
                        std::lock_guard<std::mutex> lk(m);
                        searchTargets.push_back(target);
                    }
                    std::string resp = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: " + target +
                                       "\r\nUSN: uuid:fake-igd::" + target + "\r\nEXT:\r\nSERVER: Fake/1.0 UPnP/1.1 Test/1.0\r\n"
                                       "LOCATION: http://127.0.0.1:" + std::to_string(httpPort) + "/desc.xml\r\n\r\n";
                    sock::sendTo(udp, reinterpret_cast<const uint8_t*>(resp.data()), resp.size(), from);
                    for (int k = 0; k < extraLocations; ++k) {
                        std::string extra = replaceAll(resp, "/desc.xml", "/extra" + std::to_string(k) + ".xml");
                        sock::sendTo(udp, reinterpret_cast<const uint8_t*>(extra.data()), extra.size(), from);
                    }
                }
            }
            if (ps.readable(tcp)) {
                sock::Handle c = sock::acceptOne(tcp, nullptr);
                if (c != sock::kInvalid) { serve(c); sock::closeSocket(c); }
            }
        }
    }

    static std::string field(const std::string& xml, const char* name) {
        std::string v;
        upnp::xmlFind(xml, name, v);
        return v;
    }

    void serve(sock::Handle c) {
        std::string req;
        int64_t deadline = sock::steadyMs() + 2000;
        size_t need = std::string::npos;
        while (sock::steadyMs() < deadline && !stop) {
            sock::PollSet ps;
            ps.add(c, true, false);
            ps.wait(20);
            uint8_t buf[4096];
            bool closed = false;
            int r = sock::recvSome(c, buf, sizeof buf, closed);
            if (r > 0) req.append(reinterpret_cast<char*>(buf), size_t(r));
            if (r < 0) break;
            size_t he = req.find("\r\n\r\n");
            if (he != std::string::npos && need == std::string::npos) {
                size_t cl = req.find("Content-Length: ");
                need = he + 4 + (cl == std::string::npos || cl > he ? 0 : size_t(std::atoi(req.c_str() + cl + 16)));
            }
            if (need != std::string::npos && req.size() >= need) break;
        }
        std::string status = "200 OK", body;
        int delayMs = 0;
        if (req.compare(0, 4, "GET ") == 0) {
            std::lock_guard<std::mutex> lk(m);
            ++httpGets;
        }
        if (req.compare(0, 14, "GET /desc.xml ") == 0) {
            std::lock_guard<std::mutex> lk(m);
            body = replaceAll(description, "%HTTP%", std::to_string(httpPort));
        } else if (req.compare(0, 5, "POST ") == 0) {
            std::lock_guard<std::mutex> lk(m);
            soapPaths.push_back(req.substr(5, req.find(' ', 5) - 5));
            std::string xml = req.substr(req.find("\r\n\r\n") + 4);
            auto fault = [&](int code, const char* text) {
                status = "500 Internal Server Error";
                body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><s:Fault>"
                       "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
                       "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>" +
                       std::to_string(code) + "</errorCode><errorDescription>" + text +
                       "</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
            };
            if (req.find("#GetExternalIPAddress\"") != std::string::npos) {
                ++getIpCalls;
                body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                       "<u:GetExternalIPAddressResponse xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">"
                       "<NewExternalIPAddress>" + externalIp + "</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>";
            } else if (req.find("#AddPortMapping\"") != std::string::npos) {
                Add a;
                a.extPort = std::atoi(field(xml, "NewExternalPort").c_str());
                a.intPort = std::atoi(field(xml, "NewInternalPort").c_str());
                a.lease = std::atoi(field(xml, "NewLeaseDuration").c_str());
                a.client = field(xml, "NewInternalClient");
                a.desc = field(xml, "NewPortMappingDescription");
                a.proto = field(xml, "NewProtocol");
                adds.push_back(a);
                if (cancelOnAdd) *cancelOnAdd = true;
                delayMs = addDelayMs;
                int result = 0;
                if (!addResults.empty()) { result = addResults.front(); addResults.pop_front(); }
                if (result == 718) fault(718, "ConflictInMappingEntry");
                else if (result == 725) fault(725, "OnlyPermanentLeasesSupported");
                else if (result) fault(result, "Error");
                else body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                            "<u:AddPortMappingResponse xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"/></s:Body></s:Envelope>";
            } else if (req.find("#DeletePortMapping\"") != std::string::npos) {
                deletes.push_back(std::atoi(field(xml, "NewExternalPort").c_str()));
                body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                       "<u:DeletePortMappingResponse xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"/></s:Body></s:Envelope>";
            } else {
                fault(401, "Invalid Action");
            }
        } else {
            status = "404 Not Found";
        }
        if (delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        std::string resp = "HTTP/1.1 " + status + "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nConnection: close\r\n";
        if (chunked && status[0] == '2') {
            // Two chunks, to exercise the client's chunked decoding.
            size_t half = body.size() / 2;
            char h1[24], h2[24];
            std::snprintf(h1, sizeof h1, "%zx", half);
            std::snprintf(h2, sizeof h2, "%zx", body.size() - half);
            resp += "Transfer-Encoding: chunked\r\n\r\n" + std::string(h1) + "\r\n" + body.substr(0, half) + "\r\n" + h2 + "\r\n" +
                    body.substr(half) + "\r\n0\r\n\r\n";
        } else {
            resp += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        }
        size_t sent = 0;
        int64_t sd = sock::steadyMs() + 2000;
        while (sent < resp.size() && sock::steadyMs() < sd) {
            int r = sock::sendSome(c, reinterpret_cast<const uint8_t*>(resp.data()) + sent, resp.size() - sent);
            if (r < 0) break;
            if (r == 0) { sock::PollSet ps; ps.add(c, false, true); ps.wait(20); }
            sent += size_t(std::max(r, 0));
        }
        sock::shutdownSend(c);
    }
};

// A typical IGD v1 description: root device -> WANDevice -> WANConnectionDevice with a PPP and
// an IP connection service (the IP one must be chosen).
const char* kDescRelative = R"(<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <URLBase>http://127.0.0.1:%HTTP%/</URLBase>
  <!-- <friendlyName>not this one</friendlyName> -->
  <device>
    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
    <friendlyName>Fake Box &amp; Co</friendlyName>
    <serviceList><service>
      <serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
      <controlURL>/ctl/L3F</controlURL>
    </service></serviceList>
    <deviceList><device>
      <deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
      <friendlyName>WANDevice</friendlyName>
      <deviceList><device>
        <deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
        <serviceList>
          <service>
            <serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>
            <controlURL>ctl/PPPConn</controlURL>
          </service>
          <service>
            <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
            <serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
            <controlURL>ctl/IPConn</controlURL>
          </service>
        </serviceList>
      </device></deviceList>
    </device></deviceList>
  </device>
</root>)";

// IGD v2 with an absolute control URL and namespace prefixes, no URLBase.
const char* kDescAbsolute = R"(<?xml version="1.0"?>
<u:root xmlns:u="urn:schemas-upnp-org:device-1-0">
  <u:device>
    <u:deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:2</u:deviceType>
    <u:friendlyName>Fake IGD2</u:friendlyName>
    <u:deviceList><u:device><u:deviceList><u:device>
      <u:serviceList><u:service>
        <u:serviceType>urn:schemas-upnp-org:service:WANIPConnection:2</u:serviceType>
        <u:controlURL>http://127.0.0.1:%HTTP%/upnp/control/WANIPConn2</u:controlURL>
      </u:service></u:serviceList>
    </u:device></u:deviceList></u:device></u:deviceList>
  </u:device>
</u:root>)";

// A description whose control URL points to another machine: must be refused.
const char* kDescForeign = R"(<?xml version="1.0"?>
<root><device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
<controlURL>http://10.9.8.7:5000/ctl/IPConn</controlURL></service></serviceList></device></root>)";

}  // namespace

// ---- sockets ------------------------------------------------------------------------------------

#ifndef _WIN32
// PollSet on a descriptor past FD_SETSIZE (1024), which select() cannot take: nothing until the
// timeout when idle, then readable when a datagram waits (and writable). The usual soft limit of
// 1024 descriptors is raised for the test (as far as the hard limit allows), then restored.
TEST(sock_poll_set_high_descriptor) {
    rlimit old{}, raised{};
    if (getrlimit(RLIMIT_NOFILE, &old) == 0) {
        raised = old;
        if (old.rlim_cur <= 1500 && old.rlim_max > 1500) {
            raised.rlim_cur = std::min<rlim_t>(old.rlim_max, 4096);
            if (setrlimit(RLIMIT_NOFILE, &raised) != 0) raised = old;
        }
    }
    if (raised.rlim_cur <= 1500) SKIP("fewer than 1501 descriptors allowed");
    CHECK(sock::startup());
    sock::Handle a = sock::openUdpV4(), b = sock::openUdpV4();
    CHECK(a != sock::kInvalid && b != sock::kInvalid);
    sock::Endpoint loop, at;
    CHECK(sock::Endpoint::parse("127.0.0.1", 0, loop));
    CHECK(sock::bindTo(a, loop, false));
    CHECK(sock::localEndpoint(a, at));
    const sock::Handle high = fcntl(a, F_DUPFD, 1500);   // the lowest free descriptor from 1500
    CHECK(high >= 1500);
    sock::PollSet ps;
    ps.add(high, true, false);
    auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(ps.wait(100), 0);
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(90));
    CHECK(!ps.readable(high));
    const uint8_t one[1] = {7};
    CHECK_EQ(sock::sendTo(b, one, 1, at), 1);
    // Readable once the datagram is there (macOS delivers on the loopback asynchronously: a socket
    // that is always writable would end the wait before it arrives), then readable and writable.
    ps.clear();
    ps.add(high, true, false);
    CHECK_EQ(ps.wait(2000), 1);
    CHECK(ps.readable(high));
    ps.clear();
    ps.add(high, true, true);
    CHECK_EQ(ps.wait(2000), 1);
    CHECK(ps.readable(high));
    CHECK(ps.writable(high));
    sock::closeSocket(high);
    sock::closeSocket(a);
    sock::closeSocket(b);
    setrlimit(RLIMIT_NOFILE, &old);
}
#endif

// ---- UPnP ---------------------------------------------------------------------------------------

TEST(upnp_parsers) {
    upnp::SsdpResponse r;
    CHECK(upnp::parseSsdpResponse("HTTP/1.1 200 OK\r\nCache-Control: max-age=1800\r\nlocation:  http://192.168.1.1:5431/dyndev/uuid:1 \r\n"
                                  "SERVER: Linux UPnP/1.0\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nUSN: uuid:1::x\r\n\r\n",
                                  r));
    CHECK_EQ(r.location, std::string("http://192.168.1.1:5431/dyndev/uuid:1"));
    CHECK_EQ(r.server, std::string("Linux UPnP/1.0"));
    CHECK_EQ(r.usn, std::string("uuid:1::x"));
    CHECK(!upnp::parseSsdpResponse("NOTIFY * HTTP/1.1\r\nLOCATION: http://1.2.3.4/\r\n\r\n", r));
    CHECK(!upnp::parseSsdpResponse("HTTP/1.1 200 OK\r\nST: x\r\n\r\n", r));

    upnp::Description d;
    CHECK(upnp::parseDescription(replaceAll(kDescRelative, "%HTTP%", "80"), d));
    CHECK_EQ(d.friendlyName, std::string("Fake Box & Co"));
    CHECK_EQ(d.deviceType, std::string("urn:schemas-upnp-org:device:InternetGatewayDevice:1"));
    CHECK_EQ(d.urlBase, std::string("http://127.0.0.1:80/"));
    CHECK_EQ(d.services.size(), size_t(3));
    const upnp::Service* s = upnp::pickService(d);
    CHECK(s && s->serviceType == "urn:schemas-upnp-org:service:WANIPConnection:1" && s->controlUrl == "ctl/IPConn");
    CHECK(!upnp::parseDescription("<html><body>router login</body></html>", d));

    CHECK_EQ(upnp::resolveUrl("http://192.168.1.1:5000/rootDesc.xml", "/ctl/IPConn"), std::string("http://192.168.1.1:5000/ctl/IPConn"));
    CHECK_EQ(upnp::resolveUrl("http://192.168.1.1:5000/igd/desc.xml", "ctl/IPConn"), std::string("http://192.168.1.1:5000/igd/ctl/IPConn"));
    CHECK_EQ(upnp::resolveUrl("http://192.168.1.1:5000", "ctl"), std::string("http://192.168.1.1:5000/ctl"));
    CHECK_EQ(upnp::resolveUrl("http://192.168.1.1:5000/a/", "http://192.168.1.1:80/x"), std::string("http://192.168.1.1:80/x"));
    CHECK_EQ(upnp::resolveUrl("http://192.168.1.1:5000/a/", "ftp://192.168.1.1/x"), std::string("ftp://192.168.1.1/x"));   // then refused
    CHECK_EQ(upnp::resolveUrl("http://192.168.1.1:5000/a/", "x?u=http://h/"), std::string("http://192.168.1.1:5000/a/x?u=http://h/"));
    std::string host, path;
    uint16_t port = 0;
    CHECK(upnp::splitHttpUrl("http://192.168.1.1:5000/ctl/IPConn", host, port, path));
    CHECK(host == "192.168.1.1" && port == 5000 && path == "/ctl/IPConn");
    CHECK(upnp::splitHttpUrl("HTTP://10.0.0.1", host, port, path) && port == 80 && path == "/");
    CHECK(!upnp::splitHttpUrl("https://192.168.1.1/x", host, port, path));
    CHECK(!upnp::splitHttpUrl("http://router.lan/x", host, port, path));   // IPv4 literals only
    CHECK(!upnp::splitHttpUrl("http://192.168.1.1:99999/x", host, port, path));
    CHECK(!upnp::splitHttpUrl("ftp://192.168.1.1/x", host, port, path));
    CHECK(!upnp::splitHttpUrl("http://192.168.1.1/ctl\r\nX-Injected: 1", host, port, path));   // would forge the request
    CHECK(!upnp::splitHttpUrl("http://192.168.1.1/a b", host, port, path));

    int code = 0;
    std::string desc;
    CHECK(upnp::parseSoapFault("<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode> 718 </errorCode>"
                               "<errorDescription>ConflictInMappingEntry</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>",
                               code, desc));
    CHECK_EQ(code, 718);
    CHECK_EQ(desc, std::string("ConflictInMappingEntry"));

    CHECK(upnp::cgnatSuspected("100.64.0.1"));
    CHECK(upnp::cgnatSuspected("100.127.255.254"));
    CHECK(!upnp::cgnatSuspected("100.128.0.1"));
    CHECK(upnp::cgnatSuspected("192.168.0.10"));
    CHECK(upnp::cgnatSuspected("10.20.30.40"));
    CHECK(upnp::cgnatSuspected("172.20.1.1"));
    CHECK(!upnp::cgnatSuspected("172.32.1.1"));
    CHECK(upnp::cgnatSuspected("0.0.0.0"));
    CHECK(upnp::cgnatSuspected(""));
    CHECK(!upnp::cgnatSuspected("203.0.113.7"));
    // Other ranges the Internet cannot reach: DS-Lite's B4 side, benchmarking, multicast, reserved.
    CHECK(upnp::cgnatSuspected("192.0.0.2"));
    CHECK(upnp::cgnatSuspected("198.18.0.1"));
    CHECK(upnp::cgnatSuspected("198.19.255.254"));
    CHECK(upnp::cgnatSuspected("224.0.0.1"));
    CHECK(upnp::cgnatSuspected("240.0.0.1"));
    CHECK(upnp::cgnatSuspected("255.255.255.255"));
    CHECK(!upnp::cgnatSuspected("192.0.1.1"));
    CHECK(!upnp::cgnatSuspected("198.20.0.1"));
    CHECK(!upnp::cgnatSuspected("223.255.255.254"));
}

TEST(upnp_renewal_schedule) {
    // Synthetic times; each renewal answers 3 s after it is sent.
    const int64_t lease = int64_t(upnp::kDefaultLeaseSec) * 1000;
    // Fails every renewal sent before the lease ends: each one comes a minute after the previous
    // failure. Returns how many were sent.
    auto failUntilLeaseEnds = [&](upnp::RenewalSchedule& s) {
        int n = 0;
        for (int64_t t = s.renewAt(); t < lease; t = s.renewAt(), ++n) {
            CHECK(!s.lapsed(t));
            s.started(t);
            CHECK(!s.finished(t + 3000, false));
            CHECK_EQ(s.renewAt(), t + 3000 + upnp::kRenewRetryMs);
        }
        return n;
    };
    {
        // Every renewal succeeds: one every 30 min, and the lease never ends.
        upnp::RenewalSchedule s;
        s.mapped(0, upnp::kDefaultLeaseSec);
        for (int64_t t = upnp::kRenewEveryMs; t <= 4 * lease; t += upnp::kRenewEveryMs) {
            CHECK_EQ(s.renewAt(), t);
            CHECK(!s.lapsed(t));
            s.started(t);
            CHECK(!s.finished(t + 3000, true));
        }
        CHECK(!s.lapsed(4 * lease + 3000) && !s.lost());
    }
    {
        // Failures until the lease ends: it is lost once; the retry due just after still goes,
        // then one every 30 min until a renewal maps the port again, its lease counted from then.
        upnp::RenewalSchedule s;
        s.mapped(0, upnp::kDefaultLeaseSec);
        CHECK_EQ(failUntilLeaseEnds(s), 29);
        CHECK(s.renewAt() > lease && s.renewAt() <= lease + upnp::kRenewRetryMs);
        CHECK(!s.lost());
        CHECK(s.lapsed(lease));
        CHECK(s.lost());
        CHECK(!s.lapsed(lease + 1000));
        int64_t t = s.renewAt();
        s.started(t);
        CHECK(!s.finished(t + 3000, false));
        CHECK_EQ(s.renewAt(), t + upnp::kRenewEveryMs);
        CHECK(s.lost());
        t = s.renewAt();
        s.started(t);
        CHECK(s.finished(t + 3000, true));
        CHECK(!s.lost());
        CHECK_EQ(s.renewAt(), t + upnp::kRenewEveryMs);
        CHECK(!s.lapsed(t + lease - 1));
        CHECK(s.lapsed(t + lease));
    }
    {
        // The retry due just after the lease has ended succeeds: the port is mapped again.
        upnp::RenewalSchedule s;
        s.mapped(0, upnp::kDefaultLeaseSec);
        failUntilLeaseEnds(s);
        CHECK(s.lapsed(lease));
        const int64_t t = s.renewAt();
        s.started(t);
        CHECK(s.finished(t + 3000, true));
        CHECK(!s.lost() && !s.lapsed(t + 3000));
    }
    {
        // One failure, then a retry that succeeds: the lease is never lost.
        upnp::RenewalSchedule s;
        s.mapped(0, upnp::kDefaultLeaseSec);
        s.started(upnp::kRenewEveryMs);
        CHECK(!s.finished(upnp::kRenewEveryMs + 3000, false));
        const int64_t t = s.renewAt();
        CHECK_EQ(t, upnp::kRenewEveryMs + 3000 + upnp::kRenewRetryMs);
        s.started(t);
        CHECK(!s.finished(t + 3000, true));
        CHECK_EQ(s.renewAt(), t + upnp::kRenewEveryMs);
        CHECK(!s.lapsed(lease) && !s.lost());
    }
    {
        // A permanent lease never ends.
        upnp::RenewalSchedule s;
        s.mapped(0, 0);
        CHECK(!s.lapsed(10 * lease) && !s.lost());
    }
}

TEST(upnp_fake_gateway_relative_control_url) {
    FakeGateway gw;
    gw.description = kDescRelative;
    CHECK(gw.start());
    upnp::Client client(gw.clientConfig());
    upnp::Gateway g;
    upnp::Error err;
    CHECK(client.discover(g, err));
    CHECK_EQ(err.text, std::string(""));
    CHECK_EQ(g.friendlyName, std::string("Fake Box & Co"));
    CHECK_EQ(g.serviceType, std::string("urn:schemas-upnp-org:service:WANIPConnection:1"));
    CHECK_EQ(g.controlUrl, "http://127.0.0.1:" + std::to_string(gw.httpPort) + "/ctl/IPConn");
    CHECK_EQ(g.localAddress, std::string("127.0.0.1"));
    CHECK_EQ(g.externalIp, std::string("203.0.113.7"));
    CHECK_EQ(g.server, std::string("Fake/1.0 UPnP/1.1 Test/1.0"));
    {
        std::lock_guard<std::mutex> lk(gw.m);
        // The three search targets were asked.
        bool igd1 = false, igd2 = false, wan = false;
        for (auto& st : gw.searchTargets) {
            igd1 = igd1 || st == "urn:schemas-upnp-org:device:InternetGatewayDevice:1";
            igd2 = igd2 || st == "urn:schemas-upnp-org:device:InternetGatewayDevice:2";
            wan = wan || st == "urn:schemas-upnp-org:service:WANIPConnection:1";
        }
        CHECK(igd1 && igd2 && wan);
    }

    upnp::Mapping mp;
    std::vector<uint16_t> claimed;
    CHECK(client.mapPort(g, 47100, [&](uint16_t p) { claimed.push_back(p); return true; }, mp, err));
    CHECK_EQ(mp.externalPort, 47100);
    CHECK_EQ(mp.internalPort, 47100);
    CHECK_EQ(mp.leaseSec, 3600u);
    CHECK(claimed.empty());
    CHECK(client.deletePortMapping(g, mp.externalPort, err));
    std::lock_guard<std::mutex> lk(gw.m);
    CHECK_EQ(gw.adds.size(), size_t(1));
    if (!gw.adds.empty()) {
        CHECK_EQ(gw.adds[0].extPort, 47100);
        CHECK_EQ(gw.adds[0].intPort, 47100);
        CHECK_EQ(gw.adds[0].lease, 3600);
        CHECK_EQ(gw.adds[0].client, std::string("127.0.0.1"));
        CHECK_EQ(gw.adds[0].desc, std::string("Scacelith direct match"));
        CHECK_EQ(gw.adds[0].proto, std::string("TCP"));
    }
    CHECK_EQ(gw.deletes.size(), size_t(1));
    CHECK(!gw.deletes.empty() && gw.deletes[0] == 47100);
    for (auto& p : gw.soapPaths) CHECK_EQ(p, std::string("/ctl/IPConn"));
}

TEST(upnp_fake_gateway_conflict_and_permanent_lease) {
    FakeGateway gw;
    gw.description = kDescAbsolute;
    gw.chunked = true;
    // 718 twice (the second candidate port is also busy locally), then 725, then success.
    gw.addResults = {718, 718, 725, 0};
    CHECK(gw.start());
    upnp::Client client(gw.clientConfig());
    upnp::Gateway g;
    upnp::Error err;
    CHECK(client.discover(g, err));
    CHECK_EQ(g.serviceType, std::string("urn:schemas-upnp-org:service:WANIPConnection:2"));
    CHECK_EQ(g.friendlyName, std::string("Fake IGD2"));
    CHECK_EQ(g.path, std::string("/upnp/control/WANIPConn2"));
    upnp::Mapping mp;
    std::vector<uint16_t> claimed;
    // Port 47102 is taken on this machine: it is skipped without asking the router.
    CHECK(client.mapPort(g, 47100, [&](uint16_t p) { claimed.push_back(p); return p != 47102; }, mp, err));
    CHECK_EQ(mp.externalPort, 47103);
    CHECK_EQ(mp.internalPort, 47103);
    CHECK_EQ(mp.leaseSec, 0u);
    CHECK_EQ(claimed.size(), size_t(3));   // 47101, 47102 (busy), 47103
    std::lock_guard<std::mutex> lk(gw.m);
    CHECK_EQ(gw.adds.size(), size_t(4));
    if (gw.adds.size() == 4) {
        CHECK_EQ(gw.adds[0].extPort, 47100);
        CHECK_EQ(gw.adds[1].extPort, 47101);
        CHECK_EQ(gw.adds[2].extPort, 47103);
        CHECK_EQ(gw.adds[2].lease, 3600);
        CHECK_EQ(gw.adds[3].extPort, 47103);
        CHECK_EQ(gw.adds[3].lease, 0);
    }
}

TEST(upnp_fake_gateway_answers_capped) {
    // A device answering with many distinct LOCATIONs: at most 16 descriptions are fetched (the
    // first answer, the router's, is among them).
    FakeGateway gw;
    gw.description = kDescRelative;
    gw.extraLocations = 40;
    CHECK(gw.start());
    upnp::Client client(gw.clientConfig());
    upnp::Gateway g;
    upnp::Error err;
    CHECK(client.discover(g, err));
    CHECK_EQ(g.friendlyName, std::string("Fake Box & Co"));
    std::lock_guard<std::mutex> lk(gw.m);
    CHECK_EQ(gw.httpGets, 16);
}

TEST(upnp_fake_gateway_cancel_during_add) {
    // Cancelled while the router handles an AddPortMapping: its answer is still read, so a mapping
    // it made is reported (the host deletes it when the match ends), and no other attempt follows.
    FakeGateway gw;
    gw.description = kDescRelative;
    std::atomic<bool> cancel{false};
    gw.cancelOnAdd = &cancel;
    gw.addDelayMs = 400;
    CHECK(gw.start());
    upnp::Config cfg = gw.clientConfig();
    cfg.cancel = &cancel;
    upnp::Client client(cfg);
    upnp::Gateway g;
    upnp::Error err;
    CHECK(client.discover(g, err));
    upnp::Mapping mp;
    CHECK(client.mapPort(g, 47100, nullptr, mp, err));
    CHECK(cancel.load());
    CHECK_EQ(mp.externalPort, 47100);
    CHECK_EQ(mp.leaseSec, 3600u);
    // Refused with 725: no permanent lease is asked after the cancel.
    cancel = false;
    {
        std::lock_guard<std::mutex> lk(gw.m);
        gw.addResults = {725};
    }
    CHECK(!client.mapPort(g, 47100, nullptr, mp, err));
    CHECK_EQ(err.text, std::string("cancelled"));
    // Refused with 718: the next port is not claimed here either.
    cancel = false;
    {
        std::lock_guard<std::mutex> lk(gw.m);
        gw.addResults = {718};
    }
    std::vector<uint16_t> claimed;
    CHECK(!client.mapPort(g, 47100, [&](uint16_t p) { claimed.push_back(p); return true; }, mp, err));
    CHECK_EQ(err.text, std::string("cancelled"));
    CHECK(claimed.empty());
    // Cancelled before anything is sent: nothing is asked.
    CHECK(!client.mapPort(g, 47100, nullptr, mp, err));
    CHECK_EQ(err.text, std::string("cancelled"));
    std::lock_guard<std::mutex> lk(gw.m);
    CHECK_EQ(gw.adds.size(), size_t(3));
}

TEST(upnp_fake_gateway_errors) {
    {
        // Every attempt conflicts: the policy stops after 10 tries.
        FakeGateway gw;
        gw.description = kDescRelative;
        gw.addResults = std::deque<int>(12, 718);
        CHECK(gw.start());
        upnp::Client client(gw.clientConfig());
        upnp::Gateway g;
        upnp::Error err;
        CHECK(client.discover(g, err));
        upnp::Mapping mp;
        CHECK(!client.mapPort(g, 50000, [](uint16_t) { return true; }, mp, err));
        CHECK_EQ(err.upnpCode, 718);
        std::lock_guard<std::mutex> lk(gw.m);
        CHECK_EQ(gw.adds.size(), size_t(10));
    }
    {
        // Another error code is reported as is.
        FakeGateway gw;
        gw.description = kDescRelative;
        gw.addResults = {606};
        CHECK(gw.start());
        upnp::Client client(gw.clientConfig());
        upnp::Gateway g;
        upnp::Error err;
        CHECK(client.discover(g, err));
        upnp::Mapping mp;
        CHECK(!client.mapPort(g, 50000, nullptr, mp, err));
        CHECK_EQ(err.upnpCode, 606);
        CHECK_EQ(err.httpStatus, 500);
        CHECK_EQ(err.text, std::string("606 Error"));
    }
    {
        // A control URL on another host is never contacted.
        FakeGateway gw;
        gw.description = kDescForeign;
        CHECK(gw.start());
        upnp::Client client(gw.clientConfig());
        upnp::Gateway g;
        upnp::Error err;
        CHECK(!client.discover(g, err));
        CHECK_EQ(err.text, std::string("no_wan_service"));
    }
    {
        // Nobody answers: "no_gateway" within the discovery time.
        sock::Handle silent = sock::openUdpV4();
        sock::Endpoint lo, ep;
        sock::Endpoint::parse("127.0.0.1", 0, lo);
        CHECK(sock::bindTo(silent, lo, false) && sock::localEndpoint(silent, ep));
        upnp::Config c;
        c.discoveryAddress = "127.0.0.1";
        c.discoveryPort = ep.port();
        c.discoveryMs = 300;
        upnp::Client client(c);
        upnp::Gateway g;
        upnp::Error err;
        int64_t t0 = sock::steadyMs();
        CHECK(!client.discover(g, err));
        CHECK_EQ(err.text, std::string("no_gateway"));
        CHECK(sock::steadyMs() - t0 < 1500);
        sock::closeSocket(silent);
    }
}

// ---- crypto primitives --------------------------------------------------------------------------

TEST(direct_crypto_vectors) {
    uint8_t out[64];
    CHECK(direct::sha256(reinterpret_cast<const uint8_t*>("abc"), 3, out));
    CHECK_EQ(toHex(out, 32), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    // RFC 4231 test case 2.
    const char* data = "what do ya want for nothing?";
    CHECK(direct::hmacSha256(reinterpret_cast<const uint8_t*>("Jefe"), 4, reinterpret_cast<const uint8_t*>(data), std::strlen(data), out));
    CHECK_EQ(toHex(out, 32), std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));

    // RFC 5869 test case 1.
    auto ikm = hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    auto salt = hex("000102030405060708090a0b0c");
    auto info = hex("f0f1f2f3f4f5f6f7f8f9");
    uint8_t okm[42];
    CHECK(direct::hkdfSha256(salt.data(), salt.size(), ikm.data(), ikm.size(), info.data(), info.size(), okm, sizeof okm));
    CHECK_EQ(toHex(okm, 42), std::string("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));
    // RFC 5869 test case 3 (empty salt and info).
    CHECK(direct::hkdfSha256(nullptr, 0, ikm.data(), ikm.size(), nullptr, 0, okm, sizeof okm));
    CHECK_EQ(toHex(okm, 42), std::string("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"));

    // AES-256-GCM, test cases 14 and 16 of the GCM specification (McGrew & Viega).
    {
        direct::AesGcm g;
        uint8_t key[32] = {}, nonce[12] = {}, pt[16] = {}, ct[32];
        CHECK(g.setKey(key));
        CHECK(g.seal(nonce, nullptr, 0, pt, 16, ct));
        CHECK_EQ(toHex(ct, 32), std::string("cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919"));
    }
    {
        auto key = hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
        auto iv = hex("cafebabefacedbaddecaf888");
        auto pt = hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
        auto aad = hex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
        direct::AesGcm g;
        CHECK(g.setKey(key.data()));
        std::vector<uint8_t> ct(pt.size() + 16);
        CHECK(g.seal(iv.data(), aad.data(), aad.size(), pt.data(), pt.size(), ct.data()));
        CHECK_EQ(toHex(ct.data(), ct.size()),
                 std::string("522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662"
                             "76fc6ece0f4e1768cddf8853bb2d551b"));
        std::vector<uint8_t> back(pt.size());
        CHECK(g.open(iv.data(), aad.data(), aad.size(), ct.data(), ct.size(), back.data()));
        CHECK(back == pt);
        ct[5] ^= 1;
        CHECK(!g.open(iv.data(), aad.data(), aad.size(), ct.data(), ct.size(), back.data()));
        ct[5] ^= 1;
        aad[0] ^= 0x80;
        CHECK(!g.open(iv.data(), aad.data(), aad.size(), ct.data(), ct.size(), back.data()));
    }

    // ECDH P-256, RFC 5903 section 8.1.
    {
        auto i = hex("C88F01F510D9AC3F70A292DAA2316DE544E9AAB8AFE84049C62A9C57862D1433");
        auto gi = hex("04DAD0B65394221CF9B051E1FECA5787D098DFE637FC90B9EF945D0C37725811805271A0461CDB8252D61F1C456FA3E59AB1F45B33ACCF5F58389E0577B8990BB3");
        auto r = hex("C6EF9C5D78AE012A011164ACB397CE2088685D8F06BF9BE0B283AB46476BEE53");
        auto gr = hex("04D12DFB5289C8D4F81208B70270398C342296970A0BCCB74C736FC7554494BF6356FBF3CA366CC23E8157854C13C58D6AAC23F046ADA30F8353E74F33039872AB");
        direct::EcdhP256 a, b;
        CHECK(a.setKeyPair(i.data(), gi.data()));
        CHECK(b.setKeyPair(r.data(), gr.data()));
        CHECK(std::memcmp(a.publicKey(), gi.data(), 65) == 0);
        uint8_t z1[32], z2[32];
        CHECK(a.agree(gr.data(), z1));
        CHECK(b.agree(gi.data(), z2));
        CHECK_EQ(toHex(z1, 32), std::string("d6840f6b42f6edafd13116e0e12565202fef8e9ece7dce03812464d04b9442de"));
        CHECK_EQ(toHex(z2, 32), toHex(z1, 32));
        // A point that is not on the curve is refused.
        auto bad = gr;
        bad[64] ^= 1;
        CHECK(!a.agree(bad.data(), z1));
    }
}

TEST(direct_join_codes) {
    for (int k = 0; k < 50; ++k) {
        std::string c = direct::newJoinCode();
        CHECK_EQ(c.size(), size_t(12));
        for (char ch : c) CHECK(std::strchr(direct::kCodeAlphabet, ch) != nullptr);
        std::string n;
        CHECK(direct::normalizeJoinCode(direct::formatJoinCode(c), n) && n == c);
    }
    CHECK(direct::newJoinCode() != direct::newJoinCode());
    std::string n;
    CHECK(direct::normalizeJoinCode("k7q2-m9xh 3ptr", n));
    CHECK_EQ(n, std::string("K7Q2M9XH3PTR"));
    CHECK_EQ(direct::formatJoinCode(n), std::string("K7Q2-M9XH-3PTR"));
    CHECK(!direct::normalizeJoinCode("K7Q2-M9XH-3PT0", n));   // 0 is not in the alphabet
    CHECK(!direct::normalizeJoinCode("K7Q2-M9XH-3PTO", n));   // nor O
    CHECK(!direct::normalizeJoinCode("K7Q2-M9XH-3PT1", n));   // nor 1, I, L
    CHECK(!direct::normalizeJoinCode("K7Q2-M9XH-3PT", n));
    CHECK(!direct::normalizeJoinCode("K7Q2-M9XH-3PTRR", n));
    CHECK(!direct::normalizeJoinCode("", n));
    CHECK(!direct::normalizeJoinCode("K7Q2_M9XH_3PTR", n));
}

// The guest's address field, as the host's Copy writes it or as a player types it.
TEST(direct_address_field) {
    struct Case {
        const char* field;
        bool withCode;
        const char *host, *port, *code;
    };
    const Case cases[] = {
        {"[::1]", true, "::1", "", ""},
        {"[::1]:47100 K7Q2-M9XH-3PTR", true, "::1", "47100", "K7Q2-M9XH-3PTR"},
        {"[2001:db8::1]:47100", true, "2001:db8::1", "47100", ""},
        {"1.2.3.4:5000", true, "1.2.3.4", "5000", ""},
        {"203.0.113.47:47100 k7q2-m9xh-3ptr", true, "203.0.113.47", "47100", "K7Q2-M9XH-3PTR"},
        {"  203.0.113.47:47100   K7Q2-M9XH-3PTR  ", true, "203.0.113.47", "47100", "K7Q2-M9XH-3PTR"},
        {"203.0.113.47:47100\tK7Q2.M9XH", true, "203.0.113.47", "47100", "K7Q2M9XH"},
        {"203.0.113.47", true, "203.0.113.47", "", ""},
        {"2001:db8::1", true, "2001:db8::1", "", ""},   // a bare IPv6 address: several ':'
        {"fe80::1%eth0", true, "fe80::1%eth0", "", ""},
        {"::1", true, "::1", "", ""},
        {"example.org", true, "example.org", "", ""},
        {"example.org:47100", true, "example.org", "47100", ""},
        {"1.2.3.4 K7Q2-M9XH-3PTR", false, "1.2.3.4 K7Q2-M9XH-3PTR", "", ""},   // the code field is not empty
        {"1.2.3.4:5000 K7Q2-M9XH-3PTR", false, "1.2.3.4:5000 K7Q2-M9XH-3PTR", "", ""},
        {"[::1]x", true, "[::1]x", "", ""},
        {"[::1]:", true, "[::1]:", "", ""},
        {"[::1]:4x", true, "[::1]:4x", "", ""},
        {"host:", true, "host:", "", ""},
        {":80", true, ":80", "", ""},
        {"host:123456", true, "host:123456", "", ""},  // six digits
        {"host:12a", true, "host:12a", "", ""},
        {"host:abc", true, "host:abc", "", ""},
        {"", true, "", "", ""},
    };
    for (const Case& c : cases) {
        const DirectAddress a = splitDirectAddress(c.field, c.withCode);
        if (a.host != c.host || a.port != c.port || a.code != c.code)
            std::fprintf(stderr, "  field \"%s\"%s: host \"%s\" port \"%s\" code \"%s\"\n", c.field, c.withCode ? " +code" : "",
                         a.host.c_str(), a.port.c_str(), a.code.c_str());
        CHECK_EQ(a.host, std::string(c.host));
        CHECK_EQ(a.port, std::string(c.port));
        CHECK_EQ(a.code, std::string(c.code));
    }
}

// ---- secure channel -----------------------------------------------------------------------------

namespace {

using Chan = direct::SecureChannel;

// Moves every pending byte between the two channels until nothing moves.
void pump(Chan& a, Chan& b) {
    for (int i = 0; i < 16; ++i) {
        bool moved = false;
        if (!a.outbox().empty()) {
            std::vector<uint8_t> x;
            x.swap(a.outbox());
            b.receive(x.data(), x.size());
            moved = true;
        }
        if (!b.outbox().empty()) {
            std::vector<uint8_t> x;
            x.swap(b.outbox());
            a.receive(x.data(), x.size());
            moved = true;
        }
        if (!moved) break;
    }
}

std::vector<uint8_t> bytes(const char* s) { return std::vector<uint8_t>(s, s + std::strlen(s)); }

}  // namespace

TEST(direct_channel_handshake) {
    Chan guest(Chan::Role::Guest, "K7Q2M9XH3PTR"), host(Chan::Role::Host, "K7Q2M9XH3PTR");
    CHECK(host.start());
    CHECK(host.outbox().empty());   // the host waits for the guest
    CHECK(guest.start());
    CHECK_EQ(guest.outbox().size(), Chan::kHelloLen);
    // Host hello only (nothing code-dependent) until the guest has proved the code.
    std::vector<uint8_t> gh;
    gh.swap(guest.outbox());
    CHECK(host.receive(gh.data(), gh.size()));
    CHECK_EQ(host.outbox().size(), Chan::kHelloLen);
    CHECK(!host.established());
    pump(guest, host);
    CHECK(guest.established());
    CHECK(host.established());
    auto m1 = bytes("hello host");
    auto m2 = bytes("hello guest");
    CHECK(guest.send(m1.data(), m1.size()));
    CHECK(guest.send(m2.data(), m2.size()));
    CHECK(host.send(m2.data(), m2.size()));
    pump(guest, host);
    std::vector<uint8_t> got;
    CHECK(host.popMessage(got) && got == m1);
    CHECK(host.popMessage(got) && got == m2);
    CHECK(!host.popMessage(got));
    CHECK(guest.popMessage(got) && got == m2);
    // The largest message passes, one byte more is refused.
    std::vector<uint8_t> big(Chan::kMaxPlaintext, 0x5A);
    CHECK(host.send(big.data(), big.size()));
    pump(guest, host);
    CHECK(guest.popMessage(got) && got == big);
    big.push_back(1);
    CHECK(!host.send(big.data(), big.size()));
    CHECK(host.failed());
    CHECK(host.failure() == Chan::Failure::TooLarge);
}

TEST(direct_channel_wrong_code) {
    Chan guest(Chan::Role::Guest, "K7Q2M9XH3PTR"), host(Chan::Role::Host, "K7Q2M9XH3PTS");
    CHECK(host.start() && guest.start());
    pump(guest, host);
    CHECK(host.failed());
    CHECK(host.failure() == Chan::Failure::WrongCode);
    CHECK(host.outbox().empty());   // no host confirmation for a wrong code
    CHECK(!guest.established());
    CHECK(guest.awaitingHostConfirm());
    // A malformed code never starts.
    Chan bad(Chan::Role::Guest, "SHORT");
    CHECK(!bad.start());
}

TEST(direct_channel_bad_hello) {
    {
        Chan host(Chan::Role::Host, "K7Q2M9XH3PTR");
        CHECK(host.start());
        auto http = bytes("GET / HTTP/1.1\r\n\r\n");
        CHECK(!host.receive(http.data(), http.size()));
        CHECK(host.failure() == Chan::Failure::BadHello);
        CHECK(host.outbox().empty());
    }
    {
        Chan guest(Chan::Role::Guest, "K7Q2M9XH3PTR"), host(Chan::Role::Host, "K7Q2M9XH3PTR");
        CHECK(host.start() && guest.start());
        std::vector<uint8_t> gh;
        gh.swap(guest.outbox());
        gh[4] = uint8_t(Chan::kVersion + 1);   // a future version
        CHECK(!host.receive(gh.data(), gh.size()));
        CHECK(host.failure() == Chan::Failure::BadVersion);
    }
    {
        // Version 1 carried the protocol before v1: refused the same way.
        Chan guest(Chan::Role::Guest, "K7Q2M9XH3PTR"), host(Chan::Role::Host, "K7Q2M9XH3PTR");
        CHECK(host.start() && guest.start());
        std::vector<uint8_t> gh;
        gh.swap(guest.outbox());
        CHECK_EQ(int(gh[4]), 2);
        gh[4] = 1;
        CHECK(!host.receive(gh.data(), gh.size()));
        CHECK(host.failure() == Chan::Failure::BadVersion);
    }
    {
        // A hello reflected back to the guest is refused.
        Chan guest(Chan::Role::Guest, "K7Q2M9XH3PTR");
        CHECK(guest.start());
        std::vector<uint8_t> gh = guest.outbox();
        CHECK(!guest.receive(gh.data(), gh.size()));
        CHECK(guest.failure() == Chan::Failure::BadHello);
    }
}

namespace {

// An established pair.
struct Pair {
    Chan guest{Chan::Role::Guest, "ABCDEFGHJKMN"}, host{Chan::Role::Host, "ABCDEFGHJKMN"};
    Pair() {
        host.start();
        guest.start();
        pump(guest, host);
    }
    std::vector<uint8_t> frame(const char* text) {
        auto m = bytes(text);
        guest.send(m.data(), m.size());
        std::vector<uint8_t> f;
        f.swap(guest.outbox());
        return f;
    }
};

}  // namespace

TEST(direct_channel_frame_failures) {
    {
        Pair p;
        CHECK(p.guest.established() && p.host.established());
        auto f = p.frame("move e2e4");
        f[5] ^= 0x01;   // tampered ciphertext
        CHECK(!p.host.receive(f.data(), f.size()));
        CHECK(p.host.failure() == Chan::Failure::AuthFailed);
    }
    {
        Pair p;
        auto f = p.frame("resign");
        CHECK(p.host.receive(f.data(), f.size()));
        std::vector<uint8_t> got;
        CHECK(p.host.popMessage(got));
        CHECK(!p.host.receive(f.data(), f.size()));   // replayed
        CHECK(p.host.failure() == Chan::Failure::AuthFailed);
        CHECK(!p.host.popMessage(got));
    }
    {
        Pair p;
        auto f1 = p.frame("first");
        auto f2 = p.frame("second");
        CHECK(!p.host.receive(f2.data(), f2.size()));   // reordered
        CHECK(p.host.failure() == Chan::Failure::AuthFailed);
    }
    {
        Pair p;
        auto f = p.frame("truncated frame");
        // Incomplete: waits for the rest, delivers nothing.
        CHECK(p.host.receive(f.data(), f.size() - 1));
        std::vector<uint8_t> got;
        CHECK(!p.host.popMessage(got));
        CHECK(p.host.hasPartialInput());
        // Shortened with a consistent length: the tag no longer verifies.
        Pair q;
        auto g = q.frame("truncated frame");
        size_t len = size_t(g[0]) | size_t(g[1]) << 8;
        g.pop_back();
        --len;
        g[0] = uint8_t(len & 0xFF);
        g[1] = uint8_t(len >> 8);
        CHECK(!q.host.receive(g.data(), g.size()));
        CHECK(q.host.failure() == Chan::Failure::AuthFailed);
    }
    {
        Pair p;
        // Oversize length announced: refused before reading the body.
        size_t len = Chan::kMaxPlaintext + 17;
        uint8_t hdr[2] = {uint8_t(len & 0xFF), uint8_t(len >> 8)};
        CHECK(!p.host.receive(hdr, 2));
        CHECK(p.host.failure() == Chan::Failure::BadFrame);
        // Too short to hold a tag.
        Pair q;
        uint8_t tiny[2] = {16, 0};
        CHECK(!q.host.receive(tiny, 2));
        CHECK(q.host.failure() == Chan::Failure::BadFrame);
    }
    {
        // Frames of one direction cannot be reflected to the other direction.
        Pair p;
        auto f = p.frame("ping");
        CHECK(!p.guest.receive(f.data(), f.size()));
        CHECK(p.guest.failure() == Chan::Failure::AuthFailed);
    }
}

// ---- the host authority (deterministic time) ----------------------------------------------------

namespace {

namespace P = net::proto;
using direct::Authority;
using direct::GuestSide;
using direct::HostSide;
using direct::Side;

// Drives an Authority with synthetic time and keeps a mirror chess::Game of the accepted moves.
struct Room {
    Authority a;
    Authority::Output out;
    double now = 1.7e12;
    std::vector<std::vector<uint8_t>> msgs[2];   // everything received, by side
    size_t mark[2] = {0, 0};                     // start of the messages since the last action
    uint32_t seq[2] = {0, 0};
    chess::Game mirror;

    Room(const direct::AuthorityConfig& cfg, int hostColorPref) : a(cfg, "Alice", "Bob", hostColorPref) {}

    void collect() {
        mark[0] = msgs[0].size();
        mark[1] = msgs[1].size();
        for (auto& m : out.toHost) msgs[HostSide].push_back(m);
        for (auto& m : out.toGuest) msgs[GuestSide].push_back(m);
        out.clear();
        // Moves confirmed by the authority are mirrored.
        for (size_t i = mark[HostSide]; i < msgs[HostSide].size(); ++i) {
            P::MoveMade mm;
            if (P::decode(msgs[HostSide][i].data(), msgs[HostSide][i].size(), mm) && mm.ply == mirror.moves().size())
                mirror.play(mirror.position().findLegal(chess::Square(moveFrom(mm.move)), chess::Square(moveTo(mm.move)),
                                                        chess::PieceType(movePromo(mm.move))));
        }
    }
    void start() { a.startGame(now, out); collect(); }
    void tick() { a.tick(now, out); collect(); }
    template <class T> void send(Side s, T m) {
        m.seq = ++seq[s];
        std::vector<uint8_t> buf;
        P::encode(m, buf);
        a.onMessage(s, buf.data(), buf.size(), now, out);
        collect();
    }
    void move(Side s, const char* uci, uint32_t thinkMs = 0, bool offer = false, int ply = -1, uint32_t hash = 0) {
        chess::Move mv = mirror.position().parseUCI(uci);
        P::Move m;
        m.game = a.gameId();
        m.ply = uint16_t(ply >= 0 ? ply : int(mirror.moves().size()));
        m.move = mv.valid() ? packMove(mv.from, mv.to, mv.promotion)
                            : packMove(chess::parseSquare(std::string(uci, 2)), chess::parseSquare(std::string(uci + 2, 2)), 0);
        m.posHash = hash ? hash : direct::fenDigest(mirror.position().fen());
        m.thinkMs = thinkMs;
        m.drawOffer = offer;
        send(s, m);
    }
    template <class T> std::vector<T> recent(Side s) const {   // messages of type T since the last action
        std::vector<T> v;
        for (size_t i = mark[s]; i < msgs[s].size(); ++i) {
            T t;
            if (P::decode(msgs[s][i].data(), msgs[s][i].size(), t)) v.push_back(t);
        }
        return v;
    }
    template <class T> bool has(Side s) const { return !recent<T>(s).empty(); }
};

direct::AuthorityConfig tc(int baseSec, int incSec) {
    direct::AuthorityConfig c;
    c.baseMs = int64_t(baseSec) * 1000;
    c.incMs = int64_t(incSec) * 1000;
    return c;
}

}  // namespace

TEST(direct_authority_snapshot_and_names) {
    Room r(tc(300, 2), 2);   // host plays Black
    r.start();
    auto hs = r.recent<P::GameSnapshot>(HostSide);
    auto gs = r.recent<P::GameSnapshot>(GuestSide);
    CHECK(hs.size() == 1 && gs.size() == 1);
    if (hs.size() == 1 && gs.size() == 1) {
        CHECK(hs[0].you == P::Color::Black);
        CHECK(gs[0].you == P::Color::White);
        CHECK_EQ(hs[0].white.name, std::string("Bob"));
        CHECK_EQ(hs[0].black.name, std::string("Alice"));
        CHECK_EQ(hs[0].white.userId, 2u);
        CHECK_EQ(hs[0].black.userId, 1u);
        CHECK_EQ(hs[0].white.rating, 0);
        CHECK_EQ(hs[0].category, std::string("custom"));
        CHECK(!hs[0].rated);
        CHECK_EQ(hs[0].baseMs, 300000u);
        CHECK_EQ(hs[0].incMs, 2000u);
        CHECK(hs[0].running == P::Color::None);
        CHECK_EQ(hs[0].firstMoveMs, 60000u);
        CHECK(hs[0].whiteConnected && hs[0].blackConnected);
    }
    CHECK_EQ(direct::sanitizeName("  \x01Zo\xC3\xA9  ", "X"), std::string("Zo\xC3\xA9"));
    CHECK_EQ(direct::sanitizeName("", "Guest"), std::string("Guest"));
    CHECK_EQ(direct::sanitizeName("\xFF\xFE", "Guest"), std::string("Guest"));
    // 23 ASCII bytes + a 2-byte character would make 25: the character is dropped whole.
    CHECK_EQ(direct::sanitizeName("abcdefghijklmnopqrstuvw\xC3\xA9", "X"), std::string("abcdefghijklmnopqrstuvw"));
    // Sequences the protocol's decoder refuses are dropped too: an overlong form, a surrogate,
    // code points above U+10FFFF. Such a name used to make hosting and joining fail.
    const char* refused[] = {"Ann\xC0\x80\x65", "Bob\xED\xA0\x80", "Cy\xF5\x80\x80\x80", "Di\xF4\x90\x80\x80", "Ed\xE0\x80\xAF"};
    const char* kept[] = {"Anne", "Bob", "Cy", "Di", "Ed"};
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(direct::sanitizeName(refused[i], "X"), std::string(kept[i]));
        // The host's snapshot (its name and the guest's), Welcome and the guest's Hello decode.
        Authority a(tc(300, 0), refused[i], refused[i], 1);
        Authority::Output o;
        a.startGame(1.7e12, o);
        P::GameSnapshot s;
        CHECK(o.toHost.size() == 1 && P::decode(o.toHost[0].data(), o.toHost[0].size(), s));
        P::Welcome w;
        w.username = a.guestName();
        w.serverName = direct::sanitizeName(refused[i], "Host");
        P::Hello h;
        h.proto = P::kProtocolVersion;
        h.token = "direct:" + direct::sanitizeName(refused[i], "Guest");
        h.token.resize(16, ' ');
        std::vector<uint8_t> buf;
        P::encode(w, buf);
        CHECK(P::decode(buf.data(), buf.size(), w));
        buf.clear();
        P::encode(h, buf);
        CHECK(P::decode(buf.data(), buf.size(), h));
    }
    // Valid names stay as they are (C1 controls included: the decoder accepts them).
    for (const char* name : {"\xC3\x89lodie", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "Dee\xC2\x85x", "\xF0\x9F\x98\x80 Max", "\xF4\x8F\xBF\xBF"})
        CHECK_EQ(direct::sanitizeName(name, "X"), std::string(name));
    // The router's name (any LAN device may answer the discovery): cleaned, at most 64 bytes.
    std::string routerName = "  Fake\r\nBox\x01 ";
    for (int i = 0; i < 20000; ++i) routerName += "\xE6\x97\xA5\x7F\x1B";
    std::string shown = direct::sanitizeName(routerName, "", 64);
    CHECK_EQ(shown.substr(0, 7), std::string("FakeBox"));
    CHECK(shown.size() <= 64 && shown.size() >= 62);
    CHECK_EQ(direct::sanitizeName(shown, "", 64), shown);   // valid UTF-8 without controls
    CHECK_EQ(direct::sanitizeName("Livebox 6", "", 64), std::string("Livebox 6"));
    CHECK_EQ(direct::sanitizeName(" \t", "", 64), std::string(""));
    // The digest covers the first four FEN fields only.
    CHECK_EQ(direct::fenDigest("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
             direct::fenDigest("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 5 9"));
    CHECK(direct::fenDigest("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") !=
          direct::fenDigest("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1"));
}

TEST(direct_authority_move_validation) {
    Room r(tc(300, 0), 1);   // host White
    r.start();
    // Black (guest) tries to move first: NotYourTurn.
    r.move(GuestSide, "e7e5");
    CHECK(r.has<P::MoveRejected>(GuestSide) && r.recent<P::MoveRejected>(GuestSide)[0].code == P::ErrorCode::NotYourTurn);
    CHECK(!r.has<P::MoveRejected>(HostSide));   // never shown to the opponent
    r.move(HostSide, "e2e5");
    CHECK(r.has<P::MoveRejected>(HostSide) && r.recent<P::MoveRejected>(HostSide)[0].code == P::ErrorCode::IllegalMove);
    // A promotion piece on a move that is not a promotion.
    {
        P::Move m;
        m.game = r.a.gameId();
        m.move = packMove(12, 28, 5);
        m.posHash = direct::fenDigest(r.mirror.position().fen());
        r.send(HostSide, m);
        CHECK(r.has<P::MoveRejected>(HostSide) && r.recent<P::MoveRejected>(HostSide)[0].code == P::ErrorCode::IllegalMove);
    }
    // Wrong position digest: Desync and a snapshot.
    r.move(HostSide, "e2e4", 0, false, -1, 12345);
    CHECK(r.has<P::MoveRejected>(HostSide) && r.recent<P::MoveRejected>(HostSide)[0].code == P::ErrorCode::Desync);
    CHECK(r.has<P::GameSnapshot>(HostSide));
    r.move(HostSide, "e2e4");
    auto mm = r.recent<P::MoveMade>(GuestSide);
    CHECK(mm.size() == 1 && mm[0].ply == 0 && (mm[0].flags & P::MoveFlag::DoublePush));
    CHECK_EQ(r.mirror.moves().size(), size_t(1));
    // The same move again: the original confirmation, nothing new for the opponent.
    r.move(HostSide, "e2e4", 0, false, 0, direct::fenDigest("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));
    CHECK(r.has<P::MoveMade>(HostSide) && !r.has<P::MoveMade>(GuestSide));
    // Another move for an old ply: StalePly.
    r.move(HostSide, "d2d4", 0, false, 0);
    CHECK(r.has<P::MoveRejected>(HostSide) && r.recent<P::MoveRejected>(HostSide)[0].code == P::ErrorCode::StalePly);
    // Another game id.
    {
        P::Resign m;
        m.game = 77;
        r.send(GuestSide, m);
        auto e = r.recent<P::Error>(GuestSide);
        CHECK(e.size() == 1 && e[0].code == P::ErrorCode::NotInGame && e[0].ref == r.seq[GuestSide]);
    }
    // Garbage.
    uint8_t junk[3] = {0x20, 1, 2};
    r.a.onMessage(GuestSide, junk, sizeof junk, r.now, r.out);
    r.collect();
    CHECK(r.has<P::Error>(GuestSide) && r.recent<P::Error>(GuestSide)[0].code == P::ErrorCode::Malformed);
}

TEST(direct_authority_first_move_timeout) {
    {
        Room r(tc(60, 0), 1);
        r.start();
        CHECK_EQ(r.a.nextDeadline(), r.now + 60000);
        r.now += 59999;
        r.tick();
        CHECK(!r.has<P::GameEnd>(HostSide));
        r.now += 1;
        r.tick();
        auto e = r.recent<P::GameEnd>(GuestSide);
        CHECK(e.size() == 1 && e[0].status == P::GameStatus::Aborted && e[0].reason == P::EndReason::NoShow);
    }
    {
        // Black's 60 s start with White's first move; no clock runs meanwhile.
        Room r(tc(60, 0), 1);
        r.start();
        r.now += 30000;
        r.move(HostSide, "e2e4");
        auto mm = r.recent<P::MoveMade>(GuestSide);
        CHECK(mm.size() == 1 && mm[0].firstMoveMs == 60000 && mm[0].spentMs == 0 && mm[0].whiteMs == 60000);
        CHECK_EQ(r.a.nextDeadline(), r.now + 60000 + 30);   // the guest's margin, no round trip known yet
        r.now += 59000;
        r.move(GuestSide, "e7e5");
        mm = r.recent<P::MoveMade>(HostSide);
        CHECK(mm.size() == 1 && mm[0].firstMoveMs == 0 && mm[0].blackMs == 60000);
        CHECK_EQ(r.a.nextDeadline(), r.now + 60000);   // White's clock now runs (host: no allowance)
    }
    {
        // The guest's first move has the margin of its flag (DESIGN 6.1): with a 100 ms round
        // trip, min(100 / 2 + 30, 500, quota) = 80 ms after its 60 s, which the countdown it is
        // sent does not show. A move made at the countdown's last instant still counts.
        Room r(tc(60, 0), 2);   // host Black, guest White
        r.a.onRtt(GuestSide, 100);
        r.start();
        const double start = r.now;
        CHECK_EQ(r.a.nextDeadline(), start + 60080);
        r.now = start + 60050;
        r.tick();
        CHECK(!r.has<P::GameEnd>(GuestSide));
        P::Resync rs;
        rs.game = r.a.gameId();
        r.send(GuestSide, rs);
        auto s = r.recent<P::GameSnapshot>(GuestSide);
        CHECK(s.size() == 1 && s[0].firstMoveMs == 0);
        r.move(GuestSide, "e2e4");
        auto mm = r.recent<P::MoveMade>(HostSide);
        CHECK(mm.size() == 1 && mm[0].ply == 0 && mm[0].firstMoveMs == 60000);
        // The host's first move has none.
        CHECK_EQ(r.a.nextDeadline(), r.now + 60000);
        r.now += 60000;
        r.tick();
        auto e = r.recent<P::GameEnd>(GuestSide);
        CHECK(e.size() == 1 && e[0].status == P::GameStatus::Aborted && e[0].reason == P::EndReason::NoShow);
    }
    {
        // Past the margin the game is aborted.
        Room r(tc(60, 0), 2);
        r.a.onRtt(GuestSide, 100);
        r.start();
        r.now += 60079;
        r.tick();
        CHECK(!r.has<P::GameEnd>(GuestSide));
        r.now += 1;
        r.tick();
        auto e = r.recent<P::GameEnd>(HostSide);
        CHECK(e.size() == 1 && e[0].status == P::GameStatus::Aborted && e[0].reason == P::EndReason::NoShow);
    }
}

TEST(direct_authority_clock_and_lag_compensation) {
    Room r(tc(60, 2), 1);   // host White, guest Black
    r.start();
    r.a.onRtt(GuestSide, 100);   // compensation bound: min(100/2 + 30, 500) = 80 ms
    r.move(HostSide, "e2e4");
    r.now += 5000;
    r.move(GuestSide, "e7e5");
    r.now += 5000;
    r.move(HostSide, "d2d4", 4000);   // the host gets no compensation
    auto mm = r.recent<P::MoveMade>(GuestSide);
    CHECK(mm.size() == 1 && mm[0].spentMs == 5000 && mm[0].whiteMs == 57000 && mm[0].blackMs == 60000);
    r.now += 3000;
    r.move(GuestSide, "d7d5", 2500);  // lag 500 ms, compensated 80
    mm = r.recent<P::MoveMade>(HostSide);
    CHECK(mm.size() == 1 && mm[0].spentMs == 2920 && mm[0].blackMs == 59080);
    r.now += 1000;
    r.move(HostSide, "g1f3");
    r.now += 1000;
    r.move(GuestSide, "g8f6", 5000);  // thinkMs above the elapsed time: no lag, no compensation
    mm = r.recent<P::MoveMade>(HostSide);
    CHECK(mm.size() == 1 && mm[0].spentMs == 1000);
    // The snapshot shows the running clock at its time.
    r.now += 500;
    P::Resync rs;
    rs.game = r.a.gameId();
    r.send(HostSide, rs);
    auto s = r.recent<P::GameSnapshot>(HostSide);
    CHECK(s.size() == 1 && s[0].running == P::Color::White && s[0].whiteMs == 57000 + 2000 - 1000 - 500 && s[0].moves.size() == 6);
}

TEST(direct_authority_flag) {
    {
        // The host's flag falls exactly at its remaining time.
        Room r(tc(1, 0), 1);
        r.start();
        r.move(HostSide, "e2e4");
        r.move(GuestSide, "e7e5");
        CHECK_EQ(r.a.nextDeadline(), r.now + 1000);
        r.now += 999;
        r.tick();
        CHECK(!r.has<P::GameEnd>(HostSide));
        r.now += 1;
        r.tick();
        auto e = r.recent<P::GameEnd>(GuestSide);
        CHECK(e.size() == 1 && e[0].status == P::GameStatus::BlackWins && e[0].reason == P::EndReason::Timeout && e[0].whiteMs == 0);
    }
    {
        // The guest (White) may still be saved by its compensation; not beyond.
        Room r(tc(1, 0), 2);
        r.start();
        r.move(GuestSide, "e2e4");
        r.move(HostSide, "e7e5");
        CHECK_EQ(r.a.nextDeadline(), r.now + 1030);   // unknown RTT: 30 ms of allowance
        double t0 = r.now;
        r.now = t0 + 1010;
        r.move(GuestSide, "d2d4", 950);   // lag 60, compensated 30: charged 980
        auto mm = r.recent<P::MoveMade>(HostSide);
        CHECK(mm.size() == 1 && mm[0].whiteMs == 20);
        r.move(HostSide, "d7d5");
        r.now += 1040;
        r.move(GuestSide, "c2c4", 1000);  // charged 1010 > 20: the flag fell
        auto rej = r.recent<P::MoveRejected>(GuestSide);
        CHECK(rej.size() == 1 && rej[0].code == P::ErrorCode::FlagFell);
        auto e = r.recent<P::GameEnd>(HostSide);
        CHECK(e.size() == 1 && e[0].status == P::GameStatus::BlackWins && e[0].reason == P::EndReason::Timeout);
        CHECK(!r.has<P::MoveMade>(HostSide));
    }
}

TEST(direct_authority_draws) {
    Room r(tc(300, 0), 1);
    r.start();
    // Offer alone, declined by a move.
    P::DrawOffer off;
    off.game = r.a.gameId();
    r.send(HostSide, off);
    auto ev = r.recent<P::GameEvent>(GuestSide);
    CHECK(ev.size() == 1 && ev[0].kind == P::GameEventKind::DrawOffered && ev[0].color == P::Color::White);
    r.move(HostSide, "g1f3");   // own offer stands
    CHECK(!r.has<P::GameEvent>(GuestSide));
    r.move(GuestSide, "g8f6");  // the opponent moves: declined
    ev = r.recent<P::GameEvent>(HostSide);
    CHECK(ev.size() == 1 && ev[0].kind == P::GameEventKind::DrawDeclined && ev[0].color == P::Color::Black);
    // Not again within 10 plies of the decline.
    r.send(HostSide, off);
    auto er = r.recent<P::Error>(HostSide);
    CHECK(er.size() == 1 && er[0].code == P::ErrorCode::DrawOfferLimit);
    // No offer to answer.
    P::DrawAnswer ans;
    ans.game = r.a.gameId();
    ans.accept = true;
    r.send(GuestSide, ans);
    er = r.recent<P::Error>(GuestSide);
    CHECK(er.size() == 1 && er[0].code == P::ErrorCode::NoPendingOffer);
    // Nothing to claim yet.
    P::DrawClaim claim;
    claim.game = r.a.gameId();
    r.send(HostSide, claim);
    er = r.recent<P::Error>(HostSide);
    CHECK(er.size() == 1 && er[0].code == P::ErrorCode::NothingToClaim);
    // A move with an offer (Black), then declined explicitly.
    r.move(HostSide, "f3g1");
    r.move(GuestSide, "f6g8", 0, true);
    auto mm = r.recent<P::MoveMade>(HostSide);
    CHECK(mm.size() == 1 && mm[0].drawOffer);
    ans.accept = false;
    r.send(HostSide, ans);
    ev = r.recent<P::GameEvent>(GuestSide);
    CHECK(ev.size() == 1 && ev[0].kind == P::GameEventKind::DrawDeclined && ev[0].color == P::Color::White);
    // Threefold: the start position appears for the third time after 8 plies.
    r.move(HostSide, "g1f3");
    r.move(GuestSide, "g8f6");
    r.move(HostSide, "f3g1");
    r.move(GuestSide, "f6g8");
    CHECK(r.mirror.canClaimThreefold());
    r.send(GuestSide, claim);
    auto e = r.recent<P::GameEnd>(HostSide);
    CHECK(e.size() == 1 && e[0].status == P::GameStatus::Draw && e[0].reason == P::EndReason::ThreefoldClaim);
    // Game over: moves and offers are refused.
    r.move(HostSide, "e2e4");
    CHECK(r.has<P::MoveRejected>(HostSide) && r.recent<P::MoveRejected>(HostSide)[0].code == P::ErrorCode::GameOver);
}

TEST(direct_authority_draw_agreement_and_abort) {
    {
        Room r(tc(300, 0), 1);
        r.start();
        r.move(HostSide, "e2e4");
        P::DrawOffer off;
        off.game = r.a.gameId();
        r.send(GuestSide, off);
        P::DrawAnswer ans;
        ans.game = r.a.gameId();
        ans.accept = true;
        r.send(HostSide, ans);
        auto e = r.recent<P::GameEnd>(GuestSide);
        CHECK(e.size() == 1 && e[0].status == P::GameStatus::Draw && e[0].reason == P::EndReason::Agreement);
    }
    {
        // Crossing offers are an agreement.
        Room r(tc(300, 0), 1);
        r.start();
        P::DrawOffer off;
        off.game = r.a.gameId();
        r.send(GuestSide, off);
        r.send(HostSide, off);
        auto e = r.recent<P::GameEnd>(GuestSide);
        CHECK(e.size() == 1 && e[0].reason == P::EndReason::Agreement);
    }
    {
        Room r(tc(300, 0), 1);
        r.start();
        r.move(HostSide, "e2e4");
        P::Abort ab;
        ab.game = r.a.gameId();
        r.send(HostSide, ab);   // White already moved
        auto er = r.recent<P::Error>(HostSide);
        CHECK(er.size() == 1 && er[0].code == P::ErrorCode::AbortNotAllowed);
        r.send(GuestSide, ab);  // Black has not
        auto e = r.recent<P::GameEnd>(HostSide);
        CHECK(e.size() == 1 && e[0].status == P::GameStatus::Aborted && e[0].reason == P::EndReason::Aborted);
    }
}

TEST(direct_authority_disconnection_grace) {
    {
        Room r(tc(300, 0), 1);
        r.start();
        r.move(HostSide, "e2e4");
        r.move(GuestSide, "e7e5");
        r.a.onDisconnect(GuestSide, r.now, r.out);
        r.collect();
        auto ev = r.recent<P::GameEvent>(HostSide);
        CHECK(ev.size() == 1 && ev[0].kind == P::GameEventKind::PlayerDisconnected && ev[0].color == P::Color::Black &&
              ev[0].arg == 60000);
        // Back within the grace: a snapshot for the guest, the news for the host. The snapshot
        // carries the gseq of that news, which it shows: the guest's next event follows it.
        r.now += 30000;
        r.a.onReconnect(GuestSide, r.now, r.out);
        r.collect();
        auto s = r.recent<P::GameSnapshot>(GuestSide);
        CHECK(s.size() == 1 && s[0].moves.size() == 2 && s[0].blackConnected);
        ev = r.recent<P::GameEvent>(HostSide);
        CHECK(ev.size() == 1 && ev[0].kind == P::GameEventKind::PlayerReconnected);
        CHECK(s.size() == 1 && ev.size() == 1 && s[0].gseq == ev[0].gseq);
        r.move(HostSide, "g1f3");
        auto mm = r.recent<P::MoveMade>(GuestSide);
        CHECK(s.size() == 1 && mm.size() == 1 && mm[0].gseq == s[0].gseq + 1);
        // Gone again for the whole grace: the guest loses.
        r.a.onDisconnect(GuestSide, r.now, r.out);
        r.collect();
        r.now += 60000;
        r.tick();
        auto e = r.recent<P::GameEnd>(HostSide);
        CHECK(e.size() == 1 && e[0].status == P::GameStatus::WhiteWins && e[0].reason == P::EndReason::Abandonment);
    }
    {
        // Before two plies: aborted.
        Room r(tc(300, 0), 1);
        r.start();
        r.move(HostSide, "e2e4");
        r.a.onDisconnect(GuestSide, r.now, r.out);
        r.collect();
        r.now += 60000;
        r.tick();
        auto e = r.recent<P::GameEnd>(HostSide);
        CHECK(e.size() == 1 && e[0].status == P::GameStatus::Aborted && e[0].reason == P::EndReason::NoShow);
    }
}

TEST(direct_authority_rematch) {
    Room r(tc(300, 0), 1);
    r.start();
    uint64_t first = r.a.gameId();
    P::Resign res;
    res.game = first;
    r.send(HostSide, res);
    auto e = r.recent<P::GameEnd>(GuestSide);
    CHECK(e.size() == 1 && e[0].status == P::GameStatus::BlackWins && e[0].reason == P::EndReason::Resignation);
    P::Rematch rm;
    rm.game = first;
    rm.accept = true;
    r.send(GuestSide, rm);
    auto ev = r.recent<P::GameEvent>(HostSide);
    CHECK(ev.size() == 1 && ev[0].kind == P::GameEventKind::RematchOffered && ev[0].color == P::Color::Black);
    r.send(HostSide, rm);
    auto s = r.recent<P::GameSnapshot>(HostSide);
    CHECK(s.size() == 1 && s[0].game != first && s[0].you == P::Color::Black && s[0].white.name == "Bob");
    CHECK(r.has<P::GameSnapshot>(GuestSide));
    // The new game: the rematch window of the old one is gone.
    rm.game = first;
    r.send(GuestSide, rm);
    CHECK(r.has<P::Error>(GuestSide) && r.recent<P::Error>(GuestSide)[0].code == P::ErrorCode::RematchUnavailable);
    // End it; nobody asks within 60 s: the window closes.
    res.game = r.a.gameId();
    r.send(GuestSide, res);
    r.now += 60000;
    r.tick();
    ev = r.recent<P::GameEvent>(HostSide);
    CHECK(ev.size() == 1 && ev[0].kind == P::GameEventKind::RematchDeclined && ev[0].color == P::Color::None);
    rm.game = r.a.gameId();
    r.send(HostSide, rm);
    CHECK(r.has<P::Error>(HostSide) && r.recent<P::Error>(HostSide)[0].code == P::ErrorCode::RematchUnavailable);
}

TEST(direct_authority_auto_press) {
    {
        Room r(tc(300, 0), 1);
        r.start();
        auto s = r.recent<P::GameSnapshot>(GuestSide);
        CHECK(s.size() == 1 && s[0].autoPress);   // the default
    }
    // The host's choice goes in every snapshot: the start, a reconnection, a resync, a rematch.
    direct::AuthorityConfig cfg = tc(300, 0);
    cfg.autoPress = false;
    Room r(cfg, 1);
    r.start();
    for (Side s : {HostSide, GuestSide}) {
        auto v = r.recent<P::GameSnapshot>(s);
        CHECK(v.size() == 1 && !v[0].autoPress);
    }
    r.a.onDisconnect(GuestSide, r.now, r.out);
    r.collect();
    r.now += 2000;
    r.a.onReconnect(GuestSide, r.now, r.out);
    r.collect();
    auto back = r.recent<P::GameSnapshot>(GuestSide);
    CHECK(back.size() == 1 && !back[0].autoPress);
    P::Resync rs;
    rs.game = r.a.gameId();
    r.send(GuestSide, rs);
    auto again = r.recent<P::GameSnapshot>(GuestSide);
    CHECK(again.size() == 1 && !again[0].autoPress);
    const uint64_t first = r.a.gameId();
    P::Resign res;
    res.game = first;
    r.send(HostSide, res);
    P::Rematch rm;
    rm.game = first;
    rm.accept = true;
    r.send(GuestSide, rm);
    r.send(HostSide, rm);
    for (Side s : {HostSide, GuestSide}) {
        auto v = r.recent<P::GameSnapshot>(s);
        CHECK(v.size() == 1 && v[0].game != first && !v[0].autoPress);
    }
}

TEST(direct_authority_ply_cap) {
    // No Move can carry a ply beyond 1199: the game that reaches 1200 plies ends aborted
    // (ServerAborted) as on the server, instead of leaving the side to move unable to play until
    // its flag falls. The pieces wander without captures and a pawn steps every 100 plies, so that
    // neither the 75-move rule nor a fivefold repetition ends the game first.
    Room r(tc(300, 0), 1);   // host White
    r.start();
    uint32_t rng = 12345;
    while (r.a.plies() < 1200) {
        const int n = r.a.plies();
        const std::vector<chess::Move> moves = r.mirror.position().legalMoves();
        const bool pawnTurn = n % 100 == 99;
        rng = rng * 1103515245u + 12345u;
        chess::Move pick;
        for (int pass = 0; pass < 2 && !pick.valid(); ++pass) {
            for (size_t k = 0; k < moves.size() && !pick.valid(); ++k) {
                const chess::Move& mv = moves[(k + rng / 65536) % moves.size()];
                const bool pawn = r.mirror.position().at(mv.from).type == chess::Pawn;
                if (pass == 0 && ((mv.flags & chess::MoveCapture) || pawn != pawnTurn)) continue;
                chess::Game probe = r.mirror;
                probe.play(mv);
                if (!probe.isOver()) pick = mv;
            }
        }
        CHECK(pick.valid());
        if (!pick.valid()) return;
        r.move(n % 2 == 0 ? HostSide : GuestSide, r.mirror.position().toUCI(pick).c_str());
        CHECK_EQ(r.a.plies(), n + 1);
        if (r.a.plies() != n + 1) return;
        if (n + 1 < 1200) CHECK(!r.a.isOver());
    }
    auto mm = r.recent<P::MoveMade>(GuestSide);
    CHECK(mm.size() == 1 && mm[0].ply == 1199);
    for (Side s : {HostSide, GuestSide}) {
        auto e = r.recent<P::GameEnd>(s);
        CHECK(e.size() == 1 && e[0].status == P::GameStatus::Aborted && e[0].reason == P::EndReason::ServerAborted);
    }
}

// ---- loopback matches: two DirectMatch in this process ------------------------------------------

namespace {

struct Peer {
    DirectMatch dm;
    std::vector<Event> events;
    void drain() {
        Event e;
        while (dm.poll(e)) events.push_back(e);
    }
    int count(Event::Kind k) const {
        int n = 0;
        for (auto& e : events) n += e.kind == k;
        return n;
    }
    const Event* last(Event::Kind k) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->kind == k) return &*it;
        return nullptr;
    }
    bool hasMove(int ply) const {
        for (auto& e : events)
            if (e.kind == Event::Kind::MoveMade && e.ply == ply) return true;
        return false;
    }
    bool hasConn(ConnState s) const {
        for (auto& e : events)
            if (e.kind == Event::Kind::ConnectionChanged && e.state == s) return true;
        return false;
    }
};

bool waitUntil(Peer& a, Peer& b, const std::function<bool()>& cond, int ms = 8000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        a.drain();
        b.drain();
        if (cond()) return true;
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Plays 'uci' from 'mover', waits for both confirmations, mirrors it and returns the flags.
int play(Peer& mover, Peer& other, chess::Game& g, const char* uci, bool drawOffer = false) {
    chess::Move mv = g.position().parseUCI(uci);
    CHECK(mv.valid());
    int ply = int(g.moves().size());
    mover.dm.sendMove(ply, packMove(mv.from, mv.to, mv.promotion), g.position().fen(), 40, drawOffer);
    bool ok = waitUntil(mover, other, [&] { return mover.hasMove(ply) && other.hasMove(ply); });
    CHECK(ok);
    g.play(mv);
    const Event* e = other.last(Event::Kind::MoveMade);
    return e ? e->flags : -1;
}

DirectHostOptions hostOptions(int baseSec, int incSec, int color, const char* name) {
    DirectHostOptions o;
    o.port = 0;
    o.upnp = false;
    o.baseSec = baseSec;
    o.incSec = incSec;
    o.hostColor = color;
    o.playerName = name;
    return o;
}

// Starts a hosted game and joins it; host plays White.
bool startMatch(Peer& host, Peer& guest, int baseSec, int incSec, uint16_t viaPort = 0) {
    host.dm.host(hostOptions(baseSec, incSec, 1, "Alice"));
    if (!waitUntil(host, guest, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; })) return false;
    DirectInvite inv = host.dm.invite();
    guest.dm.join("127.0.0.1", viaPort ? viaPort : inv.port, inv.code, "Bob");
    return waitUntil(host, guest, [&] { return host.count(Event::Kind::GameSnapshot) >= 1 && guest.count(Event::Kind::GameSnapshot) >= 1; });
}

// A TCP relay on 127.0.0.1 that the test can cut (a network failure between the players), hold
// (the connections it gets are closed at once: the host is out of reach for a while) or shut
// (the host's machine is gone: connections are refused).
struct Relay {
    sock::Handle listener = sock::kInvalid;
    uint16_t port = 0, target = 0;
    std::thread th;
    std::atomic<bool> stop{false}, cutNow{false}, closed{false}, hold{false};

    bool start(uint16_t targetPort) {
        target = targetPort;
        bool dual = false;
        std::string err;
        listener = sock::listenTcp(0, dual, err);
        sock::Endpoint ep;
        if (listener == sock::kInvalid || !sock::localEndpoint(listener, ep)) return false;
        port = ep.port();
        th = std::thread([this] { run(); });
        return true;
    }
    void cut() { cutNow = true; }
    void shut() { closed = true; }
    ~Relay() {
        stop = true;
        if (th.joinable()) th.join();
        sock::closeSocket(listener);
    }
    void run() {
        struct Pair { sock::Handle a, b; };
        std::vector<Pair> pairs;
        while (!stop) {
            if (closed && listener != sock::kInvalid) {
                sock::closeSocket(listener);
                listener = sock::kInvalid;
            }
            if (cutNow || closed) {
                for (auto& p : pairs) { sock::closeSocket(p.a); sock::closeSocket(p.b); }
                pairs.clear();
                cutNow = false;
            }
            sock::PollSet ps;
            if (listener != sock::kInvalid) ps.add(listener, true, false);
            for (auto& p : pairs) { ps.add(p.a, true, false); ps.add(p.b, true, false); }
            ps.wait(10);
            if (listener != sock::kInvalid && ps.readable(listener)) {
                sock::Handle a = sock::acceptOne(listener, nullptr);
                if (a != sock::kInvalid && hold) {
                    sock::closeSocket(a);
                } else if (a != sock::kInvalid) {
                    sock::Endpoint to;
                    sock::Endpoint::parse("127.0.0.1", target, to);
                    std::string err;
                    sock::Handle b = sock::connectWithTimeout(to, 2000, err);
                    if (b == sock::kInvalid) sock::closeSocket(a);
                    else pairs.push_back({a, b});
                }
            }
            for (size_t i = 0; i < pairs.size();) {
                bool dead = false;
                auto pumpOne = [&](sock::Handle from, sock::Handle to) {
                    if (!ps.readable(from)) return;
                    uint8_t buf[16384];
                    bool eof = false;
                    int r = sock::recvSome(from, buf, sizeof buf, eof);
                    if (r < 0) { dead = true; return; }
                    for (int sent = 0; sent < r;) {
                        int w = sock::sendSome(to, buf + sent, size_t(r - sent));
                        if (w < 0) { dead = true; return; }
                        if (w == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        sent += w;
                    }
                };
                pumpOne(pairs[i].a, pairs[i].b);
                pumpOne(pairs[i].b, pairs[i].a);
                if (dead) {
                    sock::closeSocket(pairs[i].a);
                    sock::closeSocket(pairs[i].b);
                    pairs.erase(pairs.begin() + long(i));
                } else {
                    ++i;
                }
            }
        }
        for (auto& p : pairs) { sock::closeSocket(p.a); sock::closeSocket(p.b); }
    }
};

}  // namespace

TEST(direct_loopback_full_game) {
    Peer host, guest;
    host.dm.host(hostOptions(300, 2, 1, "Alice"));
    CHECK(host.dm.isHost());
    CHECK(waitUntil(host, guest, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; }));
    DirectInvite inv = host.dm.invite();
    CHECK(inv.port != 0);
    CHECK_EQ(inv.code.size(), size_t(14));
    CHECK(inv.code[4] == '-' && inv.code[9] == '-');
    CHECK(inv.publicAddress.empty());
    CHECK(host.dm.upnp().state == UpnpStatus::State::NotTried);
    // The guest types the code in lower case with spaces.
    std::string typed = inv.code;
    for (char& ch : typed) ch = ch == '-' ? ' ' : char(std::tolower((unsigned char)ch));
    guest.dm.join("127.0.0.1", inv.port, typed, "Bob");
    CHECK(!guest.dm.isHost());
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameSnapshot) && guest.count(Event::Kind::GameSnapshot); }));
    CHECK(host.dm.state() == DirectMatch::State::Playing);
    CHECK(guest.dm.state() == DirectMatch::State::Playing);
    CHECK(host.hasConn(ConnState::Online));
    CHECK(guest.hasConn(ConnState::Online));
    const OnlineGame* hg = host.dm.currentGame();
    const OnlineGame* gg = guest.dm.currentGame();
    CHECK(hg && gg);
    if (!hg || !gg) return;
    CHECK_EQ(hg->you, 0);
    CHECK_EQ(gg->you, 1);
    CHECK_EQ(gg->white.name, std::string("Alice"));
    CHECK_EQ(gg->black.name, std::string("Bob"));
    CHECK_EQ(gg->category, std::string("custom"));
    CHECK(!gg->rated);
    CHECK_EQ(gg->baseMs, int64_t(300000));
    CHECK_EQ(gg->incMs, int64_t(2000));
    CHECK_EQ(hg->id, gg->id);

    chess::Game g;
    play(host, guest, g, "e2e4");
    play(guest, host, g, "d7d5");
    play(host, guest, g, "e4e5");
    play(guest, host, g, "f7f5");
    // Not the guest's turn: refused for the guest only.
    guest.dm.sendMove(int(g.moves().size()), packMove(chess::parseSquare("a7"), chess::parseSquare("a6"), 0), g.position().fen(), 10, false);
    CHECK(waitUntil(host, guest, [&] { return guest.count(Event::Kind::MoveRejected) == 1; }));
    CHECK(guest.last(Event::Kind::MoveRejected)->code == int(P::ErrorCode::NotYourTurn));
    CHECK_EQ(host.count(Event::Kind::MoveRejected), 0);
    int flags = play(host, guest, g, "e5f6");   // en passant
    CHECK((flags & P::MoveFlag::EnPassant) && (flags & P::MoveFlag::Capture));
    play(guest, host, g, "b8c6");
    play(host, guest, g, "f6g7");
    play(guest, host, g, "c8f5");
    flags = play(host, guest, g, "g7h8q");      // promotion with capture
    CHECK((flags & P::MoveFlag::Promotion) && (flags & P::MoveFlag::Capture));
    CHECK_EQ(movePromo(host.last(Event::Kind::MoveMade)->move), 5);
    play(guest, host, g, "d8d7");
    play(host, guest, g, "g1f3", true);         // with a draw offer
    CHECK(host.dm.currentGame()->drawOfferBy == 0);
    CHECK(guest.dm.currentGame()->drawOfferBy == 0);
    flags = play(guest, host, g, "e8c8");       // castling queen side declines the offer
    CHECK(flags & P::MoveFlag::CastleQueen);
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameEvent) >= 1 && guest.count(Event::Kind::GameEvent) >= 1; }));
    const Event* de = host.last(Event::Kind::GameEvent);
    CHECK(de && de->gameEventKind == int(P::GameEventKind::DrawDeclined) && de->color == 1);
    CHECK(host.dm.currentGame()->drawOfferBy == 2);
    CHECK_EQ(guest.dm.currentGame()->moves.size(), size_t(12));
    // Both sides measured the round trip; the guest's clock estimate is the host's clock.
    CHECK(waitUntil(host, guest, [&] { return host.dm.pingMs() >= 0 && guest.dm.pingMs() >= 0; }, 5000));
    CHECK(std::fabs(guest.dm.serverNowMs() - host.dm.serverNowMs()) < 250);
    // White (the host) resigns.
    host.dm.resign();
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameEnd) && guest.count(Event::Kind::GameEnd); }));
    CHECK(guest.last(Event::Kind::GameEnd)->game.status == int(P::GameStatus::BlackWins));
    CHECK(guest.last(Event::Kind::GameEnd)->game.reason == int(P::EndReason::Resignation));
    CHECK(host.dm.state() == DirectMatch::State::Playing);   // until close(), for a rematch
    // Rematch with colours swapped.
    uint64_t firstId = gg->id;
    guest.dm.rematch(true);
    CHECK(waitUntil(host, guest, [&] {
        const Event* e = host.last(Event::Kind::GameEvent);
        return e && e->gameEventKind == int(P::GameEventKind::RematchOffered);
    }));
    host.dm.rematch(true);
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameSnapshot) == 2 && guest.count(Event::Kind::GameSnapshot) == 2; }));
    CHECK(host.dm.currentGame()->id != firstId);
    CHECK_EQ(host.dm.currentGame()->you, 1);
    CHECK_EQ(guest.dm.currentGame()->you, 0);
    // The guest, now White, aborts before its first move.
    guest.dm.abortGame();
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameEnd) == 2 && guest.count(Event::Kind::GameEnd) == 2; }));
    CHECK(host.dm.currentGame()->status == int(P::GameStatus::Aborted));
    CHECK(host.dm.currentGame()->reason == int(P::EndReason::Aborted));
    guest.dm.close();
    host.dm.close();
    CHECK(host.dm.state() == DirectMatch::State::Idle);
    CHECK(host.dm.currentGame() == nullptr);
}

TEST(direct_loopback_wrong_code) {
    Peer host, guest, other;
    host.dm.host(hostOptions(300, 0, 0, "Alice"));
    CHECK(waitUntil(host, guest, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; }));
    DirectInvite inv = host.dm.invite();
    std::string wrong = inv.code;
    wrong[0] = wrong[0] == 'A' ? 'B' : 'A';
    guest.dm.join("127.0.0.1", inv.port, wrong, "Mallory");
    CHECK(waitUntil(host, guest, [&] { return guest.dm.state() == DirectMatch::State::Failed; }));
    CHECK_EQ(guest.dm.lastError(), std::string("wrong_code"));
    CHECK(guest.hasConn(ConnState::Offline));
    CHECK_EQ(host.count(Event::Kind::GameSnapshot), 0);
    CHECK(waitUntil(host, guest, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; }));
    // Malformed codes and closed ports fail at once.
    other.dm.join("127.0.0.1", inv.port, "K7Q2-M9XH-3PT0", "Bob");
    CHECK(other.dm.state() == DirectMatch::State::Failed);
    CHECK_EQ(other.dm.lastError(), std::string("invalid_code"));
    {
        sock::Handle tmp = sock::openTcp(sock::Endpoint::anyV4(0).family());
        sock::Endpoint lo, ep;
        sock::Endpoint::parse("127.0.0.1", 0, lo);
        CHECK(sock::bindTo(tmp, lo, false) && sock::localEndpoint(tmp, ep));
        uint16_t closedPort = ep.port();
        sock::closeSocket(tmp);   // bound then closed: nobody listens there
        other.dm.join("127.0.0.1", closedPort, inv.code, "Bob");
        CHECK(waitUntil(host, other, [&] { return other.dm.state() == DirectMatch::State::Failed; }));
        CHECK_EQ(other.dm.lastError(), std::string("refused"));
    }
    // The real guest still gets in.
    other.dm.join("127.0.0.1", inv.port, inv.code, "Bob");
    CHECK(waitUntil(host, other, [&] { return other.count(Event::Kind::GameSnapshot) && host.count(Event::Kind::GameSnapshot); }));
    // A second host on the same port: port_in_use.
    Peer second;
    DirectHostOptions o = hostOptions(300, 0, 0, "Eve");
    o.port = inv.port;
    second.dm.host(o);
    CHECK(waitUntil(second, guest, [&] { return second.dm.state() == DirectMatch::State::Failed; }));
    CHECK_EQ(second.dm.lastError(), std::string("port_in_use"));
}

TEST(direct_loopback_host_again_same_port) {
    // Close a match with a connected guest and host again at once on the same port: the new
    // match waits for the old one to release it.
    Peer host, guest;
    CHECK(startMatch(host, guest, 300, 0));
    uint16_t port = host.dm.invite().port;
    host.dm.close();
    DirectHostOptions o = hostOptions(300, 0, 0, "Alice");
    o.port = port;
    host.dm.host(o);
    CHECK(waitUntil(host, guest, [&] {
        auto s = host.dm.state();
        return s == DirectMatch::State::WaitingForGuest || s == DirectMatch::State::Failed;
    }));
    CHECK(host.dm.state() == DirectMatch::State::WaitingForGuest);
    CHECK_EQ(host.dm.invite().port, port);
    CHECK_EQ(host.dm.lastError(), std::string(""));
}

TEST(direct_loopback_reconnection) {
    Peer host, guest;
    host.dm.host(hostOptions(300, 0, 1, "Alice"));
    CHECK(waitUntil(host, guest, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; }));
    DirectInvite inv = host.dm.invite();
    Relay relay;
    CHECK(relay.start(inv.port));
    guest.dm.join("127.0.0.1", relay.port, inv.code, "Bob");
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameSnapshot) && guest.count(Event::Kind::GameSnapshot); }));
    chess::Game g;
    play(host, guest, g, "e2e4");
    play(guest, host, g, "c7c5");
    relay.cut();   // the network fails between the two players
    CHECK(waitUntil(host, guest, [&] {
        const Event* e = host.last(Event::Kind::GameEvent);
        return e && e->gameEventKind == int(P::GameEventKind::PlayerDisconnected);
    }));
    CHECK_EQ(host.last(Event::Kind::GameEvent)->color, 1);
    CHECK(host.dm.currentGame() && !host.dm.currentGame()->blackConnected);
    CHECK(waitUntil(host, guest, [&] { return guest.hasConn(ConnState::Reconnecting); }));
    // The guest comes back by itself with the same code and gets the game again.
    CHECK(waitUntil(host, guest, [&] { return guest.count(Event::Kind::GameSnapshot) == 2; }));
    CHECK_EQ(guest.last(Event::Kind::GameSnapshot)->game.moves.size(), size_t(2));
    CHECK(waitUntil(host, guest, [&] {
        const Event* e = host.last(Event::Kind::GameEvent);
        return e && e->gameEventKind == int(P::GameEventKind::PlayerReconnected);
    }));
    CHECK(host.dm.currentGame()->blackConnected);
    CHECK(guest.dm.state() == DirectMatch::State::Playing);
    play(host, guest, g, "g1f3");
    play(guest, host, g, "d7d6");
    // Now the host's machine vanishes: the guest gives up and the game ends as ServerAborted.
    relay.shut();
    CHECK(waitUntil(host, guest, [&] { return guest.count(Event::Kind::GameEnd) == 1; }, 15000));
    const Event* end = guest.last(Event::Kind::GameEnd);
    CHECK(end && end->game.status == int(P::GameStatus::Aborted) && end->game.reason == int(P::EndReason::ServerAborted));
    CHECK_EQ(guest.dm.lastError(), std::string("host_left"));
}

TEST(direct_loopback_flag_and_host_leaving) {
    {
        // A tiny time control: White (the host) runs out of time after the two free plies.
        Peer host, guest;
        CHECK(startMatch(host, guest, 1, 0));
        chess::Game g;
        play(host, guest, g, "e2e4");
        play(guest, host, g, "e7e5");
        CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameEnd) && guest.count(Event::Kind::GameEnd); }, 5000));
        const OnlineGame* og = guest.dm.currentGame();
        CHECK(og && og->status == int(P::GameStatus::BlackWins) && og->reason == int(P::EndReason::Timeout) && og->whiteMs == 0);
    }
    {
        // The host leaves a running game: it resigns, the guest is told, then the link closes.
        Peer host, guest;
        CHECK(startMatch(host, guest, 300, 0));
        chess::Game g;
        play(host, guest, g, "d2d4");
        host.dm.close();
        CHECK(waitUntil(host, guest, [&] { return guest.count(Event::Kind::GameEnd) == 1; }));
        const OnlineGame* og = guest.dm.currentGame();
        CHECK(og && og->status == int(P::GameStatus::BlackWins) && og->reason == int(P::EndReason::Resignation));
        CHECK(waitUntil(host, guest, [&] { return guest.hasConn(ConnState::Offline); }));
    }
    {
        // The guest leaves: it resigns too.
        Peer host, guest;
        CHECK(startMatch(host, guest, 300, 0));
        guest.dm.close();
        CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameEnd) == 1; }));
        const OnlineGame* og = host.dm.currentGame();
        CHECK(og && og->status == int(P::GameStatus::WhiteWins) && og->reason == int(P::EndReason::Resignation));
    }
}

// ---- gestures -----------------------------------------------------------------------------------

TEST(direct_loopback_gestures) {
    // The host's autoPress reaches both players, in the rematch too; Gestures go both ways, paced
    // for the receiver's bucket, the latest one last.
    Peer host, guest;
    DirectHostOptions o = hostOptions(300, 0, 1, "Alice");
    o.autoPress = false;
    host.dm.host(o);
    CHECK(waitUntil(host, guest, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; }));
    DirectInvite inv = host.dm.invite();
    guest.dm.join("127.0.0.1", inv.port, inv.code, "Bob");
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameSnapshot) && guest.count(Event::Kind::GameSnapshot); }));
    const OnlineGame* hg = host.dm.currentGame();
    const OnlineGame* gg = guest.dm.currentGame();
    CHECK(hg && gg);
    if (!hg || !gg) return;
    CHECK(!hg->autoPress && !gg->autoPress);
    const uint64_t first = gg->id;

    // Host to guest, every field.
    Gesture a;
    a.touch = 12;
    a.aim = 28;
    a.placed = packMove(12, 28, 0);
    a.flags = P::GestureFlag::Side;
    a.yaw = 0.25f;
    a.pitch = -0.4f;
    a.lean = 0.6f;
    host.dm.sendGesture(a);
    CHECK(waitUntil(host, guest, [&] { return guest.count(Event::Kind::OpponentGesture) == 1; }));
    const Event* e = guest.last(Event::Kind::OpponentGesture);
    CHECK(e && e->gameId == first && e->game.id == 0 && e->gesture.sameState(a));
    CHECK(e && std::fabs(e->gesture.yaw - 0.25f) < 1e-3f && std::fabs(e->gesture.pitch + 0.4f) < 1e-3f &&
          std::fabs(e->gesture.lean - 0.6f) < 1e-3f);
    // Guest to host.
    Gesture b;
    b.ply = 1;
    b.touch = 52;
    b.aim = 36;
    b.flags = uint8_t(P::GestureFlag::Glance | P::GestureFlag::Side);
    b.yaw = -0.3f;
    b.pitch = -0.2f;
    b.lean = 0.1f;
    guest.dm.sendGesture(b);
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::OpponentGesture) == 1; }));
    e = host.last(Event::Kind::OpponentGesture);
    CHECK(e && e->gameId == first && e->gesture.sameState(b));
    CHECK(e && std::fabs(e->gesture.yaw + 0.3f) < 1e-3f && std::fabs(e->gesture.lean - 0.1f) < 1e-3f);
    CHECK_EQ(guest.count(Event::Kind::OpponentGesture), 1);   // nobody gets their own

    // A call every 2 ms for 1.5 s from the guest: at most the host's bucket less one at once
    // (19), then 10 per second; the very latest arrives, never an older one after a newer one.
    const size_t mark = host.events.size();
    auto t0 = std::chrono::steady_clock::now();
    auto msSince = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };
    while (msSince(t0) < 1500) {
        ++b.ply;
        guest.dm.sendGesture(b);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        host.drain();
    }
    const int last = b.ply;
    CHECK(waitUntil(host, guest, [&] {
        const Event* x = host.last(Event::Kind::OpponentGesture);
        return x && x->gesture.ply == last;
    }, 3000));
    const double span = msSince(t0);
    int n = 0, prev = 1;
    bool newer = true;
    for (size_t i = mark; i < host.events.size(); ++i) {
        if (host.events[i].kind != Event::Kind::OpponentGesture) continue;
        ++n;
        newer = newer && host.events[i].gesture.ply > prev;
        prev = host.events[i].gesture.ply;
    }
    CHECK(newer);
    CHECK(n >= 20);
    CHECK(n <= gestureSendCapacity(20) + int(10 * span / 1000.0) + 1);

    // The rematch keeps the host's choice; its Gestures carry the new game.
    host.dm.resign();
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameEnd) && guest.count(Event::Kind::GameEnd); }));
    guest.dm.rematch(true);
    CHECK(waitUntil(host, guest, [&] {
        const Event* x = host.last(Event::Kind::GameEvent);
        return x && x->gameEventKind == int(P::GameEventKind::RematchOffered);
    }));
    host.dm.rematch(true);
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameSnapshot) == 2 && guest.count(Event::Kind::GameSnapshot) == 2; }));
    hg = host.dm.currentGame();
    gg = guest.dm.currentGame();
    CHECK(hg && gg && hg->id != first && gg->id == hg->id);
    if (!hg || !gg) return;
    CHECK(!hg->autoPress && !gg->autoPress);
    a.touch = 6;
    host.dm.sendGesture(a);
    CHECK(waitUntil(host, guest, [&] {
        const Event* x = guest.last(Event::Kind::OpponentGesture);
        return x && x->gesture.touch == 6;
    }));
    CHECK(guest.last(Event::Kind::OpponentGesture)->gameId == gg->id);
}

TEST(direct_loopback_gestures_not_replayed) {
    // Gestures made while the link is down are dropped, both ways: none arrives after the
    // reconnection; the next ones do.
    Peer host, guest;
    host.dm.host(hostOptions(300, 0, 1, "Alice"));
    CHECK(waitUntil(host, guest, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; }));
    DirectInvite inv = host.dm.invite();
    Relay relay;
    CHECK(relay.start(inv.port));
    guest.dm.join("127.0.0.1", relay.port, inv.code, "Bob");
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameSnapshot) && guest.count(Event::Kind::GameSnapshot); }));
    relay.hold = true;
    relay.cut();
    CHECK(waitUntil(host, guest, [&] {
        const Event* x = host.last(Event::Kind::GameEvent);
        return guest.hasConn(ConnState::Reconnecting) && x && x->gameEventKind == int(P::GameEventKind::PlayerDisconnected);
    }));
    Gesture g;
    g.touch = 12;
    for (int i = 0; i < 10; ++i) {
        host.dm.sendGesture(g);
        guest.dm.sendGesture(g);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    relay.hold = false;
    CHECK(waitUntil(host, guest, [&] {
        const Event* x = host.last(Event::Kind::GameEvent);
        return guest.count(Event::Kind::GameSnapshot) == 2 && x && x->gameEventKind == int(P::GameEventKind::PlayerReconnected);
    }, 15000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    host.drain();
    guest.drain();
    CHECK_EQ(host.count(Event::Kind::OpponentGesture), 0);
    CHECK_EQ(guest.count(Event::Kind::OpponentGesture), 0);
    g.touch = 13;
    host.dm.sendGesture(g);
    guest.dm.sendGesture(g);
    CHECK(waitUntil(host, guest, [&] {
        const Event* x = host.last(Event::Kind::OpponentGesture);
        const Event* y = guest.last(Event::Kind::OpponentGesture);
        return x && y && x->gesture.touch == 13 && y->gesture.touch == 13;
    }));
    CHECK_EQ(host.count(Event::Kind::OpponentGesture), 1);
    CHECK_EQ(guest.count(Event::Kind::OpponentGesture), 1);
}

namespace {

// One end of the secure channel driven by hand from the test's thread.
struct RawChannel {
    sock::Handle h = sock::kInvalid;
    std::unique_ptr<direct::SecureChannel> ch;
    int errors = 0;   // Error messages received

    ~RawChannel() { sock::closeSocket(h); }

    bool sendBytes(const std::vector<uint8_t>& buf) { return ch->send(buf.data(), buf.size()) && flush(); }
    // The next message of type T (others are skipped), within ms.
    template <class T> bool waitFor(T& out, int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        std::vector<uint8_t> msg;
        for (;;) {
            while (ch->popMessage(msg)) {
                P::Error e;
                if (P::decode(msg.data(), msg.size(), e)) ++errors;
                if (P::decode(msg.data(), msg.size(), out)) return true;
            }
            if (!pumpOnce() || std::chrono::steady_clock::now() > end) return false;
        }
    }
    // True when the other side closes the connection within ms (what it sends meanwhile is
    // read and dropped).
    bool waitClosed(int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            sock::PollSet ps;
            ps.add(h, true, false);
            ps.wait(10);
            uint8_t buf[4096];
            bool closed = false;
            if (ps.readable(h) && sock::recvSome(h, buf, sizeof buf, closed) < 0) return true;
        }
        return false;
    }

protected:
    // The channel's handshake over the connected socket 'h', within 5 s.
    bool handshake() {
        if (h == sock::kInvalid || !ch->start() || !flush()) return false;
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ch->established())
            if (!pumpOnce() || std::chrono::steady_clock::now() > end) return false;
        return true;
    }
    bool flush() {
        std::vector<uint8_t>& o = ch->outbox();
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        size_t done = 0;
        while (done < o.size()) {
            int w = sock::sendSome(h, o.data() + done, o.size() - done);
            if (w < 0 || std::chrono::steady_clock::now() > end) return false;
            if (w == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            done += size_t(w);
        }
        o.clear();
        return true;
    }
    // One read from the socket into the channel, then its answers out.
    bool pumpOnce() {
        sock::PollSet ps;
        ps.add(h, true, false);
        ps.wait(10);
        if (ps.readable(h)) {
            uint8_t buf[4096];
            bool closed = false;
            int r = sock::recvSome(h, buf, sizeof buf, closed);
            if (r < 0 || (r > 0 && !ch->receive(buf, size_t(r)))) return false;
        }
        return !ch->failed() && flush();
    }
};

// A guest written by hand over the secure channel, to send exactly the messages a test chooses.
struct RawGuest : RawChannel {
    uint32_t seq = 0;

    bool connect(uint16_t port, const std::string& code) {
        sock::Endpoint ep;
        std::string norm, err;
        if (!sock::Endpoint::parse("127.0.0.1", port, ep) || !direct::normalizeJoinCode(code, norm)) return false;
        h = sock::connectWithTimeout(ep, 2000, err);
        ch = std::make_unique<direct::SecureChannel>(direct::SecureChannel::Role::Guest, norm);
        return handshake();
    }
    template <class T> bool send(T m) {
        m.seq = ++seq;
        std::vector<uint8_t> buf;
        P::encode(m, buf);
        return sendBytes(buf);
    }
};

// A host written by hand: it accepts the guest's connections one at a time and sends exactly the
// messages a test chooses, when it chooses.
struct RawHost : RawChannel {
    sock::Handle listener = sock::kInvalid;
    uint16_t port = 0;
    std::string code = direct::newJoinCode();

    ~RawHost() { sock::closeSocket(listener); }

    bool listen() {
        bool dual = false;
        std::string err;
        listener = sock::listenTcp(0, dual, err);
        sock::Endpoint ep;
        if (listener == sock::kInvalid || !sock::localEndpoint(listener, ep) || code.empty()) return false;
        port = ep.port();
        return true;
    }
    // The guest's next connection, through the channel's handshake, within ms.
    bool accept(int ms) { return acceptSocket(ms) && handshake(); }
    // The guest's next connection through the handshake up to the guest's confirmation, then
    // closed before the host's: what the guest sees when the host refuses its code, or when the
    // link fails at that moment.
    bool acceptUnconfirmed(int ms) {
        if (!acceptSocket(ms) || !ch->start()) return false;
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!ch->established()) {
            if (!flush() || std::chrono::steady_clock::now() > end) return false;
            sock::PollSet ps;
            ps.add(h, true, false);
            ps.wait(10);
            uint8_t buf[4096];
            bool closed = false;
            int r = ps.readable(h) ? sock::recvSome(h, buf, sizeof buf, closed) : 0;
            if (r < 0 || (r > 0 && !ch->receive(buf, size_t(r)))) return false;
        }
        drop();   // the host's confirmation, in the outbox, is never sent
        return true;
    }
    void drop() {
        sock::closeSocket(h);
        h = sock::kInvalid;
    }
    template <class T> bool send(const T& m) {
        std::vector<uint8_t> buf;
        P::encode(m, buf);
        return sendBytes(buf);
    }

private:
    bool acceptSocket(int ms) {
        drop();
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (h == sock::kInvalid) {
            if (std::chrono::steady_clock::now() > end) return false;
            sock::PollSet ps;
            ps.add(listener, true, false);
            ps.wait(10);
            if (ps.readable(listener)) h = sock::acceptOne(listener, nullptr);
        }
        ch = std::make_unique<direct::SecureChannel>(direct::SecureChannel::Role::Host, code);
        return true;
    }
};

// Holds the stdio lock of stderr while it lives: a thread that logs meanwhile (logx writes every
// line to stderr) stops at that log line until the hold ends.
struct StderrHold {
#ifdef _WIN32
    StderrHold() { _lock_file(stderr); }
    ~StderrHold() { _unlock_file(stderr); }
#else
    StderrHold() { flockfile(stderr); }
    ~StderrHold() { funlockfile(stderr); }
#endif
    StderrHold(const StderrHold&) = delete;
    StderrHold& operator=(const StderrHold&) = delete;
};

}  // namespace

TEST(direct_guest_orders_game_events_by_gseq) {
    // The guest applies the host's game events in gseq order (PROTOCOL.md, "Ordering: gseq"), the
    // authority's messages relayed by hand: the next event applies; one its state already holds (a
    // repeated confirmation, an event its snapshot includes) is ignored; one beyond the next is not
    // applied and the guest asks for a Resync; so is a MoveMade of the next gseq for a later ply.
    using K = Event::Kind;
    Room r(tc(300, 0), 1);   // the host plays White
    r.start();
    const std::vector<std::vector<uint8_t>>& toGuest = r.msgs[GuestSide];
    RawHost raw;
    CHECK(raw.listen());
    Peer guest, nobody;
    guest.dm.join("127.0.0.1", raw.port, raw.code, "Bob");
    P::Hello hello;
    CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
    P::Welcome w;
    w.proto = P::kProtocolVersion;
    w.serverTime = sock::epochMs();
    w.userId = 2;
    w.username = "Bob";
    w.serverName = "Alice";
    w.heartbeatMs = 2000;
    w.clientPingMs = 2000;
    w.maxMsgPerSec = 40;
    w.activeGame = r.a.gameId();
    CHECK(raw.send(w));
    auto relay = [&](size_t i) { return i < toGuest.size() && raw.sendBytes(toGuest[i]); };
    CHECK(relay(0));                                    // the snapshot, gseq 0
    CHECK(waitUntil(guest, nobody, [&] { return guest.count(K::GameSnapshot) == 1; }));

    r.move(HostSide, "e2e4");                           // MoveMade, gseq 1
    CHECK(relay(1));
    CHECK(waitUntil(guest, nobody, [&] { return guest.hasMove(0); }));
    CHECK(guest.last(K::MoveMade) && guest.last(K::MoveMade)->game.gseq == 1);
    // The repeated confirmation, then the next event: only the latter shows.
    P::DrawOffer offer;
    offer.game = r.a.gameId();
    r.send(HostSide, offer);                            // GameEvent DrawOffered, gseq 2
    CHECK(relay(1) && relay(2));
    CHECK(waitUntil(guest, nobody, [&] { return guest.count(K::GameEvent) == 1; }));
    CHECK_EQ(guest.count(K::MoveMade), 1);
    CHECK(guest.dm.currentGame() && guest.dm.currentGame()->gseq == 2 && guest.dm.currentGame()->drawOfferBy == 0);

    // The guest moves: MoveMade (gseq 3), then DrawDeclined (gseq 4, the move declines the offer).
    // Only the second reaches it: not applied, a Resync instead.
    r.move(GuestSide, "e7e5");
    CHECK_EQ(toGuest.size(), size_t(5));
    CHECK(relay(4));
    P::Resync rs;
    CHECK(raw.waitFor(rs, 5000) && rs.game == r.a.gameId());
    r.send(GuestSide, rs);                              // the snapshot, gseq 4
    CHECK(relay(5));
    CHECK(waitUntil(guest, nobody, [&] { return guest.count(K::GameSnapshot) == 2; }));
    const Event* snap = guest.last(K::GameSnapshot);
    CHECK(snap && snap->game.gseq == 4 && snap->game.moves.size() == 2 && snap->game.drawOfferBy == 2);
    CHECK_EQ(guest.count(K::GameEvent), 1);            // the DrawDeclined was not applied

    // What the snapshot holds is ignored; the next event applies.
    r.move(HostSide, "g1f3");                           // MoveMade, gseq 5
    CHECK(relay(3) && relay(4) && relay(6));
    CHECK(waitUntil(guest, nobody, [&] { return guest.hasMove(2); }));
    CHECK_EQ(guest.count(K::MoveMade), 2);             // plies 0 and 2: the confirmation of ply 1 (gseq 3) was ignored
    CHECK_EQ(guest.count(K::GameEvent), 1);
    CHECK(guest.last(K::MoveMade) && guest.last(K::MoveMade)->game.gseq == 5);

    // The next gseq for a later ply than the next: a Resync too, nothing applied.
    P::MoveMade ahead;
    CHECK(toGuest.size() == 7 && P::decode(toGuest[6].data(), toGuest[6].size(), ahead));
    ahead.gseq = 6;
    ahead.ply = 4;
    CHECK(raw.send(ahead));
    CHECK(raw.waitFor(rs, 5000) && rs.game == r.a.gameId());
    guest.drain();
    CHECK_EQ(guest.count(K::MoveMade), 2);
    CHECK(guest.dm.currentGame() && guest.dm.currentGame()->gseq == 5 && guest.dm.currentGame()->moves.size() == 3);
}

TEST(direct_guest_gesture_at_once_after_reconnecting) {
    // The guest's game thread sends a Gesture the moment it sees Online again after a
    // reconnection (the scene does: the host waits for one sent after the return). The link is up
    // for Gestures before that event, so the Gesture reaches the host. The guest's worker is held
    // at the log line that follows the event, so a link brought up only after it would still be
    // down when the Gesture is sent. The guest's keepalive is the one of the host's last Welcome
    // (gestureIdleMs, clamped to 1 s .. 10 s).
    RawHost raw;
    CHECK(raw.listen());
    direct::Authority auth(direct::AuthorityConfig(), "Alice", "Bob", 1);
    direct::Authority::Output out;
    auth.startGame(sock::epochMs(), out);
    CHECK(out.toGuest.size() == 1);
    if (out.toGuest.size() != 1) return;
    auto welcome = [&](uint16_t gestureIdleMs) {
        P::Welcome w;
        w.proto = P::kProtocolVersion;
        w.serverTime = sock::epochMs();
        w.userId = 2;
        w.username = "Bob";
        w.serverName = "Alice";
        w.heartbeatMs = 2000;
        w.clientPingMs = 2000;
        w.maxMsgPerSec = 40;
        w.activeGame = auth.gameId();
        w.gestureRate = 10;
        w.gestureBurst = 20;
        w.gestureIdleMs = gestureIdleMs;
        return raw.send(w);
    };
    Peer guest, nobody;
    CHECK_EQ(guest.dm.gestureKeepaliveMs(), 1000);   // no match yet
    guest.dm.join("127.0.0.1", raw.port, raw.code, "Bob");
    P::Hello hello;
    CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
    CHECK(welcome(4000) && raw.sendBytes(out.toGuest[0]));
    CHECK(waitUntil(guest, nobody, [&] { return guest.count(Event::Kind::GameSnapshot) == 1; }));
    CHECK_EQ(guest.dm.gestureKeepaliveMs(), 4000);
    raw.drop();   // the network fails: the guest comes back by itself
    CHECK(waitUntil(guest, nobody, [&] { return guest.hasConn(ConnState::Reconnecting); }));
    CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
    Gesture g;
    g.touch = 12;
    bool online = false;
    {
        StderrHold hold;
        CHECK(welcome(30000));
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!online && std::chrono::steady_clock::now() < end) {
            Event e;
            while (!online && guest.dm.poll(e)) online = e.kind == Event::Kind::ConnectionChanged && e.state == ConnState::Online;
        }
        if (online) guest.dm.sendGesture(g);
    }
    CHECK(online);
    P::C_Gesture got;
    CHECK(raw.waitFor(got, 3000));
    CHECK(got.game == auth.gameId() && got.touch == 12);
    CHECK_EQ(guest.dm.gestureKeepaliveMs(), 10000);   // 30 s announced: clamped
}

TEST(direct_gestures_outside_flood_limit) {
    // The host announces its Gesture bucket in Welcome. 100 Gestures at once (five times the
    // flood limit) leave the link open: Gestures never count towards it. The host keeps those
    // its bucket allows (20 at once) for the current game only, and shows the latest of them.
    Peer host, nobody;
    host.dm.host(hostOptions(300, 0, 1, "Alice"));
    CHECK(waitUntil(host, nobody, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; }));
    DirectInvite inv = host.dm.invite();
    RawGuest raw;
    CHECK(raw.connect(inv.port, inv.code));
    P::Hello hello;
    hello.proto = P::kProtocolVersion;
    hello.client = "Scacelith test";
    hello.token = "direct:Raw      ";
    CHECK(raw.send(hello));
    P::Welcome w;
    CHECK(raw.waitFor(w, 5000));
    CHECK_EQ(int(w.gestureRate), 10);
    CHECK_EQ(int(w.gestureBurst), 20);
    CHECK_EQ(int(w.gestureIdleMs), 1000);
    CHECK_EQ(host.dm.gestureKeepaliveMs(), 1000);   // the host's own scene sends at that interval too
    CHECK_EQ(int(w.maxMsgPerSec), 20);
    CHECK_EQ(int(w.msgBurst), 20);
    P::GameSnapshot s;
    CHECK(raw.waitFor(s, 5000));
    CHECK(s.game != 0 && s.game == w.activeGame && s.autoPress);
    P::C_Gesture g;
    g.game = s.game;
    g.touch = 52;
    g.aim = 64;
    bool sent = true;
    for (int i = 1; i <= 100; ++i) {
        g.ply = uint16_t(i);
        sent = raw.send(g) && sent;
    }
    g.game = s.game + 1;   // another game
    g.ply = 999;
    sent = raw.send(g) && sent;
    CHECK(sent);
    P::C_Ping ping;
    ping.nonce = 4242;
    CHECK(raw.send(ping));
    P::S_Pong pong;
    CHECK(raw.waitFor(pong, 5000));   // the link is still open, and the Gestures were handled first
    CHECK_EQ(pong.nonce, 4242u);
    CHECK_EQ(raw.errors, 0);
    host.drain();
    CHECK_EQ(host.count(Event::Kind::OpponentGesture), 1);
    const Event* e = host.last(Event::Kind::OpponentGesture);
    CHECK(e && e->gameId == s.game && e->gesture.touch == 52);
    CHECK(e && e->gesture.ply >= 20 && e->gesture.ply <= 25);   // + what refilled meanwhile
    CHECK(host.dm.currentGame() && host.dm.currentGame()->blackConnected);
}

namespace {

// The Welcome a host written by hand sends for the game of 'auth'.
bool sendWelcome(RawHost& raw, const direct::Authority& auth) {
    P::Welcome w;
    w.proto = P::kProtocolVersion;
    w.serverTime = sock::epochMs();
    w.userId = 2;
    w.username = "Bob";
    w.serverName = "Alice";
    w.heartbeatMs = 2000;
    w.clientPingMs = 2000;
    w.maxMsgPerSec = 40;
    w.activeGame = auth.gameId();
    w.gestureRate = 10;
    w.gestureBurst = 20;
    w.gestureIdleMs = 1000;
    return raw.send(w);
}

// Hosts a match and joins it with a guest written by hand (its Hello, of minor 0, then the host's
// Welcome, of minor 0 too, and snapshot).
bool joinRaw(Peer& host, RawGuest& raw, P::GameSnapshot& snap) {
    Peer nobody;
    host.dm.host(hostOptions(300, 0, 1, "Alice"));
    if (!waitUntil(host, nobody, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; })) return false;
    DirectInvite inv = host.dm.invite();
    P::Hello hello;
    hello.proto = P::kProtocolVersion;
    hello.client = "Scacelith test";
    hello.token = "direct:Raw      ";
    P::Welcome w;
    return raw.connect(inv.port, inv.code) && raw.send(hello) && raw.waitFor(w, 5000) && w.minor == 0 && raw.waitFor(snap, 5000);
}

// Copies the log while it lives (logx writes every line to this file too, flushed at once). One
// file per process: test runs at the same time never share it.
struct LogCapture {
    std::string path;
    LogCapture() {
#ifdef _WIN32
        const unsigned pid = unsigned(_getpid());
#else
        const unsigned pid = unsigned(getpid());
#endif
        path = "/tmp/scacelith_direct_log_test_" + std::to_string(pid) + ".txt";
        logx::init(path.c_str());
    }
    ~LogCapture() {
        logx::shutdown();
        std::remove(path.c_str());
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;
    // Lines written so far that contain 'text'.
    int count(const char* text) const {
        int n = 0;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            char line[4300];
            while (std::fgets(line, sizeof line, f)) n += std::strstr(line, text) != nullptr;
            std::fclose(f);
        }
        return n;
    }
};

// Milliseconds the host takes to answer its own Resync with a snapshot.
double hostResyncMs(Peer& host) {
    Peer nobody;
    host.drain();
    const int before = host.count(Event::Kind::GameSnapshot);
    auto t0 = std::chrono::steady_clock::now();
    host.dm.requestResync();
    waitUntil(host, nobody, [&] { return host.count(Event::Kind::GameSnapshot) > before; }, 3000);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

TEST(direct_guest_drops_a_flooding_host) {
    // A host that sends faster than the guest's game thread polls cannot grow its event queue
    // (every event holds the whole game) without limit: the guest drops the link and comes back.
    RawHost raw;
    CHECK(raw.listen());
    direct::Authority auth(direct::AuthorityConfig(), "Alice", "Bob", 1);
    direct::Authority::Output out;
    auth.startGame(sock::epochMs(), out);
    CHECK(out.toGuest.size() == 1);
    if (out.toGuest.size() != 1) return;
    Peer guest;
    guest.dm.join("127.0.0.1", raw.port, raw.code, "Bob");
    P::Hello hello;
    CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
    CHECK(sendWelcome(raw, auth) && raw.sendBytes(out.toGuest[0]));
    // 6000 Errors while the game thread polls nothing (the link may drop meanwhile).
    P::Error e;
    e.code = P::ErrorCode::NotYourTurn;
    e.game = auth.gameId();
    std::vector<uint8_t> err;
    P::encode(e, err);
    for (int i = 0; i < 6000 && raw.sendBytes(err); ++i) {}
    CHECK(raw.waitClosed(3000));
    CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));   // the guest comes back
    guest.drain();
    CHECK(guest.count(Event::Kind::ServerError) > 1000);
    CHECK(guest.count(Event::Kind::ServerError) <= 4096);
    CHECK(guest.hasConn(ConnState::Reconnecting));
}

TEST(direct_host_refuses_without_stalling) {
    // A refused connection gets its Error, then the host half-closes it and waits up to 300 ms
    // (200 ms after a flood) for the peer to close. A peer that keeps it open no longer holds the
    // host's loop for that time: the host's own commands are handled meanwhile.
    Peer host;
    RawGuest guest;
    P::GameSnapshot snap;
    CHECK(joinRaw(host, guest, snap));
    // Another connection with the code and an incompatible Hello, never closed by its side.
    RawGuest other;
    CHECK(other.connect(host.dm.invite().port, host.dm.invite().code));
    P::Hello hello;
    hello.proto = uint16_t(P::kProtocolVersion + 1);
    hello.token = "direct:Other    ";
    CHECK(other.send(hello));
    P::Error e;
    CHECK(other.waitFor(e, 5000));
    CHECK(e.code == P::ErrorCode::UnsupportedProtocol && e.fatal);
    CHECK(hostResyncMs(host) < 100);
    CHECK(other.waitClosed(2000));
    // The guest floods: Error{Flood} and its link closes, the same way.
    P::C_Ping ping;
    for (uint32_t i = 0; i < 50; ++i) {
        ping.nonce = i;
        CHECK(guest.send(ping));
    }
    CHECK(guest.waitFor(e, 5000));
    CHECK(e.code == P::ErrorCode::Flood && e.fatal);
    CHECK(hostResyncMs(host) < 100);
    CHECK(guest.waitClosed(2000));
    CHECK(host.dm.currentGame() && !host.dm.currentGame()->blackConnected);
}

TEST(direct_host_takes_later_minor_hello) {
    // A guest of a later minor: its Hello announces unknown caps and carries a field the host does
    // not know. The host takes it and answers with the minor and the caps both sides speak.
    Peer host, nobody;
    host.dm.host(hostOptions(300, 0, 1, "Alice"));
    CHECK(waitUntil(host, nobody, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; }));
    DirectInvite inv = host.dm.invite();
    RawGuest raw;
    CHECK(raw.connect(inv.port, inv.code));
    P::Hello hello;
    hello.seq = ++raw.seq;
    hello.proto = P::kProtocolVersion;
    hello.minor = uint16_t(P::kMinor + 2);
    hello.caps = ~uint64_t(0);
    hello.client = "Scacelith later";
    hello.token = "direct:Later    ";
    std::vector<uint8_t> buf;
    P::encode(hello, buf);
    buf.push_back(7);   // the field of that minor
    CHECK(raw.sendBytes(buf));
    P::Welcome w;
    CHECK(raw.waitFor(w, 5000));
    CHECK(w.proto == P::kProtocolVersion && w.minor == P::kMinor && w.caps == P::kCaps);
    P::GameSnapshot s;
    CHECK(raw.waitFor(s, 5000));
    CHECK(s.game != 0 && s.game == w.activeGame);
    CHECK_EQ(raw.errors, 0);
}

TEST(direct_frame_for_minor) {
    // Minor 1 added EndReason::ResignationVsInsufficient: the host gives a guest of minor 0 the
    // Resignation it knows, with the Draw status (PROTOCOL.md "Minors"); nothing else changes.
    P::GameEnd end;
    end.game = 7;
    end.gseq = 3;
    end.status = P::GameStatus::Draw;
    end.reason = P::EndReason::ResignationVsInsufficient;
    end.whiteMs = 1000;
    end.blackMs = 2000;
    end.serverTime = 1.79e12;
    std::vector<uint8_t> frame, older;
    P::encode(end, frame);
    CHECK(direct::frameForMinor(frame.data(), frame.size(), 0, older));
    CHECK_EQ(older.size(), frame.size());
    P::GameEnd e;
    CHECK(P::decode(older.data(), older.size(), e));
    CHECK(e.reason == P::EndReason::Resignation && e.status == P::GameStatus::Draw);
    CHECK(e.game == end.game && e.gseq == end.gseq && e.whiteMs == end.whiteMs && e.blackMs == end.blackMs &&
          e.serverTime == end.serverTime);
    CHECK(!direct::frameForMinor(frame.data(), frame.size(), 1, older));
    CHECK(!direct::frameForMinor(frame.data(), frame.size(), P::kMinor, older));
    CHECK(!direct::frameForMinor(frame.data(), frame.size() - 1, 0, older));   // truncated
    CHECK(!direct::frameForMinor(nullptr, 0, 0, older));
    P::GameSnapshot snap;
    snap.game = 7;
    snap.category = "custom";
    snap.white.name = "Alice";
    snap.black.name = "Bob";
    snap.moves.push_back({796, 0, 0});
    snap.status = P::GameStatus::Draw;
    snap.reason = P::EndReason::ResignationVsInsufficient;
    frame.clear();
    P::encode(snap, frame);
    CHECK(direct::frameForMinor(frame.data(), frame.size(), 0, older));
    P::GameSnapshot s;
    CHECK(P::decode(older.data(), older.size(), s));
    CHECK(s.reason == P::EndReason::Resignation && s.status == P::GameStatus::Draw);
    CHECK(s.game == 7 && s.category == "custom" && s.white.name == "Alice" && s.black.name == "Bob" && s.moves.size() == 1);
    // Every other ending, and every other message, suits a guest of minor 0 as it is.
    for (P::EndReason r : {P::EndReason::Resignation, P::EndReason::TimeoutVsInsufficient, P::EndReason::AbandonmentVsInsufficient}) {
        end.reason = r;
        frame.clear();
        P::encode(end, frame);
        CHECK(!direct::frameForMinor(frame.data(), frame.size(), 0, older));
    }
    P::S_Pong pong;
    frame.clear();
    P::encode(pong, frame);
    CHECK(!direct::frameForMinor(frame.data(), frame.size(), 0, older));
}

TEST(direct_host_speaks_minor_0_to_a_guest_of_minor_0) {
    // A guest of minor 0 (a release before EndReason::ResignationVsInsufficient) gets a Welcome
    // of minor 0, and its games' endings in the values that minor knows.
    Peer host;
    RawGuest raw;
    P::GameSnapshot snap;
    CHECK(joinRaw(host, raw, snap));   // its Hello announces minor 0
    P::Resign r;
    r.game = snap.game;
    CHECK(raw.send(r));
    P::GameEnd end;
    CHECK(raw.waitFor(end, 5000));
    CHECK(end.game == snap.game && end.reason == P::EndReason::Resignation);
    CHECK(end.status == (snap.you == P::Color::White ? P::GameStatus::BlackWins : P::GameStatus::WhiteWins));
    CHECK_EQ(raw.errors, 0);
}

TEST(direct_guest_message_out_of_sequence_logged_once) {
    // Guest messages out of sequence are dropped and the link stays; the first one is logged,
    // not each of them (every log line is written and flushed to the log file at once).
    Peer host;
    RawGuest raw;
    P::GameSnapshot snap;
    CHECK(joinRaw(host, raw, snap));
    LogCapture log;
    P::C_Ping ping;
    std::vector<uint8_t> stale;
    for (uint32_t i = 0; i < 200; ++i) {
        ping.seq = 1;
        ping.nonce = i;
        stale.clear();
        P::encode(ping, stale);
        CHECK(raw.sendBytes(stale));
    }
    ping.nonce = 4242;
    CHECK(raw.send(ping));
    P::S_Pong pong;
    CHECK(raw.waitFor(pong, 5000));
    CHECK_EQ(pong.nonce, 4242u);
    CHECK_EQ(raw.errors, 0);
    CHECK_EQ(log.count("dropped (expected"), 1);
}

TEST(direct_guest_message_of_unknown_type) {
    // A guest message, in sequence, whose type id the schema does not define: always
    // Error{Malformed} (also right after a Gesture), and the link stays.
    Peer host;
    RawGuest raw;
    P::GameSnapshot snap;
    CHECK(joinRaw(host, raw, snap));
    P::C_Gesture g;
    g.game = snap.game;
    for (uint8_t type : {uint8_t(0x04), uint8_t(0x17), uint8_t(0x7F)}) {
        CHECK(raw.send(g));
        const uint32_t seq = ++raw.seq;
        std::vector<uint8_t> msg = {type, uint8_t(seq), uint8_t(seq >> 8), uint8_t(seq >> 16), uint8_t(seq >> 24)};
        CHECK(raw.sendBytes(msg));
        P::Error e;
        CHECK(raw.waitFor(e, 5000));
        CHECK(e.code == P::ErrorCode::Malformed && e.ref == seq && !e.fatal);
    }
    P::C_Ping ping;
    ping.nonce = 7;
    CHECK(raw.send(ping));
    P::S_Pong pong;
    CHECK(raw.waitFor(pong, 5000));
    CHECK_EQ(pong.nonce, 7u);
}

TEST(direct_connection_log_paced) {
    // Anyone who finds the open port can connect as fast as they like (here: 50 connections that
    // send garbage, each refused at once): at most 10 "connection from" lines a second, then the
    // count of the others. The guest still gets in.
    Peer host, guest;
    host.dm.host(hostOptions(300, 0, 1, "Alice"));
    CHECK(waitUntil(host, guest, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; }));
    DirectInvite inv = host.dm.invite();
    LogCapture log;
    sock::Endpoint ep;
    CHECK(sock::Endpoint::parse("127.0.0.1", inv.port, ep));
    static const char junk[] = "GET / HTTP/1.1\r\n\r\n";
    int refused = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) {
        RawChannel c;
        std::string err;
        c.h = sock::connectWithTimeout(ep, 2000, err);
        sock::sendSome(c.h, reinterpret_cast<const uint8_t*>(junk), sizeof junk - 1);
        refused += c.waitClosed(2000);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK_EQ(refused, 50);
    guest.dm.join("127.0.0.1", inv.port, inv.code, "Bob");
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameSnapshot) && guest.count(Event::Kind::GameSnapshot); }));
    const int lines = log.count("direct: connection from");
    CHECK(lines >= 10 && lines <= 10 * (int(seconds) + 1) + 1);
    CHECK(waitUntil(host, guest, [&] { return log.count("more connections") > 0; }, 4000));
}

TEST(direct_guest_retries_once_without_host_confirmation) {
    // On a reconnection the code is known to be right: a connection closed before the host's
    // confirmation (the link failing at that moment) is tried once more and the guest is back. A
    // second one during the same reconnection means the host refuses the code (it hosts another
    // match): it gives up.
    RawHost raw;
    CHECK(raw.listen());
    direct::Authority auth(direct::AuthorityConfig(), "Alice", "Bob", 1);
    direct::Authority::Output out;
    auth.startGame(sock::epochMs(), out);
    CHECK(out.toGuest.size() == 1);
    if (out.toGuest.size() != 1) return;
    Peer guest, nobody;
    auto conns = [&](ConnState s) {
        int n = 0;
        for (auto& e : guest.events) n += e.kind == Event::Kind::ConnectionChanged && e.state == s;
        return n;
    };
    guest.dm.join("127.0.0.1", raw.port, raw.code, "Bob");
    P::Hello hello;
    CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
    CHECK(sendWelcome(raw, auth) && raw.sendBytes(out.toGuest[0]));
    CHECK(waitUntil(guest, nobody, [&] { return guest.count(Event::Kind::GameSnapshot) == 1; }));
    raw.drop();
    CHECK(waitUntil(guest, nobody, [&] { return conns(ConnState::Reconnecting) == 1; }));
    CHECK(raw.acceptUnconfirmed(5000));
    CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
    CHECK(sendWelcome(raw, auth));
    CHECK(waitUntil(guest, nobody, [&] { return conns(ConnState::Online) == 2; }));
    raw.drop();
    CHECK(waitUntil(guest, nobody, [&] { return conns(ConnState::Reconnecting) == 2; }));
    CHECK(raw.acceptUnconfirmed(5000));
    CHECK(raw.acceptUnconfirmed(5000));
    CHECK(waitUntil(guest, nobody, [&] { return guest.count(Event::Kind::GameEnd) == 1; }));
    CHECK_EQ(guest.dm.lastError(), std::string("host_left"));
    const Event* end = guest.last(Event::Kind::GameEnd);
    CHECK(end && end->game.reason == int(P::EndReason::ServerAborted));
}

// ---- stances (protocol minor 2) -----------------------------------------------------------------

namespace {

// The OpponentStance values a peer got, in order.
std::vector<int> stancesOf(const Peer& p) {
    std::vector<int> v;
    for (const Event& e : p.events)
        if (e.kind == Event::Kind::OpponentStance) v.push_back(e.stance);
    return v;
}

// Hosts a match and joins it with a guest written by hand whose Hello announces 'minor'; the
// host's Welcome must answer that minor (the lower of the two).
bool joinRawMinor(Peer& host, RawGuest& raw, uint16_t minor, P::GameSnapshot& snap) {
    Peer nobody;
    host.dm.host(hostOptions(300, 0, 1, "Alice"));
    if (!waitUntil(host, nobody, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; })) return false;
    DirectInvite inv = host.dm.invite();
    P::Hello hello;
    hello.proto = P::kProtocolVersion;
    hello.minor = minor;
    hello.client = "Scacelith test";
    hello.token = "direct:Raw      ";
    P::Welcome w;
    return raw.connect(inv.port, inv.code) && raw.send(hello) && raw.waitFor(w, 5000) && w.minor == minor && raw.waitFor(snap, 5000);
}

// The Welcome of a host written by hand, of protocol minor 'minor' and keepalive 'gestureIdleMs'.
bool sendWelcomeMinor(RawHost& raw, const direct::Authority& auth, uint16_t minor, uint16_t gestureIdleMs) {
    P::Welcome w;
    w.proto = P::kProtocolVersion;
    w.minor = minor;
    w.serverTime = sock::epochMs();
    w.userId = 2;
    w.username = "Bob";
    w.serverName = "Alice";
    w.heartbeatMs = 2000;
    w.clientPingMs = 2000;
    w.maxMsgPerSec = 40;
    w.activeGame = auth.gameId();
    w.gestureRate = 10;
    w.gestureBurst = 20;
    w.gestureIdleMs = gestureIdleMs;
    return raw.send(w);
}

}  // namespace

TEST(direct_loopback_stances) {
    // Host and guest of minor 2: each side's stance reaches the other as an OpponentStance of the
    // game (in order, 'game' not filled in), at once and then every keepalive (1 s) while not
    // Seated, a return to Seated once. The authority never sees them: no Error comes back. A
    // standing stance goes again both ways once the link is back after a failure; once the game
    // is over nothing goes.
    Peer host, guest;
    host.dm.host(hostOptions(300, 0, 1, "Alice"));
    CHECK(waitUntil(host, guest, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; }));
    DirectInvite inv = host.dm.invite();
    Relay relay;
    CHECK(relay.start(inv.port));
    guest.dm.join("127.0.0.1", relay.port, inv.code, "Bob");
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameSnapshot) && guest.count(Event::Kind::GameSnapshot); }));
    REQUIRE(guest.dm.currentGame() && host.dm.currentGame());
    const uint64_t id = guest.dm.currentGame()->id;
    CHECK_EQ(host.dm.currentGame()->id, id);

    host.dm.sendStance(0);   // Seated: nothing to say
    guest.dm.sendStance(0);
    host.dm.sendStance(1);
    CHECK(waitUntil(host, guest, [&] { return guest.count(Event::Kind::OpponentStance) == 1; }));
    const Event* e = guest.last(Event::Kind::OpponentStance);
    CHECK(e && e->gameId == id && e->game.id == 0 && e->stance == 1);
    guest.dm.sendStance(2);
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::OpponentStance) == 1; }));
    e = host.last(Event::Kind::OpponentStance);
    CHECK(e && e->gameId == id && e->game.id == 0 && e->stance == 2);

    // Refreshed every second while standing, both ways (the stance given every frame meanwhile).
    auto t0 = std::chrono::steady_clock::now();
    CHECK(waitUntil(host, guest, [&] {
        host.dm.sendStance(1);
        guest.dm.sendStance(2);
        return guest.count(Event::Kind::OpponentStance) >= 3 && host.count(Event::Kind::OpponentStance) >= 3;
    }, 4000));
    const double span = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(span > 1500.0 && span < 3500.0);
    CHECK(stancesOf(guest) == std::vector<int>({1, 1, 1}));
    CHECK(stancesOf(host) == std::vector<int>({2, 2, 2}));

    // Back to Seated: once.
    host.dm.sendStance(0);
    guest.dm.sendStance(0);
    CHECK(waitUntil(host, guest, [&] { return stancesOf(guest).back() == 0 && stancesOf(host).back() == 0; }));
    const size_t hn = stancesOf(host).size(), gn = stancesOf(guest).size();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    host.drain();
    guest.drain();
    CHECK_EQ(stancesOf(host).size(), hn);
    CHECK_EQ(stancesOf(guest).size(), gn);
    CHECK_EQ(host.count(Event::Kind::ServerError), 0);
    CHECK_EQ(guest.count(Event::Kind::ServerError), 0);

    // Both standing when the network fails: once the guest is back, each stance goes again.
    host.dm.sendStance(3);
    guest.dm.sendStance(1);
    CHECK(waitUntil(host, guest, [&] { return stancesOf(guest).back() == 3 && stancesOf(host).back() == 1; }));
    relay.cut();
    CHECK(waitUntil(host, guest, [&] { return guest.hasConn(ConnState::Reconnecting); }));
    // The stance 'value' among the events after the last one 'mark' accepts.
    auto stanceAfter = [](const Peer& p, Event::Kind mark, int value) {
        size_t from = p.events.size();
        for (size_t i = 0; i < p.events.size(); ++i)
            if (p.events[i].kind == mark && (mark != Event::Kind::GameEvent ||
                                             p.events[i].gameEventKind == int(P::GameEventKind::PlayerReconnected)))
                from = i;
        for (size_t i = from; i < p.events.size(); ++i)
            if (p.events[i].kind == Event::Kind::OpponentStance && p.events[i].stance == value) return true;
        return false;
    };
    // The host hears the guest's after the guest is back (PlayerReconnected), the guest the host's
    // after the snapshot of its return.
    CHECK(waitUntil(host, guest, [&] {
        return guest.count(Event::Kind::GameSnapshot) == 2 && stanceAfter(host, Event::Kind::GameEvent, 1) &&
               stanceAfter(guest, Event::Kind::GameSnapshot, 3);
    }, 15000));
    CHECK_EQ(host.count(Event::Kind::ServerError), 0);
    CHECK_EQ(guest.count(Event::Kind::ServerError), 0);

    // The game is over: nothing more, either way.
    host.dm.resign();
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameEnd) && guest.count(Event::Kind::GameEnd); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    host.drain();
    guest.drain();
    const size_t hEnd = stancesOf(host).size(), gEnd = stancesOf(guest).size();
    host.dm.sendStance(2);
    guest.dm.sendStance(3);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    host.drain();
    guest.drain();
    CHECK_EQ(stancesOf(host).size(), hEnd);
    CHECK_EQ(stancesOf(guest).size(), gEnd);
}

TEST(direct_host_stance_to_guests_by_minor) {
    // A guest of minor 0 or 1 gets no Stance from the host (its codec has no such message), and
    // one it sends anyway is ignored without an Error. A guest of minor 2 gets the host's at once
    // and every keepalive; its own C_Stance becomes an OpponentStance, taken before the authority
    // (never answered), unless malformed or for another game; C_Stance counts towards the flood
    // limit like any message.
    for (uint16_t minor : {uint16_t(0), uint16_t(1)}) {
        Peer host;
        RawGuest raw;
        P::GameSnapshot snap;
        REQUIRE(joinRawMinor(host, raw, minor, snap));
        host.drain();
        host.dm.sendStance(1);
        P::S_Stance got;
        CHECK(!raw.waitFor(got, 1500));
        P::C_Stance mine;
        mine.game = snap.game;
        mine.stance = P::Stance::SideLeft;
        CHECK(raw.send(mine));
        P::C_Ping ping;
        ping.nonce = 31;
        CHECK(raw.send(ping));
        P::S_Pong pong;
        CHECK(raw.waitFor(pong, 5000));
        CHECK_EQ(raw.errors, 0);
        host.drain();
        CHECK_EQ(host.count(Event::Kind::OpponentStance), 0);
    }

    Peer host;
    RawGuest raw;
    P::GameSnapshot snap;
    REQUIRE(joinRawMinor(host, raw, 2, snap));
    host.drain();
    host.dm.sendStance(2);
    P::S_Stance got;
    CHECK(raw.waitFor(got, 2000));
    CHECK(got.game == snap.game && got.stance == P::Stance::SideLeft);
    auto t0 = std::chrono::steady_clock::now();
    CHECK(raw.waitFor(got, 2000));   // the refresh
    const double gap = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(gap > 700.0 && gap < 1500.0);
    CHECK(got.game == snap.game && got.stance == P::Stance::SideLeft);

    P::C_Stance s;
    s.game = snap.game;
    s.stance = P::Stance::SideRight;
    CHECK(raw.send(s));
    s.stance = P::Stance(9);   // malformed: a value of no minor of this codec
    CHECK(raw.send(s));
    s.stance = P::Stance::Standing;
    s.game = snap.game + 1;    // another game
    CHECK(raw.send(s));
    P::C_Ping ping;
    ping.nonce = 32;
    CHECK(raw.send(ping));
    P::S_Pong pong;
    CHECK(raw.waitFor(pong, 5000));
    CHECK_EQ(raw.errors, 0);   // never answered
    host.drain();
    CHECK(stancesOf(host) == std::vector<int>({3}));
    const Event* e = host.last(Event::Kind::OpponentStance);
    CHECK(e && e->gameId == snap.game);
    CHECK(host.dm.currentGame() && host.dm.currentGame()->moves.empty() && host.dm.currentGame()->blackConnected);

    // Fifty at once: the flood limit closes the link, as for any message.
    s.game = snap.game;
    for (int i = 0; i < 50; ++i) {
        s.stance = P::Stance(1 + i % 3);
        raw.send(s);   // the last ones may find the link closed
    }
    P::Error err;
    CHECK(raw.waitFor(err, 5000));
    CHECK(err.code == P::ErrorCode::Flood && err.fatal);
}

TEST(direct_guest_stance_to_hosts_by_minor) {
    // A host of minor 1 gets no Stance from the guest (it would answer the unknown message with an
    // Error). Back with a host of minor 2 (its Welcome after a reconnection), a stance other than
    // Seated goes at once, before any keepalive (10 s here); the host's S_Stance becomes an
    // OpponentStance with its value as it came, for the game shown only.
    RawHost raw;
    REQUIRE(raw.listen());
    direct::Authority auth(direct::AuthorityConfig(), "Alice", "Bob", 1);
    direct::Authority::Output out;
    auth.startGame(sock::epochMs(), out);
    REQUIRE(out.toGuest.size() == 1);
    Peer guest, nobody;
    guest.dm.join("127.0.0.1", raw.port, raw.code, "Bob");
    P::Hello hello;
    CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
    CHECK_EQ(int(hello.minor), int(P::kMinor));
    CHECK(sendWelcomeMinor(raw, auth, 1, 10000) && raw.sendBytes(out.toGuest[0]));
    CHECK(waitUntil(guest, nobody, [&] { return guest.count(Event::Kind::GameSnapshot) == 1; }));
    guest.dm.sendStance(1);
    P::C_Stance got;
    CHECK(!raw.waitFor(got, 1500));
    CHECK_EQ(raw.errors, 0);

    raw.drop();   // the link fails; the guest comes back to a host of minor 2
    CHECK(waitUntil(guest, nobody, [&] { return guest.hasConn(ConnState::Reconnecting); }));
    CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
    auto t0 = std::chrono::steady_clock::now();
    CHECK(sendWelcomeMinor(raw, auth, 2, 10000) && raw.sendBytes(out.toGuest[0]));
    CHECK(raw.waitFor(got, 2000));
    const double after = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(after < 1000.0);
    CHECK(got.game == auth.gameId() && got.stance == P::Stance::Standing && got.seq > 1);

    P::S_Stance s;
    s.game = auth.gameId();
    s.stance = P::Stance::SideLeft;
    CHECK(raw.send(s));
    s.stance = P::Stance(6);   // a later minor's
    CHECK(raw.send(s));
    s.game = auth.gameId() + 1;
    s.stance = P::Stance::Standing;
    CHECK(raw.send(s));
    CHECK(waitUntil(guest, nobody, [&] { return guest.count(Event::Kind::OpponentStance) == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    guest.drain();
    CHECK(stancesOf(guest) == std::vector<int>({2, 6}));
    const Event* e = guest.last(Event::Kind::OpponentStance);
    CHECK(e && e->gameId == auth.gameId() && e->game.id == 0);
}
