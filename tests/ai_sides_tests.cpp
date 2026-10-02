// Two players sharing the embedded Stockfish (viewer mode: AI vs AI): per-side settings, hash
// clears when the side changes, draw offers between AIs, preset ratings.
#include "test.h"

#include "ai/behavior.h"
#include "ai/engine.h"

#include <chrono>
#include <thread>

namespace {

using SteadyClock = std::chrono::steady_clock;

double msSince(SteadyClock::time_point t) {
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - t).count();
}

std::string waitMove(ai::Engine& e, int timeoutMs, int* eval = nullptr) {
    auto t0 = SteadyClock::now();
    while (!e.moveReady()) {
        if (msSince(t0) > timeoutMs) return "timeout";
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return e.takeMove(eval);
}

}  // namespace

TEST(ai_preset_elo) {
    const auto& p = ai::presets();
    for (size_t i = 0; i + 1 < p.size(); ++i) CHECK_EQ(ai::presetElo(int(i)), p[i].approxElo);
    ai::EngineSettings custom;
    custom.limitStrength = true;
    custom.elo = 1950;
    CHECK_EQ(ai::presetElo(int(p.size()) - 1, custom), 1950);
    CHECK_EQ(ai::presetElo(-1, custom), 1950);
    CHECK_EQ(ai::presetElo(int(p.size()) - 1), 3500);  // default settings = full strength
}

TEST(ai_draw_offers) {
    // Equal and late: offer; too early, better, or clearly lost: no offer.
    CHECK(ai::detail::offersDraw(0, 60, -1));
    CHECK(ai::detail::offersDraw(-10, 80, 30));
    CHECK(!ai::detail::offersDraw(0, 58, -1));
    CHECK(!ai::detail::offersDraw(40, 90, -1));
    CHECK(ai::detail::offersDraw(-60, 40, -1));
    CHECK(!ai::detail::offersDraw(-60, 38, -1));
    CHECK(!ai::detail::offersDraw(-400, 90, -1));
    // Not again right after an offer.
    CHECK(!ai::detail::offersDraw(0, 80, 10));
    CHECK(ai::detail::offersDraw(0, 80, 20));
    // An offer made by one AI is judged by the other with acceptsDrawOffer: an equal position late
    // in the game is agreed, a better side declines.
    CHECK(ai::detail::acceptsDrawOffer(-5, 70));
    CHECK(!ai::detail::acceptsDrawOffer(150, 70));
}

TEST(ai_same_strength) {
    const auto& p = ai::presets();
    CHECK(ai::detail::sameStrength(p[3].settings, p[3].settings));
    CHECK(!ai::detail::sameStrength(p[3].settings, p[4].settings));
    CHECK(!ai::detail::sameStrength(p[0].settings, p[1].settings));
    ai::EngineSettings a = p[5].settings, b = a;
    b.humanize = !a.humanize;  // thinking time only: same moves
    CHECK(ai::detail::sameStrength(a, b));
}

#if defined(SCACELITH_HAS_STOCKFISH)

TEST(ai_engine_side_switch) {
    // One engine, two sides with different presets (viewer mode). Every change of side clears the
    // hash (the weak side must not see the strong side's deep entries); same settings do not.
    ai::Engine e;
    CHECK(e.start());
    CHECK(e.waitReady(30000));
    const auto& p = ai::presets();
    ai::EngineSettings weak = p[0].settings, strong = p[6].settings;  // Novice, Master
    weak.hashMB = strong.hashMB = 64;
    strong.moveTimeMs = 150;  // keep the test short
    weak.humanize = strong.humanize = false;
    e.newGame();
    std::vector<std::string> moves;
    auto t0 = SteadyClock::now();
    for (int ply = 0; ply < 12; ++ply) {
        e.configure(ply % 2 == 0 ? strong : weak);
        e.requestMove(moves, ai::ClockInfo{});
        std::string m = waitMove(e, 20000);
        CHECK(m.size() >= 4 && m != "timeout");
        if (m.size() < 4 || m == "timeout") break;
        moves.push_back(m);
    }
    double total = msSince(t0);
    // The first search follows ucinewgame (nothing to clear), then one clear per ply.
    CHECK_EQ(e.hashClears(), 11);
    std::fprintf(stderr, "  12 plies Master/Novice: %.0f ms (%d hash clears)\n", total, e.hashClears());
    // Same settings on both sides: no clear.
    e.newGame();
    int before = e.hashClears();
    moves.clear();
    for (int ply = 0; ply < 4; ++ply) {
        e.configure(weak);
        e.requestMove(moves, ai::ClockInfo{});
        std::string m = waitMove(e, 20000);
        CHECK(m.size() >= 4 && m != "timeout");
        if (m.size() < 4 || m == "timeout") break;
        moves.push_back(m);
    }
    CHECK_EQ(e.hashClears(), before);
    // Cost of a clear with a 64 MB table (what a side switch adds to a move).
    CHECK(e.sync(10000));
    double worst = 0.0, sum = 0.0;
    for (int i = 0; i < 5; ++i) {
        auto t1 = SteadyClock::now();
        e.clearHash();
        CHECK(e.sync(10000));
        double ms = msSince(t1);
        worst = std::max(worst, ms);
        sum += ms;
    }
    std::fprintf(stderr, "  hash clear (64 MB, 1 thread): %.1f ms average, %.1f ms worst\n", sum / 5.0, worst);
    CHECK(worst < 1000.0);
    e.shutdown();
}

TEST(ai_engine_analysis_shares_hash) {
    // Coach mode: the coach's own (weak) moves and full-strength analyses share one engine. An
    // analysis warms the hash, so the next move of a depth-capped weak setting starts from a
    // cleared table (its calibration); a UCI_Elo setting keeps its table, and an analysis never
    // counts as a change of side.
    ai::Engine e;
    CHECK(e.start());
    CHECK(e.waitReady(30000));
    ai::EngineSettings weak = ai::coachLevelSettings(1), club = ai::coachLevelSettings(4);
    weak.hashMB = club.hashMB = 64;
    weak.humanize = club.humanize = false;
    club.moveTimeMs = 100;  // keep the test short
    const std::vector<std::string> moves = {"e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5"};
    auto analyse = [&e, &moves] {
        ai::AnalysisRequest r;
        r.moves = moves;
        r.depth = 10;
        r.moveTimeMs = 0;
        const uint32_t id = e.requestAnalysis(r);
        auto t0 = SteadyClock::now();
        while (!e.analysisReady(id) && msSince(t0) < 20000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ai::Analysis a;
        return e.takeAnalysis(id, a) && a.ok;
    };
    auto move = [&e, &moves] {
        e.requestMove(moves, ai::ClockInfo{});
        return waitMove(e, 20000).size() >= 4;
    };
    e.configure(weak);
    e.newGame();
    CHECK(move());
    const int base = e.hashClears();
    CHECK(move());  // same settings: no clear
    CHECK_EQ(e.hashClears(), base);
    CHECK(analyse());
    CHECK_EQ(e.hashClears(), base);  // the analysis itself clears nothing
    CHECK(move());                   // the weak move after it starts from a cleared table
    CHECK_EQ(e.hashClears(), base + 1);
    CHECK(move());
    CHECK_EQ(e.hashClears(), base + 1);
    CHECK(analyse());                // two analyses in a row: still one clear
    CHECK(analyse());
    CHECK(move());
    CHECK_EQ(e.hashClears(), base + 2);
    // A UCI_Elo level: switching from the weak setting clears (as between two sides), analyses do not.
    e.configure(club);
    CHECK(move());
    CHECK_EQ(e.hashClears(), base + 3);
    CHECK(analyse());
    CHECK(move());
    CHECK_EQ(e.hashClears(), base + 3);
    e.shutdown();
}

#endif
