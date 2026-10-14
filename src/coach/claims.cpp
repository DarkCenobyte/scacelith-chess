// The second step of the coach's review (review.h): what a reason's sequence establishes. Every
// detector (review_explain.cpp) checks its own motif on the board; the checks here are the ones every
// sequence goes through, whatever its motif, before its consequence is said as a fact.
#include "coach/review_internal.h"

#include <algorithm>

namespace coach {
namespace detail {

using namespace chess;

int quietReplies(const Ctx& c, int plies) {
    int n = 0;
    Position q = c.p1;
    for (int i = 0; i < plies && i < int(c.r.size()); ++i) {
        const LineStep& st = c.r[size_t(i)];
        if (st.mover == c.human && st.captured == NoPiece && !q.inCheck() && q.legalMoves().size() > 1) ++n;
        q.makeMove(st.move);
    }
    return n;
}

ClaimCheck checkClaim(const Ctx& c, const Explanation& ex) {
    ClaimCheck out;
    out.quiet = quietReplies(c, ex.fullPlies);
    if (ex.mate || ex.lost <= 0) return out;
    // The move's own material cost (beyond what the best move loses too) against the engine's drop:
    // a piece the engine's score barely misses comes back in other ways (an attack, pawns, activity).
    const int own = ex.lost - c.bestLoss;
    if (own > 0) out.compensation = std::max(0, 100 * own - c.dropCp);
    // The move's own deeper line (A0's, or A1's, from p0) loses as much, or ends in a mate against the
    // human: the refutation may be a shallower search's.
    out.held = lineLoss(c, c.r, 1, ex.fullPlies + 3);
    const bool mated = c.lp && (c.lp->score.mate < 0 || c.lp->score.matedNow);
    if (c.playedLoss >= 0 && !mated) out.ownLine = c.playedLoss >= ex.lost - (ex.lost >= 2 ? 1 : 0);
    return out;
}

namespace {

// The side to move's centipawns, a forced mate worth more than any material (mates of any length
// alike).
int sideCp(const ai::Score& s) {
    if (s.mate > 0 || s.matesNow) return 2000;
    if (s.mate < 0 || s.matedNow) return -2000;
    return std::clamp(s.cp, -1500, 1500);
}

}  // namespace

bool asGood(const ai::Score& a, const ai::Score& b) {
    return winPercent(a) >= winPercent(b) - kVerifySlack && sideCp(a) >= sideCp(b) - kVerifySlackCp;
}

std::string compareKey(const std::string& fen, const std::string& a, const std::string& b) {
    return fen + " | " + std::min(a, b) + " " + std::max(a, b);
}

bool compared(const std::map<std::string, ai::Analysis>& v, const std::string& fen, const std::string& a,
              const std::string& b, const ai::PvLine*& la, const ai::PvLine*& lb) {
    la = lb = nullptr;
    const auto it = v.find(compareKey(fen, a, b));
    if (it == v.end() || !it->second.ok) return false;
    la = it->second.line(a);
    lb = it->second.line(b);
    return la && lb && !la->pv.empty() && !lb->pv.empty();
}

int lineLoss(const Ctx& c, const std::vector<LineStep>& line, int from, int plies) {
    const int n = int(line.size());
    if (n == 0) return 0;
    // What the human is down after ply j, read where the captures that follow at once end: an
    // exchange, or a capture answered by another (...g5 Qxa5 gxh4 wins nothing back).
    const auto lossAfter = [&](int j) {
        while (j < n && line[size_t(j - 1)].captured != NoPiece && line[size_t(j)].captured != NoPiece) ++j;
        return c.base - line[size_t(j - 1)].balance;
    };
    // At the line's end, a capture the human still has to make.
    const Position end = lineEnd(c.p1, line);
    int endLoss = c.base - line.back().balance;
    if (end.sideToMove() == c.human && end.hasLegalMove()) endLoss -= bestCapturePoints(end, c.human);
    int worst = 0;
    for (int i = std::max(1, from); i <= std::min(plies, n); ++i) {
        int l = lossAfter(i);
        for (int j = i + 1; j <= std::min(n, i + kHeldPlies); ++j) l = std::min(l, lossAfter(j));
        if (i + kHeldPlies >= n) l = std::min(l, endLoss);
        worst = std::max(worst, l);
    }
    return worst;
}

Verification verifyLine(const Ctx& c, std::vector<LineStep>& line, const Position& start,
                        const std::map<std::string, ai::Analysis>& v, const ai::Analysis* first, int want,
                        int deepPlies) {
    Verification out;
    Position q = start;
    for (int i = 0; i < want && i < int(line.size()); ++i) {
        const std::vector<Move> legal = q.legalMoves();
        if (legal.empty()) break;
        // Forced: nothing to compare. Nor where the coach moves within the plies its own search saw
        // deeper: that move stays, and the coach has no defence to weigh.
        const bool human = q.sideToMove() == c.human;
        if (legal.size() == 1 || (!human && i < deepPlies)) {
            q.makeMove(line[size_t(i)].move);
            out.plies = i + 1;
            continue;
        }
        const std::string fen = q.fen();
        const auto it = v.find(fen);
        const bool useFirst = it == v.end() && i == 0 && first && first->ok;
        if (it == v.end() && !useFirst) {
            out.next = fen;
            break;
        }
        const ai::Analysis& a = useFirst ? *first : it->second;
        if (!a.ok || a.lines.empty() || a.lines.front().pv.empty()) break;   // a failed search confirms nothing
        const ai::PvLine& top = a.lines.front();
        const std::string& uci = line[size_t(i)].uci;
        // The line's move in the short search: absent from its lines, it is worth at most the last.
        const ai::PvLine* mine = a.line(uci);
        const ai::Score mineScore = mine ? mine->score : a.lines.back().score;
        if (!human) {
            // A coach move the short search finds weaker than its best (by half the slack that replaces
            // it, or out of its lines): compared with it more deeply.
            if (mine != &top && (!mine || winPercent(mineScore) < winPercent(top.score) - kReplaceSlack / 2)) {
                const ai::PvLine *lm = nullptr, *lt = nullptr;
                if (!compared(v, fen, uci, top.pv[0], lm, lt)) {
                    if (v.count(compareKey(fen, uci, top.pv[0])) == 0) out.next = compareKey(fen, uci, top.pv[0]);
                    break;
                }
                if (winPercent(lm->score) < winPercent(lt->score) - kReplaceSlack) {
                    std::vector<LineStep> tail = replayLine(q, lt->pv, c.human, 16);
                    if (!tail.empty()) {
                        out.notes.push_back("ply " + std::to_string(i + 1) + " coach " + uci + " " + lm->score.text() +
                                            " < " + top.pv[0] + " " + lt->score.text() + " @ " + fen);
                        line.resize(size_t(i));
                        line.insert(line.end(), tail.begin(), tail.end());
                        out.changed = true;
                        out.changedAt = i;
                        out.plies = i;
                        break;   // the rest of the line is new: walked again (verifyReason)
                    }
                }
            }
        } else {
            // The player's other moves about as good as the line's in the short search (its best
            // when it prefers another), each with that search's line from here.
            for (const ai::PvLine& l : a.lines) {
                if (l.pv.empty() || l.pv.front() == uci || !asGood(l.score, mineScore)) continue;
                Defence d;
                d.at = i;
                d.score = l.score;
                d.line.assign(line.begin(), line.begin() + i);
                const std::vector<LineStep> tail = replayLine(q, l.pv, c.human, 16);
                if (tail.empty()) continue;
                d.line.insert(d.line.end(), tail.begin(), tail.end());
                out.others.push_back(std::move(d));
            }
        }
        q.makeMove(line[size_t(i)].move);
        out.plies = i + 1;
    }
    return out;
}

}  // namespace detail
}  // namespace coach
