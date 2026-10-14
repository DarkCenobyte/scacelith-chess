# Developing Scacelith

The game's build in detail, its tests and audit tools, the options for development and
screenshots, and how the continuous integration and the releases work; the
[README](../README.md#building) has the short version of the build.
[ARCHITECTURE.md](ARCHITECTURE.md) describes how the engine is organised and
[PORTS.md](PORTS.md) what was found while preparing the Linux aarch64 and macOS builds.

## Building

Requirements: CMake 3.21+, Ninja, GCC with GNU binutils (tested with GCC 11 and 13; on Windows,
MinGW-w64 GCC with POSIX threads; Clang cannot build the isolated Stockfish variants, and macOS
builds a single one with Apple clang), and
Python 3 for the instruction-set audits and the Windows exception table check that run at every
build (a Windows Release build, the shipped exe, does not configure without it; other builds skip
them with a warning). The Linux build also needs the OpenSSL 3, X11 and OpenGL development files
(for example `libssl-dev`, `libx11-dev` and `libgl-dev`); ALSA (`libasound.so.2`) and libsecret
(`libsecret-1.so.0`, the keyring of the saved logins) are loaded at run time when present, not
linked, and a Linux Release build links libstdc++ and libgcc in, so that
the executable runs on other distributions than the build machine's. The Windows build is produced with
MinGW-w64 (native or cross-compiled from Linux) and is a single self-contained executable
(Stockfish 19 and its neural network are embedded). Stockfish is compiled once per x86-64
instruction set, from plain x86-64 to AVX-512 (on Linux aarch64: ARMv8-A, and with the dot
product), and the game runs the best one the CPU supports;
`-DSCACELITH_SF_VARIANTS=x86-64-avx2` (or another variant the CPU runs) builds a single one, for
quicker local builds (see `third_party/stockfish/README.scacelith.md`). The first configure also
downloads the coach's voice model (seven files, 399 MB, from Supertone's repository on Hugging
Face) into the build folder, for the unit tests and `--coach-dir build/coach`:
`-DSCACELITH_SUPERTONIC_DIR=<folder>` takes a local copy instead (a copy of the repository, with
`onnx/` and `voice_styles/`, or a folder holding the seven files), and
`-DSCACELITH_SUPERTONIC_DOWNLOAD=OFF` does without it (the tests that need the model are then
skipped; see `third_party/supertonic3/README.scacelith.md`). `-DSCACELITH_SUPERTONIC_REQUIRED=ON`
makes a missing or unverifiable model file a configure error, and
`./build/scacelith_tests --require=tts-model` fails a test that skips for want of the model, and a
run where none passed with it (the CI's Linux job does both).

```sh
# Windows x64 (cross-compiled from Linux)
cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build-win            # -> build-win/Scacelith.exe

# Linux x86-64 (the game, its tests and headless screenshots)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/scacelith_tests
# The X11 desktop tests (clipboard, input method, focus, fullscreen) on an Xvfb of their own,
# without then with a window manager (apt install xvfb openbox)
tools/test_x11.sh build

# Linux aarch64: the same commands natively on an aarch64 system, or cross-compiled from Linux
# x86-64 with the tests under qemu-user (see docs/PORTS.md)
cmake -B build-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build-arm64
qemu-aarch64 -L /usr/aarch64-linux-gnu build-arm64/scacelith_tests

# macOS on Apple Silicon (experimental; Xcode's Clang, Homebrew; see tools/macos/README.md)
tools/macos/build-deps.sh     # Mesa, the Vulkan loader and OpenSSL -> build-macos-deps/prefix (once)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DOPENSSL_ROOT_DIR="$PWD/build-macos-deps/prefix/openssl"
ninja -C build && ./build/scacelith_tests
tools/macos/make-app.sh build build-macos-deps/prefix build-macos/Scacelith.app
tools/macos/make-dmg.sh build-macos/Scacelith.app

# The Windows tests under Wine (ninja -C build-win scacelith_tests first)
tools/test_win.sh             # [filter-substring]

# The Linux tests under AddressSanitizer and UBSan (every finding fatal; the timing tests skip)
cmake -B build-san -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSCACELITH_STOCKFISH=OFF \
      -DSCACELITH_SANITIZE=address,undefined
ninja -C build-san scacelith_tests && ./build-san/scacelith_tests
```

## The scenes in the unit tests

The unit tests link the game's code too, all but the platform layer and `main.cpp` (the object
library `scacelith_game_objs`, which the game links as well), and run it without a GPU or a window:
on a null OpenGL driver (`tests/null_gl.cpp`: every entry point does nothing but what the game
reads back, such as object names, compile and link status, limits, mapped buffers and fences; it
counts the draws and the objects alive) and a fake platform layer (`tests/fake_platform.cpp`: a
window of the test's size, a clock the test advances, scripted input, the data folders in a
temporary folder). `tests/headless.h` runs `main.cpp`'s loop around a scene with the game's
command-line options, plays the mouse and the keyboard, and finds the widgets of the last frame by
their label (`ui::im::setTrace`). The null driver's defaults are generated from `glcorearb.h`
(`python3 tools/gen_null_gl.py`, after `tools/gen_gl_loader.py`). The robot, the hall, the chess
set and the font's glyphs are built once per test run (`character::keepBuiltRobot`,
`hall::keepBuiltHall`, `keepBuiltPieces`, `ui::font::keepBuiltGlyphs`; the game leaves them off).
The tests of one file can share one game (`headless::shared`); the test runner stops it before the
next file's tests (its scene holds the engine). MinGW runs the files in the reverse order, so a
test must not count on what a test of another file left behind.

The coach's voice is a third-party system for the unit tests: they never synthesize it in the
Coach and Analysis modes, nor download its model from its known URLs. The Coach and Analysis tests
give the game an empty voice folder (`tests/coach_mode_support.h`), so the coach speaks in
subtitles, and the test runner gives the game's download job the model's manifest without any
source (`game::setCoachModelManifest`): a download started by mistake fails at once. The download
tests serve made-up files from a loopback server.

## Test coverage

`-DSCACELITH_COVERAGE=ON` (Linux, GCC) instruments the game's own code for gcov: the core modules
and the game, not Stockfish, which is built apart (the test that measures the mixer's CPU time
skips, and so do those that synthesize the coach's voice). After a run of the unit tests and of
the X11 desktop tests, [gcovr](https://gcovr.com) reads the counters; the code that no test runs
counts as not covered, and the voice's audio synthesis (`src/tts` but its text front end and the
model store) is not measured:

```sh
cmake -B build-cov -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSCACELITH_COVERAGE=ON \
      -DSCACELITH_SF_VARIANTS=x86-64-avx2
ninja -C build-cov scacelith scacelith_tests && ./build-cov/scacelith_tests && tools/test_x11.sh build-cov
gcovr --root . --filter 'src/' --exclude 'src/tts/(?!text\.|nfkd_table\.|model_store\.)' \
      --txt build-cov                                     # apt install gcovr
```

The Coverage workflow (`.github/workflows/coverage.yml`) does the same on every pull request and
every push to master, with the coach's voice model, and shows the line, function and branch
coverage in the run's summary. On master it also writes the line coverage to `coverage.json` on
the `badges` branch (one commit, replaced at each run), which the README's coverage badge reads
through shields.io.

## Audit tools

The coach's audit (`ninja -C build coach_audit`, Linux only) checks what the coach and the Analysis
comments say against the board: `coach_audit record --out games.rec --pgn games.pgn` (or
`--selfplay N`) stores the engine's analyses of every move, then `coach_audit review --in games.rec`
replays each move through the coach at every level and `coach_audit commentary --in games.rec`
through the Analysis comments, each flagging a claim the board does not bear out (a demonstration
that stops before its point, a piece named that was traded back, a loss the best move suffers too,
a line shown or said that leaves a bigger piece en prise without winning as much back).
`record --material-only` keeps only the moves that leave material en prise, and `--hang-rate R`
makes the weak "human" of `--selfplay` hang a piece on that share of its moves.
See `tools/coach_audit/audit.h`.

The positions of the coach's challenges are picked from Lichess puzzle rows by
`tools/challenges/select.py` (`python3 -I tools/challenges/select.py puzzles.tsv...`), then the
challenge audit (`ninja -C build challenge_audit`, Linux only; run `./build/challenge_audit` from
the repository root) checks each one with the embedded Stockfish, together with the hand-written
endgames of `tools/challenges/endgames.txt`, and writes `assets/coach/challenges/challenges.txt`
with the positions it proves. A full run takes a few hours: `--cache build/challenge_audit.cache`
keeps its verdicts, so that an interrupted run continues where it stopped, and `--max-seconds 180`
drops a candidate too long to settle (the next one of its set is checked instead). See
`tools/challenge_audit/audit.h`.


## Wine and file names

unset, common in containers) that is ASCII, a file named after "Élodie" cannot be created and the
saved games' tests fail although Windows takes the name. `tools/test_win.sh` and
`tools/shot_win.sh` run Wine in a UTF-8 locale (`LC_ALL=C.UTF-8`) when the current one is not.


## Development options

reads shaders from disk and **F5** reloads them, **F12** saves a screenshot,
`--shot out.png --frames N --size 1280x720` renders headlessly (leaving the settings file and the
saved games as they are). `tools/shot.sh` and `tools/shot_win.sh` do this under Xvfb (Linux build
and Windows build through Wine). The Windows exe has no console: redirect what it prints
(`Scacelith.exe --list-scenes > scenes.txt`, or `| more`), or configure with
`-DSCACELITH_CONSOLE=ON` for a console build.

The game itself (`--scene game`, the default) takes `--start` (a game against Stockfish at once;
`--human white|black`), `--viewer` (a watched game at once; `--white-preset N --black-preset N`
with N an index of the preset list, `--viewpoint 0..9`), `--tc N` (time control index), `--cam
x,y,z [--look x,y,z] [--fov deg]` (initial observer camera when watching; a detached camera in a
normal game), `--handover-preview` (watching through the players' eyes with the clock frozen during
each camera handover, the same hand-over as [hot-seat](MULTIPLAYER_PLAN.md)), `--no-intro`,
`--warp <seconds>` (with `--shot`: simulate before the first frame), `--moves e2e4,e7e5,...`, `--touch <square>`,
`--mouse fx,fy` (pointer position as fractions of the window; the view follows it, `0.5,0.03` looks
up at the opponent), `--glance` (start looking at the scoresheet), `--stance
standing|side-left|side-right` (the player gets up once the game is played and the `--play` moves
are made), `--calibrate` (the brightness
calibration before the title page, as on a first start) and `--ini <file>`.

Two players on one PC: `--start --hotseat` starts one at once, with `--white-name N`, `--black-name
N`, `--rated` and `--handover <seconds>` (0 = instant cut). `--play
e2e4,e7e5,...` plays the human moves by hand, the way a player would (touch, carry, press the
clock): both sides on one PC, the human's side against Stockfish (in a coach game, once Stockfish
has started: the coach reviews each move with its analyses); with `--warp` it takes
screenshots of a game in progress, for instance the hand-over halfway (`--start --hotseat
--no-intro --play e2e4 --warp 2.35`). The UI viewer has the hot-seat screens (`--scene ui
--ui-screen newgame-hotseat|hotseat-hud|hotseat-confirm|hotseat-gameover`).

Coach mode: `--start --coach` starts a coach game at once, at the level and colour of the Coach
page unless `--coach-level 0..6` (0 = the rules lesson) or `--coach-colour white|black` say
otherwise; `--coach-dir <folder>` reads (and downloads) the voice model files in that folder instead
of the default one (`--coach-dir build/coach`: the copy a development build prepares).
`SCACELITH_COACH_SOURCE=archive` in the environment skips Supertone's repository (Supertone's
archive copy only, to try the fallback), `SCACELITH_COACH_SOURCE=official` never falls back to the
archive copy, and `--scene ui --ui-screen coach-flow [--coach-dir <folder>]` runs the download flow
over the title page (`coach-download*` screens: the prompt, its update form and the panel with
sample figures; `options-voice-update`: the update row of Options > Audio).
`--coach-stage-test` (alone, or with `--start`) runs a fixed sequence through the scene's coach
stage without the session: a line spoken and subtitled, the coach pointing at g1 and
tracing the knight's jump to f3 on their words, a mark and a highlight, two demonstration moves
taken back by hand, then the takeback card (`--coach-stage-test lesson`: on the lesson's first
position); its log gives the time of each step, for `--warp`. `--coach-auto-answer yes|no` answers
the takeback card (in a challenge, the hint offer) by itself after 1.5 s, for runs with `--play`.
`--start --coach --coach-challenge <id>` starts a challenge of the Coach page at once (its ids are
the `challenge` lines of `assets/coach/challenges/challenges.txt`), `--coach-challenge-position N`
at its position N (from 1). `--analysis-marks-test` puts a
fixed set of the Analysis mode's marks on the board once the pieces stand (tints, the six symbols'
badges, the better move's arrows, straight and a knight's), arriving one after another.

Analysis menus: `--scene ui --ui-screen analysis-page|analysis-page-online|library-analyse` shows
the Analysis page and the Saved games page (on sample games written to `pgn-viewer-sample/` in the
user data folder unless `--ui-library <folder>` is given; `--ui-clipboard <file>` stands in for the
clipboard), `gameover-analyse|gameover-analyse-coach|gameover-analyse-online|gameover-analyse-folded`
the game over card with Analyse the game, and `online-game-analysing` a history game fetching its
PGN for the analysis.

Analysis mode: `--analyse <file.pgn>` opens that file's first game (`--game N`: its Nth, from 1) in
the Analysis mode at once, on the starting position unless `--analysis-at N` names another (the
position after N plies); `--replay-keys` drives it (`J,L,K,Home,End,N,M,B`, `Goto:N` clicks the
panel's position N, `Wait:S` waits S seconds, `Leave` goes back to the menu), and with `--shot`
the review uses shallow depths so that a screenshot shows a finished review. The Analysis mode's
overlay has its UI viewer screens: `--scene ui --ui-screen analysis-hud` (a game
halfway, its review done, a commentary subtitle that `--ui-text <text>` replaces), `analysis-hud-review`
(early on, the review at work), `analysis-hud-fen` (set up from a FEN, Black's move first) and
`analysis-hud-noengine`.

## Continuous integration and releases

GitHub Actions builds and tests every push to master and every pull request
(`.github/workflows/ci.yml`): the Linux build and its unit tests (the coach's voice model
required: its tests must run and pass; the X11 desktop tests under Xvfb, without and with the
openbox window manager), the same on a 64-bit Arm runner (Linux aarch64), the same
tests built with
AddressSanitizer and UBSan, the Windows build cross-compiled with MinGW-w64 and its unit tests
under Wine (all as above), the experimental macOS build (on an Apple Silicon runner with macOS 26:
the bundled Mesa, built once and cached, the game, its unit tests, the app and its disk image,
then a headless screenshot whose failure is only reported; a failure of this job does not fail
the run), the contract with the dedicated
server and the live online tests against it, and a lint of the workflows (actionlint and zizmor).
The server is the commit pinned in `tools/interop/server-revision`, so that a run of a game commit
always tests the same pair (a manual run of the workflow takes another branch of the server as
`server-revision`, for a change made on both sides); a weekly run checks the contract against the
server's master instead. Move the pin forward with every change of the contract; the server pins
this game the same way. The Windows executable and the macOS disk image of each run are kept for
14 days as workflow artifacts, for testing. CodeQL (`.github/workflows/codeql.yml`) scans the shipped C++ code
(without `third_party/` and `tests/`) and the workflows; its alerts are in the Security tab. The
Coverage workflow (`.github/workflows/coverage.yml`) measures the test coverage
([Test coverage](#test-coverage)); it is not one of the checks master requires.

The version is set in `cmake/version.cmake`: `SCACELITH_VERSION_CORE` (`1.0.1`) and, for a
pre-release only, `SCACELITH_VERSION_PRERELEASE` (empty for a release, `rc.1` for instance);
`cmake -P cmake/version.cmake` prints it in full. Pushing the tag `v` + that version (`v1.0.1`)
publishes a release: the release
workflow (`.github/workflows/release.yml`) checks that the tag matches the version, runs the CI
again on the tagged commit, builds the game from scratch (no compiler cache) for Windows x64, for
Linux x86-64 and Linux aarch64 (on Ubuntu 22.04, the oldest toolchain supported, where the unit
tests run again and the executable is checked to need only system libraries and glibc 2.34) and
for macOS on Apple Silicon (the bundled Mesa, Vulkan loader and OpenSSL included; experimental: if
that job fails, the release is published without the disk image), attests the build
provenance of the files with actions/attest and publishes a GitHub release (a pre-release when the
version has a suffix) with `Scacelith-<version>-windows-x64.zip` (the executable, its licence and
the licence texts of what it embeds), the Windows executable alone,
`Scacelith-<version>-linux-x86_64.tar.gz` (the executable, its launcher and icons, `install.sh`
and the same licences), `Scacelith-<version>-linux-aarch64.tar.gz` (the same for aarch64),
`Scacelith-<version>-macos-arm64-experimental.dmg` (the app, signed ad hoc, with the licences of
what it carries) and `SHA256SUMS`. Started by hand, the
workflow builds the same files and keeps them as workflow artifacts without publishing anything,
unless it runs on a tag with "Publish" ticked. To check a downloaded file with the GitHub CLI:

```sh
gh attestation verify Scacelith-1.0.1-windows-x64.zip --repo DarkCenobyte/scacelith-chess
```

Dependabot (`.github/dependabot.yml`) keeps the actions the workflows use up to date (pinned by
commit SHA); the code vendored in `third_party/` and the sources of the macOS build
(`tools/macos/README.md`) are updated by hand.
