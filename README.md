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

Licence: GPL-3.0 (see `LICENSE` on `master`); the source of this exact build is commit `688615f`.
