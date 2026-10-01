// Board facts for the coach's explanations (see tactics.h). The motif rules follow the lichess
// puzzle tagger (lichess-puzzler tagger/cook.py, util.py) where research-pedagogy §2 cites it.
#include "coach/tactics.h"

#include <algorithm>
#include <cstdlib>

namespace coach {

using namespace chess;
using U64 = uint64_t;

namespace {

inline U64 bit(Square s) { return squareBit(s); }
inline Square lowest(U64 b) { return Square(__builtin_ctzll(b)); }
inline bool several(U64 b) { return (b & (b - 1)) != 0; }
inline U64 rankMask(int r) { return 0xFFULL << (8 * r); }
inline int value(PieceType t) { return kPieceValueCp[t]; }
inline int sign(int v) { return (v > 0) - (v < 0); }
inline int chebyshev(Square a, Square b) {
    return std::max(std::abs(fileOf(a) - fileOf(b)), std::abs(rankOf(a) - rankOf(b)));
}
inline bool isLastRank(Square s) { return rankOf(s) == 0 || rankOf(s) == 7; }

// Swap-list exchange on 'to': the piece of type 'moverType' and colour 'mover' standing on 'from'
// captures 'victim' (NoPiece for a quiet move), then both sides recapture with their least
// valuable attacker while it pays. Centipawns for 'mover'.
int exchange(const Position& pos, Square from, Square to, Color mover, PieceType moverType, PieceType victim,
             bool enPassant, PieceType promotion) {
    U64 occ = pos.occupancy() ^ bit(from);
    int gain[40];
    int d = 0;
    if (enPassant) {
        victim = Pawn;
        occ ^= bit(Square(to + (mover == White ? -8 : 8)));
    }
    gain[0] = value(victim);
    PieceType onSquare = moverType;
    if (promotion != NoPiece) {
        gain[0] += value(promotion) - value(Pawn);
        onSquare = promotion;
    }
    Color side = opposite(mover);
    U64 attackers = pos.attackersTo(to, occ);
    const U64 diag = pos.pieces(Bishop) | pos.pieces(Queen), orth = pos.pieces(Rook) | pos.pieces(Queen);
    while (d < 38) {
        const U64 mine = attackers & pos.pieces(side);
        if (!mine) break;
        int t = Pawn;
        while (!(mine & pos.pieces(PieceType(t)))) ++t;
        if (t == King && (attackers & pos.pieces(opposite(side)))) break;  // never into a defended square
        ++d;
        gain[d] = value(onSquare) - gain[d - 1];
        onSquare = PieceType(t);
        if (t == Pawn && isLastRank(to)) {  // a pawn recapturing on the last rank promotes
            gain[d] += value(Queen) - value(Pawn);
            onSquare = Queen;
        }
        occ ^= bit(lowest(mine & pos.pieces(PieceType(t))));
        attackers |= (attacksOf(Bishop, White, to, occ) & diag) | (attacksOf(Rook, White, to, occ) & orth);  // x-rays
        attackers &= occ;
        side = opposite(side);
    }
    while (d > 0) {
        gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
        --d;
    }
    return gain[0];
}

// Squares every piece of c attacks.
U64 attacksBy(const Position& p, Color c) {
    U64 out = 0;
    U64 b = p.pieces(c);
    while (b) {
        const Square s = lowest(b);
        b &= b - 1;
        out |= p.attacksFrom(s);
    }
    return out;
}

// Mixedness of a position (scalachess Divider.scala): each 2x2 region scores by how many white and
// black pieces it holds and how far up the board it lies ('y' = 1-based rank of its lower row).
int regionScore(int y, int white, int black) {
    switch (white * 10 + black) {
    case 0: return 0;
    case 10: return 1 + (8 - y);
    case 20: return y > 2 ? 2 + (y - 2) : 0;
    case 30: return y > 1 ? 3 + (y - 1) : 0;
    case 40: return y > 1 ? 3 + (y - 1) : 0;
    case 1: return 1 + y;
    case 11: return 5 + std::abs(3 - y);
    case 21: return 4 + y;
    case 31: return 5 + y;
    case 2: return y < 6 ? 2 + (6 - y) : 0;
    case 12: return 4 + (6 - y);
    case 22: return 7;
    case 3: return y < 7 ? 3 + (7 - y) : 0;
    case 13: return 5 + (6 - y);
    case 4: return y < 7 ? 3 + (7 - y) : 0;
    default: return 0;
    }
}

int mixedness(const Position& p) {
    int total = 0;
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 7; ++x) {
            int white = 0, black = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const Piece pc = p.at(makeSquare(x + dx, y + dy));
                    if (pc.empty()) continue;
                    (pc.color == White ? white : black) += 1;
                }
            total += regionScore(y + 1, white, black);
        }
    }
    return total;
}

bool backrankSparse(const Position& p) {
    return squareCount(p.pieces(White) & rankMask(0)) < 4 || squareCount(p.pieces(Black) & rankMask(7)) < 4;
}

}  // namespace

// ---- Material ----------------------------------------------------------------------------------

int material(const Position& p, Color c) {
    int sum = 0;
    for (int t = Pawn; t <= Queen; ++t) sum += kPiecePoints[t] * squareCount(p.pieces(c, PieceType(t)));
    return sum;
}

int materialBalance(const Position& p, Color pov) { return material(p, pov) - material(p, opposite(pov)); }

int majorsAndMinors(const Position& p) {
    return squareCount(p.occupancy() & ~(p.pieces(King) | p.pieces(Pawn)));
}

int see(const Position& p, const Move& m) {
    if (!m.valid()) return 0;
    const Piece mover = p.at(m.from);
    if (mover.empty()) return 0;
    const Piece target = p.at(m.to);
    if (!target.empty() && target.color == mover.color) return 0;  // castling or nonsense
    const bool ep = mover.type == Pawn && fileOf(m.from) != fileOf(m.to) && target.empty();
    const PieceType promo = (mover.type == Pawn && isLastRank(m.to)) ? (m.promotion != NoPiece ? m.promotion : Queen) : NoPiece;
    return exchange(p, m.from, m.to, mover.color, mover.type, target.type, ep, promo);
}

int seeSquare(const Position& p, Square sq, Color by) {
    if (sq < 0 || sq > 63) return 0;
    const Piece target = p.at(sq);
    if (target.empty() || target.color == by) return 0;
    U64 attackers = p.attackersTo(sq, by);
    bool any = false;
    int best = 0;
    while (attackers) {
        const Square a = lowest(attackers);
        attackers &= attackers - 1;
        const PieceType t = p.at(a).type;
        if (t == King && (p.attackersTo(sq, p.occupancy() ^ bit(a)) & p.pieces(opposite(by)))) continue;
        const PieceType promo = (t == Pawn && isLastRank(sq)) ? Queen : NoPiece;
        const int v = exchange(p, a, sq, by, t, target.type, false, promo);
        if (!any || v > best) best = v;
        any = true;
    }
    return any ? best : 0;
}

bool isUndefended(const Position& p, Square s) {
    const Piece x = p.at(s);
    if (x.empty()) return false;
    return p.attackersTo(s, opposite(x.color)) && !p.attackersTo(s, x.color);
}

U64 hangingPieces(const Position& p, Color c, bool undefendedOnly) {
    U64 out = 0;
    U64 b = p.pieces(c) & ~p.pieces(King);
    while (b) {
        const Square s = lowest(b);
        b &= b - 1;
        if (!p.attackersTo(s, opposite(c))) continue;
        if (undefendedOnly ? !p.attackersTo(s, c) : seeSquare(p, s, opposite(c)) > 0) out |= bit(s);
    }
    return out;
}

// ---- Motifs ------------------------------------------------------------------------------------

U64 forkTargets(const Position& after, Square sq) {
    const Piece f = after.at(sq);
    if (f.empty()) return 0;
    const Color us = f.color, them = opposite(us);
    U64 targets = after.attacksFrom(sq) & after.pieces(them);
    U64 kept = 0;
    while (targets) {
        const Square t = lowest(targets);
        targets &= targets - 1;
        const PieceType tt = after.at(t).type;
        if (tt == King || value(tt) > value(f.type) || seeSquare(after, t, us) > 0) kept |= bit(t);
    }
    if (!several(kept)) return 0;
    if (seeSquare(after, sq, them) > 0) return 0;  // the forking piece itself can be won: no fork
    return kept;
}

std::vector<Pin> pins(const Position& p, Color victim) {
    std::vector<Pin> out;
    const Color them = opposite(victim);
    const U64 occ = p.occupancy();
    U64 targets = p.pieces(victim) & ~p.pieces(Pawn);
    while (targets) {
        const Square t = lowest(targets);
        targets &= targets - 1;
        const PieceType tt = p.at(t).type;
        U64 snipers = (attacksOf(Rook, them, t, 0) & (p.pieces(them, Rook) | p.pieces(them, Queen))) |
                      (attacksOf(Bishop, them, t, 0) & (p.pieces(them, Bishop) | p.pieces(them, Queen)));
        while (snipers) {
            const Square s = lowest(snipers);
            snipers &= snipers - 1;
            const U64 b = squaresBetween(t, s) & occ;
            if (!b || several(b) || !(b & p.pieces(victim))) continue;
            const Square pinned = lowest(b);
            const bool absolute = tt == King;
            if (!absolute && value(tt) <= value(p.at(pinned).type)) continue;
            if (p.attacksFrom(pinned) & bit(s)) continue;  // it can take the pinner along the line
            out.push_back(Pin{s, pinned, t, absolute});
        }
    }
    return out;
}

std::vector<Skewer> skewers(const Position& p, Color victim) {
    std::vector<Skewer> out;
    const Color them = opposite(victim);
    U64 sliders = p.pieces(them, Bishop) | p.pieces(them, Rook) | p.pieces(them, Queen);
    while (sliders) {
        const Square s = lowest(sliders);
        sliders &= sliders - 1;
        U64 fronts = p.attacksFrom(s) & p.pieces(victim);
        while (fronts) {
            const Square f = lowest(fronts);
            fronts &= fronts - 1;
            const int df = sign(fileOf(f) - fileOf(s)), dr = sign(rankOf(f) - rankOf(s));
            int ff = fileOf(f) + df, rr = rankOf(f) + dr;
            while (ff >= 0 && ff < 8 && rr >= 0 && rr < 8 && p.at(makeSquare(ff, rr)).empty()) ff += df, rr += dr;
            if (ff < 0 || ff > 7 || rr < 0 || rr > 7) continue;
            const Square b = makeSquare(ff, rr);
            const Piece behind = p.at(b);
            if (behind.color != victim || behind.type == Pawn || behind.type == King) continue;
            const PieceType ft = p.at(f).type;
            if (ft == King || value(ft) > value(behind.type)) out.push_back(Skewer{s, f, b});
        }
    }
    return out;
}

std::vector<Discovery> discoveredAttacks(const Position& before, const Move& m) {
    std::vector<Discovery> out;
    if (!m.valid() || (m.flags & (MoveCastleKing | MoveCastleQueen))) return out;
    const Piece mover = before.at(m.from);
    if (mover.empty()) return out;
    Position after = before;
    after.makeMove(m);
    const Color us = mover.color, them = opposite(us);
    const U64 sliders = (after.pieces(us, Bishop) | after.pieces(us, Rook) | after.pieces(us, Queen)) & ~bit(m.to);
    U64 targets = after.pieces(them);
    while (targets) {
        const Square t = lowest(targets);
        targets &= targets - 1;
        U64 fresh = after.attackersTo(t, us) & sliders & ~before.attackersTo(t, us);
        while (fresh) {
            const Square s = lowest(fresh);
            fresh &= fresh - 1;
            if (squaresBetween(s, t) & bit(m.from)) out.push_back(Discovery{s, t, after.at(t).type == King});
        }
    }
    return out;
}

bool isDoubleCheck(const Position& p) { return several(p.checkers()); }

// ---- Checks, mates, back rank --------------------------------------------------------------------

bool mateInOne(const Position& p, Move* out) {
    for (const Move& m : p.legalMoves()) {
        Position q = p;
        q.makeMove(m);
        if (q.isCheckmate()) {
            if (out) *out = m;
            return true;
        }
    }
    return false;
}

bool mateThreat(const Position& p, Color by, Move* out) {
    if (p.sideToMove() == by) return mateInOne(p, out);
    Position q = p;
    if (!q.passTurn()) return false;
    return mateInOne(q, out);
}

std::vector<Move> checkingMoves(const Position& p) {
    std::vector<Move> out;
    for (const Move& m : p.legalMoves()) {
        Position q = p;
        q.makeMove(m);
        if (q.inCheck()) out.push_back(m);
    }
    return out;
}

U64 escapeSquares(const Position& p, Color c) {
    const Square k = p.kingSquare(c);
    if (k == NoSquare) return 0;
    const U64 occNoKing = p.occupancy() ^ bit(k);
    U64 around = attacksOf(King, c, k, 0) & ~p.pieces(c);
    U64 out = 0;
    while (around) {
        const Square n = lowest(around);
        around &= around - 1;
        if (p.attackersTo(n, occNoKing) & p.pieces(opposite(c)) & ~bit(n)) continue;
        out |= bit(n);
    }
    return out;
}

bool backRankWeak(const Position& p, Color c) {
    const Square k = p.kingSquare(c);
    const int home = c == White ? 0 : 7;
    if (k == NoSquare || rankOf(k) != home) return false;
    const Color them = opposite(c);
    if (!(p.pieces(them, Rook) | p.pieces(them, Queen))) return false;
    return (escapeSquares(p, c) & ~rankMask(home)) == 0;
}

MatePattern classifyMate(const Position& mated) {
    if (!mated.isCheckmate()) return MatePattern::None;
    const Color c = mated.sideToMove(), them = opposite(c);
    const Square k = mated.kingSquare(c);
    const U64 ch = mated.checkers();
    if (several(ch)) return MatePattern::Other;
    const Square cs = lowest(ch);
    const PieceType ct = mated.at(cs).type;
    const U64 around = attacksOf(King, c, k, 0);
    if (ct == Knight && (around & ~mated.pieces(c)) == 0) return MatePattern::Smothered;
    const int home = c == White ? 0 : 7, fwd = c == White ? 1 : -1;
    const bool heavy = ct == Rook || ct == Queen;
    if (rankOf(k) == home && heavy && rankOf(cs) == home && (around & rankMask(home + fwd) & mated.pieces(c)))
        return MatePattern::BackRank;
    if (ct == Queen && (around & bit(cs)) && mated.attackersTo(cs, them)) return MatePattern::Support;
    if (heavy) {
        // Ladder: the king on an edge line, checked along it, the next line inward held by another heavy piece.
        const U64 others = (mated.pieces(them, Rook) | mated.pieces(them, Queen)) & ~bit(cs);
        const int kr = rankOf(k), kf = fileOf(k);
        if ((kr == 0 || kr == 7) && rankOf(cs) == kr) {
            const int inner = kr == 0 ? 1 : 6;
            U64 o = others;
            while (o) {
                const Square s = lowest(o);
                o &= o - 1;
                if (rankOf(s) == inner) return MatePattern::Ladder;
            }
        }
        if ((kf == 0 || kf == 7) && fileOf(cs) == kf) {
            const int inner = kf == 0 ? 1 : 6;
            U64 o = others;
            while (o) {
                const Square s = lowest(o);
                o &= o - 1;
                if (fileOf(s) == inner) return MatePattern::Ladder;
            }
        }
    }
    // Epaulette: own pieces on both sides of the king, the checker on the king's file.
    const int kf = fileOf(k), kr = rankOf(k);
    if (kf > 0 && kf < 7 && fileOf(cs) == kf) {
        const Piece l = mated.at(makeSquare(kf - 1, kr)), r = mated.at(makeSquare(kf + 1, kr));
        if (!l.empty() && l.color == c && !r.empty() && r.color == c) return MatePattern::Epaulette;
    }
    return MatePattern::Other;
}

// ---- Pieces and pawns ---------------------------------------------------------------------------

int mobility(const Position& p, Color c) {
    int n = 0;
    U64 b = p.pieces(c) & (p.pieces(Knight) | p.pieces(Bishop) | p.pieces(Rook) | p.pieces(Queen));
    while (b) {
        const Square s = lowest(b);
        b &= b - 1;
        n += squareCount(p.attacksFrom(s) & ~p.pieces(c));
    }
    return n;
}

bool isTrapped(const Position& p, Square s) {
    const Piece x = p.at(s);
    if (x.empty() || x.type == Pawn || x.type == King) return false;
    const Color c = x.color;
    Position q = p;
    if (q.sideToMove() != c && !q.passTurn()) return false;
    if (q.inCheck() || (q.pinned(c) & bit(s))) return false;
    if (seeSquare(q, s, opposite(c)) <= 0) return false;
    for (const Move& m : q.legalMovesFrom(s))
        if (see(q, m) >= 0) return false;  // a safe square, or a capture worth the piece
    return true;
}

bool isPassed(const Position& p, Square pawn) {
    const Piece x = p.at(pawn);
    if (x.type != Pawn) return false;
    const int f = fileOf(pawn), r = rankOf(pawn);
    U64 ahead = 0;
    for (int ff = std::max(0, f - 1); ff <= std::min(7, f + 1); ++ff) {
        if (x.color == White)
            for (int rr = r + 1; rr < 8; ++rr) ahead |= bit(makeSquare(ff, rr));
        else
            for (int rr = r - 1; rr >= 0; --rr) ahead |= bit(makeSquare(ff, rr));
    }
    return !(ahead & p.pieces(opposite(x.color), Pawn));
}

int undevelopedMinors(const Position& p, Color c) {
    const int r = c == White ? 0 : 7;
    int n = 0;
    for (int f : {1, 6})
        if (p.at(makeSquare(f, r)) == Piece{Knight, c}) ++n;
    for (int f : {2, 5})
        if (p.at(makeSquare(f, r)) == Piece{Bishop, c}) ++n;
    return n;
}

U64 attackedSquares(const Position& p, Color c) { return attacksBy(p, c); }

bool isForced(const Position& p) { return p.legalMoves().size() == 1; }

bool isRecapture(const Game& g, size_t ply) {
    if (ply == 0 || ply >= g.moves().size()) return false;
    const Move& prev = g.moves()[ply - 1];
    const Move& m = g.moves()[ply];
    return (prev.flags & MoveCapture) && (m.flags & MoveCapture) && !(prev.flags & MoveEnPassant) && m.to == prev.to;
}

int kingZoneAttacks(const Position& p, Color c) {
    const Square k = p.kingSquare(c);
    if (k == NoSquare) return 0;
    return squareCount(attacksOf(King, c, k, 0) & attacksBy(p, opposite(c)));
}

Square promotionSquare(const Position& p, Square pawn) {
    const Piece x = p.at(pawn);
    if (x.type != Pawn) return NoSquare;
    return makeSquare(fileOf(pawn), x.color == White ? 7 : 0);
}

bool outsideSquare(const Position& p, Square pawn, bool defenderToMove) {
    const Piece x = p.at(pawn);
    if (x.type != Pawn) return false;
    const Square prom = promotionSquare(p, pawn);
    int d = std::abs(rankOf(prom) - rankOf(pawn));
    if (rankOf(pawn) == (x.color == White ? 1 : 6)) d -= 1;  // the double step saves a move
    const Square k = p.kingSquare(opposite(x.color));
    if (k == NoSquare) return true;
    // The pawn's side to move: the king must already stand inside the square (distance <= d);
    // the king to move gets one step more.
    return chebyshev(k, prom) > d + (defenderToMove ? 1 : 0);
}

// ---- Phases --------------------------------------------------------------------------------------

Phases dividePhases(const Game& g) {
    Phases ph;
    const size_t n = g.moves().size();
    for (size_t k = 0; k <= n; ++k) {
        const Position& p = g.positionAt(k);
        if (majorsAndMinors(p) <= 10 || backrankSparse(p) || mixedness(p) > 150) {
            ph.middlegame = int(k);
            break;
        }
    }
    if (ph.middlegame >= 0) {
        for (size_t k = 0; k <= n; ++k)
            if (majorsAndMinors(g.positionAt(k)) <= 6) {
                ph.endgame = int(k);
                break;
            }
        if (ph.endgame >= 0 && ph.middlegame >= ph.endgame) ph.middlegame = -1;
    }
    return ph;
}

int phaseOfPly(const Phases& ph, int ply) {
    if (ph.endgame >= 0 && ply >= ph.endgame) return 2;
    if (ph.middlegame >= 0 && ply >= ph.middlegame) return 1;
    return 0;
}

// ---- Moves ----------------------------------------------------------------------------------------

MoveFacts analyzeMove(const Position& before, const Move& m) {
    MoveFacts f;
    if (!m.valid()) return f;
    const Piece pc = before.at(m.from);
    f.mover = pc.color;
    f.piece = pc.type;
    f.from = m.from;
    f.to = m.to;
    const Color us = pc.color, them = opposite(us);
    f.enPassant = pc.type == Pawn && fileOf(m.from) != fileOf(m.to) && before.at(m.to).empty();
    f.captured = f.enPassant ? Pawn : before.at(m.to).type;
    if (f.captured != NoPiece) f.capturedOn = f.enPassant ? Square(m.to + (us == White ? -8 : 8)) : m.to;
    f.castleKing = pc.type == King && fileOf(m.to) - fileOf(m.from) == 2;
    f.castleQueen = pc.type == King && fileOf(m.from) - fileOf(m.to) == 2;
    f.promotion = (pc.type == Pawn && isLastRank(m.to)) ? (m.promotion != NoPiece ? m.promotion : Queen) : NoPiece;
    Position after = before;
    after.makeMove(m);
    f.check = after.inCheck();
    f.mate = f.check && !after.hasLegalMove();
    f.stalemate = !f.check && !after.hasLegalMove();
    const U64 checkers = after.checkers();
    f.doubleCheck = several(checkers);
    f.discoveredCheck = f.check && !(checkers & bit(m.to)) && !f.castleKing && !f.castleQueen;
    f.seeCp = see(before, m);
    f.newlyAttacked = (attacksBy(after, us) & after.pieces(them)) & ~(attacksBy(before, us) & before.pieces(them));
    f.forks = forkTargets(after, m.to);
    for (const Discovery& d : discoveredAttacks(before, m)) f.discovered |= bit(d.target);
    f.leftHanging = hangingPieces(after, us, false) & ~hangingPieces(before, us, false);
    const U64 guarded = attacksOf(pc.type, us, m.from, before.occupancy()) & before.pieces(us) & ~before.pieces(King);
    const U64 guardedAfter = after.attacksFrom(m.to) & after.pieces(us);
    f.undefended = guarded & ~guardedAfter & ~bit(m.to);
    return f;
}

std::vector<LineStep> replayLine(const Position& start, const std::vector<std::string>& uci, Color pov, int maxPlies) {
    std::vector<LineStep> out;
    Position p = start;
    for (const std::string& u : uci) {
        if (int(out.size()) >= maxPlies) break;
        const Move m = p.parseUCI(u);
        if (!m.valid()) break;
        LineStep s;
        s.move = m;
        s.uci = p.toUCI(m);
        s.san = p.toSAN(m);
        const Piece pc = p.at(m.from);
        s.mover = pc.color;
        s.piece = pc.type;
        s.captured = (m.flags & MoveEnPassant) ? Pawn : p.at(m.to).type;
        s.promotion = (m.flags & MovePromotion) ? m.promotion : NoPiece;
        p.makeMove(m);
        s.check = p.inCheck();
        const bool legal = p.hasLegalMove();
        s.mate = s.check && !legal;
        s.stalemate = !s.check && !legal;
        s.balance = materialBalance(p, pov);
        out.push_back(s);
        if (!legal) break;
    }
    return out;
}

std::string sanLine(const std::vector<LineStep>& steps, size_t from, size_t count) {
    std::string out;
    for (size_t i = from; i < steps.size() && i < from + count; ++i) {
        if (!out.empty()) out += ' ';
        out += steps[i].san;
    }
    return out;
}

Square knightCorner(Square from, Square to) {
    if (from < 0 || to < 0) return NoSquare;
    const int df = fileOf(to) - fileOf(from), dr = rankOf(to) - rankOf(from);
    if (std::abs(dr) == 2 && std::abs(df) == 1) return makeSquare(fileOf(from), rankOf(to));
    if (std::abs(df) == 2 && std::abs(dr) == 1) return makeSquare(fileOf(to), rankOf(from));
    return NoSquare;
}

std::vector<Square> movePath(PieceType t, Square from, Square to) {
    std::vector<Square> out;
    if (from < 0 || to < 0 || from == to) return out;
    const int df = fileOf(to) - fileOf(from), dr = rankOf(to) - rankOf(from);
    if (t == Knight && knightCorner(from, to) != NoSquare) {
        if (std::abs(dr) == 2) {
            out.push_back(makeSquare(fileOf(from), rankOf(from) + sign(dr)));
            out.push_back(makeSquare(fileOf(from), rankOf(to)));
        } else {
            out.push_back(makeSquare(fileOf(from) + sign(df), rankOf(from)));
            out.push_back(makeSquare(fileOf(to), rankOf(from)));
        }
    } else if (t == Bishop || t == Rook || t == Queen || (t == King && std::abs(df) == 2 && dr == 0) ||
               (t == Pawn && std::abs(dr) == 2 && df == 0)) {
        const int sf = sign(df), sr = sign(dr);
        if (squaresBetween(from, to) || chebyshev(from, to) == 1)
            for (int f = fileOf(from) + sf, r = rankOf(from) + sr; makeSquare(f, r) != to; f += sf, r += sr)
                out.push_back(makeSquare(f, r));
    }
    out.push_back(to);
    return out;
}

std::vector<Square> tracePath(PieceType t, Square from, Square to) {
    const Square corner = t == Knight ? knightCorner(from, to) : NoSquare;
    if (corner != NoSquare) return {from, corner, to};
    return {from, to};
}

}  // namespace coach
