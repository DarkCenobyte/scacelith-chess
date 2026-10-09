# Realtime protocol v1

The game and a Scacelith dedicated server talk over a WebSocket with the realtime protocol,
version 1 (frozen). This folder is the game's copy of that contract; the server lives in its own
repository, [DarkCenobyte/scacelith-chess-server](https://github.com/DarkCenobyte/scacelith-chess-server),
which is the source of every file here.

| File | Content |
|---|---|
| [PROTOCOL.md](PROTOCOL.md) | the specification: transport, encoding, connection lifecycle, games, clocks, gestures, every message |
| `scacelith-v1.json` | the schema, single source of the server's Rust codec and of the game's C++ codec (`src/net/protocol_gen.h`, `.cpp`) |
| `frozen/v1.0.json` | the wire of the released minor 1.0: later minors may only add to it |
| `protocol-vectors.json` | the golden vectors, read by `tests/net_tests.cpp` (`net_protocol_vectors`) |

Do not edit these files, nor `src/net/protocol_gen.h` and `src/net/protocol_gen.cpp`, by hand: the
server's `protogen` tool writes them all. After a change of the protocol in the server repository,
run in a checkout of it:

```sh
cargo run -p scacelith-protocol --features gen --bin protogen -- --client /path/to/scacelith-chess
```

The game also keeps copies of three other fixtures of the server, for its tests:
`tests/data/elo-vectors.json` (the server's `test/fixtures/elo-vectors.json`: offline and online
ratings follow the same rules), `tests/data/mating-material.json` (the server's
`test/fixtures/mating-material.json`: who can still checkmate, which turns a resignation, a flag
fall or a second illegal move into a draw, read by `tests/chess_tests.cpp`) and
`tests/data/server-pgn/` (the server's `test/fixtures/server-pgn/`: the PGN files a server
writes). The continuous integration of both repositories checks a pinned pair against all of them
(the server's `tools/interop/check-game.sh`) and plays the game's live online tests against a real
server (the server's `tools/live-check`): the server's CI takes the game commit of its
`tools/interop/game-revision`, the game's CI the server commit of `tools/interop/server-revision`,
and a weekly run of each checks the other side's `master`.
