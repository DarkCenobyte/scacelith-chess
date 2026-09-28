# Scacelith

A photorealistic chess game played in first person, seated at a waxed wooden table in a sunlit
royal hall, against a porcelain robot driven by Stockfish. Everything is rendered by an in-house
engine written for this game on OpenGL 4.6 (no third-party engine): procedural geometry,
real-time material shaders, physically based lighting.

## Playing

- **Left click** on one of your pieces: your hand reaches for it. Tournament rules apply: a piece
  you touch must be moved if it has a legal move (touch-move).
- **Left click** on a square: the piece is played there. Captured pieces are put beside the board.
- **Space**, or a click on the chess clock: your hand presses the clock. A move is only completed
  once the clock is pressed, and after a promotion the new piece must be on the board first.
- **Right mouse button** (hold and drag): look around from your chair. **Mouse wheel**: lean
  towards the board. **Middle click** or **C**: look at the board again.
- **Tab**: move list. **Esc**: menu (offer or claim a draw, resign, options).

A new game asks for the opponent's strength (Stockfish presets, or custom Skill Level / Elo /
depth / move time / nodes) and the time control (unlimited, 1+0, 3+0, 3+2, 5+0, 5+3, 10+0, 10+5,
15+10, 30+0, 30+20, 90+30, or custom with increment and delay). Your colour is random for the
first game, then alternates. Every physical action (touching, moving, capturing, promoting,
pressing the clock) takes exactly the same time for both players, and each clock only stops when
the lever is actually pressed.

Illegal moves are only possible when legal-move hints are turned off (Options > Gameplay). As in
tournaments, the arbiter then restores the position, gives the opponent extra time, and a second
illegal move loses the game.

### Scoresheets

As in tournaments, both players record the game on their own scoresheet, a pad lying on the side
of the table opposite the clock. Right after every clock press each player writes the move down
with the hand on the pad side, while the other hand stays free to play and press the clock, so
keeping score never costs clock time. The player whose clock stands on the left therefore plays
with the left hand and writes with the right. The header is filled in when the game starts (event
"Scacelith", date, round, the players' names and ratings: your name from Options > Player,
"Human" by default (translated with the interface), and "Stockfish"), a full page of 40 moves is turned over the top of the pad,
and the result is written before the final handshake. You hear the pen on the paper and the page
being turned.

Each player has a handwriting of their own (Caveat, Marck Script or Bad Script for Latin and
Cyrillic; names in other scripts are written in Aref Ruqaa, Klee One or LXGW WenKai), and moves use
the piece letters of the interface language.

### Your Elo

Your rating starts at 1500 and is updated after every game against Stockfish with the FIDE
formula: K = 40 for your first 30 games (the rating is provisional until then), 20 afterwards, 10
once you have reached 2400; a rating gap is counted as 400 points at most. The opponent's rating is
the preset's (Novice 800 up to Stockfish Max 3500; a custom opponent is rated from its Skill Level
or UCI Elo). A game counts once both players have moved; leaving a game, or closing the game,
resigns it. The title page shows your rating, record and peak, the new game page the score you
can expect against the selected opponent, and the game over card the change
(`Elo 1512 → 1524 (+12)`). Watched games are never rated.

## Watch a Game

**Watch a Game** on the title page lets two Stockfish players (one preset per side, and a time
control) play each other while you move freely around the hall, invisible to them. The choice is
remembered (`[viewer]` in the settings file).

- **W A S D** or **Z Q S D** (AZERTY) or the arrows: move along the view. **E / Space** and
  **C / Ctrl** (or Page Up / Down): up and down. **Shift**: three times faster.
- **Right mouse button** (hold and drag): look around. **Mouse wheel**: movement speed.
- **1 – 9**: fly to a viewpoint (beside the table, above the board, the hall, the clock, White's
  face, Black's face, the duel, the windows, the tapestries). **0**: through the eyes of the player
  to move; after each move the camera flies over the table into the other player's eyes.
- **Tab**: move list. **H**: hide the controls. **Esc**: menu (resume, options, main menu).

The players claim and offer draws like the opponent of a normal game (repetition, fifty moves, an
equal position late in the game), and the game over card offers to watch another game.

## Online play

**Play Online** on the title page plays people through a Scacelith server, still in the first
person: your opponent sits in the other chair as a robot that moves the pieces and presses the
clock by itself, as soon as their move arrives.

- **Server.** The official server is `caissa.scacelith.com` (port 44664, secure web API and
  secure WebSocket on the same port). Options > Online server > Custom server takes a community
  server instead: host (domain or IP), HTTPS/API port, WSS port (empty = the API port) and, for a
  server with a self-signed certificate, its **Certificate fingerprint (SHA-256)** as its owner
  gives it (64 hexadecimal characters, colons allowed; empty = the Windows certificate store).
  **Test connection** shows the server's name, message and whether it runs a compatible
  version. You sign in separately on each server: an account and its sign-in are never shared
  between servers, and nothing secret is written to `Scacelith.ini`.
- **Account.** Sign in with your user name or e-mail and password (and the code of your
  authenticator app once two-factor authentication is on), or with Google. New accounts confirm
  their e-mail address (the page can send the link again); a forgotten password is reset by
  e-mail. The account page lists your rating in every time control (`1500?` while it is
  provisional, with games and wins / draws / losses), changes the password, turns two-factor
  authentication on (QR code or key, then ten recovery codes shown once) or off, makes new
  recovery codes, and signs you out here or everywhere.
- **Finding a game.** Pick a time control (your rating in each is under it), rated or casual, and
  **Find opponent**: a card counts the waiting time and shows the rating range searched. You can
  also challenge a player by name (any time control; only the official ones can be rated, with
  the colour you want), or create a private game whose short code a friend enters to play you.
  Challenges you receive appear as a card wherever you are in the menus, and at the table
  between two games.
- **At the table.** You can only let go of a piece on a legal square; the move is sent the
  moment you choose it, and the robot hand then places the piece and presses the clock. The
  clocks are the server's (they never stop, not even in the Esc menu). The ping to the server is
  in the top right corner. Esc: offer or claim a draw, resign, abort before your first move,
  report the opponent, leave (which resigns). If your opponent loses the connection a banner
  counts down the time they have to come back; if yours drops, the game waits behind a
  "Reconnecting…" veil and picks up where the server is. The scoresheets are headed with the
  server's name, "Online", the time control, rated or casual, both players with their ratings and
  the game's number; the game over card shows the rating change and offers a rematch.
- **Direct match.** Two computers play each other directly, without a server or an account
  (friendly games, never rated): one player **hosts** (time control, colour, port 47100 by
  default, opened on the home router with UPnP when possible) and reads the address, the port and
  a code (`XXXX-XXXX-XXXX`) to the other, who **joins** with them. The page says whether the
  router opened the port, when to forward it by hand, and when the internet provider shares the
  address (carrier-grade NAT: try IPv6 or a VPN).

Development: `--online-mock` replaces the network with an in-process fake server and a fake
direct-match friend (any password works; see `src/game/online_mock.h` for the inputs that try
error paths), and `--start-online [category]` goes straight to a game (with `--online-mock` the
opponent is a random mover). In a mock game F9 makes the opponent disconnect for a while and F10
drops your own connection. `--scene ui --ui-screen online-play` (and the other `online-*` and
`direct-*` screens listed in `src/ui/ui_viewer.cpp`) shows the pages on the fake server.
[docs/ONLINE_CLIENT.md](docs/ONLINE_CLIENT.md) describes the client side. QR codes are drawn with
Nayuki's [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) (MIT licence,
`third_party/qrcodegen/`).

## Options

Settings are stored in `Scacelith.ini` next to the executable when that folder is writable,
otherwise in `%APPDATA%\Scacelith\`. All of them are editable from the Options page: display
mode and resolution, V-sync, render scale, quality preset, motion blur, depth of field,
brightness, volumes, ambience, legal-move hints, mouse sensitivity.

The interface speaks English, French, German, Spanish, Ukrainian, Russian, Arabic (laid out right
to left), Japanese, Simplified Chinese and Traditional Chinese. The first start follows the
system language; Options > Display > Language changes it at once. Options > Player holds your
name and the handwriting in which you fill in your scoresheet, with a preview. Translations live
in `assets/i18n/<code>.lang` (one `key = text` per line, English is the reference and the
fallback); `--lang <code>` overrides the language for one session.

## Building

Requirements: CMake 3.20+, Ninja, a C++17 compiler. The Windows build is produced with
MinGW-w64 (native or cross-compiled from Linux) and is a single self-contained executable
(Stockfish and its neural network are embedded).

```sh
# Windows x64 (cross-compiled from Linux)
cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build-win            # -> build-win/Scacelith.exe

# Linux (development, tests and headless screenshots)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/scacelith_tests
```

Development options: `--scene <name>` runs a viewer scene (`--list-scenes`), `--data-dir .`
reads shaders from disk and **F5** reloads them, **F12** saves a screenshot,
`--shot out.png --frames N --size 1280x720` renders headlessly. `tools/shot.sh` and
`tools/shot_win.sh` do this under Xvfb (Linux build and Windows build through Wine).

The game itself (`--scene game`, the default) takes `--start` (a game against Stockfish at once;
`--human white|black`), `--viewer` (a watched game at once; `--white-preset N --black-preset N`
with N an index of the preset list, `--viewpoint 0..9`), `--tc N` (time control index), `--cam
x,y,z [--look x,y,z] [--fov deg]` (initial observer camera when watching; a detached camera in a
normal game), `--handover-preview` (watching through the players' eyes with the clock frozen during
each camera handover, as planned for [hot-seat](docs/MULTIPLAYER_PLAN.md)), `--no-intro`, `--warp
<seconds>` (simulate before the first frame), `--moves e2e4,e7e5,...` and `--ini <file>`.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the engine is organised.

## Licence

Scacelith is free software under the GNU General Public License v3.0 (see `LICENSE`), because it
embeds [Stockfish](https://stockfishchess.org) (GPL-3.0), whose source is in
`third_party/stockfish/` with its own copyright notices. The Cinzel, EB Garamond and Amiri
(Khaled Hosny) interface fonts and the handwriting fonts Caveat (Impallari Type), Marck Script
(Denis Masharov), Bad Script (Gaslight), Aref Ruqaa (Abdullah Aref, Khaled Hosny), Klee One
(Fontworks) and LXGW WenKai / WenKai TC (LXGW) are under the SIL Open Font License 1.1; the
subsets shipped here are rebuilt from the upstream files by `tools/prepare_fonts.py`. The chess
figures of the promotion picker come from a subset of GNU FreeFont FreeSerif (GPL-3.0+ with the
font exception). All licence texts are in `assets/fonts/` and `assets/fonts/hand/`.
