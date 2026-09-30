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
  (upstream's own multi-architecture build, see
  [Instruction-set variants](#instruction-set-variants)) are kept so that the whole tree can be
  checked against the tag; the generated `src/.depend` is not part of it.
* `src/nn-1a298aa575a0.nnue` is Stockfish 19's default (and only) network, which upstream does not
  keep in git (`make net` downloads it): 98,511,183 bytes, sha256
  `1a298aa575a085434d29027978dc36867fe9c5bcea9376654b7a8eba1e52dfc2`, whose first 12 digits are its
  name, as upstream's `scripts/net.sh` checks.
* `Copying.txt` and `AUTHORS` come from the same commit.

## Modifications

**No upstream file is modified.** Everything Scacelith-specific lives next to it:

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Static library `stockfish_embedded`: all upstream sources except `src/main.cpp`, once per instruction-set variant |
| `cmake/isolate.cmake` | Makes each variant one object with three global symbols, and verifies it |
| `scacelith/entry.cpp` | `scacelith_sf_main_<tag>()` (one per variant, e.g. `_x86_64_avx2`): the body of upstream `main()` as a function, with a fixed command line |
| `scacelith/cpu.cpp` | The dispatcher: chooses the variant for the CPU, runs its static initialisers, calls its entry point |
| `scacelith/nnue_incbin.cpp` | The network, embedded once for all variants |
| `scacelith/local_shm.h` | Replaces upstream `src/shm.h`: the network stays in process memory (forced include) |
| `scacelith/win_shims.h` | Windows: redirects `GetNumaProcessorNodeEx` (Wine) and the console code page calls (forced include) |
| `scacelith/win_shims.cpp` | Windows: the `GetNumaProcessorNodeEx` wrapper |
| `scacelith/stockfish_embedded.h` | The only header the game sees |
| `tools/isa_audit.py`, `tools/isa_probe.cpp` | Build check: disassembles a linked program and verifies the variants' isolation |

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

* The Stockfish sources are compiled once per instruction-set variant (next section), each with
  the flags of the upstream Makefile for that `ARCH` with GCC: `-O3 -funroll-loops -fno-exceptions
  -fno-ipa-cp-clone -fconstexpr-ops-limit=500000000 -DNDEBUG -DIS_64BIT -DARCH=<arch>` plus the
  variant's instruction set (`SF_ISA_<arch>` in `CMakeLists.txt`, copied from the Makefile). No
  LTO: upstream's LTO builds measured no faster than these, and a partially linked LTO object
  cannot be isolated. Search threads
  are pthreads with 8 MB stacks with Linux GCC and MinGW-w64 alike (`thread_native.h`). The game's
  global Windows definitions (`UNICODE`, `WIN32_LEAN_AND_MEAN`, ...) are not applied. On Windows
  the library links `shell32` (`CommandLineToArgvW`).
* Each variant also gets `-DStockfish=Stockfish_<tag>` (its own namespace, e.g.
  `Stockfish_x86_64_avx2`), `-DUNIVERSAL_BINARY` (upstream's switch for multi-architecture builds:
  `nnue/network.cpp` then only declares the embedded network) and, on Linux, `-fno-gnu-unique`
  (see isolation below). The UCI command `compiler` reports the variant's `ARCH`.
* The network is embedded once, by `scacelith/nnue_incbin.cpp`, with Stockfish's own
  `INCBIN(EmbeddedNNUE, EvalFileDefaultName)` (an assembler `.incbin`, works with Linux GCC and
  MinGW-w64 GCC), which is what `nnue/network.cpp` does in a single-architecture build. The
  assembler resolves the network's relative path itself, so that file gets `-Wa,-I<this>/src`, and
  it depends on the `.nnue` file for rebuilds.
* The dispatcher, the network and the Win32 wrapper are compiled once with the default x86-64
  flags and without the forced includes, so they run on any x86-64 CPU.
* `SCACELITH_SF_VARIANTS` (a CMake cache list) selects the variants; the default is all five. Each
  is a full Stockfish compile: `-DSCACELITH_SF_VARIANTS=x86-64-sse41-popcnt` (or whichever variant
  the developer's CPU runs best) makes quicker local builds. The dispatcher only knows the variants
  that were built; without `x86-64` a CPU below every built variant cannot run the engine, and the
  game logs it and plays random moves as before.
* Clean build of the Stockfish part (`ninja -j2 stockfish_embedded`, no ccache, on the shared
  4-core build machine): Linux 6 min 13 s wall, 4 min 58 s user and 26 s system CPU; MinGW 11 min
  53 s wall, 9 min 16 s user and 53 s system CPU. The machine was busy (load average 6 to 10), so
  the wall times are long; the user CPU time is about six times that of the single SSE4.1 build
  Stockfish 19 was first embedded with (Linux 52 s, MinGW 92 s). The audit below adds 7 s (Linux)
  and 10 s (MinGW).

## Instruction-set variants

Stockfish 19 searches about half as many nodes per second as Stockfish 16 with the same
instruction set (SSE4.1, below; its network is 2.5 times larger), and newer instructions recover
part of that. The library therefore holds five builds of the engine and runs the best one the CPU
supports:

| Variant | CPUs | Checked at run time (all of the previous row's, plus) | `bench` nodes/s, 1 thread |
|---|---|---|---:|
| `x86-64` | every x86-64 CPU (Core 2, Phenom II, some virtual machines) | nothing: SSE2 is part of x86-64 | 357 k (-37 %) |
| `x86-64-sse41-popcnt` | Nehalem to Ivy Bridge, AMD Bulldozer to Steamroller, Jaguar | SSE3, SSSE3, SSE4.1, POPCNT | 563 k |
| `x86-64-avx2` | Haswell to Comet Lake, Zen 1 to 3 | AVX2 (and OS support), BMI1 | 658 k (+17 %) |
| `x86-64-avxvnni` | Alder Lake and later Intel client CPUs | BMI2, AVX-VNNI | 770 k (+37 %) |
| `x86-64-avx512icl` | Ice Lake, Tiger Lake, Rocket Lake, Sapphire Rapids and later, Zen 4 and 5 | BMI2, AVX-512 F, BW, VL, DQ, CD, VNNI, IFMA, VBMI, VBMI2, VPOPCNTDQ, BITALG, VPCLMULQDQ, GFNI, VAES (and OS support) | 833 k (+48 %) |

The speeds are `bench 16 1 13` (upstream's benchmark positions, 16 MB hash, 1 thread, depth 13) run
by the game's library with the variant forced, on the build machine's Intel CPU (AVX-512 and
AVX-VNNI): the median of 4 interleaved rounds, CPU time without the start-up, on a busy machine
(load 5 to 7, hence +-10 %). Every variant searches the same 2,497,913 nodes. Stockfish 16's former
game build did 1,138 k with SSE4.1 on the same machine (separate run, same method). Upstream's
other x86 variants were left out: `bmi2` measured the same as `avx2`, and `avx512` / `vnni512`
(Skylake-X, Cascade Lake: workstations and servers) fall back to `avx2`, which also avoids their
AVX-512 clock penalty. Adding a variant takes its name, flags and audit level in `CMakeLists.txt`
and its level and requirements in `cpu.cpp`.

**Dispatch** (`scacelith/cpu.cpp`). The rules follow upstream's `universal/entry_x86.cpp`,
restricted to these variants, and check every extension a variant's flags enable, which upstream
does not quite do: SSE3 and SSSE3 for `sse41-popcnt`, BMI1 for `avx2` (`-mbmi`), AVX512DQ and
AVX512CD for `avx512icl`. Upstream's slow-PDEP rule (AMD Excavator to Zen 2) only concerns its
`bmi2` variant: those CPUs get `avx2` here, which does not use PDEP/PEXT. The CPU features come
from libgcc's CPU model (`__builtin_cpu_supports`), which also checks the OS: it reports AVX2 only
when XCR0 shows that the OS saves the YMM registers, and the AVX-512 features only when it also
saves the opmask and ZMM registers (`and $0x6` / `and $0xe6` in `__cpu_indicator_init`); every VEX
variant requires AVX2 and the EVEX one AVX-512. The variants are not a strict chain (a CPU can
have AVX-512 but not AVX-VNNI), so each one is checked on its own and the highest built one the
CPU runs wins.

* **`engine.arch`** in `Scacelith.ini` (default `auto`), for troubleshooting: a variant name caps
  the choice at that variant, never above what the CPU runs; an unknown name is logged and
  ignored. It is applied at the next engine start.
* **Log.** Every engine start writes the variant, e.g. `ai: starting embedded Stockfish
  (x86-64-avx512icl)`, or `ai: starting embedded Stockfish (x86-64-sse41-popcnt, limited by
  engine.arch = x86-64-sse41-popcnt; this CPU runs x86-64-avx512icl)`.
* **Static initialisers.** A variant's static initialisers may use its instructions, so they
  cannot run at program start. `stockfish_embedded_supported()` runs those of the variant it
  chooses, once per process (`std::call_once`), in the order the C runtime would have used
  (`.init_array` forwards on Linux, `.ctors` backwards on MinGW). They register the variant's
  static destructors with `atexit`; the host registers its own exit handler (which stops a running
  engine) after them, once per variant, so it runs first.

**Isolation** (`cmake/isolate.cmake`). Every variant compiles the same inline functions and C++
standard library templates (`std::vector<...>`, `std::string`, ...) with its own instruction set.
Left alone, those copies are weak (COMDAT) symbols that the linker merges program-wide: the game,
or the baseline variant, could then call an AVX2 copy on a CPU without AVX2 and crash. This is why
upstream's `x86-64-universal` build is not used: its variants are only renamed, not isolated, and
it is only safe with `--allow-multiple-definition` and a favourable link order (its MinGW cross
build fails with multiple definitions). Each variant is instead turned into one object:

1. `ld -r` links its objects into one relocatable object, which resolves its COMDAT groups inside
   the variant (ELF: `--force-group-allocation`, no group left; PE: twice, as one pass leaves an
   undefined symbol next to each kept COMDAT definition).
2. PE only: the COMDAT flag is cleared on every `.text$*`, `.rdata$*`, `.data$*`, `.bss$*`,
   `.xdata$*` and `.pdata$*` section, because COFF merges COMDATs by name even when their symbol
   is local.
3. The initialiser table (`.init_array` or `.ctors`) becomes the section `sfinit_<tag>` between the
   symbols `sfinit_<tag>_start` and `sfinit_<tag>_end`, so the C runtime no longer runs it.
4. `objcopy` makes every defined symbol local except the entry point `scacelith_sf_main_<tag>` and
   the two bounds. Undefined references (libstdc++, the C runtime, pthreads, Win32) stay external
   and bind to the single copies in the executable, so the engine still uses the game's
   `std::cin` / `std::cout`. On Linux `-fno-gnu-unique` is needed first: GCC otherwise emits the
   guard variables of inline statics as `STB_GNU_UNIQUE` symbols, which `objcopy` cannot make
   local.

The script then verifies its output and fails the build if anything could leak or go missing:
exactly those three global symbols, bounds that span the whole initialiser table, no symbol both
defined and undefined, no COMDAT left, and no section the scheme does not handle (initialiser
priorities, destructor tables, thread-local storage; Stockfish 19 has none).

**Verification.**

* **Audit at every build** (`tools/isa_audit.py`, target `stockfish_isa_audit`, Linux and MinGW
  builds when Python 3 is found). A small program (`tools/isa_probe.cpp`) links the library the way
  the game does, next to ordinary code that instantiates the same standard library templates. The
  script disassembles it, attributes every function to its object file with the link map, and
  classifies every instruction (baseline, SSE3 to SSE4.2 and POPCNT, VEX, EVEX). The build fails if
  a function outside the variants uses anything above baseline, if a variant holds code above its
  own level, or if anything but the dispatcher refers to a variant's code (calls, jumps,
  RIP-relative operands, and pointers in data from the relocations). Result: no finding in the
  Linux and MinGW builds. Run by hand on the game executables relinked with a map (Linux: 7,944
  functions, 292,899 references; Windows: 11,819 functions, 330,384 references), it finds nothing
  either; it does fail when given a variant level that is too low or no exemption for the
  dispatcher.
* **Same play in every variant** (`ai_variants_play_identically` in `tests/ai_tests.cpp`): each
  variant the CPU runs, forced through the arch limit, searches four positions to depth 11 with
  identical node counts and moves. Stockfish's search is deterministic with one thread, so any
  difference would reveal a miscompiled variant.
* **Selection on other CPUs** (by hand, at every Stockfish update). `qemu-x86_64 -cpu <model>`
  runs the Linux build, and `WINELOADERNOEXEC=1 qemu-x86_64 -cpu <model> /usr/lib/wine/wine64` the
  Windows build, on an emulated CPU: qemu64 and Penryn choose `x86-64`, Nehalem
  `x86-64-sse41-popcnt`, Haswell and EPYC-Rome `x86-64-avx2`, and Icelake-Server `x86-64-avx2` as
  well (qemu does not emulate AVX-512), all with the same `bench` node counts. The Windows AI tests
  pass on Penryn and Nehalem, and `ai_variants_play_identically` on Haswell. The build machine's
  CPU gets `x86-64-avx512icl`, natively and under Wine, and runs all five variants.

## Running in-process

`src/ai/uci_host.cpp` redirects `std::cin` / `std::cout` to thread-safe in-memory stream buffers
(a blocking line queue for input, a line splitter for output), calls
`stockfish_embedded_supported()` (which chooses the variant) and runs `stockfish_embedded_main()`
on a `std::thread`. Nothing else in the game may use `std::cout` / `std::cin` (see
`docs/ARCHITECTURE.md`). `src/ai/engine.cpp` is the UCI client.

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
  started after `quit` (tested repeatedly on Linux and under Wine), in the same variant or
  another. Only the attack and Zobrist tables are process-wide (filled again, with the same
  values, by every session), plus a few static objects that hold no game state (the processor
  affinity read by the initialisers, the output mutex, ...). Only one session can run at a time
  (the standard streams are shared).
* **Restart cost.** Every session parses the network again: `uci` + `isready` until `readyok`
  takes 0.38-0.55 s on Linux (up to 1.1 s under load) and 0.45-0.58 s under Wine on the busy build
  machine with the `x86-64-avx512icl` variant (0.45-1.25 s and 0.5-0.75 s with the former single
  `x86-64-sse41-popcnt` build), against 0.4-0.75 s for the first and 40-110 ms for later sessions
  with Stockfish 16, whose network stayed in globals. The game starts the engine once per process,
  asynchronously.
* **Illegal input ends the process.** Stockfish 19 calls `std::exit(1)` on a `position` command
  with an illegal move or FEN, a malformed `go` argument or a failed `flip`. That exit would run on
  the engine thread, so the host's `atexit` handler (below) would wait for itself and the game
  would vanish without a message. `ai::Engine` therefore replays every move list with the game's
  own rules (`chess::Position::parseUCI`) before sending it; a list that fails is never sent and
  the request fails (empty move, neutral evaluation), which the game already handles. It only
  sends numbers it formats itself in `go`, and never `flip` or a FEN. The tests play every kind of
  special move through the engine (both castlings of each side, en passant, the four promotions)
  and check that illegal lines are refused.
* **Process exit without shutdown.** The host registers an `atexit` handler that sends `quit` and
  joins, after the initialisers of each variant it starts (so it runs before the destructors of
  that variant's static objects). The wait is bounded (2 s): on Windows `exit()` holds the CRT's
  atexit lock while running handlers, and an engine thread that is still initialising can block on
  it (registering the destructor of a function-local static such as the `sync_cout` mutex); the
  thread is then left parked until the process ends. Found and verified with the Windows build
  under Wine.
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
315 MB while searching; 327 MB at the peak of the network load. After `quit` 102 MB remain, all of
them pages of the executable image that the OS can drop (Stockfish 16: about 150 MB in all). The
variants the CPU does not run cost no memory: these figures are the same as with the single
`x86-64-sse41-popcnt` build.

Executable size: the Windows build is 129.2 MB, 63.0 MB more than with Stockfish 16. The network
grew from 40.1 to 98.5 MB and the code by 4.6 MB, of which 3.95 MB for the four variants added to
the former single one (about 0.8 MB of code each; Linux: +3.6 MB). Each variant also has 0.3 MB
(variants with BMI2) or 1.2 MB of zero-initialised tables, only touched in the variant that runs.

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
