// Strength presets and Stockfish 16's strength-limiting mechanics.
//
// How Stockfish 16 weakens itself (search.cpp, struct Skill):
//  * "Skill Level" L (0..20) or UCI_LimitStrength + UCI_Elo (1320..3190). UCI_Elo is converted to
//    a fractional level with Stockfish's own fit (anchored to CCRL, measured at 60s+0.6s):
//        e = (elo - 1320) / 1870,  L = clamp(((37.2473 e - 40.8525) e + 22.2943) e - 0.311438, 0, 19)
//    so UCI_Elo 1320 == Skill Level 0, the weakest setting Stockfish offers.
//  * With L < 20 the search runs at least 4 principal variations (max(4, MultiPV)) and, once
//    iteration 1 + int(L) is complete, picks one of them at random, biased towards the better ones
//    by a "weakness" of 120 - 2L (score differences are compressed ~16x at L = 0, random noise up to
//    ~1.2 pawns is added).
//
// Presets from 1500 up use UCI_Elo as is, with normal (deep) searches like Stockfish's calibration:
// the clock in timed games, 1 s per move in untimed ones (engine.cpp).
//
// Below 1320 we need Stockfish's other knobs. What the engine-vs-engine calibration showed (Stockfish
// 16 self-play, depth-1 games adjudicated by a referee engine; README.scacelith.md has the numbers):
//  * Depth cap: capping Skill Level 0 at "go depth 1" costs ~156 Elo. Not because the pick changes
//    depth (level 0 always picks after iteration 1) but because the shallow multi-PV scores then come
//    from a hash table that never saw a deep search. A node cap is no alternative: one that
//    interrupts iteration 1 leaves unsearched moves and makes the answer arbitrary, not human-like.
//  * MultiPV: the random pick is made among max(4, MultiPV) candidates, so a wider list lets the
//    engine choose clearly worse moves more often, like a player who overlooks threats. 6 costs
//    ~150 Elo, 7 ~220, and it saturates around 10 (~300).
#include "ai/behavior.h"

#include <algorithm>

namespace ai {
namespace detail {

namespace {
double levelFromElo(int elo) {
    const double e = double(std::clamp(elo, 1320, 3190) - 1320) / (3190 - 1320);
    return std::clamp((((37.2473 * e - 40.8525) * e + 22.2943) * e - 0.311438), 0.0, 19.0);
}
}  // namespace

double skillLevel(const EngineSettings& s) {
    if (s.limitStrength) return levelFromElo(s.elo);
    if (s.skillLevel < 20) return double(std::max(0, s.skillLevel));
    return -1.0;
}

}  // namespace detail

namespace {

// Measured effects on Skill Level 0 (Elo, see the file comment); applied to any handicapped
// settings as an approximation.
constexpr int kDepthCapPenalty = 156;
// Index = MultiPV (<= 4 is Stockfish's own minimum); 10 and more saturate.
constexpr int kMultiPVPenalty[] = {0, 0, 0, 0, 0, 72, 148, 222, 243, 286, 295};

EngineSettings limited(int elo) {
    EngineSettings s;
    s.limitStrength = true;
    s.elo = elo;
    return s;
}

// A low Skill Level searching one or two plies (+ quiescence) only.
EngineSettings shallowSkill(int level, int depth, int multiPV) {
    EngineSettings s;
    s.skillLevel = level;
    s.depth = depth;
    s.multiPV = multiPV;
    s.hashMB = 16;  // a shallow search barely touches the hash table
    return s;
}

}  // namespace

int Engine::estimateElo(const EngineSettings& s) {
    const double level = detail::skillLevel(s);
    if (level < 0.0) return 3500;  // full strength, 1 thread (CCRL-like scale)
    int elo;
    if (s.limitStrength) {
        elo = std::clamp(s.elo, 1320, 3190);
    } else {
        if (level >= 19.0) return 3250;
        // Invert Stockfish's (monotonic) Elo -> level fit by bisection.
        double lo = 1320, hi = 3190;
        for (int i = 0; i < 40; ++i) {
            double mid = 0.5 * (lo + hi);
            (detail::levelFromElo(int(mid)) < level ? lo : hi) = mid;
        }
        elo = int(0.5 * (lo + hi));
    }
    if (s.depth > 0 && s.depth <= 1 + int(level)) elo -= kDepthCapPenalty;
    if (s.multiPV > 4) {
        const int n = int(sizeof(kMultiPVPenalty) / sizeof(kMultiPVPenalty[0]));
        elo -= kMultiPVPenalty[std::min(s.multiPV, n - 1)];
    }
    return std::max(elo, 100);
}

// Measured against Skill Level 0 at normal search time (= 1320): Casual ~1165, Beginner ~1015,
// Novice ~750-840 (two measurement chains), Club Player (UCI_Elo 1500) +109 over Skill Level 0 in
// fast games. The labels are Stockfish's (engine, CCRL-anchored) scale, not FIDE ratings.
const std::vector<Preset>& presets() {
    static const std::vector<Preset> list = {
        {"Novice", "Just learned the moves: hangs pieces and misses simple threats.", 800, shallowSkill(0, 1, 9)},
        {"Beginner", "Knows the basics, but still blunders material regularly.", 1000, shallowSkill(0, 1, 5)},
        {"Casual", "A relaxed evening opponent: sensible moves, frequent mistakes.", 1200, shallowSkill(1, 2, 5)},
        {"Club Player", "Solid club player who punishes obvious blunders.", 1500, limited(1500)},
        {"Advanced", "Strong club player with good tactical vision.", 1800, limited(1800)},
        {"Expert", "Tournament expert: rarely errs, converts advantages.", 2100, limited(2100)},
        {"Master", "Master strength: deep calculation and fine positional play.", 2400, limited(2400)},
        {"Grandmaster", "Grandmaster level: extremely hard to beat.", 2700, limited(2700)},
        {"Stockfish Max", "Stockfish 19 at full strength. Good luck.", 3500, EngineSettings{}},
        {"Custom", "Your own engine settings.", 0, EngineSettings{}},
    };
    return list;
}

int presetElo(int index, const EngineSettings& custom) {
    const auto& list = presets();
    if (index >= 0 && index + 1 < int(list.size())) return list[size_t(index)].approxElo;
    return Engine::estimateElo(custom);
}

}  // namespace ai
