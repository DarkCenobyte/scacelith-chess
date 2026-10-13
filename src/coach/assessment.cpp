// The first step of the coach's review: the expected result before and after the move (see
// assessment.h).
#include "coach/assessment.h"

#include "coach/review.h"

#include <algorithm>

namespace coach {

Outcome outcomeOf(const ai::Score& s) {
    Outcome o;
    o.mates = s.matesNow || s.mate > 0;
    o.mated = s.matedNow || s.mate < 0;
    o.w = winPercent(s);
    o.e = s.expected();
    o.wdl = s.hasWdl;
    if (o.mates) {
        o.win = 1000;
        o.draw = o.loss = 0;
    } else if (o.mated) {
        o.loss = 1000;
        o.win = o.draw = 0;
    } else if (s.hasWdl) {
        o.win = s.win;
        o.draw = s.draw;
        o.loss = s.loss;
    } else {
        // No WDL (a hand-built score): the lichess curve, read as wins and losses only.
        o.win = int(o.e * 1000.0 + 0.5);
        o.loss = 1000 - o.win;
        o.draw = 0;
    }
    return o;
}

ResultClass resultOf(const Outcome& o) {
    if (o.win >= 500) return ResultClass::Win;
    if (o.loss >= 500) return ResultClass::Loss;
    return ResultClass::Draw;
}

double Assessment::eDrop() const { return playedIsBest ? 0.0 : std::max(0.0, best.e - played.e); }

double Assessment::eLead() const { return hasSecond ? std::max(0.0, best.e - second.e) : 0.0; }

double Assessment::wLead() const { return hasSecond ? std::max(0.0, best.w - second.w) : 0.0; }

double Assessment::eSpread() const { return std::max(best.e, played.e) - eLow; }

double Assessment::wSpread() const { return std::max(best.w, played.w) - wLow; }

bool Assessment::changesResult() const {
    return wdl && !playedIsBest && resultOf(best) != resultOf(played) && best.e - played.e >= kResultDrop;
}

Assessment assess(const std::vector<ai::PvLine>& a0, const ai::Score& played, bool playedIsBest) {
    Assessment a;
    if (a0.empty()) return a;
    a.best = outcomeOf(a0[0].score);
    a.played = playedIsBest ? a.best : outcomeOf(played);
    a.hasSecond = a0.size() >= 2;
    if (a.hasSecond) a.second = outcomeOf(a0[1].score);
    a.playedIsBest = playedIsBest;
    a.wdl = a.best.wdl && a.played.wdl;
    a.eLow = a.played.e;
    a.wLow = a.played.w;
    for (const ai::PvLine& l : a0) {
        const Outcome o = outcomeOf(l.score);
        a.wdl = a.wdl && o.wdl;
        a.eLow = std::min(a.eLow, o.e);
        a.wLow = std::min(a.wLow, o.w);
    }
    return a;
}

}  // namespace coach
