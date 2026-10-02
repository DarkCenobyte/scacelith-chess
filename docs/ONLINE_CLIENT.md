# Online play: the client side

How the game shows and plays online games. The network layer itself (`src/net/`: HTTPS API,
secure WebSocket, credential store, direct match with UPnP) and the server
(`dedicated-server/`) are described in their own documents; this page covers what sits on top
of them in `src/game/` and `src/ui/`, the server in use and the account API calls of
`net::OnlineClient`.

## Pieces

| File | Role |
|---|---|
| `src/game/online_session.h/.cpp` | `game::OnlineSession` (one per process, `onlineSession()`): the only code that polls `net::OnlineClient` and `net::DirectMatch`. Keeps what the menus show (server info, account, connection, matchmaking, challenges, cooldown and ban) and hands a started game to the scene. |
| `src/game/game_link.h` | `GameLink`: the commands of one game (move, resign, draw, abort, resync, rematch, live gestures, report, ping, server clock), for a server game or a direct match alike. |
| `src/game/game_scene_online.cpp` | `GameMode::Online` in the 3D scene: the remote player's robot, move sending, server clocks, resync, end of game. |
| `src/game/online_mock.h/.cpp` | In-process fakes of the server and of a direct-match friend (`--online-mock`). |
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
  (server shutdown, session revoked, replaced by another window) and errors become toasts. The
  `RatingRestored` notice (an opponent of our rated games was banned for cheating; its argument is
  the points given back) fetches the account again, so the ratings shown are the restored ones,
  and shows `online.notice.rating_restored`, but never while a game is being played (a server
  game, a direct match or an offline game alike; the scene tells the session with
  `setInGame()`): it then waits for the game over card or the menus, and the points of several
  notices add up.
- **Game events** go to a queue. A `GameSnapshot` of an unknown game id with status Ongoing is a
  new game (matchmaking, challenge, private game, rematch, direct match): `gameReady()` becomes
  true, the menu (or the game over card) fades to the table, and the scene calls
  `takeGame(snapshot)`, which returns the `GameLink`. Later events of that game come out of
  `nextGameEvent()`, the opponent's live gestures (`OpponentGesture`, below) included; events of
  another game are dropped.

**Live gestures** (`src/net/gesture.h` has the full rules). The scene describes its player's hand
and head with a `net::Gesture` and calls `GameLink::sendGesture()` as often as it likes (every
frame is fine): the network layer keeps only the latest one and paces them.

- Server games (`OnlineClient::sendGesture(gameId, g)`): the rate is the server's, from `Welcome`
  (`gestureRate` per second, bursts of `gestureBurst`; the server's `GESTURE_RATE` and
  `GESTURE_BURST`). The client's own bucket holds one message less than the burst: the server
  takes every gesture while the network delays vary by less than one interval (250 ms at the
  defaults); after a longer stall it drops what arrives at once beyond its burst, the latest state
  included, which the next gesture (at the latest the keepalive, a second later) brings back
  (`net/gesture.h`). Nothing is sent when the rate is 0 (a server without the relay), while not
  `Online` (connecting, reconnecting, offline: a gesture is never kept for the reconnection), or
  for another game than the one of the last `GameSnapshot`. `C_Gesture` shares the message
  numbering of the other commands.
- Direct matches (`DirectMatch::sendGesture(g)`): the same, at the host's 10 per second, bursts of
  20 (`docs/DIRECT_MATCH.md`).
- The opponent's gestures arrive as `OpponentGesture` events with `gesture` and `gameId` (`game`
  is not filled in: a gesture never changes the game state). Only those of the current game are
  kept, and a newer one replaces one the scene has not taken yet, in the network layer and in the
  session's queue alike.

`ServerError` events that name the current game (or carry a game error code 100-112) go to the
game; the others become toasts. An error with code 0 and `error == "offline"` (a command sent
while not connected, dropped by the network layer) clears the search and the pending challenge.

The server in use comes from `[online]` in the settings: the official server
(`net::officialServer()`, `caissa.scacelith.com` on port 443, the HTTPS port: API
`https://caissa.scacelith.com/api/v1` and WebSocket `wss://caissa.scacelith.com/ws`) unless
`custom_server = 1`, then `host`, `api_port`, `ws_port` (0 = the API port) and `pinned_sha256`
(certificate fingerprint of a self-signed server). A build may name another official server with
the CMake option `SCACELITH_OFFICIAL_SERVER` (`host[:apiPort[:wsPort]]`, port 443 when omitted,
`none` for none). No token is ever stored there: the network layer keeps one session per origin
(`host:apiPort`), so switching servers never reuses another server's sign-in. `applyServer()`
(called when Options are applied with another server) selects the new origin and forgets the
previous server's state; the page then resumes a session saved for that origin, if any. Each
answer of the account API names the origin it was asked of (`Event::origin`), and an answer of a
server left meanwhile is not taken for the new server's (`game::ServerAnswers`): the pages drop
it, except a PGN (the game page's Save game writes it where the player asked) and a GIF (saved
where the player asked, by `game::GifSaver`).

The official server used port 44664 before. A player signed in there stays signed in: when the
credential file has a record for `<official host>:44664` and none for the official origin, the
record (user name, token, server id, pin) moves to the official origin the first time the file is
read (`CredentialStore::addOriginMove`; on Windows the token is decrypted for the old origin and
encrypted again for the new one). Only the official host of the build moves, never a community
server.

## Account API

Besides the WebSocket, the server has an HTTPS API under `/api/v1` on the same port
(`dedicated-server/docs/API.md` is the full reference). `net::OnlineClient` uses it for the
sign-in (register, login with its proof of work, two-factor step, Google sign-in, logout), the
account (`/account/me`, password, two-factor setup, recovery codes, reports) and the account API
calls below. Each returns at once; its answer comes back as one event (`ok`, or `error` with the
server's code, `retryAfterSec` when it gave one).

| Call | Request | Event and what it carries |
|---|---|---|
| `fetchMyGames(before, limit, filter)` | `GET /account/games?before=&limit=&category=&rated=&result=` (bearer) | `GamesResult`: `gamesPage` (the games newest first, `next` = the `before` of the next page or 0, `total` = the games matching the filter; `before` and `filter` name the request, errors included) |
| `fetchGame(id)` | `GET /games/:id` (bearer when signed in) | `GameDetailsResult`: `gameDetails` (players, ratings and changes, result, reason, clocks, the moves as UCI text and `packMove` with each move's time spent and clock; `you` and `reportable` for its players) |
| `downloadPgn(id)` | `GET /games/:id/pgn` | `PgnResult`: `gameId`, `text` (the PGN as the server wrote it, `[%clk]`/`[%emt]` comments) |
| `fetchSessions()` | `GET /auth/sessions` (bearer) | `SessionsResult`: `sessions` (id, created, last active, expiry, client label, `current`) |
| `revokeSession(id)` | `DELETE /auth/sessions/:id` (bearer) | `SessionRevoked`: `sessionId` |
| `setAcceptChallenges(on)` | `PUT /account/preferences {acceptChallenges: "all"/"none"}` | `PreferencesResult`: `account.acceptChallenges` |
| `changeEmail(address, password, code)` | `POST /account/email` | `EmailChangeResult`: `status` = `verification_sent` (a link went to the new address) or `email_changed` (`account.email`) |
| `exportAccount(password, code)` | `POST /account/export` | `AccountExportResult`: `text` (the JSON document) |
| `deleteAccount(password, code)` | `POST /account/delete` | `AccountDeleted` |
| `downloadGameGif(id, options)` | `GET /games/:id/gif?size=&orientation=&delay=&coords=0\|1` (bearer, `Accept: image/gif`) | `GifResult`: `gameId`, `text` (the GIF file) |
| `renderPgnGif(pgn, options)` | `POST /gif {pgn, size, orientation, delayMs, coords}` (bearer, `Accept: image/gif`) | `GifResult`: `gameId` 0, `text` (the GIF file) |

Rules common to these calls:

- The query values are percent-encoded (`3+2` goes as `3%2B2`); `limit` is 1 to 50 (0 or less
  asks for the server's default, 20).
- The re-authenticated calls (e-mail, export, deletion) take the password and, when two-factor is
  on, a code: 6 digits go as `code`, anything else as `recoveryCode`, and nothing is sent for an
  empty one.
- A 401 answer to any call that carried the session token means the session is gone (expired,
  revoked elsewhere, the account deleted): the token is erased (the user name stays), as for
  `fetchAccount`, unless it is no longer the token that call sent (a sign-in since, on the HTTP
  thread or the GIF thread: `CredentialStore::clearToken(origin, token)`), and the event carries
  `sessionLost`. The error is then `unauthorized` whatever
  the server's code (the server answers `invalid_token` to a bearer it refuses), as for a call
  that needs the session while none is saved (nothing is sent): the game signs out on that one
  code. `fetchGame` and `downloadPgn` ask again without the token, since a game is public: their
  answer may then be ok, with `sessionLost` telling the game that it is signed out.
- `/account/me` also gives `hasPassword` (false for a Google-only account), `acceptChallenges`,
  `pendingEmail` (an address change waiting for its link), `createdAt` and `lastLoginAt`.
- Answers are untrusted: a move that is not UCI text, a game with another id than the one asked
  for, a PGN that does not start with its tags, an export that is not the export document
  (`format` `scacelith-account-export`), or a PGN over 4 MiB or an export over 64 MiB come back
  as `invalid_response`. The export is checked whole but only its top level is kept in memory
  while checking, and a document with more than 64 top-level members (the server's has about
  fifteen) is refused there, so a hostile answer can use neither gigabytes of memory nor hours of
  the HTTP thread. The calls give up after 15 s without an answer, the export after 90 s (the
  server allows itself 60 s to write it). An allocation failure during an HTTPS call fails that
  call instead of ending the game: a PGN, an export or a GIF that finds too little memory free
  still answers, `invalid_response`, so that the page or the GIF saver waiting for it ends (and
  the transport takes back the abort action of its request, `net::AbortGuard`).
- The GIFs need the session (each render counts against the account's quota: `rate_limited` and
  `server_busy` come with `retryAfterSec`, from the JSON body or else the `Retry-After` header of a
  proxy's page). They run on a thread of their own (`net-gif`), so that a render of up to 45 s on
  the server never holds up the other calls, and give up after 90 s
  (`OnlineClient::kGifTimeoutMs`). The server's `busy` (all its renderers at work) and any 503
  are `server_busy`; `render_failed` is its own code; a 404 from a server without the GIF routes
  (its router's `not_found` for an unknown path) is `gif_disabled`, as from a server that turned
  them off (for a game's GIF, a public `GET /games/:id` tells that answer from a game that is not
  there, `not_found`). Game 0 (`invalid_game_id`) and a PGN text over 64 KiB (`pgn_too_large`)
  are refused without sending anything. An answer over 16 MiB, or one that does not start with
  `GIF87a` or `GIF89a`, is `invalid_response`.
- `deleteAccount` success erases the token and the user name saved for the origin (its server id
  and pin stay) and stops the realtime connection without reconnecting. `revokeSession` on the
  session marked `current` in the last `fetchSessions` signs this game out the same way (token
  erased, connection stopped).

## The game at the table

The opponent sits in the other chair as a robot (`Controller::Remote`); the physical game is the
same as against Stockfish, with these differences:

- **My move.** A piece can only be released on a legal square (no arbiter penalties online).
  Who presses the clock is the authority's rule for the game (`OnlineGame::autoPress`: on by
  default on the server, the host's choice in a direct match):
  - `autoPress` on: the move (`packMove`, plus the promotion) is sent with
    `GameLink::sendMove` the moment the destination (and the promotion piece) is chosen, before
    the hand places the piece; the robot then places it and presses the clock by itself
    (animation only).
  - `autoPress` off: the move is staged. The robot places it and waits, the player presses the
    clock (Space or a click on it, as against Stockfish) and the move is sent at the lever's
    contact, so that its thinking time covers the placement and the press, as the server's lag
    compensation assumes. A staged move is not sent when the game ended or a rebuild happened
    meanwhile (the board is then set up again from the authority's moves). A notice at the start
    of such a game says so (`notify.manual_clock`).

  The thinking time sent with the move is measured on a steady local clock from the opponent's
  `MoveMade` (or the start of the game), not as a difference of two `serverNowMs()` readings:
  a correction of the server clock estimate between them would make an honest time look
  implausible to the server. My clock display stands still from the send until the server
  confirms the move (`MoveMade` with `mine`), but no longer than max(1 s, 3 pings) and never
  while reconnecting: the authority's clock runs meanwhile, and so does the display. A move sent
  just as the connection dropped (or pressed while it was being restored) is sent again, same
  ply and move, when the snapshot after the reconnection holds every local move but that one
  and it is still my turn there (the authority answers a duplicate with the original
  `MoveMade`), instead of the board being rebuilt without it; my clock display then stands still
  again, at the authority's time of the snapshot (the outage was charged to my clock). Moves are
  written on the scoresheet in ply order once confirmed and pressed.
- **The opponent's move.** `MoveMade` queues the move; the robot plays it without thinking
  time (reach, lift, carry, capture, castling rook, promotion swap, clock), from where their live
  gestures left it: the piece already in its hand is carried on, and a move already put down
  only gets the clock press. It starts at once: anything else their gestures left to the robot
  (another piece in hand or on its way back, another move put down) is dropped and the board set
  back from the game, behind a short dip when a piece had moved, so the move takes its usual
  time whatever came before it and gestures never hold my turn back while my clock runs. I may
  touch my pieces as soon as its pieces are down.
- **Live gestures** (the rules and their tests: `src/game/online_live.h`,
  `tests/online_live_tests.cpp`). Hot-seat and Stockfish games send and receive none.
  - Mine (`sendOnlineGesture`): built every frame while the game is played (intro, handshake,
    play; nothing while reconnecting): the square of the piece in hand, the legal square it is
    aimed at once the pointer has rested there for 120 ms, `Promoting` while the promotion
    picker is open, `placed` for a staged move from its placement to my press (with `autoPress`
    the hand is idle again once the move is sent), the look of my robot's eyes and the wheel
    lean, `Glance` while I read my scoresheet (S) and `Side` when the look falls on the table
    beside the board (clock, captured pieces, scoresheets). `ply` is the number of plies played
    when the hand's state began (at most the plies played now: a board rebuilt after a refused
    move starts it again). It goes to `GameLink::sendGesture` when that state changes, when
    the head turns by about a degree or the lean by 0.05, and once a second at least; after the
    end of the game, one last idle gesture and nothing more.
  - The opponent's (`OpponentGesture`) are cosmetic and untrusted: they never touch the game,
    the arbiter, the clocks or `og_`. Their piece fields move the robot only while the local
    game has exactly `ply` plies, it is their turn, my robot is done with my move, their
    `MoveMade` for that ply has not come, no rebuild is pending, the game goes on, and the
    gesture came after the last snapshot and after both players' last reconnection; the square
    must hold one of their pieces, the aim and `placed` must be legal there. The robot then takes
    the piece (reach, lift), carries it over the square aimed at once the aim has held for
    150 ms (back over its own square after 0.6 s without one), puts a `placed` move down (the
    placement without the press; the game is not changed), and puts the piece back when their
    hand is empty again, when their gestures stop for 5 s (our own connection lost included),
    when they disconnect and at the end of the game. Its hand takes one step at a time: a carry
    or a change of piece (taking one, going on to another, letting go) waits until it has at
    most 50 ms left of the previous one, then follows their latest gesture, so gestures that
    come faster than it can play them (a modified client may send any) never pile up. A move put
    down that its `MoveMade` does not confirm (another move, at once as above; a rebuild, the
    end, or 5 s of gestures showing something else) is taken back: once the hands are idle the
    board is set up from the game again, behind a short dip.
  - Their head: while their last gesture is under 2.5 s old and both players are connected,
    the robot's head follows their look with a critically damped spring and leans with them. A
    `Side` look turns the other way here (each client puts the clock at its own player's
    right, so what lies beside the board is mirrored), and a `Glance` turns the head to the
    robot's own scoresheet on this table, its writing hand waiting aside meanwhile. While it
    presses the clock, its eyes follow its hand. Otherwise the automatic gaze of a game
    against Stockfish returns, looking at the piece their robot holds, if any. Options >
    Gameplay > "Ignore opponent's head movements" turns the head and the lean off; the piece
    gestures still play.
- **Clocks.** The server's values, extrapolated with `serverNowMs()` for the side to move. The
  local clock never flags: a clock at zero shows 0.0 until the server's `GameEnd`.
- **Rejected move / disagreeing snapshot.** Once the robots are idle, the board, the game and the
  scoresheets are rebuilt from the server's move list behind a short fade. The same path sets up
  a game after a reconnection.
- **Events.** Draw offers (card with Accept / Decline), draw declined, opponent disconnected
  (banner "Opponent disconnected — 0:45 to return") and back, first-move timer, rematch offers.
  Our own connection loss shows a "Reconnecting…" veil; the game goes on on the server. The
  client comes back by itself after a random delay, longer when the server is full or restarting
  but 8 s at most during a game unless the server asked to wait longer (a `Retry-After`): the
  server keeps the game for the reconnection grace, at least 15 s by default and 90 s after a
  restart (`dedicated-server/docs/PROTOCOL.md`, lifecycle step 6). After a restart the server
  also holds the clock of the side to move until that player is back, 20 s at most by default:
  its snapshots then name no running clock, so both clocks stay frozen, and the snapshot that
  follows when the held clock starts (sent to the opponent too) sets them running again. The
  automatic reconnections after a restart reuse the server's `/api/v1/info` answer, as after a
  network failure; a restart that brought another server to the same address (a reinstall,
  another server id in the WebSocket's `101` answer) ends the session as "unauthorized" instead,
  and the saved sign-in is dropped without being sent.
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
scoresheets, no report, rematch allowed. A guest whose link to the host is being restored shows
the "Reconnecting…" veil (`GameLink::reconnecting()`), as in a server game. Leaving the game (or
the page) closes the match, which removes the port mapping.

## Development

- `--online-mock`: the fakes replace the network (any password works). Inputs that try error
  paths: user names `banned`, `unverified`, `ratelimited`, a name containing `mfa` (asks for a
  code), password `wrong`, a custom host containing `offline`, `badcert` or `old`; direct match:
  address `refused.test`, `timeout.test` or `unknown.test`, a code that is not 12 characters or
  starts with `2222`, host port 47199 (no UPnP router) or 47198 (carrier-grade NAT).
- `--online-manual-clock` (with `--online-mock`): the fakes' games, direct matches included, have
  `autoPress` off, so a move goes when its player presses the clock. Without it they have it on
  (a fake direct host then follows `DirectHostOptions::autoPress`).
- The fake opponent sends its live gestures like a real client (`src/game/online_mock.h`): its
  head at 4-6 Hz (over the board while it thinks, leaning in to about 0.6, towards the piece in
  hand, at its clock after pressing it, a glance at its scoresheet for about 2 s after each
  move), the piece touched 0.4-1.3 s before the move, aimed at another square first in about a
  third of the moves, then at its own 250-400 ms before; with `autoPress` off the move is put
  down first (`placed`) and its `MoveMade` comes 0.6-1.0 s later. It is silent while away (F9)
  and once the game is over.
- `--start-online [category]` signs in and plays the first opponent found (at once with the
  fakes); `--touch <square>` then touches that piece once the handshake is over, and
  `--play e2e4,d2d3,...` makes my moves by hand (touch, carry, clock press), so that the fake's
  gestures show on its robot in screenshots (with `--warp <s>`: its moves come at the same times
  from run to run).
- In a mock game, F9 makes the opponent disconnect for 20 s, F10 drops our connection for 8 s.
- `--scene ui --ui-screen <name>`: the pages on the fakes with a frozen clock: `online`,
  `online-register`, `online-mfa`, `online-play`, `online-search`, `online-account`,
  `online-mfa-setup`, `online-recovery`, `online-challenge`, `online-private`, `online-noserver`,
  `direct`, `direct-host`, `direct-wait`, `direct-join`, `online-hud`, `online-pause`,
  `online-report`, `online-gameover`; `--ui-screen options --ui-tab online` for the server
  settings.
