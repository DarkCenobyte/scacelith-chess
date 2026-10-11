# Scacelith

[![CI](https://img.shields.io/github/actions/workflow/status/DarkCenobyte/scacelith-chess/ci.yml?branch=master&event=push&label=CI&logo=githubactions&logoColor=white)](https://github.com/DarkCenobyte/scacelith-chess/actions/workflows/ci.yml)
[![Release build](https://img.shields.io/github/actions/workflow/status/DarkCenobyte/scacelith-chess/release.yml?event=push&label=release%20build&logo=githubactions&logoColor=white)](https://github.com/DarkCenobyte/scacelith-chess/actions/workflows/release.yml)
[![CodeQL](https://img.shields.io/github/actions/workflow/status/DarkCenobyte/scacelith-chess/codeql.yml?branch=master&event=push&label=CodeQL&logo=github)](https://github.com/DarkCenobyte/scacelith-chess/actions/workflows/codeql.yml)
[![Coverage](https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2FDarkCenobyte%2Fscacelith-chess%2Fbadges%2Fcoverage.json)](https://github.com/DarkCenobyte/scacelith-chess/actions/workflows/coverage.yml)
[![Latest release](https://img.shields.io/github/v/release/DarkCenobyte/scacelith-chess?sort=semver&display_name=tag&logo=github)](https://github.com/DarkCenobyte/scacelith-chess/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/DarkCenobyte/scacelith-chess/total?logo=github)](https://github.com/DarkCenobyte/scacelith-chess/releases)
[![Licence](https://img.shields.io/github/license/DarkCenobyte/scacelith-chess?color=blue)](LICENSE)
[![Last commit](https://img.shields.io/github/last-commit/DarkCenobyte/scacelith-chess/master)](https://github.com/DarkCenobyte/scacelith-chess/commits/master)

[![Top language](https://img.shields.io/github/languages/top/DarkCenobyte/scacelith-chess)](https://github.com/DarkCenobyte/scacelith-chess)
[![Languages](https://img.shields.io/github/languages/count/DarkCenobyte/scacelith-chess)](https://github.com/DarkCenobyte/scacelith-chess)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
![OpenGL 4.6](https://img.shields.io/badge/OpenGL-4.6-5586A4?logo=opengl&logoColor=white)
[![Stockfish 19](https://img.shields.io/badge/Stockfish-19-2B2B2B)](https://stockfishchess.org)
[![Online protocol](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FDarkCenobyte%2Fscacelith-chess%2Fmaster%2Fprotocol%2Fscacelith-v1.json&query=%24.minor&prefix=v1.&label=online%20protocol&color=6E4C9E)](protocol/README.md)
![Windows x64](https://img.shields.io/badge/Windows-x64-0078D4)
![Linux x86-64 | aarch64](https://img.shields.io/badge/Linux-x86--64%20%7C%20aarch64-FCC624?logo=linux&logoColor=black)
![macOS arm64 (experimental)](https://img.shields.io/badge/macOS-arm64%20%28experimental%29-555555?logo=apple&logoColor=white)

[![Website](https://img.shields.io/website?url=https%3A%2F%2Fscacelith.com&label=scacelith.com&up_message=online&down_message=offline)](https://scacelith.com)
[![Official server](https://img.shields.io/website?url=https%3A%2F%2Fcaissa.scacelith.com%2Freadyz&label=official%20server&up_message=online&down_message=offline)](https://scacelith.com)
[![Dedicated server](https://img.shields.io/github/v/release/DarkCenobyte/scacelith-chess-server?sort=semver&display_name=tag&label=dedicated%20server&logo=rust)](https://github.com/DarkCenobyte/scacelith-chess-server)
[![Built with Claude Code](https://img.shields.io/badge/built%20with-Claude%20Code-D97757?logo=claude&logoColor=white)](https://claude.com/claude-code)

A photorealistic chess game played in first person, seated at a waxed wooden table in a sunlit
royal hall, against a porcelain robot driven by Stockfish. Everything is rendered by an in-house
engine written for this game on OpenGL 4.6 (no third-party engine): procedural geometry,
real-time material shaders, physically based lighting.

- **Play** Stockfish 19 at any strength, under tournament rules (touch-move, a chess clock you
  press by hand, a scoresheet you fill in), with an Elo of your own.
- **Coach**: a robot teacher that plays at your level, talks you through the game and shows your
  mistakes on the board, and short challenges (positions of the Lichess puzzle database).
- **Analysis** of your saved games and of any PGN, **Watch a Game** between two Stockfish
  players, and **two players on one PC**.
- **Online**: rated games, challenges and private games on the official server or a community
  one ([scacelith-chess-server](https://github.com/DarkCenobyte/scacelith-chess-server)), or a
  direct match between two computers.
- The interface in English, French, German, Spanish, Ukrainian, Russian, Arabic, Japanese,
  Simplified Chinese and Traditional Chinese.

How to play and every mode and option are described in [docs/GUIDE.md](docs/GUIDE.md).

## Installing

The [releases](https://github.com/DarkCenobyte/scacelith-chess/releases) hold the game for Windows
x64, Linux x86-64 and Linux aarch64, and an experimental build for Apple Silicon Macs; all need a
GPU with OpenGL 4.6 (the Mac build brings its own), and Stockfish 19 is built in.

- **Windows**: unpack `Scacelith-<version>-windows-x64.zip` anywhere and run `Scacelith.exe` (or
  download the executable alone).
- **Linux**: unpack `Scacelith-<version>-linux-x86_64.tar.gz` and run `./scacelith`;
  `./install.sh` adds the game to your applications menu (in `~/.local`, no root needed) and
  `./install.sh --uninstall` removes it. It needs glibc 2.34 and OpenSSL 3 (Ubuntu 22.04,
  Debian 12, Fedora 36, RHEL 9 and later), X11 or Wayland with XWayland, and plays its sound
  through ALSA (`libasound2`; PulseAudio and PipeWire through their ALSA plugins). Without
  `libasound2` or a sound device the game runs silent, the coach in subtitles. Saved logins go to
  the desktop's keyring through libsecret (`libsecret-1-0`) when it is installed (a locked keyring
  asks for its password when you sign in or connect), otherwise to a file only you can read: the
  game says so after the sign-in, and Options > Online server can keep them until the game closes
  instead (`install.sh` says when libsecret is missing, and the log says why the file is used).
- **Linux on 64-bit Arm**: `Scacelith-<version>-linux-aarch64.tar.gz`, the same as above, for an
  aarch64 system with a GPU driver of OpenGL 4.6: NVIDIA (Jetson Orin, discrete cards), AMD
  discrete cards, Asahi Linux on Apple M1/M2. A Raspberry Pi 4 or 5 cannot run it (OpenGL 3.1).
- **macOS (experimental)**: `Scacelith-<version>-macos-arm64-experimental.dmg`, for Apple Silicon
  Macs (M1 or later) with macOS 26 or later; Intel Macs are not supported. Open it and drag
  Scacelith to Applications. macOS offers no OpenGL 4.6, so the app carries Mesa's (Zink on the
  KosmicKrisp Vulkan driver, over Metal); this build is new and less tested than the others. It is
  not signed by a registered Apple developer: the first time, macOS refuses to open it, and
  **System Settings > Privacy & Security > Open Anyway** lets it (or `xattr -dr
  com.apple.quarantine /Applications/Scacelith.app` in a terminal).

## Building

You need CMake 3.21 or later, Ninja, GCC with GNU binutils (tested with GCC 11 and 13; for Windows,
MinGW-w64 GCC with POSIX threads) and Python 3; the Linux build also needs the OpenSSL 3, X11 and
OpenGL development files. Stockfish 19 is in the repository (`third_party/stockfish/`) and is
built with the game. The first configure downloads the coach's voice model (399 MB, from
Supertone's repository on Hugging Face) for the unit tests; `-DSCACELITH_SUPERTONIC_DOWNLOAD=OFF`
does without it.

```sh
# Linux x86-64 or aarch64 (Debian, Ubuntu:
# apt install build-essential cmake ninja-build python3 libssl-dev libx11-dev libgl-dev)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build                # -> build/scacelith
./build/scacelith_tests

# Windows x64, cross-compiled from Linux
# (apt install g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64)
cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build-win            # -> build-win/Scacelith.exe, self-contained
```

The macOS app (experimental) builds its own Mesa first: see
[tools/macos/README.md](tools/macos/README.md). The build options (`cmake -D<option>=<value>`):

| Option | Default | Effect |
|---|---|---|
| `SCACELITH_SF_VARIANTS` | every one | the Stockfish builds to compile, one per instruction set; `x86-64-avx2` (or another one the CPU runs) alone makes a quicker local build |
| `SCACELITH_STOCKFISH` | `ON` | embed Stockfish (`OFF`: a build without the engine, for tests) |
| `SCACELITH_OFFICIAL_SERVER` | `caissa.scacelith.com:443` | the built-in official server, `host[:apiPort[:wsPort]]`; `none` for no official server |
| `SCACELITH_CONSOLE` | `OFF` | Windows: a console executable, which shows the log |
| `SCACELITH_SUPERTONIC_DIR` | | a local copy of the coach's voice model, instead of the download |
| `SCACELITH_SUPERTONIC_DOWNLOAD` | `ON` | download the voice model when no local copy is given |
| `SCACELITH_SUPERTONIC_REQUIRED` | `OFF` | a missing or unverifiable voice model file fails the configure |
| `SCACELITH_SANITIZE` | | a test build with sanitizers, for instance `address,undefined` |
| `SCACELITH_COVERAGE` | `OFF` | Linux, GCC: instrument the game's code for gcov (test coverage) |

[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) has the details: the Linux aarch64 cross-build, the
tests under Wine and the sanitizers, the X11 desktop tests, the test coverage, the audit tools,
the continuous integration and the releases.

## Command-line options

The game runs without any option. These ones start it in a given way (`Scacelith.exe <options>`
on Windows, `./scacelith <options>` on Linux, `/Applications/Scacelith.app/Contents/MacOS/Scacelith
<options>` on a Mac):

| Option | Effect |
|---|---|
| `--lang <code>` | the interface language for this session: `en`, `fr`, `de`, `es`, `uk`, `ru`, `ar`, `ja`, `zh-Hans` or `zh-Hant` |
| `--ini <file>` | read and write the settings in that file, with the saved logins beside it, instead of the user folder |
| `--calibrate` | the brightness calibration before the title page, as on a first start |
| `--coach-dir <folder>` | read (and download) the coach's voice model in that folder |
| `--start [--human white\|black] [--tc N]` | a game against Stockfish at once, with your colour and the time control (its index in the New game list, from 0) |
| `--start --hotseat` | a game between two players on one PC at once: `--white-name <name>`, `--black-name <name>`, `--rated`, `--handover <seconds>` (0: an instant cut) |
| `--start --coach [--coach-level 0..6] [--coach-colour white\|black]` | a coach game at once (level 0 is the rules lesson) |
| `--start --coach --coach-challenge <id> [--coach-challenge-position N]` | a challenge of the Coach page at once (its ids are the `challenge` lines of `assets/coach/challenges/challenges.txt`), at its position N, from 1 |
| `--viewer [--white-preset N] [--black-preset N] [--viewpoint 0..9]` | Watch a Game at once |
| `--analyse <file.pgn> [--game G] [--analysis-at P]` | game G of a PGN file (the first by default) in the Analysis mode, at the position after P plies |
| `--replay <file.pgn> [--game G] [--replay-speed x1\|x2\|x4\|x8\|instant] [--replay-paused]` | the replay at the table of game G of a saved games file |
| `--no-intro` | skip the fade-in before a game |
| `--shot <file.png> [--frames N] [--size WxH] [--warp <seconds>]` | render N frames (30 by default), save the last one as a PNG and quit, leaving the settings and the saved games as they are; `--warp` simulates that many seconds first |
| `--scene <name>`, `--list-scenes` | a viewer scene instead of the game, the list of scenes |
| `--data-dir <folder>` | read the shaders from that folder (**F5** reloads them) instead of the embedded ones |
| `--debug-gl` | an OpenGL debug context |

[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#development-options) lists the options made for tests
and screenshots (scripted moves, cameras, the UI viewer's screens).

Environment variables:

| Variable | Effect |
|---|---|
| `SCACELITH_GPU_PROFILE=N` | the time of each rendering pass on the GPU, in the log every N frames |
| `SCACELITH_GL_DISABLE=tessellation,depth_clamp` | treat these driver features as missing (one or both) |
| `SCACELITH_GL_FORCE=tessellation` | keep tessellation on a driver known to draw nothing with it |
| `SCACELITH_AUDIO=null` | no sound output |
| `SCACELITH_ALSA_DEVICE=<device>` | Linux: the ALSA device to play on, instead of `default` |
| `SCACELITH_KEYRING=off` | Linux, macOS: never use the system keyring for the saved logins |
| `SCACELITH_COACH_SOURCE=archive\|official` | the voice model's download from Supertone's archive copy first, or from the official repository without falling back |
| `SCACELITH_EGL_LIBRARY=<path>` | macOS: the libEGL to use when the app's own cannot be loaded |
| `SCACELITH_DUMP_SHADERS=<folder>` | write the preprocessed shader sources into that existing folder |

## Licence

Scacelith is free software under the GNU General Public License v3.0 (see `LICENSE`), because it
embeds [Stockfish](https://stockfishchess.org) (GPL-3.0-or-later). The third-party parts it is made
with are listed below with their licences. The release packages carry the licence texts of the
fonts, of Stockfish's authors, of the QR code generator and of the opening names (`licences/`).

### Compiled or embedded into the game

- **[Stockfish](https://github.com/official-stockfish/Stockfish) 19** (tag `sf_19`,
  `third_party/stockfish/`): GPL-3.0-or-later, © the Stockfish developers
  (`third_party/stockfish/AUTHORS`). Its neural network `nn-1a298aa575a0.nnue` (CC0-1.0,
  [official-stockfish/networks](https://github.com/official-stockfish/networks)) was trained on data
  provided by the Leela Chess Zero project, which is made available under the Open Database License
  (ODbL). It is embedded with Dale Weiler's [incbin](https://github.com/graphitemaster/incbin)
  (public domain, Unlicense).
- **[QR Code generator](https://www.nayuki.io/page/qr-code-generator-library)** (C++,
  `third_party/qrcodegen/`), for the two-factor setup page: MIT, © Project Nayuki
  (`third_party/qrcodegen/LICENSE`).
- **[stb_truetype](https://github.com/nothings/stb)** v1.26 (`third_party/stb/`), the font
  rasteriser: public domain (Unlicense) or MIT, at the user's choice, © Sean Barrett.
- **OpenGL API headers** of the Khronos Group (`third_party/khronos/`, `glcorearb.h` and
  `khrplatform.h`; only their declarations are used): MIT, © The Khronos Group Inc.
- **Opening names** from lichess's [chess-openings](https://github.com/lichess-org/chess-openings)
  (`assets/coach/openings_data/`): CC0-1.0 (`assets/licences/lichess-chess-openings-CC0.txt`).
- **Puzzles** of the [Lichess puzzle database](https://database.lichess.org/#puzzles), the
  positions of the coach's challenges (`assets/coach/challenges/challenges.txt`, some mirrored so
  that White is to play): CC0-1.0.
- **Unicode Character Database** 14.0, from which the NFKD table of the coach's voice
  (`src/tts/nfkd_table.cpp`) is generated: Unicode licence (Unicode-DFS-2016), © Unicode, Inc.
- **Fonts.** The Cinzel, EB Garamond and Amiri (Khaled Hosny) interface fonts and the handwriting
  fonts Caveat (Impallari Type), Marck Script (Denis Masharov), Bad Script (Gaslight), Aref Ruqaa
  (Abdullah Aref, Khaled Hosny), Klee One (Fontworks) and LXGW WenKai / WenKai TC (LXGW) are under
  the SIL Open Font License 1.1; the Arabic and CJK fonts (Amiri, Aref Ruqaa, Klee One, LXGW WenKai
  / WenKai TC) are subset and renamed from the upstream files by `tools/prepare_fonts.py`, the
  others are the upstream files unchanged. The chess figures of the promotion picker come from a
  subset of GNU FreeFont FreeSerif (GPL-3.0+ with the font exception). All licence texts are in
  `assets/fonts/` and `assets/fonts/hand/`.
- **The compiler's runtime libraries**, linked into the Windows executable and the Linux release
  build: libstdc++ and libgcc (GPL-3.0-or-later with the GCC Runtime Library Exception; libstdc++
  includes [Ryu](https://github.com/ulfjack/ryu), Apache-2.0 or BSL-1.0, and, in the Windows build,
  [fast_float](https://github.com/fastfloat/fast_float), Apache-2.0 or MIT). The Windows executable
  also links the [MinGW-w64](https://www.mingw-w64.org) runtime (ZPL-2.1 and other permissive
  licences, parts in the public domain) and winpthreads (MIT and BSD-3-Clause).

### Shipped with the macOS app

The macOS app (`Contents/Frameworks`) carries, built from source by `tools/macos/build-deps.sh`
and listed with their versions in `Contents/Resources/macOS-dependencies.txt`:
- **[Mesa](https://mesa3d.org)** 26.2.4 (its EGL, the Zink OpenGL driver and the KosmicKrisp Vulkan
  driver, with the patches of `tools/macos/mesa-patches/`): MIT and other permissive licences, ©
  the Mesa authors; it compiles in
  [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) (Apache-2.0, © The Khronos Group
  Inc.).
- **The [Vulkan loader](https://github.com/KhronosGroup/Vulkan-Loader)** 1.4: Apache-2.0, © The
  Khronos Group Inc., Valve Corporation, LunarG, Inc.
- **[OpenSSL](https://www.openssl.org)** 3.6 (linked into the executable): Apache-2.0, © The
  OpenSSL Project Authors.

Their licence texts are in the app's `Contents/Resources/licences/`.

### Code adapted from other projects

These parts of the game's own code follow published code closely:

- Text normalisation of the coach's voice (`src/tts/text.cpp`): Supertone's
  [Supertonic](https://github.com/supertone-inc/supertonic) sample code (`py/helper.py`, v3.0.0),
  MIT, © 2025 Supertone Inc.
- Move and game accuracy of the game review (`src/coach/review.cpp`, `src/coach/appraisal.cpp`):
  lichess's [lila](https://github.com/lichess-org/lila) (`AccuracyPercent`), AGPL-3.0-or-later.
  Game phases (`src/coach/tactics.cpp`): lichess's
  [scalachess](https://github.com/lichess-org/scalachess) (`Divider`), MIT, © Thibault Duplessis.
- AgX tone mapping (`shaders/post/tonemap.frag`, `src/render/post/display_transform.h`):
  [three.js](https://github.com/mrdoob/three.js), MIT, © three.js authors.
- Sky and atmosphere (`shaders/lighting/atmosphere.glsl` and `multiscatter_lut.comp`): Sébastien
  Hillaire's [UnrealEngineSkyAtmosphere](https://github.com/sebh/UnrealEngineSkyAtmosphere), MIT,
  © 2020 Epic Games, Inc., with Eric Bruneton's transmittance parametrisation
  ([precomputed_atmospheric_scattering](https://github.com/ebruneton/precomputed_atmospheric_scattering)),
  BSD-3-Clause, © 2008 INRIA and 2017 Eric Bruneton.
- Round cone distance of the chess set and the robot (`src/scene/sdf_mesher.cpp`,
  `src/character/sdf.h`): [Inigo Quilez](https://iquilezles.org/articles/distfunctions/), MIT.
- History clipping of the temporal anti-aliasing (`shaders/post/post_common.glsl`): Playdead's
  [temporal](https://github.com/playdeadgames/temporal), MIT, © 2015 Playdead.
- BRDF details (`shaders/include/brdf.glsl`): Google's
  [Filament](https://github.com/google/filament), Apache-2.0.
- PCG32 random numbers (`src/math/math.h`, `src/audio/dsp.h`): Melissa O'Neill's
  [pcg-c-basic](https://github.com/imneme/pcg-c-basic), Apache-2.0.
- Audio queue (`src/audio/queue.h`): Dmitry Vyukov's bounded MPMC queue
  ([1024cores.net](https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue)),
  simplified BSD licence, © 2010-2011 Dmitry Vyukov.

### Used from the system, not shipped

On Linux the game uses the system's GNU C Library (LGPL-2.1-or-later), OpenSSL 3 (Apache-2.0),
libX11 (MIT/X11), OpenGL library (libglvnd, MIT) and graphics driver, ALSA (libasound,
LGPL-2.1-or-later, loaded at run time) and libsecret (LGPL-2.1-or-later, loaded at run time when
present, with GLib); on Windows, the system's DLLs; on macOS, the system's frameworks and
libraries.

### Downloaded at the player's request

The coach's voice model (Supertonic 3) is not part of the program nor of its release package: the
game downloads Supertone's official, unmodified files from Supertone's repositories on Hugging Face
at the player's request. It has its own licence (BigScience Open RAIL-M), whose use restrictions
the download prompt shows; see
`third_party/supertonic3/README.scacelith.md`.

## Links

- Official website: [scacelith.com](https://scacelith.com)
- The dedicated server, to run an online server of your own:
  [DarkCenobyte/scacelith-chess-server](https://github.com/DarkCenobyte/scacelith-chess-server)
- Documentation: [how to play](docs/GUIDE.md), [development](docs/DEVELOPMENT.md),
  [architecture](docs/ARCHITECTURE.md), [online client](docs/ONLINE_CLIENT.md),
  [direct matches](docs/DIRECT_MATCH.md), [ports](docs/PORTS.md),
  [online protocol](protocol/README.md)
