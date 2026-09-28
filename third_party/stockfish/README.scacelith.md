# Stockfish 16 in Scacelith

Scacelith's opponent is [Stockfish](https://stockfishchess.org) 16, compiled into the game
executable and run in-process. Stockfish is free software under the **GNU General Public License
v3.0** (`Copying.txt`, authors in `AUTHORS`), which is why Scacelith as a whole is GPL-3.0.

## Provenance

* Stockfish 16 (release of 2023-06-30), taken from Ubuntu's source package `stockfish 16-1build1`
  (noble): `stockfish_16.orig.tar.gz`, sha256
  `a928bcf1ba7894a14ff15f78f2b4081396c5ed3cf5f2aab29b0827423fd883e8`.
* `src/` is byte-identical to `src/` of that upstream tarball, except that the generated
  `src/.depend` build file was not copied. Debian's patches only touch `src/Makefile`; the pristine
  upstream Makefile is kept here for reference (the game does not use it).
* `src/nn-5af11540bbfe.nnue` is Stockfish 16's default network (40,119,326 bytes, sha256
  `5af11540bbfefcb54e38c5dd000cab4b469dfa7599a1d55be5d2722c20a8929b`, matching its name).
* `Copying.txt` and `AUTHORS` come from the same tarball.

## Modifications

**No upstream file is modified.** Everything Scacelith-specific lives next to it:

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Static library `stockfish_embedded` (all upstream sources except `src/main.cpp`) |
| `scacelith/entry.cpp` | `stockfish_embedded_main()`: the body of upstream `main()` as a function |
| `scacelith/cpu.cpp` | `stockfish_embedded_supported()`: SSE4.1 + POPCNT check, built without those flags |
| `scacelith/stockfish_embedded.h` | The only header the game sees |

## Build

* Flags of the upstream Makefile for `ARCH=x86-64-sse41-popcnt`: `-O3 -fno-exceptions -DNDEBUG
  -DIS_64BIT -DUSE_POPCNT -DUSE_SSE41 -DUSE_SSSE3 -DUSE_SSE2 -msse -msse2 -msse3 -mssse3 -msse4.1
  -mpopcnt`, plus `-DUSE_PTHREADS` on Linux (as the Makefile does on non-MinGW targets). No LTO.
  The game's global Windows definitions (`UNICODE`, `WIN32_LEAN_AND_MEAN`, ...) are not applied.
* The network is embedded with Stockfish's own `INCBIN(EmbeddedNNUE, "nn-5af11540bbfe.nnue")` in
  `evaluate.cpp` (an assembler `.incbin`, works with Linux GCC and MinGW-w64 GCC). The assembler
  resolves that relative path itself, so `evaluate.cpp` gets `-Wa,-I<this>/src`, and it depends on
  the `.nnue` file for rebuilds.
* The CPU check (`scacelith/cpu.cpp`) is compiled with the default x86-64 flags; `ai::Engine::start`
  refuses to start the engine on a CPU without SSE4.1/POPCNT instead of crashing.

## Running in-process

`src/ai/uci_host.cpp` redirects `std::cin` / `std::cout` to thread-safe in-memory stream buffers
(a blocking line queue for input, a line splitter for output) and calls
`stockfish_embedded_main()` on a `std::thread`. Nothing else in the game may use `std::cout` /
`std::cin` (see `docs/ARCHITECTURE.md`). `src/ai/engine.cpp` is the UCI client.

Checked properties:

* **Embedded network only, no file access.** `EvalFile` defaults to `nn-5af11540bbfe.nnue`, which
  `Eval::NNUE::init()` loads from `"<internal>"` first; the working and binary directories are only
  tried if that failed. Verified with `strace -f` (Linux) and `WINEDEBUG=+file` (Windows build under
  wine): no `.nnue` file is ever opened. The fake `argv` (`"stockfish"`) makes
  `CommandLine::init` call only `getcwd()`.
* **Clean shutdown.** `quit` makes `UCI::loop` return (no `exit()` on that path); the entry function
  then calls `Threads.set(0)` and returns, and the host joins the thread and restores the streams.
* **Restart.** Every init step re-initialises its globals, so a new session can be started after
  `quit` (tested repeatedly). Only one session can run at a time (global state).
* **Process exit without shutdown.** The host registers an `atexit` handler on first start (so it
  runs before the destructors of Stockfish's globals) that sends `quit` and joins. The wait is
  bounded (2 s): on Windows `exit()` holds the CRT's atexit lock while running handlers, and an
  engine thread that is still initialising can block on it (registering the destructor of a
  function-local static such as the `sync_cout` mutex); the thread is then left parked until the
  process ends. Found and verified with the Windows build under wine.
* **`Use NNUE` toggling** (the weakest preset plays with the classical evaluation, evaluations use
  the network) never reloads the network: `Eval::NNUE::init()` sees it is already loaded.
* Remaining `exit()` calls in Stockfish are failure paths only: hash allocation failure, a network
  that fails to load (impossible for the embedded default; the game never sets `EvalFile`), corrupt
  Syzygy files (the game never sets `SyzygyPath`), debug log / bench file errors (never used).

Start-up (`uci` + `isready` until `readyok`): 0.4-0.75 s cold on Linux and 0.17-0.26 s under wine on
the (busy) build machine, 40-110 ms for later sessions in the same process (the network stays in
memory). Most of it is parsing the 40 MB network and allocating the hash table.

Executable size: +40.7 MB for the Windows build (40.1 MB network + ~0.6 MB code and tables),
plus ~1.3 MB of zero-initialised tables at run time.

## No AVX2 variant

A second build of the engine with AVX2 (namespace renamed, selected at run time) was tried and
dropped: the two builds compile the same C++ standard library templates with different
instruction sets, and those instantiations are weak/COMDAT symbols that the linker merges
program-wide. The AVX2 copy of e.g. `std::vector<...>::_M_default_append` could then be called
from baseline or game code on a CPU without AVX2 (many Pentium/Celeron chips) and crash. Isolating
them needs symbol renaming with objcopy (and PE/COFF COMDAT section quirks), which is not worth
it: SSE4.1 Stockfish is already far beyond any human, and every other preset is strength-limited.

## Strength presets

See `src/ai/presets.cpp`. Stockfish 16's handicap (`Skill Level`, or `UCI_LimitStrength` +
`UCI_Elo` 1320..3190, which maps onto a fractional skill level) searches at least 4 principal
variations and, after iteration `1 + int(level)`, picks one of them at random, biased towards
the better ones. `UCI_Elo 1320 == Skill Level 0` is its floor.

### Calibration of the weak presets

Stockfish offers nothing below `UCI_Elo 1320` (= Skill Level 0), so the three presets under it
combine Stockfish's own knobs, measured with Stockfish 16 self-play (a standalone build of these
sources; 200-600 games per pairing, colours alternating, a referee engine detects the end of the
game and adjudicates at 300 plies with a depth-12 evaluation, |eval| > 3 pawns = win).
Results are Elo(A) - Elo(B) with 95% intervals; "S0" = Skill Level 0, "d1" = `go depth 1`.

| A | B | games | Elo A-B |
|---|---|---:|---:|
| S0, movetime 30 ms | S0 d1 | 200 | +156 [+106, +212] |
| UCI_Elo 1500, movetime 30 ms | S0, movetime 30 ms | 200 | +109 [+61, +163] |
| S0 d1, MultiPV 5 | S0 d1 | 300 | -72 [-112, -33] |
| S0 d1, MultiPV 6 | S0 d1 | 300 + 600 | -111, -167 |
| S0 d1, MultiPV 7 | S0 d1 | 300 + 600 | -235, -216 |
| S0 d1, MultiPV 8 / 9 / 10 / 12 | S0 d1, MultiPV 7 | 300 each | -21 / -64 / -99 / -101 |
| S0 d1, MultiPV 10 | S0 d1 | 600 | -304 [-345, -268] |
| S0 d1, MultiPV 10 | S0 d1, MultiPV 6 | 600 | -135 [-165, -106] |
| S0 d1, classical eval (`Use NNUE` false) | S0 d1 | 600 | -207 [-241, -176] |
| S0 d1, MultiPV 6, classical | S0 d1, MultiPV 6 | 2 x 600 | -179, -189 |
| S0 d1, MultiPV 7, classical | S0 d1, MultiPV 7 | 600 | -192 [-226, -162] |
| S0 d1, MultiPV 7, classical | S0 d1, MultiPV 6 | 600 | -179 [-212, -150] |
| S0, movetime 100 ms | S0, movetime 30 ms | 150 | +40 [-15, +96] |

Findings:

* **Depth caps do weaken Skill Level 0, but indirectly.** Level 0 always commits after
  iteration 1, yet with a normal search budget the deeper iterations (of this and earlier moves)
  fill the hash table that the shallow multi-PV scores then use: `depth 1` costs ~156 Elo. So the
  UCI_Elo presets are never depth-capped (that would silently weaken them), and the weak ones use
  `depth 1` on purpose. A node cap is no alternative: one that interrupts iteration 1 leaves
  unsearched root moves and yields arbitrary, not human-like, moves.
* **MultiPV** widens the list the random pick is made from (6: ~-150, 7: ~-220, saturating near
  10: ~-300).
* **`Use NNUE` false** (Stockfish 16's classical evaluation) costs another ~160-210 at depth 1.
* Chains of these measurements are not perfectly transitive (the players are very random), so the
  numbers are good to roughly +-50. The hash-table effect is mostly saturated at 30 ms per move
  (+40 at 100 ms), so taking "S0 at 30 ms" as Stockfish's 1320 may put the estimates below
  ~40 Elo too high.

With Skill Level 0 at normal time as 1320 (Stockfish's anchor):

| Preset | Settings | Estimate |
|---|---|---|
| Novice (~800) | Skill 0, depth 1, MultiPV 7, classical eval | 750-840 |
| Beginner (~1000) | Skill 0, depth 1, MultiPV 6 | ~1015 |
| Casual (~1200) | Skill 0, depth 1 | ~1165 |
| Club Player ... Grandmaster | UCI_Elo 1500 / 1800 / 2100 / 2400 / 2700 | Stockfish's calibration |
| Stockfish Max | full strength | ~3500 (1 thread) |

These are engine ratings on Stockfish's CCRL-anchored scale; human (FIDE or online) ratings are
not the same scale, so the labels are approximate by nature.
