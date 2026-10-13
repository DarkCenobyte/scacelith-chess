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
    const bool mated = c.lp && (c.lp->score.mate < 0 || c.lp->score.matedNow);
    if (c.playedLoss >= 0 && !mated) out.ownLine = c.playedLoss >= ex.lost - (ex.lost >= 2 ? 1 : 0);
    return out;
}

}  // namespace detail
}  // namespace coach
