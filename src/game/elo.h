// The human player's Elo rating, as FIDE computes it (FIDE Rating Regulations effective from
// 1 March 2024; engine-free: compiled into the core library and unit-tested). Stored in
// Scacelith.ini ([player] elo, games, wins, draws, losses, peak, rated, counted_games,
// unrated_games, unrated_opponents, unrated_half_points; readRecord / writeRecord) and updated
// after every rated game against Stockfish; games watched in the viewer never change it. The
// dedicated server's dedicated-server/src/match/elo.js is a mirror of this file: both are checked
// against the same vectors (dedicated-server/test/fixtures/elo-vectors.json, tests/elo_tests.cpp),
// so a player's offline and online ratings follow the same rules to the last point.
//
//   expected score  PD from FIDE's table 8.1.2 for the rating difference D, D counting at most 400
//                   points (8.3.1); the higher-rated player gets PD, the lower 1 - PD
//   change          K x (score - PD), rounded to the nearest point (halves away from zero),
//                   computed in whole hundredths
//   K               40 until the player has kProvisionalGames (30) counted games (below), the
//                   counted games of the unrated phase included (the rating is shown as provisional
//                   until then), 20 afterwards, 10 once the player has reached 2400 (for good: the
//                   peak counts)
//   unrated phase   (8.2) a new record is unrated, with the working rating kInitialRating (shown,
//                   and used as the opponent value of another unrated player). Each counted game
//                   adds the opponent's rating and the score; after kUnratedGames (5) games
//                   Ru = Ra + dp(p), Ra = (sum of the opponents' ratings + 2 x 1800) / (n + 2),
//                   p = (score + 1) / (n + 2) rounded to hundredths (two hypothetical draws against
//                   1800-rated opponents), dp from FIDE's table 8.1.1, rounded to the nearest point
//                   and capped at 2200. The peak becomes Ru.
//   zero score      FIDE disregards an unrated player's zero score, and their opponents' results
//                   against them (8.2.1); here, one game at a time: a game lost by an unrated player
//                   who has not scored yet (no win, no draw) counts for neither player's rating (it
//                   counts in the games and results, not in the five games, the opponents' sum or
//                   the score of either side). Five losses to Stockfish Max would otherwise give a
//                   first rating of 2200, and a name that only loses would give a first rating to
//                   everyone who beats it. Ra stays FIDE's plain average: first games that mix the
//                   weakest and the strongest presets still give a first rating near their average
//                   (at most 2200), which the K = 40 games then correct.
//   unrated opponent a rated player's game against an unrated one leaves the rating unchanged (8.3:
//                   only games against rated opponents count), but counts in the games and results
//   counted games   the games that entered the rating (FIDE's rated games): those of the unrated
//                   phase that counted, then those against rated opponents. They set K and the
//                   provisional mark (the server's leaderboard too), not the games played.
//   floor           kFloor. FIDE's list starts at 1400, which makes no sense for a game played by
//                   beginners against weak Stockfish presets rated from 800.
//
// Departures from FIDE, needed here: games are rated one by one, each against the ratings before
// that game (FIDE rates monthly periods with the ratings fixed within the period, and caps K x
// games at 700 per period); a game between two unrated players counts for both, at the other's
// working rating (FIDE ignores it, but two new names in a rated hot-seat game could then never
// obtain a rating), unless it is a zero score; FIDE's K = 40 for players under 18 does not apply
// (no ages here).
//
// Stockfish seats are rated with ai::presetElo() (the preset's approxElo, or the estimate of the
// custom settings), a rated opponent. Rated hot-seat games (two people on one PC) use applyPair()
// on separate local records ([local_player_N] in the .ini).
#pragma once
#include <string>

class IniFile;

namespace elo {

constexpr int kInitialRating = 1500;
constexpr int kProvisionalGames = 30;        // K = 40 while fewer games have been counted
constexpr int kSeniorRating = 2400;          // K = 10 once this rating has been reached
constexpr int kMaxRatingGap = 400;           // FIDE: larger differences count as 400
constexpr int kFloor = 100;                  // ratings never drop below this
constexpr int kUnratedGames = 5;             // games of the unrated phase before the first rating
constexpr int kHypotheticalOpponent = 1800;  // the two hypothetical draws of the first rating
constexpr int kMaxInitialRating = 2200;      // highest first rating

struct Record {
    int rating = kInitialRating;  // the working rating while unrated
    int games = 0, wins = 0, draws = 0, losses = 0;
    int peak = kInitialRating;
    // The unrated phase: false until kUnratedGames counted games gave the first rating. Those
    // games add up their opponents' ratings and the score in half points (2 a win, 1 a draw); a
    // zero score is not counted (the zero-score rule).
    bool rated = false;
    // The games counted in the rating: those of the unrated phase (equal to unratedGames until the
    // first rating), then those against rated opponents.
    int countedGames = 0;
    int unratedGames = 0, unratedOpponents = 0, unratedHalfPoints = 0;
    bool unrated() const { return !rated; }
    bool provisional() const { return !rated || countedGames < kProvisionalGames; }
};

// PD of FIDE table 8.1.2 in hundredths (50..100) for a rating difference d (its absolute value,
// no 400-point cap).
int scoringProbability(int d);
// dp of FIDE table 8.1.1 for a percentage score p in hundredths (0..100): -800..800.
int ratingDifference(int p);
// Expected score of a player rated 'rating' against 'opponent' (0..1, table 8.1.2).
double expectedScore(int rating, int opponent);
// Development coefficient of a rated player's next game.
int kFactor(const Record& r);
// First rating of an unrated record from its accumulated games (Ra + dp, rounded, capped at
// kMaxInitialRating, floored at kFloor).
int initialRating(const Record& r);

struct Change {
    int before = 0, after = 0;
    int delta() const { return after - before; }
    double expected = 0.0;
    int k = 0;  // 0 when the K formula did not apply (unrated phase, or an unrated opponent)
};
// Applies one game (score: 1 win, 0.5 draw, 0 loss) against a rated opponent rated 'opponent'
// (Stockfish): updates the rating (or the unrated phase), the counters and the peak, and returns
// the change.
Change applyResult(Record& r, int opponent, double score);
// The same against another player's record before the game, which may be unrated.
Change applyResult(Record& r, const Record& opponent, double score);

// Rating change of the K formula for a rated record against a rated opponent, before the floor
// (e.g. to preview a result).
int ratingDelta(const Record& r, int opponent, double score);

// A rated game between two local players (hot-seat, docs/MULTIPLAYER_PLAN.md): each record is
// updated against the other's record before the game, with its own K factor (the two changes
// cancel out only when both K factors are equal). whiteScore: 1 White wins, 0.5 draw, 0 Black wins.
// These records are the local two-player ratings, never the rating against Stockfish ([player]).
struct PairChange {
    Change white, black;
};
PairChange applyPair(Record& white, Record& black, double whiteScore);

// The record stored in an .ini section ("player", "local_player_3"): keys elo, games, wins, draws,
// losses, peak, rated, counted_games, unrated_games, unrated_opponents, unrated_half_points. Missing
// keys take the defaults; a file written before the unrated phase existed has no 'rated' key, and
// its record is rated when it has games (they were rated then); a rated record without
// 'counted_games' counts all its games. Values are made consistent (floor, counts >= 0, peak >=
// rating, counted games at most the games, no unrated sums on a rated record) and bounded far above
// anything a game can reach (no overflow in the next game, whatever the file holds).
Record readRecord(const IniFile& ini, const std::string& section);
void writeRecord(IniFile& ini, const std::string& section, const Record& r);

}  // namespace elo
