# Stockfish 19 in Scacelith

Scacelith's opponent is [Stockfish](https://stockfishchess.org) 19, compiled into the game
executable and run in-process. Stockfish is free software under the **GNU General Public License
v3.0** (`Copying.txt`, authors in `AUTHORS`), which is why Scacelith as a whole is GPL-3.0.
Its network was trained on data provided by the Leela Chess Zero project, which is made available
under the Open Database License (ODbL).

## Provenance

* Stockfish 19 (release of 2026-09-05): tag `sf_19` (a lightweight tag) of
  <https://github.com/official-stockfish/Stockfish>, commit
  `edb0d9db6731067ec50ce619ff372b463bc4dd5d`, root tree `418af042b3c0aade628c1c98f13942659e67e64d`.
* `src/` is byte-identical to `src/` of that commit (git tree
  `15c9967c8add24a07ef4a34522cb6fb5c71ca580`: `git ls-files -s third_party/stockfish/src` lists the
  same blob ids and modes as `git ls-tree -r sf_19 src` in a Stockfish clone), plus the network
  below. That includes files the game does not build: the upstream `Makefile` and `src/universal/`
  (upstream's own multi-architecture build, see [Build](#build)) are kept so that the whole tree
  can be checked against the tag; the generated `src/.depend` is not part of it.
* `src/nn-1a298aa575a0.nnue` is Stockfish 19's default (and only) network, which upstream does not
  keep in git (`make net` downloads it): 98,511,183 bytes, sha256
  `1a298aa575a085434d29027978dc36867fe9c5bcea9376654b7a8eba1e52dfc2`, whose first 12 digits are its
  name, as upstream's `scripts/net.sh` checks.
* `Copying.txt` and `AUTHORS` come from the same commit.

## Modifications

**No upstream file is modified.** Everything Scacelith-specific lives next to it:

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Static library `stockfish_embedded` (all upstream sources except `src/main.cpp`) |
| `scacelith/entry.cpp` | `stockfish_embedded_main()`: the body of upstream `main()` as a function, with a fixed command line |
| `scacelith/local_shm.h` | Replaces upstream `src/shm.h`: the network stays in process memory (forced include) |
| `scacelith/win_shims.h` | Windows: redirects `GetNumaProcessorNodeEx` (Wine) and the console code page calls (forced include) |
| `scacelith/win_shims.cpp` | Windows: the `GetNumaProcessorNodeEx` wrapper |
| `scacelith/cpu.cpp` | `stockfish_embedded_supported()`: SSE4.1 + POPCNT check, built without those flags |
| `scacelith/stockfish_embedded.h` | The only header the game sees |

The shims are headers that CMake force-includes (`-include`) before every Stockfish source file,
so upstream code is compiled as is but sees them first:

* **`local_shm.h`**, all platforms. Upstream's `SystemWideSharedConstant` (`shm.h`) first tries to
  share the unpacked network (115 MB) with every other process running the same executable, and
  no option turns that off: on Linux a directory `/tmp/stockfish-<uid>/sfshm_<hash>/` with a lock
  file and a Unix socket (left behind when the process is killed), a `memfd`, a socket-server
  thread and an `atexit` handler; on Windows a named file mapping and mutex (`Local\sf_...`). It
  only falls back to a private allocation when that fails. The shim defines `shm.h`'s include
  guard and the same public API on top of that private allocation (upstream's own fallback path,
  `make_unique_large_page`), so the engine creates no file, socket, thread or named object.
  Scacelith runs one engine per process, so there is nothing to share. If upstream renames the
  guard or changes the class, both definitions meet and the build fails.
* **`win_shims.h`**, Windows only, with `win_shims.cpp`:
  * `GetNumaProcessorNodeEx` becomes `scacelith_GetNumaProcessorNodeEx`. Wine, and so Proton,
    only stubs that function (`ERROR_CALL_NOT_IMPLEMENTED`, Wine 9.0 to 11.0); upstream then
    builds an empty NUMA configuration while it still finds the L3 caches, and
    `NumaConfig::try_get_l3_aware_config()` calls `std::map::at` on the empty map, which
    terminates the game (`std::terminate`, no exceptions) as soon as the engine starts. The wrapper
    calls the real function and, only when it is not implemented, reports node 0 for every
    existing processor. Windows implements it, so there the wrapper changes nothing.
  * `SetConsoleCP` / `SetConsoleOutputCP` do nothing. Upstream calls them (`set_console_utf8()`) at
    every session start; in a console build (`SCACELITH_CONSOLE`) they would switch the code page
    of the console the game was started from, which outlives the game.

  A macro only redirects direct calls: if upstream ever reached these functions another way (for
  example through `GetProcAddress`), the Wine crash would come back. Hence the Wine check below at
  every Stockfish update.

## Build

* Flags of the upstream Makefile for `ARCH=x86-64-sse41-popcnt` with GCC: `-O3 -funroll-loops
  -fno-exceptions -fno-ipa-cp-clone -fconstexpr-ops-limit=500000000 -DNDEBUG -DIS_64BIT
  -DUSE_POPCNT -DUSE_SSE41 -DUSE_SSSE3 -DUSE_SSE2 -msse -msse2 -msse3 -mssse3 -msse4.1 -mpopcnt`.
  No LTO. Search threads are pthreads with 8 MB stacks with Linux GCC and MinGW-w64 alike
  (`thread_native.h`). The game's global Windows definitions (`UNICODE`, `WIN32_LEAN_AND_MEAN`,
  ...) are not applied. On Windows the library links `shell32` (`CommandLineToArgvW`).
* The network is embedded with Stockfish's own `INCBIN(EmbeddedNNUE, "nn-1a298aa575a0.nnue")` in
  `nnue/network.cpp` (an assembler `.incbin`, works with Linux GCC and MinGW-w64 GCC). The assembler
  resolves that relative path itself, so `network.cpp` gets `-Wa,-I<this>/src`, and it depends on
  the `.nnue` file for rebuilds.
* The CPU check (`scacelith/cpu.cpp`) is compiled with the default x86-64 flags; `ai::Engine::start`
  refuses to start the engine on a CPU without SSE4.1/POPCNT instead of crashing.
* Upstream's `x86-64-universal` build (`src/universal/`, several instruction sets in one binary) is
  not used: cross-compiled with MinGW its final link fails, and it is only safe with
  `--allow-multiple-definition` and a particular link order.

## Running in-process

`src/ai/uci_host.cpp` redirects `std::cin` / `std::cout` to thread-safe in-memory stream buffers
(a blocking line queue for input, a line splitter for output) and calls
`stockfish_embedded_main()` on a `std::thread`. Nothing else in the game may use `std::cout` /
`std::cin` (see `docs/ARCHITECTURE.md`). `src/ai/engine.cpp` is the UCI client.

Checked properties:

* **Embedded network only, no file access.** `EvalFile` defaults to `nn-1a298aa575a0.nnue`, which
  the engine loads from its embedded copy first; the binary and working directories are only tried
  if that failed. `strace -f` of engine sessions (Linux) shows only reads of
  `/sys/devices/system/{cpu,node}` (CPU and NUMA topology): no `.nnue` file, no `/tmp` directory,
  no `memfd`, no socket.
* **Fixed command line.** The entry function passes `argc = 1`, so the UCI loop reads `std::cin`
  until `quit`. On Windows upstream's `CommandLine` ignores the arguments it is given and re-reads
  the process command line (`GetCommandLineW`): the game's own arguments (`--start`, ...) would
  become a one-shot UCI command and the engine would never answer. The entry function sets them
  back after construction.
* **Sessions.** `quit` (or end of input) makes the UCI loop return; destroying the `UCIEngine`
  then joins the search threads and frees the hash table and the network. A new session can be
  started after `quit` (tested repeatedly on Linux and under Wine). Only the attack and Zobrist
  tables are process-wide (filled again, with the same values, by every session), plus a few
  static objects that hold no game state (the processor affinity read at start-up, the output
  mutex, ...). Only one session can run at a time (the standard streams are shared).
* **Restart cost.** Every session parses the network again: `uci` + `isready` until `readyok`
  takes 0.45-1.25 s on Linux and 0.5-0.75 s under Wine on the (busy) build machine, against
  0.4-0.75 s for the first and 40-110 ms for later sessions with Stockfish 16, whose network stayed
  in globals. The game starts the engine once per process, asynchronously.
* **Illegal input ends the process.** Stockfish 19 calls `std::exit(1)` on a `position` command
  with an illegal move or FEN, a malformed `go` argument or a failed `flip`. That exit would run on
  the engine thread, so the host's `atexit` handler (below) would wait for itself and the game
  would vanish without a message. `ai::Engine` therefore replays every move list with the game's
  own rules (`chess::Position::parseUCI`) before sending it; a list that fails is never sent and
  the request fails (empty move, neutral evaluation), which the game already handles. It only
  sends numbers it formats itself in `go`, and never `flip` or a FEN. The tests play every kind of
  special move through the engine (both castlings of each side, en passant, the four promotions)
  and check that illegal lines are refused.
* **Process exit without shutdown.** The host registers an `atexit` handler on first start (so it
  runs before the destructors of Stockfish's static objects) that sends `quit` and joins. The wait
  is bounded (2 s): on Windows `exit()` holds the CRT's atexit lock while running handlers, and an
  engine thread that is still initialising can block on it (registering the destructor of a
  function-local static such as the `sync_cout` mutex); the thread is then left parked until the
  process ends. Found and verified with the Windows build under Wine.
* **Threads, hash, NUMA.** `engine.threads` and `engine.hash_mb` (`Scacelith.ini`) reach `Threads`
  and `Hash` as before. With more than one thread the client first sends `NumaPolicy none` (one
  node with every CPU): on a machine with several NUMA nodes Stockfish would otherwise bind its
  threads to nodes and copy the network to each. With one thread (the default) it does neither,
  and the option is not sent: it would cost a copy of the network (~0.2 s) per session.
* **Wine and Proton.** The Windows build runs under Wine (unit tests and `tools/shot_win.sh` on a
  game against Stockfish) thanks to `win_shims.h`. Check it again at every Stockfish update.
* Remaining `exit()` calls in Stockfish are failure paths only: allocation failures (hash table,
  network), a network that fails to load (impossible for the embedded default; the game never sets
  `EvalFile`), thread creation failure, NUMA binding failures (only with bound threads, never with
  `NumaPolicy none`), corrupt Syzygy files (the game never sets `SyzygyPath`), debug log and
  benchmark file errors (never used).

Memory (Linux, in-process, 1 thread, 64 MB hash): 266 MB resident once the engine is ready, of
which 101 MB are the embedded network's pages of the executable image and 115 MB its unpacked copy;
316 MB while searching; 327 MB at the peak of the network load. After `quit` 103 MB remain, all of
them pages of the executable image that the OS can drop (Stockfish 16: about 150 MB in all).

Executable size: the Windows build is 125.2 MB, 59.0 MB more than with Stockfish 16 (the network
grew from 40.1 to 98.5 MB, the code by 0.6 MB), plus about 1.2 MB of zero-initialised tables at
run time.

## Strength presets

See `src/ai/presets.cpp`. Stockfish's handicap (`Skill Level`, or `UCI_LimitStrength` +
`UCI_Elo` 1320..3190, which maps onto a fractional skill level with the same fit in Stockfish 16
and 19) searches at least 4 principal variations and, after iteration `1 + int(level)`, picks one
of them at random, biased towards the better ones. `UCI_Elo 1320 == Skill Level 0` is its floor.

### Calibration of the weak presets

Stockfish offers nothing below `UCI_Elo 1320` (= Skill Level 0), so the three presets under it
combine Stockfish's own knobs. The measurements below were made with Stockfish 16 self-play (a
standalone build of its sources; 200-600 games per pairing, colours alternating, a referee engine
detects the end of the game and adjudicates at 300 plies with a depth-12 evaluation, |eval| > 3
pawns = win). Stockfish 19 has no classical evaluation any more, so the presets now use interim
settings, each measured directly against the Stockfish 16 preset it replaces (400 games).
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
* Chains of these measurements are not perfectly transitive (the players are very random), so the
  numbers are good to roughly +-50. The hash-table effect is mostly saturated at 30 ms per move
  (+40 at 100 ms), so taking "S0 at 30 ms" as Stockfish's 1320 may put the estimates below
  ~40 Elo too high.

With Skill Level 0 at normal time as 1320 (Stockfish's anchor):

| Preset | Stockfish 19 settings | Against the Stockfish 16 preset | Estimate |
|---|---|---:|---|
| Novice (~800) | Skill 0, depth 1, MultiPV 9 | -11 [-45, +22] | ~785 |
| Beginner (~1000) | Skill 0, depth 1, MultiPV 5 | -27 [-61, +6] | ~990 |
| Casual (~1200) | Skill 1, depth 2, MultiPV 5 | +20 [-14, +54] | ~1185 |
| Club Player ... Grandmaster | UCI_Elo 1500 / 1800 / 2100 / 2400 / 2700 | | Stockfish's calibration |
| Stockfish Max | full strength | | ~3500 (1 thread) |

These are engine ratings on Stockfish's CCRL-anchored scale; human (FIDE or online) ratings are
not the same scale, so the labels are approximate by nature.
