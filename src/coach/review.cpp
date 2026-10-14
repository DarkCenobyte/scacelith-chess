// The coach's review: classification, voicing, praise and takeback policy, script assembly,
// announcements and threat warnings (see review.h). The explanation detectors live in
// review_explain.cpp.
#include "coach/review.h"

#include "coach/review_internal.h"
#include "coach/tactics.h"

#include <algorithm>
#include <sstream>
#include <cmath>

namespace coach {

using namespace chess;
using namespace detail;

// ---- Win percentage, accuracy, classes ------------------------------------------------------------

double winPercent(int cp) {
    cp = std::clamp(cp, -1000, 1000);
    return 50.0 + 50.0 * (2.0 / (1.0 + std::exp(-0.00368208 * cp)) - 1.0);
}

namespace {
bool mates(const ai::Score& s) { return s.mate > 0 || s.matesNow; }
bool mated(const ai::Score& s) { return s.mate < 0 || s.matedNow; }
int mateMoves(const ai::Score& s) { return s.matesNow || s.matedNow ? 0 : std::abs(s.mate); }
}  // namespace

double winPercent(const ai::Score& s) {
    if (mates(s)) return winPercent(1000);
    if (mated(s)) return winPercent(-1000);
    return winPercent(s.cp);
}

double moveAccuracy(double wBefore, double wAfter) {
    if (wAfter >= wBefore) return 100.0;
    const double raw = 103.1668100711649 * std::exp(-0.04354415386753951 * (wBefore - wAfter)) - 3.166924740191411;
    return std::clamp(raw + 1.0, 0.0, 100.0);
}

const char* moveClassName(MoveClass c) {
    switch (c) {
    case MoveClass::Unjudged: return "unjudged";
    case MoveClass::Book: return "book";
    case MoveClass::Forced: return "forced";
    case MoveClass::Best: return "best";
    case MoveClass::Excellent: return "excellent";
    case MoveClass::Good: return "good";
    case MoveClass::Inaccuracy: return "inaccuracy";
    case MoveClass::Mistake: return "mistake";
    case MoveClass::Blunder: return "blunder";
    }
    return "";
}

MoveClass classifyDelta(double delta, bool playedIsBest) {
    if (playedIsBest || delta < 0.5) return MoveClass::Best;
    if (delta < 2.0) return MoveClass::Excellent;
    if (delta < 5.0) return MoveClass::Good;
    if (delta < 10.0) return MoveClass::Inaccuracy;
    if (delta < 15.0) return MoveClass::Mistake;
    return MoveClass::Blunder;
}

Judgement judge(const ai::Score& best, const ai::Score& played, bool playedIsBest) {
    Judgement j;
    j.wBest = winPercent(best);
    j.wPlayed = winPercent(played);
    j.delta = std::max(0.0, j.wBest - j.wPlayed);
    // Lichess Advice.scala: mate advice first, then the win-chance delta.
    if (mates(best) && !mates(played)) {
        j.mate = MateChange::Lost;
        if (mated(played)) j.cls = MoveClass::Blunder;
        else j.cls = played.cp > 999 ? MoveClass::Inaccuracy : played.cp > 700 ? MoveClass::Mistake : MoveClass::Blunder;
        return j;
    }
    if (!mates(best) && !mated(best) && mated(played)) {
        j.mate = MateChange::Created;
        j.cls = best.cp < -999 ? MoveClass::Inaccuracy : best.cp < -700 ? MoveClass::Mistake : MoveClass::Blunder;
        return j;
    }
    if (mates(best) && mates(played) && mateMoves(played) > mateMoves(best)) j.mate = MateChange::Delayed;
    j.cls = classifyDelta(j.delta, playedIsBest);
    return j;
}

const char* exTypeName(ExType t) {
    switch (t) {
    case ExType::None: return "none";
    case ExType::MateAllowed: return "mate_allowed";
    case ExType::MateMissed: return "mate_missed";
    case ExType::Stalemate: return "stalemate";
    case ExType::Fork: return "fork";
    case ExType::Discovered: return "discovered";
    case ExType::Skewer: return "skewer";
    case ExType::Pin: return "pin";
    case ExType::Trapped: return "trapped";
    case ExType::BackRank: return "back_rank";
    case ExType::Hanging: return "hanging";
    case ExType::Exchange: return "exchange";
    case ExType::MissedCapture: return "missed_capture";
    case ExType::MissedFork: return "missed_fork";
    case ExType::PromotionRace: return "promotion";
    case ExType::KingSafety: return "king_safety";
    case ExType::BadTrade: return "bad_trade";
    case ExType::Opening: return "opening";
    case ExType::Endgame: return "endgame";
    case ExType::Positional: return "positional";
    case ExType::Material: return "material";
    }
    return "none";
}

Band band(int level) {
    level = std::clamp(level, 1, 6);
    // level, full demo, demo, line, mate line, missed mate, lookahead, sentences, remarks/10, praise
    // every, offer cap, pause. Beginners see the whole sequence (a fork, the escape and the capture:
    // three plies, five with a check and a recapture; a mate in two at level 1, in three at levels 2-3);
    // from level 4 the demonstration stops where the problem stands (a mate in three: the four plies
    // before the mate).
    // The conversion slope last: levels 1 and 2 (750 and 1000 Elo) take the lowest band measured.
    static const Band kBands[6] = {
        {1, true, 5, 1, 3, 1, 2, 3, 5, 3, -1, 0.80f, 0.14},
        {2, true, 7, 3, 5, 2, 3, 3, 4, 4, -1, 0.60f, 0.14},
        {3, true, 7, 3, 5, 2, 4, 4, 3, 5, -1, 0.50f, 0.17},
        {4, false, 4, 4, 7, 3, 5, 4, 3, 6, 5, 0.45f, 0.20},
        {5, false, 4, 6, 7, 4, 7, 3, 2, 8, 3, 0.35f, 0.29},
        {6, false, 4, 8, 7, 5, 9, 2, 2, 8, 2, 0.35f, 0.36},
    };
    return kBands[level - 1];
}

double levelWinPercent(double w, const Band& b) {
    if (w <= 0.0 || w >= 100.0) return w;
    const double x = std::log(w / (100.0 - w)) * (b.conversion / 0.368208);
    return 100.0 / (1.0 + std::exp(-x));
}

// ---- Beat helpers (review_internal.h) ----------------------------------------------------------------

namespace detail {

std::string bandKey(const std::string& family, int level) { return family + ".b" + std::to_string(level); }

Beat sayBeat(const std::string& key, Look look, int ply, Priority pr) {
    Beat b;
    b.kind = BeatKind::Say;
    b.line.key = key;
    b.look = look;
    b.ply = ply;
    b.priority = pr;
    return b;
}

Arg pieceArg(const Position& p, Square s, Color listener) {
    const Piece pc = p.at(s);
    return Arg::ofPiece(pc.type, pc.color, pc.color == listener, s);
}

Arg moveArg(const Position& before, const Move& m) { return Arg::ofMove(before.toSAN(m), before.toUCI(m)); }
Arg moveArg(const LineStep& s) { return Arg::ofMove(s.san, s.uci); }

namespace {
Gesture gesture(GestureKind k, Square s, const std::string& anchor) {
    Gesture g;
    g.kind = k;
    g.square = s;
    g.anchor = anchor;
    return g;
}
bool isPointing(GestureKind k) {
    return k == GestureKind::PointPiece || k == GestureKind::PointSquare || k == GestureKind::Trace;
}
}  // namespace

void markSquare(Beat& b, Square s, const std::string& anchor) {
    if (s == NoSquare) return;
    Mark m;
    m.kind = Mark::Kind::Square;
    m.square = s;
    m.anchor = anchor;
    b.marks.push_back(m);
}

void markPiece(Beat& b, Square s, const std::string& anchor) {
    if (s == NoSquare) return;
    Mark m;
    m.kind = Mark::Kind::Piece;
    m.square = s;
    m.anchor = anchor;
    b.marks.push_back(m);
}

void markArrow(Beat& b, PieceType t, Square from, Square to, const std::string& anchor) {
    if (from == NoSquare || to == NoSquare) return;
    Mark m;
    m.kind = Mark::Kind::Arrow;
    m.from = from;
    m.to = to;
    m.via = t == Knight ? knightCorner(from, to) : NoSquare;
    m.anchor = anchor;
    b.marks.push_back(m);
}

void pointPiece(Beat& b, Square s, const std::string& anchor, bool emphasis) {
    if (s == NoSquare) return;
    Gesture g = gesture(GestureKind::PointPiece, s, anchor);
    g.emphasis = emphasis;
    b.gestures.push_back(g);
    markPiece(b, s, anchor);
}

void pointSquare(Beat& b, Square s, const std::string& anchor) {
    if (s == NoSquare) return;
    b.gestures.push_back(gesture(GestureKind::PointSquare, s, anchor));
    markSquare(b, s, anchor);
}

void traceMove(Beat& b, PieceType t, Square from, Square to, const std::string& anchor) {
    if (from == NoSquare || to == NoSquare) return;
    Gesture g = gesture(GestureKind::Trace, to, anchor);
    g.path = tracePath(t, from, to);
    b.gestures.push_back(g);
    markArrow(b, t, from, to, anchor);
}

void limitPointing(Beat& b) {
    int n = 0;
    std::vector<Gesture> kept;
    for (const Gesture& g : b.gestures) {
        if (isPointing(g.kind) && ++n > 3) continue;   // its mark stays: a highlight without the finger
        kept.push_back(g);
    }
    b.gestures.swap(kept);
}

int Ctx::lossAfter(int plies) const { return heldLoss(r, size_t(std::max(0, plies)), base, human, rEnd); }

int Ctx::followable(int want, bool extend) const {
    return int(followablePlies(r, size_t(std::max(0, want)), extend));
}

int Ctx::followableBest(int want) const {
    return std::max(std::min(want, 1), int(followablePlies(best, size_t(std::max(0, want)), true)));
}

int Ctx::forcingPlies(int plies) const {
    plies = std::min(plies, int(r.size()));
    for (int i = 0; i < plies; ++i) {
        const LineStep& st = r[size_t(i)];
        if (st.mover == coach && !st.check && st.captured == NoPiece && st.promotion == NoPiece) return 0;
    }
    // Where it ends, the human takes nothing back (a take-back at the end of a long line, and what
    // it costs in turn, would be one more thing to show).
    Position q = p1;
    for (int i = 0; i < plies; ++i) q.makeMove(r[size_t(i)].move);
    if (q.sideToMove() == human && bestCapturePoints(q, human) > 0) return 0;
    return level == 1 ? 9 : 11;
}

int Ctx::lossWithin(int plies) const {
    int worst = 0;
    const int n = std::min<int>(plies, int(r.size()));
    for (int i = 1; i <= n; ++i) worst = std::max(worst, lossAfter(i));
    return worst;
}

bool Ctx::lossExplains(int lost, bool pawn) const {
    // A piece for two pawns is a loss when the best move loses nothing; when it loses something too,
    // the move must lose two points more (one for a pawn, when the best move loses no more than a
    // pawn: with a piece gone either way, "you've lost 4 points" for a pawn more says too much).
    const int need = bestLoss == 0 || (pawn && bestLoss <= 1) ? 1 : 2;
    if (lost < need || lost - bestLoss < need) return false;
    // A pawn explains a drop of up to 2.5 pawns; a piece for a pawn (or the exchange) up to 8: the
    // engine counts a minor piece well above three pawns, more so with the position it leaves.
    return lost >= 3 || (lost >= 2 ? 400 : 250) * lost >= dropCp;
}

bool Ctx::losesMaterial(int plies) const {
    if (playedLoss > bestLoss) return true;
    const int n = std::min<int>(plies, int(r.size()));
    for (int i = 1; i <= n; ++i)
        if (lossAfter(i) > bestLoss && lossAfter(std::min(int(r.size()), i + 4)) > bestLoss) return true;
    return false;
}

bool Ctx::concreteLoss(int plies) const {
    if (lossWithin(plies) - bestLoss >= 2 || j.mate == MateChange::Created) return true;
    for (int i = 0; i < int(r.size()) && i < plies; ++i)
        if (r[size_t(i)].mate && r[size_t(i)].mover == coach) return true;
    return false;
}

int heldLoss(const std::vector<LineStep>& line, size_t plies, int base, Color pov, const Position& end) {
    const size_t n = std::min(plies, line.size());
    if (n == 0) return 0;
    int l = base - line[n - 1].balance;
    if (n < line.size()) {
        // pov's capture on the next ply, taken back on the one after, is an exchange: counted whole.
        size_t m = n;
        if (line[n].mover == pov && line[n].captured != NoPiece && n + 1 < line.size() &&
            line[n + 1].captured != NoPiece && line[n + 1].move.to == line[n].move.to)
            m = n + 1;
        l = std::min(l, base - line[m].balance);
    } else if (end.sideToMove() == pov) {
        l -= bestCapturePoints(end, pov);
    }
    return l;
}

int heldGain(const std::vector<LineStep>& line, size_t plies, int base, Color pov, const Position& end) {
    const size_t n = std::min(plies, line.size());
    if (n == 0) return 0;
    int g = line[n - 1].balance - base;
    if (n < line.size()) g = std::min(g, line[n].balance - base);
    else if (end.sideToMove() != pov) g -= bestCapturePoints(end, opposite(pov));
    return g;
}

Position lineEnd(const Position& start, const std::vector<LineStep>& line) {
    Position q = start;
    for (const LineStep& st : line) q.makeMove(st.move);
    return q;
}

int guessLossBound(const ai::Score& best, const ai::Score& played) {
    // whiteCp(s, true): the side to move's centipawns, mates as +-1000, clamped.
    const int b = whiteCp(best, true), p = whiteCp(played, true);
    return (std::max({0, b - p, -p}) + 50) / 100 + 2;
}

void extendRefutation(std::vector<LineStep>& r, const Position& p1, Color human, size_t want, int base, int maxLoss) {
    Position q = lineEnd(p1, r);
    for (int added = 0; added < 3 && r.size() < want; ++added) {
        if (!r.empty() && (r.back().mate || r.back().stalemate)) return;
        Move pick;
        if (q.sideToMove() == human) {
            // Take back on the square the coach just took on, with the least valuable piece that
            // does not lose by it.
            if (r.empty() || r.back().mover == human || r.back().captured == NoPiece) return;
            const Square s = r.back().move.to;
            for (const Move& m : q.legalMoves()) {
                if (m.to != s || (m.promotion != NoPiece && m.promotion != Queen) || seePoints(q, m) < 0) continue;
                if (!pick.valid() || q.at(m.from).type < q.at(pick.from).type) pick = m;
            }
        } else {
            // The coach's capture that wins the most, 2 points at least.
            int most = 1;
            for (const Move& m : q.legalMoves()) {
                if (q.at(m.to).empty() || (m.promotion != NoPiece && m.promotion != Queen)) continue;
                const int g = seePoints(q, m);
                if (g > most) {
                    most = g;
                    pick = m;
                }
            }
        }
        if (!pick.valid()) return;
        std::vector<LineStep> step = replayLine(q, {q.toUCI(pick)}, human, 1);
        if (step.empty()) return;
        Position next = q;
        next.makeMove(pick);
        // A capture that leaves the human more down than the engine's score allows is not what the
        // engine sees: the human had better than the recapture guessed before it (a bigger piece to
        // save, a counter-attack), or the capture does not work. Neither is guessed.
        if (step[0].mover != human && base - step[0].balance - bestCapturePoints(next, human) > maxLoss) {
            if (!r.empty() && r.back().guessed && r.back().mover == human) r.pop_back();
            return;
        }
        step[0].guessed = true;
        r.push_back(step[0]);
        q = next;
    }
}

namespace {

Line narration(const Ctx& c, const Position& before, const LineStep& st, int i) {
    Line l;
    // The coach's in-between move says why the bigger prize waits ({t1}: it is taken next).
    const bool between = st.mover == c.coach && isInBetween(c.r, size_t(i));
    if (c.level >= 4) {
        l.key = between ? "demo.first" : "demo.move";
        l.with("move", moveArg(st));
        if (between) l.with("t1", pieceArg(before, st.prize, c.human));
        return l;
    }
    const Square capSq = (st.move.flags & MoveEnPassant) ? Square(st.move.to + (st.mover == White ? -8 : 8)) : st.move.to;
    if (between) {
        // "My queen is attacked, so it takes your bishop first" (the piece would be lost anyway),
        // "my knight takes your rook, with check: your queen can wait", "first, check with my rook",
        // "first, my bishop goes to f5".
        l.with("my", pieceArg(before, st.move.from, c.human)).with("sq", Arg::ofSquare(st.move.to));
        l.with("t1", pieceArg(before, st.prize, c.human));
        if (st.captured == NoPiece) {
            l.key = st.check ? "demo.my.first.check_move" : "demo.my.first.move";
        } else {
            l.with("your", Arg::ofPiece(st.captured, c.human, true, capSq));
            const bool attacked = seeSquarePoints(before, st.move.from, c.human) > 0;
            l.key = attacked ? "demo.my.first.attacked" : st.check ? "demo.my.first.check" : "demo.my.first.take";
        }
        return l;
    }
    if (st.mover == c.coach) {
        l.with("my", pieceArg(before, st.move.from, c.human)).with("sq", Arg::ofSquare(st.move.to));
        if (st.mate) l.key = "demo.my.mate";
        else if (st.captured != NoPiece) {
            l.key = "demo.my.take";
            l.with("your", Arg::ofPiece(st.captured, c.human, true, capSq));
        } else if (st.check) l.key = "demo.my.check";
        else l.key = "demo.my.move";
    } else {
        l.with("your", pieceArg(before, st.move.from, c.human)).with("sq", Arg::ofSquare(st.move.to));
        // "Your king has to move": only when nothing else gets out of the check.
        bool kingOnly = before.inCheck() && st.piece == King && st.captured == NoPiece;
        for (const Move& m : kingOnly ? before.legalMoves() : std::vector<Move>())
            if (before.at(m.from).type != King) kingOnly = false;
        if (kingOnly) l.key = "demo.your.king";
        else if (st.captured != NoPiece && i > 0 && c.r[size_t(i - 1)].move.to == st.move.to) l.key = "demo.your.back";
        else if (st.captured != NoPiece) {
            l.key = "demo.your.take";
            l.with("my", Arg::ofPiece(st.captured, c.coach, false, capSq));
        } else l.key = "demo.your.move";
    }
    return l;
}

}  // namespace

namespace {

// The plies of c.r before the first move of the human's that promotes (the table has no spare queen
// for the human: a demonstration never goes past it), the whole line when none does.
int beforeHumanPromotion(const Ctx& c) {
    for (int i = 0; i < int(c.r.size()); ++i)
        if (c.r[size_t(i)].mover == c.human && c.r[size_t(i)].promotion != NoPiece) return i;
    return int(c.r.size());
}

Position afterPlies(const Ctx& c, int plies) {
    Position q = c.p1;
    for (int i = 0; i < plies && i < int(c.r.size()); ++i) q.makeMove(c.r[size_t(i)].move);
    return q;
}

// Whether the human's piece on 'sq' after the first 'from' plies of c.r is traded where the coach
// takes it later along the line (tradedFor: Qxf8+ Qxf8 Bxf8, the queens traded).
bool tradedLater(const Ctx& c, int from, Square sq) {
    for (int i = from; i < int(c.r.size()); ++i) {
        const LineStep& st = c.r[size_t(i)];
        if (st.mover == c.human && st.move.from == sq) sq = st.move.to;
        else if (st.mover == c.coach && st.captured != NoPiece && st.move.to == sq)
            return tradedFor(c, i) >= points(st.captured);
    }
    return false;
}

// Whether the side to move in 'p' can legally take on 'sq' (a pinned knight cannot take back).
bool legalCaptureOn(const Position& p, Square sq) {
    for (const Move& m : p.legalMoves())
        if (m.to == sq && (m.flags & MoveCapture)) return true;
    return false;
}

// Whether the piece on 'sq' in 'p' (its side to move) could be won where it stands: with the turn
// passed, a legal capture there wins material. A defender behind the attacker counts (Rf3xf1 Rxf1
// through f3 wins nothing), and an attacker pinned to its king takes nothing.
bool wonWhereItStood(const Position& p, Square sq) {
    Position q = p;
    if (!q.passTurn()) return false;
    for (const Move& m : q.legalMoves())
        if (m.to == sq && (m.flags & MoveCapture) && seePoints(q, m) > 0) return true;
    return false;
}

// Whether the human's piece that stands on 'sq' after the move survives the first plies of the move's
// own line (followed as it moves): a piece saved from one attack and lost to the next was not saved.
// Taken there and taken back as much at once, it is traded, and counts as kept.
bool pieceKept(const Ctx& c, Square sq) {
    for (size_t i = 1; i < c.playedLine.size() && i <= 6; ++i) {
        const LineStep& st = c.playedLine[i];
        if (st.mover == c.human) {
            if (st.move.from == sq) sq = st.move.to;
            continue;
        }
        if (st.captured == NoPiece || st.move.to != sq) continue;
        return i + 1 < c.playedLine.size() && c.playedLine[i + 1].mover == c.human &&
               c.playedLine[i + 1].move.to == st.move.to && points(c.playedLine[i + 1].captured) >= points(st.captured);
    }
    return true;
}

// The human's pieces among ex.targets that stand at the position after 'plies' plies of c.r, the
// king aside (a check is said by the detector's own lines), nor one traded later along the line
// ("now your queen can't be saved" when the queens are traded).
std::vector<Square> standingTargets(const Ctx& c, const Explanation& ex, const Position& q, int plies) {
    std::vector<Square> out;
    for (Square t : ex.targets) {
        const Piece pc = q.at(t);
        if (!pc.empty() && pc.color == c.human && pc.type != King && out.size() < 2 && !tradedLater(c, plies, t))
            out.push_back(t);
    }
    return out;
}

// The coach points at the problem where a demonstration stops before the last capture: the mate
// that comes next, else the pieces that fall (ex.targets), said with the pieces where they stand.
bool designation(const Ctx& c, const Explanation& ex, const Position& q, Beat& out) {
    const int k = ex.demoPlies;
    if (ex.mate) {
        if (k >= int(c.r.size()) || !c.r[size_t(k)].mate || c.r[size_t(k)].mover != c.coach) return false;
        const LineStep& m = c.r[size_t(k)];
        out = sayBeat("ex.point.mate", Look::Target, c.ply);
        out.line.with("reply", moveArg(m));
        traceMove(out, m.piece, m.move.from, m.move.to, "reply");
        return true;
    }
    const std::vector<Square> ts = standingTargets(c, ex, q, k);
    if (ts.empty()) return false;
    // "Can't be saved" with the human to move: only for what the line still costs from here. A piece
    // the coach takes while the human wins as much meanwhile (Nxe5 dxe5 Bxb5, a pawn lost in all)
    // could move away.
    int cheapest = points(Queen);
    for (Square t : ts) cheapest = std::min(cheapest, points(q.at(t).type));
    if (q.sideToMove() == c.human && ex.lost - (c.base - materialBalance(q, c.human)) < std::min(2, cheapest))
        return false;
    out = sayBeat(ts.size() >= 2 ? "ex.point.two" : "ex.point.piece", Look::Target, c.ply);
    out.line.with("t1", pieceArg(q, ts[0], c.human));
    pointPiece(out, ts[0], "t1", true);
    if (ts.size() >= 2) {
        out.line.with("t2", pieceArg(q, ts[1], c.human));
        pointPiece(out, ts[1], "t2", true);
    }
    return true;
}

}  // namespace

void planDemo(const Ctx& c, Explanation& ex) {
    ex.demoPlies = 0;
    ex.full = false;
    if (ex.missed || c.r.empty() || c.r[0].mover != c.coach || ex.fullPlies <= 0) return;
    // Never past a move the player could not follow (Ctx::followable).
    const int reach = std::min({int(c.r.size()), beforeHumanPromotion(c), c.followable(int(c.r.size()), true)});
    // Levels 1-3: the whole sequence, when the level can follow it.
    const int fullCap = std::max(ex.mate ? c.b.mateLinePlies : c.b.demoPlies, c.forcingPlies(ex.fullPlies));
    if (c.b.fullDemo && ex.fullPlies <= std::min(reach, fullCap) && c.followable(ex.fullPlies, false) == ex.fullPlies) {
        ex.full = true;
        ex.demoPlies = ex.fullPlies;
        return;
    }
    // A mate in one is the problem itself: shown at every level.
    if (ex.mate && ex.fullPlies == 1 && reach >= 1) {
        ex.full = true;
        ex.demoPlies = 1;
        return;
    }
    // Up to where the problem stands, and the coach points at what comes next.
    if (ex.threatPlies <= 0 || ex.threatPlies >= ex.fullPlies || ex.threatPlies > std::min(reach, c.b.demoPlies) ||
        c.followable(ex.threatPlies, false) < ex.threatPlies)
        return;
    ex.demoPlies = ex.threatPlies;
    Beat b;
    if (ex.threatTail.empty() && !designation(c, ex, afterPlies(c, ex.threatPlies), b)) ex.demoPlies = 0;
}

// "If you take back with Qxd2, Qxh1 takes your rook": said where the sequence stops when the
// demonstration plays it to the end, else with the cause (the table then shows p1).
bool retakeBeat(const Ctx& c, const Explanation& ex, const Position& table, Beat& out) {
    if (!ex.retake.valid() || !ex.punish.valid() || ex.fullPlies <= 0) return false;
    const Position q = afterPlies(c, ex.fullPlies);
    Position after = q;
    after.makeMove(ex.retake);
    const Move& hit = ex.punish;
    const Square lostOn = (hit.flags & MoveEnPassant) ? Square(hit.to + (c.coach == White ? -8 : 8)) : hit.to;
    const Piece taker = q.at(ex.retake.from), hitter = after.at(hit.from), victim = after.at(lostOn);
    if (taker.empty() || taker.color != c.human || hitter.empty() || hitter.color != c.coach || victim.empty() ||
        victim.color != c.human)
        return false;
    // Where each piece stands on the table: the piece that takes back, before it does.
    const Square victimAt = lostOn == ex.retake.to ? ex.retake.from : lostOn;
    const auto there = [&](Square s, const Piece& pc) { return table.at(s) == pc ? s : NoSquare; };
    const Square takerOn = there(ex.retake.from, taker), hitterOn = there(hit.from, hitter),
                 victimOn = there(victimAt, victim);
    out = sayBeat(bandKey("ex.retake", c.level), Look::Target, c.ply);
    out.line.with("move", moveArg(q, ex.retake))
        .with("reply", moveArg(after, hit))
        .with("your2", Arg::ofPiece(taker.type, c.human, true, takerOn))
        .with("my", Arg::ofPiece(hitter.type, c.coach, false, hitterOn))
        .with("your", Arg::ofPiece(victim.type, c.human, true, victimOn));
    // Levels 1-2 name the pieces, from level 3 the moves.
    if (c.level <= 2) {
        pointPiece(out, takerOn, "your2");
        pointPiece(out, hitterOn, "my");
    } else {
        if (takerOn != NoSquare) traceMove(out, taker.type, ex.retake.from, ex.retake.to, "move");
        if (hitterOn != NoSquare) traceMove(out, hitter.type, hit.from, hit.to, "reply");
    }
    if (victimOn != takerOn) pointPiece(out, victimOn, "your", true);
    return true;
}

// A full demonstration, cut short neither by the band nor by the human's promotion.
bool demoReachesEnd(const Ctx& c, const Explanation& ex) {
    if (!ex.full || ex.demoPlies != ex.fullPlies || demoLength(c, ex) != ex.demoPlies) return false;
    for (int i = 0; i < ex.demoPlies && i < int(c.r.size()); ++i)
        if (c.r[size_t(i)].mover == c.human && c.r[size_t(i)].promotion != NoPiece) return false;
    return true;
}

int demoLength(const Ctx& c, const Explanation& ex) {
    const int cap = std::max({c.b.demoPlies, ex.mate ? c.b.mateLinePlies : 0, c.forcingPlies(ex.demoPlies)});
    return std::max(0, std::min({ex.demoPlies, int(c.r.size()), cap}));
}

void appendDemo(const Ctx& c, const Explanation& ex, Script& s) {
    int shown = 0;
    Position pos = c.p1;
    const int plies = demoLength(c, ex);
    bool stopped = false;
    for (int i = 0; i < plies; ++i) {
        const LineStep& st = c.r[size_t(i)];
        if (st.mover == c.human && st.promotion != NoPiece) {
            // The human's spare queen is out of the coach's reach: point at the square instead (the
            // lines name a queen; an underpromotion is only said as the pawn's move).
            Beat b = sayBeat(st.promotion == Queen ? "demo.promote" : "demo.your.move", Look::Target, c.ply);
            b.line.with("sq", Arg::ofSquare(st.move.to)).with("your", pieceArg(pos, st.move.from, c.human));
            pointSquare(b, st.move.to, "sq");
            s.push_back(b);
            stopped = true;
            break;
        }
        Beat d;
        d.kind = BeatKind::DemoMove;
        d.uci = st.uci;
        d.line = narration(c, pos, st, i);
        d.look = Look::Board;
        d.ply = c.ply;
        s.push_back(d);
        Beat pause;
        pause.kind = BeatKind::Pause;
        pause.seconds = c.b.demoPause;
        pause.look = Look::Board;
        pause.ply = c.ply;
        s.push_back(pause);
        pos.makeMove(st.move);
        ++shown;
        // A full demonstration says what stands on the board once the problem is there (the fork,
        // the pin), then plays on.
        if (ex.full && shown == ex.threatPlies && shown < plies)
            for (const Beat& t : ex.threatTail) s.push_back(t);
    }
    if (shown > 0 && !stopped && shown == ex.demoPlies) {
        if (ex.full) {
            Beat rb;
            if (retakeBeat(c, ex, pos, rb)) s.push_back(rb);
            // The end of the sequence: the detector's closing lines, else what the human has lost
            // (a single capture says it by itself: "I take your knight").
            if (!ex.tail.empty()) {
                for (const Beat& t : ex.tail) s.push_back(t);
            } else if (!ex.mate && shown >= 2 && ex.lost >= 1 && c.level <= 3) {
                // The human, to move, may still take something back: the board shows more than is
                // lost for good, and the line says so ("you can take back, but ...").
                const bool back = pos.sideToMove() == c.human && c.base - materialBalance(pos, c.human) > ex.lost;
                Beat b = sayBeat(bandKey(back ? "ex.result_back" : "ex.result", c.level), Look::Player, c.ply);
                b.line.with("pts", Arg::ofNumber(ex.lost));
                s.push_back(b);
            }
        } else if (!ex.threatTail.empty()) {
            for (const Beat& t : ex.threatTail) s.push_back(t);
        } else {
            Beat b;
            if (designation(c, ex, pos, b)) s.push_back(b);
        }
    }
    if (shown > 0) {
        Beat r;
        r.kind = BeatKind::Rewind;
        r.count = shown;
        r.look = Look::Board;
        r.ply = c.ply;
        r.skippable = false;   // a skipped rewind still runs, faster
        if (c.level <= 4) r.line.key = bandKey("ex.rewind", c.level);
        s.push_back(r);
    }
}

}  // namespace detail

// ---- Reviewer ------------------------------------------------------------------------------------

namespace detail {

std::string startFenOf(const Game& g) {
    const Position& s = g.startPosition();
    return s.isStandardStart() ? std::string() : s.fen();
}

ai::AnalysisRequest requestAt(const Game& g, size_t plies) {
    ai::AnalysisRequest r;
    r.startFen = startFenOf(g);
    std::vector<std::string> moves = g.uciMoves();
    moves.resize(std::min(plies, moves.size()));
    r.moves = moves;
    return r;
}

int whiteCp(const ai::Score& s, bool whiteToMove) {
    const int cp = mates(s) ? 1000 : mated(s) ? -1000 : std::clamp(s.cp, -1000, 1000);
    return whiteToMove ? cp : -cp;
}

Arg evalArg(const ai::Score& s) { return Arg::ofEval(s.cp, s.matesNow ? 1 : s.matedNow ? -1 : s.mate); }

}  // namespace detail

namespace {

// A synthetic line for a move that ends the game (the engine has nothing to search after it).
ai::PvLine terminalLine(const Position& p1, const std::string& uci) {
    ai::PvLine l;
    l.pv = {uci};
    if (p1.isCheckmate()) l.score.mate = 1;
    else l.score.cp = 0;
    return l;
}

// A mate allowed or missed is offered back whatever the level's count of takebacks
// (Band::offerCap), and taking it back does not use one.
bool mateOffer(ExType t) { return t == ExType::MateAllowed || t == ExType::MateMissed; }

// What follows from the refutation (Ctx::r): where it stops, the first ply after which the human is
// 2+ points down for good, the loss within the band's lookahead.
void refutationFacts(Ctx& c, int lookahead) {
    c.rEnd = lineEnd(c.p1, c.r);
    c.gain = -1;
    for (size_t i = 0; i < c.r.size(); ++i)
        if (c.lossAfter(int(i) + 1) >= 2) {
            c.gain = int(i) + 1;
            break;
        }
    c.loss = c.lossWithin(lookahead);
}

// A reason whose sequence the verification checks: what the move allows along the refutation, at
// least one ply of it. Not a chance missed (the best line, from p0), nor a forced mate or a
// stalemate, which the engine's own score establishes whatever the line shown.
// What the played move costs (win-% points) in the iteration of A0 at 'depth': -1 when that
// iteration does not tell (gone from A0's history, or the move out of its lines with no iteration of
// A1, its own search, at that depth).
double costAtDepth(const ai::Analysis& a0, const ai::Analysis* a1, const std::string& played, int depth) {
    const ai::DepthReport* d = nullptr;
    for (const ai::DepthReport& r : a0.history)
        if (r.depth == depth) d = &r;
    if (!d || d->lines.empty()) return -1.0;
    const double best = winPercent(d->lines.front().second);
    for (const auto& l : d->lines)
        if (l.first == played) return std::max(0.0, best - winPercent(l.second));
    if (a1)
        for (const ai::DepthReport& r : a1->history)
            if (r.depth == depth && !r.lines.empty() && r.lines.front().first == played)
                return std::max(0.0, best - winPercent(r.lines.front().second));
    return -1.0;
}

// What the human is down after the first j plies of a line from p0, read where it settles: a
// capture taken back on the next ply is a whole exchange, and so are the captures on that square
// that follow at once (exd4 exd4 Rxd4 Rxd4 Bxd4 Kxd4: read where it ends), and where the line
// stops, the human to move takes back what it can at once.
int settledAt(const Ctx& c, const std::vector<LineStep>& line, const Position& end, size_t j) {
    while (j < line.size() && line[j - 1].captured != NoPiece && line[j].captured != NoPiece &&
           line[j].move.to == line[j - 1].move.to)
        ++j;
    int l = c.base - line[j - 1].balance;
    if (j == line.size() && end.sideToMove() == c.human && end.hasLegalMove()) l -= bestCapturePoints(end, c.human);
    return l;
}

// ... and the least of that and the next four plies (a loss won back by then, an in-between check
// first, is none).
int settledFor(const Ctx& c, const std::vector<LineStep>& line, const Position& end, size_t i) {
    int l = settledAt(c, line, end, i);
    for (size_t j = i + 1; j <= std::min(line.size(), i + 4); ++j) l = std::min(l, settledAt(c, line, end, j));
    return l;
}

// What a line from p0 still loses at the end of its first 8 plies (Ctx::bestLoss for the best's).
int eightPlyLoss(const Ctx& c, const std::vector<LineStep>& line) {
    if (line.empty()) return 0;
    return std::max(0, settledFor(c, line, lineEnd(c.p0, line), std::min<size_t>(8, line.size())));
}

// The better move to show (point 4): the best, unless it is a quiet move a quick look would not
// find while another of A0's lines is as good (asGood, and the same expected result), loses no more
// material along its line, and is more natural for the player: the shallow search's move, else a
// capture. Null: the best itself.
const ai::PvLine* naturalBest(const Ctx& c, const ai::Analysis& a0, const ai::Analysis* shallow) {
    const ai::PvLine& best = a0.lines.front();
    const auto capture = [&](const std::string& uci) {
        const Move m = c.p0.parseUCI(uci);
        return m.valid() && (m.flags & MoveCapture);
    };
    const std::string quick = shallow && shallow->ok ? shallow->bestMove : std::string();
    if (capture(best.pv.front()) || best.pv.front() == quick) return nullptr;
    const Outcome bo = outcomeOf(best.score);
    const int bestLoss = eightPlyLoss(c, replayLine(c.p0, best.pv, c.human, 16));
    const auto same = [&](const ai::PvLine& l) {
        if (&l == &best || l.pv.empty() || l.pv.front() == c.playedUci || !asGood(l.score, best.score)) return false;
        const Outcome o = outcomeOf(l.score);
        if (o.wdl && bo.wdl && std::abs(o.e - bo.e) >= kFlatE) return false;
        return eightPlyLoss(c, replayLine(c.p0, l.pv, c.human, 16)) <= bestLoss;
    };
    if (!quick.empty())
        for (const ai::PvLine& l : a0.lines)
            if (same(l) && l.pv.front() == quick) return &l;
    for (const ai::PvLine& l : a0.lines)
        if (same(l) && capture(l.pv.front())) return &l;
    return nullptr;
}

bool checkable(const Explanation& ex) {
    return ex.type != ExType::None && !ex.missed && !ex.mate && ex.type != ExType::Stalemate && ex.fullPlies >= 1;
}

// What the verification did to a reason (ReviewInput::verified).
struct Checked {
    int plies = -1;          // plies of the line confirmed move by move (-1: not checked)
    bool changed = false;    // the line is no longer the engine's as it came
    int reduced = 0;         // 0 the reason first found, 1 a smaller one, 2 none
    std::string next;        // a position the check still needs searched
    std::vector<std::string> log;   // Review::verifyLog
};

// The reason's sequence confirmed move by move against the searches of 'v' (verifyLine): the coach's
// moves as good as the engine's there, the player's the best defences. A coach move found weaker
// after a deeper comparison gives way to the better one, and the line goes on with it; a defence
// the player had that loses less than the reason says, as good as the line's move in a deeper
// comparison too, becomes the line. Each time the reason is looked for again on the new line: it
// may say less, or nothing. A sequence whose searches are not all there (Checked::next) keeps no
// reason, nor does one that keeps changing. 'deep': the plies the refutation's own search saw
// deeper than a short search (their coach moves stay).
Checked verifyReason(Ctx& c, Explanation& ex, bool& found, const std::map<std::string, ai::Analysis>& v,
                     const ai::Analysis* first, int lookahead, int deep, int guessBound) {
    Checked out;
    const Explanation said = ex;
    // After a change at ply 'at', the comparison's own line: its first plies as deep as the engine's.
    const int compareDeep = std::max(0, Reviewer::kCompareDepth - Reviewer::kVerifyDepth - 2);
    const auto relook = [&]() {
        extendRefutation(c.r, c.p1, c.human, size_t(lookahead + 1), c.base, guessBound);
        refutationFacts(c, lookahead);
        Explanation again;
        found = findExplanation(c, again);
        ex = found ? again : Explanation{};
    };
    bool settled = false;
    int changedFrom = 0;   // the first ply of the line the verification changed (0: none)
    for (int round = 0; round < 8 && !settled; ++round) {
        if (!found || !checkable(ex)) {
            if (round > 0) out.log.push_back(found ? std::string("then ") + exTypeName(ex.type) : "then no reason");
            settled = true;
            break;
        }
        const int want = std::min<int>(int(c.r.size()), ex.fullPlies + 1);
        const int horizon = std::max(want + 2, lookahead + 1);
        const int limit = ex.lost - (ex.lost >= 2 ? 1 : 0);
        // The loss the reason says must stand along its own line (lineLoss): at least half of it, a
        // line that wins most of it back (Qxc5 Bd4+ Qxd4 Rxd4, Nxa1 Nxc7+ Kf8 Nxa8) losing nothing to
        // speak of; on a line the verification changed (a better coach move, a defence that loses
        // less), all of it from the change on, as a defence's must to be taken.
        const int held = lineLoss(c, c.r, std::max(1, changedFrom), std::max(horizon, changedFrom));
        if (changedFrom > 0 ? held < limit : 2 * held < ex.lost) {
            out.log.push_back((round > 0 ? std::string("then ") : std::string()) + exTypeName(ex.type) + " lost " +
                              std::to_string(ex.lost) + ", held " + std::to_string(held) + " from ply " +
                              std::to_string(std::max(1, changedFrom)));
            found = false;
            ex = Explanation{};
            settled = true;
            break;
        }
        const Verification vr = verifyLine(c, c.r, c.p1, v, first, want, deep);
        out.plies = vr.plies;
        std::string note = std::string(exTypeName(ex.type)) + " lost " + std::to_string(ex.lost) + ": " +
                           std::to_string(vr.plies) + "/" + std::to_string(want) + " plies";
        for (const std::string& n : vr.notes) note += ", " + n;
        if (vr.changed) {
            out.log.push_back(note);
            out.changed = true;
            changedFrom = vr.changedAt + 1;
            deep = vr.changedAt + 1 + compareDeep;
            relook();
            continue;
        }
        if (vr.plies < want) {   // not confirmed to its end
            out.log.push_back(note + (vr.next.empty() ? ", a search failed" : ", to search: " + vr.next));
            out.next = vr.next;
            break;
        }
        // The player's defences that lose less than the reason says (a point less for two or more),
        // within the sequence and two plies more (or the band's lookahead), the least loss first:
        // each compared more deeply with the line's move. One as good there, that still loses less
        // along the comparison's line, becomes the line.
        std::vector<std::pair<int, const Defence*>> lesser;
        for (const Defence& d : vr.others) {
            const int l = lineLoss(c, d.line, d.at + 1, horizon);
            if (l < limit) lesser.emplace_back(l, &d);
        }
        std::stable_sort(lesser.begin(), lesser.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        const Defence* escape = nullptr;
        std::vector<LineStep> escapeLine;
        int escapeLoss = 0;
        bool waiting = false;
        for (const auto& [l, d] : lesser) {
            const Position at = afterPlies(c, d->at);
            const std::string fen = at.fen();
            const std::string &kept = c.r[size_t(d->at)].uci, &alt = d->line[size_t(d->at)].uci;
            const ai::PvLine *la = nullptr, *lk = nullptr;
            if (!compared(v, fen, alt, kept, la, lk)) {
                if (v.count(compareKey(fen, alt, kept)) == 0) {
                    out.next = compareKey(fen, alt, kept);
                    waiting = true;
                    break;
                }
                continue;   // the comparison failed: no escape shown by it
            }
            if (!asGood(la->score, lk->score)) continue;   // deeper, the defence is worse
            std::vector<LineStep> line(c.r.begin(), c.r.begin() + d->at);
            const std::vector<LineStep> tail = replayLine(at, la->pv, c.human, 16);
            line.insert(line.end(), tail.begin(), tail.end());
            const int deeper = lineLoss(c, line, d->at + 1, horizon);
            if (deeper >= limit) continue;   // deeper, it loses as much
            escape = d;
            escapeLine = std::move(line);
            escapeLoss = deeper;
            break;
        }
        if (waiting) {
            out.log.push_back(note + ", to compare: " + out.next);
            break;
        }
        if (!escape) {
            out.log.push_back(note + (lesser.empty() ? ", confirmed" : ", confirmed (" + std::to_string(lesser.size()) +
                                                                          " defences compared)"));
            settled = true;
            break;
        }
        out.log.push_back(note + ", ply " + std::to_string(escape->at + 1) + " " +
                          escapeLine[size_t(escape->at)].uci + " instead of " + c.r[size_t(escape->at)].uci +
                          " loses " + std::to_string(escapeLoss) + " @ " + afterPlies(c, escape->at).fen());
        deep = escape->at + 1 + compareDeep;
        c.r = std::move(escapeLine);
        out.changed = true;
        changedFrom = escape->at + 1;
        relook();
    }
    if (!settled) {
        if (!out.log.empty() && out.next.empty()) out.log.push_back("no settled line");
        found = false;
        ex = Explanation{};
    }
    if (!found) out.reduced = 2;
    else if (ex.type != said.type || ex.lost < said.lost || ex.mate != said.mate) out.reduced = 1;
    return out;
}

}  // namespace

void Reviewer::reset(int level, Color human, bool offersEnabled) {
    *this = Reviewer();
    level_ = std::clamp(level, 1, 6);
    human_ = human;
    offersEnabled_ = offersEnabled;
}

bool Reviewer::remarkAllowed() const {
    int n = 0;
    for (int m : remarks_)
        if (humanMoves_ - m < 10) ++n;
    return n < band(level_).remarksPer10;
}

void Reviewer::noteRemark() {
    remarks_.push_back(humanMoves_);
    while (!remarks_.empty() && humanMoves_ - remarks_.front() >= 10) remarks_.pop_front();
}

ai::AnalysisRequest Reviewer::beforeRequest(const Game& g) const {
    ai::AnalysisRequest r = requestAt(g, g.moves().size());
    r.multiPV = 3;
    r.depth = level_ <= 3 ? 16 : 18;
    r.moveTimeMs = 2500;
    return r;
}

bool Reviewer::needsPlayedRequest(const ai::Analysis& before, const std::string& playedUci) {
    if (!before.ok || before.noLegalMove) return false;
    const ai::PvLine* l = before.line(playedUci);
    // The best line is judged against itself whatever its bound (review()): no re-score for it.
    return l == nullptr || (l->score.bound != ai::Score::Bound::Exact && l != &before.lines.front());
}

ai::AnalysisRequest Reviewer::playedRequest(const Game& g, const ai::Analysis& before) const {
    const size_t n = g.moves().size();
    ai::AnalysisRequest r = requestAt(g, n > 0 ? n - 1 : 0);
    r.multiPV = 1;
    r.depth = before.depth > 0 ? before.depth : 16;
    r.moveTimeMs = 1500;
    if (n > 0) r.searchMoves = {g.positionAt(n - 1).toUCI(g.moves()[n - 1])};
    return r;
}

ai::AnalysisRequest Reviewer::afterRequest(const Game& g) const {
    ai::AnalysisRequest r = requestAt(g, g.moves().size());
    r.multiPV = 2;
    r.depth = 14;
    r.moveTimeMs = 1500;
    return r;
}

ai::AnalysisRequest Reviewer::verifyRequest(const std::string& key) {
    ai::AnalysisRequest r;
    const size_t bar = key.find(" | ");
    r.startFen = key.substr(0, bar);
    if (bar == std::string::npos) {   // the short search of every move
        r.multiPV = 3;
        r.depth = kVerifyDepth;
        r.moveTimeMs = 1000;
        return r;
    }
    std::istringstream moves(key.substr(bar + 3));   // the comparison of two moves
    std::string m;
    while (moves >> m) r.searchMoves.push_back(m);
    r.multiPV = 2;
    r.depth = kCompareDepth;
    r.moveTimeMs = 2500;
    return r;
}

ai::AnalysisRequest Reviewer::shallowRequest(const Game& g) const {
    ai::AnalysisRequest r = requestAt(g, g.moves().size());
    r.multiPV = 1;
    r.depth = 6;
    r.moveTimeMs = 0;
    return r;
}

Script Reviewer::announce(const Game& g) {
    Script s;
    if (g.moves().empty()) return s;
    const int ply = int(g.moves().size()) - 1;
    const Position& p = g.position();
    const Color mover = g.positionAt(size_t(ply)).sideToMove();
    const bool byHuman = mover == human_;
    if (p.isCheckmate()) {
        Beat b = sayBeat(byHuman ? (level_ <= 3 ? "ann.mate.human.b1" : "ann.mate.human")
                                 : (level_ <= 2 ? "ann.mate.coach.b1" : "ann.mate.coach"),
                         Look::Player, ply, Priority::Urgent);
        if (byHuman) b.gestures.push_back(Gesture{GestureKind::Nod, NoSquare, {}, "", 0.0f, false});
        s.push_back(b);
        return s;
    }
    if (!p.inCheck()) return s;
    if (level_ >= 2 && squareCount(p.checkers()) >= 2) {
        s.push_back(sayBeat(byHuman ? "ann.double_check.human" : "ann.double_check.coach", Look::Board, ply,
                            Priority::Urgent));
        return s;
    }
    if (byHuman) {
        s.push_back(sayBeat(level_ <= 2 ? "ann.check.human.b1" : "ann.check.human", Look::Board, ply, Priority::Urgent));
        return s;
    }
    s.push_back(sayBeat("ann.check.coach", Look::Board, ply, Priority::Urgent));
    if (level_ <= 2 && !coachCheckExplained_) {
        coachCheckExplained_ = true;
        Beat b = sayBeat("ann.check.coach.first", Look::Target, ply, Priority::Urgent);
        const Square k = p.kingSquare(human_);
        b.line.with("your", pieceArg(p, k, human_));
        pointPiece(b, k, "your", true);
        s.push_back(b);
    }
    return s;
}

Review Reviewer::review(const ReviewInput& in) {
    Review out;
    if (!in.game || in.game->moves().empty()) return out;
    const Game& g = *in.game;
    const Band bd = band(level_);

    Ctx c;
    c.b = bd;
    c.level = level_;
    c.human = human_;
    c.coach = opposite(human_);
    c.g = &g;
    c.ply = int(g.moves().size()) - 1;
    c.p0 = g.positionAt(size_t(c.ply));
    c.p1 = g.position();
    c.played = g.moves()[size_t(c.ply)];
    c.playedUci = c.p0.toUCI(c.played);
    c.playedSan = g.sanMoves()[size_t(c.ply)];
    c.f = analyzeMove(c.p0, c.played);
    c.base = materialBalance(c.p0, human_);
    // Moves taken back (an offer accepted or not) leave the W% history.
    humanW_.resize(size_t(c.ply), -1.0);

    PlyVerdict& v = out.verdict;
    v.ply = c.ply;
    v.mover = c.p0.sideToMove();
    v.human = true;
    v.uci = c.playedUci;
    v.san = c.playedSan;
    v.phase = uint8_t(phaseOfPly(dividePhases(g), c.ply));
    v.check = c.p1.inCheck();
    v.captured = points(c.f.captured);

    const bool retry = retryPending_ && retry_.ply == c.ply;
    retryPending_ = false;
    if (!retry) ++humanMoves_;
    if (retry) {
        out.isRetry = true;
        out.takeback = retry_;
        out.takeback.same = retry_.firstUci == c.playedUci;
    }

    // ---- Engine figures ----
    const ai::Analysis* a0 = (in.before && in.before->ok && !in.before->lines.empty()) ? in.before : nullptr;
    ai::PvLine terminal;
    if (a0) {
        c.l1 = &a0->lines[0];
        if (a0->lines.size() > 1) c.l2 = &a0->lines[1];
        c.lp = a0->line(c.playedUci);
        // A1 when A0 lacks the move, or holds only a bound for it (a stopped search): the move's
        // class must not come from a score the engine did not finish. Not for the best line itself:
        // the move is judged against the very score it is compared with (another search's score
        // would make the engine's own move a mate missed, or less than accurate).
        if ((!c.lp || (c.lp->score.bound != ai::Score::Bound::Exact && c.lp != c.l1)) && in.played && in.played->ok &&
            !in.played->lines.empty() && !in.played->lines[0].pv.empty() && in.played->lines[0].pv[0] == c.playedUci)
            c.lp = &in.played->lines[0];
        if (!c.lp && !c.p1.hasLegalMove()) {
            terminal = terminalLine(c.p1, c.playedUci);
            c.lp = &terminal;
        }
    }
    if (!c.l1 || c.l1->pv.empty() || !c.lp) {
        // No judgement without the engine: the announcements and the threat warnings are
        // rules-based and run anyway.
        if (retry) out.takeback.fixed = false;
        return out;
    }
    c.isBest = c.l1->pv[0] == c.playedUci;
    c.j = judge(c.l1->score, c.lp->score, c.isBest);
    c.a = assess(a0->lines, c.lp->score, c.isBest);
    out.assessment = c.a;
    const bool forced = c.p0.legalMoves().size() == 1;
    MoveClass cls = c.j.cls;
    if (forced) cls = MoveClass::Forced;
    else if (in.inBook && c.j.delta < 15.0) cls = MoveClass::Book;
    // A fault the search has not settled: the move about as good as the best (under kUnsettledCost)
    // at one of the two iterations before the last, or a search stopped below the verification's
    // depth. Said at most as a mistake, without a reason or a demonstration. A mate found stands.
    bool unsettled = false;
    if ((cls == MoveClass::Mistake || cls == MoveClass::Blunder) && c.j.mate == MateChange::None) {
        const ai::Analysis* a1 = in.played && in.played->ok ? in.played : nullptr;
        for (int k = 1; k <= 2; ++k) {
            const double cost = costAtDepth(*a0, a1, c.playedUci, a0->depth - k);
            out.costBefore[k - 1] = cost;
            if (cost >= 0.0 && cost < kUnsettledCost) unsettled = true;
        }
        if (a0->depth > 0 && a0->depth < kVerifyDepth) unsettled = true;
        if (unsettled && cls == MoveClass::Blunder) cls = MoveClass::Mistake;
    }
    out.unsettled = unsettled;

    v.cls = cls;
    v.wBest = c.j.wBest;
    v.wPlayed = c.j.wPlayed;
    v.delta = c.j.delta;
    v.accuracy = moveAccuracy(c.j.wBest, c.j.wPlayed);
    v.cpWhiteAfter = whiteCp(c.lp->score, c.p0.sideToMove() == White);
    v.hasEvalAfter = true;
    v.cpWhiteBefore = whiteCp(c.l1->score, c.p0.sideToMove() == White);
    v.hasEvalBefore = true;
    // A fault's better move, said and shown from here on: the most natural of the moves as good as
    // the best. What the move costs stays measured against the best (top): the class, the drop a
    // reason must explain, a mate hastened.
    const ai::PvLine* const top = c.l1;
    if ((cls == MoveClass::Inaccuracy || cls == MoveClass::Mistake || cls == MoveClass::Blunder) && !c.isBest)
        if (const ai::PvLine* natural = naturalBest(c, *a0, in.shallow)) c.l1 = natural;
    v.bestUci = c.l1->pv[0];
    {
        const Move bm = c.p0.parseUCI(v.bestUci);
        v.bestSan = bm.valid() ? c.p0.toSAN(bm) : std::string();
    }
    v.mateAllowed = c.j.mate == MateChange::Created;
    v.mateMissed = c.j.mate == MateChange::Lost;
    // Lost to a mate either way, but sooner after this move: Best by the numbers, never praised or highlighted.
    v.hastensMate = mated(c.lp->score) && mated(top->score) && mateMoves(c.lp->score) < mateMoves(top->score);

    // ---- Lines replayed on the board ----
    std::vector<std::string> refutation(c.lp->pv.begin() + (c.lp->pv.empty() ? 0 : 1), c.lp->pv.end());
    int refDepth = c.lp->depth - 1;   // the search depth left for the refutation's first move
    if (in.after && in.after->ok && !in.after->lines.empty()) {
        const std::vector<std::string>& pv = in.after->lines[0].pv;
        if (pv.size() > refutation.size() && (refutation.empty() || pv[0] == refutation[0])) {
            refutation = pv;
            refDepth = in.after->lines[0].depth;
        }
    }
    c.r = replayLine(c.p1, refutation, human_, 16);
    // The plies of the refutation its own search saw two plies deeper or more than a verification
    // search (kVerifyDepth) sees them: the verification keeps their moves. Not the replies guessed.
    const int deepPlies = std::clamp(refDepth - (kVerifyDepth + 2) + 1, 0, int(c.r.size()));
    const int guessBound = guessLossBound(top->score, c.lp->score);
    extendRefutation(c.r, c.p1, human_, size_t(bd.lookahead + 1), c.base, guessBound);
    c.best = replayLine(c.p0, c.l1->pv, human_, 16);
    c.playedLine = replayLine(c.p0, c.lp->pv, human_, 16);
    c.bestEnd = lineEnd(c.p0, c.best);
    c.playedEnd = lineEnd(c.p0, c.playedLine);
    refutationFacts(c, bd.lookahead);
    // What is still lost at the end of the best line's first 8 plies (settledFor), and the most the
    // move's own line loses over its first 9.
    c.bestLoss = eightPlyLoss(c, c.best);
    if (c.playedLine.size() >= 8) {
        c.playedLoss = 0;
        for (size_t i = 1; i <= 9 && i <= c.playedLine.size(); ++i)
            c.playedLoss = std::max(c.playedLoss, settledFor(c, c.playedLine, c.playedEnd, i));
    }
    // whiteCp(s, true): the side to move's (the human's) centipawns.
    c.dropCp = std::max(0, whiteCp(top->score, true) - whiteCp(c.lp->score, true));
    v.materialSwing = heldGain(c.playedLine, 3, c.base, human_, c.playedEnd);

    // ---- What to say ----
    Explanation ex;
    bool found = !unsettled && findExplanation(c, ex);
    Checked checked;
    if (in.verified && found && checkable(ex)) {
        Claim first;
        first.type = ex.type;
        first.lost = ex.lost;
        first.mate = ex.mate;
        first.plies = std::min(ex.fullPlies, int(c.r.size()));
        for (int i = 0; i < first.plies; ++i) first.line.push_back(c.r[size_t(i)].uci);
        const ai::Analysis* a2 = in.after && in.after->ok ? in.after : nullptr;
        checked = verifyReason(c, ex, found, *in.verified, a2, bd.lookahead, deepPlies, guessBound);
        out.verifyNext = checked.next;
        out.verifyLog = checked.log;
        if (checked.changed)
            for (const LineStep& st : c.r) out.refutation.push_back(st.uci);
        if (checked.reduced > 0) {
            first.verifiedPlies = checked.plies;
            first.lineChanged = checked.changed;
            first.reduced = checked.reduced;
            out.dropped = first;
        }
    }
    if (found) {
        v.exType = ex.type;
        planDemo(c, ex);
    }
    const bool topMove = cls == MoveClass::Best || cls == MoveClass::Excellent ||
                         ((cls == MoveClass::Book || cls == MoveClass::Forced) && c.j.delta < 2.0);
    const bool fault = cls == MoveClass::Inaccuracy || cls == MoveClass::Mistake || cls == MoveClass::Blunder;
    // Decided, still better, clearly ahead: as a player of the level converts an advantage
    // (levelWinPercent). A beginner's +5 is no decided game: the coach still says what a move gives
    // away there, offers it back, and names a blunder that leaves the player only a little ahead.
    // A mate found or allowed where the game was decided the other way already (judge's softened mate
    // classes: +7 or more without the mate, -7 or less before it) keeps the engine's scale: a mate
    // missed at +9, or a mate in 19 allowed at -7, is no fault to stress at any level.
    const bool settledMate = (c.j.mate == MateChange::Lost && !mated(c.lp->score) && c.lp->score.cp > 700) ||
                             (c.j.mate == MateChange::Created && top->score.cp < -700);
    const double hBest = settledMate ? c.j.wBest : levelWinPercent(c.j.wBest, bd);
    const double hPlayed = settledMate ? c.j.wPlayed : levelWinPercent(c.j.wPlayed, bd);
    const bool decidedWin = hBest >= 90.0 && hPlayed >= 80.0;
    const bool decidedLoss = hBest <= 10.0;
    const bool mateType = ex.type == ExType::MateAllowed || ex.type == ExType::MateMissed;
    const bool missedPiece = ex.type == ExType::MissedCapture && ex.offer;   // levels 1-2, a piece of 3+
    // A reason the coach can give: what the move allows or misses on the board, a king left open, a
    // lost ending; not the positional fallback (it only names the better move).
    const bool reason = found && ex.type != ExType::Positional;
    // Still better after the move: a costly move without a reason the board shows only gets a word
    // about the better move, never a blunder or mistake verdict that a player still ahead cannot see.
    const bool stillBetter = hPlayed >= 60.0;
    // A blunder whose reason the coach cannot give reliably (none found, only the positional
    // fallback, or one beyond a beginner's reach): "that's a blunder, your knight had a better move"
    // and a takeback offered without naming the move. The move is said once the offer is declined,
    // or at once when none can be made; the coach says so when the player finds it. Not for a won
    // game drawn by repetition: the tip says why.
    const bool hintOnly = cls == MoveClass::Blunder && !stillBetter && !c.isBest && !c.best.empty() &&
                          !(reason && (level_ >= 3 || ex.concrete)) && !drawTip(c, tipsSaid_);

    bool voice = false;
    bool isRemark = false;   // counts against the unsolicited-remarks cap
    // A dubious move (an inaccuracy, or a costly move with no reason to show while still better):
    // at most a short word about the better move, levels 3-6, never a scenario, a demonstration or
    // an offer.
    bool slip = false;
    // An inaccuracy gets a remark and nothing more: no reason, scenario, demonstration or better
    // line. Levels 3-6: the slip above, or "that move looks dubious" alone when the move loses
    // material ("a bit better" would play it down). Levels 1-2: "that move is dubious, maybe you can
    // do better?" with the takeback offered, the move kept back. A free piece or a mate missed keeps
    // its word and offer at levels 1-2 (the detectors set ex.offer there).
    bool dubious = false;
    const bool materialType = ex.type == ExType::Fork || ex.type == ExType::Discovered || ex.type == ExType::Skewer ||
                              ex.type == ExType::Pin || ex.type == ExType::Trapped || ex.type == ExType::BackRank ||
                              ex.type == ExType::Hanging || ex.type == ExType::Exchange || ex.type == ExType::Material;
    // A piece or more lost on the board while the win percentage barely moves (a one-sided game):
    // said whatever the slip's size.
    const bool lostPiece = found && materialType && ex.lost >= 3;
    const bool missedKept = level_ <= 2 && found && (ex.type == ExType::MateMissed || missedPiece);
    // Level 6, an inaccuracy at the bottom of its class that loses no piece: the slip in a hedged
    // word ("Nf3 may have been a little more precise"), also in place of "that move looks dubious".
    bool hedged = false;
    if (cls == MoveClass::Inaccuracy && !missedKept) {
        isRemark = true;
        const bool due = !c.isBest && !c.best.empty() && humanMoves_ - lastSlip_ >= 3;
        hedged = level_ == 6 && c.j.delta < kHedgedSlipCost && !lostPiece;
        if (level_ <= 2) {
            // One offer per move: not on its replay.
            dubious = due && (c.j.delta >= 7.0 || lostPiece) && offersEnabled_ && !retry && !g.isOver();
        } else if (c.concreteLoss(8) && !hedged) {
            dubious = due && (level_ == 6 || c.j.delta >= 7.0 || lostPiece);
        } else {
            slip = due && (level_ == 6 || c.j.delta >= 7.0);
        }
        voice = slip || dubious;
    } else if (found && ex.type == ExType::Stalemate) voice = true;
    else if (found && ex.type == ExType::MateMissed) voice = true;   // the detector checks the band's limit
    else if (missedPiece) voice = true;
    else if (fault && !reason && stillBetter) {
        // Never for a move that loses material: "a bit better" would play it down.
        slip = level_ >= 3 && !c.isBest && !c.best.empty() && !c.concreteLoss(8);
        voice = slip;
        isRemark = true;
    } else if (fault) {
        // Level 2 says a mistake (in words, no demonstration) when the coach wins something at once,
        // within two plies: the reason's own sequence, or a loss the coach still holds four plies
        // later (an exchange there counted whole). A pawn taken and won back while the reason is about
        // a piece that falls later would have the table show the wrong piece.
        bool keptAtOnce = ex.fullPlies <= 2;
        for (int i = 1; i <= std::min(2, int(c.r.size())); ++i)
            keptAtOnce = keptAtOnce || (c.lossAfter(i) >= 1 && c.lossAfter(std::min(int(c.r.size()), i + 4)) >= 1);
        switch (level_) {
        case 1: voice = cls == MoveClass::Blunder && ((found && ex.concrete) || hintOnly); break;
        case 2:
            voice = (cls == MoveClass::Blunder && ((found && ex.concrete) || hintOnly)) ||
                    (cls == MoveClass::Mistake && found && ex.concrete && c.lossWithin(2) >= 1 && keptAtOnce);
            isRemark = cls != MoveClass::Blunder;
            break;
        case 3: voice = true; isRemark = cls == MoveClass::Mistake; break;
        default: voice = true; isRemark = cls != MoveClass::Blunder; break;
        }
    }
    const bool remarkOnly = slip || dubious;
    if (voice && decidedWin && (remarkOnly || !(ex.type == ExType::Stalemate || ex.type == ExType::MateMissed))) voice = false;
    if (voice && decidedLoss && (remarkOnly || !(level_ <= 2 && ex.type == ExType::MateAllowed && ex.concrete))) voice = false;
    if (voice && (cls == MoveClass::Forced || cls == MoveClass::Book) && !mateType && ex.type != ExType::Stalemate) voice = false;
    if (voice && isRemark && !remarkAllowed()) voice = false;
    if (voice && retry && out.takeback.same) voice = false;
    // A mistake, not a blunder, with a reason the board shows: said in words, without a
    // demonstration (nor a takeback); the coach then plays its move and the player answers it.
    // Mates are still shown.
    const bool spoken = cls == MoveClass::Mistake && reason && !mateType && ex.type != ExType::Stalemate;
    if (!voice) slip = dubious = false;
    // A principle instead (opening, technique): never a fault claim, Low priority, once per game each;
    // never for a top move (the coach would fault its own choice: Be2, blocking a bishop on the queen,
    // is no "bishop moved twice").
    Explanation tipEx;
    bool tip = false;
    if (!voice && !topMove && !(retry && out.takeback.same) && !decidedLoss && !decidedWin && remarkAllowed())
        tip = findTip(c, tipsSaid_, tipEx) && (level_ <= 3 || c.j.delta >= 10.0);

    Script s;
    const int ply = c.ply;
    // The takeback offered: its line said and the card shown; the move remembered for the replay.
    auto pushOffer = [&](const std::string& key) {
        Beat ob;
        ob.kind = BeatKind::OfferTakeback;
        ob.line.key = key;
        ob.look = Look::Player;
        ob.ply = ply;
        ob.gestures.push_back(Gesture{GestureKind::Open, NoSquare, {}, "", 0.0f, false});
        s.push_back(ob);
        ++offers_;
        if (offered_.ply == ply) ++offersAtPly_;
        else offersAtPly_ = 1;
        offered_ = TakebackRecord{};
        offered_.ply = ply;
        offered_.firstUci = c.playedUci;
        offeredHint_ = NoSquare;
        offeredReveal_ = Beat{};
        out.offersTakeback = true;
        v.offered = true;
    };

    // A replayed move after a takeback.
    if (retry) {
        if (out.takeback.same) {
            s.push_back(sayBeat("tb.same", Look::Player, ply));
        } else {
            out.takeback.fixed = c.j.delta < 5.0;
            out.takeback.found = (retry_.hintOnly || retry_.dubious) && c.playedUci == retry_.bestUci;
            if (out.takeback.found) {
                // The move the hint was about.
                Beat b = sayBeat(level_ <= 2 ? "tb.found.b1" : "tb.found", Look::Player, ply);
                b.gestures.push_back(Gesture{GestureKind::Nod, NoSquare, {}, "", 0.0f, false});
                s.push_back(b);
            } else if (out.takeback.fixed) {
                // After a dubious move, "much better" would say too much.
                Beat b = sayBeat(level_ <= 2 && !retry_.dubious ? "tb.fixed.b1" : "tb.fixed", Look::Player, ply);
                b.gestures.push_back(Gesture{GestureKind::Nod, NoSquare, {}, "", 0.0f, false});
                s.push_back(b);
            }
        }
    }

    if (slip) {
        // "Inaccurate: Nf3 was more precise" ("Nf3 may have been a little more precise" when hedged),
        // the better move traced.
        v.voiced = true;
        noteRemark();
        lastSlip_ = humanMoves_;
        const std::string key = hedged ? "ex.verdict.inaccuracy.maybe" : bandKey("ex.verdict.inaccuracy", level_);
        Beat vb = sayBeat(key, Look::Target, ply);
        vb.line.with("move", Arg::ofMove(c.playedSan, c.playedUci)).with("best", moveArg(c.best[0]));
        traceMove(vb, c.best[0].piece, c.best[0].move.from, c.best[0].move.to, "best");
        s.push_back(vb);
    } else if (dubious) {
        // "That move looks dubious" (levels 3-6); "... maybe you can do better?" with the takeback
        // offered (levels 1-2), nothing said on an answer, the coach's praise if the replay finds
        // the best move.
        v.voiced = true;
        noteRemark();
        lastSlip_ = humanMoves_;
        if (level_ <= 2) {
            pushOffer(bandKey("ex.dubious", level_));
            offered_.dubious = true;
            offered_.bestUci = c.l1->pv[0];
        } else {
            s.push_back(sayBeat("ex.verdict.dubious", Look::Player, ply));
        }
    } else if (voice) {
        v.voiced = true;
        if (isRemark) noteRemark();
        // Offer policy: a voiced blunder whose reason the coach shows on the board (not to a player
        // still clearly ahead), and the cases the detector flags per level (a mate within the band's
        // limit, stalemate when winning, a free piece of 3+ points missed at levels 1-2).
        // A mate missed while the game stays won: said (levels 3-6) without a verdict or an offer.
        const bool wonAnyway = ex.type == ExType::MateMissed && hPlayed >= 75.0 && level_ >= 3;
        bool offer = hintOnly || (cls == MoveClass::Blunder && reason && hPlayed < 75.0) ||
                     (ex.offer && !wonAnyway);
        if (!offersEnabled_) offer = false;
        if (retry && out.takeback.same) offer = false;
        if (offered_.ply == ply && offersAtPly_ >= 2) offer = false;
        if (g.isOver()) offer = false;   // the move (or a draw meanwhile) ended the game: nothing to take back
        // Levels 4-6: no offer once the player has taken back the level's count of moves (an offer
        // declined costs nothing). The coach says so, once, where the offer would have been.
        const bool noneLeft = offer && bd.offerCap >= 0 && takebacks_ >= bd.offerCap && !mateOffer(ex.type);
        if (noneLeft) offer = false;

        // Sentence budget: verdict, cause, tip, better move (the demonstration's own lines, the rewind
        // and the offer excluded).
        const bool wantBetter = level_ >= 3 && !hintOnly && !ex.includesBest && !c.isBest && !c.best.empty() &&
                                ex.type != ExType::MateMissed && ex.type != ExType::MissedCapture &&
                                ex.type != ExType::MissedFork;
        // The better move said: levels 3-6 with the idea that follows it ("Nf3, with the idea d4 c5"),
        // levels 1-2 as the piece and its square (a hint-only blunder's move, once revealed).
        auto betterBeat = [&]() {
            if (level_ <= 2) {
                const LineStep& b0 = c.best[0];
                Beat bb = sayBeat(bandKey("ex.reveal", level_), Look::Target, ply);
                bb.line.with("your", Arg::ofPiece(b0.piece, human_, true, b0.move.from)).with("sq", Arg::ofSquare(b0.move.to));
                traceMove(bb, b0.piece, b0.move.from, b0.move.to, "your");
                return bb;
            }
            // {line} is what follows {best}, never {best} again; level 5 names that idea, so without
            // a continuation it says the level-4 line instead. Never past a move the player could not
            // follow, a win-back with its capture (followablePlies). An idea holds the player's own
            // next move: my reply alone is none ("Nxg5, with the idea Nxg5").
            const size_t ideaPlies = size_t(std::max(1, std::min(4, bd.linePlies) - 1));
            const size_t sayable = followablePlies(c.best, 1 + ideaPlies, true);
            std::string idea = sayable >= 3 ? sanLine(c.best, 1, sayable - 1) : std::string();
            Beat bb = sayBeat(bandKey("ex.better", idea.empty() && level_ == 5 ? 4 : level_), Look::Target, ply);
            bb.line.with("best", moveArg(c.best[0]))
                .with("move", Arg::ofMove(c.playedSan, c.playedUci))
                .with("line", Arg::ofMoves(idea))
                .with("eval", evalArg(c.l1->score));
            traceMove(bb, c.best[0].piece, c.best[0].move.from, c.best[0].move.to, "best");
            return bb;
        };
        // The move leaves the coach's threat standing (the threat search after its last move, levels
        // 1-2), and the reason is that threat carried out: the refutation starts with its move and
        // wins what it threatened. The verdict says so ("you did not answer my threat").
        const bool ignoredThreat = level_ <= 2 && found && !hintOnly && threat_.ply == c.ply - 1 && !c.r.empty() &&
                                   c.r.front().uci == threat_.uci &&
                                   (threat_.mate ? ex.type == ExType::MateAllowed : (materialType && ex.lost >= 2));
        std::string verdictKey;
        bool shake = false;
        // No verdict for a move that is not a fault (a slower mate at levels 5-6, a free piece
        // missed with a small loss at levels 1-2 gets the "missed" one): the cause says it all.
        if (ex.missed && level_ <= 2) verdictKey = bandKey("ex.verdict.missed", level_);
        else if (wonAnyway) verdictKey.clear();
        else if (cls == MoveClass::Blunder) {
            verdictKey = bandKey(ignoredThreat ? "ex.verdict.ignored" : "ex.verdict.blunder", level_);
            out.ignoredThreat = ignoredThreat;
            shake = true;
        } else if (cls == MoveClass::Mistake) {
            if (level_ >= 3) verdictKey = bandKey("ex.verdict.mistake", level_);   // level 2: one sentence only
        }
        int budget = bd.sentences - (verdictKey.empty() ? 0 : 1);
        std::vector<Beat> cause = ex.cause, tip = ex.tip;
        Explanation shown = ex;
        if (spoken) shown.demoPlies = 0;
        if (hintOnly) {
            // Neither a reason (none is reliable) nor a demonstration: the piece that had the better
            // move, when a takeback lets the player look for it; else the move itself.
            cause.clear();
            tip.clear();
            shown.demoPlies = 0;
            const LineStep& b0 = c.best[0];
            // Where that piece stands on the table: the moved piece stands on the move's square.
            const Square on = b0.move.from == c.played.from ? c.played.to : b0.move.from;
            const Piece pc = c.p1.at(on);
            if (offer && pc.color == human_ && (pc.type == b0.piece || on == c.played.to)) {
                // "... one that wins material", when that is plainly why the move was a blunder.
                Beat hb = sayBeat(bandKey(missedGain(c) > 0 ? "ex.hint.gain" : "ex.hint", level_), Look::Target, ply);
                hb.line.with("your", pieceArg(c.p1, on, human_));
                pointPiece(hb, on, "your", true);
                cause.push_back(hb);
            } else if (offer) {
                cause.push_back(sayBeat("ex.hint.move", Look::Player, ply));
            } else {
                cause.push_back(betterBeat());
            }
        }
        if (level_ == 2 && cls == MoveClass::Mistake) {   // "one sentence"
            if (cause.size() > 1) cause.resize(1);
            tip.clear();
        }
        const bool better = wantBetter && (cause.empty() || budget >= 2);
        if (better) --budget;
        std::vector<Beat> useCause, useTip;
        for (size_t i = 0; i < cause.size() && budget > 0; ++i, --budget) useCause.push_back(cause[i]);
        for (size_t i = 0; i < tip.size() && budget > 0; ++i, --budget) useTip.push_back(tip[i]);

        if (!verdictKey.empty()) {
            Beat vb = sayBeat(verdictKey, Look::Player, ply);
            vb.line.with("move", Arg::ofMove(c.playedSan, c.playedUci));
            if (!c.best.empty()) vb.line.with("best", moveArg(c.best[0]));
            if (shake) vb.gestures.push_back(Gesture{GestureKind::ShakeHead, NoSquare, {}, "", 0.0f, false});
            s.push_back(vb);
        }
        const bool demo = shown.demoPlies > 0 && !c.r.empty();
        // Why the take-back the table would show fails, with the cause when no demonstration plays
        // the sequence to its end (a cause line said: not the hint), where the listener knows the
        // position it stands in: right after the reply, or at the end of the sequence a material
        // reason says ({line}, levels 3-6). After "I win it", "if you take back with exd6" would
        // leave the moves on the way unsaid.
        if (!useCause.empty() && !hintOnly && !(demo && demoReachesEnd(c, shown)) &&
            (shown.fullPlies <= 1 || (shown.type == ExType::Material && level_ >= 3))) {
            Beat rb;
            if (retakeBeat(c, shown, c.p1, rb)) useCause.push_back(rb);
        }
        for (Beat b : useCause) {
            if (demo)
                for (Mark& m : b.marks) m.untilRewind = true;
            limitPointing(b);
            s.push_back(b);
        }
        if (!useCause.empty() && !hintOnly) {
            Claim& cl = out.claim;
            cl.type = shown.type;
            cl.lost = shown.lost;
            cl.mate = shown.mate;
            cl.plies = std::min(shown.fullPlies, int(c.r.size()));
            for (int i = 0; i < cl.plies; ++i) {
                cl.line.push_back(c.r[size_t(i)].uci);
                cl.guessed = cl.guessed || c.r[size_t(i)].guessed;
            }
            const ClaimCheck cc = checkClaim(c, shown);
            cl.quiet = cc.quiet;
            cl.compensation = cc.compensation;
            cl.ownLine = cc.ownLine;
            cl.held = cc.held;
            cl.bestLoss = c.bestLoss;
            cl.playedLoss = c.playedLoss;
            cl.dropCp = c.dropCp;
            cl.verifiedPlies = checked.plies;
            cl.lineChanged = checked.changed;
            cl.reduced = checked.reduced;
        }
        if (demo) {
            for (Beat& t : shown.tail) limitPointing(t);
            for (Beat& t : shown.threatTail) limitPointing(t);
            appendDemo(c, shown, s);
        }
        for (Beat b : useTip) {
            limitPointing(b);
            s.push_back(b);
        }
        if (better) s.push_back(betterBeat());
        if (offer) {
            pushOffer(bandKey("ex.offer", level_));
            offered_.firstType = ex.type;
            offeredHint_ = ex.hintSquare;
            if (hintOnly) {
                offered_.hintOnly = true;
                offered_.bestUci = c.l1->pv[0];
                offeredHint_ = c.best[0].move.from;   // the piece, back on its square after the takeback
                offeredReveal_ = betterBeat();
            }
        } else {
            if (noneLeft && !noneLeftSaid_) {
                s.push_back(sayBeat("tb.none_left", Look::Player, ply));
                noneLeftSaid_ = true;
            }
            if (demo && !g.isOver()) {
                // The pieces are back and no takeback is offered: the coach's real move comes next,
                // said last so that the player does not take it for more of the demonstration.
                s.push_back(sayBeat("demo.play_on", Look::Player, ply));
            }
        }
    } else if (tip) {
        // A principle, said as a tip (Low priority).
        for (Beat b : tipEx.cause) {
            b.priority = Priority::Low;
            limitPointing(b);
            s.push_back(b);
        }
        if (tipEx.tipBit >= 0) tipsSaid_ |= 1u << tipEx.tipBit;
        noteRemark();
        v.voiced = true;
        if (v.exType == ExType::None) v.exType = tipEx.type;
    } else if (topMove && !(retry && !s.empty())) {
        // ---- Praise: top moves only ----
        const int legal = int(c.p0.legalMoves().size());
        const double w1 = c.j.wBest, w2 = c.l2 ? winPercent(c.l2->score) : w1;
        // Praise says a move mattered: it is weighed by what it does to the result (assessment.h).
        const Assessment& A = c.a;
        // The move alone keeps the result the engine expects: the second best changes it (a win
        // against a draw, a draw against a loss). Worth a word in a decided game too.
        const bool resultLead = A.wdl && c.isBest && A.eLead() >= kDecidedLead;
        const bool decided = w1 >= 90.0 && !resultLead && !(c.p1.isCheckmate() || (c.l1->score.mate > 0 && c.isBest));
        // Lost already: the win chances no longer tell the moves apart (Rb1 four pawns worse than Rf3,
        // both near 2 %), so only the move the lines prefer earns praise; with the engine's WDL, only
        // the move that alone saves the result.
        const bool lostAnyway = w1 <= 10.0 && !(A.wdl ? resultLead : c.isBest);
        // Won or lost whatever is played in the engine's eyes (every move compared at E >= 0.98, or
        // the best at E <= 0.02): a small variation is no feat. Praise then needs the result itself, a
        // forced mate found (c8=R mates where Kb3 only wins), or a large practical lead over the second
        // move (10 W% points: a free rook in a game the engine already calls won).
        const bool settled = A.wdl && (A.eLow >= 0.98 || std::max(A.best.e, A.played.e) <= 0.02);
        const bool flatSettled = settled && !resultLead && !(c.isBest && (A.wLead() >= 10.0 || c.l1->score.mate > 0));
        // The moves compared lie together on both scales: whichever is played, nothing changes. Not a
        // critical moment: no praise for finding one of many moves as good (only the principles a
        // beginner is told to follow, castling, trading when ahead, a free piece taken, keep theirs).
        // A forced mate found where the next move only wins is no such choice (c8=R mates, Kb3 and
        // Kc3 keep a won ending: all at E 1.00).
        const bool mateFound = c.isBest && c.l1->score.mate > 0 && !(c.l2 && c.l2->score.mate > 0);
        const bool flat = A.wdl && A.eSpread() < kFlatE && A.wSpread() < kFlatW && !mateFound;
        const bool recapture = isRecapture(g, size_t(c.ply));
        const bool tricky = in.shallow && in.shallow->ok && !in.shallow->bestMove.empty() && in.shallow->bestMove != c.playedUci;
        // The shallow search's move is one of A0's lines and worth as much as the move played: the
        // move was not harder to find than an equal one.
        bool shallowEqual = false;
        if (tricky && A.wdl)
            if (const ai::PvLine* sl = a0->line(in.shallow->bestMove)) {
                const Outcome so = outcomeOf(sl->score);
                shallowEqual = so.wdl && std::abs(so.e - A.played.e) < kFlatE && std::abs(so.w - A.played.w) < kFlatW;
            }
        // What the move wins for good along its own line: the gain after the first 'plies' plies
        // (heldGain), still there four plies later (the least over that window: Rxd7 Bxc4 bxc4 gives
        // the rook back with the queen).
        auto keptGain = [&](size_t plies) {
            const size_t n = std::min(plies, c.playedLine.size());
            if (n == 0) return 0;
            int kept = heldGain(c.playedLine, n, c.base, human_, c.playedEnd);
            for (size_t j = n + 1; j <= std::min(c.playedLine.size(), n + 4); ++j)
                kept = std::min(kept, heldGain(c.playedLine, j, c.base, human_, c.playedEnd));
            return kept;
        };
        // The most the move wins for good within its line's first 'plies' plies.
        auto lineGain = [&](size_t plies) {
            int bestGain = 0;
            for (size_t i = 1; i <= c.playedLine.size() && i <= plies; ++i) bestGain = std::max(bestGain, keptGain(i));
            return bestGain;
        };
        bool promoSoon = false;
        for (size_t i = 0; i < c.playedLine.size() && i < 2; ++i)
            if (c.playedLine[i].promotion != NoPiece) promoSoon = true;
        // Material given up for good: down after the reply and still down after the player's next move
        // (31...Rxe6 32.Rxe6 Qxe6 wins a pawn: no sacrifice).
        const bool downAfter = c.playedLine.size() >= 2 && c.playedLine[1].balance <= c.base - 2 &&
                               (c.playedLine.size() < 3 || c.playedLine[2].balance <= c.base - 2);
        // Or given up by exchange, starting with a capture the coach can legally make: Bf5 against a
        // bishop pinned on e6 (Bxf5 is illegal), Ne5 against a knight pinned on c6, offer nothing.
        int takenBack = 0;
        for (const Move& m : c.p1.legalMoves())
            if (m.to == c.played.to && (m.flags & MoveCapture)) takenBack = std::max(takenBack, seePoints(c.p1, m));
        const int byExchange = takenBack - (c.f.captured != NoPiece ? points(c.f.captured) : 0);
        // Nor won back at once elsewhere by the player's next move: Bxf7+ Kxf7 hxg3, the bishop on
        // d5 was lost anyway (a check on the way that wins a pawn).
        const bool wonBack = c.playedLine.size() >= 3 && c.playedLine[1].captured != NoPiece &&
                             c.playedLine[1].move.to == c.played.to && c.playedLine[2].captured != NoPiece &&
                             c.playedLine[2].move.to != c.played.to && c.playedLine[2].balance >= c.base - 1;
        // Given up by exchange only when the reply takes it: a piece the coach leaves, because taking
        // costs more (Nxf2 Kxf2 Rxd2+ wins the bishop back), is given up by nobody.
        const bool accepted = c.playedLine.size() >= 2 && c.playedLine[1].move.to == c.played.to &&
                              c.playedLine[1].captured != NoPiece;
        // Nor is a loss the move did not cause: the reply takes elsewhere what was en prise already
        // (Ne2 Bxh1 Rxh1, the rook trapped on h1 before the move, which only saves two points).
        const bool lostBefore = c.playedLine.size() >= 2 && c.playedLine[1].captured != NoPiece &&
                                c.playedLine[1].move.to != c.played.to &&
                                hasBit(hangingPieces(c.p0, human_, false), c.playedLine[1].move.to);
        const bool sacrifice = !promoSoon && !wonBack &&
                               ((c.f.seePts <= -2 && byExchange >= 2 && accepted) || (downAfter && !lostBefore));
        // A capture that wins the whole piece ("for free"); at levels 1-2 the lines also say it had
        // no protector.
        const bool freeCapture = c.f.captured != NoPiece && points(c.f.captured) >= 3 &&
                                 c.f.seePts >= points(c.f.captured);
        // ... and keeps it: the game goes on (a capture that stalemates wins nothing) and the engine's
        // line does not win it back (within a pawn) over the next four plies.
        const bool captureKept = freeCapture && c.p1.hasLegalMove() && keptGain(1) >= points(c.f.captured) - 1;
        const bool unprotected = c.f.captured != NoPiece && !c.p0.attackersTo(c.f.capturedOn, c.coach);
        // The best move by far in practice: the second best is 15 W% points worse...
        const bool farBest = c.isBest && legal >= 2 && c.l2 && w1 - w2 >= 15.0 && w1 >= 25.0 && !(c.p0.inCheck() && legal <= 2);
        // ... and the only move when, with the engine's WDL, the second best changes the result
        // (kOnlyLead): a second move that wins as well, or holds as well, makes it the better of two
        // good moves ("good move", not "the only move").
        const bool only = farBest && (!A.wdl || A.eLead() >= kOnlyLead);
        // A fork praised wins one of the pieces it hits: the player's next move along the played line
        // takes a forked piece, not a pawn. Nd3+ Qxd3 Qxd3 wins the queen that took the knight (a
        // decoy), not the pawn on b2 the knight hit; Rxc7+ Rd7 exd7 wins the rook put in the way.
        const bool forkWins = c.playedLine.size() >= 3 && c.playedLine[2].mover == human_ &&
                              c.playedLine[2].captured != NoPiece && c.playedLine[2].captured != Pawn &&
                              hasBit(c.f.forks, c.playedLine[2].move.to);
        double lastW = -1.0;   // the human's W% after their previous judged move
        for (size_t i = humanW_.size(); i-- > 0 && lastW < 0.0;) lastW = humanW_[i];
        std::string key;
        // A sacrifice in a game already won is praised only when it wins far more than the second move.
        const bool sacrificeLead = c.isBest && (A.wLead() >= 15.0 || (A.wdl && A.eLead() >= kOnlyLead));
        if (!c.p1.isCheckmate() && !recapture && !decided && !lostAnyway && !v.hastensMate && cls != MoveClass::Forced) {
            if (!brilliantDone_ && c.j.delta < 2.0 && sacrifice && (w1 <= 85.0 || sacrificeLead) && c.j.wPlayed >= 50.0 &&
                !flatSettled) {
                key = level_ <= 2 ? "praise.sacrifice" : bandKey("praise.brilliant", level_);
                brilliantDone_ = true;
                v.brilliant = true;
            } else if (level_ >= 2 && c.isBest && w1 >= 60.0 && c.l2 && w1 - w2 >= 20.0 && lastW >= 0.0 &&
                       lastW <= 45.0 && (!A.wdl || A.eLead() >= kOnlyLead)) {
                key = bandKey("praise.great", level_);
                v.great = true;
            } else if (level_ >= 2 && only) {
                key = bandKey("praise.only", level_);
            } else if (level_ >= 2 && squareCount(c.f.forks) >= 2 && forkWins && lineGain(4) >= 2 && !flatSettled) {
                key = level_ == 2 ? "praise.fork.b2" : "praise.fork.b3";
            } else if (level_ <= 2 && captureKept && unprotected && !flatSettled) {
                key = bandKey("praise.capture", level_);
                v.goodCapture = true;
            } else if (level_ <= 3 && c.isBest && (lineGain(3) >= 2 || c.l1->score.mate > 0 || farBest) && (level_ < 3 || tricky) &&
                       !flatSettled && !flat) {
                key = bandKey("praise.best", level_);
            } else if (level_ <= 3 && c.base >= 3 && c.f.captured != NoPiece && points(c.f.captured) >= 3 &&
                       c.f.seePts == 0 && legalCaptureOn(c.p1, c.played.to)) {
                key = bandKey("praise.trade", level_);
            } else if ((level_ == 3 || level_ == 4) && c.j.delta < 2.0 && c.f.captured == NoPiece && !c.f.check && tricky &&
                       !flat && !shallowEqual && !flatSettled) {
                key = bandKey("praise.excellent", level_);
            } else if (level_ == 1 && (c.f.castleKing || c.f.castleQueen) && !castlePraised_) {
                key = "praise.castle.b1";
                castlePraised_ = true;
            } else if (level_ == 1 && c.p0.at(c.played.from).type != Pawn &&
                       hasBit(hangingPieces(c.p0, human_, true), c.played.from) && wonWhereItStood(c.p0, c.played.from) &&
                       !hangingPieces(c.p1, human_, false) && !flatSettled && !flat && pieceKept(c, c.played.to)) {
                // Saved only when nothing else is left en prise (Rb1 while the queen on b3 hangs to a rook),
                // and only a piece: the line says "piece", and a pawn is none.
                key = "praise.saved.b1";
            } else if (level_ <= 2 && in.inBook && bookPraises_ < 2) {
                key = "praise.book.b1";
                ++bookPraises_;
            }
        }
        v.only = only;
        v.goodCapture = v.goodCapture || captureKept;
        if (!key.empty() && humanMoves_ - lastPraise_ >= bd.praiseEvery && remarkAllowed() && !retry) {
            Beat b = sayBeat(key, Look::Player, ply);
            if (c.f.captured != NoPiece) b.line.with("my", Arg::ofPiece(c.f.captured, c.coach, false, NoSquare));
            b.line.with("move", Arg::ofMove(c.playedSan, c.playedUci));
            b.gestures.push_back(Gesture{GestureKind::Nod, NoSquare, {}, "", 0.0f, false});
            s.push_back(b);
            lastPraise_ = humanMoves_;
            noteRemark();
            v.praised = true;
        }
    }

    humanW_.push_back(c.j.wPlayed);
    out.script = s;
    return out;
}

std::string Reviewer::threatKey(const Game& g) const {
    if (g.moves().empty() || level_ > 2 || g.isOver()) return std::string();
    if (g.positionAt(g.moves().size() - 1).sideToMove() == human_) return std::string();
    Position turn = g.position();
    return turn.passTurn() ? turn.fen() : std::string();
}

namespace {

// What the coach wins for good along the threat search's line (the turn passed to it, from 'turn'):
// the most the human is down within its first four plies and still down two plies later (or at the
// line's end, a capture the human still has made); a forced mate: mate.
struct EngineThreat {
    bool known = false;
    std::string uci;
    int gain = 0;
    bool mate = false;
};

EngineThreat engineThreat(const Position& turn, const ai::Analysis* threat, Color human) {
    EngineThreat out;
    if (!threat || !threat->ok || threat->lines.empty() || threat->lines.front().pv.empty()) return out;
    const ai::PvLine& top = threat->lines.front();
    const std::vector<LineStep> line = replayLine(turn, top.pv, human, 8);
    if (line.empty()) return out;
    out.known = true;
    out.uci = top.pv.front();
    out.mate = top.score.mate > 0 || top.score.matesNow;
    const int base = materialBalance(turn, human);
    const Position end = lineEnd(turn, line);
    const int n = int(line.size());
    const auto lossAfter = [&](int j) {
        int l = base - line[size_t(j - 1)].balance;
        if (j == n && end.sideToMove() == human && end.hasLegalMove()) l -= bestCapturePoints(end, human);
        return l;
    };
    for (int i = 1; i <= std::min(4, n); ++i) {
        int l = lossAfter(i);
        for (int j = i + 1; j <= std::min(n, i + 2); ++j) l = std::min(l, lossAfter(j));
        out.gain = std::max(out.gain, l);
    }
    return out;
}

}  // namespace

Script Reviewer::coachMoved(const Game& g, const ai::Analysis* before, const ai::Analysis* threat) {
    Script s;
    if (g.moves().empty() || level_ > 2 || g.isOver()) return s;
    const int ply = int(g.moves().size()) - 1;
    const Position& now = g.position();
    const Position& prev = g.positionAt(size_t(ply));
    if (prev.sideToMove() == human_) return s;   // the last move was not the coach's
    const Color coach = opposite(human_);
    // The engine's view of the threats: what the coach would play were the turn passed to it.
    EngineThreat et;
    {
        Position turn = now;
        if (turn.passTurn()) et = engineThreat(turn, threat, human_);
    }
    threat_ = Threat{};
    if (et.known && (et.mate || et.gain >= 2)) threat_ = Threat{ply, et.uci, et.gain, et.mate};

    // "My last move was a mistake. Can you punish it?" (level 1, once, confirmed by A0).
    if (level_ == 1 && !punishHintDone_ && before && before->ok && !before->lines.empty() && !before->lines[0].pv.empty()) {
        const uint64_t hung = hangingPieces(now, coach, true) & ~hangingPieces(prev, coach, true);
        const Move bm = now.parseUCI(before->lines[0].pv[0]);
        // What the coach's move took counts against it: a piece taken back after it took as much
        // (16.Nxg4 Nxg4) is a trade, not a mistake. And the move cost the coach as much as a mistake:
        // the player's W% now 10 points above what it was after their own last move (an exchange
        // that wins the piece back, Rxa2 Qxa2 dxc5, costs nothing).
        const Move last = g.moves().back();
        const int took = points(prev.at(last.to).type);
        const double lastW = ply >= 1 && size_t(ply - 1) < humanW_.size() ? humanW_[size_t(ply - 1)] : -1.0;
        const bool costly = lastW >= 0.0 && winPercent(before->lines[0].score) - lastW >= 10.0;
        // The engine's line keeps the material: 2 points more than before the coach's move once four
        // plies are played (Qxc5 hxg4, a knight for a knight, punishes nothing).
        const std::vector<LineStep> line = replayLine(now, before->lines[0].pv, human_, 4);
        const bool kept = !line.empty() && line.back().balance - materialBalance(prev, human_) >= 2;
        if (bm.valid() && hasBit(hung, bm.to) && points(now.at(bm.to).type) >= 3 && seePoints(now, bm) - took >= 2 &&
            costly && kept && remarkAllowed()) {
            Beat b = sayBeat("threat.punish.b1", Look::Player, ply, Priority::Low);
            b.gestures.push_back(Gesture{GestureKind::Open, NoSquare, {}, "", 0.0f, false});
            s.push_back(b);
            punishHintDone_ = true;
            noteRemark();
            return s;
        }
    }
    if (!remarkAllowed()) return s;
    // A mate threat first (the threat search, when there is one, mates as well).
    Move mate;
    if (mateThreat(now, coach, &mate) && (!et.known || et.mate)) {
        Beat b = sayBeat(bandKey("threat.mate", level_), Look::Target, ply, Priority::Low);
        b.line.with("sq", Arg::ofSquare(mate.to)).with("my", pieceArg(now, mate.from, human_));
        if (level_ == 1) pointSquare(b, mate.to, "sq");
        s.push_back(b);
        noteRemark();
        return s;
    }
    // New threats against the human's pieces (level 1: 3+ points; level 2: the queen), by a capture
    // the coach could make next: never by a piece pinned to its king.
    Position turn = now;
    const bool passed = turn.passTurn();
    const std::vector<Move> next = passed ? turn.legalMoves() : std::vector<Move>();
    auto takers = [&](Square t) {
        uint64_t out = 0;
        for (Square a : squaresOf(now.attackersTo(t, coach))) {
            bool legal = !passed && !hasBit(now.pinned(coach), a);
            for (const Move& m : next) legal = legal || (m.from == a && m.to == t);
            if (legal) out |= squareBit(a);
        }
        return out;
    };
    const uint64_t fresh = hangingPieces(now, human_, true) & ~hangingPieces(prev, human_, true);
    Square target = NoSquare;
    for (Square t : squaresOf(fresh)) {
        const PieceType tt = now.at(t).type;
        if (level_ == 2 && tt != Queen) continue;
        if (points(tt) < 3 || !takers(t)) continue;
        if (target == NoSquare || points(tt) > points(now.at(target).type)) target = t;
    }
    if (target == NoSquare) return s;
    // The threat search, when there is one, wins that piece or as much: a piece the rules find en
    // prise that a tactic defends is no threat. Its first move is the coach's, read with the turn
    // passed (`turn`; `now` has the player to move and knows no such move).
    if (et.known) {
        const Move first = turn.parseUCI(et.uci);
        const bool takesIt = first.valid() && first.to == target;
        if (et.gain < 2 || (!takesIt && et.gain < points(now.at(target).type) - 1)) return s;
    }
    // The attacker: the coach's moved piece when it is one of them, else the cheapest.
    const uint64_t attackers = takers(target);
    Square att = hasBit(attackers, g.moves().back().to) ? g.moves().back().to : NoSquare;
    if (att == NoSquare)
        for (Square a : squaresOf(attackers))
            if (att == NoSquare || points(now.at(a).type) < points(now.at(att).type)) att = a;
    Beat b = sayBeat(bandKey("threat.piece", level_), Look::Target, ply, Priority::Low);
    b.line.with("my", pieceArg(now, att, human_)).with("your", pieceArg(now, target, human_));
    if (level_ == 1) traceMove(b, now.at(att).type, att, target, "my");
    pointPiece(b, target, "your", true);
    limitPointing(b);
    s.push_back(b);
    noteRemark();
    return s;
}

Script Reviewer::takebackAccepted(const Game& g, bool fromOffer) {
    Script s;
    if (offered_.ply < 0) return s;
    retryPending_ = true;
    retry_ = offered_;
    // The card answered yes: one of the level's takebacks gone (levels 4-6, not for a mate).
    if (fromOffer && offerUsesTakeback()) ++takebacks_;
    // A dubious move: no hint, the player looks again on their own.
    if (level_ > 2 || offered_.dubious) return s;
    const int ply = int(g.moves().size());
    const Position& p = g.position();
    // A hint-only offer named the piece with the better move: the hint stays on it.
    const ExType t = offered_.hintOnly ? ExType::None : offered_.firstType;
    if (t == ExType::MateMissed) {
        s.push_back(sayBeat("tb.hint.mate_missed.b1", Look::Player, ply));
    } else if (t == ExType::MissedCapture || t == ExType::MissedFork) {
        s.push_back(sayBeat("tb.hint.missed.b1", Look::Player, ply));
    } else if (t == ExType::MateAllowed || t == ExType::BackRank || t == ExType::KingSafety) {
        Beat b = sayBeat("tb.hint.king.b1", Look::Target, ply);
        const Square k = p.kingSquare(human_);
        b.line.with("your", pieceArg(p, k, human_));
        pointPiece(b, k, "your");
        s.push_back(b);
    } else if (offeredHint_ != NoSquare && !p.at(offeredHint_).empty() && p.at(offeredHint_).color == human_) {
        Beat b = sayBeat("tb.hint.b1", Look::Target, ply);
        b.line.with("your", pieceArg(p, offeredHint_, human_));
        pointPiece(b, offeredHint_, "your");
        s.push_back(b);
    } else {
        s.push_back(sayBeat("tb.hint.general.b1", Look::Player, ply));
    }
    return s;
}

Script Reviewer::takebackDeclined() {
    retryPending_ = false;
    Script s;
    // The better move a hint-only offer kept back: said now, once.
    if (offered_.hintOnly && !offeredReveal_.line.key.empty()) s.push_back(offeredReveal_);
    offeredReveal_ = Beat{};
    return s;
}

int Reviewer::takebacksLeft() const {
    const int cap = band(level_).offerCap;
    if (cap < 0 || !offersEnabled_) return -1;
    return std::max(0, cap - takebacks_);
}

bool Reviewer::offerUsesTakeback() const {
    return offered_.ply >= 0 && band(level_).offerCap >= 0 && !mateOffer(offered_.firstType);
}

}  // namespace coach
