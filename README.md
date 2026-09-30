# Scacelith builds

Prebuilt Windows binaries. This branch only holds binaries; the source code is on `master`.

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

Licence: GPL-3.0 (see `LICENSE` on `master`); the source of each build is the commit named above.
