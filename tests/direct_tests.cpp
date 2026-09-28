// Direct match: UPnP client against a fake gateway on 127.0.0.1, the secure channel (vectors
// and failure cases) and full loopback matches between two DirectMatch instances.
#include "test.h"
#include "net/direct_crypto.h"
#include "net/socket_util.h"
#include "net/upnp.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

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
    // What the client did.
    std::vector<std::string> searchTargets;
    struct Add { int extPort = 0, intPort = 0, lease = -1; std::string client, desc, proto; };
    std::vector<Add> adds;
    std::vector<int> deletes;
    std::vector<std::string> soapPaths;
    int getIpCalls = 0;

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
    std::string host, path;
    uint16_t port = 0;
    CHECK(upnp::splitHttpUrl("http://192.168.1.1:5000/ctl/IPConn", host, port, path));
    CHECK(host == "192.168.1.1" && port == 5000 && path == "/ctl/IPConn");
    CHECK(upnp::splitHttpUrl("HTTP://10.0.0.1", host, port, path) && port == 80 && path == "/");
    CHECK(!upnp::splitHttpUrl("https://192.168.1.1/x", host, port, path));
    CHECK(!upnp::splitHttpUrl("http://router.lan/x", host, port, path));   // IPv4 literals only
    CHECK(!upnp::splitHttpUrl("http://192.168.1.1:99999/x", host, port, path));

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
        gh[4] = 2;   // a future version
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
