// Elo rating (src/game/elo.h): FIDE's tables 8.1.1 and 8.1.2, the K formula, the unrated phase and
// the first rating (with the zero-score rule for both players), games against unrated players, the
// counted games (K, provisional), the .ini records,
// and the vectors of the dedicated server's rating (dedicated-server/test/fixtures/elo-vectors.json,
// written by dedicated-server/tools/gen-elo-vectors.js from src/match/elo.js): the game and the
// server must give the same numbers to the last point. The vectors file is looked up from the
// current directory (run from the repository root), $SCACELITH_SOURCE_DIR and the executable's
// parent directories.
#include "test.h"
#include "core/ini.h"
#include "game/elo.h"
#include "net/json.h"
#include "net/net_sys.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using net::json::Value;

namespace {

std::string readRepoFile(const std::string& rel) {
    std::vector<std::string> roots;
    if (const char* env = std::getenv("SCACELITH_SOURCE_DIR")) roots.push_back(std::string(env) + "/");
    roots.push_back("");
    roots.push_back("../");
    roots.push_back("../../");
    std::string exe = net::sys::exeDirectory();
    roots.push_back(exe + "../");
    roots.push_back(exe + "../../");
    for (auto& r : roots) {
        std::string text;
        if (net::sys::readFile(r + rel, text, 16 << 20)) return text;
    }
    return std::string();
}

elo::Record rated(int rating, int games, int peak = 0) {
    elo::Record r;
    r.rated = true;
    r.rating = rating;
    r.games = r.countedGames = games;
    r.peak = std::max(rating, peak);
    return r;
}

elo::Record recordOf(const Value& v) {
    elo::Record r;
    r.rating = int(v["rating"].asInt());
    r.games = int(v["games"].asInt());
    r.wins = int(v["wins"].asInt());
    r.draws = int(v["draws"].asInt());
    r.losses = int(v["losses"].asInt());
    r.peak = int(v["peak"].asInt());
    r.rated = v["rated"].asBool();
    r.countedGames = int(v["countedGames"].asInt());
    r.unratedGames = int(v["unratedGames"].asInt());
    r.unratedOpponents = int(v["unratedOpponents"].asInt());
    r.unratedHalfPoints = int(v["unratedHalfPoints"].asInt());
    return r;
}

bool sameRecord(const elo::Record& a, const elo::Record& b) {
    return a.rating == b.rating && a.games == b.games && a.wins == b.wins && a.draws == b.draws && a.losses == b.losses &&
           a.peak == b.peak && a.rated == b.rated && a.countedGames == b.countedGames && a.unratedGames == b.unratedGames &&
           a.unratedOpponents == b.unratedOpponents && a.unratedHalfPoints == b.unratedHalfPoints;
}

}  // namespace

TEST(elo_fide_table_8_1_2) {
    // FIDE's table is the normal distribution with sigma = 2000 / 7 rounded to hundredths, except at
    // six differences where the published rows keep the neighbouring value (see elo.cpp).
    std::vector<int> mismatches;
    for (int d = 0; d <= 800; ++d) {
        double cdf = 0.5 * (1.0 + std::erf(d / (2000.0 / 7.0) / std::sqrt(2.0)));
        int rounded = int(std::floor(100.0 * cdf + 0.5));
        int table = elo::scoringProbability(d);
        if (rounded != table) {
            mismatches.push_back(d);
            CHECK_EQ(std::abs(rounded - table), 1);
        }
    }
    CHECK(mismatches == std::vector<int>({54, 343, 344, 358, 392, 620}));
    // The published rows up to the 400-point cap: {first D, last D, PD}.
    const int rows[][3] = {
        {0, 3, 50},     {4, 10, 51},    {11, 17, 52},   {18, 25, 53},   {26, 32, 54},   {33, 39, 55},   {40, 46, 56},
        {47, 53, 57},   {54, 61, 58},   {62, 68, 59},   {69, 76, 60},   {77, 83, 61},   {84, 91, 62},   {92, 98, 63},
        {99, 106, 64},  {107, 113, 65}, {114, 121, 66}, {122, 129, 67}, {130, 137, 68}, {138, 145, 69}, {146, 153, 70},
        {154, 162, 71}, {163, 170, 72}, {171, 179, 73}, {180, 188, 74}, {189, 197, 75}, {198, 206, 76}, {207, 215, 77},
        {216, 225, 78}, {226, 235, 79}, {236, 245, 80}, {246, 256, 81}, {257, 267, 82}, {268, 278, 83}, {279, 290, 84},
        {291, 302, 85}, {303, 315, 86}, {316, 328, 87}, {329, 344, 88}, {345, 357, 89}, {358, 374, 90}, {375, 391, 91},
        {392, 400, 92},
    };
    int wrong = 0;
    for (const auto& row : rows)
        for (int d = row[0]; d <= row[1]; ++d) wrong += elo::scoringProbability(d) != row[2];
    CHECK_EQ(wrong, 0);
    CHECK_EQ(elo::scoringProbability(735), 99);
    CHECK_EQ(elo::scoringProbability(736), 100);
    CHECK_EQ(elo::scoringProbability(-60), 58);
}

TEST(elo_fide_table_8_1_1) {
    CHECK_EQ(elo::ratingDifference(100), 800);
    CHECK_EQ(elo::ratingDifference(99), 677);
    CHECK_EQ(elo::ratingDifference(86), 309);
    CHECK_EQ(elo::ratingDifference(64), 102);
    CHECK_EQ(elo::ratingDifference(51), 7);
    CHECK_EQ(elo::ratingDifference(50), 0);
    CHECK_EQ(elo::ratingDifference(36), -102);
    CHECK_EQ(elo::ratingDifference(0), -800);
    for (int p = 0; p <= 100; ++p) CHECK_EQ(elo::ratingDifference(p), -elo::ratingDifference(100 - p));
}

TEST(elo_expected_score) {
    CHECK_EQ(elo::expectedScore(1500, 1500), 0.5);
    CHECK_EQ(elo::expectedScore(1504, 1500), 0.51);
    CHECK_EQ(elo::expectedScore(1500, 1700), 0.24);  // D 200: 0.76 for the higher-rated
    CHECK_EQ(elo::expectedScore(1700, 1500), 0.76);
    // FIDE: a difference above 400 points counts as 400.
    CHECK_EQ(elo::expectedScore(1500, 3500), elo::expectedScore(1500, 1900));
    CHECK_EQ(elo::expectedScore(2400, 800), 0.92);
}

TEST(elo_k_factor) {
    elo::Record r = rated(1500, 5);
    CHECK_EQ(elo::kFactor(r), 40);
    r.games = r.countedGames = 29;
    CHECK_EQ(elo::kFactor(r), 40);
    r.games = r.countedGames = 30;
    CHECK_EQ(elo::kFactor(r), 20);
    // The counted games set K, not the games played (some against unrated opponents, zero scores).
    r.games = 80;
    r.countedGames = 29;
    CHECK_EQ(elo::kFactor(r), 40);
    CHECK(r.provisional());
    r.countedGames = 30;
    CHECK(!r.provisional());
    r.peak = 2400;
    CHECK_EQ(elo::kFactor(r), 10);
    r.rating = 2300;  // stays 10 once 2400 has been reached
    CHECK_EQ(elo::kFactor(r), 10);
}

TEST(elo_rated_games) {
    elo::Record r = rated(1500, 40);
    CHECK_EQ(elo::ratingDelta(r, 1500, 1.0), 10);
    CHECK_EQ(elo::ratingDelta(r, 1600, 0.0), -7);  // PD 0.36: 20 x -0.36 = -7.2
    CHECK_EQ(elo::ratingDelta(r, 1560, 0.5), 2);   // PD 0.42: 20 x 0.08 = 1.6
    // K 10, D 35 (PD 0.55): 4.5 -> 5 and -5.5 -> -6, halves away from zero in exact hundredths.
    elo::Record senior = rated(2435, 200);
    CHECK_EQ(elo::ratingDelta(senior, 2400, 1.0), 5);
    CHECK_EQ(elo::ratingDelta(senior, 2400, 0.0), -6);
    elo::Change c = elo::applyResult(r, 1500, 1.0);
    CHECK_EQ(c.before, 1500);
    CHECK_EQ(c.after, 1510);
    CHECK_EQ(c.k, 20);
    CHECK_EQ(r.games, 41);
    CHECK_EQ(r.wins, 1);
    CHECK_EQ(r.peak, 1510);
    // Two established players at the same K exchange the same number of points.
    elo::Record a = rated(1620, 40), b = rated(1480, 40);
    elo::PairChange p = elo::applyPair(a, b, 0.0);
    CHECK_EQ(p.white.delta(), -14);  // D 140: PD 0.69, 20 x -0.69 = -13.8
    CHECK_EQ(p.black.delta(), 14);
    // Floor.
    elo::Record low = rated(elo::kFloor + 5, 50, 1500);
    elo::applyResult(low, 150, 0.0);
    CHECK_EQ(low.rating, elo::kFloor);
    CHECK_EQ(low.peak, 1500);
}

TEST(elo_initial_rating) {
    auto first = [](int n, int sum, int half) {
        elo::Record r;
        r.unratedGames = n;
        r.unratedOpponents = sum;
        r.unratedHalfPoints = half;
        return elo::initialRating(r);
    };
    // Ra = (sum + 2 x 1800) / 7, p = (score + 1) / 7: five draws against 1500 -> 1585.7 + 0.
    CHECK_EQ(first(5, 7500, 5), 1586);
    CHECK_EQ(first(5, 7500, 10), 1895);  // p 0.86, dp 309
    CHECK_EQ(first(5, 7500, 0), 1277);   // p 0.14, dp -309
    CHECK_EQ(first(5, 8000, 3), 1555);   // 1.5 / 5 against 1200..2000: 1657.1 - 102
    CHECK_EQ(first(5, 17500, 10), elo::kMaxInitialRating);  // five wins against Stockfish Max
    CHECK_EQ(first(5, 500, 0), 277);
}

TEST(elo_unrated_phase) {
    // A new player against Stockfish Novice (800): 3 wins, a draw, a loss.
    elo::Record r;
    CHECK(r.unrated());
    CHECK(r.provisional());
    const double scores[] = {1.0, 1.0, 0.5, 0.0};
    for (double s : scores) {
        elo::Change c = elo::applyResult(r, 800, s);
        CHECK_EQ(c.before, 1500);
        CHECK_EQ(c.after, 1500);  // the working rating until the fifth game
        CHECK_EQ(c.k, 0);
    }
    CHECK(r.unrated());
    CHECK_EQ(r.unratedGames, 4);
    CHECK_EQ(r.countedGames, 4);
    CHECK_EQ(r.unratedOpponents, 3200);
    CHECK_EQ(r.unratedHalfPoints, 5);
    CHECK_EQ(r.games, 4);
    // Ra = (4000 + 3600) / 7 = 1085.7, p = 4.5 / 7 = 0.64 (dp 102) -> 1188; the peak becomes it.
    elo::Change c = elo::applyResult(r, 800, 1.0);
    CHECK_EQ(c.before, 1500);
    CHECK_EQ(c.after, 1188);
    CHECK(r.rated);
    CHECK_EQ(r.peak, 1188);
    CHECK_EQ(r.countedGames, 5);
    CHECK_EQ(r.unratedGames, 0);
    CHECK_EQ(r.unratedOpponents, 0);
    CHECK_EQ(r.wins, 3);
    CHECK_EQ(r.draws, 1);
    CHECK_EQ(r.losses, 1);
    // Then K = 40 until 30 counted games, the unrated ones included: D 388, PD 0.91, 40 x 0.09 =
    // 3.6 -> +4.
    c = elo::applyResult(r, 800, 1.0);
    CHECK_EQ(c.k, 40);
    CHECK_EQ(c.after, 1192);
    CHECK_EQ(r.countedGames, 6);
    CHECK(r.provisional());
}

TEST(elo_zero_score_rule) {
    // FIDE 8.2.1: the losses before the first half point stay out of the unrated phase. Five
    // losses to Stockfish Max leave the player unrated (the sums would give 2200).
    elo::Record r;
    for (int i = 0; i < 5; ++i) {
        elo::Change c = elo::applyResult(r, 3500, 0.0);
        CHECK_EQ(c.after, 1500);
    }
    CHECK(r.unrated());
    CHECK_EQ(r.games, 5);
    CHECK_EQ(r.losses, 5);
    CHECK_EQ(r.countedGames, 0);
    CHECK_EQ(r.unratedGames, 0);
    CHECK_EQ(r.unratedOpponents, 0);
    CHECK(r.provisional());
    // Once the player has scored, the losses count: a draw and four losses against 1500:
    // Ra = 11100 / 7 = 1585.7, p = 1.5 / 7 = 0.21 (dp -230) -> 1356, the peak too.
    elo::Record s;
    const double scores[] = {0.0, 0.5, 0.0, 0.0, 0.0};
    for (double x : scores) elo::applyResult(s, 1500, x);
    CHECK(s.unrated());
    CHECK_EQ(s.unratedGames, 4);
    CHECK_EQ(s.unratedHalfPoints, 1);
    elo::Change c = elo::applyResult(s, 1500, 0.0);
    CHECK(s.rated);
    CHECK_EQ(c.after, 1356);
    CHECK_EQ(s.peak, 1356);
    CHECK_EQ(s.games, 6);
}

TEST(elo_zero_score_rule_disregards_the_winner_too) {
    // FIDE 8.2.1 also disregards the opponents' results against a zero score. A name that only
    // loses stays unrated and gives nothing to the names that beat it, however many: without the
    // rule, five wins against it gave a first rating of 1895 and the loser never moved.
    elo::Record booster;
    for (int n = 0; n < 3; ++n) {
        elo::Record fresh;
        for (int i = 0; i < 30; ++i) {
            elo::PairChange c = elo::applyPair(fresh, booster, 1.0);
            CHECK_EQ(c.white.after, 1500);
            CHECK_EQ(c.black.after, 1500);
        }
        CHECK(fresh.unrated());
        CHECK_EQ(fresh.games, 30);
        CHECK_EQ(fresh.wins, 30);
        CHECK_EQ(fresh.countedGames, 0);
        CHECK_EQ(fresh.unratedOpponents, 0);
    }
    CHECK(booster.unrated());
    CHECK_EQ(booster.games, 90);
    CHECK_EQ(booster.countedGames, 0);
    // Once the loser has scored (a draw), its losses count for both: it is rated after five
    // counted games and then loses points like anyone. A draw and four wins against 1500:
    // p = 11 / 14 = 0.79 (dp 230) -> 1816 for the winner, 1356 for the loser.
    elo::Record a, b;
    elo::applyPair(a, b, 0.5);
    for (int i = 0; i < 4; ++i) elo::applyPair(a, b, 1.0);
    CHECK(a.rated && b.rated);
    CHECK_EQ(a.rating, 1816);
    CHECK_EQ(b.rating, 1356);
    elo::PairChange c = elo::applyPair(a, b, 1.0);
    CHECK_EQ(c.black.delta(), -3);  // D 460 counted as 400: PD 0.92, 40 x -0.08 = -3.2
    CHECK_EQ(c.white.k, 40);
    // A player who has scored only against zero scores has scored: their loss counts, and so does
    // the win against them.
    elo::Record scorer, beginner, winner;
    elo::applyPair(scorer, beginner, 1.0);
    CHECK_EQ(scorer.countedGames, 0);
    elo::applyPair(winner, scorer, 1.0);
    CHECK_EQ(scorer.countedGames, 1);
    CHECK_EQ(scorer.unratedHalfPoints, 0);
    CHECK_EQ(winner.countedGames, 1);
    CHECK_EQ(winner.unratedOpponents, 1500);
}

TEST(elo_unrated_opponents) {
    // A rated player against an unrated one: no change, the game counts in the games and results,
    // not in the counted games.
    elo::Record est = rated(1700, 45), fresh;
    elo::PairChange c = elo::applyPair(est, fresh, 0.0);
    CHECK_EQ(c.white.delta(), 0);
    CHECK_EQ(c.white.k, 0);
    CHECK_EQ(est.games, 46);
    CHECK_EQ(est.countedGames, 45);
    CHECK_EQ(est.losses, 1);
    CHECK_EQ(fresh.unratedOpponents, 1700);
    CHECK_EQ(fresh.unratedHalfPoints, 2);
    // Two unrated players: each counts the other's working rating.
    elo::Record a, b;
    elo::applyPair(a, b, 0.5);
    CHECK_EQ(a.unratedOpponents, 1500);
    CHECK_EQ(b.unratedOpponents, 1500);
    CHECK_EQ(a.unratedHalfPoints, 1);
    CHECK(a.unrated() && b.unrated());
}

TEST(elo_ini_records) {
    // A file written before the unrated phase existed: a record with games is rated.
    IniFile old;
    old.setInt("player.elo", 1623);
    old.setInt("player.games", 12);
    old.setInt("player.wins", 7);
    old.setInt("player.peak", 1650);
    elo::Record r = elo::readRecord(old, "player");
    CHECK(r.rated);
    CHECK_EQ(r.rating, 1623);
    CHECK_EQ(r.games, 12);
    CHECK_EQ(r.countedGames, 12);  // all its games were rated
    CHECK_EQ(r.peak, 1650);
    CHECK_EQ(elo::applyResult(r, 1623, 1.0).k, 40);
    // A file written before the counted games existed: a rated record counts all its games.
    IniFile noCount;
    noCount.setInt("player.games", 40);
    noCount.setBool("player.rated", true);
    CHECK_EQ(elo::readRecord(noCount, "player").countedGames, 40);
    CHECK(!elo::readRecord(noCount, "player").provisional());
    // No record at all, or one without games: unrated at the initial rating.
    IniFile none;
    elo::Record n = elo::readRecord(none, "local_player_1");
    CHECK(n.unrated());
    CHECK_EQ(n.rating, elo::kInitialRating);
    CHECK_EQ(n.peak, elo::kInitialRating);
    // Round trip, the unrated phase included.
    elo::Record u;
    elo::applyResult(u, 1320, 1.0);
    elo::applyResult(u, 2000, 0.5);
    IniFile f;
    elo::writeRecord(f, "local_player_2", u);
    CHECK(f.has("local_player_2.unrated_opponents"));
    CHECK(sameRecord(elo::readRecord(f, "local_player_2"), u));
    elo::writeRecord(f, "player", r);
    CHECK(sameRecord(elo::readRecord(f, "player"), r));
    elo::Record fewer = rated(1600, 50);
    fewer.countedGames = 22;
    elo::writeRecord(f, "local_player_3", fewer);
    CHECK_EQ(f.getInt("local_player_3.counted_games", 0), 22);
    CHECK(sameRecord(elo::readRecord(f, "local_player_3"), fewer));
    // Inconsistent values are made consistent.
    IniFile bad;
    bad.setInt("player.elo", 20);
    bad.setInt("player.games", -3);
    bad.setInt("player.peak", 10);
    bad.setBool("player.rated", false);
    bad.setInt("player.unrated_games", 9);
    bad.setInt("player.unrated_half_points", 40);
    elo::Record b = elo::readRecord(bad, "player");
    CHECK_EQ(b.rating, elo::kFloor);
    CHECK_EQ(b.games, 0);
    CHECK_EQ(b.peak, elo::kFloor);
    CHECK_EQ(b.unratedGames, elo::kUnratedGames - 1);
    CHECK_EQ(b.unratedHalfPoints, 2 * (elo::kUnratedGames - 1));
    IniFile sums;
    sums.setInt("player.games", 40);
    sums.setBool("player.rated", true);
    sums.setInt("player.unrated_games", 3);
    sums.setInt("player.counted_games", 45);
    CHECK_EQ(elo::readRecord(sums, "player").unratedGames, 0);
    CHECK_EQ(elo::readRecord(sums, "player").countedGames, 40);  // at most the games
    IniFile phase;
    phase.setInt("player.games", 6);
    phase.setBool("player.rated", false);
    phase.setInt("player.unrated_games", 2);
    phase.setInt("player.counted_games", 6);
    CHECK_EQ(elo::readRecord(phase, "player").countedGames, 2);  // those of the unrated phase
}

TEST(elo_matches_server_vectors) {
    const std::string rel = "dedicated-server/test/fixtures/elo-vectors.json";
    std::string text = readRepoFile(rel);
    CHECK(!text.empty());
    if (text.empty()) {
        std::fprintf(stderr, "  %s not found (run from the repository root or set SCACELITH_SOURCE_DIR)\n", rel.c_str());
        return;
    }
    net::json::Limits limits;
    limits.maxBytes = 16 << 20;
    limits.maxElements = 1 << 20;
    Value v;
    std::string err;
    CHECK(net::json::parse(text, v, &err, limits));
    CHECK_EQ(int(v["seniorRating"].asInt()), elo::kSeniorRating);
    int wrong = 0;
    const Value& pd = v["pd"];
    CHECK_EQ(int(pd.size()), 801);
    for (size_t d = 0; d < pd.size(); ++d) wrong += elo::scoringProbability(int(d)) != int(pd[d].asInt());
    const Value& dp = v["dp"];
    CHECK_EQ(int(dp.size()), 101);
    for (size_t p = 0; p < dp.size(); ++p) wrong += elo::ratingDifference(int(p)) != int(dp[p].asInt());
    CHECK_EQ(wrong, 0);
    const Value& initial = v["initial"];
    CHECK(initial.size() >= 40);
    for (const Value& x : initial.items()) {
        elo::Record r;
        r.unratedGames = int(x["unratedGames"].asInt());
        r.unratedOpponents = int(x["unratedOpponents"].asInt());
        r.unratedHalfPoints = int(x["unratedHalfPoints"].asInt());
        if (elo::initialRating(r) != int(x["rating"].asInt())) {
            std::fprintf(stderr, "  initial rating %s: %d\n", x.dump().c_str(), elo::initialRating(r));
            ++wrong;
        }
    }
    const Value& games = v["games"];
    CHECK(games.size() >= 200);
    for (const Value& g : games.items()) {
        elo::Record w = recordOf(g["white"]), b = recordOf(g["black"]);
        elo::PairChange c = elo::applyPair(w, b, g["score"].asNumber());
        const Value& rw = g["result"]["white"];
        const Value& rb = g["result"]["black"];
        bool ok = c.white.before == int(rw["before"].asInt()) && c.white.after == int(rw["after"].asInt()) &&
                  c.white.k == int(rw["k"].asInt()) && sameRecord(w, recordOf(rw["record"])) &&
                  c.black.before == int(rb["before"].asInt()) && c.black.after == int(rb["after"].asInt()) &&
                  c.black.k == int(rb["k"].asInt()) && sameRecord(b, recordOf(rb["record"]));
        if (!ok) {
            std::fprintf(stderr, "  game %s: White %d -> %d (K %d), Black %d -> %d (K %d)\n", g.dump().c_str(), c.white.before,
                         c.white.after, c.white.k, c.black.before, c.black.after, c.black.k);
            ++wrong;
        }
    }
    CHECK_EQ(wrong, 0);
}
