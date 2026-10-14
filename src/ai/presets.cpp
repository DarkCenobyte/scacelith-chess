// Strength presets and Stockfish's strength-limiting mechanics.
//
// How Stockfish 19 weakens itself (search.h / search.cpp, struct Skill):
//  * "Skill Level" L (0..20) or UCI_LimitStrength + UCI_Elo (1320..3190). UCI_Elo is converted to
//    a fractional level with Stockfish's own fit (games against versions of the Stash engine,
//    anchored to CCRL Blitz; unchanged since Stockfish 16):
//        e = (elo - 1320) / 1870,  L = clamp(((37.2473 e - 40.8525) e + 22.2943) e - 0.311438, 0, 19)
//    so UCI_Elo 1320 == Skill Level 0, the weakest setting Stockfish offers.
//  * With L < 20 the search runs at least 4 principal variations (max(4, MultiPV)) and, once
//    iteration 1 + int(L) is complete (or after the last iteration, if a depth cap stops the search
//    earlier), picks one of them at random, biased towards the better ones by a "weakness" of
//    120 - 2L: score differences are compressed ~16x at L = 0 (~13x at L = 1), and random noise up to
//    ~0.6-0.7 pawn is added (~0.4 in Stockfish 16, so the same settings now play weaker).
//
// Presets from 1500 up use UCI_Elo as is, with normal (deep) searches like Stockfish's calibration:
// 1 s per move, untimed and on the clock (engine.cpp; Stockfish's time management may stop it
// sooner in bullet and time trouble). Measured against Stockfish 16 at the same UCI_Elo (1500,
// 1800, 2100), they play as they did within the measurement's ~40 Elo, so their labels stay.
//
// Below 1320 we need Stockfish's other knobs. What the engine-vs-engine calibration showed (Stockfish
// 19 self-play and matches against the Stockfish 16 presets, adjudicated by a referee engine;
// README.scacelith.md has the numbers, tools/sf_match.py plays the games):
//  * Depth cap: capping the search at the pick iteration ("go depth 1" at level 0) costs ~105 Elo.
//    Not because the pick changes depth but because the shallow multi-PV scores then come from a
//    hash table that never saw a deep search. A cap below the pick iteration (level 1, which picks
//    after iteration 2, at "go depth 1") makes the pick on shallower scores: ~100 more per missing
//    ply. A node cap is no alternative: one that interrupts an iteration leaves unsearched moves and
//    makes the answer arbitrary, not human-like.
//  * MultiPV: the random pick is made among max(4, MultiPV) candidates, so a wider list lets the
//    engine choose clearly worse moves more often, like a player who overlooks threats. 5 costs
//    ~85 Elo, 6 ~150, 7 ~230, 9 ~300, and every further line still costs a little (20: ~470).
//  * Level: under a depth cap the level still counts, as the pick keeps (8 + 2L) / 128 of the score
//    differences against the noise: level 1 at depth 1 is ~95 Elo stronger than level 0 at depth 1
//    with the same MultiPV.
#include "ai/behavior.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

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

// Measured effects (Elo, see the file comment), applied to any handicapped settings as an
// approximation. The scale is the labels': Stockfish 16's Skill Level 0 at normal search time = 1320,
// on which the weak presets were first calibrated. Stockfish 19's levels 0 and 1 play ~65-120 Elo
// below Stockfish 16's (the drift fades by UCI_Elo 1500), so kDepthCapPenalty adds ~80 of that to the
// ~105 a depth cap at or below the pick iteration costs: every depth-capped preset is at level 0 or 1.
// An uncapped Custom setting at level 0 or 1 is therefore rated ~65-120 too high (its rating; the
// time-pressure drop starts from what it plays, detail::presetScaleElo).
constexpr int kDepthCapPenalty = 185;
constexpr int kShortOfPickPenalty = 97;  // per ply a depth cap stops short of the pick iteration
// Index = MultiPV (<= 4 is Stockfish's own minimum); 20 and more count as 20.
constexpr int kMultiPVPenalty[] = {0,   0,   0,   0,   0,   85,  152, 228, 284, 303, 330,
                                   353, 376, 392, 408, 419, 430, 439, 448, 457, 466};

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
    return s;
}

}  // namespace

int Engine::estimateElo(const EngineSettings& s) {
    const double level = detail::skillLevel(s);
    if (level < 0.0) return 3500;  // full strength, 1 thread (CCRL-like scale)
    int elo;
    if (s.limitStrength) {
        elo = std::clamp(s.elo, 1320, 3190);
    } else if (level >= 19.0) {
        // Beyond the fit's range (UCI_Elo 3190 is level 18.4), and like any level still handicapped
        // by the depth cap and MultiPV below: a time-pressure drop (weakenedSettings) starts from
        // this estimate and must not trade Skill 19 at depth 1 for a deep UCI_Elo 3100 search.
        elo = 3250;
    } else {
        // Invert Stockfish's (monotonic) Elo -> level fit by bisection.
        double lo = 1320, hi = 3190;
        for (int i = 0; i < 40; ++i) {
            double mid = 0.5 * (lo + hi);
            (detail::levelFromElo(int(mid)) < level ? lo : hi) = mid;
        }
        elo = int(0.5 * (lo + hi));
    }
    const int pickDepth = 1 + int(level);  // Stockfish picks after this iteration (or the last one)
    if (s.depth > 0 && s.depth <= pickDepth) elo -= kDepthCapPenalty + kShortOfPickPenalty * (pickDepth - s.depth);
    if (s.multiPV > 4) {
        const int n = int(sizeof(kMultiPVPenalty) / sizeof(kMultiPVPenalty[0]));
        elo -= kMultiPVPenalty[std::min(s.multiPV, n - 1)];
    }
    return std::max(elo, 100);
}

// Each weak preset was matched against the Stockfish 16 preset it replaces (the labels' scale):
// Novice -2, Beginner -6, Casual +13 Elo (+-15-21), so they keep their strengths: Novice ~750-840,
// Beginner ~1010, Casual ~1175. Stockfish 19 against Stockfish 16 at the same UCI_Elo (250 ms per
// move): 1500 -22, 1800 +12, 2100 +22 (+-35-40). The labels are Stockfish's (engine, CCRL-anchored)
// scale, not FIDE ratings.
const std::vector<Preset>& presets() {
    static const std::vector<Preset> list = {
        {"Novice", "Just learned the moves: hangs pieces and misses simple threats.", 800, shallowSkill(0, 1, 9)},
        {"Beginner", "Knows the basics, but still blunders material regularly.", 1000, shallowSkill(1, 1, 6)},
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

// Coach mode levels: the coach plays a little below the top of the student's band, on the presets'
// scale (the labels above), from the same measurements:
//  0  rules lesson (scripted)  Skill 0, depth 1, MultiPV 20  ~650   estimate 1320-185-466 = 669; S0 d1 MPV 20
//                                                                   is ~135 below the Novice (-449 vs -315)
//  1  First steps (600-900)    Skill 0, depth 1, MultiPV 12  ~750   estimate 759; ~75 below the Novice
//                                                                   (S0 d1: MPV 12 -389, MPV 9 -315)
//  2  Beginner (900-1200)      the Beginner preset           ~1000  measured -6 against its Stockfish 16 self
//  3  Improver (1200-1500)     UCI_Elo 1400                  ~1300  see below
//  4  Club player (1500-1800)  UCI_Elo 1650                  ~1650  Stockfish 19 vs 16: 1500 -22, 1800 +12
//  5  Strong club (1800-2100)  UCI_Elo 1950                  ~1950  1800 +12, 2100 +22
//  6  Expert (2100+)           UCI_Elo 2300                  ~2300  Stockfish's calibration
// Level 3: UCI_Elo 1320 (Skill Level 0 without a depth cap) would play at only ~1200 on this scale
// (Stockfish 19's level 0 at a normal search time scores -120 against Stockfish 16's, the scale's
// 1320), about as the Casual preset, while UCI_Elo 1500 (level 1.49) is within ~20 of its label.
// Between the two the handicap is driven by the fractional level (~185 per level here): ~1300
// needs level ~0.55, i.e. UCI_Elo ~1400 (level 0.57). estimateElo() says 1400: like any uncapped
// setting at level 0-1 it is ~100 too high (see kDepthCapPenalty).
// The UCI_Elo levels search 1 s per move in the coach's untimed games (engine.cpp), as the
// presets do; the depth-capped ones answer in milliseconds.
EngineSettings coachLevelSettings(int level) {
    switch (std::clamp(level, 0, kCoachLevels - 1)) {
    case 0: return shallowSkill(0, 1, 20);
    case 1: return shallowSkill(0, 1, 12);
    case 2: return shallowSkill(1, 1, 6);  // = presets()[1], the Beginner
    case 3: return limited(1400);
    case 4: return limited(1650);
    case 5: return limited(1950);
    default: return limited(2300);
    }
}

int coachLevelElo(int level) {
    static const int elo[kCoachLevels] = {650, 750, 1000, 1300, 1650, 1950, 2300};
    return elo[std::clamp(level, 0, kCoachLevels - 1)];
}

}  // namespace ai

// "Dynamic Elo by time": a player short of time plays worse, so the engine is weakened when its own
// clock runs low (ClockInfo::dynamicElo, an option of the New Game and Watch a Game pages; engine.cpp
// applies it to each move search). It never needs this to avoid losing on time: the humanised
// thinking time already shrinks to almost nothing in time trouble (humanize.cpp) and the
// handicapped searches stop within a second.
//  * The time that counts is the time left plus ten increments (its next ten moves).
//  * Low time from one minute down, or earlier in rapid and blitz games (base + 60 x increment under
//    one hour, as chess::TimeControl counts them): at a fifth of the base time, at most three
//    minutes; in bullet (base under two minutes) at half the base time. A gradual drop, up to
//    kLowTimeEloDrop at the threshold of time trouble.
//  * Time trouble below 15 s (in bullet: below half the low-time threshold): up to
//    kTimeTroubleEloDrop with an empty clock.
// The sizes are a judgement, not a measurement: players lose much more than a pawn's worth of
// accuracy in the last seconds, and the drop has to be felt across a whole preset step (the
// presets are 200-300 Elo apart).
namespace ai::detail {

int timePressureEloDrop(int64_t remainingMs, int64_t incMs, int64_t baseMs) {
    const double left = double(std::max<int64_t>(0, remainingMs)) + 10.0 * double(std::max<int64_t>(0, incMs));
    const double base = double(std::max<int64_t>(0, baseMs));
    double lowTime = 60000.0;
    if (baseMs + 60 * std::max<int64_t>(0, incMs) < 3600000)
        lowTime = std::clamp(base / 5.0, std::min(60000.0, base / 2.0), 180000.0);
    if (lowTime <= 0.0 || left >= lowTime) return 0;
    const double trouble = std::min(15000.0, lowTime / 2.0);
    if (left >= trouble) return int(std::lround(kLowTimeEloDrop * (lowTime - left) / (lowTime - trouble)));
    return int(std::lround(kLowTimeEloDrop + (kTimeTroubleEloDrop - kLowTimeEloDrop) * (trouble - left) / trouble));
}

namespace {

// UCI_Elo for a strength on the presets' scale from 1200 to 1500. Stockfish's own calibration holds
// from 1500 (presets.cpp); below, levels 0-1 play weaker than their number: UCI_Elo 1400 ~1300 (coach
// level 3), 1320 (Skill Level 0) ~1200.
int uciEloFor(int elo) {
    if (elo >= 1500) return std::min(elo, 3190);
    if (elo >= 1300) return 1400 + (elo - 1300) / 2;
    return 1320 + (std::max(elo, 1200) - 1200) * 4 / 5;
}

// The inverse: what UCI_Elo `uci` plays on the presets' scale.
int eloForUci(int uci) {
    if (uci >= 1500) return uci;
    if (uci >= 1400) return 1300 + (uci - 1400) * 2;
    return 1200 + (std::max(uci, 1320) - 1320) * 5 / 4;
}

// The depth-capped handicaps measured below 1200 (README.scacelith.md), strongest first: Casual,
// Skill 1 depth 1 MultiPV 5, Beginner, Skill 0 depth 1 MultiPV 7, Novice, coach levels 1 and 0.
const std::vector<EngineSettings>& shallowLadder() {
    static const std::vector<EngineSettings> ladder = {shallowSkill(1, 2, 5),  shallowSkill(1, 1, 5),
                                                       shallowSkill(1, 1, 6),  shallowSkill(0, 1, 7),
                                                       shallowSkill(0, 1, 9),  shallowSkill(0, 1, 12),
                                                       shallowSkill(0, 1, 20)};
    return ladder;
}

}  // namespace

int presetScaleElo(const EngineSettings& s) {
    const int elo = Engine::estimateElo(s);
    const double level = skillLevel(s);
    if (level < 0.0 || (s.depth > 0 && s.depth <= 1 + int(level))) return elo;  // full strength or depth-capped
    EngineSettings bare = s;  // the same level without the MultiPV penalty
    bare.depth = 0;
    bare.multiPV = 1;
    const int uci = Engine::estimateElo(bare);
    return std::max(elo - (uci - eloForUci(uci)), 100);
}

EngineSettings weakenedSettings(const EngineSettings& s, int drop) {
    if (drop <= 0 || s.nodes > 0 || s.moveTimeMs > 0) return s;
    const double level = skillLevel(s);
    if (level < 0.0 && s.depth > 0) return s;
    // The drop starts from what `s` plays: an uncapped Skill Level 0 or 1 plays ~60-120 below its
    // estimate, which a small drop would otherwise turn into a stronger UCI_Elo. A UCI_Elo setting
    // starts from its UCI_Elo (below 1500 the guard below keeps it from playing stronger).
    const int nominal = s.limitStrength ? Engine::estimateElo(s) : presetScaleElo(s);
    const int target = nominal - drop;
    EngineSettings w;
    if (target >= 1200) {
        if (level < 0.0 && target > 3190) return s;  // nothing between full strength and UCI_Elo 3190
        w = limited(uciEloFor(target));
        if (s.limitStrength && s.depth <= 0 && s.multiPV <= 4 && w.elo >= s.elo) return s;
    } else {
        // The ladder's step nearest to the target, if it is nearer than `s` itself.
        int bestGap = drop;
        bool found = false;
        for (const EngineSettings& c : shallowLadder()) {
            const int e = Engine::estimateElo(c);
            if (e >= nominal) continue;
            const int gap = std::abs(e - target);
            if (gap < bestGap) {
                bestGap = gap;
                w = c;
                found = true;
            }
        }
        if (!found) return s;
    }
    w.threads = s.threads;
    w.hashMB = s.hashMB;
    w.useClock = s.useClock;
    w.humanize = s.humanize;
    return w;
}

}  // namespace ai::detail
