// UPnP Internet Gateway Device client (IGD v1 and v2): asks the home router to forward a TCP
// port to this machine for the direct match (src/net/direct_match.h).
//
//   discover()          SSDP M-SEARCH (UDP to 239.255.255.250:1900, MX 2, ST
//                       InternetGatewayDevice:1, :2 and WANIPConnection:1, sent from every IPv4
//                       interface), then HTTP GET of each answering device's description (16
//                       at most) and a scan for its WAN connection service (WANIPConnection:2,
//                       :1, then WANPPPConnection:1) and control URL.
//   getExternalIp()     SOAP GetExternalIPAddress.
//   addPortMapping()    SOAP AddPortMapping (NewRemoteHost empty = any remote, TCP).
//   mapPort()           the mapping policy: lease 3600 s, a permanent lease (0) after error 725
//                       OnlyPermanentLeasesSupported, the next port after 718
//                       ConflictInMappingEntry, at most 10 attempts.
//   deletePortMapping() SOAP DeletePortMapping.
//
// Every call blocks the calling thread (a DirectMatch worker, never the game thread) for a
// bounded time: discovery <= Config::discoveryMs, each HTTP exchange <= Config::httpTimeoutMs,
// responses <= Config::maxHttpBytes. A Config::cancel flag stops a call early, except an
// AddPortMapping already sent, whose answer is awaited (mapPort() then reports the mapping made).
//
// Trust: SSDP answers can come from any machine of the LAN. A description is only fetched from
// the address that answered, over plain HTTP to an IPv4 literal, and a control URL must point to
// that same host; nothing else is ever contacted.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace net {
namespace upnp {

constexpr uint32_t kDefaultLeaseSec = 3600;
constexpr int kMaxMappingTries = 10;
constexpr const char* kMappingDescription = "Scacelith direct match";
constexpr int64_t kRenewEveryMs = 30 * 60 * 1000;
constexpr int64_t kRenewRetryMs = 60 * 1000;   // after a failed renewal, until the lease has ended

struct Config {
    // Where the M-SEARCH goes: the SSDP multicast group, or a unicast address (tests point it at a
    // fake gateway on 127.0.0.1).
    std::string discoveryAddress = "239.255.255.250";
    uint16_t discoveryPort = 1900;
    int discoveryMs = 2500;           // longest wait for SSDP answers
    int settleMs = 300;               // after the first answer, wait this long for other devices
    int httpTimeoutMs = 3000;         // per HTTP exchange (connect + request + response)
    size_t maxHttpBytes = 256 * 1024; // larger responses are refused
    const std::atomic<bool>* cancel = nullptr;
};

struct SsdpResponse {
    std::string location, server, usn;
    std::string from;                 // IP address of the device that answered
};

struct Service {
    std::string serviceType, controlUrl;   // as written in the description
};

struct Description {
    std::string urlBase, deviceType, friendlyName;   // of the root device
    std::vector<Service> services;                    // every <service> of every embedded device
};

struct Gateway {
    std::string server, friendlyName;
    std::string serviceType;          // "urn:schemas-upnp-org:service:WANIPConnection:1"...
    std::string controlUrl;           // absolute "http://192.168.1.1:5000/ctl/IPConn"
    std::string host;                 // parsed control URL
    uint16_t port = 0;
    std::string path;
    std::string localAddress;         // our IPv4 address on the route to the gateway (NewInternalClient)
    std::string externalIp;           // from GetExternalIPAddress during discover(), when it answered
};

struct Error {
    int httpStatus = 0;               // 0 = no HTTP answer
    int upnpCode = 0;                 // UPnPError errorCode from the SOAP fault (718, 725...)
    std::string text;                 // "725 OnlyPermanentLeasesSupported", "timeout", "no_gateway"...
};

struct Mapping {
    uint16_t externalPort = 0, internalPort = 0;
    uint32_t leaseSec = kDefaultLeaseSec;   // 0 = permanent (until deleted or the router restarts)
    std::string internalClient;       // empty = Gateway::localAddress
    std::string description = kMappingDescription;
};

class Client {
public:
    explicit Client(const Config& cfg = Config());

    // Finds the router and its WAN connection service. err.text "no_gateway" when nothing answered.
    bool discover(Gateway& out, Error& err);
    bool getExternalIp(const Gateway& gw, std::string& ip, Error& err);
    bool addPortMapping(const Gateway& gw, const Mapping& m, Error& err);
    bool deletePortMapping(const Gateway& gw, uint16_t externalPort, Error& err);
    // Maps 'port' (already bound locally by the caller) with the policy above. On a 718 conflict
    // the next port is tried, and claimLocalPort(p) is called first so the caller can move its
    // listening socket there (return false when p is not free locally: p is skipped). The
    // internal and external ports are always equal.
    bool mapPort(const Gateway& gw, uint16_t port, const std::function<bool(uint16_t)>& claimLocalPort, Mapping& out,
                 Error& err);
    // Renews a mapping before its lease ends (same AddPortMapping; a no-op for permanent leases).
    bool renew(const Gateway& gw, const Mapping& m, Error& err);

private:
    Config cfg_;
    bool soap(const Gateway& gw, const char* action, const std::vector<std::pair<std::string, std::string>>& args,
              std::string& body, Error& err, bool finishOnceSent = false);
    bool cancelled() const { return cfg_.cancel && cfg_.cancel->load(); }
};

// When a mapping with a lease is renewed (the caller sends the renewals and reports their
// results): every kRenewEveryMs; after a failed renewal, every kRenewRetryMs until the lease has
// ended (the last retry may come just after it). A lease that ends without a renewal is lost
// until a later renewal succeeds; a permanent lease (0) never ends. Times in milliseconds of a
// steady clock.
class RenewalSchedule {
public:
    void mapped(int64_t now, uint32_t leaseSec);   // the mapping was made at 'now'
    void started(int64_t now);                     // a renewal was sent at 'now'
    // Its result, at 'now'. True when it maps the port again after the lease was lost.
    bool finished(int64_t now, bool ok);
    // True once, at the first call after the lease has ended without a renewal.
    bool lapsed(int64_t now);
    bool lost() const { return lost_; }
    int64_t renewAt() const { return renewAt_; }

private:
    uint32_t leaseSec_ = 0;
    int64_t renewAt_ = 0, start_ = 0, leaseEnd_ = 0;
    bool lost_ = false;
};

// ---- parsing helpers (exposed for the tests) ----
bool parseSsdpResponse(const std::string& text, SsdpResponse& out);    // "HTTP/1.1 200 OK" + LOCATION
bool parseDescription(const std::string& xml, Description& out);       // false when not a UPnP device description
bool parseSoapFault(const std::string& xml, int& code, std::string& description);
// Text of the first element whose local name (namespace prefix ignored) is 'name'.
bool xmlFind(const std::string& xml, const std::string& name, std::string& text);
// Resolves a controlURL against URLBase or the description location.
std::string resolveUrl(const std::string& base, const std::string& ref);
// "http://host[:port]/path" with an IPv4 literal host.
bool splitHttpUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path);
// The best WAN connection service of a description (nullptr when none).
const Service* pickService(const Description& d);
// True when an external IPv4 address cannot be reached from the Internet: private (RFC 1918),
// shared carrier-grade NAT space 100.64.0.0/10, link-local, loopback, unspecified, the IETF
// protocol assignments 192.0.0.0/24 (DS-Lite's B4 side), benchmarking 198.18.0.0/15, multicast
// and reserved (224.0.0.0 and above).
bool cgnatSuspected(const std::string& externalIpv4);

}  // namespace upnp
}  // namespace net
