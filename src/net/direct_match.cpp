// DirectMatch: one worker thread per hosted or joined match (a Session). The game thread only
// queues commands and drains events; the worker owns every socket, the secure channel, the
// UPnP exchanges and (host) the game authority.
#include "direct_match.h"
#include "direct_authority.h"
#include "direct_crypto.h"
#include "protocol_gen.h"
#include "socket_util.h"
#include "upnp.h"
#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace net {

namespace P = net::proto;
using direct::SecureChannel;

namespace {

constexpr int kConnectMs = 5000;           // per address tried
constexpr int kHandshakeMs = 10000;        // handshake, then Hello -> Welcome
constexpr int kPingEveryMs = 2000;         // both sides measure the round trip
constexpr int kSilenceMs = 10000;          // nothing received for this long: the link is dead
constexpr int kMaxPendingHandshakes = 4;
constexpr size_t kMaxClosing = 4;          // refused or flooding links lingering (beyond, the oldest is closed)
constexpr int kMaxFailedHandshakes = 10;   // wrong codes per hosted game, then the host stops listening
constexpr int kMaxMsgPerSec = 20;          // announced in Welcome; twice as many closes the link (Gestures aside)
constexpr int kGestureRate = 10;           // Gestures per second each way (Welcome.gestureRate)...
constexpr int kGestureBurst = 20;          // ...with bursts up to this many (Welcome.gestureBurst)
constexpr int64_t kRenewEveryMs = 30 * 60 * 1000;
constexpr size_t kMaxOutbox = 1 << 20;
constexpr int64_t kDefaultGraceMs = 60000;
constexpr size_t kMaxQueuedCommands = 32;
constexpr size_t kMaxQueuedEvents = 4096;  // guest: events not polled yet (each holds the whole game)
constexpr const char* kClientString = "Scacelith direct";
constexpr const char* kTokenPrefix = "direct:";
constexpr size_t kTokenMin = 16;
constexpr size_t kMaxRouterName = 64;      // bytes of the router's friendlyName shown on the hosting page

// ---- the game as one player sees it ------------------------------------------------------------

// Rebuilds OnlineGame from the server->client messages and turns them into Events, exactly as
// OnlineClient does with the dedicated server's messages. Used by the guest (messages from the
// channel) and by the host (the authority's messages for the host, decoded locally).
struct ClientView {
    OnlineGame game;
    bool have = false;
    bool needResync = false;

    bool apply(const uint8_t* p, size_t n, Event& ev) {
        P::MsgType t;
        if (!P::peekType(p, n, t)) return false;
        ev = Event();
        switch (t) {
        case P::MsgType::GameSnapshot: {
            P::GameSnapshot s;
            if (!P::decode(p, n, s)) return false;
            game = onlineGameFromSnapshot(s);
            have = true;
            needResync = false;
            ev.kind = Event::Kind::GameSnapshot;
            break;
        }
        case P::MsgType::MoveMade: {
            P::MoveMade m;
            if (!P::decode(p, n, m) || !have || m.game != game.id) return false;
            if (size_t(m.ply) < game.moves.size()) return false;                    // a repeated confirmation
            if (size_t(m.ply) > game.moves.size()) { needResync = true; return false; }
            const int mover = m.ply % 2;
            game.moves.push_back({m.move, m.spentMs, mover == 0 ? m.whiteMs : m.blackMs});
            game.whiteMs = m.whiteMs;
            game.blackMs = m.blackMs;
            game.serverTimeMs = m.serverTime;
            game.running = m.ply + 1 >= 2 ? 1 - mover : 2;
            game.firstMoveMs = m.firstMoveMs;
            if (m.drawOffer) game.drawOfferBy = mover;
            ev.kind = Event::Kind::MoveMade;
            ev.ply = m.ply;
            ev.move = m.move;
            ev.flags = m.flags;
            ev.spentMs = m.spentMs;
            ev.mine = mover == game.you;
            break;
        }
        case P::MsgType::MoveRejected: {
            P::MoveRejected r;
            if (!P::decode(p, n, r)) return false;
            ev.kind = Event::Kind::MoveRejected;
            ev.ply = r.ply;
            ev.move = r.move;
            ev.code = int(r.code);
            ev.gameId = r.game;
            break;
        }
        case P::MsgType::GameEvent: {
            P::GameEvent e;
            if (!P::decode(p, n, e) || !have || e.game != game.id) return false;
            const int color = int(e.color);
            switch (e.kind) {
            case P::GameEventKind::DrawOffered: game.drawOfferBy = color; break;
            case P::GameEventKind::DrawDeclined: game.drawOfferBy = 2; break;
            case P::GameEventKind::PlayerDisconnected:
                (color == 0 ? game.whiteConnected : game.blackConnected) = false;
                game.graceMs = e.arg;
                break;
            case P::GameEventKind::PlayerReconnected: (color == 0 ? game.whiteConnected : game.blackConnected) = true; break;
            case P::GameEventKind::RematchOffered: game.rematchBy = color; break;
            case P::GameEventKind::RematchDeclined: game.rematchBy = 2; break;
            default: break;
            }
            ev.kind = Event::Kind::GameEvent;
            ev.gameEventKind = int(e.kind);
            ev.color = color;
            ev.arg = e.arg;
            break;
        }
        case P::MsgType::GameEnd: {
            P::GameEnd e;
            if (!P::decode(p, n, e) || !have || e.game != game.id) return false;
            game.status = int(e.status);
            game.reason = int(e.reason);
            game.whiteMs = e.whiteMs;
            game.blackMs = e.blackMs;
            game.serverTimeMs = e.serverTime;
            game.running = 2;
            game.drawOfferBy = 2;
            game.firstMoveMs = 0;
            game.rematchBy = 2;
            ev.kind = Event::Kind::GameEnd;
            break;
        }
        case P::MsgType::Error: {
            P::Error e;
            if (!P::decode(p, n, e)) return false;
            ev.kind = Event::Kind::ServerError;
            ev.code = int(e.code);
            ev.fatal = e.fatal;
            ev.gameId = e.game;
            break;
        }
        default:
            return false;
        }
        if (ev.kind != Event::Kind::ServerError && ev.kind != Event::Kind::MoveRejected) ev.gameId = game.id;
        ev.game = game;
        return true;
    }

    // The guest lost the host for good: the running game ends here as ServerAborted.
    bool endLocally(double now, Event& ev) {
        if (!have || game.status != int(P::GameStatus::Ongoing)) return false;
        if (game.running == 0 || game.running == 1) {
            int64_t& ms = game.running == 0 ? game.whiteMs : game.blackMs;
            ms = std::max<int64_t>(0, ms - int64_t(now - game.serverTimeMs));
        }
        game.status = int(P::GameStatus::Aborted);
        game.reason = int(P::EndReason::ServerAborted);
        game.running = 2;
        game.drawOfferBy = 2;
        game.rematchBy = 2;
        game.firstMoveMs = 0;
        game.serverTimeMs = now;
        ev = Event();
        ev.kind = Event::Kind::GameEnd;
        ev.gameId = game.id;
        ev.game = game;
        return true;
    }
};

// ---- commands from the game thread ---------------------------------------------------------------

struct Command {
    enum class Kind { Move, Resign, OfferDraw, AnswerDraw, ClaimDraw, Abort, Resync, Rematch } kind = Kind::Resync;
    uint64_t game = 0;
    uint16_t ply = 0, move = 0;
    uint32_t posHash = 0, thinkMs = 0;
    bool flag = false;   // Move: drawOffer; AnswerDraw / Rematch: accept
};

void encodeCommand(const Command& c, uint32_t seq, std::vector<uint8_t>& out) {
    const uint64_t game = c.game < P::kId53Limit ? c.game : 0;
    switch (c.kind) {
    case Command::Kind::Move: {
        P::Move m;
        m.seq = seq;
        m.game = game;
        m.ply = c.ply;
        m.move = c.move;
        m.posHash = c.posHash;
        m.thinkMs = c.thinkMs;
        m.drawOffer = c.flag;
        P::encode(m, out);
        break;
    }
    case Command::Kind::Resign: { P::Resign m; m.seq = seq; m.game = game; P::encode(m, out); break; }
    case Command::Kind::OfferDraw: { P::DrawOffer m; m.seq = seq; m.game = game; P::encode(m, out); break; }
    case Command::Kind::AnswerDraw: { P::DrawAnswer m; m.seq = seq; m.game = game; m.accept = c.flag; P::encode(m, out); break; }
    case Command::Kind::ClaimDraw: { P::DrawClaim m; m.seq = seq; m.game = game; P::encode(m, out); break; }
    case Command::Kind::Abort: { P::Abort m; m.seq = seq; m.game = game; P::encode(m, out); break; }
    case Command::Kind::Resync: { P::Resync m; m.seq = seq; m.game = game; P::encode(m, out); break; }
    case Command::Kind::Rematch: { P::Rematch m; m.seq = seq; m.game = game; m.accept = c.flag; P::encode(m, out); break; }
    }
}

// The Hello token of a direct match: "direct:" + the guest's name, padded with spaces to the
// schema's 16-byte minimum (the host strips both).
std::string tokenFor(const std::string& name) {
    std::string t = kTokenPrefix + direct::sanitizeName(name, "Guest");
    while (t.size() < kTokenMin) t += ' ';
    return t;
}

std::string nameFromToken(const std::string& token) {
    std::string n = token.compare(0, std::strlen(kTokenPrefix), kTokenPrefix) == 0 ? token.substr(std::strlen(kTokenPrefix)) : token;
    return direct::sanitizeName(n, "Guest");
}

// ---- one TCP connection with its secure channel -------------------------------------------------

struct Conn {
    sock::Handle h = sock::kInvalid;
    std::unique_ptr<SecureChannel> ch;
    int64_t deadline = 0;    // steady ms: end of the handshake / Hello, or of a closeStep() close
    int64_t lastRecv = 0;
    uint32_t lastSeq = 0;    // host: last seq received from the guest
    bool badSeqReported = false;   // host: a guest message out of sequence was logged
    bool peerClosed = false;
    bool halfClosed = false; // closeStep(): the sending side is shut down

    Conn() = default;
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;
    ~Conn() { sock::closeSocket(h); }

    // Reads what is available into the channel. False: the connection is over.
    bool read() {
        uint8_t buf[8192];
        for (int i = 0; i < 64; ++i) {
            bool closed = false;
            int r = sock::recvSome(h, buf, sizeof buf, closed);
            if (r > 0) {
                lastRecv = sock::steadyMs();
                if (!ch->receive(buf, size_t(r))) return false;
                continue;
            }
            if (r == 0) return true;
            peerClosed = closed;
            return false;
        }
        return true;
    }

    bool wantsWrite() const { return ch && !ch->outbox().empty(); }

    bool write() {
        if (!ch) return true;
        auto& ob = ch->outbox();
        while (!ob.empty()) {
            int r = sock::sendSome(h, ob.data(), ob.size());
            if (r < 0) return false;
            if (r == 0) break;
            ob.erase(ob.begin(), ob.begin() + r);
        }
        return ob.size() <= kMaxOutbox;
    }

    bool send(const std::vector<uint8_t>& msg) { return ch && ch->established() && ch->send(msg.data(), msg.size()); }

    // Graceful close: flush the outbox (bounded), half-close, let the peer read and close.
    void flushAndClose(int ms) {
        if (h == sock::kInvalid) return;
        const int64_t end = sock::steadyMs() + ms;
        while (wantsWrite() && sock::steadyMs() < end) {
            sock::PollSet ps;
            ps.add(h, false, true);
            ps.wait(int(std::max<int64_t>(1, end - sock::steadyMs())));
            if (!write()) break;
        }
        sock::shutdownSend(h);
        // Drain until the peer closes (closing with unread data would reset the connection).
        const int64_t lingerEnd = std::min(end, sock::steadyMs() + 300);
        uint8_t buf[4096];
        while (sock::steadyMs() < lingerEnd) {
            sock::PollSet ps;
            ps.add(h, true, false);
            ps.wait(int(std::max<int64_t>(1, lingerEnd - sock::steadyMs())));
            bool closed = false;
            int r = sock::recvSome(h, buf, sizeof buf, closed);
            if (r < 0) break;
        }
        sock::closeSocket(h);
        h = sock::kInvalid;
    }

    // The same close without blocking (the host's loop): called whenever the socket may be ready
    // until it returns true (closed); 'deadline' ends it, like the delay of flushAndClose.
    bool closeStep(int64_t now) {
        if (!halfClosed) {
            if (wantsWrite() && now < deadline && write() && wantsWrite()) return false;
            sock::shutdownSend(h);
            halfClosed = true;
        }
        uint8_t buf[4096];
        for (int i = 0; now < deadline; ++i) {
            bool closed = false;
            int r = sock::recvSome(h, buf, sizeof buf, closed);
            if (r < 0) break;
            if (r == 0 || i == 63) return false;
        }
        sock::closeSocket(h);
        h = sock::kInvalid;
        return true;
    }
};

// ---- Session: shared state between the game thread and one worker ---------------------------------

class Session {
public:
    explicit Session(bool host) : isHost(host) {}
    virtual ~Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start() {
        thread = std::thread([this] {
            run();
            finished = true;
        });
    }
    void requestStop() {
        stopFlag = true;
        waker.wake();
    }
    void join() {
        if (thread.joinable()) thread.join();
    }
    void post(const Command& c) {
        {
            std::lock_guard<std::mutex> lk(m);
            if (commands.size() >= kMaxQueuedCommands) return;
            commands.push_back(c);
        }
        waker.wake();
    }
    bool popEvent(Event& ev) {
        std::lock_guard<std::mutex> lk(m);
        if (events.empty()) return false;
        ev = std::move(events.front());
        events.pop_front();
        return true;
    }
    // The game thread's latest Gesture replaces the one not sent yet; nothing is kept while the
    // link to the other player is down.
    void postGesture(uint64_t game, const Gesture& g) {
        bool wake;
        {
            std::lock_guard<std::mutex> lk(m);
            if (!gestureLink) return;
            wake = !gestureOut.pending;   // one already waiting has woken the worker
            gestureOut.pending = true;
            gestureOut.game = game;
            gestureOut.g = g;
        }
        if (wake) waker.wake();
    }

    // Shared with the game thread (under m).
    mutable std::mutex m;
    DirectMatch::State state = DirectMatch::State::Idle;
    std::string lastError;
    DirectInvite invite;
    UpnpStatus upnpStatus;
    std::deque<Event> events;
    std::deque<Command> commands;
    struct GestureOut { bool pending = false; uint64_t game = 0; Gesture g; } gestureOut;
    bool gestureLink = false;   // the other player is connected (the worker sets it)
    int pingMs = -1;
    double clockOffset = 0;   // guest: host clock - local clock
    const bool isHost;
    std::atomic<bool> stopFlag{false}, finished{false};
    sock::Waker waker;
    std::thread thread;

protected:
    virtual void run() = 0;

    void setState(DirectMatch::State s) {
        std::lock_guard<std::mutex> lk(m);
        state = s;
    }
    void pushEvent(Event ev) {
        std::lock_guard<std::mutex> lk(m);
        events.push_back(std::move(ev));
    }
    size_t queuedEvents() const {
        std::lock_guard<std::mutex> lk(m);
        return events.size();
    }
    // The other player's Gesture: it replaces the one of the same game still waiting to be
    // polled (only the latest state matters), so the queue holds at most one per game.
    void pushGesture(uint64_t game, const Gesture& g) {
        Event ev;
        ev.kind = Event::Kind::OpponentGesture;
        ev.ok = true;
        ev.gameId = game;
        ev.gesture = g;
        std::lock_guard<std::mutex> lk(m);
        for (auto it = events.begin(); it != events.end(); ++it) {
            if (it->kind == Event::Kind::OpponentGesture && it->gameId == game) {
                events.erase(it);
                break;
            }
        }
        events.push_back(std::move(ev));
    }
    // The link to the other player came up (paced for the receiver's bucket of rate / burst) or
    // went down (the Gesture waiting is dropped: none is kept for the reconnection). It comes up
    // before any event that tells the game thread the other player is there (the snapshot,
    // Online), so the first Gesture the game thread sends on that news is never dropped.
    void gestureLinkUp(int rate, int burst) {
        std::lock_guard<std::mutex> lk(m);
        gestureBucket_.reset(double(sock::steadyMs()), rate, gestureSendCapacity(burst));
        gestureOut.pending = false;
        gestureLink = true;
    }
    void gestureLinkDown() {
        std::lock_guard<std::mutex> lk(m);
        gestureOut.pending = false;
        gestureLink = false;
    }
    // The Gesture to send now, if one waits and the bucket allows it; one that cannot go at all
    // (link down, no relay) is dropped.
    bool nextGesture(int64_t now, uint64_t& game, Gesture& g) {
        std::lock_guard<std::mutex> lk(m);
        if (!gestureOut.pending) return false;
        const bool usable = gestureLink && gestureBucket_.enabled();
        if (usable && !gestureBucket_.take(double(now))) return false;
        gestureOut.pending = false;
        game = gestureOut.game;
        g = gestureOut.g;
        return usable;
    }
    // When the Gesture waiting may go (now + 1 s when none waits).
    int64_t gestureDeadline(int64_t now) const {
        std::lock_guard<std::mutex> lk(m);
        if (!gestureOut.pending) return now + 1000;
        return int64_t(std::ceil(gestureBucket_.readyAtMs(double(now))));
    }
    void connectionEvent(ConnState st, const std::string& err = std::string()) {
        Event ev;
        ev.kind = Event::Kind::ConnectionChanged;
        ev.state = st;
        ev.error = err;
        pushEvent(std::move(ev));
    }
    void fail(const std::string& err) {
        LOGW("direct: %s failed: %s", isHost ? "hosting" : "joining", err.c_str());
        {
            std::lock_guard<std::mutex> lk(m);
            state = DirectMatch::State::Failed;
            lastError = err;
        }
        connectionEvent(err == "incompatible" ? ConnState::Incompatible : ConnState::Offline, err);
    }
    std::deque<Command> takeCommands() {
        std::lock_guard<std::mutex> lk(m);
        std::deque<Command> c;
        c.swap(commands);
        return c;
    }
    void setPing(double rtt) {
        std::lock_guard<std::mutex> lk(m);
        pingMs = rtt < 0 ? -1 : int(std::lround(rtt));
    }

private:
    GestureBucket gestureBucket_;   // pacing of the Gestures sent (under m)
};

// ---- host ---------------------------------------------------------------------------------------

class HostSession : public Session {
public:
    // previous: host sessions still closing (they may hold the port and its UPnP mapping).
    HostSession(const DirectHostOptions& o, std::vector<std::shared_ptr<Session>> previous)
        : Session(true), opt_(o), previous_(std::move(previous)) {}

private:
    DirectHostOptions opt_;
    std::vector<std::shared_ptr<Session>> previous_;
    std::string code_;
    sock::Handle listener_ = sock::kInvalid;
    uint16_t port_ = 0;
    std::vector<std::unique_ptr<Conn>> pending_;
    std::vector<std::unique_ptr<Conn>> closing_;   // refused or flooding links (closeLater)
    std::unique_ptr<Conn> guest_;
    std::unique_ptr<direct::Authority> auth_;
    ClientView view_;
    int failedHandshakes_ = 0;
    bool quit_ = false;
    uint32_t pingNonce_ = 0;
    std::map<uint32_t, int64_t> pingSent_;
    int64_t nextPing_ = 0;
    double rtt_ = -1;
    int64_t rateWindow_ = 0;
    int rateCount_ = 0;
    GestureBucket guestGestures_;   // the guest's Gestures, at most kGestureRate (kGestureBurst at once)
    bool mapped_ = false;
    upnp::Gateway gw_;
    upnp::Mapping mapping_;
    int64_t renewAt_ = 0;
    std::thread renewThread_;

    void run() override {
        sock::startup();
        // A previous match that is still closing releases its port and deletes its mapping first
        // (otherwise its DeletePortMapping could remove the mapping this match is about to make).
        const int64_t waitEnd = sock::steadyMs() + 6000;
        for (auto& p : previous_)
            while (!p->finished && !stopFlag && sock::steadyMs() < waitEnd) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        previous_.clear();
        if (stopFlag) return;
        code_ = direct::newJoinCode();
        if (code_.empty() || !waker.valid()) { fail("network"); return; }
        std::string err;
        bool dual = false;
        listener_ = sock::listenTcp(opt_.port, dual, err);
        if (listener_ == sock::kInvalid) { fail(err == "in_use" ? "port_in_use" : "network"); return; }
        sock::Endpoint le;
        sock::localEndpoint(listener_, le);
        port_ = le.port();
        LOGI("direct: hosting on TCP port %u (%s)", unsigned(port_), dual ? "IPv6 and IPv4" : "IPv4 only");
        std::string externalIp;
        if (opt_.upnp) openPort(externalIp);
        if (!stopFlag) {
            std::lock_guard<std::mutex> lk(m);
            invite.port = port_;
            invite.code = direct::formatJoinCode(code_);
            invite.lanAddresses = sock::localAddresses(true, true);
            if (!externalIp.empty() && !upnpStatus.cgnatSuspected) invite.publicAddress = externalIp;
            state = DirectMatch::State::WaitingForGuest;
        }
        loop();
        cleanup();
    }

    // UPnP: find the router, learn the external address, map the port (the listener may move to
    // another port on a 718 conflict so that the internal and external ports stay equal).
    void openPort(std::string& externalIp) {
        {
            std::lock_guard<std::mutex> lk(m);
            state = DirectMatch::State::OpeningPort;
            upnpStatus.state = UpnpStatus::State::Searching;
        }
        upnp::Config cfg;
        cfg.cancel = &stopFlag;
        upnp::Client client(cfg);
        upnp::Error e;
        if (!client.discover(gw_, e)) {
            std::lock_guard<std::mutex> lk(m);
            upnpStatus.state = e.text == "no_gateway" || e.text == "no_wan_service" ? UpnpStatus::State::NoGateway : UpnpStatus::State::Failed;
            upnpStatus.error = e.text;
            LOGI("direct: UPnP: %s", e.text.c_str());
            return;
        }
        std::string ip = gw_.externalIp;
        if (ip.empty()) client.getExternalIp(gw_, ip, e);
        if (ip == "0.0.0.0") ip.clear();
        auto claim = [this](uint16_t p) {
            std::string er;
            bool dual = false;
            sock::Handle nl = sock::listenTcp(p, dual, er);
            if (nl == sock::kInvalid) return false;
            sock::closeSocket(listener_);
            listener_ = nl;
            port_ = p;
            return true;
        };
        upnp::Mapping mp;
        bool ok = client.mapPort(gw_, port_, claim, mp, e);
        // Any device of the LAN may answer the discovery: its name is cleaned like a player's.
        const std::string routerName = direct::sanitizeName(gw_.friendlyName, "", kMaxRouterName);
        std::lock_guard<std::mutex> lk(m);
        upnpStatus.gatewayName = routerName;
        upnpStatus.externalIp = ip;
        upnpStatus.cgnatSuspected = !ip.empty() && upnp::cgnatSuspected(ip);
        if (ok) {
            upnpStatus.state = UpnpStatus::State::Mapped;
            upnpStatus.externalPort = mp.externalPort;
            mapped_ = true;
            mapping_ = mp;
            renewAt_ = sock::steadyMs() + kRenewEveryMs;
            LOGI("direct: UPnP: %s maps %s:%u -> %s:%u (lease %u s)", routerName.c_str(), ip.c_str(), unsigned(mp.externalPort),
                 mp.internalClient.c_str(), unsigned(mp.internalPort), unsigned(mp.leaseSec));
        } else {
            upnpStatus.state = UpnpStatus::State::Failed;
            upnpStatus.error = e.text;
            LOGW("direct: UPnP: mapping refused: %s", e.text.c_str());
        }
        if (upnpStatus.cgnatSuspected) LOGW("direct: UPnP: external address %s is not public (carrier-grade NAT?)", ip.c_str());
        externalIp = ip;
    }

    void dispatch(direct::Authority::Output& out) {
        for (auto& msg : out.toHost) {
            Event ev;
            if (view_.apply(msg.data(), msg.size(), ev)) pushEvent(std::move(ev));
        }
        if (guest_)
            for (auto& msg : out.toGuest) guest_->send(msg);
        out.clear();
    }

    void loop() {
        direct::Authority::Output out;
        while (!stopFlag && !quit_) {
            int64_t now = sock::steadyMs();
            int64_t wait = 1000;
            auto until = [&](int64_t t) { wait = std::min(wait, std::max<int64_t>(0, t - now)); };
            if (auth_) {
                double d = auth_->nextDeadline();
                if (std::isfinite(d)) until(now + int64_t(std::ceil(d - sock::epochMs())) + 1);
            }
            for (auto& c : pending_) until(c->deadline);
            for (auto& c : closing_) until(c->deadline);
            if (guest_) {
                until(nextPing_);
                until(guest_->lastRecv + kSilenceMs + 1);
                until(gestureDeadline(now));
            }
            if (mapped_ && mapping_.leaseSec) until(renewAt_);
            sock::PollSet ps;
            ps.add(waker.handle(), true, false);
            if (listener_ != sock::kInvalid) ps.add(listener_, true, false);
            for (auto& c : pending_) ps.add(c->h, true, c->wantsWrite());
            for (auto& c : closing_) ps.add(c->h, true, !c->halfClosed && c->wantsWrite());
            if (guest_) ps.add(guest_->h, true, guest_->wantsWrite());
            ps.wait(int(wait));
            if (ps.readable(waker.handle())) waker.drain();
            if (stopFlag) break;
            now = sock::steadyMs();

            // The host's own actions: the same messages, the same authority.
            for (auto& c : takeCommands()) {
                if (!auth_) continue;
                std::vector<uint8_t> msg;
                encodeCommand(c, 0, msg);
                auth_->onMessage(direct::HostSide, msg.data(), msg.size(), sock::epochMs(), out);
                dispatch(out);
            }
            if (listener_ != sock::kInvalid && ps.readable(listener_)) acceptAll(now);
            serviceHandshakes(now, ps, out);
            if (guest_) serviceGuest(now, ps, out);
            for (size_t i = 0; i < closing_.size();) {
                if (closing_[i]->closeStep(now)) closing_.erase(closing_.begin() + long(i));
                else ++i;
            }
            if (auth_) {
                auth_->tick(sock::epochMs(), out);
                dispatch(out);
            }
            if (guest_ && now >= nextPing_) sendPing(now);
            sendGesture(now);
            if (mapped_ && mapping_.leaseSec && now >= renewAt_) renew(now);
            if (guest_ && !guest_->write()) dropGuest(out, "write failed");
            if (!auth_) {
                std::lock_guard<std::mutex> lk(m);
                if (state == DirectMatch::State::WaitingForGuest || state == DirectMatch::State::Handshake)
                    state = pending_.empty() ? DirectMatch::State::WaitingForGuest : DirectMatch::State::Handshake;
            }
        }
    }

    void acceptAll(int64_t now) {
        for (;;) {
            sock::Endpoint peer;
            sock::Handle h = sock::acceptOne(listener_, &peer);
            if (h == sock::kInvalid) break;
            if (int(pending_.size()) >= kMaxPendingHandshakes) {
                sock::closeSocket(h);
                continue;
            }
            auto c = std::make_unique<Conn>();
            c->h = h;
            c->ch = std::make_unique<SecureChannel>(SecureChannel::Role::Host, code_);
            c->ch->start();
            c->deadline = now + kHandshakeMs;
            c->lastRecv = now;
            LOGI("direct: connection from %s", peer.toString().c_str());
            pending_.push_back(std::move(c));
        }
    }

    void serviceHandshakes(int64_t now, const sock::PollSet& ps, direct::Authority::Output& out) {
        for (size_t i = 0; i < pending_.size();) {
            Conn& c = *pending_[i];
            bool alive = !ps.readable(c.h) || c.read();
            bool drop = false, promoted = false;
            if (c.ch->failed()) {
                if (c.ch->failure() == SecureChannel::Failure::WrongCode) {
                    ++failedHandshakes_;
                    LOGW("direct: handshake with a wrong code (%d/%d)", failedHandshakes_, kMaxFailedHandshakes);
                }
                drop = true;
            } else if (c.ch->established()) {
                std::vector<uint8_t> msg;
                if (c.ch->popMessage(msg)) {
                    promoted = handleHello(pending_[i], msg, now, out);
                    drop = !promoted;
                }
            }
            if (!promoted && !drop && (!alive || now >= c.deadline || !c.write())) drop = true;
            if (promoted || drop) pending_.erase(pending_.begin() + long(i));
            else ++i;
        }
        if (failedHandshakes_ >= kMaxFailedHandshakes && listener_ != sock::kInvalid) {
            LOGW("direct: too many wrong codes, no longer accepting connections");
            sock::closeSocket(listener_);
            listener_ = sock::kInvalid;
            pending_.clear();
            if (!auth_) {
                fail("too_many_attempts");
                quit_ = true;
            }
        }
    }

    // First message of an established channel: must be a compatible Hello.
    bool handleHello(std::unique_ptr<Conn>& cp, const std::vector<uint8_t>& msg, int64_t now, direct::Authority::Output& out) {
        Conn& c = *cp;
        auto refuse = [&](P::ErrorCode code, uint32_t ref) {
            P::Error e;
            e.ref = ref;
            e.code = code;
            e.fatal = true;
            std::vector<uint8_t> buf;
            P::encode(e, buf);
            c.send(buf);
            closeLater(std::move(cp), 300);
            LOGW("direct: guest refused (%s)", P::enumName(code));
        };
        P::MsgType t;
        P::Hello h;
        if (!P::peekType(msg.data(), msg.size(), t) || t != P::MsgType::Hello) { refuse(P::ErrorCode::HelloRequired, 0); return false; }
        if (!P::decode(msg.data(), msg.size(), h) || h.seq != 1) { refuse(P::ErrorCode::Malformed, h.seq); return false; }
        if (h.proto < P::kProtocolMin || h.proto > P::kProtocolVersion || h.schema != P::kSchemaHash) {
            refuse(P::ErrorCode::UnsupportedProtocol, h.seq);
            return false;
        }
        // Accepted: this connection is the guest from now on (it replaces a stale one).
        if (guest_) LOGI("direct: the guest's new connection replaces the previous one");
        guest_ = std::move(cp);
        guest_->lastSeq = 1;
        guest_->lastRecv = now;
        nextPing_ = now;
        rateWindow_ = now;
        rateCount_ = 0;
        guestGestures_.reset(double(now), kGestureRate, kGestureBurst);
        const double enow = sock::epochMs();
        const bool first = !auth_;
        if (first) {
            direct::AuthorityConfig cfg;
            cfg.baseMs = int64_t(std::min(std::max(opt_.baseSec, 1), 10800)) * 1000;
            cfg.incMs = int64_t(std::min(std::max(opt_.incSec, 0), 180)) * 1000;
            cfg.autoPress = opt_.autoPress;
            auth_ = std::make_unique<direct::Authority>(cfg, opt_.playerName, nameFromToken(h.token), opt_.hostColor);
            auth_->startGame(enow, out);
        } else {
            auth_->onReconnect(direct::GuestSide, enow, out);
        }
        P::Welcome w;
        w.proto = P::kProtocolVersion;
        w.serverTime = enow;
        w.userId = 2;
        w.username = auth_->guestName();
        w.serverName = direct::sanitizeName(opt_.playerName, "Host");
        w.heartbeatMs = kPingEveryMs;
        w.clientPingMs = kPingEveryMs;
        w.maxMsgPerSec = kMaxMsgPerSec;
        w.activeGame = auth_->gameId();
        w.gestureRate = kGestureRate;
        w.gestureBurst = kGestureBurst;
        std::vector<uint8_t> buf;
        P::encode(w, buf);
        guest_->send(buf);   // before the snapshot in 'out'
        gestureLinkUp(kGestureRate, kGestureBurst);   // the host's Gestures still go after the snapshot
        dispatch(out);
        LOGI("direct: guest \"%s\" %s", auth_->guestName().c_str(), first ? "joined" : "reconnected");
        if (first) {
            setState(DirectMatch::State::Playing);
            connectionEvent(ConnState::Online);
        }
        return true;
    }

    void serviceGuest(int64_t now, const sock::PollSet& ps, direct::Authority::Output& out) {
        bool alive = !ps.readable(guest_->h) || guest_->read();
        std::vector<uint8_t> msg;
        while (guest_ && guest_->ch->popMessage(msg)) handleGuestMessage(msg, now, out);
        if (!guest_) return;
        if (!alive || guest_->ch->failed()) dropGuest(out, guest_->peerClosed ? "closed" : guest_->ch->failed() ? "channel failure" : "network");
        else if (now - guest_->lastRecv > kSilenceMs) dropGuest(out, "silent");
    }

    void handleGuestMessage(const std::vector<uint8_t>& msg, int64_t now, direct::Authority::Output& out) {
        if (msg.size() < 5 || !P::isClientType(msg[0])) return;
        const uint32_t seq = uint32_t(msg[1]) | uint32_t(msg[2]) << 8 | uint32_t(msg[3]) << 16 | uint32_t(msg[4]) << 24;
        if (seq != guest_->lastSeq + 1) {
            // Logged once per connection (as the server does), not for every such message.
            if (!guest_->badSeqReported) LOGW("direct: guest message with seq %u dropped (expected %u)", seq, guest_->lastSeq + 1);
            guest_->badSeqReported = true;
            return;
        }
        guest_->lastSeq = seq;
        P::MsgType t{};   // 0 (no such type) for an id the schema does not define: the authority answers Malformed
        P::peekType(msg.data(), msg.size(), t);
        // Gestures have their own bucket and never count towards the flood limit: one beyond the
        // announced rate is dropped (the next one carries the whole state again).
        if (t == P::MsgType::C_Gesture) {
            P::C_Gesture g;
            if (!P::decode(msg.data(), msg.size(), g) || !auth_ || g.game != auth_->gameId()) return;
            if (guestGestures_.take(double(now))) pushGesture(g.game, gestureFromWire(g));
            return;
        }
        if (now - rateWindow_ >= 1000) {
            rateWindow_ = now;
            rateCount_ = 0;
        }
        if (++rateCount_ > 2 * kMaxMsgPerSec) {
            P::Error e;
            e.ref = seq;
            e.code = P::ErrorCode::Flood;
            e.fatal = true;
            std::vector<uint8_t> buf;
            P::encode(e, buf);
            guest_->send(buf);
            closeLater(std::move(guest_), 200);
            dropGuest(out, "flood");
            return;
        }
        switch (t) {
        case P::MsgType::C_Ping: {
            P::C_Ping p;
            if (!P::decode(msg.data(), msg.size(), p)) return;
            P::S_Pong r;
            r.nonce = p.nonce;
            r.serverTime = sock::epochMs();
            std::vector<uint8_t> buf;
            P::encode(r, buf);
            guest_->send(buf);
            return;
        }
        case P::MsgType::C_Pong: {
            P::C_Pong p;
            if (!P::decode(msg.data(), msg.size(), p)) return;
            auto it = pingSent_.find(p.nonce);
            if (it == pingSent_.end()) return;
            double sample = double(now - it->second);
            pingSent_.erase(it);
            rtt_ = rtt_ < 0 ? sample : rtt_ * 0.8 + sample * 0.2;
            if (auth_) auth_->onRtt(direct::GuestSide, sample);
            setPing(rtt_);
            return;
        }
        case P::MsgType::Hello: {
            P::Error e;
            e.ref = seq;
            e.code = P::ErrorCode::ProtocolViolation;
            std::vector<uint8_t> buf;
            P::encode(e, buf);
            guest_->send(buf);
            return;
        }
        default:
            if (!auth_) return;
            auth_->onMessage(direct::GuestSide, msg.data(), msg.size(), sock::epochMs(), out);
            dispatch(out);
            return;
        }
    }

    void sendPing(int64_t now) {
        P::S_Ping p;
        p.nonce = ++pingNonce_;
        p.serverTime = sock::epochMs();
        std::vector<uint8_t> buf;
        P::encode(p, buf);
        guest_->send(buf);
        pingSent_[p.nonce] = now;
        while (pingSent_.size() > 8) pingSent_.erase(pingSent_.begin());
        nextPing_ = now + kPingEveryMs;
    }

    // The host's own Gesture, straight to the guest (the authority never sees Gestures).
    void sendGesture(int64_t now) {
        uint64_t game = 0;
        Gesture g;
        if (!nextGesture(now, game, g) || !guest_ || !auth_ || game != auth_->gameId()) return;
        P::S_Gesture m;
        m.game = game;
        gestureToWire(g, m);
        std::vector<uint8_t> buf;
        P::encode(m, buf);
        guest_->send(buf);
    }

    // Closes c like flushAndClose(ms) without blocking the loop (a peer that keeps the connection
    // open would hold it for the whole delay): the loop flushes it, half-closes it and closes it
    // once the peer has closed too, or after ms.
    void closeLater(std::unique_ptr<Conn> c, int ms) {
        c->deadline = sock::steadyMs() + ms;
        if (closing_.size() >= kMaxClosing) closing_.erase(closing_.begin());
        closing_.push_back(std::move(c));
    }

    void dropGuest(direct::Authority::Output& out, const char* why) {
        LOGI("direct: guest connection lost (%s)", why);
        gestureLinkDown();
        guest_.reset();
        pingSent_.clear();
        rtt_ = -1;
        setPing(-1);
        if (auth_) {
            auth_->onDisconnect(direct::GuestSide, sock::epochMs(), out);
            dispatch(out);
        }
    }

    void renew(int64_t now) {
        if (renewThread_.joinable()) renewThread_.join();
        renewAt_ = now + kRenewEveryMs;
        upnp::Gateway gw = gw_;
        upnp::Mapping mp = mapping_;
        renewThread_ = std::thread([this, gw, mp] {
            upnp::Client client;
            upnp::Error e;
            if (!client.renew(gw, mp, e)) {
                LOGW("direct: UPnP lease renewal failed: %s", e.text.c_str());
                std::lock_guard<std::mutex> lk(m);
                upnpStatus.error = "renewal: " + e.text;
            }
        });
    }

    void cleanup() {
        direct::Authority::Output out;
        if (auth_ && !auth_->isOver()) {
            auth_->hostLeaves(sock::epochMs(), out);   // leaving = resigning (like online)
            dispatch(out);
        }
        gestureLinkDown();
        if (guest_) guest_->flushAndClose(1000);
        guest_.reset();
        for (auto& c : closing_) c->flushAndClose(int(std::max<int64_t>(0, c->deadline - sock::steadyMs())));
        closing_.clear();
        pending_.clear();
        sock::closeSocket(listener_);
        listener_ = sock::kInvalid;
        if (renewThread_.joinable()) renewThread_.join();
        if (mapped_) {
            upnp::Config cfg;
            cfg.httpTimeoutMs = 2000;
            upnp::Client client(cfg);
            upnp::Error e;
            if (client.deletePortMapping(gw_, mapping_.externalPort, e)) LOGI("direct: UPnP mapping of port %u removed", unsigned(mapping_.externalPort));
            else LOGW("direct: UPnP DeletePortMapping failed: %s", e.text.c_str());
            mapped_ = false;
        }
    }
};

// ---- guest --------------------------------------------------------------------------------------

class GuestSession : public Session {
public:
    GuestSession(const std::string& address, uint16_t port, const std::string& code, const std::string& name)
        : Session(false), address_(address), port_(port), code_(code), name_(name) {}

private:
    enum class Phase { Connect, Handshake, Hello, Online, Backoff, Done };
    std::string address_;
    uint16_t port_;
    std::string code_, name_;
    std::vector<sock::Endpoint> endpoints_;
    size_t epIndex_ = 0;
    std::string connectError_;
    Phase phase_ = Phase::Connect;
    std::unique_ptr<Conn> conn_;
    int64_t phaseDeadline_ = 0, retryAt_ = 0, reconnectDeadline_ = 0;
    bool everOnline_ = false, reconnecting_ = false;
    int refused_ = 0, attempt_ = 0;
    uint32_t seq_ = 0;
    ClientView view_;
    std::deque<Command> queued_;
    uint32_t pingNonce_ = 0;
    std::map<uint32_t, int64_t> pingSent_;
    int64_t nextPing_ = 0;
    double rtt_ = -1, offset_ = 0;
    bool haveRttOffset_ = false;

    template <class T> void sendMsg(T& msg) {
        msg.seq = ++seq_;
        std::vector<uint8_t> buf;
        P::encode(msg, buf);
        conn_->send(buf);
    }

    void run() override {
        sock::startup();
        if (!waker.valid()) { fail("network"); return; }
        connectionEvent(ConnState::Connecting);
        if (!sock::resolve(address_, port_, true, endpoints_)) { fail("not_found"); return; }
        epIndex_ = 0;
        connectNext(sock::steadyMs());
        while (!stopFlag && phase_ != Phase::Done) {
            int64_t now = sock::steadyMs();
            int64_t wait = 1000;
            auto until = [&](int64_t t) { wait = std::min(wait, std::max<int64_t>(0, t - now)); };
            if (phase_ == Phase::Connect || phase_ == Phase::Handshake || phase_ == Phase::Hello) until(phaseDeadline_);
            if (phase_ == Phase::Backoff) until(retryAt_);
            if (phase_ == Phase::Online && conn_) {
                until(nextPing_);
                until(conn_->lastRecv + kSilenceMs + 1);
                until(gestureDeadline(now));
            }
            sock::PollSet ps;
            ps.add(waker.handle(), true, false);
            if (conn_) {
                if (phase_ == Phase::Connect) ps.add(conn_->h, false, true);
                else ps.add(conn_->h, true, conn_->wantsWrite());
            }
            ps.wait(int(wait));
            if (ps.readable(waker.handle())) waker.drain();
            if (stopFlag) break;
            now = sock::steadyMs();
            for (auto& c : takeCommands()) {
                if (phase_ == Phase::Online && conn_) sendCommand(c);
                else if (reconnecting_ && queued_.size() < kMaxQueuedCommands) queued_.push_back(c);
            }
            switch (phase_) {
            case Phase::Connect:
                if (conn_ && ps.writable(conn_->h)) {
                    int e = sock::connectResult(conn_->h);
                    if (e == 0) beginHandshake(now);
                    else { connectError_ = sock::errorName(e); connectNext(now); }
                } else if (now >= phaseDeadline_) {
                    connectError_ = "timeout";
                    connectNext(now);
                }
                break;
            case Phase::Handshake:
            case Phase::Hello:
            case Phase::Online:
                if (conn_) service(now, ps);
                break;
            case Phase::Backoff:
                if (now >= retryAt_) {
                    epIndex_ = 0;
                    connectNext(now);
                }
                break;
            case Phase::Done:
                break;
            }
        }
        if (stopFlag) leave();
    }

    void connectNext(int64_t now) {
        conn_.reset();
        while (epIndex_ < endpoints_.size()) {
            const sock::Endpoint& ep = endpoints_[epIndex_++];
            sock::Handle h = sock::openTcp(ep.family());
            if (h == sock::kInvalid) { connectError_ = "network"; continue; }
            int e = 0;
            int r = sock::connectStart(h, ep, e);
            if (r < 0) {
                connectError_ = sock::errorName(e);
                sock::closeSocket(h);
                continue;
            }
            conn_ = std::make_unique<Conn>();
            conn_->h = h;
            phase_ = Phase::Connect;
            phaseDeadline_ = now + kConnectMs;
            if (!everOnline_) setState(DirectMatch::State::Connecting);
            if (r == 1) beginHandshake(now);
            return;
        }
        attemptFailed(connectError_.empty() ? "unreachable" : connectError_, now);
    }

    void beginHandshake(int64_t now) {
        conn_->ch = std::make_unique<SecureChannel>(SecureChannel::Role::Guest, code_);
        if (!conn_->ch->start()) { attemptFailed("network", now); return; }
        conn_->lastRecv = now;
        phase_ = Phase::Handshake;
        phaseDeadline_ = now + kHandshakeMs;
        if (!everOnline_) setState(DirectMatch::State::Handshake);
        conn_->write();
    }

    static std::string failureReason(SecureChannel::Failure f) {
        switch (f) {
        case SecureChannel::Failure::WrongCode: return "wrong_code";
        case SecureChannel::Failure::BadHello:
        case SecureChannel::Failure::BadVersion: return "incompatible";
        default: return "closed";
        }
    }

    void service(int64_t now, const sock::PollSet& ps) {
        bool alive = !ps.readable(conn_->h) || conn_->read();
        if (phase_ == Phase::Handshake && conn_->ch->established()) {
            P::Hello h;
            h.proto = P::kProtocolVersion;
            h.schema = P::kSchemaHash;
            h.client = kClientString;
            h.token = tokenFor(name_);
            seq_ = 0;
            sendMsg(h);   // seq 1
            phase_ = Phase::Hello;
            phaseDeadline_ = now + kHandshakeMs;
        }
        std::vector<uint8_t> msg;
        while (conn_ && conn_->ch->established() && conn_->ch->popMessage(msg)) handleMessage(msg, now);
        if (!conn_) return;
        if (conn_->ch->failed()) {
            if (phase_ == Phase::Online) lost(now);
            else attemptFailed(failureReason(conn_->ch->failure()), now);
            return;
        }
        if (!alive) {
            if (phase_ == Phase::Online) lost(now);
            else attemptFailed(phase_ == Phase::Handshake && conn_->ch->awaitingHostConfirm() ? "wrong_code" : "closed", now);
            return;
        }
        if ((phase_ == Phase::Handshake || phase_ == Phase::Hello) && now >= phaseDeadline_) {
            attemptFailed("timeout", now);
            return;
        }
        if (phase_ == Phase::Online) {
            if (now - conn_->lastRecv > kSilenceMs) { lost(now); return; }
            if (now >= nextPing_) sendPing(now);
            sendGesture(now);
            if (view_.needResync && view_.have) {
                P::Resync r;
                r.game = view_.game.id;
                sendMsg(r);
                view_.needResync = false;
            }
        }
        if (!conn_->write()) {
            if (phase_ == Phase::Online) lost(now);
            else attemptFailed("closed", now);
        }
    }

    void handleMessage(const std::vector<uint8_t>& msg, int64_t now) {
        P::MsgType t;
        if (!P::peekType(msg.data(), msg.size(), t)) return;
        if (phase_ == Phase::Hello) {
            if (t == P::MsgType::Welcome) {
                P::Welcome w;
                if (!P::decode(msg.data(), msg.size(), w)) { attemptFailed("incompatible", now); return; }
                online(now, w);
            } else if (t == P::MsgType::Error) {
                P::Error e;
                bool unsupported = P::decode(msg.data(), msg.size(), e) && e.code == P::ErrorCode::UnsupportedProtocol;
                attemptFailed(unsupported ? "incompatible" : "refused", now);
            }
            return;
        }
        if (phase_ != Phase::Online) return;
        switch (t) {
        case P::MsgType::S_Ping: {
            P::S_Ping p;
            if (!P::decode(msg.data(), msg.size(), p)) return;
            P::C_Pong r;
            r.nonce = p.nonce;
            sendMsg(r);
            return;
        }
        case P::MsgType::S_Pong: {
            P::S_Pong p;
            if (!P::decode(msg.data(), msg.size(), p)) return;
            auto it = pingSent_.find(p.nonce);
            if (it == pingSent_.end()) return;
            double sample = double(now - it->second);
            pingSent_.erase(it);
            // The host's clock read half a round trip ago.
            double off = p.serverTime + sample / 2 - sock::epochMs();
            if (!haveRttOffset_) { rtt_ = sample; offset_ = off; haveRttOffset_ = true; }
            else { rtt_ = rtt_ * 0.8 + sample * 0.2; offset_ = offset_ * 0.8 + off * 0.2; }
            setPing(rtt_);
            std::lock_guard<std::mutex> lk(m);
            clockOffset = offset_;
            return;
        }
        case P::MsgType::S_Gesture: {
            P::S_Gesture g;
            if (!P::decode(msg.data(), msg.size(), g) || !view_.have || g.game != view_.game.id) return;
            pushGesture(g.game, gestureFromWire(g));
            return;
        }
        default: {
            // A host that sends faster than the game thread polls cannot grow the queue without
            // limit: the link is dropped instead (the reconnection brings a snapshot).
            if (queuedEvents() >= kMaxQueuedEvents) {
                LOGW("direct: the host floods events");
                lost(now);
                return;
            }
            Event ev;
            if (view_.apply(msg.data(), msg.size(), ev)) pushEvent(std::move(ev));
            return;
        }
        }
    }

    void online(int64_t now, const P::Welcome& w) {
        phase_ = Phase::Online;
        const bool first = !everOnline_;
        everOnline_ = true;
        reconnecting_ = false;
        refused_ = attempt_ = 0;
        if (!haveRttOffset_) {
            std::lock_guard<std::mutex> lk(m);
            offset_ = w.serverTime - sock::epochMs();
            clockOffset = offset_;
        }
        nextPing_ = now;
        gestureLinkUp(w.gestureRate, w.gestureBurst);
        if (first) setState(DirectMatch::State::Playing);
        connectionEvent(ConnState::Online);
        LOGI("direct: %s the match of \"%s\"", first ? "joined" : "rejoined", w.serverName.c_str());
        // Actions made while the link was down go now (the host answers them after the snapshot).
        for (auto& c : queued_) sendCommand(c);
        queued_.clear();
    }

    void sendCommand(const Command& c) {
        std::vector<uint8_t> buf;
        encodeCommand(c, ++seq_, buf);
        conn_->send(buf);
    }

    // The guest's own Gesture, for the game the host's messages describe.
    void sendGesture(int64_t now) {
        uint64_t game = 0;
        Gesture g;
        if (!nextGesture(now, game, g) || !conn_ || !view_.have || game != view_.game.id) return;
        P::C_Gesture m;
        m.game = game;
        gestureToWire(g, m);
        sendMsg(m);
    }

    void sendPing(int64_t now) {
        P::C_Ping p;
        p.nonce = ++pingNonce_;
        sendMsg(p);
        pingSent_[p.nonce] = now;
        while (pingSent_.size() > 8) pingSent_.erase(pingSent_.begin());
        nextPing_ = now + kPingEveryMs;
    }

    // The established link broke.
    void lost(int64_t now) {
        gestureLinkDown();
        conn_.reset();
        pingSent_.clear();
        setPing(-1);
        if (view_.have && view_.game.status == int(P::GameStatus::Ongoing)) {
            LOGI("direct: connection to the host lost, reconnecting");
            reconnecting_ = true;
            refused_ = attempt_ = 0;
            int64_t grace = view_.game.graceMs ? int64_t(view_.game.graceMs) : kDefaultGraceMs;
            reconnectDeadline_ = now + grace + 5000;
            connectionEvent(ConnState::Reconnecting);
            phase_ = Phase::Backoff;
            retryAt_ = now + 250;
        } else {
            {
                std::lock_guard<std::mutex> lk(m);
                lastError = "closed";
            }
            connectionEvent(ConnState::Offline, "closed");
            phase_ = Phase::Done;
        }
    }

    // A connection attempt (first join or reconnection) failed.
    void attemptFailed(const std::string& reason, int64_t now) {
        conn_.reset();
        if (!everOnline_) {
            fail(reason);
            phase_ = Phase::Done;
            return;
        }
        if (reason == "refused") ++refused_;
        if (reason == "wrong_code" || reason == "incompatible" || refused_ >= 3 || now >= reconnectDeadline_) {
            giveUp();
            return;
        }
        ++attempt_;
        phase_ = Phase::Backoff;
        retryAt_ = now + std::min<int64_t>(1000 * attempt_, 5000);
    }

    // The host is gone for good (closed its port, or never came back within its grace).
    void giveUp() {
        LOGW("direct: the host is gone");
        Event ev;
        if (view_.endLocally(sock::epochMs() + offset_, ev)) pushEvent(std::move(ev));
        {
            std::lock_guard<std::mutex> lk(m);
            lastError = "host_left";
        }
        connectionEvent(ConnState::Offline, "host_left");
        phase_ = Phase::Done;
    }

    // close(): resign a running game (like leaving online), then close gracefully.
    void leave() {
        gestureLinkDown();
        if (phase_ == Phase::Online && conn_ && view_.have && view_.game.status == int(P::GameStatus::Ongoing)) {
            P::Resign r;
            r.game = view_.game.id;
            sendMsg(r);
        }
        if (conn_) conn_->flushAndClose(1000);
        conn_.reset();
    }
};

}  // namespace

// ---- DirectMatch --------------------------------------------------------------------------------

struct DirectMatch::Impl {
    mutable std::mutex m;
    std::shared_ptr<Session> cur;
    std::vector<std::shared_ptr<Session>> retiring;   // closing in the background
    OnlineGame view;
    bool haveView = false;

    void retire() {
        if (cur) {
            cur->requestStop();
            retiring.push_back(cur);
            cur.reset();
        }
        haveView = false;
        view = OnlineGame();
    }
    void reap(bool all) {
        for (size_t i = 0; i < retiring.size();) {
            if (all || retiring[i]->finished || !retiring[i]->thread.joinable()) {
                retiring[i]->join();
                retiring.erase(retiring.begin() + long(i));
            } else {
                ++i;
            }
        }
    }
    void post(Command c) {
        if (!cur) return;
        c.game = haveView ? view.id : 0;
        cur->post(c);
    }
};

DirectMatch::DirectMatch() : impl_(new Impl) {}

DirectMatch::~DirectMatch() {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->retire();
    impl_->reap(true);
}

void DirectMatch::host(const DirectHostOptions& opt) {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->retire();
    impl_->reap(false);
    std::vector<std::shared_ptr<Session>> closingHosts;
    for (auto& r : impl_->retiring)
        if (r->isHost) closingHosts.push_back(r);
    auto s = std::make_shared<HostSession>(opt, std::move(closingHosts));
    s->state = State::OpeningPort;   // listening, then UPnP when enabled
    s->start();
    impl_->cur = s;
}

void DirectMatch::join(const std::string& address, uint16_t port, const std::string& code, const std::string& playerName) {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->retire();
    impl_->reap(false);
    std::string norm;
    bool validCode = direct::normalizeJoinCode(code, norm);
    auto s = std::make_shared<GuestSession>(address, port, norm, playerName);
    if (!validCode || address.empty() || port == 0) {
        s->state = State::Failed;
        s->lastError = !validCode ? "invalid_code" : "bad_address";
        Event ev;
        ev.kind = Event::Kind::ConnectionChanged;
        ev.state = ConnState::Offline;
        ev.error = s->lastError;
        s->events.push_back(ev);
    } else {
        s->state = State::Connecting;
        s->start();
    }
    impl_->cur = s;
}

void DirectMatch::close() {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->retire();
    impl_->reap(false);
}

DirectMatch::State DirectMatch::state() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (!impl_->cur) return State::Idle;
    std::lock_guard<std::mutex> lk2(impl_->cur->m);
    return impl_->cur->state;
}

std::string DirectMatch::lastError() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (!impl_->cur) return std::string();
    std::lock_guard<std::mutex> lk2(impl_->cur->m);
    return impl_->cur->lastError;
}

DirectInvite DirectMatch::invite() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (!impl_->cur) return DirectInvite();
    std::lock_guard<std::mutex> lk2(impl_->cur->m);
    return impl_->cur->invite;
}

UpnpStatus DirectMatch::upnp() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (!impl_->cur) return UpnpStatus();
    std::lock_guard<std::mutex> lk2(impl_->cur->m);
    return impl_->cur->upnpStatus;
}

bool DirectMatch::isHost() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->cur && impl_->cur->isHost;
}

void DirectMatch::sendMove(int ply, uint16_t move, const std::string& fen, uint32_t thinkMs, bool drawOffer) {
    Command c;
    c.kind = Command::Kind::Move;
    c.ply = uint16_t(std::max(0, std::min(ply, 0xFFFF)));
    c.move = move;
    c.posHash = direct::fenDigest(fen);
    c.thinkMs = thinkMs;
    c.flag = drawOffer;
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->post(c);
}

static void postSimple(DirectMatch::Impl& impl, Command::Kind kind, bool flag) {
    Command c;
    c.kind = kind;
    c.flag = flag;
    std::lock_guard<std::mutex> lk(impl.m);
    impl.post(c);
}

void DirectMatch::resign() { postSimple(*impl_, Command::Kind::Resign, false); }
void DirectMatch::offerDraw() { postSimple(*impl_, Command::Kind::OfferDraw, false); }
void DirectMatch::answerDraw(bool accept) { postSimple(*impl_, Command::Kind::AnswerDraw, accept); }
void DirectMatch::claimDraw() { postSimple(*impl_, Command::Kind::ClaimDraw, false); }
void DirectMatch::abortGame() { postSimple(*impl_, Command::Kind::Abort, false); }
void DirectMatch::requestResync() { postSimple(*impl_, Command::Kind::Resync, false); }
void DirectMatch::rematch(bool accept) { postSimple(*impl_, Command::Kind::Rematch, accept); }

void DirectMatch::sendGesture(const Gesture& g) {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (impl_->cur && impl_->haveView) impl_->cur->postGesture(impl_->view.id, g);
}

const OnlineGame* DirectMatch::currentGame() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->haveView ? &impl_->view : nullptr;
}

int DirectMatch::pingMs() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (!impl_->cur) return -1;
    std::lock_guard<std::mutex> lk2(impl_->cur->m);
    return impl_->cur->pingMs;
}

double DirectMatch::serverNowMs() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    double offset = 0;
    if (impl_->cur && !impl_->cur->isHost) {
        std::lock_guard<std::mutex> lk2(impl_->cur->m);
        offset = impl_->cur->clockOffset;
    }
    return sock::epochMs() + offset;
}

bool DirectMatch::poll(Event& out) {
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->reap(false);
    if (!impl_->cur || !impl_->cur->popEvent(out)) return false;
    if (out.game.id != 0 && out.kind != Event::Kind::ConnectionChanged) {
        impl_->view = out.game;
        impl_->haveView = true;
    }
    return true;
}

DirectMatch& directMatch() {
    static DirectMatch instance;
    return instance;
}

}  // namespace net
