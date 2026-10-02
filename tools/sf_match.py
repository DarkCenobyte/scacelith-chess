#!/usr/bin/env python3
r"""Plays engine matches for calibrating Scacelith's strength presets, and pools their results (the
measurements of third_party/stockfish/README.scacelith.md, "Calibration of the weak presets").

Requirements: Python 3.8+ with python-chess (`pip install chess`; 1.11.2 was used), and standalone
UCI engines, e.g. Stockfish built from a clone checked out at a release tag (`make -j2 build
ARCH=x86-64-sse41-popcnt` in its src/). The game's embedded engine has no UCI executable.

    tools/sf_match.py play --games N --pgn FILE --referee ENGINE
        --a ENGINE [--a-name NAME] [--a-opt NAME=VALUE ...] [--a-go ARGS]
        --b ENGINE [--b-name NAME] [--b-opt NAME=VALUE ...] [--b-go ARGS]
        [--common-opt NAME=VALUE ...] [--ref-depth 12] [--max-plies 300] [--win-cp 300] [--event TEXT]
    tools/sf_match.py pool --a-name NAME FILE.pgn [FILE.pgn ...]

For example the Novice (Stockfish 19) against the Stockfish 16 Novice it replaced:

    tools/sf_match.py play --games 1000 --pgn novice.pgn --referee sf19/src/stockfish \
        --a sf19/src/stockfish --a-name "SF19 Novice" --a-opt "Skill Level=0" --a-opt MultiPV=9 \
        --a-go "depth 1" \
        --b sf16/src/stockfish --b-name "SF16 Novice" --b-opt "Skill Level=0" --b-opt MultiPV=7 \
        --b-opt "Use NNUE=false" --b-go "depth 1"

Protocol of "play":
  * Each engine is its own process, set up with Threads 1 and Hash 64 (as the game plays), any
    --common-opt, then its own --a-opt / --b-opt; both get "ucinewgame" before every game.
  * Games start from the initial position; A has White in the odd games, B in the even ones. Every
    move is one "go" with the side's arguments (--a-go / --b-go, e.g. "depth 1" or "movetime 250").
  * python-chess checks every move (an illegal one stops the match) and ends the game on
    checkmate, stalemate, insufficient material, the 50-move rule or a threefold repetition.
  * Adjudication: a game still running after --max-plies plies goes to the referee engine, which
    evaluates the position to --ref-depth after a "ucinewgame": more than --win-cp centipawns for
    one side (or a mate score) is a win for that side, anything else a draw.
  * Every finished game is appended to --pgn at once, with how it ended, its length, the mean
    nodes per search of each side (the effective budget of "movetime" searches on a busy machine)
    and the load average. A run whose PGN file exists resumes after the games already in it, so an
    interrupted match loses at most the game in progress, and a match can be run in shards.
  * Result: A's wins, draws and losses, and Elo(A) - Elo(B) from the score with a 95% interval
    (normal approximation of the win / draw / loss distribution).
"pool" prints the same result for all the games of several PGN files, and their mean length.

Stockfish seeds the random pick of its Skill handicap from the clock, so a match with handicapped
engines is only reproducible in distribution; "movetime" matches also depend on the machine's load.
"""
import argparse
import math
import os
import subprocess
import sys
import time

import chess
import chess.pgn


class Engine:
    """A UCI engine process driven line by line."""

    def __init__(self, path, options, go):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.go = go
        self.nodes = self.searches = 0  # since the last new_game()
        self.send("uci")
        self.wait("uciok")
        for name, value in options:
            self.send(f"setoption name {name} value {value}")
        self.sync()

    def send(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def wait(self, prefix):
        """Reads until a line starting with prefix; returns every line read."""
        lines = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("engine died")
            lines.append(line.rstrip("\n"))
            if lines[-1].startswith(prefix):
                return lines

    def sync(self):
        self.send("isready")
        self.wait("readyok")

    def new_game(self):
        self.nodes = self.searches = 0
        self.send("ucinewgame")
        self.sync()

    def search(self, moves, go=None):
        """(best move, score in centipawns for the side to move or None) of one search."""
        self.send("position startpos" + (" moves " + " ".join(moves) if moves else ""))
        self.send("go " + (go or self.go))
        lines = self.wait("bestmove")
        score = None
        nodes = 0
        for line in lines:  # the last score of the first line (MultiPV 1)
            t = line.split()
            if not t or t[0] != "info":
                continue
            if "nodes" in t:
                nodes = int(t[t.index("nodes") + 1])
            if "score" in t and ("multipv" not in t or t[t.index("multipv") + 1] == "1"):
                i = t.index("score")
                v = int(t[i + 2])
                score = v if t[i + 1] == "cp" else (100000 if v > 0 else -100000)
        self.nodes += nodes
        self.searches += 1
        return lines[-1].split()[1], score

    def quit(self):
        try:
            self.send("quit")
            self.p.wait(5)
        except Exception:
            self.p.kill()


def parse_options(items):
    out = []
    for item in items or []:
        name, _, value = item.partition("=")
        out.append((name.strip(), value.strip()))
    return out


def play_game(white, black, referee, args):
    """Plays one game; returns (final board, result, how it ended)."""
    board = chess.Board()
    moves = []
    white.new_game()
    black.new_game()
    while True:
        if board.is_checkmate():
            return board, ("0-1" if board.turn == chess.WHITE else "1-0"), "mate"
        if board.is_stalemate() or board.is_insufficient_material():
            return board, "1/2-1/2", "stalemate/material"
        if board.halfmove_clock >= 100 or board.is_repetition(3):
            return board, "1/2-1/2", "50-move/repetition"
        if len(moves) >= args.max_plies:
            referee.new_game()
            _, cp = referee.search(moves, f"depth {args.ref_depth}")
            cp = cp or 0
            if board.turn == chess.BLACK:
                cp = -cp  # White's point of view
            if cp > args.win_cp:
                return board, "1-0", "adjudicated"
            if cp < -args.win_cp:
                return board, "0-1", "adjudicated"
            return board, "1/2-1/2", "adjudicated"
        player = white if board.turn == chess.WHITE else black
        uci, _ = player.search(moves)
        move = chess.Move.from_uci(uci)
        if move not in board.legal_moves:
            raise RuntimeError(f"illegal move {uci} after {' '.join(moves)}")
        board.push(move)
        moves.append(uci)


def elo(score):
    score = min(max(score, 1e-6), 1 - 1e-6)
    return -400 * math.log10(1 / score - 1)


class Tally:
    """A's results over a set of games."""

    def __init__(self):
        self.wins = self.draws = self.losses = self.plies = 0
        self.endings = {}

    def add(self, headers, a_name):
        if a_name not in (headers.get("White"), headers.get("Black")):
            raise ValueError(f"a game without {a_name!r}")
        result = headers.get("Result")
        if result not in ("1-0", "0-1", "1/2-1/2"):
            raise ValueError(f"a game without a final result ({result!r})")
        why = headers.get("Termination", "?")
        self.endings[why] = self.endings.get(why, 0) + 1
        self.plies += int(headers.get("PlyCount", 0))
        if result == "1/2-1/2":
            self.draws += 1
        elif (result == "1-0") == (headers.get("White") == a_name):
            self.wins += 1
        else:
            self.losses += 1

    def games(self):
        return self.wins + self.draws + self.losses

    def summary(self):
        n = self.games()
        if n == 0:
            return "games 0"
        s = (self.wins + 0.5 * self.draws) / n
        var = (self.wins * (1 - s) ** 2 + self.draws * (0.5 - s) ** 2 + self.losses * s ** 2) / n
        se = math.sqrt(var / n)
        return (f"games {n}  A: +{self.wins} ={self.draws} -{self.losses}  score {s:.3f}  "
                f"Elo(A-B) {elo(s):+.0f} [{elo(s - 1.96 * se):+.0f}, {elo(s + 1.96 * se):+.0f}]  "
                f"mean plies {self.plies / n:.0f}  end: {self.endings}")


def read_games(path, a_name, tally):
    with open(path) as f:
        while True:
            headers = chess.pgn.read_headers(f)
            if headers is None:
                return
            try:
                tally.add(headers, a_name)
            except ValueError as e:
                sys.exit(f"{path}: {e}")


def play(args):
    if args.a_name == args.b_name:
        sys.exit("--a-name and --b-name must differ")
    tally = Tally()
    if os.path.exists(args.pgn):
        read_games(args.pgn, args.a_name, tally)
    first = tally.games()
    if first >= args.games:
        print(tally.summary() + "  (already complete)", flush=True)
        return
    common = parse_options(args.common_opt)
    engines = []
    try:
        a = Engine(args.a, common + parse_options(args.a_opt), args.a_go)
        engines.append(a)
        b = Engine(args.b, common + parse_options(args.b_opt), args.b_go)
        engines.append(b)
        referee = Engine(args.referee, common, f"depth {args.ref_depth}")
        engines.append(referee)
        t0 = time.time()
        with open(args.pgn, "a") as out:
            for g in range(first, args.games):
                a_white = g % 2 == 0
                white, black = (a, b) if a_white else (b, a)
                board, result, why = play_game(white, black, referee, args)
                game = chess.pgn.Game.from_board(board)
                game.headers["Event"] = args.event or f"{args.a_name} vs {args.b_name}"
                game.headers["Round"] = str(g + 1)
                game.headers["White"] = args.a_name if a_white else args.b_name
                game.headers["Black"] = args.b_name if a_white else args.a_name
                game.headers["Result"] = result
                game.headers["Termination"] = why
                game.headers["PlyCount"] = str(len(board.move_stack))
                game.headers["WhiteNodesPerMove"] = str(white.nodes // max(1, white.searches))
                game.headers["BlackNodesPerMove"] = str(black.nodes // max(1, black.searches))
                if hasattr(os, "getloadavg"):
                    game.headers["LoadAverage"] = f"{os.getloadavg()[0]:.1f}"
                print(game, file=out, end="\n\n", flush=True)
                tally.add(game.headers, args.a_name)
        print(f"{tally.summary()}  {time.time() - t0:.0f} s for {args.games - first} games", flush=True)
    finally:
        for engine in engines:
            engine.quit()


def pool(args):
    tally = Tally()
    for path in args.files:
        read_games(path, args.a_name, tally)
    print(tally.summary())


def main():
    ap = argparse.ArgumentParser(description="Engine matches for calibrating Scacelith's strength presets "
                                             "(see the top of this file).")
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("play", help="play a match (or resume one) and print A's result")
    p.add_argument("--games", type=int, default=100, help="total games in --pgn (default 100)")
    p.add_argument("--pgn", required=True, help="games are appended here; an existing file is resumed")
    for side in ("a", "b"):
        p.add_argument(f"--{side}", required=True, help=f"engine {side.upper()} (UCI executable)")
        p.add_argument(f"--{side}-name", default=side.upper(), help="player name in the PGN file")
        p.add_argument(f"--{side}-opt", action="append", metavar="NAME=VALUE", help="UCI option (repeatable)")
        p.add_argument(f"--{side}-go", default="depth 1", metavar="ARGS", help='go arguments (default "depth 1")')
    p.add_argument("--referee", required=True, help="adjudicating engine (UCI executable)")
    p.add_argument("--ref-depth", type=int, default=12, help="adjudication search depth (default 12)")
    p.add_argument("--max-plies", type=int, default=300, help="adjudicate after this many plies (default 300)")
    p.add_argument("--win-cp", type=int, default=300, help="adjudicated win above this evaluation (default 300)")
    p.add_argument("--common-opt", action="append", default=["Threads=1", "Hash=64"], metavar="NAME=VALUE",
                   help="UCI option for all three engines (repeatable; added to Threads=1 and Hash=64)")
    p.add_argument("--event", default="", help='PGN event (default "A vs B")')
    p.set_defaults(run=play)
    q = sub.add_parser("pool", help="print A's result over the games of several PGN files")
    q.add_argument("--a-name", required=True, help="the player whose result is printed")
    q.add_argument("files", nargs="+", metavar="FILE.pgn")
    q.set_defaults(run=pool)
    args = ap.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
