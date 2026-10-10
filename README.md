# Scacelith builds

Prebuilt binaries: the game for Windows in `windows/`, the dedicated server for Linux in
`linux-server/`. This branch only holds binaries; the source code is on `master`.

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

Licence: GPL-3.0 (see `LICENSE` on `master`; the dedicated server is GPL-3.0-or-later); the source of
each build is the commit named above.
