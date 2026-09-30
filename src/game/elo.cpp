#include "elo.h"
#include "../core/ini.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace elo {

namespace {

// FIDE table 8.1.2 as published: the highest D of each row and the PD of the higher-rated player in
// hundredths; D above 735 gives 1.00. The table is the normal distribution with a standard
// deviation of 2000 / 7 rounded to hundredths, except at six differences where FIDE's rows keep
// the neighbouring value (54, 343, 344, 358, 392 and 620; tests/elo_tests.cpp): FIDE applies the
// table, so the table is what counts. The 400-point rule caps D before the lookup; the rows past
// 400 are the published table's, and table 8.1.1 below is the middle of each row (rounded down).
struct PdRow {
    int maxD, pd;
};
constexpr PdRow kPdTable[] = {
    {3, 50},   {10, 51},  {17, 52},  {25, 53},  {32, 54},  {39, 55},  {46, 56},  {53, 57},  {61, 58},  {68, 59},
    {76, 60},  {83, 61},  {91, 62},  {98, 63},  {106, 64}, {113, 65}, {121, 66}, {129, 67}, {137, 68}, {145, 69},
    {153, 70}, {162, 71}, {170, 72}, {179, 73}, {188, 74}, {197, 75}, {206, 76}, {215, 77}, {225, 78}, {235, 79},
    {245, 80}, {256, 81}, {267, 82}, {278, 83}, {290, 84}, {302, 85}, {315, 86}, {328, 87}, {344, 88}, {357, 89},
    {374, 90}, {391, 91}, {411, 92}, {432, 93}, {456, 94}, {484, 95}, {517, 96}, {559, 97}, {619, 98}, {735, 99},
};

// FIDE table 8.1.1: dp for p = 1.00, 0.99 ... 0.50; below 0.50, dp(p) = -dp(1 - p).
constexpr int kDpTable[] = {
    800, 677, 589, 538, 501, 470, 444, 422, 401, 383, 366, 351, 336, 322, 309, 296, 284, 273, 262, 251,
    240, 230, 220, 211, 202, 193, 184, 175, 166, 158, 149, 141, 133, 125, 117, 110, 102, 95,  87,  80,
    72,  65,  57,  50,  43,  36,  29,  21,  14,  7,   0,
};

// a / b rounded to the nearest integer, halves away from zero (b > 0).
long long divRound(long long a, long long b) {
    return a < 0 ? -((-2 * a + b) / (2 * b)) : (2 * a + b) / (2 * b);
}

// Expected score in hundredths: the table's PD after the 400-point rule.
int expected100(int rating, int opponent) {
    int pd = scoringProbability(std::min(kMaxRatingGap, std::abs(rating - opponent)));
    return rating >= opponent ? pd : 100 - pd;
}

// A score (clamped to 0..1) in half points: 2 win, 1 draw, 0 loss.
int halfPoints(double score) {
    return int(std::lround(std::clamp(score, 0.0, 1.0) * 2.0));
}

}  // namespace

int scoringProbability(int d) {
    d = std::abs(d);
    for (const PdRow& row : kPdTable)
        if (d <= row.maxD) return row.pd;
    return 100;
}

int ratingDifference(int p) {
    p = std::clamp(p, 0, 100);
    return p >= 50 ? kDpTable[100 - p] : -kDpTable[p];
}

double expectedScore(int rating, int opponent) {
    return expected100(rating, opponent) / 100.0;
}

int kFactor(const Record& r) {
    if (r.peak >= kSeniorRating || r.rating >= kSeniorRating) return 10;
    return r.games < kProvisionalGames ? 40 : 20;
}

int initialRating(const Record& r) {
    long long n = r.unratedGames + 2;
    // p = (score + 1) / (n + 2) = (half points + 2) / (2 (n + 2)), in hundredths rounded half up.
    int p = int(divRound(100LL * (r.unratedHalfPoints + 2), 2 * n));
    long long ru = divRound(r.unratedOpponents + 2LL * kHypotheticalOpponent + ratingDifference(p) * n, n);
    return int(std::clamp<long long>(ru, kFloor, kMaxInitialRating));
}

int ratingDelta(const Record& r, int opponent, double score) {
    return int(divRound(kFactor(r) * (50 * halfPoints(score) - expected100(r.rating, opponent)), 100));
}

Change applyResult(Record& r, const Record& opponent, double score) {
    Change c;
    c.before = r.rating;
    c.expected = expectedScore(r.rating, opponent.rating);
    int half = halfPoints(score);
    if (!r.rated) {
        // The zero-score rule: the losses before the first half point are disregarded.
        if (half > 0 || r.unratedHalfPoints > 0) {
            ++r.unratedGames;
            r.unratedOpponents += opponent.rating;
            r.unratedHalfPoints += half;
            if (r.unratedGames >= kUnratedGames) {
                r.rating = r.peak = initialRating(r);
                r.rated = true;
                r.unratedGames = r.unratedOpponents = r.unratedHalfPoints = 0;
            }
        }
    } else if (opponent.rated) {
        c.k = kFactor(r);
        r.rating = std::max(kFloor, r.rating + ratingDelta(r, opponent.rating, score));
        r.peak = std::max(r.peak, r.rating);
    }
    ++r.games;
    if (half == 2) ++r.wins;
    else if (half == 0) ++r.losses;
    else ++r.draws;
    c.after = r.rating;
    return c;
}

Change applyResult(Record& r, int opponent, double score) {
    Record o;
    o.rating = o.peak = opponent;
    o.rated = true;
    return applyResult(r, o, score);
}

PairChange applyPair(Record& white, Record& black, double whiteScore) {
    Record whiteBefore = white, blackBefore = black;
    double s = std::clamp(whiteScore, 0.0, 1.0);
    PairChange c;
    c.white = applyResult(white, blackBefore, s);
    c.black = applyResult(black, whiteBefore, 1.0 - s);
    return c;
}

Record readRecord(const IniFile& ini, const std::string& section) {
    const std::string k = section + ".";
    Record r;
    r.rating = std::max(kFloor, ini.getInt(k + "elo", kInitialRating));
    r.games = std::max(0, ini.getInt(k + "games", 0));
    r.wins = std::max(0, ini.getInt(k + "wins", 0));
    r.draws = std::max(0, ini.getInt(k + "draws", 0));
    r.losses = std::max(0, ini.getInt(k + "losses", 0));
    r.peak = std::max(r.rating, ini.getInt(k + "peak", r.rating));
    r.rated = ini.getBool(k + "rated", r.games > 0);
    if (!r.rated) {
        // The phase ends at its kUnratedGames-th game: a record never waits with that many.
        r.unratedGames = std::clamp(ini.getInt(k + "unrated_games", 0), 0, kUnratedGames - 1);
        r.unratedOpponents = std::max(0, ini.getInt(k + "unrated_opponents", 0));
        r.unratedHalfPoints = std::clamp(ini.getInt(k + "unrated_half_points", 0), 0, 2 * r.unratedGames);
    }
    return r;
}

void writeRecord(IniFile& ini, const std::string& section, const Record& r) {
    const std::string k = section + ".";
    ini.setInt(k + "elo", r.rating);
    ini.setInt(k + "games", r.games);
    ini.setInt(k + "wins", r.wins);
    ini.setInt(k + "draws", r.draws);
    ini.setInt(k + "losses", r.losses);
    ini.setInt(k + "peak", r.peak);
    ini.setBool(k + "rated", r.rated);
    ini.setInt(k + "unrated_games", r.unratedGames);
    ini.setInt(k + "unrated_opponents", r.unratedOpponents);
    ini.setInt(k + "unrated_half_points", r.unratedHalfPoints);
}

}  // namespace elo
