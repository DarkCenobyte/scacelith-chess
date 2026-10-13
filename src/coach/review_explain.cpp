// Explanation detectors of the coach's review: each one checks its motif on the board (tactics.h)
// along the engine's refutation or best line, and builds the lines that say it, with their pointing
// and marks. findExplanation() tries them in the order of the numbered sections below; findTip() the
// principles (opening, technique) that are said as tips, never as faults.
#include "coach/review_internal.h"

#include <algorithm>
#include <cstdlib>

namespace coach {
namespace detail {

using namespace chess;

namespace {

// Replaces an argument already set on the line (the common ones below), or adds it.
void put(Line& l, const std::string& name, const Arg& a) {
    for (auto& p : l.args)
        if (p.first == name) {
            p.second = a;
            return;
        }
    l.args.emplace_back(name, a);
}

size_t refutationLength(const Ctx& c) {
    int n = c.gain > 0 ? c.gain : 3;
    n = std::clamp(n, 2, c.b.linePlies + 1);
    // A move that leaves a bigger prize is said with the capture that wins as much back (wonBackAt).
    return size_t(c.followable(std::min<int>(n, int(c.r.size())), true));
}

// A line with the arguments every explanation may use: the move, the better move, the reply, the
// refutation line, the points lost after exactly that line (for good; a detector whose lines say
// "{pts}" makes sure it is at least 1), the evaluation after the move.
Beat line(const Ctx& c, const std::string& key, Look look = Look::Target) {
    Beat b = sayBeat(key, look, c.ply);
    Line& l = b.line;
    const size_t shown = refutationLength(c);
    l.with("move", Arg::ofMove(c.playedSan, c.playedUci));
    if (!c.best.empty()) l.with("best", moveArg(c.best[0]));
    if (!c.r.empty()) l.with("reply", moveArg(c.r[0]));
    l.with("line", Arg::ofMoves(sanLine(c.r, 0, shown)));
    l.with("pts", Arg::ofNumber(c.lossAfter(int(shown))));
    l.with("eval", evalArg(c.lp->score));
    return b;
}
Beat bandLine(const Ctx& c, const std::string& family, Look look = Look::Target) {
    return line(c, bandKey(family, c.level), look);
}

// The anchor of the coach's refutation move in a line: "my" (the piece) at levels 1-2, where the
// lines name pieces; "reply" (the move) from level 3.
const char* replyAnchor(const Ctx& c) { return c.level <= 2 ? "my" : "reply"; }

// Where a human piece of the table (p1) stood before the move under review.
Square squareOnP0(const Ctx& c, Square onP1) { return onP1 == c.played.to ? c.played.from : onP1; }

// Motifs 4-9 below: "R wins material with R[0]". A motif pays off right after the threat, the
// escape and the capture (3 plies, 5 with an intermediate check or a pawn that piles on), whatever
// the band's lookahead: the motif itself stands on the board after R[0] (that is what the lines
// show), and the PV must capture one of its targets within motifReach() plies (index 4): later, the
// loss has other causes too. A trapped piece may take a little longer to collect.
int motifReach(const Ctx&) { return 4; }
int trapReach(const Ctx&) { return 6; }
bool materialLost(const Ctx& c) {
    return c.j.delta >= 10.0 || (c.gain > 0 && c.gain <= c.b.lookahead + 4);
}

// c's castling rights (both wings).
constexpr uint8_t castlingOf(Color c) {
    return c == White ? uint8_t(WhiteKingSide | WhiteQueenSide) : uint8_t(BlackKingSide | BlackQueenSide);
}

// Squares next to a king that are empty (the "escape squares" a mate or a stalemate takes away).
std::vector<Square> kingNeighbours(const Position& p, Square k) {
    std::vector<Square> out;
    for (Square s : squaresOf(attacksOf(King, White, k, 0)))
        if (p.at(s).empty()) out.push_back(s);
    return out;
}

// The ply of r (an even one from 2, up to 'maxPly') where the coach captures the piece standing on
// 'sq'; -1 when it does not.
int coachTakesAt(const Ctx& c, Square sq, int maxPly) {
    for (int i = 2; i < int(c.r.size()) && i <= maxPly; i += 2)
        if (c.r[size_t(i)].mover == c.coach && c.r[size_t(i)].move.to == sq && c.r[size_t(i)].captured != NoPiece) return i;
    return -1;
}

// The position after the first 'plies' plies of r.
Position afterR(const Ctx& c, int plies) {
    Position q = c.p1;
    for (int i = 0; i < plies && i < int(c.r.size()); ++i) q.makeMove(c.r[size_t(i)].move);
    return q;
}

// Where the human's piece that stands on 'sq' just before ply k of r stood after the first 'at'
// plies (its moves in between traced back; NoSquare when it came by castling).
Square squareBefore(const Ctx& c, Square sq, int k, int at) {
    for (int j = k - 1; j >= at && j >= 0; --j) {
        const LineStep& st = c.r[size_t(j)];
        if (st.mover != c.human) continue;
        if (st.move.to == sq) sq = st.move.from;
        else if (st.piece == King && (st.move.flags & (MoveCastleKing | MoveCastleQueen))) {
            // The rook's castling move is not the king's: a rook taken on its new square is not traced.
            const int rank = rankOf(st.move.to);
            const Square rookTo = makeSquare(fileOf(st.move.to) == 6 ? 5 : 3, rank);
            if (sq == rookTo) return NoSquare;
        }
    }
    return sq;
}

// What the capture 'm' (legal in 'now') already won in 'before', where its side was to move too:
// the same piece taking the same piece on the same squares (0 when it was no such move then). A
// capture that stood there all along is no take-back and no punishment of one.
int standingGain(const Position& before, const Position& now, const Move& m) {
    if (before.at(m.from) != now.at(m.from) || before.at(m.to) != now.at(m.to) ||
        before.sideToMove() != now.sideToMove())
        return 0;
    for (const Move& x : before.legalMoves())
        if (x.from == m.from && x.to == m.to && x.promotion == m.promotion) return std::max(0, seePoints(before, x));
    return 0;
}

// Whether the coach answers the human's capture 'm' in q (taking 'took' by exchange) with a capture
// elsewhere that wins back as much, more than that capture already won on the table after the move
// (p1: a piece that stood en prise all along is no answer), and leaves the human down 'need' or
// more whatever the human takes next. 'punish': that capture (the one that takes most).
bool answered(const Ctx& c, const Position& q, const Move& m, int took, int need, Move* punish) {
    Position after = q;
    after.makeMove(m);
    int best = -1;
    for (const Move& r : after.legalMoves()) {
        // The exchange on the capture's own square is counted in what it takes (SEE).
        if (!(r.flags & MoveCapture) || r.to == m.to || (r.promotion != NoPiece && r.promotion != Queen)) continue;
        const int see = seePoints(after, r);
        if (see - standingGain(c.p1, after, r) < took || see <= best) continue;
        Position end = after;
        end.makeMove(r);
        int lost = c.base - materialBalance(end, c.human);
        if (end.hasLegalMove()) lost -= bestCapturePoints(end, c.human);
        if (lost < need) continue;
        best = see;
        if (punish) *punish = r;
    }
    return best >= 0;
}

// The human's loss for good in q (the human to move, where the line shows the take-back never
// comes): every capture worth 'least' points or more must fail ("Qxd2 Qxh1": answered()), a
// smaller one that nothing answers is taken off (a pawn grabbed elsewhere), and the line must show
// as much lost later on ('cap'). A capture counts for what it takes beyond what it took before the
// move (p0): one the human could make all along takes nothing back. 0 when a take-back holds.
// 'take', 'punish': the take-back a player looks at first (the one that takes most, on 'last'
// first) and its answer; invalid when there is no take-back worth 'least'.
int lossIfNoTakeBack(const Ctx& c, const Position& q, int least, int cap, Square last, Move* take, Move* punish) {
    const int board = c.base - materialBalance(q, c.human);
    const auto capture = [](const Move& m) {
        return (m.flags & MoveCapture) && (m.promotion == NoPiece || m.promotion == Queen);
    };
    const auto takes = [&](const Move& m) { return seePoints(q, m) - standingGain(c.p0, q, m); };
    int grab = 0;
    for (const Move& m : q.legalMoves()) {
        if (!capture(m)) continue;
        const int took = takes(m);
        if (took >= 1 && took < least && !answered(c, q, m, took, board, nullptr)) grab = std::max(grab, took);
    }
    const int need = std::min(board - grab, cap);
    if (need <= 0) return 0;
    int shown = -1;
    for (const Move& m : q.legalMoves()) {
        if (!capture(m)) continue;
        const int took = takes(m);
        if (took < std::max(1, least)) continue;
        Move hit;
        if (!answered(c, q, m, took, need, &hit)) return 0;
        const int rank = 2 * took + (m.to == last ? 1 : 0);
        if (rank > shown) {
            shown = rank;
            *take = m;
            *punish = hit;
        }
    }
    return need;
}

// The square a capture of the coach's along 'line' takes on (the pawn's own square en passant).
Square takenOn(const LineStep& st, Color coach) {
    return (st.move.flags & MoveEnPassant) ? Square(st.move.to + (coach == White ? -8 : 8)) : st.move.to;
}

// The points the better move gives up too with the human's piece that stands on 'onP0' before the
// move: the coach takes it within the first ten plies of the best line (the piece followed as it
// moves), and the captures on that square right after it do not win as much back; what the piece
// took on its way is taken off ("Bxg2 Kxg2": a bishop for a pawn gives up 2). 0 when it survives,
// or is only traded.
int bestGivesUp(const Ctx& c, Square onP0) {
    const Piece was = c.p0.at(onP0);
    if (was.empty() || was.color != c.human) return 0;
    Square at = onP0;
    int took = 0;
    for (size_t i = 0; i < c.best.size() && i < 10; ++i) {
        const LineStep& st = c.best[i];
        if (st.mover == c.human) {
            if (st.move.from == at) {
                at = st.move.to;
                took += points(st.captured);
            } else if (st.piece == King && (st.move.flags & (MoveCastleKing | MoveCastleQueen))) {
                // The rook castles with the king.
                const int rank = rankOf(st.move.to);
                const bool king = fileOf(st.move.to) == 6;
                if (at == makeSquare(king ? 7 : 0, rank)) at = makeSquare(king ? 5 : 3, rank);
            }
            continue;
        }
        if (st.captured == NoPiece || takenOn(st, c.coach) != at) continue;
        // The captures on that square after it (an in-between move or two allowed): what the human
        // wins back there (a trade gives up nothing; "Nxe4 Nxe4 Rxe4" still costs the pawn).
        int net = points(st.captured);
        for (size_t j = i + 1, last = i; j < c.best.size() && j <= last + 3; ++j) {
            if (c.best[j].captured == NoPiece || c.best[j].move.to != st.move.to) continue;   // an in-between move
            net += c.best[j].mover == c.human ? -points(c.best[j].captured) : points(c.best[j].captured);
            last = j;
        }
        return std::max(0, std::min(net, points(st.captured)) - took);
    }
    return 0;
}

// What the better move gives up too of the pieces the first k+1 plies of r take (bestGivesUp, each
// piece followed back to where it stood before the move), at most what its line loses for good;
// 'atK': the piece taken at ply k is one, and that line loses as much.
int givenUpAlong(const Ctx& c, int k, bool& atK) {
    int total = 0;
    atK = false;
    // The most the better move's line is down for good within its first ten plies (a loss that
    // still holds four plies later): a piece it gives up while it wins as much back elsewhere ("fxe5"
    // takes a knight for the bishop) is traded, not given up.
    // A capture of the human's that the coach takes back on the next ply is read with that
    // recapture (the exchange whole: "dxe5 Qxe5" gives up the pawn, it wins no bishop back).
    const auto lossAt = [&](size_t j) {
        if (j < c.best.size() && c.best[j - 1].mover == c.human && c.best[j - 1].captured != NoPiece &&
            c.best[j].mover != c.human && c.best[j].captured != NoPiece && c.best[j].move.to == c.best[j - 1].move.to)
            ++j;
        return c.base - c.best[j - 1].balance;
    };
    int endLoss = 0;
    for (size_t i = 1; i <= std::min<size_t>(c.best.size(), 10); ++i) {
        int l = lossAt(i);
        for (size_t j = i + 1; j <= std::min(c.best.size(), i + 4); ++j) l = std::min(l, lossAt(j));
        endLoss = std::max(endLoss, l);
    }
    if (endLoss <= 0) return 0;
    for (int j = 0; j <= k && j < int(c.r.size()); ++j) {
        const LineStep& st = c.r[size_t(j)];
        if (st.mover != c.coach || st.captured == NoPiece) continue;
        const Square onP1 = squareBefore(c, takenOn(st, c.coach), j, 0);
        if (onP1 == NoSquare) continue;
        const int g = bestGivesUp(c, onP1 == c.played.to ? c.played.from : onP1);
        total += g;
        if (j == k && g > 0 && endLoss >= g) atK = true;
    }
    return std::min(total, endLoss);
}

// The sequence of a loss: the coach's capture at ply k of r, with the human's recapture when one
// comes next; what the human has lost then (from p0), where the human, to move, cannot take back
// enough at once (the board's best capture counted, unless the line shows it never comes) and is
// still down by as much four plies later along the line; more than before that capture (a trade on
// the way is not where a loss comes from); and whether that is the reason the move is bad
// (Ctx::lossExplains). At most a few plies
// past the band's reach (the lines say the sequence, the table shows it). threatPlies: the plies of
// r until the problem stands on the board. The captured piece is the target, on its square once the
// problem stands; it must still be the same piece there. targetOnly: the reason names that target
// only (a motif), not the whole line.
bool settleLoss(const Ctx& c, int k, int threatPlies, Explanation& ex, bool targetOnly = false) {
    const int n = int(c.r.size());
    if (k < 0 || k >= n || c.r[size_t(k)].mover != c.coach || c.r[size_t(k)].captured == NoPiece) return false;
    const bool pawn = c.r[size_t(k)].captured == Pawn;
    // A piece the better move gives up too is no reason the move is bad; one taken on the way would
    // be counted in what the line says is lost: the move must then cost a piece or more on its own,
    // twice what the better move gives up as well.
    bool atK = false;
    const int givenUp = givenUpAlong(c, k, atK);
    if (atK) return false;
    const int cap = std::max(c.b.demoPlies, c.b.linePlies) + 2;
    int plies = 0, lost = 0;
    // The human's recapture on the next ply is part of the sequence (shown and said with it).
    const bool retake = k + 1 < n && c.r[size_t(k + 1)].mover == c.human && c.r[size_t(k + 1)].captured != NoPiece &&
                        c.r[size_t(k + 1)].move.to == c.r[size_t(k)].move.to && k + 2 <= cap;
    // What the human had lost already: the capture at k (with the recapture) must add to it, else it
    // is a trade on the way and not where the loss comes from. A trade the human starts on that very
    // square just before is the same ("you take my rook, I take yours"): what was lost counts from
    // before it (not when the human's capture there takes back one of the coach's).
    const auto capturesOn = [&](int i, Color who, Square sq) {
        return i >= 0 && c.r[size_t(i)].mover == who && c.r[size_t(i)].captured != NoPiece && c.r[size_t(i)].move.to == sq;
    };
    const Square at = c.r[size_t(k)].move.to;
    // A reason that names only this capture's target, when the coach's reply itself takes as much
    // at once on another square (the human's capture right after it counted): that capture is the
    // story ("cxd3" took a pawn, and forks two pieces: the pawn on b3 the bishop behind it wins next
    // is not what the move costs).
    if (targetOnly && k > 0 && c.r[0].captured != NoPiece && c.r[0].move.to != at) {
        const size_t once = n > 1 && c.r[1].mover == c.human && c.r[1].captured != NoPiece ? 1 : 0;
        const int atOnce = c.base - c.r[once].balance;
        if (atOnce >= 1 && atOnce >= points(c.r[size_t(k)].captured)) return false;
    }
    const bool tradeBack = capturesOn(k - 1, c.human, at) && !capturesOn(k - 2, c.coach, at);
    const int already = k > 0 ? c.lossAfter(tradeBack ? k - 1 : k) : 0;
    // Whether material changed hands before ply k on another square than this capture's.
    bool earlier = false;
    for (int i = 0; i < k; ++i)
        if (c.r[size_t(i)].captured != NoPiece && c.r[size_t(i)].move.to != at) earlier = true;
    Move take, punish;
    for (int p = retake ? k + 2 : k + 1; p <= std::min({n, k + 2, cap}) && plies == 0; ++p) {
        const LineStep& st = c.r[size_t(p - 1)];
        if (st.captured == NoPiece || st.mate) continue;
        // Never a sequence that stops after a move that leaves a bigger prize and before the capture
        // that wins as much back ("I take your knight with check", the queen still en prise).
        if (c.followable(p, false) < p) continue;
        const Position q = afterR(c, p);
        const int board = c.base - st.balance;
        int l = board;
        if (q.sideToMove() == c.human) l -= bestCapturePoints(q, c.human);
        // Still lost a few plies later (an in-between check and a recapture are allowed for). What
        // the human could take back at once is taken off, unless that loss would not explain the move
        // and the line shows the take-back never comes (the human's next move takes nothing, and the
        // loss holds): then what the board shows.
        const int held = p < n ? c.lossAfter(std::min(n, p + 4)) : l;
        // Only when the human's next move takes nothing (a capture elsewhere, taken back at once, would
        // leave the take-back the board shows unexplained: "after Nxf3, 3 points lost", gxf3 there),
        // and when every such take-back loses as much at once to a capture (lossIfNoTakeBack: not a
        // gambit the line prefers, nor a refutation the table cannot show); the take-back is then
        // said with the loss.
        Move tk, pn;
        bool fails = false;
        if (held > l && p < n && c.r[size_t(p)].mover == c.human && c.r[size_t(p)].captured == NoPiece &&
            !(l > already && c.lossExplains(l, pawn))) {
            // Four plies later, the board still shows as much lost, whatever the human could take then.
            const int at = std::min(n, p + 4);
            const Position later = afterR(c, at);
            int still = c.base - c.r[size_t(at - 1)].balance;
            if (later.sideToMove() == c.human) still -= bestCapturePoints(later, c.human);
            const int raised = lossIfNoTakeBack(c, q, std::min(2, points(c.r[size_t(k)].captured)),
                                                std::min(held, still), st.move.to, &tk, &pn);
            if (raised > l) {
                l = raised;
                fails = tk.valid() && pn.valid();
            }
        }
        // What the human wins back only for a while (a pawn taken in the middle of an exchange the
        // line completes within four more plies) wins nothing back: the loss found again then
        // counts, up to what this capture leaves.
        int heldBack = held;
        for (int j = p + 5; j <= std::min(n, p + 8); ++j) heldBack = std::max(heldBack, std::min(l, c.lossAfter(j)));
        if (l <= already || !c.lossExplains(l, pawn) || !c.lossExplains(heldBack, pawn)) continue;
        // When the better move loses material too, what this line costs beyond it must hold along
        // the move's own deeper line as well: the refutation may be a shallower search's, with a
        // defence that loses more (Kc7 e6+ Qd6 loses the rook; e6+ Rf4, the deeper line's, less).
        if (c.bestLoss > 0 && c.playedLoss >= 0 && !c.lossExplains(std::min(l, c.playedLoss), pawn)) continue;
        // A reason that names only this capture's target: what was lost before it on other squares
        // is the reason when it explains the move by itself and this capture adds no more ("Nxb3"
        // took the queen: the pawn a discovered attack wins next is not it).
        if (targetOnly && earlier && already > c.bestLoss && already >= l - already && c.lossExplains(already)) continue;
        if (givenUp > 0 && (l - givenUp < std::max(3, 2 * givenUp) || !c.lossExplains(l - givenUp, pawn))) continue;
        plies = p;
        lost = l;
        if (fails) {
            take = tk;
            punish = pn;
        }
    }
    if (plies == 0) return false;
    ex.fullPlies = plies;
    ex.lost = lost;
    // The take-back the table shows there fails: said with the loss (the line stops before it).
    ex.retake = take;
    ex.punish = punish;
    ex.threatPlies = std::min(threatPlies, k);
    ex.targets.clear();
    const Square to = (c.r[size_t(k)].move.flags & MoveEnPassant)
                          ? Square(c.r[size_t(k)].move.to + (c.coach == White ? -8 : 8))
                          : c.r[size_t(k)].move.to;
    const Square t = squareBefore(c, to, k, ex.threatPlies);
    if (t != NoSquare) {
        const Piece pc = afterR(c, ex.threatPlies).at(t);
        if (pc.color == c.human && pc.type == c.r[size_t(k)].captured) ex.targets.push_back(t);
    }
    return true;
}

// The cause line's {line} and {pts}: the whole sequence and what it costs.
void putLoss(Beat& b, const Ctx& c, const Explanation& ex) {
    put(b.line, "line", Arg::ofMoves(sanLine(c.r, 0, size_t(ex.fullPlies))));
    put(b.line, "pts", Arg::ofNumber(ex.lost));
}

std::vector<Square> byValue(const Position& p, uint64_t set) {
    std::vector<Square> v = squaresOf(set);
    std::stable_sort(v.begin(), v.end(), [&](Square a, Square b) {
        const PieceType ta = p.at(a).type, tb = p.at(b).type;
        if ((ta == King) != (tb == King)) return ta == King;
        return kPieceValueCp[ta] > kPieceValueCp[tb];
    });
    return v;
}

// ---- 1. Mate allowed ----
bool mateAllowed(const Ctx& c, Explanation& out) {
    if (c.j.mate != MateChange::Created) return false;
    const ai::Score& s = c.lp->score;
    const int n = s.matedNow ? 0 : -s.mate;
    if (n <= 0) return false;
    const int plies = 2 * n - 1;
    const bool lineKnown = int(c.r.size()) >= plies && c.r[size_t(plies - 1)].mate;
    Explanation ex;
    ex.type = ExType::MateAllowed;
    ex.mate = true;
    // Levels 1-2: a mate the level can follow (a mate in two at level 1, in three at level 2), shown.
    ex.concrete = plies <= std::max(c.b.lookahead, c.b.mateLinePlies);
    if (c.level <= 2 && (!ex.concrete || !lineKnown)) return false;   // too deep for the band: not this cause
    static const int kMateLimit[6] = {1, 2, 2, 3, 3, 4};
    ex.offer = n <= kMateLimit[c.level - 1];
    if (lineKnown) {
        ex.fullPlies = plies;
        ex.threatPlies = plies - 1;
    }
    const Square k = c.p1.kingSquare(c.human);
    // Levels 3, 4 and 6 show the mate ("{line} is mate"): when the engine's line stops before it, the
    // line of level 5 says it without moves. Level 1 says the mate in one with the piece ("my queen
    // goes to h7"), a longer one as level 2 does.
    const bool one = c.level == 1 && n == 1;
    // A line too long to follow in words is not said (the level-5 line says the mate without it).
    const bool sayLine = lineKnown && plies <= std::max(c.b.linePlies, c.b.mateLinePlies) + 2;
    Beat b = !sayLine && c.level >= 3 ? line(c, "ex.mate_allowed.b5")
             : c.level == 1 && !one   ? line(c, "ex.mate_allowed.b2")
                                      : bandLine(c, "ex.mate_allowed");
    put(b.line, "m", Arg::ofNumber(n));
    put(b.line, "your", pieceArg(c.p1, k, c.human));
    if (lineKnown) put(b.line, "line", Arg::ofMoves(sanLine(c.r, 0, size_t(plies))));
    const LineStep& first = c.r.empty() ? LineStep{} : c.r[0];
    if (one) {
        put(b.line, "my", pieceArg(c.p1, first.move.from, c.human));
        put(b.line, "sq", Arg::ofSquare(first.move.to));
        traceMove(b, first.piece, first.move.from, first.move.to, "my");
    } else if (c.level <= 2) {
        pointPiece(b, k, "your", true);
    } else if (c.level != 5 && sayLine) {
        traceMove(b, first.piece, first.move.from, first.move.to, "line");
    }
    ex.cause.push_back(b);
    // Shown in full (levels 1-3, planDemo): said on the mate itself.
    const bool full = lineKnown && c.b.fullDemo && plies <= c.b.mateLinePlies;
    if (full && c.level <= 2) {
        // The king has no escape square (where it stands at the end of the line).
        const Position q = afterR(c, plies);
        const Square kq = q.kingSquare(c.human);
        Beat t = line(c, "ex.mate_allowed.tail.b1", Look::Target);
        put(t.line, "your", pieceArg(q, kq, c.human));
        pointPiece(t, kq, "your", true);
        for (Square e : kingNeighbours(q, kq)) markSquare(t, e, "your");
        ex.tail.push_back(t);
    }
    // Levels 2-4 may name the pattern of the final position, once it is shown or said.
    if (lineKnown && c.level >= 2 && c.level <= 4 && (full || (sayLine && c.level >= 3))) {
        const MatePattern mp = classifyMate(afterR(c, plies));
        const char* name = mp == MatePattern::BackRank    ? "name.pattern.back_rank"
                           : mp == MatePattern::Smothered ? "name.pattern.smothered"
                           : mp == MatePattern::Support   ? "name.pattern.support"
                           : mp == MatePattern::Ladder    ? "name.pattern.ladder"
                           : mp == MatePattern::Epaulette ? "name.pattern.epaulette"
                                                          : nullptr;
        if (name) {
            // Named on the mate itself when the demonstration plays the whole line.
            Beat t = line(c, "ex.mate_allowed.pattern", Look::Board);
            put(t.line, "text", Arg::ofText(name));
            if (full) ex.tail.push_back(t);
            else ex.cause.push_back(t);
        }
    }
    if (c.level == 3) ex.tip.push_back(line(c, "ex.mate_allowed.tip.b3", Look::Player));
    out = ex;
    return true;
}

// ---- 2. Mate missed ----
bool mateMissed(const Ctx& c, Explanation& out) {
    const bool lost = c.j.mate == MateChange::Lost;
    const bool delayed = c.j.mate == MateChange::Delayed && c.level >= 5;
    if (!lost && !delayed) return false;
    const int n = c.l1->score.mate;
    if (n <= 0 || n > c.b.missedMateMax || c.best.empty()) return false;
    if (delayed && std::abs(c.lp->score.mate) < n + 2) return false;
    if (n == 1 && !mateInOne(c.p0)) return false;   // the engine-free check of the claim
    Explanation ex;
    ex.type = ExType::MateMissed;
    ex.missed = true;
    ex.concrete = true;
    ex.includesBest = true;
    // The offer: levels 1-2 try again to find it; from level 3, only when missing it costs the win.
    ex.offer = lost && (c.level <= 2 || c.j.cls != MoveClass::Inaccuracy);
    // Most lines from level 3 show the mate: when the engine's best line stops before it, the lines
    // of level 2 say it with the first move only.
    const size_t plies = size_t(2 * n - 1);
    const bool lineKnown = c.best.size() >= plies && c.best[plies - 1].mate;
    Beat b = delayed     ? bandLine(c, "ex.mate_delayed")
             : lineKnown ? bandLine(c, "ex.mate_missed")
                         : line(c, c.level == 1 ? "ex.mate_missed.b1" : "ex.mate_missed.b2");
    put(b.line, "m", Arg::ofNumber(n));
    if (lineKnown) put(b.line, "line", Arg::ofMoves(sanLine(c.best, 0, plies)));
    const Square ck = c.p1.kingSquare(c.coach);
    put(b.line, "my", pieceArg(c.p1, ck, c.human));
    const LineStep& m = c.best[0];
    traceMove(b, m.piece, m.move.from, m.move.to, "best");
    if (c.level == 1) pointPiece(b, ck, "my");
    ex.cause.push_back(b);
    out = ex;
    return true;
}

// ---- 3. Stalemate when winning ----
bool stalemate(const Ctx& c, Explanation& out) {
    Explanation ex;
    ex.type = ExType::Stalemate;
    ex.concrete = true;
    ex.offer = true;
    const Square ck = c.p1.kingSquare(c.coach);
    if (c.f.stalemate && c.j.wBest >= 80.0) {
        Beat b = bandLine(c, "ex.stalemate");
        put(b.line, "my", pieceArg(c.p1, ck, c.human));
        pointPiece(b, ck, "my", true);
        for (Square s : kingNeighbours(c.p1, ck)) markSquare(b, s, "my");
        ex.includesBest = c.level >= 4;
        ex.cause.push_back(b);
        if (c.level <= 2) ex.tip.push_back(line(c, "ex.stalemate.tip.b1", Look::Player));
        out = ex;
        return true;
    }
    if (c.j.wBest >= 85.0 && std::abs(c.j.wPlayed - 50.0) < 5.0) {
        // The lines say the coach's king is stalemated after the human takes what the coach gives.
        bool given = false;
        for (int i = 0; i < int(c.r.size()) && i < c.b.lookahead + 2; ++i) {
            const LineStep& st = c.r[size_t(i)];
            if (st.mover == c.human && st.captured != NoPiece) given = true;
            if (!st.stalemate || st.mover != c.human || !given) continue;
            Beat b = bandLine(c, "ex.stalemate_trick");
            put(b.line, "my", pieceArg(c.p1, ck, c.human));
            put(b.line, "line", Arg::ofMoves(sanLine(c.r, 0, size_t(i + 1))));
            pointPiece(b, ck, "my");
            ex.cause.push_back(b);
            ex.fullPlies = i + 1;   // shown in full at levels 1-3 (planDemo), said from level 4
            out = ex;
            return true;
        }
    }
    return false;
}

// Whether the human's piece on 'sq' stays there from ply 'from' of r until ply 'to' (excluded): a
// capture there takes that very piece, not another one that came to its square.
bool stays(const Ctx& c, Square sq, int from, int to) {
    for (int j = from; j < to && j < int(c.r.size()); ++j)
        if (c.r[size_t(j)].mover == c.human && c.r[size_t(j)].move.from == sq) return false;
    return true;
}

// The first ply (from 2, within the motif's reach) where the coach takes one of 'targets' (squares
// after r[0]), that very piece; -1 when none.
int firstTaken(const Ctx& c, uint64_t targets, Square* which = nullptr) {
    int best = -1;
    for (Square t : squaresOf(targets)) {
        const int at = coachTakesAt(c, t, motifReach(c));
        if (at < 0 || !stays(c, t, 1, at)) continue;
        if (best < 0 || at < best) {
            best = at;
            if (which) *which = t;
        }
    }
    return best;
}

// ---- 4. Fork ----
bool fork(const Ctx& c, Explanation& out) {
    if (c.r.empty() || c.r[0].mover != c.coach) return false;
    const LineStep& r0 = c.r[0];
    Position p2 = c.p1;
    p2.makeMove(r0.move);
    const uint64_t targets = forkTargets(p2, r0.move.to);
    if (squareCount(targets) < 2) return false;
    // The line takes one of the targets, and that loss is the reason (nothing the human took
    // meanwhile makes up for it).
    Square taken = NoSquare;
    const int takenAt = firstTaken(c, targets, &taken);
    Explanation ex;
    if (!settleLoss(c, takenAt, 1, ex, true)) return false;
    ex.type = ExType::Fork;
    ex.concrete = true;
    ex.includesBest = c.level == 6;
    ex.hintSquare = squareOnP0(c, taken);
    // The two targets named: by value, the one that falls among them.
    std::vector<Square> ts = byValue(p2, targets);
    if (ts[0] != taken && ts[1] != taken) ts[1] = taken;
    const bool kingFork = p2.at(ts[0]).type == King;
    ex.targets = kingFork ? std::vector<Square>{taken} : std::vector<Square>{ts[0], ts[1]};
    Beat b = bandLine(c, "ex.fork");
    putLoss(b, c, ex);
    put(b.line, "my", pieceArg(c.p1, r0.move.from, c.human));
    put(b.line, "sq", Arg::ofSquare(r0.move.to));
    put(b.line, "t1", pieceArg(c.p1, ts[0], c.human));
    put(b.line, "t2", pieceArg(c.p1, ts[1], c.human));
    traceMove(b, r0.piece, r0.move.from, r0.move.to, replyAnchor(c));
    if (c.level <= 5) {
        pointPiece(b, ts[0], "t1");
        pointPiece(b, ts[1], "t2");
    }
    ex.cause.push_back(b);
    // Once the fork stands: "your king must get out of check, so I take your rook"; level 1 also
    // "you can only save one of them" (in the middle of the demonstration, or where it stops).
    if (kingFork) {
        Beat t = line(c, "ex.fork.tail_king");
        put(t.line, "your", pieceArg(p2, ts[0], c.human));
        put(t.line, "t1", pieceArg(p2, taken, c.human));
        pointPiece(t, ts[0], "your");
        pointPiece(t, taken, "t1", true);
        ex.threatTail.push_back(t);
    } else if (c.level == 1) {
        Beat t = line(c, "ex.fork.tail.b1");
        put(t.line, "t1", pieceArg(p2, ts[0], c.human));
        put(t.line, "t2", pieceArg(p2, ts[1], c.human));
        pointPiece(t, ts[0], "t1");
        pointPiece(t, ts[1], "t2");
        ex.threatTail.push_back(t);
    }
    if (c.level == 3) {
        Beat m = line(c, "ex.fork.more.b3");
        put(m.line, "sq", Arg::ofSquare(r0.move.to));
        put(m.line, "my", pieceArg(c.p1, r0.move.from, c.human));
        pointSquare(m, r0.move.to, "sq");
        pointPiece(m, r0.move.from, "my");
        ex.cause.push_back(m);
    }
    if (c.level <= 2 && r0.piece == Knight) ex.tip.push_back(line(c, "ex.fork.tip.knight", Look::Player));
    out = ex;
    return true;
}

// ---- 5. Discovered attack ----
bool discovered(const Ctx& c, Explanation& out) {
    if (c.r.empty() || c.r[0].mover != c.coach) return false;
    const LineStep& r0 = c.r[0];
    const std::vector<Discovery> all = discoveredAttacks(c.p1, r0.move);
    // A target the line takes, that very piece (the lines say it falls to the discovered attack).
    for (const Discovery& d : all) {
        if (d.check) continue;
        const int at = coachTakesAt(c, d.target, motifReach(c));
        if (at < 0 || !stays(c, d.target, 1, at)) continue;
        Explanation ex;
        if (!settleLoss(c, at, 1, ex, true)) continue;
        ex.type = ExType::Discovered;
        ex.concrete = true;
        ex.hintSquare = squareOnP0(c, d.target);
        Beat b = bandLine(c, "ex.discovered");
        putLoss(b, c, ex);
        put(b.line, "my", pieceArg(c.p1, r0.move.from, c.human));
        put(b.line, "my2", pieceArg(c.p1, d.slider, c.human));
        put(b.line, "t1", pieceArg(c.p1, d.target, c.human));
        if (c.level <= 2) {
            pointPiece(b, r0.move.from, "my");
            traceMove(b, c.p1.at(d.slider).type, d.slider, d.target, "my2");
            pointPiece(b, d.target, "t1");
        } else {
            traceMove(b, r0.piece, r0.move.from, r0.move.to, "reply");
            if (c.level != 5) pointPiece(b, d.target, "t1");
            markArrow(b, c.p1.at(d.slider).type, d.slider, d.target, "reply");
        }
        ex.cause.push_back(b);
        out = ex;
        return true;
    }
    // A discovered check, when the line then wins material (levels 1-3 and 5: the lines of levels 4
    // and 6 say the target itself falls).
    if (c.level == 4 || c.level == 6) return false;
    for (const Discovery& d : all) {
        if (!d.check) continue;
        for (int k = 1; k < int(c.r.size()) && k <= motifReach(c); ++k) {
            Explanation ex;
            if (!settleLoss(c, k, 1, ex, true)) continue;
            ex.type = ExType::Discovered;
            ex.concrete = true;
            ex.hintSquare = c.p1.kingSquare(c.human);
            Beat b = bandLine(c, "ex.discovered");
            putLoss(b, c, ex);
            put(b.line, "my", pieceArg(c.p1, r0.move.from, c.human));
            put(b.line, "my2", pieceArg(c.p1, d.slider, c.human));
            put(b.line, "t1", pieceArg(c.p1, d.target, c.human));
            if (c.level <= 2) {
                pointPiece(b, r0.move.from, "my");
                traceMove(b, c.p1.at(d.slider).type, d.slider, d.target, "my2");
                pointPiece(b, d.target, "t1");
            } else {
                traceMove(b, r0.piece, r0.move.from, r0.move.to, "reply");
                markArrow(b, c.p1.at(d.slider).type, d.slider, d.target, "reply");
            }
            ex.cause.push_back(b);
            out = ex;
            return true;
        }
    }
    return false;
}

// ---- 6. Skewer ----
bool skewer(const Ctx& c, Explanation& out) {
    if (c.r.empty() || c.r[0].mover != c.coach) return false;
    const LineStep& r0 = c.r[0];
    Position p2 = c.p1;
    p2.makeMove(r0.move);
    for (const Skewer& sk : skewers(p2, c.human)) {
        if (sk.attacker != r0.move.to) continue;
        const int at = coachTakesAt(c, sk.behind, motifReach(c));
        if (at < 0 || !stays(c, sk.behind, 1, at)) continue;
        Explanation ex;
        if (!settleLoss(c, at, 1, ex, true)) continue;
        ex.type = ExType::Skewer;
        ex.concrete = true;
        ex.hintSquare = squareOnP0(c, sk.behind);
        Beat b = bandLine(c, "ex.skewer");
        putLoss(b, c, ex);
        put(b.line, "my", pieceArg(c.p1, r0.move.from, c.human));
        put(b.line, "t1", pieceArg(c.p1, sk.front, c.human));
        put(b.line, "t2", pieceArg(c.p1, sk.behind, c.human));
        traceMove(b, r0.piece, r0.move.from, r0.move.to, replyAnchor(c));
        if (c.level <= 5) {
            pointPiece(b, sk.front, "t1");
            pointPiece(b, sk.behind, "t2");
        }
        ex.cause.push_back(b);
        out = ex;
        return true;
    }
    return false;
}

// ---- 7. Pin ----
bool pin(const Ctx& c, Explanation& out) {
    if (c.r.empty() || c.r[0].mover != c.coach) return false;
    const LineStep& r0 = c.r[0];
    Position p2 = c.p1;
    p2.makeMove(r0.move);
    const std::vector<Pin> before = pins(c.p1, c.human);
    const int reach = motifReach(c);
    // (a) The reply pins a piece that is then won; (c) the moved piece walked into a pin.
    for (const Pin& pn : pins(p2, c.human)) {
        const bool fresh = pn.pinner == r0.move.to &&
                           std::none_of(before.begin(), before.end(), [&](const Pin& o) { return o.pinned == pn.pinned; });
        const bool walkedIn = pn.pinned == c.played.to;
        if (!fresh && !walkedIn) continue;
        // A pin the move walked into, the pinner already in place: the lines of levels 1 and 4-6
        // say the reply pins ("{my} can go to {sq}", "{reply} pins").
        if (!fresh && pn.pinner != r0.move.to && (c.level == 1 || c.level >= 4)) continue;
        const int at = coachTakesAt(c, pn.pinned, reach);
        if (at < 0 || !stays(c, pn.pinned, 1, at)) continue;
        Explanation ex;
        if (!settleLoss(c, at, 1, ex, true)) continue;
        ex.type = ExType::Pin;
        ex.concrete = true;
        ex.hintSquare = squareOnP0(c, pn.pinned);
        Beat b = bandLine(c, "ex.pin");
        putLoss(b, c, ex);
        const Square pinnerOnP1 = pn.pinner == r0.move.to ? r0.move.from : pn.pinner;
        put(b.line, "my", pieceArg(c.p1, pinnerOnP1, c.human));
        put(b.line, "sq", Arg::ofSquare(pn.pinner));
        put(b.line, "your", pieceArg(c.p1, pn.pinned, c.human));
        put(b.line, "t1", pieceArg(c.p1, pn.behind, c.human));
        if (c.level == 1) traceMove(b, c.p1.at(pinnerOnP1).type, pinnerOnP1, pn.pinner, "my");
        if (c.level <= 3) {
            pointPiece(b, pn.pinned, "your", true);
            pointPiece(b, pn.behind, "t1");
        } else {
            traceMove(b, r0.piece, r0.move.from, r0.move.to, "reply");
        }
        markArrow(b, Queen, pn.pinner, pn.behind, c.level <= 3 ? "your" : "reply");
        ex.cause.push_back(b);
        out = ex;
        return true;
    }
    // (b) A pinned piece is a bad defender: the only defender of the captured piece is pinned, and
    // still cannot take back after the reply (a pinner that captured off the line freed it).
    if (r0.captured != NoPiece) {
        const Square s = r0.move.to;
        const uint64_t defenders = c.p1.attackersTo(s, c.human);
        const uint64_t pinnedSet = c.p1.pinned(c.human);
        bool canRetake = false;
        for (Square d : squaresOf(defenders))
            if (p2.findLegal(d, s, Queen).valid()) canRetake = true;
        if (defenders && (defenders & ~pinnedSet) == 0 && !canRetake) {
            const Square d = squaresOf(defenders)[0];
            for (const Pin& pn : before) {
                if (pn.pinned != d) continue;
                Explanation ex;
                if (!settleLoss(c, 0, 0, ex)) return false;
                ex.type = ExType::Pin;
                ex.concrete = true;
                ex.hintSquare = squareOnP0(c, s);
                Beat b = bandLine(c, "ex.pin_defender");
                putLoss(b, c, ex);
                put(b.line, "your", pieceArg(c.p1, d, c.human));
                put(b.line, "sq", Arg::ofSquare(s));
                put(b.line, "t1", pieceArg(c.p1, pn.behind, c.human));
                put(b.line, "my", pieceArg(c.p1, pn.pinner, c.human));
                pointPiece(b, d, "your", true);
                pointSquare(b, s, "sq");
                if (c.level <= 3) pointPiece(b, pn.behind, "t1");
                markArrow(b, Queen, pn.pinner, pn.behind, "your");
                ex.cause.push_back(b);
                out = ex;
                return true;
            }
        }
    }
    return false;
}

// ---- 8. Trapped piece ----
// The ply of r where the coach takes the human's piece that stands on 'sq' after r[0], followed as it
// moves; -1 when the line does not take it within 'reach' plies.
int takenLater(const Ctx& c, Square sq, int reach) {
    for (int i = 1; i < int(c.r.size()) && i <= reach; ++i) {
        const LineStep& st = c.r[size_t(i)];
        if (st.mover == c.human && st.move.from == sq) sq = st.move.to;
        else if (st.mover == c.coach && st.move.to == sq && st.captured != NoPiece) return i;
    }
    return -1;
}

bool trapped(const Ctx& c, Explanation& out) {
    if (c.r.empty() || c.r[0].mover != c.coach) return false;
    Position p2 = c.p1;
    p2.makeMove(c.r[0].move);
    const int reach = trapReach(c);
    Square x = NoSquare;
    Explanation ex;
    for (Square s : squaresOf(p2.pieces(c.human) & ~(p2.pieces(Pawn) | p2.pieces(King))))
        if (isTrapped(p2, s) && settleLoss(c, takenLater(c, s, reach), 1, ex, true)) {
            x = s;
            break;
        }
    // A special pattern: a bishop grabs a rim pawn (Bxa7? b6, Bxh7? g6) and a pawn push
    // closes its retreat. Nothing attacks it yet (the king comes later), so isTrapped() does not see it.
    const bool rimGrab = c.f.piece == Bishop && c.f.captured == Pawn &&
                         (fileOf(c.played.to) == 0 || fileOf(c.played.to) == 7) && c.r[0].piece == Pawn;
    if (x == NoSquare && rimGrab && p2.at(c.played.to).type == Bishop) {
        bool boxed = !p2.inCheck();
        if (boxed)
            for (const Move& m : p2.legalMovesFrom(c.played.to))
                if (seePoints(p2, m) >= 0) boxed = false;
        if (boxed && settleLoss(c, takenLater(c, c.played.to, reach), 1, ex, true)) x = c.played.to;
    }
    if (x == NoSquare) return false;
    ex.type = ExType::Trapped;
    ex.concrete = true;
    ex.hintSquare = squareOnP0(c, x);
    ex.targets = {x};
    Beat b = bandLine(c, "ex.trapped");
    putLoss(b, c, ex);
    put(b.line, "your", pieceArg(c.p1, x, c.human));
    pointPiece(b, x, "your", true);
    for (const Move& m : p2.legalMovesFrom(x)) markSquare(b, m.to, "your");
    ex.cause.push_back(b);
    if (c.level == 1) ex.tip.push_back(line(c, "ex.trapped.tip.b1", Look::Player));
    out = ex;
    return true;
}

// ---- 9. Back rank, material won through it ----
bool backRank(const Ctx& c, Explanation& out) {
    if (!backRankWeak(c.p1, c.human)) return false;
    const int home = c.human == White ? 0 : 7;
    // Level 1 says the king is locked in by its pawns (and the tip, to move one of them).
    if (c.level == 1) {
        const Square k = c.p1.kingSquare(c.human);
        const uint64_t front = attacksOf(King, c.human, k, 0) & ~(0xFFULL << (8 * home));
        if ((front & c.p1.pieces(c.human, Pawn)) != front) return false;
    }
    for (int i = 0; i < int(c.r.size()) && i < c.b.lookahead; i += 2) {
        const LineStep& st = c.r[size_t(i)];
        if (st.mover != c.coach || !(st.piece == Rook || st.piece == Queen) || rankOf(st.move.to) != home || !st.check)
            continue;
        // The lines say the check wins material: the line wins it, for good, after the check.
        Explanation ex;
        bool won = false;
        int at = i;
        for (; at < int(c.r.size()) && at <= i + 4 && !won; ++at) won = settleLoss(c, at, i + 1, ex, true);
        if (!won) return false;
        // The check that takes a loose piece and wins no more: that piece was hanging (section 10),
        // whatever the back rank.
        if (at - 1 == i && st.captured != NoPiece && isUndefended(afterR(c, i), st.move.to)) return false;
        const Square k = c.p1.kingSquare(c.human);
        ex.type = ExType::BackRank;
        ex.concrete = true;
        Beat b = bandLine(c, "ex.back_rank");
        putLoss(b, c, ex);
        put(b.line, "your", pieceArg(c.p1, k, c.human));
        put(b.line, "sq", Arg::ofSquare(st.move.to));
        put(b.line, "my", i == 0 ? pieceArg(c.p1, st.move.from, c.human) : Arg::ofPiece(st.piece, c.coach, false));
        pointPiece(b, k, "your");
        if (i == 0) traceMove(b, st.piece, st.move.from, st.move.to, c.level <= 2 ? "my" : "line");
        else markSquare(b, st.move.to, "your");
        ex.cause.push_back(b);
        if (c.level == 1) ex.tip.push_back(line(c, "ex.back_rank.tip.b1", Look::Player));
        out = ex;
        return true;
    }
    return false;
}

// ---- 10. Hanging piece, removal of the guard, a threat not seen ----
// Every level's lines say the piece has no protector ("loose", "for free", "nothing recaptures"):
// a piece defended, even badly, is lost in an exchange (section 11).
bool hanging(const Ctx& c, Explanation& out) {
    if (c.r.empty() || c.r[0].mover != c.coach || c.r[0].captured == NoPiece) return false;
    const LineStep& r0 = c.r[0];
    const Square s = r0.move.to;
    const PieceType x = r0.captured;
    if (x == Pawn && c.level <= 2) return false;
    // The piece that has just captured is lost in a trade (section 11), not "for free".
    if (s == c.played.to && c.f.captured != NoPiece) return false;
    if (!isUndefended(c.p1, s) || seePoints(c.p1, r0.move) < points(x)) return false;
    Explanation ex;
    if (!settleLoss(c, 0, 0, ex)) return false;
    const bool guard = hasBit(c.f.undefended, s) && c.level <= 4;
    // A threat not seen: the coach's last move made the capturing piece attack this one ("my last
    // move attacked it", "it was already attacking it"), and the move under review left it there.
    bool threat = false;
    if (!guard && s != c.played.to && c.level <= 3 && c.ply > 0 && seeSquarePoints(c.p0, s, c.coach) > 0 &&
        hasBit(c.p0.attackersTo(s, c.coach), r0.move.from)) {
        const Position& before = c.g->positionAt(size_t(c.ply - 1));   // before the coach's last move
        threat = !hasBit(hangingPieces(before, c.human, false), s) && !hasBit(before.attackersTo(s, c.coach), r0.move.from);
    }
    ex.type = ExType::Hanging;
    ex.concrete = true;
    ex.hintSquare = squareOnP0(c, s);
    Beat b = bandLine(c, guard ? "ex.hanging_guard" : threat ? "ex.hanging_threat" : "ex.hanging");
    put(b.line, "your", pieceArg(c.p1, s, c.human));
    put(b.line, "sq", Arg::ofSquare(s));
    put(b.line, "my", pieceArg(c.p1, r0.move.from, c.human));
    put(b.line, "pts", Arg::ofNumber(points(x)));
    if (guard) {
        put(b.line, "your2", Arg::ofPiece(c.f.piece, c.human, true, c.played.to));
        pointSquare(b, c.played.from, "your2");
        pointPiece(b, s, "your", true);
        traceMove(b, r0.piece, r0.move.from, s, replyAnchor(c));
    } else if (threat) {
        traceMove(b, r0.piece, r0.move.from, s, "my");
        pointPiece(b, s, "your", true);
    } else {
        pointPiece(b, s, "your", true);
        traceMove(b, r0.piece, r0.move.from, s, replyAnchor(c));
    }
    ex.cause.push_back(b);
    if (c.level == 1) ex.tip.push_back(line(c, "ex.hanging.tip.b1", Look::Player));
    out = ex;
    return true;
}

// ---- 11. Lost in an exchange ----
// Each family names why the exchange on the square loses (a bad capture, more attackers than
// defenders, a cheaper attacker), so the claim is checked on the board and along the line: the
// captures there leave the human down (from before the move: a capture elsewhere counts), in the
// points the lines speak, after exactly the moves they show; a line that stops where the human can
// still take back proves nothing.
bool exchange(const Ctx& c, Explanation& out) {
    if (c.r.empty() || c.r[0].mover != c.coach) return false;
    const LineStep& r0 = c.r[0];
    if (r0.captured == NoPiece || (r0.move.flags & MoveEnPassant)) return false;
    const Square s = r0.move.to;
    const PieceType victim = c.p1.at(s).type;
    // The human's capture on s loses by exchange: the coach takes back.
    const bool badCapture = c.f.captured != NoPiece && c.f.seePts <= -2 && s == c.played.to;
    int run = 0;   // consecutive captures on s from the reply on
    while (run < int(c.r.size()) && c.r[size_t(run)].move.to == s && c.r[size_t(run)].captured != NoPiece) ++run;
    // The last capture of the coach's in the run (the human's recapture after it comes with it).
    int last = run - 1;
    if (last >= 0 && c.r[size_t(last)].mover != c.coach) --last;
    Explanation ex;
    if (!settleLoss(c, last, 0, ex)) return false;
    // The count says who really takes part: a slider behind another (a battery) counts, a piece pinned
    // to its king off the line does not.
    const uint64_t all = exchangeParticipants(c.p1, s, false);
    const uint64_t att = all & c.p1.pieces(c.coach), def = all & c.p1.pieces(c.human);
    std::string family;
    int n = 0, n2 = 0;
    if (badCapture) {
        // "{your} is worth {n}, you only got {n2}".
        if (points(c.f.captured) >= points(victim)) return false;
        family = "ex.exchange_capture";
        n = points(victim);
        n2 = points(c.f.captured);
    } else if (points(r0.piece) < points(victim)) {
        // "{your} is worth {n}, {my} only {n2}": cheaper in points, not only in centipawns. Before the
        // count: a pawn that takes a queen wins it however many defend it.
        family = "ex.exchange_cheap";
        n = points(victim);
        n2 = points(r0.piece);
    } else if (squareCount(att) > squareCount(def) && def != 0) {
        // "I attack it {n} times, you defend it {n2} times".
        family = "ex.exchange_count";
        n = squareCount(att);
        n2 = squareCount(def);
    } else {
        return false;
    }
    ex.type = ExType::Exchange;
    ex.concrete = true;
    ex.hintSquare = squareOnP0(c, s);
    ex.targets = {s};
    Beat b = bandLine(c, family);
    put(b.line, "sq", Arg::ofSquare(s));
    put(b.line, "your", pieceArg(c.p1, s, c.human));
    put(b.line, "my", pieceArg(c.p1, r0.move.from, c.human));
    put(b.line, "n", Arg::ofNumber(n));
    put(b.line, "n2", Arg::ofNumber(n2));
    putLoss(b, c, ex);
    if (family == "ex.exchange_count") {
        pointSquare(b, s, "sq");
        for (Square a : squaresOf(att)) markPiece(b, a, "sq");
        for (Square d : squaresOf(def)) markPiece(b, d, "sq");
    } else {
        pointPiece(b, s, "your", true);
        traceMove(b, r0.piece, r0.move.from, s, c.level <= 2 ? "my" : "sq");
    }
    ex.cause.push_back(b);
    out = ex;
    return true;
}

// ---- 11b. Material lost along the line, no motif to name ----
// "After {line}, you've lost {pts}": the first capture of the line that costs the human material for
// good, beyond what the best move costs, and as much as the move costs (settleLoss); levels 1-2 name
// the piece that falls.
bool materialLine(const Ctx& c, Explanation& out) {
    if (c.r.empty() || c.r[0].mover != c.coach) return false;
    const int reach = std::min(int(c.r.size()), std::max(8, c.b.lookahead + 4));
    for (int k = 0; k < reach; ++k) {
        Explanation ex;
        if (!settleLoss(c, k, k, ex, c.level <= 2)) continue;
        // The piece named: the one whose capture settles the loss (not one traded on the way), on
        // its square of the table (p1).
        Square named = NoSquare;
        {
            const LineStep& st = c.r[size_t(k)];
            const Square on = (st.move.flags & MoveEnPassant) ? NoSquare : squareBefore(c, st.move.to, k, 0);
            if (on != NoSquare && c.p1.at(on).color == c.human && c.p1.at(on).type == st.captured) named = on;
        }
        // Levels 1-2 name the piece and show the whole sequence (a claim the table does not show
        // would be lost on a beginner).
        // The piece named is the reason by itself ("I can win your pawn" for two pawns lost says too little).
        if (c.level <= 2 && (named == NoSquare || ex.fullPlies > c.b.demoPlies ||
                             !c.lossExplains(points(c.p1.at(named).type), c.p1.at(named).type == Pawn)))
            continue;
        ex.type = ExType::Material;
        ex.concrete = true;
        ex.hintSquare = named != NoSquare ? squareOnP0(c, named) : NoSquare;
        Beat b = bandLine(c, "ex.material");
        putLoss(b, c, ex);
        if (named != NoSquare) {
            // Levels 1-2 name it ({t1}); from level 3 the line says the sequence, and the piece is
            // pointed at as it is said.
            put(b.line, "t1", pieceArg(c.p1, named, c.human));
            pointPiece(b, named, c.level <= 2 ? "t1" : "line", true);
        }
        const LineStep& r0 = c.r[0];
        traceMove(b, r0.piece, r0.move.from, r0.move.to, c.level <= 2 ? "t1" : "line");
        ex.cause.push_back(b);
        out = ex;
        return true;
    }
    return false;
}

// ---- 12. Missed capture of a free piece and missed fork ----
bool missedCapture(const Ctx& c, Explanation& out) {
    if (c.best.empty() || c.isBest) return false;
    const LineStep& b0 = c.best[0];
    if (b0.captured == NoPiece || b0.mover != c.human) return false;
    const Square y = b0.move.to;
    const PieceType yt = b0.captured;
    // Every line says the piece was free ("loose", "unprotected", "for nothing"): no protector, and
    // the capture wins all of it, x-rays included.
    if (!isUndefended(c.p0, y) || seePoints(c.p0, b0.move) < points(yt)) return false;
    if (c.p1.at(y).empty() || c.p1.at(y).color != c.coach) return false;   // taken after all, or moved
    if (c.f.captured != NoPiece && c.f.seePts >= points(yt) - 1) return false;   // took as much elsewhere
    // A move that loses as much as the piece missed: the loss is the story (materialLine).
    if (c.concreteLoss(8) && c.lossWithin(8) >= points(yt)) return false;
    const bool bigPiece = points(yt) >= 3;
    const bool fault = c.j.delta >= 5.0;
    if (!fault && !(c.level <= 2 && bigPiece)) return false;
    Explanation ex;
    ex.type = ExType::MissedCapture;
    ex.missed = true;
    ex.concrete = true;
    ex.includesBest = true;
    ex.offer = c.level <= 2 && bigPiece;
    Beat b = bandLine(c, "ex.missed_capture");
    put(b.line, "my", pieceArg(c.p1, y, c.human));
    put(b.line, "sq", Arg::ofSquare(y));
    put(b.line, "your", Arg::ofPiece(b0.piece, c.human, true, b0.move.from));
    pointPiece(b, y, "my", true);
    traceMove(b, b0.piece, b0.move.from, y, c.level == 1 ? "your" : "best");
    ex.cause.push_back(b);
    out = ex;
    return true;
}

bool missedFork(const Ctx& c, Explanation& out) {
    if (c.best.size() < 2 || c.isBest || c.j.delta < 5.0) return false;
    const LineStep& b0 = c.best[0];
    Position q = c.p0;
    q.makeMove(b0.move);
    const uint64_t targets = forkTargets(q, b0.move.to);
    if (squareCount(targets) < 2) return false;
    // The best line takes one of the targets, and what the fork wins along it is kept for good
    // (where the line stops, the coach may still take back on the board).
    bool takes = false;
    for (size_t i = 2; i < c.best.size() && i < 5; i += 2)
        if (c.best[i].mover == c.human && c.best[i].captured != NoPiece && hasBit(targets, c.best[i].move.to)) takes = true;
    if (!takes) return false;
    int gainPts = 0;
    for (size_t i = 1; i <= c.best.size() && i <= 5; ++i)
        gainPts = std::max(gainPts, heldGain(c.best, i, c.base, c.human, c.bestEnd));
    if (gainPts < 2) return false;
    const std::vector<Square> ts = byValue(q, targets);
    Explanation ex;
    ex.type = ExType::MissedFork;
    ex.missed = true;
    ex.concrete = true;
    ex.includesBest = true;
    Beat b = bandLine(c, "ex.missed_fork");
    // The targets are the coach's pieces: they still stand on p1 unless the move took one.
    put(b.line, "t1", Arg::ofPiece(q.at(ts[0]).type, c.coach, false, ts[0]));
    put(b.line, "t2", Arg::ofPiece(q.at(ts[1]).type, c.coach, false, ts[1]));
    traceMove(b, b0.piece, b0.move.from, b0.move.to, "best");
    for (int i = 0; i < 2; ++i) {
        const Square t = ts[size_t(i)];
        if (!c.p1.at(t).empty() && c.p1.at(t).color == c.coach) pointPiece(b, t, i == 0 ? "t1" : "t2");
        else markSquare(b, t, i == 0 ? "t1" : "t2");
    }
    ex.cause.push_back(b);
    out = ex;
    return true;
}

bool endgamePhase(const Ctx& c) {
    return phaseOfPly(dividePhases(*c.g), c.ply) == 2 || majorsAndMinors(c.p1) <= 4;
}

// Where the pawn that moves at line[i] stood when the line began: its earlier moves (its side's plies)
// traced back, since it may have captured on its way.
Square pawnOrigin(const std::vector<LineStep>& line, int i) {
    Square from = line[size_t(i)].move.from;
    for (int j = i - 2; j >= 0; j -= 2) {
        const LineStep& prev = line[size_t(j)];
        if (prev.mover == line[size_t(i)].mover && prev.piece == Pawn && prev.move.to == from) from = prev.move.from;
    }
    return from;
}

// ---- 13. Pawn promotion race ----
bool promotionRace(const Ctx& c, Explanation& out) {
    if (!endgamePhase(c) || c.j.delta < 5.0) return false;
    const Square hk = c.p1.kingSquare(c.human);
    // (a) The coach queens along the refutation (the part a player can follow).
    const int reach = c.followable(c.b.lookahead + 2, true);
    for (int i = 0; i < reach; ++i) {
        const LineStep& st = c.r[size_t(i)];
        if (st.mover != c.coach || st.promotion == NoPiece) continue;
        const Square prom = st.move.to;
        // The coach's pawn on the table now: the one that queens, else the nearest one on that file.
        const Square from = pawnOrigin(c.r, i);
        Square pawn = c.p1.at(from) == Piece{Pawn, c.coach} ? from : NoSquare;
        if (pawn == NoSquare)
            for (Square s : squaresOf(c.p1.pieces(c.coach, Pawn)))
                if (fileOf(s) == fileOf(st.move.from) && (pawn == NoSquare || std::abs(rankOf(s) - rankOf(prom)) < std::abs(rankOf(pawn) - rankOf(prom))))
                    pawn = s;
        if (pawn == NoSquare) continue;
        // Every line says the king is too far: the rule of the square on the square the line queens on
        // (the coach to move, as outsideSquare()). Inside it, the pawn queens for another reason.
        const int kingSteps = std::max(std::abs(fileOf(hk) - fileOf(prom)), std::abs(rankOf(hk) - rankOf(prom)));
        int pawnSteps = std::abs(rankOf(prom) - rankOf(pawn));
        if (rankOf(pawn) == (c.coach == White ? 1 : 6)) pawnSteps -= 1;   // the double step saves a move
        if (kingSteps <= pawnSteps) continue;
        Explanation ex;
        ex.type = ExType::PromotionRace;
        ex.concrete = true;
        ex.fullPlies = i + 1;   // shown in full at levels 1-3 (planDemo), said from level 4
        Beat b = bandLine(c, "ex.promotion");
        put(b.line, "sq", Arg::ofSquare(prom));
        put(b.line, "your", pieceArg(c.p1, hk, c.human));
        put(b.line, "my", pieceArg(c.p1, pawn, c.human));
        put(b.line, "n", Arg::ofNumber(pawnSteps));
        put(b.line, "n2", Arg::ofNumber(kingSteps));
        traceMove(b, Rook, pawn, prom, "sq");   // a straight stroke from the pawn to the square
        pointPiece(b, hk, "your");
        ex.includesBest = c.level == 6;
        ex.cause.push_back(b);
        out = ex;
        return true;
    }
    // (b) The human's own pawn could have run.
    for (int i = 0; i < int(c.best.size()) && i < c.b.lookahead + 2; ++i) {
        const LineStep& st = c.best[size_t(i)];
        if (st.mover != c.human || st.promotion == NoPiece) continue;
        // The human did move that pawn, or one on its file.
        if (c.f.piece == Pawn &&
            (c.played.from == pawnOrigin(c.best, i) || fileOf(c.played.from) == fileOf(st.move.to)))
            return false;
        Explanation ex;
        ex.type = ExType::PromotionRace;
        ex.missed = true;
        ex.concrete = true;
        ex.includesBest = c.level >= 3;
        Beat b = bandLine(c, "ex.promotion_missed");
        put(b.line, "sq", Arg::ofSquare(st.move.to));
        pointSquare(b, st.move.to, "sq");
        if (c.level >= 3) traceMove(b, c.best[0].piece, c.best[0].move.from, c.best[0].move.to, "best");
        ex.cause.push_back(b);
        out = ex;
        return true;
    }
    return false;
}

// ---- 14. King safety ----
// Sections 14-18 say what the move does to the position: never when the line loses material or
// mates (a concrete loss is the reason; when no detector can name it, the better move says enough).
bool kingSafety(const Ctx& c, Explanation& out) {
    // The checks counted and the line said: the part of the refutation a player can follow.
    const int reach = c.followable(c.b.lookahead + 2, true);
    // Never for a move that loses material: the king's safety may be why it is lost (a take-back
    // that opens the king), but the coach cannot show it.
    if (c.j.delta < 5.0 || c.concreteLoss(8) || c.losesMaterial(8) || reach == 0) return false;
    int checks = 0;
    for (int i = 0; i < reach; ++i)
        if (c.r[size_t(i)].mover == c.coach && c.r[size_t(i)].check) ++checks;
    // Pawns and a rook around a king in an endgame are no danger to it (an active king is the norm).
    const bool zone = !endgamePhase(c) && kingZoneAttacks(c.p1, c.human) >= 3;
    if (checks < 2 && !zone) return false;
    const Square k = c.p0.kingSquare(c.human);
    const int home = c.human == White ? 0 : 7, up = c.human == White ? 1 : -1;
    const bool castled = rankOf(k) == home && (fileOf(k) <= 2 || fileOf(k) >= 5);
    const bool shield = c.f.piece == Pawn && castled && std::abs(fileOf(c.played.from) - fileOf(k)) <= 1 &&
                        (rankOf(c.played.from) == home + up || rankOf(c.played.from) == home + 2 * up);
    const bool walk = c.f.piece == King && !c.f.castleKing && !c.f.castleQueen && !c.p0.inCheck() &&
                      c.p0.pieces(c.human, Queen) && c.p0.pieces(c.coach, Queen);
    const bool centre = c.ply >= 20 && fileOf(k) == 4 && rankOf(k) == home &&
                        (c.p0.castling() & castlingOf(c.human));
    if (!shield && !walk && !centre && !zone) return false;
    if (!shield && (c.level == 2 || c.level == 3) && checks < 2) return false;   // "I can give many checks"
    Explanation ex;
    ex.type = ExType::KingSafety;
    ex.concrete = true;
    const Square k1 = c.p1.kingSquare(c.human);
    Beat b = bandLine(c, shield ? "ex.king_shield" : "ex.king_exposed");
    put(b.line, "your", pieceArg(c.p1, k1, c.human));
    put(b.line, "sq", Arg::ofSquare(shield ? c.played.from : k1));
    pointPiece(b, k1, "your", true);
    if (shield) markSquare(b, c.played.from, "your");
    ex.cause.push_back(b);
    out = ex;
    return true;
}

// ---- 15. Bad trade: into a lost pawn ending; the bishop pair ----
bool badTrade(const Ctx& c, Explanation& out) {
    if (c.level < 3 || c.j.delta < 5.0 || c.concreteLoss(8) || c.losesMaterial(8)) return false;
    // A trade needs pieces on both sides (a piece simply lost is another explanation).
    const uint64_t kingsPawns = c.p0.pieces(Pawn) | c.p0.pieces(King);
    const bool bothHavePieces = (c.p0.pieces(c.human) & ~kingsPawns) && (c.p0.pieces(c.coach) & ~kingsPawns);
    if (c.j.wBest >= 45.0 && c.j.wPlayed <= 30.0 && bothHavePieces) {
        Position q = c.p1;
        bool pawnEnding = majorsAndMinors(q) == 0;
        int plies = 0;
        for (int i = 0; !pawnEnding && i < int(c.r.size()) && i < c.b.lookahead + 2; ++i) {
            q.makeMove(c.r[size_t(i)].move);
            if (majorsAndMinors(q) == 0) {
                pawnEnding = true;
                plies = i + 1;
            }
        }
        if (pawnEnding) {
            Explanation ex;
            ex.type = ExType::BadTrade;
            ex.concrete = true;
            ex.fullPlies = plies;   // the trades, shown at level 3
            Beat b = bandLine(c, "ex.bad_trade", Look::Board);
            put(b.line, "line", Arg::ofMoves(sanLine(c.r, 0, size_t(c.followable(std::max(plies, 2), true)))));
            ex.cause.push_back(b);
            out = ex;
            return true;
        }
    }
    if (c.level >= 5 && squareCount(c.p0.pieces(c.human, Bishop)) == 2 && c.f.piece == Bishop && c.f.captured != NoPiece &&
        !c.r.empty() && c.r[0].move.to == c.played.to && c.r[0].captured == Bishop &&
        squareCount(c.p1.pieces(c.coach, Bishop)) == 2) {
        Explanation ex;
        ex.type = ExType::BadTrade;
        ex.concrete = true;
        Beat b = bandLine(c, "ex.bishop_pair", Look::Board);
        ex.cause.push_back(b);
        out = ex;
        return true;
    }
    return false;
}

// ---- 17. Endgame technique ----
bool endgame(const Ctx& c, Explanation& out) {
    if (!endgamePhase(c) || c.best.empty() || c.isBest || c.j.delta < 5.0 || c.concreteLoss(8) || c.losesMaterial(8))
        return false;
    const LineStep& b0 = c.best[0];
    Explanation ex;
    ex.type = ExType::Endgame;
    ex.concrete = true;
    std::string family;
    // Opposition (pawn endings, levels 3+): the best king move faces the other king, one square between.
    if (c.level >= 3 && majorsAndMinors(c.p0) == 0 && b0.piece == King && c.f.piece != King && c.j.delta >= 15.0) {
        Position q = c.p0;
        q.makeMove(b0.move);
        const Square a = q.kingSquare(c.human), d = q.kingSquare(c.coach);
        const int df = std::abs(fileOf(a) - fileOf(d)), dr = std::abs(rankOf(a) - rankOf(d));
        if ((df == 0 && dr == 2) || (dr == 0 && df == 2)) family = "ex.opposition";
    }
    // Push the passed pawn.
    if (family.empty() && b0.piece == Pawn && isPassed(c.p0, b0.move.from) &&
        !(c.f.piece == Pawn && c.played.from == b0.move.from))
        family = "ex.push_passed";
    // Bring the king forward.
    if (family.empty() && b0.piece == King && b0.captured == NoPiece && c.f.piece != King) {
        auto centre = [](Square s) {
            return std::max(std::abs(2 * fileOf(s) - 7), std::abs(2 * rankOf(s) - 7));
        };
        if (centre(b0.move.to) < centre(b0.move.from)) family = "ex.king_active";
    }
    if (family.empty()) return false;
    // A level-5 sentence names the line; when only the move itself can be followed, level 4's.
    const int plies = c.followableBest(std::min(4, c.b.linePlies));
    Beat b = line(c, bandKey(family, c.level == 5 && plies < 2 ? 4 : c.level), Look::Target);
    const Square k = c.p1.kingSquare(c.human);
    put(b.line, "your", pieceArg(c.p1, k, c.human));
    const Square prom = b0.piece == Pawn ? promotionSquare(c.p0, b0.move.from) : NoSquare;
    put(b.line, "sq", Arg::ofSquare(prom != NoSquare ? prom : b0.move.to));
    put(b.line, "line", Arg::ofMoves(sanLine(c.best, 0, size_t(plies))));
    put(b.line, "eval", evalArg(c.l1->score));
    if (c.level == 1 && family == "ex.push_passed") pointSquare(b, prom, "sq");
    else if (c.level <= 2 && family == "ex.king_active") pointPiece(b, k, "your", true);
    else traceMove(b, b0.piece, b0.move.from, b0.move.to, "best");
    ex.includesBest = c.level >= 2;
    ex.cause.push_back(b);
    out = ex;
    return true;
}

// ---- 18. Positional fallback (levels 4-6) ----
// "X keeps a better position": never for a move that loses material (Ctx::losesMaterial).
bool positional(const Ctx& c, Explanation& out) {
    if (c.level < 4 || c.best.empty() || c.isBest || c.concreteLoss(8) || c.losesMaterial(8)) return false;
    Explanation ex;
    ex.type = ExType::Positional;
    ex.includesBest = true;
    // A level-5 sentence names the line; when only the move itself can be followed, level 4's.
    const int plies = c.followableBest(std::min(4, c.b.linePlies));
    Beat b = line(c, bandKey("ex.positional", c.level == 5 && plies < 2 ? 4 : c.level), Look::Target);
    put(b.line, "line", Arg::ofMoves(sanLine(c.best, 0, size_t(plies))));
    put(b.line, "eval", evalArg(c.l1->score));
    traceMove(b, c.best[0].piece, c.best[0].move.from, c.best[0].move.to, "best");
    ex.cause.push_back(b);
    out = ex;
    return true;
}

// ---- Tips ----
enum TipBit { EarlyQueen, SamePiece, Castle, QueenGrab, Centre, Flank, KnightRim, MateTechnique, Fifty, Repetition };

// The coach's reply develops a piece with an attack on the human's queen (a tempo).
bool tempoOnQueen(const Ctx& c) {
    if (c.r.empty() || c.r[0].mover != c.coach) return false;
    const LineStep& r0 = c.r[0];
    if (!(r0.piece == Knight || r0.piece == Bishop || r0.piece == Pawn)) return false;
    Position q = c.p1;
    q.makeMove(r0.move);
    return (q.attacksFrom(r0.move.to) & q.pieces(c.human, Queen)) != 0;
}

Beat tipBeat(const Ctx& c, const char* name) { return line(c, bandKey(std::string("tip.") + name, c.level), Look::Player); }

}  // namespace

bool findExplanation(const Ctx& c, Explanation& out) {
    // A move that stalemates ends the game: that is what the player must hear, even when it also
    // missed a mate (the stalemate before the missed mate in this one case).
    if (c.f.stalemate && stalemate(c, out)) return true;
    if (mateAllowed(c, out) || mateMissed(c, out) || stalemate(c, out)) return true;
    if (materialLost(c) && (fork(c, out) || discovered(c, out) || skewer(c, out) || pin(c, out) || trapped(c, out) ||
                            backRank(c, out)))
        return true;
    if (hanging(c, out) || exchange(c, out)) return true;
    if (missedCapture(c, out) || missedFork(c, out)) return true;
    if (materialLine(c, out)) return true;
    if (promotionRace(c, out) || kingSafety(c, out) || badTrade(c, out) || endgame(c, out)) return true;
    return positional(c, out);
}

bool findTip(const Ctx& c, uint32_t tipsSaid, Explanation& out) {
    auto fresh = [&](TipBit bit) { return !(tipsSaid & (1u << bit)); };
    auto done = [&](TipBit bit, Beat b, ExType type) {
        out = Explanation{};
        out.type = type;
        out.tipBit = bit;
        out.cause.push_back(b);
        return true;
    };
    const Color H = c.human, C = c.coach;
    const int full = c.p0.fullmoveNumber();
    const int undeveloped = undevelopedMinors(c.p0, H);
    const bool worth = c.j.delta >= 3.0 || tempoOnQueen(c);

    // Endgame technique tips (levels 1-3).
    if (c.level <= 2 && fresh(MateTechnique) && c.l1->score.mate > 0 && (c.p1.pieces(C) & ~c.p1.pieces(King)) == 0 &&
        c.p0.halfmoveClock() >= 16 && !c.isBest) {
        Beat b = line(c, "tip.mate_technique.b1", Look::Player);
        return done(MateTechnique, b, ExType::Endgame);
    }
    if (c.level <= 3 && fresh(Fifty) && c.p1.halfmoveClock() >= 80 && c.j.wBest >= 70.0)
        return done(Fifty, line(c, "tip.fifty.b1", Look::Player), ExType::Endgame);
    // No explanation type: a repetition is not a stalemate fault (the appraisal's themes).
    if (c.level <= 3 && fresh(Repetition) && c.j.wBest >= 85.0 && c.g->repetitionCount() == 2)
        return done(Repetition, line(c, "tip.repetition.b1", Look::Player), ExType::None);

    // Opening principles (levels 1-3; level 4 only the costly pawn grab), before move 12; never for a
    // move whose cost is material lost or a mate (a principle would hide what goes wrong).
    if (c.ply >= 24 || !worth || c.concreteLoss(8)) return false;
    if (c.level <= 3 && fresh(EarlyQueen) && c.f.piece == Queen && full < 6 && undeveloped >= 2 &&
        !(c.f.captured != NoPiece && c.f.seePts >= 1) && !mateThreat(c.p1, H)) {
        Beat b = tipBeat(c, "early_queen");
        put(b.line, "your", pieceArg(c.p1, c.played.to, H));
        pointPiece(b, c.played.to, "your");
        const int r = H == White ? 0 : 7;
        for (int f : {1, 2, 5, 6}) {
            const Square s = makeSquare(f, r);
            const Piece pc = c.p1.at(s);
            if (pc.color == H && (pc.type == Knight || pc.type == Bishop)) markPiece(b, s, "your");
        }
        return done(EarlyQueen, b, ExType::Opening);
    }
    if (c.level <= 4 && fresh(QueenGrab) && c.f.piece == Queen && c.f.captured == Pawn &&
        (fileOf(c.played.to) <= 1 || fileOf(c.played.to) >= 6) && full < 12 &&
        (c.j.delta >= 5.0 || tempoOnQueen(c)) && (c.level <= 3 || c.j.delta >= 10.0)) {
        Beat b = tipBeat(c, "queen_grab");
        put(b.line, "your", pieceArg(c.p1, c.played.to, H));
        pointPiece(b, c.played.to, "your");
        return done(QueenGrab, b, ExType::Opening);
    }
    if (c.level > 3) return false;
    if (fresh(SamePiece) && full <= 10 && undeveloped >= 2 && c.f.captured == NoPiece && !c.f.check &&
        c.f.piece != Pawn && c.f.piece != King && !c.p0.attackersTo(c.played.from, C)) {
        bool again = false;
        for (int i = c.ply - 2; i >= 0; i -= 2)
            if (c.g->moves()[size_t(i)].to == c.played.from) again = true;
        if (again) {
            Beat b = tipBeat(c, "same_piece");
            put(b.line, "your", pieceArg(c.p1, c.played.to, H));
            pointPiece(b, c.played.to, "your");
            return done(SamePiece, b, ExType::Opening);
        }
    }
    if (fresh(Castle) && c.ply >= 20 && !(c.f.castleKing || c.f.castleQueen)) {
        const Square k = c.p0.kingSquare(H);
        const int home = H == White ? 0 : 7;
        const bool rights = c.p0.castling() & castlingOf(H);
        bool openCentre = false;
        for (int f : {3, 4}) {
            bool pawn = false;
            for (Square s : squaresOf(c.p0.pieces(H, Pawn)))
                if (fileOf(s) == f) pawn = true;
            if (!pawn) openCentre = true;
        }
        bool castleBest = false;
        for (const ai::PvLine* l : {c.l1, c.l2}) {
            if (!l || l->pv.empty()) continue;
            const Move m = c.p0.parseUCI(l->pv[0]);
            if (m.valid() && c.p0.at(m.from).type == King && std::abs(fileOf(m.to) - fileOf(m.from)) == 2) castleBest = true;
        }
        if (fileOf(k) == 4 && rankOf(k) == home && rights && openCentre && castleBest) {
            Beat b = tipBeat(c, "castle");
            put(b.line, "your", pieceArg(c.p1, c.p1.kingSquare(H), H));
            pointPiece(b, c.p1.kingSquare(H), "your");
            return done(Castle, b, ExType::Opening);
        }
    }
    if (fresh(KnightRim) && c.f.piece == Knight && (fileOf(c.played.to) == 0 || fileOf(c.played.to) == 7) && full < 8 &&
        c.f.captured == NoPiece) {
        Beat b = tipBeat(c, "knight_rim");
        put(b.line, "your", pieceArg(c.p1, c.played.to, H));
        pointPiece(b, c.played.to, "your");
        return done(KnightRim, b, ExType::Opening);
    }
    if (fresh(Flank) && c.f.piece == Pawn && (fileOf(c.played.from) == 0 || fileOf(c.played.from) == 7) && full <= 8 &&
        undeveloped >= 3) {
        int flank = 0;
        for (int i = c.ply; i >= 0; i -= 2) {
            const Move& m = c.g->moves()[size_t(i)];
            const Piece pc = c.g->positionAt(size_t(i)).at(m.from);
            if (pc.type == Pawn && (fileOf(m.from) == 0 || fileOf(m.from) == 7)) ++flank;
        }
        if (flank >= 2) return done(Flank, tipBeat(c, "flank"), ExType::Opening);
    }
    if (fresh(Centre) && full > 4 && full <= 12) {
        const int r4 = H == White ? 3 : 4;
        const Square d = makeSquare(3, r4), e = makeSquare(4, r4);
        const bool pawn = c.p1.at(d) == Piece{Pawn, H} || c.p1.at(e) == Piece{Pawn, H};
        bool control = false;
        for (int f = 3; f <= 4; ++f)
            for (int r = 3; r <= 4; ++r)
                if (c.p1.attackersTo(makeSquare(f, r), H)) control = true;
        if (!pawn && !control) {
            Beat b = tipBeat(c, "centre");
            put(b.line, "sq", Arg::ofSquare(c.p1.at(e).empty() ? e : d));
            pointSquare(b, c.p1.at(e).empty() ? e : d, "sq");
            return done(Centre, b, ExType::Opening);
        }
    }
    return false;
}

bool drawTip(const Ctx& c, uint32_t tipsSaid) {
    Explanation t;
    return findTip(c, tipsSaid, t) && (t.tipBit == Fifty || t.tipBit == Repetition);
}

namespace {
// The human's lead from c.base along a line, read as bestLoss reads a loss: after the line's first
// 'from' plies, where it settles (a capture taken back on the next ply is a whole exchange; where the
// line stops, the coach to move takes what it can at once), and over the next four plies: the least
// (what the coach wins back by then was never won) and the most. False when a mate, a stalemate or
// a promotion comes into the line: the points would not tell what happens.
struct Lead {
    const Ctx& c;
    const std::vector<LineStep>& line;
    const Position& end;
    bool plain() const {
        for (const LineStep& st : line)
            if (st.mate || st.stalemate || st.promotion != NoPiece) return false;
        return true;
    }
    int at(size_t j) const {
        if (j < line.size() && line[j - 1].captured != NoPiece && line[j].captured != NoPiece &&
            line[j].move.to == line[j - 1].move.to)
            ++j;
        int g = line[j - 1].balance - c.base;
        if (j == line.size() && end.sideToMove() == c.coach && end.hasLegalMove()) g -= bestCapturePoints(end, c.coach);
        return g;
    }
    void window(size_t from, int& least, int& most) const {
        least = most = at(from);
        for (size_t j = from + 1; j <= std::min(line.size(), from + 4); ++j) {
            least = std::min(least, at(j));
            most = std::max(most, at(j));
        }
    }
};
}  // namespace

int missedGain(const Ctx& c) {
    if (c.best.empty() || c.isBest || c.bestLoss > 0 || c.losesMaterial(8)) return 0;
    const Lead best{c, c.best, c.bestEnd}, played{c, c.playedLine, c.playedEnd};
    if (!best.plain() || !played.plain()) return 0;
    // Where the best line settles after 8 plies, as bestLoss is read.
    int won = 0, most = 0;
    best.window(std::min<size_t>(8, c.best.size()), won, most);
    if (won < 1) return 0;
    // The move played wins nothing for good along its own line (not even a pawn it takes at once
    // and gives back later) and loses nothing.
    for (size_t i = 1; i <= std::min<size_t>(12, c.playedLine.size()); ++i) {
        int least = 0, high = 0;
        played.window(i, least, high);
        if (least > 0 || high < 0) return 0;
    }
    // As Ctx::lossExplains: a pawn explains a drop of up to 2.5 pawns, 2 points up to 8.
    return won >= 3 || (won >= 2 ? 400 : 250) * won >= c.dropCp ? won : 0;
}

}  // namespace detail
}  // namespace coach
