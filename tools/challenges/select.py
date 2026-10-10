#!/usr/bin/env python3
"""Picks the candidate positions of the coach's challenges from sampled Lichess puzzles.

    python3 -I tools/challenges/select.py PUZZLES.tsv... [--out tools/challenges/candidates.txt]

Input: rows of the Lichess puzzle database (https://database.lichess.org/#puzzles, CC0), tab
separated: PuzzleId, FEN, Moves, Rating, Popularity, NbPlays, Themes (comma separated); a header
row and rows that cannot be read are skipped. In a Lichess puzzle the FEN is the position before
the opponent's move Moves[0]; the solver answers with Moves[1], and so on. Here Moves[0] becomes
the coach's "lead" and Moves[1:] the line; when the solver is Black, the FEN and every move are
mirrored (ranks reversed, colours swapped, as coach::mirrorFen / mirrorUci) so that the player
always has White, and the source gets a "/m".

Output: tools/challenges/candidates.txt, in the format of assets/coach/challenges/challenges.txt
with two additions that only the audit reads:
    # want <n>           right after a challenge line: the positions the set needs
    line <src> <rating> | <fen> | - | ?
                         an escape candidate: White (the defender) to move, no lead, the line is
                         left to the audit (the one move that holds)
Every set of the menu is written, in menu order; the hand-written endgames (tools/challenges/
endgames.txt) only get their header here. The candidates of a set come in preference order:
round-robin over <want> rating bands (or over the mating patterns), the most popular puzzles of
each band first. tools/challenge_audit keeps the first <want> that pass its checks and sorts them
by rating. Each puzzle is a candidate of one set at most.

The rows are untrusted: only their form is checked here (FEN shape, UCI moves, numbers). The
audit replays every move on a real board and drops whatever is illegal or not proven.
"""

import argparse
import math
import os
import re
import sys

# The menu: id, group, level, positions wanted, then the selection rule.
#   themes: any of these Lichess themes; mate: True = mate puzzles only, False = no mate puzzle,
#   None = either; rating: (low, high) inclusive, None = any; moves: allowed player-move counts;
#   kind: "line" (Lichess line), "escape" (made from mate puzzles), "play" (endgames.txt).
PATTERNS = ["smotheredMate", "anastasiaMate", "arabianMate", "epauletteMate", "operaMate",
            "pillsburysMate", "bodenMate", "doubleBishopMate", "hookMate", "dovetailMate",
            "swallowstailMate", "cornerMate", "triangleMate", "morphysMate", "blindSwineMate",
            "vukovicMate", "killBoxMate", "balestraMate"]
SETS = [
    dict(id="mate1", group="mates", level=1, want=8, themes=["mateIn1"], mate=True, rating=(400, 1300)),
    dict(id="mate2", group="mates", level=2, want=6, themes=["mateIn2"], mate=True, rating=(900, 1600)),
    dict(id="mate3", group="mates", level=4, want=5, themes=["mateIn3"], mate=True, rating=(1300, 2100), moves=(3,)),
    dict(id="backrank", group="mates", level=2, want=6, themes=["backRankMate"], mate=True),
    dict(id="patterns", group="mates", level=3, want=6, themes=PATTERNS, mate=True, by_theme=True),
    dict(id="hanging", group="tactics", level=1, want=6, themes=["hangingPiece"], mate=False, rating=(0, 1299)),
    dict(id="fork", group="tactics", level=2, want=6, themes=["fork"], mate=False, rating=(800, 1700)),
    dict(id="pin", group="tactics", level=2, want=6, themes=["pin"], mate=False, rating=(900, 1800)),
    dict(id="skewer", group="tactics", level=3, want=6, themes=["skewer"], mate=False, rating=(900, 1900)),
    dict(id="discovered", group="tactics", level=3, want=6,
         themes=["discoveredAttack", "discoveredCheck", "doubleCheck"], mate=False, rating=(900, 1900)),
    dict(id="deflection", group="tactics", level=4, want=6,
         themes=["deflection", "attraction", "capturingDefender"], mate=False, rating=(1100, 2000)),
    dict(id="intermezzo", group="tactics", level=4, want=5, themes=["intermezzo"], rating=(1200, 2100)),
    dict(id="trapped", group="tactics", level=3, want=5, themes=["trappedPiece"]),
    dict(id="promotion", group="tactics", level=2, want=6, themes=["promotion", "advancedPawn"], mate=False,
         rating=(800, 1800)),
    dict(id="quiet", group="tactics", level=5, want=5, themes=["quietMove"], mate=False, rating=(1400, 2300)),
    dict(id="opening", group="tactics", level=2, want=6, themes=["opening"], all_themes=["short"], mate=False,
         rating=(800, 1600)),
    dict(id="escape", group="defence", level=3, want=6, themes=["mateIn2", "mateIn3"], mate=True, kind="escape",
         factor=25),
    dict(id="defend", group="defence", level=4, want=6, themes=["defensiveMove"], mate=False, rating=(1000, 2100)),
    dict(id="kq", group="endgames", level=1, want=3, kind="play"),
    dict(id="kr", group="endgames", level=2, want=3, kind="play"),
    dict(id="kp", group="endgames", level=2, want=4, kind="play"),
    dict(id="kp_hold", group="endgames", level=3, want=3, kind="play"),
    dict(id="rook", group="endgames", level=4, want=3, kind="play"),
    dict(id="endgame", group="endgames", level=3, want=6, themes=["pawnEndgame", "rookEndgame", "endgame"],
         mate=False, rating=(900, 1900), prefer=["pawnEndgame", "rookEndgame"]),
]
# The order in which the sets choose their puzzles (a puzzle goes to the first set that takes it):
# the sets with the fewest puzzles to choose from first, the escape last (it may use any mate).
CHOICE_ORDER = ["patterns", "backrank", "mate3", "intermezzo", "trapped", "quiet", "skewer", "deflection",
                "discovered", "defend", "promotion", "pin", "opening", "hanging", "endgame", "fork", "mate2",
                "mate1", "escape"]
FACTOR = 4                 # candidates per wanted position (the audit drops many)
MIN_POPULARITY = 70        # Lichess popularity (-100..100): below, the puzzle is often disputed
MIN_PLAYS = 50

UCI = re.compile(r"^[a-h][1-8][a-h][1-8][qrbn]?$")
ID = re.compile(r"^[A-Za-z0-9]{3,10}$")


def mirror_uci(uci):
    """coach::mirrorUci: the ranks reversed."""
    return uci[0] + str(9 - int(uci[1])) + uci[2] + str(9 - int(uci[3])) + uci[4:]


def mirror_fen(fen):
    """coach::mirrorFen: ranks reversed, colours swapped (castling and en passant follow)."""
    f = fen.split()
    board = "/".join(r.swapcase() for r in reversed(f[0].split("/")))
    side = "b" if f[1] == "w" else "w"
    if f[2] == "-":
        castling = "-"
    else:
        castling = "".join(c.upper() for c in f[2] if c.islower()) + "".join(c.lower() for c in f[2] if c.isupper())
    ep = f[3]
    if len(ep) == 2 and ep[1] in "12345678":
        ep = ep[0] + str(9 - int(ep[1]))
    return " ".join([board, side, castling, ep] + f[4:])


def fen_shape_ok(fen):
    f = fen.split()
    if len(f) != 6 or f[1] not in ("w", "b") or not re.match(r"^(-|[KQkq]{1,4})$", f[2]):
        return False
    if not re.match(r"^(-|[a-h][36])$", f[3]) or not f[4].isdigit() or not f[5].isdigit():
        return False
    ranks = f[0].split("/")
    if len(ranks) != 8:
        return False
    for r in ranks:
        n = 0
        for c in r:
            if c.isdigit():
                n += int(c)
            elif c in "pnbrqkPNBRQK":
                n += 1
            else:
                return False
        if n != 8:
            return False
    return f[0].count("K") == 1 and f[0].count("k") == 1


def piece_at(fen, square):
    """The FEN letter on a square ('' when empty)."""
    rows = fen.split()[0].split("/")
    row = rows[8 - int(square[1])]
    file = ord(square[0]) - ord("a")
    x = 0
    for c in row:
        if c.isdigit():
            x += int(c)
        else:
            if x == file:
                return c
            x += 1
        if x > file:
            return ""
    return ""


def read_rows(paths):
    rows, bad, seen = [], 0, set()
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw in f:
                raw = raw.rstrip("\r\n")
                if not raw.strip() or raw.startswith("PuzzleId"):
                    continue
                c = [x.strip() for x in raw.split("\t")]
                try:
                    if len(c) != 7 or not ID.match(c[0]) or not fen_shape_ok(c[1]):
                        raise ValueError
                    moves = c[2].split()
                    if len(moves) < 2 or len(moves) % 2 or not all(UCI.match(m) for m in moves):
                        raise ValueError
                    # The opponent's move starts from one of its own pieces.
                    p = piece_at(c[1], moves[0][:2])
                    if not p or p.isupper() != (c[1].split()[1] == "w"):
                        raise ValueError
                    row = dict(id=c[0], fen=c[1], moves=moves, rating=int(c[3]), popularity=int(c[4]),
                               plays=int(c[5]), themes=set(t for t in c[6].split(",") if t))
                except ValueError:
                    bad += 1
                    continue
                if row["id"] in seen:
                    continue
                seen.add(row["id"])
                rows.append(row)
    return rows, bad


def is_mate(row):
    return "mate" in row["themes"] or any(t.startswith("mateIn") for t in row["themes"])


def eligible(row, s):
    themes = row["themes"]
    if "veryLong" in themes or row["popularity"] < MIN_POPULARITY or row["plays"] < MIN_PLAYS:
        return False
    if not themes.intersection(s["themes"]) or not set(s.get("all_themes", [])).issubset(themes):
        return False
    mate = s.get("mate")
    if mate is not None and is_mate(row) != mate:
        return False
    if s.get("kind", "line") == "line":
        k = len(row["moves"]) // 2
        if k not in s.get("moves", (1, 2, 3)):
            return False
        if s["id"] in ("backrank", "patterns") and not themes.intersection({"mateIn1", "mateIn2", "mateIn3"}):
            return False
    r = s.get("rating")
    return r is None or r[0] <= row["rating"] <= r[1]


def preference(row, s):
    """Higher first: popular, often played, with one of the set's preferred themes."""
    bonus = 10 if row["themes"].intersection(s.get("prefer", [])) else 0
    return row["popularity"] + 8 * math.log10(row["plays"] + 1) + bonus


def round_robin(buckets, count):
    out = []
    depth = 0
    while len(out) < count and any(depth < len(b) for b in buckets):
        for b in buckets:
            if depth < len(b) and len(out) < count:
                out.append(b[depth])
        depth += 1
    return out


def choose(rows, s, used):
    pool = [r for r in rows if r["id"] not in used and eligible(r, s)]
    count = s["want"] * s.get("factor", FACTOR)
    if s.get("by_theme"):
        # One mating pattern per bucket, the classics first.
        buckets, taken = [], set()
        for theme in dict.fromkeys(s["themes"]):
            b = [r for r in pool if theme in r["themes"] and r["id"] not in taken]
            b.sort(key=lambda r: -preference(r, s))
            taken.update(r["id"] for r in b)
            if b:
                buckets.append(b)
    else:
        if not pool:
            return []
        lo = s["rating"][0] if s.get("rating") else min(r["rating"] for r in pool)
        hi = s["rating"][1] if s.get("rating") else max(r["rating"] for r in pool)
        n = s["want"]
        buckets = [[] for _ in range(n)]
        for r in pool:
            i = min(n - 1, max(0, int((r["rating"] - lo) * n / max(1, hi - lo + 1))))
            buckets[i].append(r)
        for b in buckets:
            b.sort(key=lambda r: -preference(r, s))
    return round_robin(buckets, count)


def line_entry(row):
    """The candidate line of a Lichess puzzle, mirrored when the solver is Black."""
    fen, moves, src = row["fen"], row["moves"], "lichess:" + row["id"]
    if fen.split()[1] == "w":   # White plays the lead: the solver is Black
        fen, moves, src = mirror_fen(fen), [mirror_uci(m) for m in moves], src + "/m"
    return "line %s %d | %s | %s | %s" % (src, row["rating"], fen, moves[0], " ".join(moves[1:]))


def escape_entry(row):
    """The set-up of a mate puzzle as an escape candidate: the defender (to move) gets White."""
    fen, src = row["fen"], "lichess:" + row["id"]
    if fen.split()[1] == "b":
        fen, src = mirror_fen(fen), src + "/m"
    return "line %s %d | %s | - | ?" % (src, row["rating"], fen)


HEADER = """\
# Candidates of the coach's challenges, written by tools/challenges/select.py from sampled rows of
# the Lichess puzzle database (CC0). Do not edit: run select.py again. tools/challenge_audit checks
# them with the embedded Stockfish, keeps the first <want> valid ones of each set (here in
# preference order: round-robin over rating bands, the most popular first), sorts them by rating
# and writes assets/coach/challenges/challenges.txt (whose header documents the format).
# Two additions to that format, read by the audit only:
#   # want <n>                               (after a challenge line) the positions the set needs
#   line <src> <rating> | <fen> | - | ?      an escape candidate: the set-up of a mate puzzle, the
#                                            defender (White) to move; the audit finds the move
#                                            that holds. <rating> is the mate puzzle's.
# Sets without candidates here are hand-written: tools/challenges/endgames.txt.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("tsv", nargs="+", help="Lichess puzzle rows (TSV)")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "candidates.txt"))
    a = ap.parse_args()
    rows, bad = read_rows(a.tsv)
    rows.sort(key=lambda r: r["id"])   # input order never matters
    picked, used = {}, set()
    by_id = {s["id"]: s for s in SETS}
    for sid in CHOICE_ORDER:
        s = by_id[sid]
        picked[sid] = choose(rows, s, used)
        used.update(r["id"] for r in picked[sid])
    out = [HEADER]
    for s in SETS:
        out.append("\nchallenge %s %s %d\n# want %d\n" % (s["id"], s["group"], s["level"], s["want"]))
        kind = s.get("kind", "line")
        if kind == "play":
            out.append("# hand-written: tools/challenges/endgames.txt\n")
            continue
        for r in picked[s["id"]]:
            out.append((escape_entry(r) if kind == "escape" else line_entry(r)) + "\n")
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(out))
    print("%d puzzles read (%d rows skipped)" % (len(rows), bad))
    for s in SETS:
        if s.get("kind") == "play":
            continue
        p = picked[s["id"]]
        rng = "%d-%d" % (min(r["rating"] for r in p), max(r["rating"] for r in p)) if p else "-"
        short = "" if len(p) >= 2 * s["want"] else "  (fewer than twice the count)"
        print("  %-11s %3d candidates for %d, rating %s%s" % (s["id"], len(p), s["want"], rng, short))
    print("written: %s" % a.out)


if __name__ == "__main__":
    sys.exit(main())
