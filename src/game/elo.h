// The human player's Elo rating, FIDE style (engine-free: compiled into the core library and
// unit-tested). Stored in Scacelith.ini ([player] elo, games, wins, draws, losses, peak) and
// updated after every rated game against Stockfish; games watched in the viewer never change it.
//
//   expected score  E = 1 / (1 + 10^((Ro - Rp) / 400)), a rating difference counting at most
//                   400 points (FIDE rating regulations 8.3.1)
//   change          round(K * (score - E)), score 1 / 0.5 / 0
//   K               40 for the first 30 games (provisional rating), 20 afterwards, 10 once the
//                   player has reached 2400 (for good, as FIDE does with the published rating)
//
// Stockfish seats are rated with ai::presetElo() (the preset's approxElo, or the estimate of the
// custom settings), which is also what a human game is rated against. Rated hot-seat games (two
// people on one PC) use applyPair() on separate local records ([local_player_N] in the .ini).
#pragma once

namespace elo {

constexpr int kInitialRating = 1500;
constexpr int kProvisionalGames = 30;   // K = 40 while fewer games have been played
constexpr int kSeniorRating = 2400;     // K = 10 once this rating has been reached
constexpr int kMaxRatingGap = 400;      // FIDE: larger differences count as 400
constexpr int kFloor = 100;             // ratings never drop below this

struct Record {
    int rating = kInitialRating;
    int games = 0, wins = 0, draws = 0, losses = 0;
    int peak = kInitialRating;
    bool provisional() const { return games < kProvisionalGames; }
};

// Expected score of a player rated 'rating' against 'opponent' (0..1).
double expectedScore(int rating, int opponent);
// Development coefficient for the next game.
int kFactor(const Record& r);

struct Change {
    int before = 0, after = 0;
    int delta() const { return after - before; }
    double expected = 0.0;
    int k = 0;
};
// Applies one game (score: 1 win, 0.5 draw, 0 loss) against an opponent rated 'opponent':
// updates the rating, the counters and the peak, and returns the change.
Change applyResult(Record& r, int opponent, double score);

// Rating change without applying it (e.g. to preview a result).
int ratingDelta(const Record& r, int opponent, double score);

// A rated game between two local players (hot-seat, docs/MULTIPLAYER_PLAN.md): each record is
// updated against the other's rating before the game, with its own K factor (the two changes
// cancel out only when both K factors are equal). whiteScore: 1 White wins, 0.5 draw, 0 Black wins.
// These records are the local two-player ratings, never the rating against Stockfish ([player]).
struct PairChange {
    Change white, black;
};
PairChange applyPair(Record& white, Record& black, double whiteScore);

}  // namespace elo
