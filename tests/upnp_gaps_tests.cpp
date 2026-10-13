// UPnP client (net/upnp.h), the paths tests/direct_tests.cpp leaves out: XML entities read and
// written (a mapping description with the five special characters), control URLs with a bad port,
// renewals of a permanent and of a timed lease, the router's answers that end the exchange
// (an HTTP error without a SOAP fault, a body shorter than its Content-Length, a head that never
// ends), a discovery address that is not IPv4, SSDP answers pointing to another machine, and a
// router whose only WAN connection does not tell its external address. The router is a loopback
// server and an SSDP responder on 127.0.0.1; nothing goes to the network.
#include "test.h"
#include "net_gaps_helpers.h"
#include "net/socket_util.h"
#include "net/upnp.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace net;

namespace {

const char kService[] = "urn:schemas-upnp-org:service:WANIPConnection:1";

std::string soapEnvelope(const std::string& inner) {
    return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>" + inner +
           "</s:Body></s:Envelope>";
}

std::string soapFault(int code, const char* text) {
    return soapEnvelope("<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
                        "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>" +
                        std::to_string(code) + "</errorCode><errorDescription>" + text +
                        "</errorDescription></UPnPError></detail></s:Fault>");
}

// The router's HTTP side: every request is recorded (head and body), the answer is 'reply' as it
// stands (whatever bytes the test wants), then the connection closes. GET /desc.xml gets
// 'description' instead when it is set.
struct Router {
    std::mutex m;
    std::string reply;
    std::string description;
    std::vector<std::string> heads, bodies;
    gaps::TcpServer server{[this](gaps::Conn& c) {
        std::string head, body;
        if (!c.readHead(head) || !gaps::readBody(c, head, body)) return;
        std::string answer;
        {
            std::lock_guard<std::mutex> lk(m);
            heads.push_back(head);
            bodies.push_back(body);
            answer = !description.empty() && gaps::requestPath(head) == "/desc.xml"
                         ? gaps::httpAnswer(200, description, std::string(), "text/xml")
                         : reply;
        }
        c.sendAll(answer);
    }};

    void answer(const std::string& r) {
        std::lock_guard<std::mutex> lk(m);
        reply = r;
    }
    size_t requests() {
        std::lock_guard<std::mutex> lk(m);
        return heads.size();
    }
    std::string lastBody() {
        std::lock_guard<std::mutex> lk(m);
        return bodies.empty() ? std::string() : bodies.back();
    }
    std::string lastHead() {
        std::lock_guard<std::mutex> lk(m);
        return heads.empty() ? std::string() : heads.back();
    }
    upnp::Gateway gateway() {
        upnp::Gateway g;
        g.serviceType = kService;
        g.host = "127.0.0.1";
        g.port = server.port();
        g.path = "/ctl/IPConn";
        g.controlUrl = "http://127.0.0.1:" + std::to_string(server.port()) + g.path;
        g.localAddress = "127.0.0.1";
        return g;
    }
};

upnp::Config quickConfig() {
    upnp::Config c;
    c.httpTimeoutMs = 3000;
    return c;
}

// An SSDP responder on 127.0.0.1: every M-SEARCH gets one answer with the LOCATION it is given.
struct Ssdp {
    sock::Handle udp = sock::kInvalid;
    uint16_t port = 0;
    std::string location;
    std::atomic<int> searches{0};
    std::atomic<bool> stop{false};
    std::thread th;

    explicit Ssdp(std::string loc) : location(std::move(loc)) {
        udp = sock::openUdpV4();
        sock::Endpoint lo, ep;
        if (udp == sock::kInvalid || !sock::Endpoint::parse("127.0.0.1", 0, lo) || !sock::bindTo(udp, lo, false) ||
            !sock::localEndpoint(udp, ep))
            return;
        port = ep.port();
        th = std::thread([this] { run(); });
    }
    ~Ssdp() {
        stop = true;
        if (th.joinable()) th.join();
        sock::closeSocket(udp);
    }
    Ssdp(const Ssdp&) = delete;
    Ssdp& operator=(const Ssdp&) = delete;

    upnp::Config config(int discoveryMs) const {
        upnp::Config c = quickConfig();
        c.discoveryAddress = "127.0.0.1";
        c.discoveryPort = port;
        c.discoveryMs = discoveryMs;
        c.settleMs = 50;
        return c;
    }

    void run() {
        while (!stop) {
            sock::PollSet ps;
            ps.add(udp, true, false);
            if (ps.wait(50) <= 0) continue;
            uint8_t buf[2048];
            sock::Endpoint from;
            int r;
            while ((r = sock::recvFrom(udp, buf, sizeof buf, from)) > 0) {
                if (std::string(reinterpret_cast<char*>(buf), size_t(r)).compare(0, 8, "M-SEARCH") != 0) continue;
                ++searches;
                const std::string resp = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: " + std::string(kService) +
                                         "\r\nUSN: uuid:gaps-igd\r\nSERVER: Gaps/1.0 UPnP/1.1\r\nLOCATION: " + location + "\r\n\r\n";
                sock::sendTo(udp, reinterpret_cast<const uint8_t*>(resp.data()), resp.size(), from);
            }
        }
    }
};

}  // namespace

TEST(upnp_gaps_xml_entities_and_urls) {
    // Entities: the five named ones, decimal and hexadecimal character references of ASCII
    // (others dropped), an unknown entity kept as it is, an '&' too far from its ';' or without one.
    std::string text;
    CHECK(upnp::xmlFind("<a>&lt;&gt;&quot;&apos;&amp;&#65;&#x42;&#X43;&#233;&#0;&nbsp;</a>", "a", text));
    CHECK_EQ(text, std::string("<>\"'&ABC&nbsp;"));
    CHECK(upnp::xmlFind("<x:b>&averyveryverylongname; R&D &</x:b>", "b", text));
    CHECK_EQ(text, std::string("&averyveryverylongname; R&D &"));
    // Control URLs: the port must be 1 to 5 digits, 1..65535.
    std::string host, path;
    uint16_t port = 0;
    CHECK(upnp::splitHttpUrl("http://192.168.1.1:5000/ctl", host, port, path) && port == 5000 && path == "/ctl");
    CHECK(upnp::splitHttpUrl("http://192.168.1.1/ctl", host, port, path) && port == 80);
    for (const char* bad : {"http://192.168.1.1:/ctl", "http://192.168.1.1:123456/ctl", "http://192.168.1.1:50a0/ctl",
                            "http://192.168.1.1:0/ctl", "http://192.168.1.1:70000/ctl"})
        CHECK(!upnp::splitHttpUrl(bad, host, port, path));
}

TEST(upnp_gaps_mapping_description_escaped_and_renewals) {
    Router router;
    REQUIRE(router.server.ok());
    router.answer(gaps::httpAnswer(200, soapEnvelope("<u:AddPortMappingResponse xmlns:u=\"" + std::string(kService) + "\"/>"),
                                   std::string(), "text/xml"));
    upnp::Client client(quickConfig());
    upnp::Gateway gw = router.gateway();
    upnp::Mapping mp;
    mp.externalPort = mp.internalPort = 47100;
    mp.description = "Bob's \"A&B\" <game>";
    upnp::Error err;
    CHECK(client.addPortMapping(gw, mp, err));
    const std::string body = router.lastBody();
    CHECK(body.find("<NewPortMappingDescription>Bob&apos;s &quot;A&amp;B&quot; &lt;game&gt;</NewPortMappingDescription>") !=
          std::string::npos);
    CHECK(body.find("<NewInternalClient>127.0.0.1</NewInternalClient>") != std::string::npos);
    std::string desc;
    CHECK(upnp::xmlFind(body, "NewPortMappingDescription", desc) && desc == mp.description);   // read back as written
    CHECK(router.lastHead().find("SOAPAction: \"" + std::string(kService) + "#AddPortMapping\"") != std::string::npos);
    // A permanent lease is never renewed: no request at all.
    const size_t before = router.requests();
    mp.leaseSec = 0;
    CHECK(client.renew(gw, mp, err));
    CHECK_EQ(router.requests(), before);
    // A timed lease is renewed with the same AddPortMapping.
    mp.leaseSec = 3600;
    CHECK(client.renew(gw, mp, err));
    CHECK_EQ(router.requests(), before + 1);
    CHECK(router.lastBody().find("<NewLeaseDuration>3600</NewLeaseDuration>") != std::string::npos);
    // A renewal the router refuses says why.
    router.answer(gaps::httpAnswer(500, soapFault(501, "ActionFailed"), std::string(), "text/xml"));
    CHECK(!client.renew(gw, mp, err));
    CHECK_EQ(err.upnpCode, 501);
    CHECK_EQ(err.text, std::string("501 ActionFailed"));
}

TEST(upnp_gaps_router_answers_that_end_the_exchange) {
    Router router;
    REQUIRE(router.server.ok());
    upnp::Client client(quickConfig());
    upnp::Gateway gw = router.gateway();
    std::string ip;
    upnp::Error err;
    // An HTTP error without a SOAP fault: its status.
    router.answer(gaps::httpAnswer(404, "no such control URL", std::string(), "text/plain"));
    CHECK(!client.getExternalIp(gw, ip, err));
    CHECK_EQ(err.text, std::string("http 404"));
    CHECK_EQ(err.httpStatus, 404);
    CHECK_EQ(err.upnpCode, 0);
    // A body shorter than its Content-Length, then the close.
    router.answer("HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: 400\r\n\r\n<?xml version=\"1.0\"?><s:Env");
    CHECK(!client.getExternalIp(gw, ip, err));
    CHECK_EQ(err.text, std::string("truncated"));
    // A chunked body cut before its last chunk.
    router.answer("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n<?xml\r\n");
    CHECK(!client.getExternalIp(gw, ip, err));
    CHECK_EQ(err.text, std::string("truncated"));
    // A head that never ends before the close.
    router.answer("HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\n");
    CHECK(!client.getExternalIp(gw, ip, err));
    CHECK_EQ(err.text, std::string("bad_http"));
    // Something else than HTTP.
    router.answer("SSH-2.0-OpenSSH_9.6\r\nContent-Length: 0\r\n\r\n");
    CHECK(!client.getExternalIp(gw, ip, err));
    CHECK_EQ(err.text, std::string("bad_http"));
    // A deletion the router accepts.
    router.answer(gaps::httpAnswer(200, soapEnvelope("<u:DeletePortMappingResponse xmlns:u=\"" + std::string(kService) + "\"/>"),
                                   std::string(), "text/xml"));
    CHECK(client.deletePortMapping(gw, 47100, err));
    CHECK(router.lastBody().find("<NewExternalPort>47100</NewExternalPort>") != std::string::npos);
}

TEST(upnp_gaps_discovery_address_not_ipv4) {
    for (const char* address : {"gateway.home.arpa", "::1", ""}) {
        upnp::Config c = quickConfig();
        c.discoveryAddress = address;
        upnp::Client client(c);
        upnp::Gateway gw;
        upnp::Error err;
        CHECK(!client.discover(gw, err));
        CHECK_EQ(err.text, std::string("bad_discovery_address"));
    }
}

TEST(upnp_gaps_answer_pointing_elsewhere_ignored) {
    // An SSDP answer whose LOCATION is on another machine than the one that answered is never
    // followed: nothing usable answered.
    Ssdp ssdp("http://192.0.2.1:5000/desc.xml");
    REQUIRE(ssdp.port != 0);
    upnp::Client client(ssdp.config(400));
    upnp::Gateway gw;
    upnp::Error err;
    CHECK(!client.discover(gw, err));
    CHECK_EQ(err.text, std::string("no_gateway"));
    CHECK(ssdp.searches.load() >= 3);   // the three search targets (asked again when nothing usable came)
}

TEST(upnp_gaps_gateway_without_external_address) {
    // The only WAN connection does not tell its external address: discover() still returns it,
    // with an empty externalIp.
    Router router;
    REQUIRE(router.server.ok());
    const std::string base = "http://127.0.0.1:" + std::to_string(router.server.port());
    router.description = "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device>"
                         "<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>"
                         "<friendlyName>Gaps &amp; Router</friendlyName><serviceList><service>"
                         "<serviceType>" + std::string(kService) + "</serviceType><controlURL>/ctl/IPConn</controlURL>"
                         "</service></serviceList></device></root>";
    router.answer(gaps::httpAnswer(500, soapFault(501, "ActionFailed"), std::string(), "text/xml"));
    Ssdp ssdp(base + "/desc.xml");
    REQUIRE(ssdp.port != 0);
    upnp::Client client(ssdp.config(1500));
    upnp::Gateway gw;
    upnp::Error err;
    CHECK(client.discover(gw, err));
    CHECK_EQ(gw.controlUrl, base + "/ctl/IPConn");
    CHECK_EQ(gw.friendlyName, std::string("Gaps & Router"));
    CHECK_EQ(gw.serviceType, std::string(kService));
    CHECK(gw.externalIp.empty());
    CHECK_EQ(gw.localAddress, std::string("127.0.0.1"));
    std::string ip;
    CHECK(!client.getExternalIp(gw, ip, err));
    CHECK_EQ(err.upnpCode, 501);
}
