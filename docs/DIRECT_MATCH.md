# Direct match

A friendly game between two players by direct connection: no server, no account, never rated.
One player **hosts** (the game opens a TCP port and asks the home router to forward it), the other
**joins** with the host's address, port and a 12-character code. The host's game is the authority
of the match, exactly like the dedicated server is for online games, and both sides speak the same
protocol as online play inside an encrypted channel, so the 3D scene plays a direct match with the
online game code.

Code: `src/net/direct_match.h` (API), `direct_match.cpp` (sessions and threads),
`direct_authority.*` (the host's game rules), `direct_crypto.*` (codes, handshake, frames),
`upnp.*` (router port forwarding), `socket_util.*` (portable sockets). Tests:
`tests/direct_tests.cpp`.

## Hosting

1. Choose the time control (any base from 1 s to 3 h, increment 0 to 180 s), your colour (White,
   Black or random) and host.
2. The game opens TCP port **47100** (or the next free one if the router already uses it for
   another machine), tries UPnP (a few seconds), then shows the invitation:
   - the **public address** (your router's external IPv4, when UPnP found it and it is public),
   - your **local addresses** (for a player on the same network, and your IPv6 addresses),
   - the **port**,
   - the **code**, shown as `K7Q2-M9XH-3PTR`: new for every hosted game, never reused.
3. Give the address, port and code to your opponent (voice, chat...). The game starts as soon as
   they join; each player then has 60 s for their first move.

The first time you host, **Windows Defender Firewall asks whether to allow Scacelith**: allow it on
private networks (and on public networks only if you play from one). Without that permission
nobody can connect, even with a working port forward.

## Joining

Enter the host's address (IPv4, IPv6 without brackets, or a DNS name), the port and the code.
Dashes, spaces and letter case in the code do not matter; the code only uses
`23456789ABCDEFGHJKMNPQRSTUVWXYZ` (no 0/O, 1/I/L to confuse). Common failures:

| lastError | meaning |
|---|---|
| `refused` | nothing listens there: wrong address or port, or the host stopped |
| `timeout` / `unreachable` | the port is not forwarded, a firewall drops it, or the address is wrong |
| `wrong_code` | the host refused the code (or someone in the middle tried to impersonate it) |
| `incompatible` | the other game speaks another protocol version: update both |
| `not_found` | the DNS name does not resolve |
| `invalid_code` | the code is not 12 characters of the alphabet above |

## Ports, routers and firewalls

Only one inbound TCP port is needed on the host's side; the guest needs nothing. The host listens
on IPv6 and IPv4 at once (dual stack) when the system allows it.

**UPnP** (Internet Gateway Device v1/v2): the game searches the router with SSDP from every IPv4
interface, reads its description, finds its WAN connection service (WANIPConnection v2, v1, or
WANPPPConnection) and asks:

- `GetExternalIPAddress` for the public address;
- `AddPortMapping` TCP *external port -> this machine's address, same port*, description
  "Scacelith direct match", lease 3600 s renewed every 30 minutes. A router that only accepts
  permanent leases (error 725) gets lease 0; a port already mapped to another machine (error 718)
  makes the game move to the next free port (at most 10 attempts), and the invitation shows it;
- `DeletePortMapping` when the match ends or the game closes. If the game crashes, a 3600 s lease
  expires by itself; a permanent one stays until the router restarts (look for the description
  above in the router's page to remove it).

The hosting page shows the router's name when the mapping succeeded, the external address and
port in the invitation, or that UPnP found no router, was refused or suspects carrier-grade NAT;
the router's error code is in the log. When UPnP is disabled on the router or not available,
forward TCP 47100 manually to the host's local IPv4 address in the router's settings. NAT-PMP and
PCP (some Apple and ISP routers) are not supported.

**Carrier-grade NAT (CGNAT).** Many mobile and some fibre/cable providers share one public IPv4
between customers. The game suspects it when the router's "external" address is private
(10/8, 172.16/12, 192.168/16), in the shared range 100.64.0.0/10 or in another range the Internet
cannot reach (192.0.0.0/24, where DS-Lite puts the router's IPv4 side; 198.18.0.0/15; 224.0.0.0
and above): nobody can reach you on IPv4 then, whatever the router does. What works instead:

- **IPv6**: if both players have IPv6, the host gives one of its global IPv6 addresses (listed with
  the local addresses). The router's IPv6 firewall may still block inbound connections: allow TCP
  47100 to the host's machine in the router (UPnP IPv6 pinholes are not supported);
- a **VPN** or mesh network between the two players (then use the VPN address);
- let the other player host, or play on an online server.

## The game

The host's game applies the rules of the dedicated server (`dedicated-server/docs/DESIGN.md`
6.1 to 6.4) in a simplified, unrated form, with its own values below and no credit for stalls of
the host itself:

- Each player has 60 s for their first move (no clock runs for plies 0 and 1); otherwise the game
  is aborted. As on the server, the guest's first move has the margin of its flag (the lag
  compensation bound of the next point, beyond the 60 s its countdown shows), so a move made at
  the countdown's last instant still counts. The clocks start with White's second move; Fischer
  increment afterwards.
- The host measures time. The guest's moves get a small network lag compensation: at most
  min(round trip / 2 + 30 ms, 500 ms) per move, from a 2 s budget (+100 ms per move). A move
  arriving after the flag is refused and the game is lost on time (drawn when the opponent cannot
  mate).
- Moves are validated by the host (turn, legality, position digest); a refused move is never
  shown to the opponent.
- Draw offers (with a move or alone; 3 per player per game, not again within 10 plies of a
  decline; a move declines the opponent's offer), claims (threefold repetition, fifty moves),
  automatic endings (mate, stalemate, insufficient material, fivefold, 75 moves), resignation,
  abort before one's own first move.
- **Guest disconnected**: the host notices when the connection closes, or after 10 s without any
  data from the guest; the 60 s grace starts then. The guest's clock keeps running, and the guest
  reconnects by itself with the same code. At the end of the grace the guest loses by abandonment
  (a draw if the host cannot mate; aborted before the second ply).
- **Host gone** (crash, lost network): the guest keeps trying until the grace period ends (plus
  5 s); it gives up sooner when the host's machine refuses the connection three times (the host's
  game is gone), or at once when the host answers with another code or protocol (it hosts a new
  match), and the game then ends as *aborted by the server* on the guest's side.
- **Leaving** a running game (either side) resigns it, as online.
- **Rematch** within 60 s after the end, colours swapped, when both accept.
- **Clock presses**: whether the robots press the clock by themselves is the host's choice
  (`DirectHostOptions::autoPress`, on by default). It is in every `GameSnapshot`
  (`OnlineGame::autoPress`), the first one, after a reconnection or a resync, and in every
  rematch, for both players. When it is off, each player's move is sent when they press the
  clock, so their clock runs until then.

Players appear as user 1 (host) and 2 (guest) with the names they chose, no rating, category
"custom".

**Gestures.** The players' live gestures (the piece in hand, where it is aimed, the move put down
before the clock press, the head; `src/net/gesture.h`) are cosmetic and go straight to the other
player, never through the authority nor the command queue: the host sends `S_Gesture`, the guest
`C_Gesture` (numbered with its other messages). The host's `Welcome` announces the bucket each
side receives with: `gestureRate` 10 per second, `gestureBurst` 20. `DirectMatch::sendGesture()`
keeps only the latest gesture and sends it when a bucket one smaller than that (19) allows;
nothing is kept while the link is down and nothing made then is sent after the reconnection. The
link takes gestures again before the game hears that the other player is there (the snapshot,
`Online` after a reconnection), so the first gesture sent on that news is never dropped. The host
checks the guest's gestures with a bucket of its own (one beyond the rate, or for another game
than the current one, is dropped) and they never count towards the flood limit (more than 40
other messages within a second close the guest's link). The guest's spare token covers network
delays that vary by up to 100 ms; a longer stall of the link can deliver more at once than the
host's bucket holds, and the host drops the excess, the latest state included, until the next
gesture (at the latest the keepalive, a second later) brings it back. A gesture received becomes
an `OpponentGesture` event of that game (the latest one replaces one still waiting to be polled);
neither side ever echoes its own.

## Security design

The code is the only secret: it authenticates both players to each other and keys the channel.

**Handshake** (all integers little-endian unless noted):

| step | bytes |
|---|---|
| guest -> host `GuestHello` | `"SCDM"` \| version `1` \| nonce Ng (32 random bytes) \| P-256 public key Qg (65, uncompressed) |
| host -> guest `HostHello` | `"SCDM"` \| `1` \| Nh \| Qh |
| both | Z = ECDH X coordinate (32 bytes, big-endian); OKM = HKDF-SHA256(salt = Ng \|\| Nh, ikm = Z \|\| code (12 ASCII), info = `"scacelith direct match v1"`, 128 bytes) = K guest->host \| K host->guest \| K guest confirm \| K host confirm; TH = SHA-256(GuestHello \|\| HostHello) |
| guest -> host `GuestConfirm` | AES-256-GCM(K guest confirm, nonce 0, aad `"SCDM guest confirm"`, TH): 48 bytes |
| host | checks it (tag and TH, constant time); on failure closes the connection without a word |
| host -> guest `HostConfirm` | AES-256-GCM(K host confirm, nonce 0, aad `"SCDM host confirm"`, TH): 48 bytes |

**Frames**: u16 length (= ciphertext + 16) | ciphertext | 16-byte tag, AES-256-GCM with the
direction's key, nonce = 4 zero bytes || 64-bit counter (big-endian; 0, 1, 2... per direction),
aad = the two length bytes. One frame = one protocol message of 1 to 16384 bytes. Any tag failure
or out-of-range length ends the connection. The first message of the guest is a protocol `Hello`
(protocol version, schema hash, token `"direct:" + name` padded to 16 bytes); the host answers
`Welcome` then the `GameSnapshot`, or `Error{UnsupportedProtocol}` and closes. Windows uses BCrypt
(ECDH P-256, AES-GCM, SHA-256/HMAC, system RNG); the Linux test build uses OpenSSL.

What this gives:

- **Eavesdroppers** on the path learn nothing and cannot attack the code offline: every key
  depends on the ephemeral ECDH secret. Keys are new for every connection (forward secrecy).
- **Tampering, replay, reordering, truncation** of the game messages are detected (AEAD with
  per-direction counters).
- **Strangers who find the open port** learn nothing that depends on the code: the host only
  answers a guest who proved the code first. Each attempt is one online guess out of 31^12
  (about 2^59), and the host stops listening after 10 wrong codes. Garbage and silence cost a
  stranger a 10 s handshake slot (4 at most at a time) and are not counted as guesses.
- **Mutual authentication**: the guest also checks the host's confirmation, so a fake host is
  detected (the guest sees `wrong_code`).

Limits, accepted for a friendly unrated game:

- **An active attacker on the path who impersonates the host** receives the guest's confirmation,
  which depends only on the code and values the attacker knows: it can test codes offline. With
  59 bits and one HKDF + AES-GCM per guess this costs thousands of CPU-years, the attacker must be
  on the path at that moment, the guest's connection has already failed (it sees `wrong_code`),
  and a found code is only useful while the same hosted game still waits for a guest. A PAKE
  (SPAKE2, CPace) would remove this, but needs elliptic-curve operations BCrypt does not offer.
- **Anyone who has the code is the guest**: a new connection with the code replaces the current
  guest link (this is how reconnection works). Only give the code to your opponent.
- **The host is the authority**: a modified host game can cheat (clocks, results). Direct matches
  are for playing with people you trust; they are never rated.
- **Denial of service**: someone who can reach the port can occupy the handshake slots, or burn
  the 10 wrong-code attempts, which stops the hosting: host again with a new code.
- The router's port is open during the match only, to this one program, which parses nothing
  before the peer proved the code except the 102-byte hello. SSDP answers are only trusted from the
  device that sent them, and only an IPv4 literal on the LAN is contacted.

## Timers

| what | value |
|---|---|
| connect (per resolved address) | 5 s |
| handshake, then Hello -> Welcome | 10 s each (guest); 10 s for both (host) |
| ping (both directions, measures the round trip and the host clock) | every 2 s |
| gestures, each way | 10 per second, bursts of 20 (the sender paces for 19) |
| link considered dead without any data | 10 s |
| first move of each player | 60 s |
| guest disconnection grace | 60 s |
| rematch window | 60 s |
| UPnP discovery / HTTP exchange | 2.5 s / 3 s |
| UPnP lease / renewal | 3600 s / every 30 min |

## Not supported

NAT-PMP and PCP, UPnP IPv6 pinholes, relays (TURN) and hole punching, more than one guest or
spectators, rated games, unlimited time controls, games longer than 1200 plies (the protocol's
limit: as on the server, such a game ends *aborted by the server* after its 1200th ply, and is
kept unfinished).

## Tests

`build/scacelith_tests direct_` and `upnp_` (Linux), `tools/test_win.sh direct_`
(Windows build): UPnP against a fake gateway on 127.0.0.1 (SSDP, relative and absolute control
URLs, chunked HTTP, 718 and 725 policies, errors, foreign control URLs refused), crypto vectors
(SHA-256, HMAC RFC 4231, HKDF RFC 5869, AES-GCM, ECDH RFC 5903), channel failures (wrong code, bad
hello, tampered, replayed, reordered, truncated and oversize frames), the authority with synthetic
time (clocks, lag compensation, flags, first-move timeout and its margin for the guest, draws,
claims, abort, grace, rematch, `autoPress` in every snapshot), and full matches between two
`DirectMatch` on 127.0.0.1
(castling, en passant, promotion, a draw offer declined by a move, resignation, rematch, wrong
code, reconnection through a relay that cuts the connection, the host vanishing, a flag with a
1 s clock, leaving; `autoPress` off through a rematch, gestures both ways and their pacing, none
replayed after a reconnection), a guest written by hand (`Welcome`'s gesture values, 100
gestures at once without tripping the flood limit, the host keeping its bucket's worth) and a host
written by hand (a guest's gesture sent the moment it is back online after a reconnection
arrives).
