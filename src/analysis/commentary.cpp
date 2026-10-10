// The commentator of the Analysis mode (see commentary.h): what is said on each position of a
// reviewed game, as catalog lines (assets/coach/speech/<lang>/analysis.lang) with the marks lit
// while they are said.
//
// Every claim is a board fact (coach/tactics.h) or the engine's own line, checked before the line is
// chosen: a mate "in one" is a checkmate on the board, a piece "left unprotected" has no defender and
// is taken whole by the reply, a fork's targets are found by coach::forkTargets and the line takes
// one of them, material "won" is held after the plies shown. When nothing can be proven, only the
// verdict and the better move are said: silence rather than a doubtful sentence.
//
// The placeholders of one comment are distinct across its lines ({move} aside, which no mark uses):
// every mark's anchor names exactly one line of its comment.
#include "analysis/commentary.h"

#include "analysis/review_facts.h"
#include "coach/openings.h"
#include "coach/review_internal.h"
#include "coach/tactics.h"

#include <algorithm>
#include <cstdlib>

namespace analysis {

using namespace chess;
using coach::Arg;
using coach::LineStep;
using coach::Mark;

namespace {

int points(PieceType t) { return coach::kPiecePoints[t]; }

coach::Line say(const std::string& key) {
    coach::Line l;
    l.key = key;
    return l;
}

// The coach's mark helpers work on a beat: borrow one, keep its marks.
void take(Comment& c, const coach::Beat& b) { c.marks.insert(c.marks.end(), b.marks.begin(), b.marks.end()); }
void markArrow(Comment& c, PieceType t, Square from, Square to, const std::string& anchor) {
    coach::Beat b;
    coach::detail::markArrow(b, t, from, to, anchor);
    take(c, b);
}
void markPiece(Comment& c, Square s, const std::string& anchor) {
    coach::Beat b;
    coach::detail::markPiece(b, s, anchor);
    take(c, b);
}
void markSquare(Comment& c, Square s, const std::string& anchor) {
    coach::Beat b;
    coach::detail::markSquare(b, s, anchor);
    take(c, b);
}

Arg sidePiece(const Position& p, Square s) {
    const Piece pc = p.at(s);
    return Arg::ofSidePiece(pc.type, pc.color, s);
}
Arg moveArg(const LineStep& s) { return coach::detail::moveArg(s); }
// The mate itself, named in a line that says "is checkmate": without its '#', which the voice would
// read as a second "checkmate" ("queen takes f7, checkmate, was checkmate").
Arg mateMoveArg(const LineStep& s) {
    Arg a = moveArg(s);
    while (!a.san.empty() && (a.san.back() == '#' || a.san.back() == '+')) a.san.pop_back();
    return a;
}

// Targets of a fork, the king first, then by value (a1 first on ties).
std::vector<Square> byValue(const Position& p, uint64_t set) {
    std::vector<Square> v = squaresOf(set);
    std::stable_sort(v.begin(), v.end(), [&](Square a, Square b) {
        const PieceType ta = p.at(a).type, tb = p.at(b).type;
        if ((ta == King) != (tb == King)) return ta == King;
        return coach::kPieceValueCp[ta] > coach::kPieceValueCp[tb];
    });
    return v;
}

// ---- The move that led to a position ------------------------------------------------------------

struct MoveCtx {
    Verdict v;
    Position before, after;
    Move move;
    Color mover = White, opp = Black;
    const PositionEval* e0 = nullptr;   // before the move: the mover's best line
    const PositionEval* e1 = nullptr;   // after it: the opponent's best line
    std::vector<LineStep> reply;        // e1's line on the board (the opponent first), balances the opponent's
    Position replyEnd;
    std::vector<LineStep> best;         // e0's line on the board, balances the mover's
    Position bestEnd;
    coach::Judgement j;

    // The opponent's material gain held after the first n plies of 'reply', from the position
    // after the move (coach::detail::heldGain: a recapture on the next ply undoes it).
    int replyGain(int n) const {
        return coach::detail::heldGain(reply, size_t(n), coach::materialBalance(after, opp), opp, replyEnd);
    }
    int bestGain(int n) const {
        return coach::detail::heldGain(best, size_t(n), coach::materialBalance(before, mover), mover, bestEnd);
    }
    int bestGainWithin(int n) const {
        int g = 0;
        for (int i = 1; i <= n && i <= int(best.size()); ++i) g = std::max(g, bestGain(i));
        return g;
    }

    // What the mover has lost for good after the first n plies of 'reply', from the position before
    // the move (a capture the move made counts), as coach::detail::heldLoss measures it: the mover's
    // recapture on the next ply undoes it, not a capture there that is taken back on the ply after.
    int moverLoss(int n) const {
        const int size = int(reply.size());
        n = std::min(n, size);
        const int base = coach::materialBalance(before, opp);
        if (n <= 0) return coach::materialBalance(after, opp) - base;
        int l = reply[size_t(n - 1)].balance - base;
        if (n < size) {
            int m = n;
            if (reply[size_t(n)].mover == mover && reply[size_t(n)].captured != NoPiece && n + 1 < size &&
                reply[size_t(n + 1)].captured != NoPiece && reply[size_t(n + 1)].move.to == reply[size_t(n)].move.to)
                m = n + 1;
            l = std::min(l, reply[size_t(m)].balance - base);
        } else if (replyEnd.sideToMove() == mover) {
            l -= coach::bestCapturePoints(replyEnd, mover);
        }
        return l;
    }
    int bestLoss = 0;   // what the mover is still down near the end of the best line's first 8 plies
    int dropCp = 0;     // the mover's centipawns after the best move minus after the move played
    // Whether losing 'lost' points is why the move is bad (coach::detail::Ctx::lossExplains): a
    // real loss, one the best move avoids, and the size of the engine's drop.
    bool explains(int lost) const {
        const int need = bestLoss == 0 ? 1 : 2;
        if (lost < need || lost - bestLoss < need) return false;
        return lost >= 3 || 250 * lost >= dropCp;
    }
    // The loss the opponent's capture at ply k of 'reply' settles (with the mover's recapture when it
    // comes next), where the mover cannot take back enough at once and is still down by as much four
    // plies later, more than before that capture; 0 when it does not explain the move.
    int settledLoss(int k, int* plies = nullptr) const {
        const int n = int(reply.size());
        if (k < 0 || k >= n || reply[size_t(k)].mover != opp || reply[size_t(k)].captured == NoPiece) return 0;
        Position q = after;
        for (int i = 0; i <= k; ++i) q.makeMove(reply[size_t(i)].move);
        const int already = k > 0 ? moverLoss(k) : 0;
        // The mover's recapture on the next ply is part of the sequence (said with it).
        const bool retake = k + 1 < n && reply[size_t(k + 1)].mover == mover && reply[size_t(k + 1)].captured != NoPiece &&
                            reply[size_t(k + 1)].move.to == reply[size_t(k)].move.to;
        for (int p = k + 1; p <= std::min(n, k + 2); ++p) {
            if (p > k + 1) q.makeMove(reply[size_t(p - 1)].move);
            if (retake && p == k + 1) continue;
            const LineStep& st = reply[size_t(p - 1)];
            if (st.captured == NoPiece || st.mate) continue;
            const int board = coach::materialBalance(q, opp) - coach::materialBalance(before, opp);
            int l = board;
            if (q.sideToMove() == mover) l -= coach::bestCapturePoints(q, mover);
            // What the mover could take back at once is taken off, unless that loss would not
            // explain the move and the line shows the take-back never comes (the board's loss then);
            // the capture at k must add to what was lost before it.
            const int held = p < n ? moverLoss(std::min(n, p + 4)) : l;
            if (held > l && p < n && reply[size_t(p)].mover == mover && reply[size_t(p)].captured == NoPiece &&
                !(l > already && explains(l))) {
                // Four plies later, the board still shows as much lost, whatever the mover could take then.
                const int at = std::min(n, p + 4);
                Position later = q;
                for (int i = p; i < at; ++i) later.makeMove(reply[size_t(i)].move);
                int still = coach::materialBalance(later, opp) - coach::materialBalance(before, opp);
                if (later.sideToMove() == mover) still -= coach::bestCapturePoints(later, mover);
                l = std::max(l, std::min({board, held, still}));
            }
            if (l <= already || !explains(l) || !explains(held)) continue;
            if (plies) *plies = p;
            return l;
        }
        return 0;
    }
};

MoveCtx contextOf(const GameReview& r, int ply) {
    MoveCtx c;
    c.v = r.verdict(ply);
    c.before = r.positionAt(ply);
    c.after = r.positionAt(ply + 1);
    c.move = r.game().moves()[size_t(ply)];
    c.mover = c.before.sideToMove();
    c.opp = opposite(c.mover);
    c.e0 = &r.position(ply);
    c.e1 = &r.position(ply + 1);
    if (!c.v.known) return c;
    c.reply = coach::replayLine(c.after, c.e1->pv, c.opp, 16);
    c.replyEnd = coach::detail::lineEnd(c.after, c.reply);
    c.best = coach::replayLine(c.before, c.e0->pv, c.mover, 16);
    c.bestEnd = coach::detail::lineEnd(c.before, c.best);
    const ai::Score played = c.v.playedBest ? c.e0->best : c.e1->best.flipped();
    c.j = coach::judge(c.e0->best, played, c.v.playedBest);
    c.dropCp = std::max(0, coach::detail::whiteCp(c.e0->best, true) - coach::detail::whiteCp(played, true));
    const size_t w = std::min<size_t>(8, c.best.size());
    const int base = coach::materialBalance(c.before, c.mover);
    int lost = w > 0 ? coach::detail::heldLoss(c.best, w, base, c.mover, c.bestEnd) : 0;
    for (size_t i = w > 2 ? w - 2 : 1; i < w; ++i) lost = std::min(lost, coach::detail::heldLoss(c.best, i, base, c.mover, c.bestEnd));
    c.bestLoss = std::max(0, lost);
    return c;
}

// What a comment has said already.
struct Said {
    bool best = false;   // a line names the better move ({best})
    bool mate = false;   // a line names a forced mate
};

// ---- What a mistake allows -------------------------------------------------------------------------

// The opponent mates now: the engine's mate, the first move of its line on the board.
bool mateAllowed(const MoveCtx& c, Comment& out, Said& said) {
    if (c.j.mate != coach::MateChange::Created || c.reply.empty()) return false;
    const int n = c.e1->best.mate;
    if (n <= 0) return false;
    const LineStep& r0 = c.reply[0];
    coach::Line l;
    if (n == 1) {
        if (!r0.mate) return false;   // "is checkmate": the board says so
        l = say("an.mate_allowed.one");
    } else {
        l = say("an.mate_allowed");
        l.with("m", Arg::ofNumber(n));
    }
    l.with("reply", n == 1 ? mateMoveArg(r0) : moveArg(r0));
    out.lines.push_back(l);
    markArrow(out, r0.piece, r0.move.from, r0.move.to, "reply");
    said.mate = true;
    return true;
}

// The reply forks two of the mover's pieces, and the line takes one of them, that very piece, soon
// after: a loss that explains the move (MoveCtx::settledLoss; a capture the move made counts).
bool forkAllowed(const MoveCtx& c, Comment& out) {
    if (c.reply.size() < 3 || c.reply[0].mover != c.opp) return false;
    const LineStep& r0 = c.reply[0];
    Position p2 = c.after;
    p2.makeMove(r0.move);
    const uint64_t targets = coach::forkTargets(p2, r0.move.to);
    if (squareCount(targets) < 2) return false;
    Square taken = NoSquare;
    for (size_t i = 2; i < c.reply.size() && i <= 4 && taken == NoSquare; i += 2) {
        const LineStep& st = c.reply[i];
        if (st.captured == NoPiece || !coach::detail::hasBit(targets, st.move.to)) continue;
        bool stays = true;   // the target did not move away (another piece took its square)
        for (size_t j = 1; j < i; ++j)
            if (c.reply[j].mover == c.mover && c.reply[j].move.from == st.move.to) stays = false;
        if (stays && c.settledLoss(int(i)) > 0) taken = st.move.to;
    }
    if (taken == NoSquare) return false;
    std::vector<Square> ts = byValue(p2, targets);
    if (ts[0] != taken && ts[1] != taken) ts[1] = taken;   // the piece that falls is named
    coach::Line l = say("an.fork");
    l.with("reply", moveArg(r0)).with("t1", sidePiece(p2, ts[0])).with("t2", sidePiece(p2, ts[1]));
    out.lines.push_back(l);
    markArrow(out, r0.piece, r0.move.from, r0.move.to, "reply");
    markSquare(out, r0.move.to, "reply");
    markPiece(out, ts[0], "t1");
    markPiece(out, ts[1], "t2");
    return true;
}

// A piece of the mover's has no defender, and the reply takes the whole of it, a loss that explains
// the move (counted from before it).
bool hanging(const MoveCtx& c, Comment& out) {
    if (c.reply.empty() || c.reply[0].mover != c.opp) return false;
    const LineStep& r0 = c.reply[0];
    if (r0.captured == NoPiece || r0.captured == Pawn || r0.captured == King || (r0.move.flags & MoveEnPassant))
        return false;
    const Square s = r0.move.to;
    if (!coach::isUndefended(c.after, s) || coach::seePoints(c.after, r0.move) < points(r0.captured)) return false;
    if (c.settledLoss(0) <= 0) return false;
    coach::Line l = say("an.hanging");
    l.with("piece", sidePiece(c.after, s)).with("reply", moveArg(r0));
    out.lines.push_back(l);
    markPiece(out, s, "piece");
    markArrow(out, r0.piece, r0.move.from, s, "reply");
    return true;
}

// Material the engine's line wins and keeps, within 4 plies, as much as the move costs and more than
// the best move loses: "{line} wins {pts}" (net, from before the move).
bool materialLine(const MoveCtx& c, Comment& out) {
    if (c.reply.empty() || c.reply[0].mover != c.opp) return false;
    int plies = 0, gain = 0;
    for (int k = 0; k < 4 && k < int(c.reply.size()) && plies == 0; ++k) gain = c.settledLoss(k, &plies);
    if (plies == 0 || gain <= 0) return false;
    coach::Line l = say("an.material");
    l.with("line", Arg::ofMoves(coach::sanLine(c.reply, 0, size_t(plies)))).with("pts", Arg::ofNumber(gain));
    out.lines.push_back(l);
    const LineStep& r0 = c.reply[0];
    markArrow(out, r0.piece, r0.move.from, r0.move.to, "line");
    return true;
}

// ---- What a mistake misses ---------------------------------------------------------------------------

bool mateMissed(const MoveCtx& c, Comment& out, Said& said) {
    if (c.j.mate != coach::MateChange::Lost || c.v.playedBest || c.best.empty()) return false;
    const int n = c.e0->best.mate;
    if (n <= 0) return false;
    const LineStep& b0 = c.best[0];
    coach::Line l;
    if (n == 1) {
        if (!b0.mate) return false;
        l = say("an.mate_missed.one");
    } else {
        l = say("an.mate_missed");
        l.with("m", Arg::ofNumber(n));
    }
    l.with("best", n == 1 ? mateMoveArg(b0) : moveArg(b0));
    out.lines.push_back(l);
    markArrow(out, b0.piece, b0.move.from, b0.move.to, "best");
    said.best = said.mate = true;
    return true;
}

// The engine's move took a piece that had no defender, whole, for good; the piece is still there.
bool missedCapture(const MoveCtx& c, Comment& out, Said& said) {
    if (c.v.playedBest || c.best.empty()) return false;
    const LineStep& b0 = c.best[0];
    if (b0.captured == NoPiece || b0.captured == Pawn || b0.captured == King || (b0.move.flags & MoveEnPassant))
        return false;
    const Square y = b0.move.to;
    if (!coach::isUndefended(c.before, y) || coach::seePoints(c.before, b0.move) < points(b0.captured)) return false;
    const Piece still = c.after.at(y);
    if (still.type != b0.captured || still.color != c.opp) return false;
    const coach::MoveFacts f = coach::analyzeMove(c.before, c.move);
    if (f.captured != NoPiece && f.seePts >= points(b0.captured) - 1) return false;   // took as much elsewhere
    if (c.bestGainWithin(3) < 2) return false;
    coach::Line l = say("an.missed_capture");
    l.with("best", moveArg(b0)).with("piece", sidePiece(c.after, y));
    out.lines.push_back(l);
    markArrow(out, b0.piece, b0.move.from, y, "best");
    markPiece(out, y, "piece");
    said.best = true;
    return true;
}

// The engine's move forked two of the opponent's pieces, and its line wins one of them.
bool missedFork(const MoveCtx& c, Comment& out, Said& said) {
    if (c.v.playedBest || c.best.size() < 3) return false;
    const LineStep& b0 = c.best[0];
    Position q = c.before;
    q.makeMove(b0.move);
    const uint64_t targets = coach::forkTargets(q, b0.move.to);
    if (squareCount(targets) < 2) return false;
    bool takes = false;
    for (size_t i = 2; i < c.best.size() && i <= 4; i += 2)
        if (c.best[i].captured != NoPiece && coach::detail::hasBit(targets, c.best[i].move.to)) takes = true;
    if (!takes || c.bestGainWithin(5) < 2) return false;
    const std::vector<Square> ts = byValue(q, targets);
    coach::Line l = say("an.missed_fork");
    l.with("best", moveArg(b0)).with("t1", sidePiece(q, ts[0])).with("t2", sidePiece(q, ts[1]));
    out.lines.push_back(l);
    markArrow(out, b0.piece, b0.move.from, b0.move.to, "best");
    for (int i = 0; i < 2; ++i) {
        const Square t = ts[size_t(i)];
        const char* anchor = i == 0 ? "t1" : "t2";
        if (c.after.at(t) == q.at(t)) markPiece(out, t, anchor);   // still there on the board
        else markSquare(out, t, anchor);
    }
    said.best = true;
    return true;
}

void betterMove(const MoveCtx& c, Comment& out, Said& said) {
    if (said.best || c.v.betterUci.empty() || c.best.empty()) return;
    const LineStep& b0 = c.best[0];
    coach::Line l = say("an.better");
    l.with("best", moveArg(b0));
    out.lines.push_back(l);
    markArrow(out, b0.piece, b0.move.from, b0.move.to, "best");
    said.best = true;
}

// ---- The moves ----------------------------------------------------------------------------------------

// The lines about the move that led to position p (p >= 1); returns their weight (0: nothing).
int moveLines(const GameReview& r, int p, Comment& out, Said& said) {
    const int ply = p - 1;
    const MoveCtx c = contextOf(r, ply);
    if (!c.v.known) return 0;
    const Arg move = Arg::ofMove(c.v.san, c.v.uci);
    switch (c.v.nag) {
    case Nag::Blunder:
    case Nag::Mistake: {
        // After the opponent's own mistake, the move that fails to punish it: the chance missed.
        const Verdict prev = ply > 0 ? r.verdict(ply - 1) : Verdict();
        const bool chance = prev.known && (prev.nag == Nag::Blunder || prev.nag == Nag::Mistake);
        out.lines.push_back(say(chance ? "an.missed_chance" : c.v.nag == Nag::Blunder ? "an.blunder" : "an.mistake")
                                .with("move", move));
        const bool found =
            mateAllowed(c, out, said) || mateMissed(c, out, said) ||
            (chance ? (missedCapture(c, out, said) || missedFork(c, out, said) || forkAllowed(c, out) ||
                       hanging(c, out) || materialLine(c, out))
                    : (forkAllowed(c, out) || hanging(c, out) || materialLine(c, out) || missedCapture(c, out, said) ||
                       missedFork(c, out, said)));
        (void)found;   // without a proven cause, the verdict and the better move say enough
        betterMove(c, out, said);
        return c.v.nag == Nag::Blunder ? 3 : 2;
    }
    case Nag::Brilliant: {
        const detail::Given g = detail::materialGiven(c.before, c.move);
        if (!g.any()) return 0;
        out.lines.push_back(say("an.brilliant").with("move", move).with("piece", sidePiece(c.after, g.square)));
        markPiece(out, g.square, "piece");
        if (c.e1->best.mate < 0) {   // the opponent, to move, is mated in N
            out.lines.push_back(say("an.brilliant.mate").with("m", Arg::ofNumber(-c.e1->best.mate)));
            said.mate = true;
        }
        return 3;
    }
    case Nag::Good: {
        // What "only" means is what the second best line gives away: every other move is at most
        // as good as it.
        if (!c.e0->hasSecond) return 0;
        const double w1 = c.v.wBest, w2 = coach::winPercent(c.e0->second);
        const char* key = w1 >= 60.0 && w2 < 55.0   ? "an.only.win"
                          : w1 >= 40.0 && w2 < 35.0 ? "an.only.hold"
                          : w1 < 40.0 && w2 < 15.0  ? "an.only.defend"
                                                    : "an.only.best";
        out.lines.push_back(say(key).with("move", move));
        return 2;
    }
    case Nag::Dubious: {
        if (c.v.betterUci.empty() || c.best.empty()) return 0;
        const LineStep& b0 = c.best[0];
        out.lines.push_back(say("an.inaccuracy").with("move", move).with("best", moveArg(b0)));
        markArrow(out, b0.piece, b0.move.from, b0.move.to, "best");
        said.best = true;
        return 1;
    }
    case Nag::Interesting: {
        const detail::Given g = detail::materialGiven(c.before, c.move);
        if (!g.any()) return 0;
        out.lines.push_back(say("an.interesting").with("move", move).with("piece", sidePiece(c.after, g.square)));
        markPiece(out, g.square, "piece");
        return 1;
    }
    case Nag::None: break;
    }
    return 0;
}

// The first position of a forced mate for one side (the bar turns to M#), when nothing said it yet.
int mateOnLines(const GameReview& r, int p, Comment& out, const Said& said) {
    if (said.mate || p < 1 || p >= r.plies() || r.position(p).terminal) return 0;
    const EvalBar now = r.bar(p), prev = r.bar(p - 1);
    if (!now.known || !prev.known || now.mateWhite == 0) return 0;
    if (prev.mateWhite != 0 && (prev.mateWhite > 0) == (now.mateWhite > 0)) return 0;
    out.lines.push_back(say(now.mateWhite > 0 ? "an.mate_on.white" : "an.mate_on.black")
                            .with("m", Arg::ofNumber(std::abs(now.mateWhite))));
    return 3;
}

// ---- The opening ------------------------------------------------------------------------------------

// Position p is the first one out of the opening book, after at least one book move.
bool leavesBook(const GameReview& r, int p) {
    if (p < 2 || p > r.plies()) return false;
    for (int k = 0; k < p - 1; ++k)
        if (!r.verdict(k).book) return false;
    return !r.verdict(p - 1).book;
}

// The opening's name as the book had it on its last position (the first 'plies' moves): the
// latest notable variation, else the family; "" for a generic family, a rare opening, a set-up start.
std::string openingRef(const GameReview& r, int plies) {
    Game g = r.game();
    const int extra = int(g.moves().size()) - plies;
    if (extra > 0) g.undo(extra);
    const coach::OpeningBook& book = coach::OpeningBook::instance();
    const coach::OpeningState st = coach::classify(g, book);
    if (!st.standardStart) return std::string();
    std::string ref;
    int at = -1;
    for (const coach::OpeningSideLabels& s : st.side)
        if (s.variation >= 0 && size_t(s.variation) < book.variations().size() && s.variationPly > at) {
            at = s.variationPly;
            ref = "variation:" + book.variations()[size_t(s.variation)].id;
        }
    if (ref.empty() && st.family >= 0 && size_t(st.family) < book.families().size() &&
        !book.families()[size_t(st.family)].generic)
        ref = "family:" + book.families()[size_t(st.family)].id;
    return ref;
}

int openingLines(const GameReview& r, int p, Comment& out) {
    if (!leavesBook(r, p)) return 0;
    const std::string ref = openingRef(r, p - 1);
    if (!ref.empty()) out.lines.push_back(say("an.opening").with("opening", Arg::ofOpening(ref)));
    const Verdict v = r.verdict(p - 1);
    // A move that leaves the book with a ? or ?? is named by the lines that follow: the opening's
    // name is enough before them.
    if (v.known && (v.nag == Nag::Mistake || v.nag == Nag::Blunder)) return ref.empty() ? 0 : 2;
    out.lines.push_back(say("an.book_exit").with("move", Arg::ofMove(v.san, v.uci)));
    return 2;
}

// ---- The end -------------------------------------------------------------------------------------------

const char* sideName(Color c) { return c == White ? "white" : "black"; }

int endLines(const GameReview& r, const GameInfo& info, Comment& out) {
    const Position& last = r.positionAt(r.plies());
    std::string key;
    // The board first: a final checkmate or stalemate is a fact whatever the record says.
    if (last.isCheckmate()) {
        key = std::string("an.end.mate.") + sideName(opposite(last.sideToMove()));
    } else if (last.isStalemate()) {
        key = "an.end.stalemate";
    } else if (info.result == "1-0" || info.result == "0-1") {
        const char* side = info.result == "1-0" ? "white" : "black";
        if (info.endReasonKey == "reason.resignation") key = std::string("an.end.resigned.") + side;
        else if (info.endReasonKey == "reason.timeout") key = std::string("an.end.timeout.") + side;
        else key = std::string("an.end.win.") + side;
    } else if (info.result == "1/2-1/2") {
        const std::string& why = info.endReasonKey;
        if (why == "reason.agreement") key = "an.end.draw.agreement";
        else if (why == "reason.threefold_claim" || why == "reason.fivefold") key = "an.end.draw.repetition";
        else if (why == "reason.50_moves_claim") key = "an.end.draw.fifty";
        else if (why == "reason.75_moves") key = "an.end.draw.seventy_five";
        else if (why == "reason.insufficient") key = "an.end.draw.material";
        else key = "an.end.draw";
    } else {
        key = "an.end.unfinished";
    }
    out.lines.push_back(say(key));

    const SideSummary s[2] = {r.summary(White), r.summary(Black)};
    // Accuracy means little over a handful of moves.
    if (s[0].moves >= 5 && s[1].moves >= 5)
        out.lines.push_back(say("an.end.accuracy")
                                .with("acc_white", Arg::ofNumber(int(s[0].accuracy + 0.5)))
                                .with("acc_black", Arg::ofNumber(int(s[1].accuracy + 0.5))));
    for (const Color c : {White, Black}) {
        const SideSummary& x = s[c];
        if (x.moves == 0) continue;
        const int mistakes = x.count[int(Nag::Mistake)], blunders = x.count[int(Nag::Blunder)];
        const std::string side = sideName(c);
        if (mistakes == 0 && blunders == 0) out.lines.push_back(say("an.end.clean." + side));
        else if (blunders == 0) out.lines.push_back(say("an.end.mistakes." + side).with("n", Arg::ofNumber(mistakes)));
        else if (mistakes == 0) out.lines.push_back(say("an.end.blunders." + side).with("m", Arg::ofNumber(blunders)));
        else
            out.lines.push_back(say("an.end.errors." + side)
                                    .with("n", Arg::ofNumber(mistakes))
                                    .with("m", Arg::ofNumber(blunders)));
    }
    return 3;
}

// ---- A position's comment, before the crowding rule --------------------------------------------------

Comment build(const GameReview& r, const GameInfo& info, int p) {
    Comment c;
    c.position = p;
    if (p < 0 || p > r.plies()) return c;
    int weight = 0;
    if (p == 0) {
        c.lines.push_back(say(r.positionAt(0).isStandardStart() ? "an.start" : "an.start.setup"));
        weight = 2;
    } else {
        Said said;
        weight = std::max(weight, openingLines(r, p, c));
        weight = std::max(weight, moveLines(r, p, c, said));
        weight = std::max(weight, mateOnLines(r, p, c, said));
    }
    if (p == r.plies()) weight = std::max(weight, endLines(r, info, c));
    c.weight = c.lines.empty() ? 0 : weight;
    return c;
}

}  // namespace

void Commentator::reset(const GameInfo& info) { info_ = info; }

bool Commentator::ready(const GameReview& review, int p) const {
    if (p < 0 || p > review.plies()) return false;
    if (p == review.plies() && !review.complete()) return false;
    if (p == 0) return true;   // the opening words need no evaluation
    // The plies a comment reads (the move, the opponent's move before it for a chance missed, and
    // the comments of the two positions before for the crowding rule): positions p - 3 .. p.
    for (int i = std::max(0, p - 3); i <= p; ++i) {
        const PositionEval& e = review.position(i);
        if (!e.final && !e.failed) return false;
    }
    return true;
}

Comment Commentator::commentAt(const GameReview& review, int p) const {
    Comment c = build(review, info_, p);
    // A colour comment (an inaccuracy, an interesting try) only in a quiet stretch: nothing to say
    // on either of the two positions before.
    if (c.weight == 1)
        for (int q = p - 1; q >= 0 && q >= p - 2; --q)
            if (!build(review, info_, q).empty()) {
                Comment none;
                none.position = p;
                return none;
            }
    return c;
}

std::vector<std::string> Commentator::keys() {
    return {
        "an.start", "an.start.setup",
        "an.opening", "an.book_exit",
        "an.blunder", "an.mistake", "an.missed_chance",
        "an.mate_allowed.one", "an.mate_allowed", "an.fork", "an.hanging", "an.material",
        "an.mate_missed.one", "an.mate_missed", "an.missed_capture", "an.missed_fork",
        "an.better",
        "an.brilliant", "an.brilliant.mate",
        "an.only.win", "an.only.hold", "an.only.defend", "an.only.best",
        "an.inaccuracy", "an.interesting",
        "an.mate_on.white", "an.mate_on.black",
        "an.end.mate.white", "an.end.mate.black",
        "an.end.resigned.white", "an.end.resigned.black",
        "an.end.timeout.white", "an.end.timeout.black",
        "an.end.win.white", "an.end.win.black",
        "an.end.stalemate",
        "an.end.draw.agreement", "an.end.draw.repetition", "an.end.draw.fifty", "an.end.draw.seventy_five",
        "an.end.draw.material", "an.end.draw",
        "an.end.unfinished",
        "an.end.accuracy",
        "an.end.clean.white", "an.end.clean.black",
        "an.end.mistakes.white", "an.end.mistakes.black",
        "an.end.blunders.white", "an.end.blunders.black",
        "an.end.errors.white", "an.end.errors.black",
    };
}

}  // namespace analysis
