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
