// Small portable socket layer for the direct match and the UPnP client (Winsock2 on Windows,
// POSIX sockets elsewhere). Internal to src/net: the platform headers stay in socket_util.cpp,
// addresses travel as an opaque Endpoint.
//
// Every socket used by the direct match is non-blocking and driven by PollSet (select() on
// Windows, poll() elsewhere), so a worker thread never waits longer than the deadline it chose;
// nothing here runs on the game thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace net {
namespace sock {

#ifdef _WIN32
using Handle = uintptr_t;            // SOCKET
constexpr Handle kInvalid = ~Handle(0);
#else
using Handle = int;
constexpr Handle kInvalid = -1;
#endif

bool startup();                      // WSAStartup once per process (no-op elsewhere); thread-safe
void closeSocket(Handle h);
bool setNonBlocking(Handle h);
int lastError();                     // WSAGetLastError() / errno
// Short stable name of a socket error for lastError() strings: "refused", "timeout",
// "unreachable", "reset", "in_use", "network".
std::string errorName(int err);

// An IPv4 or IPv6 socket address (sockaddr_storage inside).
struct Endpoint {
    alignas(8) uint8_t storage[128] = {};
    int len = 0;
    int family() const;              // AF_INET / AF_INET6 (0 when empty)
    bool isV4() const;
    bool isV6() const;
    uint16_t port() const;
    void setPort(uint16_t p);
    std::string ip() const;          // "192.168.1.2", "2001:db8::1" (IPv4-mapped IPv6 shown as IPv4)
    std::string toString() const;    // "192.168.1.2:47100", "[2001:db8::1]:47100"
    // Numeric address only (no DNS); IPv6 without brackets.
    static bool parse(const std::string& ip, uint16_t port, Endpoint& out);
    static Endpoint anyV4(uint16_t port);
    static Endpoint anyV6(uint16_t port);
};

// getaddrinfo (blocking: worker threads only). Numeric addresses resolve at once.
bool resolve(const std::string& host, uint16_t port, bool tcp, std::vector<Endpoint>& out);

Handle openTcp(int family);          // non-blocking
Handle openUdpV4();                  // non-blocking
bool bindTo(Handle h, const Endpoint& ep, bool reuseAddr);
bool localEndpoint(Handle h, Endpoint& out);

// Listening socket on 'port' (0 = any): IPv6 dual-stack when the system allows it, IPv4
// otherwise. err = errorName() of the failure ("in_use" when the port is taken).
Handle listenTcp(uint16_t port, bool& dualStack, std::string& err);
// Listening socket on 127.0.0.1 only, on a port the system picks (returned in 'port'): the
// loopback redirect of the Google sign-in (net/loopback_redirect.h). Never another interface (no
// firewall prompt, nothing reachable from the LAN), exclusive on Windows, and never inherited by
// the processes the game starts (the browser). err = errorName() of the failure.
Handle listenLoopbackV4(uint16_t& port, std::string& err);
// kInvalid when nothing is pending. noInherit: the accepted socket is not inherited by child
// processes either (the sockets of listenLoopbackV4).
Handle acceptOne(Handle listener, Endpoint* peer, bool noInherit = false);

// Non-blocking connect: 1 = connected, 0 = in progress (wait for writability, then
// connectResult), -1 = failed (err set).
int connectStart(Handle h, const Endpoint& ep, int& err);
int connectResult(Handle h);         // 0 = connected, otherwise the socket error
// Blocking-with-deadline connect for simple request/response clients (UPnP HTTP).
Handle connectWithTimeout(const Endpoint& ep, int timeoutMs, std::string& err);

// >0 bytes moved, 0 = would block, -1 = error or (recv) orderly close: see closed.
int sendSome(Handle h, const uint8_t* p, size_t n);
int recvSome(Handle h, uint8_t* p, size_t n, bool& closed);
int sendTo(Handle h, const uint8_t* p, size_t n, const Endpoint& to);
int recvFrom(Handle h, uint8_t* p, size_t n, Endpoint& from);
void shutdownSend(Handle h);

// UDP multicast sending options (SSDP): outgoing interface and TTL.
bool setMulticastIf(Handle h, const Endpoint& localV4);
bool setMulticastTtl(Handle h, int ttl);

// select() over a handful of sockets.
class PollSet {
public:
    void clear();
    void add(Handle h, bool read, bool write);
    // Waits up to timeoutMs (<0: forever). Returns the number of ready sockets, 0 on timeout,
    // -1 on error.
    int wait(int timeoutMs);
    bool readable(Handle h) const;
    bool writable(Handle h) const;   // also true for a failed non-blocking connect
private:
    struct Item { Handle h; bool r, w; bool rr = false, ww = false; };
    std::vector<Item> items_;
};

// Wakes a thread blocked in PollSet::wait from another thread (a loopback UDP socket that
// sends to itself; works on Windows where select() only takes sockets).
class Waker {
public:
    Waker();
    ~Waker();
    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;
    bool valid() const { return h_ != kInvalid; }
    Handle handle() const { return h_; }
    void wake();
    void drain();
private:
    Handle h_ = kInvalid;
};

// The machine's usable unicast addresses (up interfaces, no loopback, no link-local):
// IPv4 first, then global/unique-local IPv6.
std::vector<std::string> localAddresses(bool ipv4, bool ipv6);

// Clocks. steadyMs: monotonic. epochMs: wall clock in ms since 1970, but advancing with the
// monotonic clock after its first reading (never jumps with system time changes).
int64_t steadyMs();
double epochMs();

}  // namespace sock
}  // namespace net
