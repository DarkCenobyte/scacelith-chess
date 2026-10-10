# Scacelith builds

Prebuilt binaries: the game for Windows in `windows/`, for Linux on 64-bit Arm in
`linux-aarch64/` and, once, for Linux x86-64 in `linux-x86_64/`, the dedicated server for Linux
in `linux-server/`. This branch only holds
binaries; the source code is on `master`. The experimental macOS disk image is built by GitHub
Actions only (the artifact of the CI's macOS job, or a release).

## windows/Scacelith-2026-09-28-offline.exe

- Source: `master` at commit `688615f` (merge of pull request #2), before the online multiplayer work.
- Contents: play against Stockfish (tournament rules, scoresheets written by the players, Elo),
  Watch a Game mode, ten languages. No online play.
- Windows x64, self-contained (Stockfish 16 and its NNUE network are embedded). Needs a GPU with
  OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (102 tests); it has
  not been run on real Windows hardware by the build.
- SHA-256: `6b42e50a3fe42028a1603d440d5d750018678971f6736a237ed494a8eff94d40`

## windows/Scacelith-2026-09-29-online.exe

- Source: branch `claude/online-multiplayer-p0bhrp` at commit `8e9207f` (pull request #3, not merged
  yet), which includes `master` up to pull request #4 (play comfort). This file replaces the earlier
  build of commit `fd204d6`; that one is still in this branch's history.
- Contents: everything above, plus online play on a Scacelith server (the official
  `caissa.scacelith.com:44664` by default, or a community server chosen in Options > Online server),
  direct matches by IP address with UPnP, two players on one PC (hot-seat), the game pointer and
  square targeting of pull request #4. Since `fd204d6`: the ping interval comes from the server,
  reconnections after a restart are spread out and reuse the server's answer, a server whose
  identity changed never receives the saved login, and a busy or full server gets a clear message.
- Windows x64, self-contained (Stockfish 16 and its NNUE network are embedded). Needs a GPU with
  OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (160 tests), and it
  played a rated game against a local dedicated server of the same commit under Wine; it has not
  been run on real Windows hardware by the build.
- SHA-256: `183543a6718c9c43786c54b8d564727210a1874b13a38f39a57b9b09ee7bdf2f`

## windows/Scacelith-2026-09-30-fiabilite.zip

- Holds `Scacelith-2026-09-30-fiabilite.exe` (129 MB, too large for a plain file on GitHub, so it is
  zipped; the zip is 94 MB).
- Source: branch `claude/reliability-visual-polish-mq6fwh` at commit `dd6655d` (pull request #5, not
  merged yet), based on `master` at `71881cc` (online multiplayer merged).
- Contents: everything above, plus the opponent's robot hand and head following the other player
  live in online games, the clock press automatic or by hand (an option offline, the server's or
  the host's choice online), captured pieces that no longer clip, looking up at the opponent with
  the pointer at the top of the screen, a closed table without see-through seams, an info mark
  with each option's definition, a brightness calibration page on the first start, FIDE 2024
  ratings and point refunds for the victims of a banned cheater. The embedded engine is
  Stockfish 19, in five builds (x86-64, SSE4.1, AVX2, AVX-VNNI, AVX-512); the game runs the fastest
  one the CPU supports.
- Protocol version 2: it plays online only on a server running this branch or later, and older clients are
  refused by such a server.
- Windows x64, self-contained (Stockfish 19 and its NNUE network are embedded). Needs a GPU with
  OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (223 tests); it has
  not been run on real Windows hardware by the build.
- SHA-256 of the exe: `589b8c6d106ffa40af3957bfbb9df43fb46772aa0fe4b2957a50e7e87f562ccb`
- SHA-256 of the zip: `822bca587586c53cade9e6d2eecec30815be3366441a0e6ab40b05b33ae62c6e`

## windows/Scacelith-2026-10-01-coach.zip

- Holds `Scacelith-2026-10-01-coach.exe` (135 MB, zipped to 96 MB).
- Source: branch `claude/coach-mode-qpdb1i` at commit `2a1069a` (the Coach mode pull request, not
  merged yet), based on `master` at `435d8bc` (pull request #5 merged).
- Contents: everything above, plus Coach mode (a talking, pointing robot coach: the rules lesson and
  levels 1 to 6, explanations of mistakes with takebacks and demonstration lines played on the
  board, opening names, an end-of-game appraisal), untimed games without a clock press, games saved
  as PGN files with a "Saved Games" page, and replays of those games with the free camera.
- The coach's voice model (Supertonic 3, 145 MB, OpenRAIL-M licence) is not in the exe: the game
  offers to download it from Hugging Face (fallback: the sherpa-onnx GitHub release) the first time
  Coach mode or the "Coach voice" option needs it, into `%APPDATA%\scacelith\coach\`. Saved games go to
  `%APPDATA%\scacelith\pgn\`.
- Windows x64, self-contained otherwise (Stockfish 19 in five builds and its NNUE network are
  embedded). Needs a GPU with OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (486 tests); it has
  not been run on real Windows hardware by the build.
- SHA-256 of the exe: `573090297946ea6d6282c0e263e8eb4b59c16d68ce54bb838209865f59acd7e3`
- SHA-256 of the zip: `c80a2befb7eed7764c997cd37a33b97e13c97e61660c44a434f4097674cb323a`

## windows/Scacelith-2026-10-02-compte.zip

- Holds `Scacelith-2026-10-02-compte.exe` (135 MB, zipped to 96 MB).
- Source: branch `claude/account-api-j0hobz` at commit `6f5485d` (pull request #7, merged into
  `master` as `0f59ca2`).
- Contents: everything above, plus an Account page in the game for an online server: game history
  with filters, game details, PGN download, an animated GIF of a game made by the server, account
  details, e-mail and password change, two-factor authentication on or off, data export and
  account deletion. Servers are reached on port 443 by default (entries with the old port 44664
  still work).
- Windows x64, self-contained otherwise (Stockfish 19 in five builds and its NNUE network are
  embedded; the coach's voice is downloaded by the game). Needs a GPU with OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release); it has not been run on real Windows hardware by the
  build.
- SHA-256 of the exe: `6f91371723ab218c7bb57b35bfa615df8cf8579bb4d55cf58f4e25e7f781b417`
- SHA-256 of the zip: `f35986976f7aec3e906839e2ec6bee2c0b3e5806214a102373417b873e32ec15`

## windows/Scacelith-2026-10-03-qualite.zip

- Holds `Scacelith-2026-10-03-qualite.exe` (132 MB, zipped to 95 MB).
- Source: branch `claude/quality-pass-wzqsae` at commit `936a2fe` (pull request #8, not merged yet),
  based on `master` at `0f59ca2` (pull request #7 merged).
- Contents: everything above after a quality and optimisation pass over the game and the dedicated
  server: faster loading, lighting baked from the loaded hall (sun shadows from the first frame),
  a repaired Windows exception table, Google sign-in through a redirect to the game on 127.0.0.1,
  safer settings saves, and many fixes listed in the pull request.
- Protocol version 3: it plays online only on a server running this branch or later, and older
  clients are refused by such a server. Two exes on either side of the change cannot play a
  direct match.
- Windows x64, self-contained otherwise (Stockfish 19 in five builds and its NNUE network are
  embedded; the coach's voice is downloaded by the game). Needs a GPU with OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (718 tests); it has
  not been run on real Windows hardware by the build.
- SHA-256 of the exe: `0c184bac8b60922c745f21804f4a050b7d80e17a72c060a9dca5789afd242258`
- SHA-256 of the zip: `9b9ffd2651c5d8ce10ff476c794e4ee788555dd22086b03e7eef59d184cf0369`

## windows/Scacelith-2026-10-03-serveur-rust.zip

- Holds `Scacelith-2026-10-03-serveur-rust.exe` (132 MB, zipped to 95 MB).
- Source: branch `claude/rust-dedicated-server-9t6idc` at commit `cf9ecd9` (pull request #9, not
  merged yet), based on `master` at `7531830` (pull request #8 merged).
- Contents: everything above, moved to the realtime protocol version 1 of the Rust dedicated
  server. The game waits as long as a full or restarting server asks (`Retry-After`), applies the
  events of a game in order and asks for the game's state again when one is missing, and follows
  the gesture rate and keepalive the server announces.
- Protocol version 1 (`scacelith.rt1`, frozen): it plays online only on a server of this pull
  request or later (the Linux server below), and earlier exes cannot connect to such a server. Two
  exes on either side of the change cannot play a direct match.
- Windows x64, self-contained otherwise (Stockfish 19 in five builds and its NNUE network are
  embedded; the coach's voice is downloaded by the game). Needs a GPU with OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (723 tests, one known
  Wine timing flake that passes when run again), and its online client played a rated game, used
  the Account page and signed in with Google (simulated) against the Linux server below, under Wine
  and on Linux; it has not been run on real Windows hardware by the build.
- SHA-256 of the exe: `282ad6719ec8aedf081ffc2b2d070119bd019f23c1809b5984f0eb1780ac0d00`
- SHA-256 of the zip: `2e9622146c1e91251bce195bd5be5ab371e6ad2de4139ad0ab688cec79f036fc`

## windows/Scacelith-2026-10-09-poignee.zip

- Holds `Scacelith-2026-10-09-poignee.exe` (132 MB, zipped to 95 MB).
- Source: branch `claude/handshake-finish-iaf0m1` at commit `6d28df0` (pull request #17, not merged
  yet), based on `master` at `f2ff8ca` (1.0.0-beta.2 and the CodeQL fixes of pull request #16).
- Contents: version 1.0.0-beta.2 with the handshake's second pass: a handshake cut short by an
  online opponent's move lets go at a human pace (back at rest 0.70 s after the cut, at most
  2.2 - 2.5 m/s, no elbow jump or lunge whatever the instant), and a refitted clasp (fit g7: at
  most 0.66 mm of interpenetration over the whole motion, palms and fingers closer to the other
  hand). Details in `docs/handshake/README.md` on that branch.
- Windows x64, self-contained otherwise (Stockfish 19 in five builds and its NNUE network are
  embedded; the coach's voice is downloaded by the game). Needs a GPU with OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (734 tests, 12
  skipped); it has not been run on real Windows hardware by the build.
- SHA-256 of the exe: `e0af731147ea3a22a7ea7bc55b28d7776a245ab79c50651c29001a7d2aeed2c0`
- SHA-256 of the zip: `19efc5587911a826c6ceb71b72c6f35fefca76ef47b661b94da953e93927ac8d`

## windows/Scacelith-2026-10-10-supertonic3.zip

- Holds `Scacelith-2026-10-10-supertonic3.exe` (132 MB, zipped to 96 MB).
- Source: branch `claude/supertonic3-official-yjzrfn` at commit `898a660` (pull request #22, not
  merged yet), based on `master` at `d40961a` (Analysis mode, pull request #21).
- Contents: the coach speaks with Supertone's official Supertonic 3 release (fp32, 398.7 MB,
  OpenRAIL-M licence) instead of sherpa-onnx's 8-bit conversion. The game downloads its seven files
  one after the other from huggingface.co/Supertone/supertonic-3 (fallback: the same files on
  supertone-oss-archive/supertonic-3), each checked against its SHA-256, into
  `%APPDATA%\scacelith\coach\`. A player who has the old model keeps it working: the update is
  offered once at start-up, then from the "Voice model" row in Options > Audio, and it deletes the
  old files before writing the new ones. Options > Audio also has a "Voice quality" slider (4 to 9
  synthesis steps, 5 by default).
- Windows x64, self-contained otherwise (Stockfish 19 in five builds and its NNUE network are
  embedded; the coach's voice is downloaded by the game). Needs a GPU with OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (793 tests, 12
  skipped); it has not been run on real Windows hardware by the build.
- SHA-256 of the exe: `69fc1e647e543bcd148acdaed0c25de2bad4533d6209db4538736f5525faad49`
- SHA-256 of the zip: `321f36956ec3a93fc94c3505b19d2d936640ce1761504de454b8fbc6fbc5f72f`

## windows/Scacelith-2026-10-10-qualite-voix.zip

- Holds `Scacelith-2026-10-10-qualite-voix.exe` (132 MB, zipped to 96 MB).
- Source: branch `claude/supertonic3-official-yjzrfn` at commit `5cfac12` (pull request #23, not
  merged yet), based on `master` at `dcee8b6` (pull request #22 merged).
- Contents: everything in the build above, with Options > Audio > Voice quality named by level
  (Low, Medium, High, Very high, Ultra, Maximum for 4 to 9 synthesis steps; Medium by default) and
  greyed out until a voice model is installed (the official one or the old 8-bit one).
- Windows x64, self-contained otherwise (Stockfish 19 in five builds and its NNUE network are
  embedded; the coach's voice is downloaded by the game). Needs a GPU with OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (793 tests, 12
  skipped); it has not been run on real Windows hardware by the build.
- SHA-256 of the exe: `7164d799652427d15262851adc861d764b14f0212418081e2ca1b0035adaca87`
- SHA-256 of the zip: `dfe6de8a7cdc6355d3a6c271ca1606d91e6c666c39a67a95283dbe6f8684049d`

## windows/Scacelith-2026-10-10-capture.zip

- Holds `Scacelith-2026-10-10-capture.exe` (132 MB, zipped to 96 MB).
- Source: branch `claude/screenshot-freeze-19zdgd` at commit `9fce275` (pull request #24, not
  merged yet), based on `master` at `dcee8b6` (pull request #22 merged).
- Contents: everything in the `supertonic3` build above, with the borderless fullscreen window kept
  composed by Windows: Win+Shift+S and the Snipping Tool capture the current frame instead of an
  old one (the menu, or the previous capture). Without pull request #23's voice quality levels.
- Windows x64, self-contained otherwise (Stockfish 19 in five builds and its NNUE network are
  embedded; the coach's voice is downloaded by the game). Needs a GPU with OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (797 tests, 21
  skipped); it has not been run on real Windows hardware by the build.
- SHA-256 of the exe: `3d274a577b065274c80dbb8d02d17ca750d9d72fd2db748753e7f6fd353d96ea`
- SHA-256 of the zip: `cd4c9e5286bf83bb12030bf0b09b671b063be630d497d7dde6de01c5646c270d`

## windows/Scacelith-2026-10-10-points-de-vue.zip

- Holds `Scacelith-2026-10-10-points-de-vue.exe` (133 MB, zipped to 96 MB).
- Source: branch `claude/standing-viewpoints-iru8qq` at commit `66e21f6` (pull request #28, not
  merged yet), which includes `master` up to pull request #26 (coach challenges).
- Contents: standing viewpoints during a game. Up arrow: stand in front of the chair; Left / Right
  arrow: walk to that end of the table; Down arrow: sit back down. The clock keeps running and only
  a seated player plays. Online (protocol v1.2, server pull request #5 of
  `DarkCenobyte/scacelith-chess-server`) the opponent's robot stands and walks too; against a
  server still on protocol v1.1 everything else works and the stances are simply not relayed.
  Also the game-side fixes of the 2026-10-10 audit (N01 analysis comments, A08 Linux sign-in
  file, N02 CI voice model) and a move refused by a busy server sent again. The robot's thighs turn
  on porcelain balls inside the pelvis (hips 1.5 cm closer, no hollow over a standing robot's
  thigh). This file replaces the builds of commits `735a472`, `134671a` and `8816fbb`, still in this
  branch's history.
- Windows x64, self-contained otherwise (Stockfish 19 in five builds and its NNUE network are
  embedded; the coach's voice is downloaded by the game). Needs a GPU with OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (858 tests, 12
  skipped); the live check against the server of pull request #5 passes in the pull request's CI.
  It has not been run on real Windows hardware by the build.
- SHA-256 of the exe: `2912c089f8bfab2d43ef0475c28f12d7a782656dfe02c5103b6d870e1054d1f5`
- SHA-256 of the zip: `d0e7d73994fe59caf6be1fdbcec81fa8ffb62033f6553dbaa6ad72c2a4a771e4`

## windows/Scacelith-2026-10-10-macos-arm.zip

- Holds `Scacelith-2026-10-10-macos-arm.exe` (133 MB, zipped to 96 MB).
- Source: branch `claude/project-thread-s7odk6` at commit `e7bb260` (pull request #29, not merged
  yet), based on `master` at `c8f96e7` (pull request #28 merged).
- Contents: the Windows side of the experimental macOS (Apple Silicon) and Linux aarch64 port. The
  engine now reads optional OpenGL features from the driver's extension list: without
  tessellation, tessellation is off whatever the preset and greyed out in Options > Graphics;
  without depth clamping, the sun shadows widen their near plane instead. A conforming OpenGL 4.6
  driver, as on any Windows PC that ran the earlier builds, has both, so nothing changes there.
- Windows x64, self-contained otherwise (Stockfish 19 in five builds and its NNUE network are
  embedded; the coach's voice is downloaded by the game). Needs a GPU with OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine in the pull
  request's CI on the same source. It has not been run on real Windows hardware by the build.
- SHA-256 of the exe: `a2331cc169d06256cb801cfa62594040a4495af1170fb969c46e6d3a12ad8d6f`
- SHA-256 of the zip: `6f80dbcb3c56cb8481ebea85ce148805c2ccfa5a8e93822daf13e11462f63a45`

## windows/Scacelith-2026-10-10-audit-final.zip

- Holds `Scacelith-2026-10-10-audit-final.exe` (133 MB, zipped to 96 MB).
- Source: branch `claude/audit-n03-a10-5gbp56` at commit `f147ee9` (pull request #30, not merged
  yet), based on `master` at `60f3b5b` (pull request #29 merged).
- Contents: the game-side fix of the final third-party audit (N03): in a "hold the draw" coach
  challenge, a coach's answer that stalemates the player or leaves no mating material ends the
  position as held (it used to wait for a move that did not exist). Also, in the challenges: a
  hint asked for while the engine was still looking is dropped when the player moves first
  (it used to come late, about the position left), no hint is offered when none can be given,
  and a move the coach answers with checkmate is told as a mate.
- Windows x64, self-contained otherwise (Stockfish 19 in five builds and its NNUE network are
  embedded; the coach's voice is downloaded by the game). Needs a GPU with OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine in the pull
  request's CI on the same source. It has not been run on real Windows hardware by the build.
- SHA-256 of the exe: `03706fc5bf35649127e09af6f4478f2cca38182d7af2a958ded6980de9a57ecf`
- SHA-256 of the zip: `f50a8b9b7fb41ec0c156dd9c787fae52c3833812893ad29aac4491525adf3e1d`

## linux-aarch64/Scacelith-2026-10-10-linux-aarch64.tar.gz

- The game for Linux on 64-bit Arm: the executable `scacelith` (130 MB), `install.sh` (adds it to
  the applications menu), its icons, the licences and a README (archive of 95 MB).
- Source: the same commit `e7bb260` of pull request #29.
- Needs a GPU with OpenGL 4.6 (an NVIDIA Jetson Orin or discrete card, an AMD discrete card, or
  Asahi Linux on Apple M1/M2; not a Raspberry Pi), X11 or XWayland, glibc 2.38 and OpenSSL 3
  (Ubuntu 24.04, Debian 13, Fedora 39 or later). The release workflow builds the same game on
  Ubuntu 22.04, for glibc 2.34.
- Stockfish 19 in two builds (Armv8, and Armv8 with the dot-product instructions; the game runs
  the faster one the CPU supports) and its NNUE network are embedded; the coach's voice runs on
  NEON and is downloaded by the game.
- Cross-compiled with GCC for aarch64-linux-gnu (Release) on Ubuntu 24.04. Its test program
  passes natively on GitHub's Arm runner in the pull request's CI on the same source. It has not
  been run on an Arm GPU by the build.
- SHA-256 of the archive: `856a2092f8327b420c0bfa33b255445fe6846ba25554a0b35718a6baf961bf68`
- SHA-256 of the executable: `4f5ec585c36baf3a42c3e1f68cb79d607beece97af4f8d205e187ff8c3f880e1`

## linux-x86_64/Scacelith-2026-10-10-audit-final-linux-x86_64.tar.gz

- The game for Linux x86-64: the executable `scacelith` (133 MB), `install.sh` (adds it to the
  applications menu), its icons, the licences and a README (archive of 97 MB). A one-off test
  build: Linux x86-64 games otherwise come from the releases.
- Source: the same commit `f147ee9` of pull request #30 as
  `windows/Scacelith-2026-10-10-audit-final.zip`.
- Needs a GPU with OpenGL 4.6, X11 or XWayland, glibc 2.38 and OpenSSL 3 (Ubuntu 24.04, Debian
  13, Fedora 40 or later). It links only libX11, libGL, OpenSSL and glibc (libasound and
  libsecret are loaded when present). The release workflow builds the same game on Ubuntu 22.04,
  for glibc 2.34.
- Stockfish 19 in five builds (the game runs the fastest one the CPU supports) and its NNUE
  network are embedded; the coach's voice is downloaded by the game.
- Built with GCC 13 (Release) on Ubuntu 24.04. Its test program passes on the same machine (886
  tests, 24 skipped), as in the pull request's CI on the same source. It has not been run on a
  real GPU by the build.
- SHA-256 of the archive: `643552630496704c1f90418b6a4684fcd01c90a74f28bfc7489514afea21e5cd`
- SHA-256 of the executable: `ec40024df2b43b84ab7c9f75eaf6d848cd1f4ba8fb7e9947a8e8014595a08c80`

## linux-server/scacelith-server-2026-10-03

- The dedicated server, rewritten in Rust (`scacelith-server` 1.0.0, 13.5 MB): a static x86-64
  executable (musl) that runs on any x86-64 Linux, whatever its C library, with SQLite built in.
- Source: branch `claude/rust-dedicated-server-9t6idc` at commit `cf9ecd9` (pull request #9, not
  merged yet), directory `dedicated-server/`.
- Install it as `/usr/local/bin/scacelith-server` and follow `dedicated-server/docs/DEPLOY.md`;
  the systemd unit and the other example files are in `dedicated-server/deploy/systemd/`. It
  starts from an empty database (a database or journal of the former Node server is not opened)
  and speaks realtime protocol version 1 only, so players need the game exe above or later.
- Built with Rust 1.99.0 (`cargo build --release --locked -p scacelith-server --target
  x86_64-unknown-linux-musl`). Its test suites pass (1,468 Rust tests), and the game's client
  passed the live checks against this very binary, on Linux and under Wine.
- SHA-256: `ab406fc8eeeaf62c09b4b84643577f0cb6022eadcbcc58b30e889898e1ef995e`

## linux-server/scacelith-server-2026-10-10-audit-final

- The dedicated server (`scacelith-server` 0.9.1, 14 MB): a static x86-64 executable (musl) that
  runs on any x86-64 Linux, whatever its C library, with SQLite built in.
- Source: `DarkCenobyte/scacelith-chess-server`, branch `claude/audit-n03-a10-5gbp56` at commit
  `c510a05` (pull request #6, not merged yet), based on `master` at `32ec176` (pull request #5
  merged: realtime protocol v1.2, standing viewpoints).
- Contents: the server-side fix of the final third-party audit (A10 residual): a player who
  reconnects over and over to a game whose host lags behind no longer piles up attach and detach
  messages in that host's inbox (at most two per player's game wait there; a newer connection
  waits for its game in its own task, tried again every 200 ms). New metric
  `scacelith_game_attach_deferred_total`. Same database and configuration as the earlier builds;
  protocol unchanged.
- Install it as `/usr/local/bin/scacelith-server` and follow `docs/DEPLOY.md` of the server
  repository.
- Built with Rust 1.99.0 (`cargo build --release --locked -p scacelith-server --target
  x86_64-unknown-linux-musl`). Its test suites pass (`cargo test --workspace`, locally and in the
  pull request's CI), as do the contract and live tests with the game.
- SHA-256: `f3d43b0bafb89be083fb9c1b5d44c902dc5c700b62a06fbc667ff281ce45517a`

Licence: GPL-3.0 (see `LICENSE` on `master`; the dedicated server is GPL-3.0-or-later); the source of
each build is the commit named above.
