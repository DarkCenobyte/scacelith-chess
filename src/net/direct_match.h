// Direct match: a friendly (never rated) game between two players without any server. One
// player hosts (listens on a TCP port, opened on the home router with UPnP when possible), the
// other joins with the host's address, port and a short code the host reads to them.
//
// Implementation notes:
//   - Frame plaintext limit: 16 KiB. A GameSnapshot carries 10 bytes per ply (up to 1200 plies,
//     about 12.2 KB).
//   - The host runs up to 4 handshakes at a time (10 s each). A new connection that proves the
//     code and sends a valid Hello replaces the current guest link: that is how a guest whose old
//     connection is half-open gets back in. The 10-failure limit counts wrong codes only (garbage
//     and timeouts do not, so a port scanner cannot close the game).
//   - host() puts the state at OpeningPort at once (also with UPnP off, while the socket opens);
//     WaitingForGuest follows when invite() is ready.
//   - invite().publicAddress is the router's external IPv4 whenever UPnP learned it and it looks
//     public, even if the mapping itself failed (a manual port forward may exist: upnp().state
//     says whether the router agreed); empty when unknown or when carrier-grade NAT is suspected.
//   - Time control: baseSec is clamped to 1..10800, incSec to 0..180.
//   - Hello from the guest: token = "direct:" + player name, padded with spaces to the schema's
//     16-byte minimum; the host strips both and sanitises the name (1..24 bytes of UTF-8).
//   - lastError(): "network" (both); "port_in_use", "too_many_attempts" (host); "invalid_code",
//     "bad_address", "not_found" (DNS), "refused", "timeout", "unreachable", "reset", "in_use",
//     "closed", "wrong_code", "incompatible", "host_left" (guest). After ConnectionChanged(Offline)
//     the commands have no effect; close() and start again.
//   - DirectHostOptions::autoPress reaches both players in every GameSnapshot (OnlineGame::
//     autoPress): at the start, after a reconnection and in every rematch.
//   - Gestures (sendGesture, net/gesture.h) go straight to the other player, never through the
//     authority nor the command queue: the host sends S_Gesture, the guest C_Gesture (numbered
//     with its other messages). Each side keeps only the latest one and paces them at 10 per
//     second, bursts of 20 (the host's Welcome.gestureRate / gestureBurst; the sender's bucket
//     is one smaller, see gestureSendCapacity); nothing is kept while the link is down. The
//     scenes' keepalive is the host's Welcome.gestureIdleMs, 1 s (gestureKeepaliveMs()). The host
//     drops a guest's Gesture beyond that rate or for another game; Gestures never count towards
//     the flood limit. A received one becomes an OpponentGesture event (the latest replaces one
//     still queued).
//   - Stances (sendStance, protocol minor 2; net/stance.h has the rules) go straight to the other
//     player too, when the player's stance changes and, while not Seated, every keepalive: the
//     host sends S_Stance to a guest whose Welcome negotiated minor 2 or later, the guest C_Stance
//     to a host whose Welcome says minor 2 or later (a host of minor 1 would answer the unknown
//     message with an Error). The host takes the guest's C_Stance before the authority, which never
//     sees it (counted towards the flood limit, never answered nor kept). Each side sends its
//     stance again once the link is back, when it is not Seated. A received one becomes an
//     OpponentStance event, in order with the game events.
//
// The host's game is the authority, exactly like the dedicated server is for online games: it
// validates the guest's move intents with chess::Position, runs the clocks and decides the
// result. Both sides then speak the same binary protocol as online play (net::proto messages,
// protocol/PROTOCOL.md) inside an encrypted channel, and DirectMatch emits
// the same net::Event values as OnlineClient (GameSnapshot, MoveMade, MoveRejected, GameEvent,
// GameEnd, ConnectionChanged, ServerError, OpponentGesture, OpponentStance), so the 3D scene plays
// a direct match with the online game code. The host's own moves go through the same authority
// (no special path).
//
// Secure channel (docs/DIRECT_MATCH.md has the full specification):
//   - The join code: 12 characters from "23456789ABCDEFGHJKMNPQRSTUVWXYZ" (~60 bits), shown as
//     XXXX-XXXX-XXXX, new for every hosted game, never reused.
//   - Handshake: both sides exchange a magic "SCDM", a version byte, a 32-byte random nonce and an
//     ephemeral ECDH P-256 public key. Keys = HKDF-SHA256(ikm = ECDH secret || code, salt = guest
//     nonce || host nonce, info = "scacelith direct match v1") -> one AES-256-GCM key per
//     direction. The GUEST proves knowledge of the code first (an encrypted confirmation over the
//     transcript); the host answers only after checking it, so a stranger who connects to the
//     open port learns nothing that depends on the code. A passive eavesdropper cannot attack the
//     code offline (ECDH). The host accepts at most 10 failed handshakes per hosted game (then it
//     stops listening) and one guest at a time.
//   - Frames: u16 length | AES-256-GCM ciphertext | 16-byte tag; the nonce is a per-direction
//     64-bit counter (no replay, no reordering); plaintext = one net::proto message (<= 16 KiB).
//   - Windows: BCrypt (ECDH P-256, AES-GCM, SHA-256, RNG) and Winsock; Linux dev builds: OpenSSL.
//
// UPnP (Internet Gateway Device, UPnP IGD v1/v2): SSDP discovery on 239.255.255.250:1900,
// device description over HTTP, WANIPConnection (v2, v1) or WANPPPConnection control URL, SOAP
// GetExternalIPAddress / AddPortMapping (TCP, lease 3600 s renewed every 30 min; falls back to a
// permanent lease on error 725 and to the next port on 718) / DeletePortMapping when the match
// ends or the game closes. When the router's external address is private, in 100.64.0.0/10 or
// another range the Internet cannot reach (upnp::cgnatSuspected), the host is probably behind
// carrier-grade NAT: the page says so and suggests IPv6 or a VPN.
//
// Engine-free (no GL, no UI); compiled into scacelith_core and unit-tested (tests/direct_tests.cpp).
#pragma once
#include "online_client.h"   // net::Event, net::OnlineGame, packMove...
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace net {

struct DirectHostOptions {
    uint16_t port = 47100;            // TCP port to listen on (0 = any free port)
    bool upnp = true;                 // ask the router to forward the port
    int baseSec = 600, incSec = 5;    // time control (custom values allowed; never rated)
    int hostColor = 0;                // net::proto::ColorPref: 0 random, 1 White, 2 Black
    std::string playerName;           // written on the scoresheets
    bool autoPress = true;            // the robots press the clock by themselves (OnlineGame::autoPress)
};

struct UpnpStatus {
    enum class State { NotTried, Searching, Mapped, NoGateway, Failed } state = State::NotTried;
    std::string gatewayName;          // friendlyName of the router, if known
    std::string externalIp;           // from GetExternalIPAddress
    uint16_t externalPort = 0;
    bool cgnatSuspected = false;      // external address private / shared (100.64.0.0/10) / not global
    std::string error;                // UPnP error code/description when Failed
};

// What the host tells the guest.
struct DirectInvite {
    std::string publicAddress;        // external IPv4 (UPnP) or empty when unknown
    std::vector<std::string> lanAddresses;  // the host's local IPv4/IPv6 addresses (same network, IPv6)
    uint16_t port = 0;
    std::string code;                 // "K7Q2-M9XH-3PTR"
};

// The guest's address field read as the host's Copy writes it ("[v6]:port CODE",
// "a.b.c.d:port CODE"): its host, and the port and code it carries ("" when none). The code is a
// last word after a space, looked for only when 'withCode' (upper-cased; letters, digits and
// dashes kept); "host:port" is split only with a single ':' (a bare IPv6 address has several) and
// a port of 1 to 5 digits, "[v6]" only with nothing or ":port" after it. Spaces and tabs around
// the field and the code are ignored; anything else is the host as typed.
struct DirectAddress {
    std::string host, port, code;
};
DirectAddress splitDirectAddress(const std::string& field, bool withCode);

class DirectMatch {
public:
    enum class State {
        Idle,
        OpeningPort,     // host: opening the port (and UPnP when enabled)
        WaitingForGuest, // host: listening
        Connecting,      // guest: TCP connect
        Handshake,       // both: secure channel being established
        Playing,         // game running (also after its end until close(), for rematch)
        Failed           // see lastError()
    };

    DirectMatch();
    ~DirectMatch();                   // closes the connection, removes the port mapping, joins threads
    DirectMatch(const DirectMatch&) = delete;
    DirectMatch& operator=(const DirectMatch&) = delete;

    void host(const DirectHostOptions& opt);
    // address: IPv4, IPv6 (without brackets) or a DNS name; code with or without dashes, any case.
    void join(const std::string& address, uint16_t port, const std::string& code, const std::string& playerName);
    void close();                     // leaves: resigns a running game first (like leaving online)

    State state() const;
    std::string lastError() const;    // "refused", "timeout", "wrong_code", "incompatible", "port_in_use", ...
    DirectInvite invite() const;      // host, once WaitingForGuest
    UpnpStatus upnp() const;
    bool isHost() const;

    // ---- the game: same meaning as the OnlineClient methods ----
    void sendMove(int ply, uint16_t move, const std::string& fen, uint32_t thinkMs, bool drawOffer);
    void resign();
    void offerDraw();
    void answerDraw(bool accept);
    void claimDraw();
    void abortGame();
    void requestResync();
    void rematch(bool accept);
    // Live gestures in the current game, straight to the other player over the encrypted link
    // (cosmetic; the latest state only, paced, dropped while the link is down). Cheap enough to
    // call every frame.
    void sendGesture(const Gesture& g);
    // The local player's stance in the current game (protocol minor 2, net/gesture.h and
    // net/stance.h): sent when it changes (250 ms apart at least) and, while not Seated, again
    // every gesture keepalive; again when the link comes back. Nothing to a peer of a minor below
    // 2, nor once the game is over; kept while the link is down. Cheap enough to call every frame.
    void sendStance(uint8_t stance);
    const OnlineGame* currentGame() const;
    int pingMs() const;
    double serverNowMs() const;       // the host's clock (the host: its own)
    int gestureKeepaliveMs() const;   // ms: the host's Welcome.gestureIdleMs, clamped (net/gesture.h)

    bool poll(Event& out);            // drains one event (game thread, once per frame)

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// Process-wide instance used by the game (created on first use).
DirectMatch& directMatch();

}  // namespace net
