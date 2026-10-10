#include "socket_util.h"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>

namespace net {
namespace sock {

#ifdef _WIN32
using socklen = int;
static SOCKET S(Handle h) { return SOCKET(h); }
#else
using socklen = socklen_t;
static int S(Handle h) { return h; }
#endif

#ifdef __APPLE__
// macOS has no MSG_NOSIGNAL: every socket made here gets SO_NOSIGPIPE instead (noSigPipe), so that
// a send to a peer that has gone fails with EPIPE rather than raise SIGPIPE. Nor has it
// SOCK_CLOEXEC or accept4: close-on-exec is set right after (the programs the game starts inherit
// no descriptor anyway: net::sys::spawnProgram).
static constexpr int kSendNoSignal = 0;
static void noSigPipe(int h) {
    int on = 1;
    setsockopt(h, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
}
static void closeOnExec(int h) { fcntl(h, F_SETFD, FD_CLOEXEC); }
#elif !defined(_WIN32)
static constexpr int kSendNoSignal = MSG_NOSIGNAL;
#endif

bool startup() {
#ifdef _WIN32
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wsa;
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    });
    return ok;
#else
    return true;
#endif
}

void closeSocket(Handle h) {
    if (h == kInvalid) return;
#ifdef _WIN32
    closesocket(S(h));
#else
    ::close(h);
#endif
}

bool setNonBlocking(Handle h) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(S(h), FIONBIO, &on) == 0;
#else
    int fl = fcntl(h, F_GETFL, 0);
    return fl >= 0 && fcntl(h, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}

int lastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static bool isWouldBlock(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

static bool isInProgress(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN;
#endif
}

std::string errorName(int e) {
#ifdef _WIN32
    switch (e) {
    case WSAECONNREFUSED: return "refused";
    case WSAETIMEDOUT: return "timeout";
    case WSAEHOSTUNREACH: case WSAENETUNREACH: case WSAENETDOWN: case WSAEADDRNOTAVAIL: return "unreachable";
    case WSAECONNRESET: case WSAECONNABORTED: case WSAENETRESET: return "reset";
    case WSAEADDRINUSE: case WSAEACCES: return "in_use";
    default: return "network";
    }
#else
    switch (e) {
    case ECONNREFUSED: return "refused";
    case ETIMEDOUT: return "timeout";
    case EHOSTUNREACH: case ENETUNREACH: case ENETDOWN: case EADDRNOTAVAIL: return "unreachable";
    case ECONNRESET: case ECONNABORTED: case EPIPE: return "reset";
    case EADDRINUSE: case EACCES: return "in_use";
    default: return "network";
    }
#endif
}

// ---- Endpoint -----------------------------------------------------------------------------------

static const sockaddr* SA(const Endpoint& e) { return reinterpret_cast<const sockaddr*>(e.storage); }
static sockaddr* SA(Endpoint& e) { return reinterpret_cast<sockaddr*>(e.storage); }

int Endpoint::family() const { return len ? SA(*this)->sa_family : 0; }
bool Endpoint::isV4() const { return family() == AF_INET; }
bool Endpoint::isV6() const { return family() == AF_INET6; }

uint16_t Endpoint::port() const {
    if (isV4()) return ntohs(reinterpret_cast<const sockaddr_in*>(storage)->sin_port);
    if (isV6()) return ntohs(reinterpret_cast<const sockaddr_in6*>(storage)->sin6_port);
    return 0;
}

void Endpoint::setPort(uint16_t p) {
    if (isV4()) reinterpret_cast<sockaddr_in*>(storage)->sin_port = htons(p);
    else if (isV6()) reinterpret_cast<sockaddr_in6*>(storage)->sin6_port = htons(p);
}

std::string Endpoint::ip() const {
    char buf[INET6_ADDRSTRLEN + 1] = {};
    if (isV4()) {
        inet_ntop(AF_INET, (void*)&reinterpret_cast<const sockaddr_in*>(storage)->sin_addr, buf, sizeof buf);
    } else if (isV6()) {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(storage);
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&a6->sin6_addr);
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (std::memcmp(b, mapped, 12) == 0) {
            in_addr v4;
            std::memcpy(&v4, b + 12, 4);
            inet_ntop(AF_INET, (void*)&v4, buf, sizeof buf);
        } else {
            inet_ntop(AF_INET6, (void*)&a6->sin6_addr, buf, sizeof buf);
        }
    }
    return buf;
}

std::string Endpoint::toString() const {
    std::string a = ip();
    if (a.find(':') != std::string::npos) return "[" + a + "]:" + std::to_string(port());
    return a + ":" + std::to_string(port());
}

bool Endpoint::parse(const std::string& ip, uint16_t port, Endpoint& out) {
    startup();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(ip.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    bool ok = false;
    if (res->ai_addrlen <= sizeof(out.storage)) {
        out = Endpoint();
        std::memcpy(out.storage, res->ai_addr, res->ai_addrlen);
        out.len = int(res->ai_addrlen);
        out.setPort(port);
        ok = true;
    }
    freeaddrinfo(res);
    return ok;
}

Endpoint Endpoint::anyV4(uint16_t port) {
    Endpoint e;
    auto* a = reinterpret_cast<sockaddr_in*>(e.storage);
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = htonl(INADDR_ANY);
    a->sin_port = htons(port);
    e.len = sizeof(sockaddr_in);
    return e;
}

Endpoint Endpoint::anyV6(uint16_t port) {
    Endpoint e;
    auto* a = reinterpret_cast<sockaddr_in6*>(e.storage);
    a->sin6_family = AF_INET6;
    a->sin6_addr = in6addr_any;
    a->sin6_port = htons(port);
    e.len = sizeof(sockaddr_in6);
    return e;
}

// ---- name resolution ----------------------------------------------------------------------------

namespace {

std::atomic<Lookup::Fn> g_lookupForTests{nullptr};
// Lookup threads still running (their callers may have given up on them).
constexpr int kMaxLookupThreads = 16;
std::atomic<int> g_lookupThreads{0};

bool systemLookup(const std::string& host, uint16_t port, bool tcp, std::vector<Endpoint>& out, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = tcp ? SOCK_STREAM : SOCK_DGRAM;
    addrinfo* res = nullptr;
    int e = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (e != 0) {
#ifdef _WIN32
        error = "getaddrinfo " + std::to_string(e);   // gai_strerror is not thread-safe there
#else
        error = gai_strerror(e);
#endif
        return false;
    }
    for (addrinfo* r = res; r; r = r->ai_next) {
        if ((r->ai_family != AF_INET && r->ai_family != AF_INET6) || r->ai_addrlen > sizeof(Endpoint::storage)) continue;
        Endpoint ep;
        std::memcpy(ep.storage, r->ai_addr, r->ai_addrlen);
        ep.len = int(r->ai_addrlen);
        ep.setPort(port);
        bool dup = false;
        for (auto& o : out) dup = dup || (o.len == ep.len && std::memcmp(o.storage, ep.storage, size_t(ep.len)) == 0);
        if (!dup) out.push_back(ep);
    }
    freeaddrinfo(res);
    if (out.empty()) error = "no IPv4 or IPv6 address";
    return !out.empty();
}

}  // namespace

// What the caller and the lookup's thread share (each holds a reference).
struct Lookup::State {
    std::mutex m;
    std::condition_variable cv;
    bool done = false, found = false, stopped = false;
    std::vector<Endpoint> endpoints;
    std::string error;
};

Lookup::Lookup(const std::string& host, uint16_t port, bool tcp) : s_(std::make_shared<State>()) {
    startup();
    Endpoint numeric;
    if (Endpoint::parse(host, port, numeric)) {
        s_->endpoints.push_back(numeric);
        s_->done = s_->found = true;
        return;
    }
    Fn fn = g_lookupForTests.load();
    if (!fn) fn = systemLookup;
    // A resolver that never answers keeps each thread it was given: past kMaxLookupThreads still
    // running, a new lookup fails at once instead of adding one more.
    if (g_lookupThreads.fetch_add(1) >= kMaxLookupThreads) {
        g_lookupThreads.fetch_sub(1);
        s_->error = "too many name lookups still running";
        s_->done = true;
        return;
    }
    std::shared_ptr<State> s = s_;
    try {
        std::thread([s, fn, host, port, tcp] {
            std::vector<Endpoint> out;
            std::string error;
            const bool found = fn(host, port, tcp, out, error);
            g_lookupThreads.fetch_sub(1);
            {
                std::lock_guard<std::mutex> lk(s->m);
                s->found = found;
                s->endpoints = std::move(out);
                s->error = std::move(error);
                s->done = true;
            }
            s->cv.notify_all();
        }).detach();
    } catch (const std::system_error&) {
        g_lookupThreads.fetch_sub(1);
        s_->error = "no thread for the lookup";
        s_->done = true;
    }
}

Lookup::~Lookup() = default;

Lookup::Result Lookup::wait(int timeoutMs, const std::function<bool()>& stopped) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);
    std::unique_lock<std::mutex> lk(s_->m);
    for (;;) {
        if (s_->done) return s_->found ? Result::Found : Result::NotFound;
        if (s_->stopped || (stopped && stopped())) return Result::Stopped;
        const auto now = std::chrono::steady_clock::now();
        if (now >= end) return Result::TimedOut;
        s_->cv.wait_until(lk, stopped ? std::min(end, now + std::chrono::milliseconds(20)) : end);
    }
}

void Lookup::stop() {
    {
        std::lock_guard<std::mutex> lk(s_->m);
        s_->stopped = true;
    }
    s_->cv.notify_all();
}

const std::vector<Endpoint>& Lookup::endpoints() const { return s_->endpoints; }

std::string Lookup::error() const {
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->error;
}

void Lookup::setForTests(Fn fn) { g_lookupForTests.store(fn); }

// ---- sockets ------------------------------------------------------------------------------------

static void setNoDelay(Handle h) {
    int on = 1;
    setsockopt(S(h), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

Handle openTcp(int family) {
    startup();
    Handle h = Handle(socket(family, SOCK_STREAM, IPPROTO_TCP));
    if (h == kInvalid) return kInvalid;
#ifdef __APPLE__
    noSigPipe(h);
#endif
    if (!setNonBlocking(h)) { closeSocket(h); return kInvalid; }
    setNoDelay(h);
    return h;
}

Handle openUdpV4() {
    startup();
    Handle h = Handle(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (h == kInvalid) return kInvalid;
#ifdef __APPLE__
    noSigPipe(h);
#endif
    if (!setNonBlocking(h)) { closeSocket(h); return kInvalid; }
    return h;
}

bool bindTo(Handle h, const Endpoint& ep, bool reuseAddr) {
#ifdef _WIN32
    // Windows: default semantics. SO_REUSEADDR would let another socket steal the port, and
    // SO_EXCLUSIVEADDRUSE would keep it unusable while old connections sit in TIME_WAIT (hosting
    // again right after a game would fail).
    (void)reuseAddr;
#else
    if (reuseAddr) {
        int on = 1;
        setsockopt(h, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    }
#endif
    return ::bind(S(h), SA(ep), socklen(ep.len)) == 0;
}

bool localEndpoint(Handle h, Endpoint& out) {
    out = Endpoint();
    socklen l = socklen(sizeof out.storage);
    if (getsockname(S(h), SA(out), &l) != 0) return false;
    out.len = int(l);
    return true;
}

Handle listenTcp(uint16_t port, bool& dualStack, std::string& err) {
    startup();
    dualStack = false;
    err.clear();
    // IPv6 dual-stack first: one socket for both families.
    Handle h = openTcp(AF_INET6);
    if (h != kInvalid) {
        int off = 0;
        if (setsockopt(S(h), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off) == 0 &&
            bindTo(h, Endpoint::anyV6(port), true) && ::listen(S(h), 8) == 0) {
            dualStack = true;
            return h;
        }
        err = errorName(lastError());
        closeSocket(h);
    }
    h = openTcp(AF_INET);
    if (h == kInvalid) {
        err = errorName(lastError());
        return kInvalid;
    }
    if (!bindTo(h, Endpoint::anyV4(port), true) || ::listen(S(h), 8) != 0) {
        err = errorName(lastError());
        closeSocket(h);
        return kInvalid;
    }
    err.clear();
    return h;
}

Handle listenLoopbackV4(uint16_t& port, std::string& err) {
    startup();
    port = 0;
    err.clear();
#ifdef _WIN32
    // Not inherited from its creation (Windows 7 SP1 and later); SetHandleInformation as well, in
    // case a layered service provider ignores the flag (best effort, like SO_EXCLUSIVEADDRUSE:
    // safe here, unlike the hosting socket of bindTo, because the port is a fresh one).
    Handle h = Handle(WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
    if (h == kInvalid) { err = errorName(lastError()); return kInvalid; }
    SetHandleInformation(reinterpret_cast<HANDLE>(h), HANDLE_FLAG_INHERIT, 0);
    BOOL on = TRUE;
    setsockopt(S(h), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
#elif defined(__APPLE__)
    // Close-on-exec as soon as it is made (and spawnProgram hands no descriptor to the browser).
    Handle h = Handle(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (h == kInvalid) { err = errorName(lastError()); return kInvalid; }
    closeOnExec(h);
    noSigPipe(h);
#else
    // Close-on-exec from its creation: posix_spawnp (net::sys) would hand it to the browser.
    Handle h = Handle(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
    if (h == kInvalid) { err = errorName(lastError()); return kInvalid; }
#endif
    Endpoint lo, local;
    if (!setNonBlocking(h) || !Endpoint::parse("127.0.0.1", 0, lo) || ::bind(S(h), SA(lo), socklen(lo.len)) != 0 ||
        ::listen(S(h), 8) != 0 || !localEndpoint(h, local) || local.port() == 0) {
        err = errorName(lastError());
        closeSocket(h);
        return kInvalid;
    }
    setNoDelay(h);
    port = local.port();
    return h;
}

Handle acceptOne(Handle listener, Endpoint* peer, bool noInherit) {
    Endpoint tmp;
    socklen l = socklen(sizeof tmp.storage);
#ifdef _WIN32
    Handle h = Handle(::accept(S(listener), SA(tmp), &l));
    if (h != kInvalid && noInherit) SetHandleInformation(reinterpret_cast<HANDLE>(h), HANDLE_FLAG_INHERIT, 0);
#elif defined(__APPLE__)
    Handle h = Handle(::accept(listener, SA(tmp), &l));
    if (h != kInvalid) {
        if (noInherit) closeOnExec(h);
        noSigPipe(h);
    }
#else
    Handle h = Handle(::accept4(listener, SA(tmp), &l, noInherit ? SOCK_CLOEXEC : 0));
#endif
    if (h == kInvalid) return kInvalid;
    tmp.len = int(l);
    if (!setNonBlocking(h)) { closeSocket(h); return kInvalid; }
    setNoDelay(h);
    if (peer) *peer = tmp;
    return h;
}

int connectStart(Handle h, const Endpoint& ep, int& err) {
    err = 0;
    if (::connect(S(h), SA(ep), socklen(ep.len)) == 0) return 1;
    int e = lastError();
    if (isInProgress(e)) return 0;
    err = e;
    return -1;
}

int connectResult(Handle h) {
    int e = 0;
    socklen l = sizeof e;
    if (getsockopt(S(h), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &l) != 0) return lastError();
    return e;
}

Handle connectWithTimeout(const Endpoint& ep, int timeoutMs, std::string& err, const std::atomic<bool>* cancel) {
    err.clear();
    Handle h = openTcp(ep.family());
    if (h == kInvalid) { err = errorName(lastError()); return kInvalid; }
    int e = 0;
    int r = connectStart(h, ep, e);
    if (r < 0) { err = errorName(e); closeSocket(h); return kInvalid; }
    if (r == 0) {
        int64_t deadline = steadyMs() + timeoutMs;
        for (;;) {
            if (cancel && cancel->load()) { err = "cancelled"; closeSocket(h); return kInvalid; }
            int64_t left = deadline - steadyMs();
            if (left <= 0) { err = "timeout"; closeSocket(h); return kInvalid; }
            PollSet ps;
            ps.add(h, false, true);
            int n = ps.wait(int(cancel ? std::min<int64_t>(left, 100) : left));
            if (n < 0) { err = "network"; closeSocket(h); return kInvalid; }
            if (n > 0 && ps.writable(h)) break;
        }
        e = connectResult(h);
        if (e != 0) { err = errorName(e); closeSocket(h); return kInvalid; }
    }
    return h;
}

int sendSome(Handle h, const uint8_t* p, size_t n) {
#ifdef _WIN32
    int r = ::send(S(h), reinterpret_cast<const char*>(p), int(std::min<size_t>(n, 1 << 20)), 0);
#else
    ssize_t r = ::send(h, p, n, kSendNoSignal);
#endif
    if (r >= 0) return int(r);
    return isWouldBlock(lastError()) ? 0 : -1;
}

int recvSome(Handle h, uint8_t* p, size_t n, bool& closed) {
    closed = false;
#ifdef _WIN32
    int r = ::recv(S(h), reinterpret_cast<char*>(p), int(n), 0);
#else
    ssize_t r = ::recv(h, p, n, 0);
#endif
    if (r > 0) return int(r);
    if (r == 0) { closed = true; return -1; }
    return isWouldBlock(lastError()) ? 0 : -1;
}

int sendTo(Handle h, const uint8_t* p, size_t n, const Endpoint& to) {
#ifdef _WIN32
    int r = ::sendto(S(h), reinterpret_cast<const char*>(p), int(n), 0, SA(to), to.len);
#else
    ssize_t r = ::sendto(h, p, n, kSendNoSignal, SA(to), socklen(to.len));
#endif
    if (r >= 0) return int(r);
    return isWouldBlock(lastError()) ? 0 : -1;
}

int recvFrom(Handle h, uint8_t* p, size_t n, Endpoint& from) {
    from = Endpoint();
    socklen l = socklen(sizeof from.storage);
#ifdef _WIN32
    int r = ::recvfrom(S(h), reinterpret_cast<char*>(p), int(n), 0, SA(from), &l);
#else
    ssize_t r = ::recvfrom(h, p, n, 0, SA(from), &l);
#endif
    if (r >= 0) { from.len = int(l); return int(r); }
    return isWouldBlock(lastError()) ? 0 : -1;
}

void shutdownSend(Handle h) {
#ifdef _WIN32
    ::shutdown(S(h), SD_SEND);
#else
    ::shutdown(h, SHUT_WR);
#endif
}

bool setMulticastIf(Handle h, const Endpoint& localV4) {
    if (!localV4.isV4()) return false;
    in_addr a = reinterpret_cast<const sockaddr_in*>(localV4.storage)->sin_addr;
    return setsockopt(S(h), IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&a), sizeof a) == 0;
}

bool setMulticastTtl(Handle h, int ttl) {
#ifdef _WIN32
    DWORD v = DWORD(ttl);
#else
    unsigned char v = (unsigned char)ttl;
#endif
    return setsockopt(S(h), IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&v), sizeof v) == 0;
}

// ---- PollSet ------------------------------------------------------------------------------------

void PollSet::clear() { items_.clear(); }

void PollSet::add(Handle h, bool read, bool write) {
    if (h == kInvalid) return;
    for (auto& it : items_)
        if (it.h == h) { it.r = it.r || read; it.w = it.w || write; return; }
    items_.push_back({h, read, write});
}

int PollSet::wait(int timeoutMs) {
    for (auto& it : items_) it.rr = it.ww = false;
    if (items_.empty()) {
        if (timeoutMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        return 0;
    }
#ifdef _WIN32
    fd_set rs, ws, es;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    FD_ZERO(&es);
    for (auto& it : items_) {
        if (it.r) FD_SET(S(it.h), &rs);
        if (it.w) { FD_SET(S(it.h), &ws); FD_SET(S(it.h), &es); }
    }
    timeval tv{};
    timeval* ptv = nullptr;
    if (timeoutMs >= 0) {
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ptv = &tv;
    }
    int n = ::select(0, &rs, &ws, &es, ptv);   // nfds: ignored by Winsock
    if (n <= 0) return n < 0 ? -1 : 0;
    int ready = 0;
    for (auto& it : items_) {
        it.rr = it.r && FD_ISSET(S(it.h), &rs);
        // Windows reports a failed non-blocking connect in the exception set only.
        it.ww = it.w && (FD_ISSET(S(it.h), &ws) || FD_ISSET(S(it.h), &es));
        if (it.rr || it.ww) ++ready;
    }
    return ready;
#else
    // poll(): FD_SET is undefined for a descriptor of FD_SETSIZE (1024) or more. What select() on
    // Linux reports: the read set is POLLIN, RDNORM, RDBAND, HUP and ERR; the write set is POLLOUT,
    // WRNORM, WRBAND and ERR; the exception set (with the write one, as on Windows) is POLLPRI; a
    // descriptor that is not open fails the whole call (EBADF).
    const short readEvents = POLLIN | POLLRDNORM | POLLRDBAND;
    const short writeEvents = POLLOUT | POLLWRNORM | POLLWRBAND | POLLPRI;
    std::vector<pollfd> fds(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) {
        fds[i].fd = items_[i].h;
        fds[i].events = short((items_[i].r ? readEvents : 0) | (items_[i].w ? writeEvents : 0));
    }
    int n = ::poll(fds.data(), nfds_t(fds.size()), timeoutMs < 0 ? -1 : timeoutMs);
    if (n <= 0) return n < 0 ? -1 : 0;
    for (const pollfd& p : fds)
        if (p.revents & POLLNVAL) return -1;
    int ready = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        Item& it = items_[i];
        const short re = fds[i].revents;
        it.rr = it.r && (re & (readEvents | POLLHUP | POLLERR));
        it.ww = it.w && (re & (writeEvents | POLLERR));
        if (it.rr || it.ww) ++ready;
    }
    return ready;
#endif
}

bool PollSet::readable(Handle h) const {
    for (auto& it : items_)
        if (it.h == h) return it.rr;
    return false;
}

bool PollSet::writable(Handle h) const {
    for (auto& it : items_)
        if (it.h == h) return it.ww;
    return false;
}

// ---- Waker --------------------------------------------------------------------------------------

Waker::Waker() {
    startup();
    Handle h = openUdpV4();
    if (h == kInvalid) return;
    Endpoint lo;
    Endpoint::parse("127.0.0.1", 0, lo);
    Endpoint self;
    if (!bindTo(h, lo, false) || !localEndpoint(h, self) || ::connect(S(h), SA(self), socklen(self.len)) != 0) {
        closeSocket(h);
        return;
    }
    h_ = h;
}

Waker::~Waker() { closeSocket(h_); }

void Waker::wake() {
    if (h_ == kInvalid) return;
    uint8_t b = 1;
    sendSome(h_, &b, 1);
}

void Waker::drain() {
    if (h_ == kInvalid) return;
    uint8_t buf[64];
    bool closed = false;
    for (int i = 0; i < 64; ++i)
        if (recvSome(h_, buf, sizeof buf, closed) <= 0) break;
}

// ---- local addresses ----------------------------------------------------------------------------

static bool usableV4(const uint8_t* a) {
    if (a[0] == 127 || a[0] == 0) return false;          // loopback, "this network"
    if (a[0] == 169 && a[1] == 254) return false;        // link-local
    if (a[0] >= 224) return false;                       // multicast, reserved
    return true;
}

static bool usableV6(const uint8_t* a) {
    if ((a[0] & 0xE0) == 0x20) return true;              // global unicast 2000::/3
    if ((a[0] & 0xFE) == 0xFC) return true;              // unique local fc00::/7
    return false;
}

std::vector<std::string> localAddresses(bool ipv4, bool ipv6) {
    std::vector<std::string> v4, v6;
    auto push = [](std::vector<std::string>& v, const std::string& s) {
        if (!s.empty() && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
    };
#ifdef _WIN32
    startup();
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && r == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.assign(size, 0);
        r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                 reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (r == NO_ERROR) {
        for (auto* ad = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); ad; ad = ad->Next) {
            if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = ad->FirstUnicastAddress; u; u = u->Next) {
                const sockaddr* sa = u->Address.lpSockaddr;
                if (!sa || u->Address.iSockaddrLength > int(sizeof(Endpoint::storage))) continue;
                Endpoint e;
                std::memcpy(e.storage, sa, u->Address.iSockaddrLength);
                e.len = u->Address.iSockaddrLength;
                if (sa->sa_family == AF_INET && ipv4) {
                    if (usableV4(reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in*>(sa)->sin_addr))) push(v4, e.ip());
                } else if (sa->sa_family == AF_INET6 && ipv6) {
                    if (u->DadState != IpDadStatePreferred) continue;
                    if (usableV6(reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr))) push(v6, e.ip());
                }
            }
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            Endpoint e;
            if (i->ifa_addr->sa_family == AF_INET && ipv4) {
                std::memcpy(e.storage, i->ifa_addr, sizeof(sockaddr_in));
                e.len = sizeof(sockaddr_in);
                if (usableV4(reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr))) push(v4, e.ip());
            } else if (i->ifa_addr->sa_family == AF_INET6 && ipv6) {
                std::memcpy(e.storage, i->ifa_addr, sizeof(sockaddr_in6));
                e.len = sizeof(sockaddr_in6);
                if (usableV6(reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in6*>(i->ifa_addr)->sin6_addr))) push(v6, e.ip());
            }
        }
        freeifaddrs(ifs);
    }
#endif
    v4.insert(v4.end(), v6.begin(), v6.end());
    return v4;
}

// ---- clocks -------------------------------------------------------------------------------------

int64_t steadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

double epochMs() {
    using namespace std::chrono;
    static const double base = [] {
        double wall = double(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count()) / 1000.0;
        double mono = double(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
        return wall - mono;
    }();
    return base + double(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
}

}  // namespace sock
}  // namespace net
