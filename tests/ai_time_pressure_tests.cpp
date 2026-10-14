// The opponent's pace and its "Dynamic Elo by time": how much weaker it plays when its clock runs
// low (presets.cpp), the settings it then searches with, and the embedded engine applying them
// (hash clears) and capping handicapped searches in timed games (engine.cpp).
#include "test.h"

#include "ai/behavior.h"
#include "ai/engine.h"

#include <chrono>
#include <thread>
#include <vector>

namespace {

using SteadyClock = std::chrono::steady_clock;

int msSince(SteadyClock::time_point t) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - t).count());
}

std::string waitMove(ai::Engine& e, int timeoutMs) {
    auto t0 = SteadyClock::now();
    while (!e.moveReady()) {
        if (msSince(t0) > timeoutMs) return "timeout";
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return e.takeMove();
}

const ai::EngineSettings& preset(const char* name) {
    for (const auto& p : ai::presets())
        if (std::string(p.name) == name) return p.settings;
    static ai::EngineSettings none;
    return none;
}

constexpr int64_t kSec = 1000, kMin = 60 * kSec;

}  // namespace

TEST(ai_time_pressure_drop) {
    using ai::detail::timePressureEloDrop;
    using ai::detail::kLowTimeEloDrop;
    using ai::detail::kTimeTroubleEloDrop;
    // Classical 90+0 (a standard game): nothing above one minute, then a gradual drop to
    // kLowTimeEloDrop at 15 s, and kTimeTroubleEloDrop with an empty clock.
    CHECK_EQ(timePressureEloDrop(10 * kMin, 0, 90 * kMin), 0);
    CHECK_EQ(timePressureEloDrop(60 * kSec, 0, 90 * kMin), 0);
    CHECK_EQ(timePressureEloDrop(15 * kSec, 0, 90 * kMin), kLowTimeEloDrop);
    CHECK_EQ(timePressureEloDrop(0, 0, 90 * kMin), kTimeTroubleEloDrop);
    const int mid = timePressureEloDrop(37500, 0, 90 * kMin);
    CHECK(mid > 60 && mid < 90);
    // The increment counts ten times: 30 s left with 30 s per move is no time trouble.
    CHECK_EQ(timePressureEloDrop(30 * kSec, 30 * kSec, 90 * kMin), 0);
    // Rapid starts earlier: 10+0 at a fifth of the base time (2 min), 30+0 capped at 3 min.
    CHECK_EQ(timePressureEloDrop(121 * kSec, 0, 10 * kMin), 0);
    CHECK(timePressureEloDrop(100 * kSec, 0, 10 * kMin) > 0);
    CHECK(timePressureEloDrop(170 * kSec, 0, 30 * kMin) > 0);
    CHECK_EQ(timePressureEloDrop(181 * kSec, 0, 30 * kMin), 0);
    // Blitz 3+0 and 5+0: from one minute, like a standard game.
    CHECK_EQ(timePressureEloDrop(61 * kSec, 0, 3 * kMin), 0);
    CHECK(timePressureEloDrop(59 * kSec, 0, 5 * kMin) > 0);
    // Bullet 1+0: from half the base time; time trouble below 15 s.
    CHECK_EQ(timePressureEloDrop(31 * kSec, 0, 1 * kMin), 0);
    CHECK_EQ(timePressureEloDrop(15 * kSec, 0, 1 * kMin), kLowTimeEloDrop);
    // Never negative, never more than kTimeTroubleEloDrop, never smaller with less time.
    for (int64_t base : {1 * kMin, 3 * kMin, 10 * kMin, 15 * kMin, 30 * kMin, 90 * kMin})
        for (int64_t inc : {int64_t(0), 2 * kSec, 10 * kSec}) {
            int last = 0;
            for (int64_t left = 5 * kMin; left >= -kSec; left -= 250) {
                const int d = timePressureEloDrop(left, inc, base);
                CHECK(d >= last && d <= kTimeTroubleEloDrop);
                last = d;
            }
        }
}

TEST(ai_weakened_settings) {
    using ai::Engine;
    using ai::detail::weakenedSettings;
    const ai::EngineSettings& club = preset("Club Player");
    // No drop, or settings whose strength is unknown: unchanged.
    CHECK(ai::detail::sameStrength(weakenedSettings(club, 0), club));
    ai::EngineSettings nodes;
    nodes.nodes = 100000;
    CHECK(ai::detail::sameStrength(weakenedSettings(nodes, 300), nodes));
    ai::EngineSettings depthOnly;
    depthOnly.depth = 8;
    CHECK(ai::detail::sameStrength(weakenedSettings(depthOnly, 300), depthOnly));
    // UCI_Elo from 1200 up on the presets' scale (UCI_Elo 1400 ~1300).
    ai::EngineSettings w = weakenedSettings(club, 150);
    CHECK(w.limitStrength && w.depth == 0 && w.elo > 1400 && w.elo < 1450);
    w = weakenedSettings(preset("Grandmaster"), 400);
    CHECK(w.limitStrength && w.elo == 2300);
    // Below 1200, the nearest measured depth-capped handicap: Club - 400 = 1100 -> Skill 1,
    // depth 1, MultiPV 5 (~1077).
    w = weakenedSettings(club, 400);
    CHECK(!w.limitStrength && w.skillLevel == 1 && w.depth == 1 && w.multiPV == 5);
    // The Novice goes down to the rules lesson's level; a small drop leaves it as it is.
    w = weakenedSettings(preset("Novice"), 150);
    CHECK(w.skillLevel == 0 && w.depth == 1 && w.multiPV == 20);
    CHECK(ai::detail::sameStrength(weakenedSettings(preset("Novice"), 20), preset("Novice")));
    // Full strength only weakens below UCI_Elo's top (3190).
    const ai::EngineSettings& max = preset("Stockfish Max");
    CHECK(ai::detail::sameStrength(weakenedSettings(max, 150), max));
    w = weakenedSettings(max, 400);
    CHECK(w.limitStrength && w.elo == 3100);
    // Every preset, every drop: never stronger, weaker as the drop grows, the user's threads,
    // hash and humanised time kept.
    for (const auto& p : ai::presets()) {
        ai::EngineSettings s = p.settings;
        s.threads = 3;
        s.hashMB = 32;
        s.humanize = false;
        int last = Engine::estimateElo(s);
        for (int drop = 0; drop <= ai::detail::kTimeTroubleEloDrop; drop += 10) {
            const ai::EngineSettings ws = weakenedSettings(s, drop);
            const int e = Engine::estimateElo(ws);
            CHECK(e <= last);
            CHECK(ws.threads == 3 && ws.hashMB == 32 && !ws.humanize);
            last = e;
        }
    }
}

TEST(ai_weakened_custom_skill19_depth_cap) {
    // Audit N16: a Custom opponent at Skill Level 19 capped at depth 1 (no Elo limit) on a 5+0 clock
    // with 15 s left (-150 Elo). Its estimate counts the cap, like Skill 18 at depth 1, so the drop
    // gives a depth-capped handicap a little weaker, not UCI_Elo 3100 without a depth cap.
    using ai::Engine;
    using ai::detail::weakenedSettings;
    ai::EngineSettings s;
    s.skillLevel = 19;
    s.depth = 1;
    ai::EngineSettings s18 = s;
    s18.skillLevel = 18;
    const int nominal = Engine::estimateElo(s);
    CHECK(std::abs(nominal - Engine::estimateElo(s18)) <= 100);
    const int drop = ai::detail::timePressureEloDrop(15 * kSec, 0, 5 * kMin);
    CHECK_EQ(drop, ai::detail::kLowTimeEloDrop);
    const ai::EngineSettings w = weakenedSettings(s, drop);
    CHECK(!w.limitStrength);
    CHECK(w.depth >= 1 && w.depth <= 2);
    CHECK(Engine::estimateElo(w) < nominal);
    // Every Custom level (0-19) capped at or below its pick iteration, any MultiPV, every drop: a
    // drop that leaves less than 1200 keeps a depth-capped handicap, or UCI_Elo 1320 (~1200) while it
    // is the nearest step (down to 1187), never a stronger one.
    for (int level = 0; level < 20; ++level)
        for (int depth = 1; depth <= 1 + level; ++depth)
            for (int multiPV : {1, 5, 9, 20}) {
                ai::EngineSettings c;
                c.skillLevel = level;
                c.depth = depth;
                c.multiPV = multiPV;
                const int e = Engine::estimateElo(c);
                for (int d : {10, ai::detail::kLowTimeEloDrop, ai::detail::kTimeTroubleEloDrop}) {
                    if (e - d >= 1200) continue;
                    const ai::EngineSettings cw = weakenedSettings(c, d);
                    if (cw.limitStrength) {
                        CHECK_EQ(cw.elo, 1320);
                        CHECK(e - d >= 1187);
                    } else {
                        CHECK(cw.depth > 0);
                    }
                    CHECK(ai::detail::presetScaleElo(cw) <= e);
                }
            }
}

TEST(ai_weakened_custom_low_skill_uncapped) {
    // A Custom opponent at Skill Level 0 or 1 without a depth cap: rated 1320 / 1444 by Stockfish's
    // calibration, it plays ~1200 / ~1388 on the presets' scale (the UCI_Elo levels below 1500 play
    // weaker than their number). A drop starts from there: a small one gave it a stronger UCI_Elo.
    using ai::Engine;
    using ai::detail::presetScaleElo;
    using ai::detail::weakenedSettings;
    ai::EngineSettings s0;
    s0.skillLevel = 0;
    CHECK_EQ(Engine::estimateElo(s0), 1320);
    CHECK_EQ(presetScaleElo(s0), 1200);
    // 5+0 with 50 s left (-33): the Casual's settings (~1175), not UCI_Elo 1387 (~1290).
    const int drop = ai::detail::timePressureEloDrop(50 * kSec, 0, 5 * kMin);
    CHECK_EQ(drop, 33);
    CHECK(ai::detail::sameStrength(weakenedSettings(s0, drop), preset("Casual")));
    ai::EngineSettings s1;
    s1.skillLevel = 1;
    CHECK_EQ(presetScaleElo(s1), 1388);
    ai::EngineSettings w = weakenedSettings(s1, drop);
    CHECK(w.limitStrength && w.elo > 1400 && w.elo < Engine::estimateElo(s1));
    CHECK(std::abs(presetScaleElo(w) - (1388 - drop)) <= 2);
    // Skill 0-1 without a depth cap (none, or one past the pick iteration), any MultiPV, every drop:
    // never stronger, weaker as the drop grows, and within half the widest step between the
    // measured settings (Beginner ~1010 to Skill 0 depth 1 MultiPV 7 ~907) of the target while one
    // of them reaches it.
    const int floor = Engine::estimateElo(ai::coachLevelSettings(0));
    for (int level = 0; level <= 1; ++level)
        for (int depth : {0, 4})
            for (int multiPV : {1, 5, 9, 20}) {
                ai::EngineSettings c;
                c.skillLevel = level;
                c.depth = depth;
                c.multiPV = multiPV;
                const int nominal = presetScaleElo(c);
                CHECK(nominal < Engine::estimateElo(c));
                int last = nominal;
                for (int d = 0; d <= ai::detail::kTimeTroubleEloDrop; d += 5) {
                    const int e = presetScaleElo(weakenedSettings(c, d));
                    CHECK(e <= last);
                    if (nominal - d >= floor) CHECK(std::abs(e - (nominal - d)) <= 55);
                    last = e;
                }
            }
    // Every other setting starts from its estimate as before: the presets (and so the coach's
    // depth-capped levels), Skill 2 and up.
    for (const auto& p : ai::presets()) CHECK_EQ(presetScaleElo(p.settings), Engine::estimateElo(p.settings));
    for (int level = 2; level < 20; ++level) {
        ai::EngineSettings c;
        c.skillLevel = level;
        CHECK_EQ(presetScaleElo(c), Engine::estimateElo(c));
    }
}

TEST(ai_weakened_custom_uci_elo_low) {
    // Audit N17: a Custom opponent at UCI_Elo 1320 with a depth limit of 4 (past the pick iteration,
    // so no cap) on a 5+0 clock with 50 s left (-33). It plays as Skill Level 0 at depth 4 (~1200)
    // and now drops the same way, to the Casual's settings, not to UCI_Elo 1389 without the limit.
    using ai::Engine;
    using ai::detail::presetScaleElo;
    using ai::detail::weakenedSettings;
    ai::EngineSettings uci;
    uci.limitStrength = true;
    uci.elo = 1320;
    uci.depth = 4;
    ai::EngineSettings skill;
    skill.skillLevel = 0;
    skill.depth = 4;
    CHECK_EQ(presetScaleElo(uci), 1200);
    CHECK_EQ(presetScaleElo(uci), presetScaleElo(skill));
    const int drop = ai::detail::timePressureEloDrop(50 * kSec, 0, 5 * kMin);
    CHECK_EQ(drop, 33);
    ai::EngineSettings w = weakenedSettings(uci, drop);
    CHECK(ai::detail::sameStrength(w, weakenedSettings(skill, drop)));
    CHECK(ai::detail::sameStrength(w, preset("Casual")));
    // UCI_Elo 1400 (~1300) weakens from the first drops, as Club Player does from 1500.
    ai::EngineSettings u1400;
    u1400.limitStrength = true;
    u1400.elo = 1400;
    w = weakenedSettings(u1400, drop);
    CHECK(w.limitStrength && w.elo < 1400);
    CHECK(std::abs(presetScaleElo(w) - (1300 - drop)) <= 2);
    // Every Custom setting (UCI_Elo or Skill Level, any depth limit and MultiPV), every drop: never
    // stronger on the presets' scale, and never a higher UCI_Elo than its own. (Every drop one by one
    // in ai_weakened_larger_drop_never_stronger.)
    std::vector<ai::EngineSettings> all;
    for (int elo = 1320; elo <= 3190; elo += 10) {
        ai::EngineSettings c;
        c.limitStrength = true;
        c.elo = elo;
        all.push_back(c);
    }
    for (int level = 0; level < 20; ++level) {
        ai::EngineSettings c;
        c.skillLevel = level;
        all.push_back(c);
    }
    for (ai::EngineSettings c : all)
        for (int depth : {0, 1, 2, 3, 4, 8, 20})
            for (int multiPV : {1, 5, 9, 20}) {
                c.depth = depth;
                c.multiPV = multiPV;
                const int nominal = presetScaleElo(c);
                for (int d = 0; d <= ai::detail::kTimeTroubleEloDrop; d += 10) {
                    const ai::EngineSettings cw = weakenedSettings(c, d);
                    CHECK(presetScaleElo(cw) <= nominal);
                    if (c.limitStrength && cw.limitStrength) CHECK(cw.elo <= c.elo);
                }
            }
}

TEST(ai_weakened_larger_drop_never_stronger) {
    // Audit N18: a Custom opponent at UCI_Elo 1400 with a depth limit of 1 (~1215) dropped 15 to
    // UCI_Elo 1320 (~1200), the last step above 1200, but 16 back to its own settings: the steps
    // below 1200 left UCI_Elo 1320 out and the ladder's top, the Casual (~1175), was further from
    // the target than its own settings. Same at UCI_Elo 1330 (~1212) for drops 12 and 13.
    using ai::detail::presetScaleElo;
    using ai::detail::weakenedSettings;
    ai::EngineSettings u1400;
    u1400.limitStrength = true;
    u1400.elo = 1400;
    u1400.depth = 1;
    CHECK_EQ(presetScaleElo(u1400), 1215);
    ai::EngineSettings u1320;
    u1320.limitStrength = true;
    u1320.elo = 1320;
    CHECK(ai::detail::sameStrength(weakenedSettings(u1400, 15), u1320));
    CHECK(ai::detail::sameStrength(weakenedSettings(u1400, 16), u1320));
    ai::EngineSettings u1330;
    u1330.limitStrength = true;
    u1330.elo = 1330;
    CHECK_EQ(presetScaleElo(u1330), 1212);
    CHECK(ai::detail::sameStrength(weakenedSettings(u1330, 12), u1320));
    CHECK(ai::detail::sameStrength(weakenedSettings(u1330, 13), u1320));
    // Club Player's drop goes the same way: UCI_Elo 1320 while it is the nearest step (a target of
    // 1187-1200), then the Casual.
    const ai::EngineSettings club = preset("Club Player");
    CHECK(ai::detail::sameStrength(weakenedSettings(club, 313), u1320));
    CHECK(ai::detail::sameStrength(weakenedSettings(club, 314), preset("Casual")));
    // Every Custom setting of the grid above (UCI_Elo in steps of 100 from 1600, where it is played
    // as is), each drop one by one: never stronger than a smaller drop, on the presets' scale.
    std::vector<ai::EngineSettings> all;
    for (int elo = 1320; elo <= 3190; elo += elo < 1600 ? 10 : 100) {
        ai::EngineSettings c;
        c.limitStrength = true;
        c.elo = std::min(elo, 3190);
        all.push_back(c);
    }
    for (int level = 0; level < 20; ++level) {
        ai::EngineSettings c;
        c.skillLevel = level;
        all.push_back(c);
    }
    int rises = 0;
    for (ai::EngineSettings c : all)
        for (int depth : {0, 1, 2, 3, 4, 8, 20})
            for (int multiPV : {1, 5, 9, 20}) {
                c.depth = depth;
                c.multiPV = multiPV;
                int previous = presetScaleElo(c);
                for (int d = 1; d <= ai::detail::kTimeTroubleEloDrop; ++d) {
                    const int now = presetScaleElo(weakenedSettings(c, d));
                    if (now > previous) ++rises;
                    previous = now;
                }
            }
    CHECK_EQ(rises, 0);
}

#if defined(SCACELITH_HAS_STOCKFISH)

TEST(ai_timed_handicap_search_capped) {
    // A UCI_Elo preset on a 15+10 clock: Stockfish's time management alone would think 20-60 s
    // over this middlegame move; the handicapped search stops after 1 s.
    ai::Engine e;
    CHECK(e.start());
    CHECK(e.waitReady(30000));
    ai::EngineSettings s = preset("Club Player");
    s.hashMB = 16;
    e.configure(s);
    e.newGame();
    ai::ClockInfo clock;
    clock.timed = true;
    clock.whiteMs = clock.blackMs = 15 * kMin;
    clock.whiteIncMs = clock.blackIncMs = 10 * kSec;
    clock.moveOverheadMs = 1500;
    clock.baseMs = 15 * kMin;
    const std::vector<std::string> moves = {"e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "g8f6",
                                            "e1g1", "f8e7", "f1e1", "b7b5", "a4b3", "d7d6", "c2c3", "e8g8"};
    auto t0 = SteadyClock::now();
    e.requestMove(moves, clock);
    const std::string m = waitMove(e, 30000);
    const int ms = msSince(t0);
    std::fprintf(stderr, "  Club Player, 15+10, move 9: %s in %d ms\n", m.c_str(), ms);
    CHECK(m.size() >= 4 && m != "timeout");
    CHECK(ms < 4000);
    e.shutdown();
}

TEST(ai_engine_time_pressure) {
    // With "Dynamic Elo by time" the move searches of a short clock use weakened settings. Going
    // from UCI_Elo (deep searches) to a depth-capped handicap clears the hash once; changes within
    // one kind, or back to the configured settings, clear nothing (the same player).
    ai::Engine e;
    CHECK(e.start());
    CHECK(e.waitReady(30000));
    ai::EngineSettings s = preset("Club Player");
    s.hashMB = 16;
    s.moveTimeMs = 0;
    e.configure(s);
    e.newGame();
    ai::ClockInfo clock;
    clock.timed = true;
    clock.whiteMs = clock.blackMs = 5 * kMin;
    clock.moveOverheadMs = 100;
    clock.baseMs = 5 * kMin;
    clock.dynamicElo = true;
    const std::vector<std::string> moves = {"e2e4", "e7e5", "g1f3", "b8c6"};
    auto play = [&] {
        e.requestMove(moves, clock);
        return waitMove(e, 20000);
    };
    CHECK(play().size() >= 4);  // plenty of time: the Club Player itself
    CHECK(e.sync(10000));
    const int base = e.hashClears();
    clock.whiteMs = 40 * kSec;  // low time: UCI_Elo lowered, still a deep search
    CHECK(play().size() >= 4);
    CHECK(e.sync(10000));
    CHECK_EQ(e.hashClears(), base);
    clock.whiteMs = 2 * kSec;  // time trouble: a depth-capped handicap, a cleared hash
    CHECK(play().size() >= 4);
    CHECK(e.sync(10000));
    CHECK_EQ(e.hashClears(), base + 1);
    CHECK(play().size() >= 4);
    CHECK(e.sync(10000));
    CHECK_EQ(e.hashClears(), base + 1);
    clock.whiteMs = 5 * kMin;  // the clock is back (increment): the configured settings again
    CHECK(play().size() >= 4);
    CHECK(e.sync(10000));
    CHECK_EQ(e.hashClears(), base + 1);
    // Without the option the clock changes nothing.
    clock.dynamicElo = false;
    clock.whiteMs = 2 * kSec;
    CHECK(play().size() >= 4);
    CHECK(e.sync(10000));
    CHECK_EQ(e.hashClears(), base + 1);
    e.shutdown();
}

#endif
