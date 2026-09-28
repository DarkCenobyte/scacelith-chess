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

## Options

Settings are stored in `Scacelith.ini` next to the executable when that folder is writable,
otherwise in `%APPDATA%\Scacelith\`. All of them are editable from the Options page: display
mode and resolution, V-sync, render scale, quality preset, motion blur, depth of field,
brightness, volumes, ambience, legal-move hints, mouse sensitivity.

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
`third_party/stockfish/` with its own copyright notices. The Cinzel and EB Garamond fonts are
under the SIL Open Font License; the chess figures of the promotion picker come from a subset of GNU
FreeFont FreeSerif (GPL-3.0+ with the font exception). Their licence texts are in `assets/fonts/`.
