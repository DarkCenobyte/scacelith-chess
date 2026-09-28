#include "elo.h"
#include <algorithm>
#include <cmath>

namespace elo {

double expectedScore(int rating, int opponent) {
    int gap = std::clamp(opponent - rating, -kMaxRatingGap, kMaxRatingGap);
    return 1.0 / (1.0 + std::pow(10.0, double(gap) / 400.0));
}

int kFactor(const Record& r) {
    if (r.peak >= kSeniorRating || r.rating >= kSeniorRating) return 10;
    return r.games < kProvisionalGames ? 40 : 20;
}

int ratingDelta(const Record& r, int opponent, double score) {
    double s = std::clamp(score, 0.0, 1.0);
    return int(std::lround(double(kFactor(r)) * (s - expectedScore(r.rating, opponent))));
}

Change applyResult(Record& r, int opponent, double score) {
    Change c;
    c.before = r.rating;
    c.expected = expectedScore(r.rating, opponent);
    c.k = kFactor(r);
    r.rating = std::max(kFloor, r.rating + ratingDelta(r, opponent, score));
    ++r.games;
    if (score > 0.75) ++r.wins;
    else if (score < 0.25) ++r.losses;
    else ++r.draws;
    r.peak = std::max(r.peak, r.rating);
    c.after = r.rating;
    return c;
}

}  // namespace elo
