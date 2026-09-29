# Online play: the client side

How the game shows and plays online games. The network layer itself (`src/net/`: HTTPS API,
secure WebSocket, credential store, direct match with UPnP) and the server
(`dedicated-server/`) are described in their own documents; this page covers what sits on top
of them in `src/game/` and `src/ui/`.

## Pieces

| File | Role |
|---|---|
| `src/game/online_session.h/.cpp` | `game::OnlineSession` (one per process, `onlineSession()`): the only code that polls `net::OnlineClient` and `net::DirectMatch`. Keeps what the menus show (server info, account, connection, matchmaking, challenges, cooldown and ban) and hands a started game to the scene. |
| `src/game/game_link.h` | `GameLink`: the commands of one game (move, resign, draw, abort, resync, rematch, report, ping, server clock), for a server game or a direct match alike. |
| `src/game/game_scene_online.cpp` | `GameMode::Online` in the 3D scene: the remote player's robot, move sending, server clocks, resync, end of game. |
| `src/game/online_mock.h/.cpp` | In-process fakes of the server and of a direct-match friend (`--online-mock`). |
| `src/game/online_stub.cpp` | `net::OnlineClient` / `net::DirectMatch` backed by the fakes, compiled only without `SCACELITH_NET_REAL` (builds without the real network layer). |
| `src/ui/ui_screens_online.cpp` | The "Play Online" page and its sub-pages, Options > Online server. |
| `src/ui/ui_online_hud.cpp` | Ping indicator, in-game overlay, online Esc menu, report dialog, challenge cards. |
| `src/ui/ui_qr.cpp`, `ui_clipboard.cpp` | QR code of the two-factor setup (`third_party/qrcodegen`), copy to the Windows clipboard. |

## Session

`OnlineSession::update(dt)` runs once per frame in every state of the game scene (menus and
games). It drains both network objects and sorts the events:

- **HTTPS results** (login, register, account, two-factor...) are stored by kind. A page sends a
  command through `api()`, calls `expect(kind)` (so `busy(kind)` is true meanwhile) and takes the
  answer once with `take(kind, event)`. `LoginResult` also marks the session signed in and opens
  the realtime connection; `AccountResult` refreshes the account.
- **Realtime state** is kept: `conn()` (`ConnState`), `queue()` (searching, category, rated, time
  searched, rating window, players waiting), `outgoing()` (our challenge or private game and its
  code), `incoming()` (challenges received, with their expiry), cooldown and ban ends. Notices
  (server shutdown, session revoked, replaced by another window) and errors become toasts.
- **Game events** go to a queue. A `GameSnapshot` of an unknown game id with status Ongoing is a
  new game (matchmaking, challenge, private game, rematch, direct match): `gameReady()` becomes
  true, the menu (or the game over card) fades to the table, and the scene calls
  `takeGame(snapshot)`, which returns the `GameLink`. Later events of that game come out of
  `nextGameEvent()`.

`ServerError` events that name the current game (or carry a game error code 100-112) go to the
game; the others become toasts. An error with code 0 and `error == "offline"` (a command sent
while not connected, dropped by the network layer) clears the search and the pending challenge.

The server in use comes from `[online]` in the settings: the official server
(`net::officialServer()`, `caissa.scacelith.com:44664`, API and WebSocket on that port) unless
`custom_server = 1`, then `host`, `api_port`, `ws_port` (0 = the API port) and `pinned_sha256`
(certificate fingerprint of a self-signed server). No token is ever stored there: the network
layer keeps one session per origin (`host:apiPort`), so switching servers never reuses another
server's sign-in. `applyServer()` (called when Options are applied with another server) selects
the new origin and forgets the previous server's state; the page then resumes a session saved
for that origin, if any.

## The game at the table

The opponent sits in the other chair as a robot (`Controller::Remote`); the physical game is the
same as against Stockfish, with these differences:

- **My move.** A piece can only be released on a legal square (no arbiter penalties online).
  The move (`packMove`, plus the promotion) is sent with `GameLink::sendMove` the moment the
  destination is chosen, before the hand places the piece; the robot then places it and presses
  the clock by itself (animation only). My clock display is frozen from that moment until the
  server confirms the move (`MoveMade` with `mine`). Moves are written on the scoresheet in ply
  order once confirmed and pressed.
- **The opponent's move.** `MoveMade` queues the move; the robot plays it without thinking
  time (reach, lift, carry, capture, castling rook, promotion swap, clock). I may touch my pieces
  as soon as its pieces are down.
- **Clocks.** The server's values, extrapolated with `serverNowMs()` for the side to move. The
  local clock never flags: a clock at zero shows 0.0 until the server's `GameEnd`.
- **Rejected move / disagreeing snapshot.** Once the robots are idle, the board, the game and the
  scoresheets are rebuilt from the server's move list behind a short fade. The same path sets up
  a game after a reconnection.
- **Events.** Draw offers (card with Accept / Decline), draw declined, opponent disconnected
  (banner "Opponent disconnected — 0:45 to return") and back, first-move timer, rematch offers.
  Our own connection loss shows a "Reconnecting…" veil; the game goes on on the server. The
  client comes back by itself after a random delay, longer when the server is full or restarting
  but never more than 8 s during a game (`dedicated-server/docs/PROTOCOL.md`, lifecycle step 6).
- **Esc menu.** Resume, offer draw, claim draw, abort (before my first move, in place of
  resign), resign, report opponent (server games), options, leave (confirmed: resigns, or aborts
  before my first move). The clock keeps running behind it.
- **End.** `GameEnd` waits for the last move to be on the board, then the usual end (result on
  the scoresheets, handshake, game over card) with the online reasons (abandonment, no show,
  aborted...), the rating change from `RatingUpdate` ("Rating: …" until it arrives), rematch and
  report. A challenge received between two games shows its card over the table.
- **Scoresheet header.** Event = the server's name ("Friendly match" for a direct match), Site
  and time control in the note field ("Online, 5+3 rated" / "Direct, 10+5"), today's date, round
  "-", no board number, both players' names with their ratings (`1500?` when provisional; no
  rating in a direct match), and the game's number in the reference field.
- **Ping.** Top right in online menus and games: green under 80 ms, amber under 200, red above,
  a grey dash while reconnecting. In a direct match it is the latency to the friend. Against a
  server it is the smoothed round trip of the client's own `Ping`: four quick ones after each
  connection, then one every `Welcome.clientPingMs` (the server's `CLIENT_PING_INTERVAL_MS`, 10 s
  by default), which also keeps the estimate of the server clock fresh.

There is no camera handover online: each player sees the game from their own chair.

## Direct match

`OnlineSession::hostDirect()` / `joinDirect()` start `net::DirectMatch`; the direct-match pages
show its state (opening the port, the invitation with address, port and code, the router's
answer, connecting). When the host's game starts, the session announces it like a server game
and the scene plays it through a `GameLink` of kind `Direct`: never rated, names only on the
scoresheets, no report, rematch allowed. Leaving the game (or the page) closes the match, which
removes the port mapping.

## Development

- `--online-mock`: the fakes replace the network (any password works). Inputs that try error
  paths: user names `banned`, `unverified`, `ratelimited`, a name containing `mfa` (asks for a
  code), password `wrong`, a custom host containing `offline`, `badcert` or `old`; direct match:
  address `refused.test`, `timeout.test` or `unknown.test`, a code that is not 12 characters or
  starts with `2222`, host port 47199 (no UPnP router) or 47198 (carrier-grade NAT).
- `--start-online [category]` signs in and plays the first opponent found (at once with the
  fakes); `--touch <square>` then touches that piece once the handshake is over.
- In a mock game, F9 makes the opponent disconnect for 20 s, F10 drops our connection for 8 s.
- `--scene ui --ui-screen <name>`: the pages on the fakes with a frozen clock: `online`,
  `online-register`, `online-mfa`, `online-play`, `online-search`, `online-account`,
  `online-mfa-setup`, `online-recovery`, `online-challenge`, `online-private`, `online-noserver`,
  `direct`, `direct-host`, `direct-wait`, `direct-join`, `online-hud`, `online-pause`,
  `online-report`, `online-gameover`; `--ui-screen options --ui-tab online` for the server
  settings.
