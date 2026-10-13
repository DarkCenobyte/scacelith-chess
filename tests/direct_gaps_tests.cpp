// Direct match, the paths tests/direct_tests.cpp leaves out: the draw commands of both players
// over a loopback match (offer, decline, claim with nothing to claim, agreement) and a declined
// rematch; the guest's commands kept while it reconnects and sent once it is back; the host's
// answers to a second Hello, to a client message the authority does not take and to a cut one;
// the host's limits on connections (4 handshakes at a time, 10 wrong codes); the guest's first
// join refused at each step (no such host name, a host that is not a direct match, another
// handshake version, an Error or a close in place of the Welcome, a broken frame) and a host that
// floods it with stances. Everything runs on 127.0.0.1; the host name lookups go through a fake
// (sock::Lookup::setForTests), never through a real DNS.
#include "test.h"
#include "chess/chess.h"
#include "net/direct_authority.h"
#include "net/direct_crypto.h"
#include "net/direct_match.h"
#include "net/protocol_gen.h"
#include "net/socket_util.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace net;

namespace {

namespace P = net::proto;
using direct::GuestSide;
using direct::HostSide;
using direct::Side;

// ---- two DirectMatch instances (as in tests/direct_tests.cpp) -----------------------------------

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
    bool hasConn(ConnState s) const {
        for (auto& e : events)
            if (e.kind == Event::Kind::ConnectionChanged && e.state == s) return true;
        return false;
    }
    // GameEvents of this kind received so far.
    int gameEvents(P::GameEventKind k) const {
        int n = 0;
        for (auto& e : events) n += e.kind == Event::Kind::GameEvent && e.gameEventKind == int(k);
        return n;
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

// Hosts a game (no UPnP, any free port) and waits for the invite.
bool hostGame(Peer& host) {
    Peer nobody;
    host.dm.host(hostOptions(300, 0, 1, "Alice"));
    return waitUntil(host, nobody, [&] { return host.dm.state() == DirectMatch::State::WaitingForGuest; });
}

// ---- the secure channel driven by hand (as in tests/direct_tests.cpp) ---------------------------

struct RawChannel {
    sock::Handle h = sock::kInvalid;
    std::unique_ptr<direct::SecureChannel> ch;

    ~RawChannel() { sock::closeSocket(h); }

    bool sendBytes(const std::vector<uint8_t>& buf) { return ch->send(buf.data(), buf.size()) && flush(); }
    // Bytes straight to the socket, outside the channel.
    bool sendRaw(const std::vector<uint8_t>& buf) {
        size_t done = 0;
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (done < buf.size()) {
            int w = sock::sendSome(h, buf.data() + done, buf.size() - done);
            if (w < 0 || std::chrono::steady_clock::now() > end) return false;
            if (w == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            done += size_t(w);
        }
        return true;
    }
    // The next message of type T (others are skipped), within ms.
    template <class T> bool waitFor(T& out, int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        std::vector<uint8_t> msg;
        for (;;) {
            while (ch->popMessage(msg))
                if (P::decode(msg.data(), msg.size(), out)) return true;
            if (!pumpOnce() || std::chrono::steady_clock::now() > end) return false;
        }
    }
    // True when the other side closes the connection within ms.
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
    bool handshake() {
        if (h == sock::kInvalid || !ch->start() || !flush()) return false;
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ch->established())
            if (!pumpOnce() || std::chrono::steady_clock::now() > end) return false;
        return true;
    }
    bool flush() {
        std::vector<uint8_t>& o = ch->outbox();
        bool ok = sendRaw(o);
        o.clear();
        return ok;
    }
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

// A guest written by hand.
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

// A host written by hand: it accepts the guest's connections one at a time.
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
    // The guest's next connection, through the channel's handshake.
    bool accept(int ms) { return acceptSocket(ms) && handshake(); }
    // The guest's next connection, without any handshake (the test writes the bytes).
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
    void drop() {
        sock::closeSocket(h);
        h = sock::kInvalid;
    }
    template <class T> bool send(const T& m) {
        std::vector<uint8_t> buf;
        P::encode(m, buf);
        return sendBytes(buf);
    }
};

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

// Hosts a match and joins it with a guest written by hand (its Hello, then the host's Welcome
// and snapshot).
bool joinRaw(Peer& host, RawGuest& raw, P::GameSnapshot& snap) {
    if (!hostGame(host)) return false;
    DirectInvite inv = host.dm.invite();
    P::Hello hello;
    hello.proto = P::kProtocolVersion;
    hello.client = "Scacelith test";
    hello.token = "direct:Raw      ";
    P::Welcome w;
    return raw.connect(inv.port, inv.code) && raw.send(hello) && raw.waitFor(w, 5000) && raw.waitFor(snap, 5000);
}

// A guest of a DirectMatch joined to a host written by hand that runs 'auth' (Welcome and
// snapshot sent); 'hello' receives the guest's Hello.
bool joinHand(Peer& guest, RawHost& raw, direct::Authority& auth, P::Hello& hello) {
    direct::Authority::Output out;
    auth.startGame(sock::epochMs(), out);
    if (out.toGuest.size() != 1) return false;
    Peer nobody;
    guest.dm.join("127.0.0.1", raw.port, raw.code, "Bob");
    return raw.accept(5000) && raw.waitFor(hello, 5000) && sendWelcome(raw, auth) && raw.sendBytes(out.toGuest[0]) &&
           waitUntil(guest, nobody, [&] { return guest.count(Event::Kind::GameSnapshot) == 1; });
}

// A first join that the host written by hand ends: waits until the guest gives up and returns
// its lastError().
std::string failedJoin(Peer& guest) {
    Peer nobody;
    if (!waitUntil(guest, nobody, [&] { return guest.dm.state() == DirectMatch::State::Failed; })) return "(still trying)";
    return guest.dm.lastError();
}

// A frame of the channel's format (u16 length, then the sealed bytes) whose tag cannot verify.
std::vector<uint8_t> forgedFrame() {
    std::vector<uint8_t> f(2 + 40, 0x5A);
    f[0] = 40;
    f[1] = 0;
    return f;
}

// ---- the name lookup of the guest, replaced -----------------------------------------------------

bool lookupNotFound(const std::string&, uint16_t, bool, std::vector<sock::Endpoint>&, std::string& error) {
    error = "no such host (test)";
    return false;
}

// Two addresses: one no TCP connection can reach (the limited broadcast address), then the host.
bool lookupBroadcastThenLoopback(const std::string&, uint16_t port, bool, std::vector<sock::Endpoint>& out, std::string&) {
    sock::Endpoint a, b;
    if (!sock::Endpoint::parse("255.255.255.255", port, a) || !sock::Endpoint::parse("127.0.0.1", port, b)) return false;
    out = {a, b};
    return true;
}

// Puts a lookup in place while it lives, then getaddrinfo again.
struct FakeDns {
    explicit FakeDns(sock::Lookup::Fn fn) { sock::Lookup::setForTests(fn); }
    ~FakeDns() { sock::Lookup::setForTests(nullptr); }
    FakeDns(const FakeDns&) = delete;
    FakeDns& operator=(const FakeDns&) = delete;
};

// ---- the authority with synthetic time (as in tests/direct_tests.cpp) ---------------------------

struct Room {
    direct::Authority a;
    direct::Authority::Output out;
    double now = 1.7e12;
    std::vector<std::vector<uint8_t>> msgs[2];
    size_t mark[2] = {0, 0};
    uint32_t seq[2] = {0, 0};

    explicit Room(int hostColorPref) : a(direct::AuthorityConfig(), "Alice", "Bob", hostColorPref) {}

    void collect() {
        mark[0] = msgs[0].size();
        mark[1] = msgs[1].size();
        for (auto& m : out.toHost) msgs[HostSide].push_back(m);
        for (auto& m : out.toGuest) msgs[GuestSide].push_back(m);
        out.clear();
    }
    void start() { a.startGame(now, out); collect(); }
    void sendBytes(Side s, const std::vector<uint8_t>& buf) {
        a.onMessage(s, buf.data(), buf.size(), now, out);
        collect();
    }
    template <class T> void send(Side s, T m) {
        m.seq = ++seq[s];
        std::vector<uint8_t> buf;
        P::encode(m, buf);
        sendBytes(s, buf);
    }
    template <class T> std::vector<T> recent(Side s) const {
        std::vector<T> v;
        for (size_t i = mark[s]; i < msgs[s].size(); ++i) {
            T t;
            if (P::decode(msgs[s][i].data(), msgs[s][i].size(), t)) v.push_back(t);
        }
        return v;
    }
};

}  // namespace

// ---- the authority ------------------------------------------------------------------------------

TEST(direct_gaps_authority_refuses_other_messages) {
    // A client message the authority does not take (the lobby's QueueLeave, a second Hello) is a
    // ProtocolViolation; one of a type it takes but cut short is Malformed; neither ends the game.
    Room r(1);
    r.start();
    P::QueueLeave q;
    r.send(GuestSide, q);
    auto e = r.recent<P::Error>(GuestSide);
    CHECK(e.size() == 1 && e[0].code == P::ErrorCode::ProtocolViolation && e[0].ref == 1 && e[0].game == 0);
    P::Hello hello;
    hello.proto = P::kProtocolVersion;
    hello.token = "direct:Bob      ";
    r.send(HostSide, hello);
    e = r.recent<P::Error>(HostSide);
    CHECK(e.size() == 1 && e[0].code == P::ErrorCode::ProtocolViolation);
    P::Resign res;
    res.seq = 7;
    res.game = r.a.gameId();
    std::vector<uint8_t> cut;
    P::encode(res, cut);
    cut.resize(cut.size() - 3);
    r.sendBytes(GuestSide, cut);
    e = r.recent<P::Error>(GuestSide);
    CHECK(e.size() == 1 && e[0].code == P::ErrorCode::Malformed && e[0].ref == 7);
    CHECK(r.recent<P::GameEnd>(HostSide).empty());
    // The game goes on: a resignation ends it, a declined rematch closes the window for both.
    res.seq = 8;
    r.send(GuestSide, res);
    auto end = r.recent<P::GameEnd>(HostSide);
    CHECK(end.size() == 1 && end[0].status == P::GameStatus::WhiteWins && end[0].reason == P::EndReason::Resignation);
    P::Rematch rm;
    rm.game = r.a.gameId();
    rm.accept = false;
    r.send(HostSide, rm);
    auto ev = r.recent<P::GameEvent>(GuestSide);
    CHECK(ev.size() == 1 && ev[0].kind == P::GameEventKind::RematchDeclined && ev[0].color == P::Color::White);
    rm.accept = true;
    r.send(GuestSide, rm);
    e = r.recent<P::Error>(GuestSide);
    CHECK(e.size() == 1 && e[0].code == P::ErrorCode::RematchUnavailable);
}

// ---- both players over 127.0.0.1 ----------------------------------------------------------------

TEST(direct_gaps_loopback_draw_commands) {
    // offerDraw / answerDraw / claimDraw of both players through their DirectMatch, then a
    // rematch the guest declines.
    using K = Event::Kind;
    Peer host, guest;
    CHECK(hostGame(host));
    DirectInvite inv = host.dm.invite();
    guest.dm.join("127.0.0.1", inv.port, inv.code, "Bob");
    CHECK(waitUntil(host, guest, [&] { return host.count(K::GameSnapshot) >= 1 && guest.count(K::GameSnapshot) >= 1; }));
    host.dm.offerDraw();
    CHECK(waitUntil(host, guest, [&] { return guest.gameEvents(P::GameEventKind::DrawOffered) == 1; }));
    CHECK(guest.last(K::GameEvent) && guest.last(K::GameEvent)->color == int(P::Color::White));
    guest.dm.answerDraw(false);
    CHECK(waitUntil(host, guest, [&] { return host.gameEvents(P::GameEventKind::DrawDeclined) == 1; }));
    guest.dm.claimDraw();
    CHECK(waitUntil(host, guest, [&] { return guest.count(K::ServerError) == 1; }));
    CHECK(guest.last(K::ServerError) && guest.last(K::ServerError)->code == int(P::ErrorCode::NothingToClaim));
    guest.dm.offerDraw();   // both players see every GameEvent: this is the second offer for each
    CHECK(waitUntil(host, guest, [&] { return host.gameEvents(P::GameEventKind::DrawOffered) == 2; }));
    CHECK(host.last(K::GameEvent) && host.last(K::GameEvent)->color == int(P::Color::Black));
    host.dm.answerDraw(true);
    CHECK(waitUntil(host, guest, [&] { return host.count(K::GameEnd) == 1 && guest.count(K::GameEnd) == 1; }));
    for (Peer* p : {&host, &guest}) {
        const Event* e = p->last(K::GameEnd);
        CHECK(e && e->game.status == int(P::GameStatus::Draw) && e->game.reason == int(P::EndReason::Agreement));
    }
    guest.dm.rematch(false);
    CHECK(waitUntil(host, guest, [&] { return host.gameEvents(P::GameEventKind::RematchDeclined) == 1; }));
    CHECK(host.count(K::GameSnapshot) == 1 && guest.count(K::GameSnapshot) == 1);   // no new game
}

TEST(direct_gaps_guest_tries_the_next_address) {
    // The host's name gives two addresses: the first one cannot take a TCP connection (on Linux
    // the connection fails at once), the guest goes on with the second one and joins.
    Peer host, guest;
    CHECK(hostGame(host));
    DirectInvite inv = host.dm.invite();
    {
        FakeDns dns(lookupBroadcastThenLoopback);
        guest.dm.join("host.invalid", inv.port, inv.code, "Bob");
        CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameSnapshot) == 1 && guest.count(Event::Kind::GameSnapshot) == 1; }, 9000));
    }
    CHECK(guest.dm.state() == DirectMatch::State::Playing);
}

TEST(direct_gaps_guest_host_name_not_found) {
    // A host name the lookup does not know: "not_found", and the guest stops there.
    Peer guest, nobody;
    {
        FakeDns dns(lookupNotFound);
        guest.dm.join("nowhere.invalid", 47100, "K7Q2-M9XH-3PTR", "Bob");
        CHECK(waitUntil(guest, nobody, [&] { return guest.dm.state() == DirectMatch::State::Failed; }));
    }
    CHECK_EQ(guest.dm.lastError(), std::string("not_found"));
    CHECK(guest.hasConn(ConnState::Offline));
}

// ---- the host's limits --------------------------------------------------------------------------

TEST(direct_gaps_host_four_handshakes_at_a_time) {
    // Four connections that say nothing hold the host's four handshake places (for 10 s each): a
    // fifth one is closed at once. Once they leave, the guest gets in.
    Peer host, guest;
    CHECK(hostGame(host));
    DirectInvite inv = host.dm.invite();
    sock::Endpoint ep;
    CHECK(sock::Endpoint::parse("127.0.0.1", inv.port, ep));
    std::vector<std::unique_ptr<RawChannel>> silent;
    std::string err;
    for (int i = 0; i < 4; ++i) {
        silent.push_back(std::make_unique<RawChannel>());
        silent.back()->h = sock::connectWithTimeout(ep, 2000, err);
        CHECK(silent.back()->h != sock::kInvalid);
    }
    // The host has taken them all once it says Handshake.
    CHECK(waitUntil(host, guest, [&] { return host.dm.state() == DirectMatch::State::Handshake; }));
    RawChannel fifth;
    fifth.h = sock::connectWithTimeout(ep, 2000, err);
    CHECK(fifth.h != sock::kInvalid);
    CHECK(fifth.waitClosed(3000));
    for (auto& c : silent) CHECK(!c->waitClosed(1));
    silent.clear();
    guest.dm.join("127.0.0.1", inv.port, inv.code, "Bob");
    CHECK(waitUntil(host, guest, [&] { return host.count(Event::Kind::GameSnapshot) == 1 && guest.count(Event::Kind::GameSnapshot) == 1; }));
}

TEST(direct_gaps_host_stops_after_ten_wrong_codes) {
    // Ten handshakes with a wrong code before any guest got in: the host stops listening and
    // fails with "too_many_attempts".
    Peer host, nobody;
    CHECK(hostGame(host));
    DirectInvite inv = host.dm.invite();
    std::string right, wrong;
    CHECK(direct::normalizeJoinCode(inv.code, right));
    do wrong = direct::newJoinCode(); while (wrong == right || wrong == inv.code);
    for (int i = 0; i < 10; ++i) {
        RawGuest g;
        CHECK(!g.connect(inv.port, wrong));
    }
    CHECK(waitUntil(host, nobody, [&] { return host.dm.state() == DirectMatch::State::Failed; }));
    CHECK_EQ(host.dm.lastError(), std::string("too_many_attempts"));
}

TEST(direct_gaps_host_answers_a_second_hello) {
    // A second Hello on an established link is a ProtocolViolation; the link stays. A message
    // the authority does not take, then one cut short, are answered the same way they are by the
    // authority itself.
    Peer host;
    RawGuest raw;
    P::GameSnapshot snap;
    CHECK(joinRaw(host, raw, snap));
    P::Hello hello;
    hello.proto = P::kProtocolVersion;
    hello.token = "direct:Raw      ";
    CHECK(raw.send(hello));
    P::Error e;
    CHECK(raw.waitFor(e, 5000));
    CHECK(e.code == P::ErrorCode::ProtocolViolation && e.ref == raw.seq && !e.fatal);
    P::QueueLeave q;
    CHECK(raw.send(q));
    CHECK(raw.waitFor(e, 5000));
    CHECK(e.code == P::ErrorCode::ProtocolViolation && e.ref == raw.seq);
    P::Resign res;
    res.seq = ++raw.seq;
    res.game = snap.game;
    std::vector<uint8_t> cut;
    P::encode(res, cut);
    cut.resize(cut.size() - 2);
    CHECK(raw.sendBytes(cut));
    CHECK(raw.waitFor(e, 5000));
    CHECK(e.code == P::ErrorCode::Malformed && e.ref == raw.seq);
    P::C_Ping ping;
    ping.nonce = 99;
    CHECK(raw.send(ping));
    P::S_Pong pong;
    CHECK(raw.waitFor(pong, 5000));
    CHECK_EQ(pong.nonce, 99u);
    Peer nobody;
    CHECK(waitUntil(host, nobody, [&] { return host.dm.currentGame() != nullptr; }));
    CHECK(host.count(Event::Kind::GameEnd) == 0 && host.dm.currentGame()->status == int(P::GameStatus::Ongoing));
}

// ---- the guest against a host written by hand ---------------------------------------------------

TEST(direct_gaps_guest_queues_commands_while_reconnecting) {
    // The link drops during the game: the draw commands the player gives meanwhile are kept and
    // go, in order, once the guest is back (after its Hello).
    RawHost raw;
    CHECK(raw.listen());
    direct::Authority auth(direct::AuthorityConfig(), "Alice", "Bob", 1);
    Peer guest, nobody;
    P::Hello hello;
    CHECK(joinHand(guest, raw, auth, hello));
    raw.drop();
    CHECK(waitUntil(guest, nobody, [&] { return guest.hasConn(ConnState::Reconnecting); }));
    guest.dm.offerDraw();
    guest.dm.answerDraw(true);
    guest.dm.claimDraw();
    CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
    CHECK_EQ(hello.seq, 1u);
    CHECK(sendWelcome(raw, auth));
    P::DrawOffer offer;
    P::DrawAnswer answer;
    P::DrawClaim claim;
    CHECK(raw.waitFor(offer, 5000) && raw.waitFor(answer, 5000) && raw.waitFor(claim, 5000));
    CHECK(offer.game == auth.gameId() && answer.game == auth.gameId() && claim.game == auth.gameId());
    CHECK(answer.accept);
    CHECK(offer.seq < answer.seq && answer.seq < claim.seq);
}

TEST(direct_gaps_guest_dropped_by_a_stance_flood) {
    // A host that sends stances faster than the game thread polls cannot grow the guest's event
    // queue without limit either: the guest drops the link and comes back.
    RawHost raw;
    CHECK(raw.listen());
    direct::Authority auth(direct::AuthorityConfig(), "Alice", "Bob", 1);
    Peer guest, nobody;
    P::Hello hello;
    CHECK(joinHand(guest, raw, auth, hello));
    P::S_Stance s;
    s.game = auth.gameId();
    s.stance = P::Stance::Standing;
    std::vector<uint8_t> buf;
    P::encode(s, buf);
    for (int i = 0; i < 5000 && raw.sendBytes(buf); ++i) {}
    CHECK(raw.waitClosed(3000));
    CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
    guest.drain();
    CHECK(guest.count(Event::Kind::OpponentStance) > 1000);
    CHECK(guest.count(Event::Kind::OpponentStance) <= 4096);
    CHECK(guest.hasConn(ConnState::Reconnecting));
}

TEST(direct_gaps_guest_broken_frame) {
    // A frame whose tag does not verify: during the game the guest reconnects; in place of the
    // Welcome, the first join ends with "closed".
    {
        RawHost raw;
        CHECK(raw.listen());
        direct::Authority auth(direct::AuthorityConfig(), "Alice", "Bob", 1);
        Peer guest, nobody;
        P::Hello hello;
        CHECK(joinHand(guest, raw, auth, hello));
        CHECK(raw.sendRaw(forgedFrame()));
        CHECK(waitUntil(guest, nobody, [&] { return guest.hasConn(ConnState::Reconnecting); }));
        CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
    }
    {
        RawHost raw;
        CHECK(raw.listen());
        Peer guest;
        guest.dm.join("127.0.0.1", raw.port, raw.code, "Bob");
        P::Hello hello;
        CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
        CHECK(raw.sendRaw(forgedFrame()));
        CHECK_EQ(failedJoin(guest), std::string("closed"));
    }
}

TEST(direct_gaps_guest_first_join_refused_in_hello) {
    // What the host does with the guest's Hello, before any Welcome: an Error saying the protocol
    // is not supported is "incompatible", any other Error "refused", a close "closed".
    for (int variant = 0; variant < 3; ++variant) {
        RawHost raw;
        CHECK(raw.listen());
        Peer guest;
        guest.dm.join("127.0.0.1", raw.port, raw.code, "Bob");
        P::Hello hello;
        CHECK(raw.accept(5000) && raw.waitFor(hello, 5000));
        if (variant < 2) {
            P::Error e;
            e.ref = hello.seq;
            e.code = variant == 0 ? P::ErrorCode::UnsupportedProtocol : P::ErrorCode::ServerFull;
            e.fatal = true;
            CHECK(raw.send(e));
        } else {
            raw.drop();
        }
        const char* expected = variant == 0 ? "incompatible" : variant == 1 ? "refused" : "closed";
        CHECK_EQ(failedJoin(guest), std::string(expected));
        CHECK(guest.dm.state() == DirectMatch::State::Failed);
    }
}

TEST(direct_gaps_guest_first_join_not_a_direct_match) {
    // The port answers, but not with a direct match's handshake: bytes of another protocol, or
    // the right magic with another version byte. Both are "incompatible".
    for (int variant = 0; variant < 2; ++variant) {
        RawHost raw;
        CHECK(raw.listen());
        Peer guest;
        guest.dm.join("127.0.0.1", raw.port, raw.code, "Bob");
        CHECK(raw.acceptSocket(5000));
        std::vector<uint8_t> bytes(direct::SecureChannel::kHelloLen, 0);
        if (variant == 0) {
            static const char http[] = "HTTP/1.1 400 Bad Request\r\n\r\n";
            std::memcpy(bytes.data(), http, sizeof http - 1);
        } else {
            std::memcpy(bytes.data(), "SCDM", 4);
            bytes[4] = 0x7E;
        }
        CHECK(raw.sendRaw(bytes));
        CHECK_EQ(failedJoin(guest), std::string("incompatible"));
    }
}
