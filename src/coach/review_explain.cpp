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
    n = std::clamp(n, 2, c.b.demoPlies + 1);
    return size_t(std::min<int>(n, int(c.r.size())));
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

// Motifs 4-9 below: "R wins material with R[0]". A motif pays off after the threat, the
// escape and the capture (3 plies at least, more with an intermediate check), so the gain is not
// bounded by the band's lookahead: the motif itself stands on the board after R[0] (that is what
// the lines show), the engine's loss confirms it works, and the PV must capture one of its targets
// within motifReach() plies.
int motifReach(const Ctx& c) { return std::max(6, c.b.lookahead + 2); }
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
bool coachTakesOn(const Ctx& c, Square sq, int maxPly) { return coachTakesAt(c, sq, maxPly) >= 0; }

// A loss a line says with "after {line}, you've lost {pts}": the plies of r to show for the capture
// at ply k (with the next ply when it takes something back), and what the human has lost after
// exactly those plies (relative to p0). Not proven when r stops right there and the human can
// still win material back on the board.
struct LossShown {
    int plies = 0, lost = 0;
    bool proven = false;
};
LossShown lossShown(const Ctx& c, int k) {
    LossShown out;
    const int n = int(c.r.size());
    if (k < 0 || k >= n) return out;
    auto lostAfter = [&](int plies) { return c.base - c.r[size_t(plies - 1)].balance; };
    out.plies = k + 1;
    out.lost = lostAfter(out.plies);
    out.proven = true;
    if (out.plies < n) {
        if (lostAfter(out.plies + 1) < out.lost) out.lost = lostAfter(++out.plies);
    } else if (c.rEnd.sideToMove() == c.human && bestCapturePoints(c.rEnd, c.human) > 0) {
        out.proven = false;
    }
    return out;
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
    ex.concrete = plies <= c.b.lookahead;
    if (c.level <= 2 && (!ex.concrete || !lineKnown)) return false;   // too deep for the band: not this cause
    static const int kMateLimit[6] = {1, 2, 2, 3, 3, 4};
    ex.offer = n <= kMateLimit[c.level - 1];
    const Square k = c.p1.kingSquare(c.human);
    // Levels 3, 4 and 6 show the mate ("{line} is mate"): when the engine's line stops before it, the
    // line of level 5 says it without moves.
    Beat b = lineKnown ? bandLine(c, "ex.mate_allowed") : line(c, "ex.mate_allowed.b5");
    put(b.line, "m", Arg::ofNumber(n));
    put(b.line, "your", pieceArg(c.p1, k, c.human));
    if (lineKnown) put(b.line, "line", Arg::ofMoves(sanLine(c.r, 0, size_t(plies))));
    const LineStep& first = c.r.empty() ? LineStep{} : c.r[0];
    if (c.level == 1) {   // mate in one: "my queen goes to h7"
        put(b.line, "my", pieceArg(c.p1, first.move.from, c.human));
        put(b.line, "sq", Arg::ofSquare(first.move.to));
        traceMove(b, first.piece, first.move.from, first.move.to, "my");
    } else if (c.level == 2) {
        pointPiece(b, k, "your", true);
    } else if (c.level != 5 && lineKnown) {
        traceMove(b, first.piece, first.move.from, first.move.to, "line");
    }
    ex.cause.push_back(b);
    if (lineKnown && plies <= c.b.mateLinePlies) ex.demoPlies = plies;
    // After a one-move demonstration: the king has no escape square.
    if (c.level <= 2 && ex.demoPlies == 1) {
        Beat t = line(c, "ex.mate_allowed.tail.b1", Look::Target);
        put(t.line, "your", pieceArg(c.p1, k, c.human));
        pointPiece(t, k, "your", true);
        for (Square s : kingNeighbours(c.p1, k)) markSquare(t, s, "your");
        ex.tail.push_back(t);
    }
    // Levels 2-4 may name the pattern of the final position.
    if (lineKnown && c.level >= 2 && c.level <= 4) {
        Position q = c.p1;
        for (int i = 0; i < plies; ++i) q.makeMove(c.r[size_t(i)].move);
        const MatePattern mp = classifyMate(q);
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
            if (ex.demoPlies == plies) ex.tail.push_back(t);
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
    ex.offer = lost;
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
            ex.demoPlies = i + 1 <= c.b.demoPlies ? i + 1 : 0;
            out = ex;
            return true;
        }
    }
    return false;
}

// ---- 4. Fork ----
bool fork(const Ctx& c, Explanation& out) {
    if (c.r.empty() || c.r[0].mover != c.coach) return false;
    const LineStep& r0 = c.r[0];
    Position p2 = c.p1;
    p2.makeMove(r0.move);
    const uint64_t targets = forkTargets(p2, r0.move.to);
    if (squareCount(targets) < 2) return false;
    bool taken = false;
    for (Square t : squaresOf(targets))
        if (coachTakesOn(c, t, motifReach(c))) taken = true;
    if (!taken) return false;
    const std::vector<Square> ts = byValue(p2, targets);
    // The first target the line takes, and what the human has lost once the capture is answered.
    int takenAt = -1;
    for (Square t : squaresOf(targets)) {
        const int at = coachTakesAt(c, t, motifReach(c));
        if (at >= 0 && (takenAt < 0 || at < takenAt)) takenAt = at;
    }
    const LossShown shown = lossShown(c, takenAt);
    Explanation ex;
    ex.type = ExType::Fork;
    ex.concrete = true;
    ex.includesBest = c.level == 6;
    ex.demoPlies = c.level == 1 ? 1 : std::min(3, c.b.demoPlies);
    for (Square t : ts)
        if (coachTakesOn(c, t, motifReach(c))) {
            ex.hintSquare = squareOnP0(c, t);
            break;
        }
    // Level 4 says "after {line}, you've lost {pts}": when the capture leaves the human no material
    // down (the fork leads to mate, or the line stops before the human takes back), the lines of
    // level 5 say the fork alone.
    Beat b = c.level == 4 && (!shown.proven || shown.lost < 1) ? line(c, "ex.fork.b5") : bandLine(c, "ex.fork");
    put(b.line, "line", Arg::ofMoves(sanLine(c.r, 0, size_t(shown.plies))));
    put(b.line, "pts", Arg::ofNumber(shown.lost));
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
    if (c.level == 1) {
        Beat t = line(c, "ex.fork.tail.b1");
        put(t.line, "t1", pieceArg(c.p1, ts[0], c.human));
        put(t.line, "t2", pieceArg(c.p1, ts[1], c.human));
        pointPiece(t, ts[0], "t1");
        pointPiece(t, ts[1], "t2");
        ex.tail.push_back(t);
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
    // A target the line takes first; then a discovered check, when the line wins material (levels 1-3
    // and 5: the lines of levels 4 and 6 say the target itself falls).
    std::vector<Discovery> found;
    for (const Discovery& d : discoveredAttacks(c.p1, r0.move))
        if (!d.check && coachTakesOn(c, d.target, motifReach(c))) found.push_back(d);
    if (found.empty() && c.level != 4 && c.level != 6 && c.gain > 0 && c.gain <= motifReach(c) + 1)
        for (const Discovery& d : discoveredAttacks(c.p1, r0.move))
            if (d.check) found.push_back(d);
    for (const Discovery& d : found) {
        Explanation ex;
        ex.type = ExType::Discovered;
        ex.concrete = true;
        ex.demoPlies = std::min(3, c.b.demoPlies);
        ex.hintSquare = squareOnP0(c, d.target);
        Beat b = bandLine(c, "ex.discovered");
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
        if (!coachTakesOn(c, sk.behind, motifReach(c))) continue;
        Explanation ex;
        ex.type = ExType::Skewer;
        ex.concrete = true;
        ex.demoPlies = c.level == 1 ? 1 : std::min(3, c.b.demoPlies);
        ex.hintSquare = squareOnP0(c, sk.behind);
        Beat b = bandLine(c, "ex.skewer");
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
        if (!coachTakesOn(c, pn.pinned, reach)) continue;
        Explanation ex;
        ex.type = ExType::Pin;
        ex.concrete = true;
        ex.demoPlies = std::min(3, c.b.demoPlies);
        ex.hintSquare = squareOnP0(c, pn.pinned);
        Beat b = bandLine(c, "ex.pin");
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
                ex.type = ExType::Pin;
                ex.concrete = true;
                ex.demoPlies = std::min(2, c.b.demoPlies);
                ex.hintSquare = squareOnP0(c, s);
                Beat b = bandLine(c, "ex.pin_defender");
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
bool trapped(const Ctx& c, Explanation& out) {
    if (c.r.empty() || c.r[0].mover != c.coach) return false;
    Position p2 = c.p1;
    p2.makeMove(c.r[0].move);
    const int reach = motifReach(c);
    auto lostLater = [&](PieceType t) {
        for (int i = 2; i < int(c.r.size()) && i <= reach; i += 2)
            if (c.r[size_t(i)].captured == t) return true;
        return false;
    };
    Square x = NoSquare;
    for (Square s : squaresOf(p2.pieces(c.human) & ~(p2.pieces(Pawn) | p2.pieces(King))))
        if (isTrapped(p2, s) && lostLater(p2.at(s).type)) {
            x = s;
            break;
        }
    // A special pattern: a bishop grabs a rim pawn (Bxa7? b6, Bxh7? g6) and a pawn push
    // closes its retreat. Nothing attacks it yet (the king comes later), so isTrapped() does not see it.
    const bool rimGrab = c.f.piece == Bishop && c.f.captured == Pawn &&
                         (fileOf(c.played.to) == 0 || fileOf(c.played.to) == 7) && c.r[0].piece == Pawn;
    if (x == NoSquare && rimGrab && p2.at(c.played.to).type == Bishop && lostLater(Bishop)) {
        bool boxed = !p2.inCheck();
        if (boxed)
            for (const Move& m : p2.legalMovesFrom(c.played.to))
                if (seePoints(p2, m) >= 0) boxed = false;
        if (boxed) x = c.played.to;
    }
    if (x == NoSquare) return false;
    Explanation ex;
    ex.type = ExType::Trapped;
    ex.concrete = true;
    ex.demoPlies = c.level == 1 ? 1 : std::min(3, c.b.demoPlies);
    ex.hintSquare = squareOnP0(c, x);
    Beat b = bandLine(c, "ex.trapped");
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
    // The lines say the check wins material: the line must win some, for good.
    if (c.gain < 0 || c.gain > motifReach(c) + 1) return false;
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
        const Square k = c.p1.kingSquare(c.human);
        Explanation ex;
        ex.type = ExType::BackRank;
        ex.concrete = true;
        ex.demoPlies = std::min(i + 1, c.b.demoPlies);
        Beat b = bandLine(c, "ex.back_rank");
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
    if (c.lossWithin(c.b.lookahead) < (c.level <= 2 ? 2 : 1)) return false;
    if (!isUndefended(c.p1, s) || seePoints(c.p1, r0.move) < points(x)) return false;
    const bool guard = hasBit(c.f.undefended, s) && c.level <= 4;
    // A threat not seen: the coach's last move made the capturing piece attack this one ("my last
    // move attacked it", "it was already attacking it"), and the move under review left it there.
    bool threat = false;
    if (!guard && s != c.played.to && c.level <= 3 && c.ply > 0 && seeSquarePoints(c.p0, s, c.coach) > 0 &&
        hasBit(c.p0.attackersTo(s, c.coach), r0.move.from)) {
        const Position& before = c.g->positionAt(size_t(c.ply - 1));   // before the coach's last move
        threat = !hasBit(hangingPieces(before, c.human, false), s) && !hasBit(before.attackersTo(s, c.coach), r0.move.from);
    }
    Explanation ex;
    ex.type = ExType::Hanging;
    ex.concrete = true;
    ex.demoPlies = 1;
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
// captures there leave the human 2+ points down, in the points the lines speak, after exactly the
// moves they show; a line that stops where the human can still take back proves nothing.
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
    const LossShown shown = lossShown(c, run - 1);
    // The cost of the exchange: from before the move for a bad capture (it is part of it), else from
    // the position the move left.
    if (!shown.proven) return false;
    const int lost = shown.lost + (badCapture ? 0 : materialBalance(c.p1, c.human) - c.base);
    if (lost < 2) return false;
    const uint64_t seen = exchangeParticipants(c.p1, s, true), all = exchangeParticipants(c.p1, s, false);
    const uint64_t att = seen & c.p1.pieces(c.coach), def = seen & c.p1.pieces(c.human);
    std::string family;
    int n = 0, n2 = 0;
    if (badCapture) {
        // "{your} is worth {n}, you only got {n2}".
        if (points(c.f.captured) >= points(victim)) return false;
        family = "ex.exchange_capture";
        n = points(victim);
        n2 = points(c.f.captured);
    } else if (squareCount(att) > squareCount(def) && def != 0 && all == seen) {
        // "I attack it {n} times, you defend it {n2} times": no x-ray or pinned piece changes the count.
        family = "ex.exchange_count";
        n = squareCount(att);
        n2 = squareCount(def);
    } else if (points(r0.piece) < points(victim)) {
        // "{your} is worth {n}, {my} only {n2}": cheaper in points, not only in centipawns.
        family = "ex.exchange_cheap";
        n = points(victim);
        n2 = points(r0.piece);
    } else {
        return false;
    }
    Explanation ex;
    ex.type = ExType::Exchange;
    ex.concrete = true;
    ex.demoPlies = std::max(1, std::min(run, c.b.demoPlies));
    ex.hintSquare = squareOnP0(c, s);
    Beat b = bandLine(c, family);
    put(b.line, "sq", Arg::ofSquare(s));
    put(b.line, "your", pieceArg(c.p1, s, c.human));
    put(b.line, "my", pieceArg(c.p1, r0.move.from, c.human));
    put(b.line, "n", Arg::ofNumber(n));
    put(b.line, "n2", Arg::ofNumber(n2));
    put(b.line, "line", Arg::ofMoves(sanLine(c.r, 0, size_t(shown.plies))));
    put(b.line, "pts", Arg::ofNumber(lost));
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
    // (a) The coach queens along the refutation.
    for (int i = 0; i < int(c.r.size()) && i < c.b.lookahead + 2; ++i) {
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
        ex.demoPlies = i + 1 <= c.b.demoPlies ? i + 1 : 0;
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
bool kingSafety(const Ctx& c, Explanation& out) {
    if (c.j.delta < 5.0 || c.lossWithin(c.b.lookahead) >= 2 || c.r.empty()) return false;
    int checks = 0;
    for (int i = 0; i < int(c.r.size()) && i < c.b.lookahead + 2; ++i)
        if (c.r[size_t(i)].mover == c.coach && c.r[size_t(i)].check) ++checks;
    const bool zone = kingZoneAttacks(c.p1, c.human) >= 3;
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
    ex.demoPlies = std::min(c.b.demoPlies, 4);
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
    if (c.level < 3 || c.j.delta < 5.0) return false;
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
            ex.demoPlies = std::min(plies, c.b.demoPlies);
            Beat b = bandLine(c, "ex.bad_trade", Look::Board);
            put(b.line, "line", Arg::ofMoves(sanLine(c.r, 0, size_t(std::max(plies, 2)))));
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
    if (!endgamePhase(c) || c.best.empty() || c.isBest || c.j.delta < 5.0) return false;
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
    Beat b = bandLine(c, family);
    const Square k = c.p1.kingSquare(c.human);
    put(b.line, "your", pieceArg(c.p1, k, c.human));
    const Square prom = b0.piece == Pawn ? promotionSquare(c.p0, b0.move.from) : NoSquare;
    put(b.line, "sq", Arg::ofSquare(prom != NoSquare ? prom : b0.move.to));
    put(b.line, "line", Arg::ofMoves(sanLine(c.best, 0, size_t(std::min(4, c.b.demoPlies)))));
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
bool positional(const Ctx& c, Explanation& out) {
    if (c.level < 4 || c.best.empty() || c.isBest) return false;
    Explanation ex;
    ex.type = ExType::Positional;
    ex.includesBest = true;
    Beat b = bandLine(c, "ex.positional");
    put(b.line, "line", Arg::ofMoves(sanLine(c.best, 0, size_t(std::min(4, c.b.demoPlies)))));
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

    // Opening principles (levels 1-3; level 4 only the costly pawn grab), before move 12.
    if (c.ply >= 24 || !worth) return false;
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

}  // namespace detail
}  // namespace coach
