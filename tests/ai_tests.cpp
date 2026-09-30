// Tests for src/ai: presets, humanised timing, draw decisions and the embedded Stockfish 19
// (start-up, moves, evaluation, stop, new games, shutdown/restart, instruction-set variants).
#include "test.h"

#include "ai/behavior.h"
#include "ai/engine.h"
#include "ai/uci_host.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace {

using SteadyClock = std::chrono::steady_clock;

int msSince(SteadyClock::time_point t) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - t).count());
}

std::string waitMove(ai::Engine& e, int timeoutMs, int* eval = nullptr) {
    auto t0 = SteadyClock::now();
    while (!e.moveReady()) {
        if (msSince(t0) > timeoutMs) return "timeout";
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return e.takeMove(eval);
}

bool waitEval(ai::Engine& e, int timeoutMs) {
    auto t0 = SteadyClock::now();
    while (!e.evalReady()) {
        if (msSince(t0) > timeoutMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// Minimal board replay for checking engine answers: the move must be well-formed and start on a
// piece of the side to move and not land on one of its own pieces.
bool plausibleMove(const std::vector<std::string>& moves, const std::string& m) {
    if (m.size() < 4 || m.size() > 5) return false;
    auto sq = [](const std::string& s, size_t at) {
        int f = s[at] - 'a', r = s[at + 1] - '1';
        return (f >= 0 && f < 8 && r >= 0 && r < 8) ? r * 8 + f : -1;
    };
    char b[64];
    const char* back = "RNBQKBNR";
    for (int f = 0; f < 8; ++f) {
        b[f] = back[f]; b[8 + f] = 'P'; b[48 + f] = 'p'; b[56 + f] = char(back[f] + 32);
        for (int r = 2; r < 6; ++r) b[r * 8 + f] = '.';
    }
    for (const auto& mv : moves) {
        int from = sq(mv, 0), to = sq(mv, 2);
        char pc = b[from];
        if ((pc == 'P' || pc == 'p') && (from & 7) != (to & 7) && b[to] == '.') b[(from & ~7) | (to & 7)] = '.';
        if ((pc == 'K' || pc == 'k') && std::abs((to & 7) - (from & 7)) == 2) {
            int rank = from & ~7, rf = rank + ((to & 7) == 6 ? 7 : 0), rt = rank + ((to & 7) == 6 ? 5 : 3);
            b[rt] = b[rf]; b[rf] = '.';
        }
        if (mv.size() == 5) pc = pc == 'P' ? char(mv[4] - 32) : mv[4];
        b[to] = pc; b[from] = '.';
    }
    const bool whiteToMove = moves.size() % 2 == 0;
    int from = sq(m, 0), to = sq(m, 2);
    if (from < 0 || to < 0 || from == to) return false;
    auto own = [&](char c) { return c != '.' && (whiteToMove ? (c >= 'A' && c <= 'Z') : (c >= 'a' && c <= 'z')); };
    return own(b[from]) && !own(b[to]);
}

const std::vector<std::string> kItalian = {"e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5", "c2c3", "g8f6", "d2d4", "e5d4"};

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

ai::EngineSettings fast() {
    ai::EngineSettings s;
    s.depth = 6;
    s.hashMB = 16;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Pure behaviour
// ---------------------------------------------------------------------------------------------

TEST(ai_presets_ordered) {
    const auto& p = ai::presets();
    CHECK(p.size() >= 10);
    CHECK_EQ(std::string(p.back().name), std::string("Custom"));
    const char* expected[] = {"Novice", "Beginner", "Casual", "Club Player", "Advanced", "Expert", "Master",
                              "Grandmaster", "Stockfish Max"};
    for (size_t i = 0; i < 9 && i < p.size(); ++i) CHECK_EQ(std::string(p[i].name), std::string(expected[i]));
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        CHECK(p[i].description && std::strlen(p[i].description) > 10);
        if (i + 2 < p.size()) {
            CHECK(p[i].approxElo < p[i + 1].approxElo);
            CHECK(ai::Engine::estimateElo(p[i].settings) < ai::Engine::estimateElo(p[i + 1].settings));
        }
        // the label and the estimate agree within 150 Elo
        if (i + 2 < p.size()) CHECK(std::abs(ai::Engine::estimateElo(p[i].settings) - p[i].approxElo) <= 150);
    }
}

TEST(ai_skill_mapping) {
    ai::EngineSettings s;
    CHECK(ai::detail::skillLevel(s) < 0.0);  // full strength
    s.skillLevel = 0;
    CHECK(ai::detail::skillLevel(s) == 0.0);
    s.skillLevel = 10;
    CHECK(ai::detail::skillLevel(s) == 10.0);
    s.limitStrength = true;
    s.elo = 1320;
    CHECK(ai::detail::skillLevel(s) == 0.0);
    s.elo = 3190;
    CHECK(ai::detail::skillLevel(s) > 18.0 && ai::detail::skillLevel(s) < 19.0);
    s.elo = 1500;
    CHECK(ai::detail::skillLevel(s) > 1.4 && ai::detail::skillLevel(s) < 1.6);
    CHECK(std::abs(ai::Engine::estimateElo(s) - 1500) <= 1);
    ai::EngineSettings sk;
    sk.skillLevel = 5;
    s.elo = ai::Engine::estimateElo(sk);  // inverse mapping round-trips
    CHECK(std::abs(ai::detail::skillLevel(s) - 5.0) < 0.05);
}

TEST(ai_think_time) {
    using ai::detail::humanThinkTimeMs;
    ai::EngineSettings strong, weak = ai::presets().front().settings;
    ai::ClockInfo untimed;
    CHECK_EQ(humanThinkTimeMs([] { ai::EngineSettings s; s.humanize = false; return s; }(), untimed, 30, 30, false, false, 0), 0);
    for (double z : {-3.0, -1.0, 0.0, 1.0, 3.0}) {
        int t = humanThinkTimeMs(strong, untimed, 40, 35, false, false, z);
        CHECK(t >= 2000 && t <= 12000);
        int w = humanThinkTimeMs(weak, untimed, 40, 35, false, false, z);
        CHECK(w >= 600 && w <= t);
        CHECK(humanThinkTimeMs(strong, untimed, 40, 1, false, false, z) <= 1500);           // only move
        CHECK(humanThinkTimeMs(strong, untimed, 2, 20, false, false, z) < t);               // opening
        CHECK(humanThinkTimeMs(strong, untimed, 40, 35, false, true, z) < t);               // recapture
        CHECK(humanThinkTimeMs(strong, untimed, 40, 5, true, false, z) < t);                // check evasion
    }
    // timed 5+0 game at move 20: about remaining/35, never more than 10% of the usable time
    ai::ClockInfo c;
    c.timed = true;
    c.whiteMs = c.blackMs = 180000;
    for (double z : {-2.0, 0.0, 2.5}) {
        int t = humanThinkTimeMs(strong, c, 40, 30, false, false, z);
        CHECK(t >= 700 && t <= int((180000 - 1000 - 500) * 0.10) + 1);
    }
    // time trouble never flags: 3 s left with 1 s move overhead
    c.whiteMs = 3000;
    CHECK(humanThinkTimeMs(strong, c, 80, 30, false, false, 2.5) <= int((3000 - 1500) * 0.10) + 1);
    c.whiteMs = 1200;  // less than overhead + margin: move at once
    CHECK_EQ(humanThinkTimeMs(strong, c, 80, 30, false, false, 2.5), 0);
    // black's clock is used when black is to move
    c.whiteMs = 1200;
    c.blackMs = 600000;
    CHECK(humanThinkTimeMs(strong, c, 41, 30, false, false, 0) > 5000);
}

TEST(ai_think_time_never_flags) {
    // Over many clock states, thinking plus the move overhead always leaves time on the clock and
    // never takes more than 10% of what is usable (plus most of the increment).
    using ai::detail::humanThinkTimeMs;
    int checked = 0, bad = 0;
    for (const auto& p : ai::presets())
        for (int64_t remaining : {300, 1500, 4000, 15000, 60000, 180000, 900000})
            for (int64_t inc : {0, 2000})
                for (int overhead : {0, 1000, 3000})
                    for (int ply : {0, 1, 20, 61, 150})
                        for (double z : {-2.5, 0.0, 2.5}) {
                            ai::ClockInfo c;
                            c.timed = true;
                            c.whiteMs = c.blackMs = remaining;
                            c.whiteIncMs = c.blackIncMs = inc;
                            c.moveOverheadMs = overhead;
                            int t = humanThinkTimeMs(p.settings, c, ply, 25, false, false, z);
                            double usable = double(remaining - overhead - 500);
                            bool ok = t >= 0 && (usable <= 0 ? t == 0 : t <= usable * 0.10 + inc * 0.8 + 1);
                            ok = ok && (t == 0 || t + overhead < remaining);
                            bad += !ok;
                            ++checked;
                        }
    CHECK(checked > 1000);
    CHECK_EQ(bad, 0);
}

TEST(ai_draw_decisions) {
    using ai::detail::acceptsDrawOffer;
    CHECK(!acceptsDrawOffer(150, 80));
    CHECK(!acceptsDrawOffer(100000, 80));
    CHECK(acceptsDrawOffer(-150, 10));
    CHECK(acceptsDrawOffer(-100000, 50));
    CHECK(!acceptsDrawOffer(0, 10));   // too early
    CHECK(!acceptsDrawOffer(0, 40));   // equal but not late
    CHECK(acceptsDrawOffer(0, 70));    // dead equal, late
    CHECK(acceptsDrawOffer(-40, 40));  // slightly worse after the opening
    CHECK(!acceptsDrawOffer(50, 90));  // slightly better: play on
}

TEST(ai_recapture_detection) {
    using ai::detail::isRecapture;
    std::vector<std::string> m = {"e2e4", "d7d5", "e4d5"};
    CHECK(isRecapture(m, "d8d5"));
    CHECK(!isRecapture(m, "g8f6"));
    CHECK(!isRecapture({"e2e4", "d7d5"}, "e4d5"));                          // last move was quiet
    CHECK(isRecapture({"e2e4", "a7a6", "e4e5", "d7d5", "e5d6"}, "c7d6"));  // en passant, then recapture
}

// ---------------------------------------------------------------------------------------------
// Embedded Stockfish
// ---------------------------------------------------------------------------------------------
#if defined(SCACELITH_HAS_STOCKFISH)

TEST(ai_engine_start_and_move) {
    std::streambuf* coutBefore = std::cout.rdbuf();
    std::streambuf* cinBefore = std::cin.rdbuf();
    ai::Engine e;
    auto t0 = SteadyClock::now();
    CHECK(e.start());
    CHECK(e.available());
    CHECK(e.waitReady(30000));
    std::fprintf(stderr, "  Stockfish init (uci + isready): %d ms\n", msSince(t0));
    e.configure(fast());
    e.newGame();
    t0 = SteadyClock::now();
    e.requestMove({}, ai::ClockInfo{});
    int eval = 12345;
    std::string m = waitMove(e, 20000, &eval);
    std::fprintf(stderr, "  startpos depth 6: %s (%d cp) in %d ms (search %d ms)\n", m.c_str(), eval, msSince(t0),
                 e.lastSearchMs());
    CHECK(plausibleMove({}, m));
    CHECK(eval > -150 && eval < 150);
    CHECK(!e.moveReady());
    CHECK_EQ(e.takeMove(), std::string());

    e.requestMove(kItalian, ai::ClockInfo{});
    m = waitMove(e, 20000);
    CHECK(plausibleMove(kItalian, m));
    e.shutdown();
    CHECK(!e.available());
    CHECK(std::cout.rdbuf() == coutBefore);
    CHECK(std::cin.rdbuf() == cinBefore);
}

TEST(ai_engine_eval) {
    ai::Engine e;
    CHECK(e.start());
    e.configure(ai::presets().front().settings);  // evaluation ignores the handicap
    e.requestEval({});
    CHECK(waitEval(e, 20000));
    int cp = e.takeEval();
    std::fprintf(stderr, "  startpos eval %d cp\n", cp);
    CHECK(cp > -100 && cp < 100);
    // Scholar's mate pattern, white to move: Qxf7#
    e.requestEval({"e2e4", "e7e5", "f1c4", "b8c6", "d1h5", "g8f6"});
    CHECK(waitEval(e, 20000));
    CHECK_EQ(e.takeEval(), 100000);
    // black to move, a clean piece down after 1.e4 e5 2.Nf3 Qh4?? 3.Nxh4
    e.requestEval({"e2e4", "e7e5", "g1f3", "d8h4", "f3h4"});
    CHECK(waitEval(e, 20000));
    CHECK(e.takeEval() < -500);
    // eval and move requests queue behind each other
    e.configure(fast());
    e.requestMove(kItalian, ai::ClockInfo{});
    e.requestEval(kItalian);
    std::string m = waitMove(e, 20000);
    CHECK(plausibleMove(kItalian, m));
    CHECK(waitEval(e, 20000));
    e.shutdown();
}

TEST(ai_every_preset_moves) {
    ai::Engine e;
    CHECK(e.start());
    CHECK(e.waitReady(30000));
    ai::ClockInfo clock;  // blitz clock: the full-strength presets use their time management
    clock.timed = true;
    clock.whiteMs = clock.blackMs = 8000;
    clock.moveOverheadMs = 100;
    for (const auto& p : ai::presets()) {
        e.configure(p.settings);
        e.newGame();
        auto t0 = SteadyClock::now();
        e.requestMove(kItalian, clock);
        int eval = 0;
        std::string m = waitMove(e, 20000, &eval);
        std::fprintf(stderr, "  %-14s -> %-6s %6d cp %5d ms  (think %d ms)\n", p.name, m.c_str(), eval, msSince(t0),
                     e.thinkTimeMs(clock, int(kItalian.size()), 30, false));
        CHECK(plausibleMove(kItalian, m));
    }
    e.shutdown();
}

TEST(ai_special_moves) {
    // Stockfish 19 ends the whole process on a "position" command it cannot replay, so every kind of
    // move must reach it in the notation it expects: the four castlings, en passant by both sides
    // and the four promotions, each as the last move of a request.
    const std::string promotion = "e2e4 d7d5 e4d5 c7c6 d5c6 g8f6 c6b7 c8d7 b7a8";
    const std::vector<std::vector<std::string>> lines = {
        words("d2d4 d7d5 g1f3 b8c6 e2e3 c8f5 f1e2 d8d7 e1g1"),       // White castles kingside
        words("d2d4 d7d5 g1f3 b8c6 e2e3 c8f5 f1e2 d8d7 e1g1 e8c8"),  // Black castles queenside
        words("d2d4 g8f6 b1c3 g7g6 c1f4 f8g7 d1d2 e8g8"),            // Black castles kingside
        words("d2d4 g8f6 b1c3 g7g6 c1f4 f8g7 d1d2 e8g8 e1c1"),       // White castles queenside
        words("e2e4 a7a6 e4e5 d7d5 e5d6"),                           // White takes en passant
        words("g1f3 d7d5 b1c3 d5d4 e2e4 d4e3"),                      // Black takes en passant
        words(promotion + "q"), words(promotion + "r"), words(promotion + "b"), words(promotion + "n"),
    };
    ai::Engine e;
    CHECK(e.start());
    e.configure(fast());
    for (const auto& line : lines) {
        e.requestMove(line, ai::ClockInfo{});
        std::string m = waitMove(e, 20000);
        CHECK(plausibleMove(line, m));
        if (!plausibleMove(line, m)) std::fprintf(stderr, "  after %s: '%s'\n", line.back().c_str(), m.c_str());
    }
    e.requestEval(lines.back());  // evaluations send the same "position" command
    CHECK(waitEval(e, 20000));
    e.shutdown();
}

TEST(ai_illegal_line_not_sent) {
    // A move list the game's rules reject never reaches Stockfish, which would end the process: the
    // request fails (empty move, neutral evaluation) and the engine keeps working.
    const std::vector<std::vector<std::string>> bad = {
        {"e2e5"},                     // not a legal move
        {"e2e4", "e7e5", "e1g1"},     // castling through pieces
        {"e2e4", "d7d5", "e4e5", "f7f5", "h2h3", "b8c6", "e5f6"},  // en passant one move too late
        {"e2e4", "e7e5", "e4e5"},     // blocked pawn
        {"e2e4q"},                    // promotion letter on a quiet move
        {"e2e4", ""},                 // empty move
        {"e2e4", "e7e5 g1f3"},        // two moves in one
    };
    ai::Engine e;
    CHECK(e.start());
    e.configure(fast());
    for (const auto& line : bad) {
        e.requestMove(line, ai::ClockInfo{});
        CHECK_EQ(waitMove(e, 5000), std::string());
        e.requestEval(line);
        CHECK(waitEval(e, 5000));
        CHECK_EQ(e.takeEval(), 0);
    }
    e.requestMove(kItalian, ai::ClockInfo{});
    CHECK(plausibleMove(kItalian, waitMove(e, 20000)));
    e.shutdown();
}

TEST(ai_stop_search) {
    ai::Engine e;
    CHECK(e.start());
    ai::EngineSettings s;
    s.moveTimeMs = 60000;
    e.configure(s);
    e.requestMove(kItalian, ai::ClockInfo{});
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!e.moveReady());
    auto t0 = SteadyClock::now();
    e.stopSearch();
    std::string m = waitMove(e, 2000);
    std::fprintf(stderr, "  stopped after 300 ms: %s, stop latency %d ms\n", m.c_str(), msSince(t0));
    CHECK(plausibleMove(kItalian, m));
    // stop immediately after the request: still a searched (depth >= 1) move
    e.requestMove({"e2e4", "e7e5"}, ai::ClockInfo{});
    e.stopSearch();
    m = waitMove(e, 5000);
    CHECK(plausibleMove({"e2e4", "e7e5"}, m));
    e.shutdown();
}

TEST(ai_supersede_and_new_games) {
    ai::Engine e;
    CHECK(e.start());
    e.configure(fast());
    // a second request replaces the first one: only its answer arrives
    e.requestMove({}, ai::ClockInfo{});
    e.requestMove({"e2e4"}, ai::ClockInfo{});
    std::string m = waitMove(e, 20000);
    CHECK(plausibleMove({"e2e4"}, m));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!e.moveReady());
    for (int i = 0; i < 5; ++i) {
        e.newGame();
        std::vector<std::string> moves;
        if (i % 2) moves = {"d2d4"};
        e.requestMove(moves, ai::ClockInfo{});
        m = waitMove(e, 20000);
        CHECK(plausibleMove(moves, m));
    }
    // newGame while searching discards the search
    ai::EngineSettings slow;
    slow.moveTimeMs = 5000;
    e.configure(slow);
    e.requestMove(kItalian, ai::ClockInfo{});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    e.newGame();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(!e.moveReady());
    e.shutdown();
}

TEST(ai_shutdown_restart) {
    ai::Engine a, b;
    CHECK(a.start());
    CHECK(!b.start());  // Stockfish's state is global: one session at a time
    CHECK(!b.available());
    b.requestMove({}, ai::ClockInfo{});  // requests on an engine that is not running fail at once
    CHECK(b.moveReady());
    CHECK_EQ(b.takeMove(), std::string());
    a.configure(fast());
    a.requestMove({}, ai::ClockInfo{});
    a.shutdown();  // with a search possibly still running
    CHECK(b.start());
    CHECK(b.waitReady(30000));
    b.configure(fast());
    b.requestMove({"g1f3"}, ai::ClockInfo{});
    CHECK(plausibleMove({"g1f3"}, waitMove(b, 20000)));
    b.shutdown();
    for (int i = 0; i < 3; ++i) {
        auto t0 = SteadyClock::now();
        CHECK(a.start());
        CHECK(a.waitReady(30000));
        std::fprintf(stderr, "  restart %d: ready in %d ms\n", i, msSince(t0));
        a.configure(fast());
        a.requestMove({"e2e4"}, ai::ClockInfo{});
        CHECK(plausibleMove({"e2e4"}, waitMove(a, 20000)));
        a.shutdown();
    }
}

namespace {

// Reads engine output until a line starting with `prefix`, keeping the lines; false on timeout.
bool readUntil(ai::detail::UciHost& host, const std::string& prefix, std::vector<std::string>& lines,
               int timeoutMs) {
    auto t0 = SteadyClock::now();
    std::string line;
    while (msSince(t0) < timeoutMs) {
        if (!host.waitLine(line, 100)) continue;
        lines.push_back(line);
        if (line.compare(0, prefix.size(), prefix) == 0) return true;
    }
    return false;
}

// Fixed-depth searches in a raw session: "<nodes>/<best move>" per position, as Stockfish reports
// them in its last "info depth" line and "bestmove".
std::string searchSignature(ai::detail::UciHost& host) {
    static const char* const positions[] = {
        "startpos",
        "startpos moves e2e4 e7e5 g1f3 b8c6 f1c4 f8c5 c2c3 g8f6 d2d4 e5d4",
        "fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "fen 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    };
    std::vector<std::string> lines;
    host.send("uci");
    host.send("setoption name Threads value 1");
    host.send("setoption name Hash value 16");
    host.send("isready");
    if (!readUntil(host, "readyok", lines, 30000)) return "no readyok";
    std::string signature;
    for (const char* position : positions) {
        host.send("ucinewgame");
        host.send(std::string("position ") + position);
        host.send("go depth 11");
        lines.clear();
        if (!readUntil(host, "bestmove", lines, 60000)) return signature + "timeout";
        std::string nodes = "?";
        for (const std::string& l : lines) {
            const std::vector<std::string> w = words(l);
            for (size_t i = 0; i + 1 < w.size(); ++i)
                if (w[0] == "info" && w[1] == "depth" && w[i] == "nodes") nodes = w[i + 1];
        }
        signature += nodes + "/" + words(lines.back()).at(1) + " ";
    }
    return signature;
}

}  // namespace

TEST(ai_variants_play_identically) {
    // Each variant of this build that the CPU runs, forced in turn through the arch limit, must
    // search exactly the same trees: same node counts, same moves.
    ai::detail::UciHost& host = ai::detail::UciHost::instance();
    const std::vector<std::string> variants = host.variants();
    CHECK(!variants.empty());
    int owner = 0;
    std::string reference, bestRun;
    for (const std::string& v : variants) {
        ai::Engine::setArchLimit(v);
        CHECK(host.acquire(&owner));
        if (v != host.arch()) {  // this CPU does not run it: the limit chose a lower variant
            std::fprintf(stderr, "  %s: not run by this CPU (%s chosen)\n", v.c_str(), host.arch());
            host.release(&owner);
            continue;
        }
        auto t0 = SteadyClock::now();
        const std::string signature = searchSignature(host);
        std::fprintf(stderr, "  %s: %s in %d ms\n", v.c_str(), signature.c_str(), msSince(t0));
        host.release(&owner);
        if (reference.empty()) reference = signature;
        CHECK_EQ(signature, reference);
        bestRun = v;
    }
    CHECK(!bestRun.empty());

    // An unknown name leaves the limit alone; "auto" chooses the best variant this CPU runs.
    ai::Engine::setArchLimit(variants.front());
    ai::Engine::setArchLimit("x86-64-no-such-variant");
    CHECK(host.acquire(&owner));
    CHECK_EQ(std::string(host.arch()), variants.front());
    host.release(&owner);
    ai::Engine::setArchLimit("auto");
    CHECK(host.acquire(&owner));
    CHECK_EQ(std::string(host.arch()), bestRun);
    host.release(&owner);
}

#else

TEST(ai_engine_unavailable) {
    ai::Engine e;
    CHECK(!e.start());
    CHECK(!e.available());
    e.requestMove({}, ai::ClockInfo{});
    CHECK(e.moveReady());
    CHECK_EQ(e.takeMove(), std::string());
}

#endif
