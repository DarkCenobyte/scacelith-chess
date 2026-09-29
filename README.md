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

- Source: branch `claude/online-multiplayer-p0bhrp` at commit `fd204d6` (pull request #3, not merged
  yet), which includes `master` up to pull request #4 (play comfort).
- Contents: everything above, plus online play on a Scacelith server (the official
  `caissa.scacelith.com:44664` by default, or a community server chosen in Options > Online server),
  direct matches by IP address with UPnP, two players on one PC (hot-seat), the game pointer and
  square targeting of pull request #4.
- Windows x64, self-contained (Stockfish 16 and its NNUE network are embedded). Needs a GPU with
  OpenGL 4.6.
- Cross-compiled with MinGW-w64 (Release). Its test program passes under Wine (157 tests), and it
  played a rated game against a local dedicated server under Wine; it has not been run on real
  Windows hardware by the build.
- SHA-256: `1ca34f4de41a141a8f476d5631f90f8d23bd382cbe456831a3fdcb44d49048be`

Licence: GPL-3.0 (see `LICENSE` on `master`); the source of each build is the commit named above.
