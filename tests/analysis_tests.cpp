// Tests for the full-strength analysis API (src/ai/analysis.h, Engine::requestAnalysis): the parser
// on lines Stockfish 19 really prints, score arithmetic, and with the embedded engine: MultiPV lines,
// mates, positions without legal moves, search moves, invalid input that must never reach Stockfish,
// queue order, stop / cancel / newGame, and move searches from a FEN.
#include "test.h"

#include "ai/analysis.h"
#include "ai/engine.h"
#include "chess/chess.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace {

using SteadyClock = std::chrono::steady_clock;

int msSince(SteadyClock::time_point t) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - t).count());
}

// Literal lines of Stockfish 19 (this build, UCI_ShowWDL on), captured with a raw UCI session.
// Italian game after 5...exd4, MultiPV 3, go depth 8: the last report.
const char* const kItalianD8[] = {
    "info depth 8 seldepth 10 multipv 1 score cp 58 wdl 154 844 2 nodes 31270 nps 651458 hashfull 9 tbhits 0 "
    "time 48 pv e1g1 f6e4",
    "info depth 8 seldepth 13 multipv 2 score cp 14 wdl 30 960 10 nodes 31270 nps 651458 hashfull 9 tbhits 0 "
    "time 48 pv c3d4 c5b4 b1c3 f6e4 e1g1 b4c3 d4d5 c3f6",
    "info depth 8 seldepth 16 multipv 3 score cp -10 wdl 11 963 26 nodes 31270 nps 651458 hashfull 9 tbhits 0 "
    "time 48 pv e4e5 d7d5 c4b5 f6e4 c3d4 c5b4 b1d2",
};
// 6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1, go depth 6: Rd8 mates.
const char* const kMateIn1 =
    "info depth 6 seldepth 2 multipv 1 score mate 1 wdl 1000 0 0 nodes 1108 nps 554000 hashfull 0 tbhits 0 time 2 "
    "pv d1d8";
// Searches stopped in the middle of an iteration (go movetime 60000, then stop).
const char* const kUpperBound =
    "info depth 8 seldepth 13 multipv 1 score cp 14 upperbound wdl 30 960 10 nodes 8708 nps 414666 hashfull 3 "
    "tbhits 0 time 21 pv c3d4 c5b4";
const char* const kLowerBound =
    "info depth 10 seldepth 18 multipv 1 score cp 13 lowerbound wdl 29 961 10 nodes 67352 nps 580620 hashfull 18 "
    "tbhits 0 time 116 pv c3d4 c5b4";
// 1.e4 e5 2.Nf3 d6 3.Bc4 Bg4 4.Nc3, go depth 10 searchmoves g7g6: the refutation 5.Nxe5! is in the PV.
const char* const kSearchMovesG6 =
    "info depth 10 seldepth 11 multipv 1 score cp -266 wdl 0 1 999 nodes 1991 nps 497750 hashfull 0 tbhits 0 "
    "time 4 pv g7g6 f3e5 d6e5 d1g4 g8f6 g4f3 b8c6 d2d3";

const std::vector<std::string> kItalian = {"e2e4", "e7e5", "g1f3", "b8c6", "f1c4",
                                           "f8c5", "c2c3", "g8f6", "d2d4", "e5d4"};
// Before Black's 4...g6?? in 1.e4 e5 2.Nf3 d6 3.Bc4 Bg4 4.Nc3 (Black to move).
const std::vector<std::string> kBeforeG6 = {"e2e4", "e7e5", "g1f3", "d7d6", "f1c4", "c8g4", "b1c3"};
const std::string kMateIn1Fen = "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1";

}  // namespace

// ---------------------------------------------------------------------------------------------
// Parser and scores (no engine)
// ---------------------------------------------------------------------------------------------

TEST(analysis_parse_info_lines) {
    ai::PvLine l;
    int64_t nodes = -1;
    int timeMs = -1;
    CHECK(ai::parseInfoLine(kItalianD8[1], l, &nodes, &timeMs));
    CHECK_EQ(l.multipv, 2);
    CHECK_EQ(l.depth, 8);
    CHECK_EQ(l.seldepth, 13);
    CHECK_EQ(l.score.cp, 14);
    CHECK_EQ(l.score.mate, 0);
    CHECK(!l.score.matedNow && !l.score.matesNow);
    CHECK(l.score.hasWdl);
    CHECK_EQ(l.score.win, 30);
    CHECK_EQ(l.score.draw, 960);
    CHECK_EQ(l.score.loss, 10);
    CHECK(l.score.bound == ai::Score::Bound::Exact);
    CHECK_EQ(nodes, int64_t(31270));
    CHECK_EQ(timeMs, 48);
    CHECK_EQ(l.pv.size(), size_t(8));
    CHECK_EQ(l.pv.front(), std::string("c3d4"));
    CHECK_EQ(l.pv.back(), std::string("c3f6"));
    for (const char* line : kItalianD8) CHECK(ai::parseInfoLine(line, l));

    CHECK(ai::parseInfoLine(kMateIn1, l));
    CHECK_EQ(l.score.mate, 1);
    CHECK(l.score.isMate());
    CHECK_EQ(l.score.expected(), 1.0);
    CHECK_EQ(l.score.text(), std::string("M1"));
    CHECK_EQ(l.pv.size(), size_t(1));
    CHECK_EQ(l.pv.front(), std::string("d1d8"));

    CHECK(ai::parseInfoLine(kUpperBound, l));
    CHECK(l.score.bound == ai::Score::Bound::Upper);
    CHECK_EQ(l.score.cp, 14);
    CHECK(l.score.hasWdl && l.score.win == 30);  // the bound comes before "wdl"
    CHECK(ai::parseInfoLine(kLowerBound, l));
    CHECK(l.score.bound == ai::Score::Bound::Lower);
    CHECK(l.score.flipped().bound == ai::Score::Bound::Upper);

    CHECK(ai::parseInfoLine(kSearchMovesG6, l));
    CHECK_EQ(l.score.cp, -266);
    CHECK_EQ(l.pv.at(0), std::string("g7g6"));
    CHECK_EQ(l.pv.at(1), std::string("f3e5"));

    // A position without legal moves: the only line printed, no multipv, no pv.
    CHECK(ai::parseInfoLine("info depth 0 score mate 0", l));
    CHECK_EQ(l.depth, 0);
    CHECK_EQ(l.multipv, 1);
    CHECK(l.score.matedNow);
    CHECK(l.pv.empty());
    CHECK_EQ(l.score.expected(), 0.0);
    CHECK(ai::parseInfoLine("info depth 0 score cp 0", l));  // stalemate
    CHECK(!l.score.matedNow && !l.score.isMate());
    CHECK_EQ(l.score.cp, 0);

    // Without UCI_ShowWDL: the expected score comes from the centipawns.
    CHECK(ai::parseInfoLine("info depth 1 seldepth 2 multipv 1 score cp 20 nodes 20 nps 20000 hashfull 0 tbhits 0 "
                            "time 1 pv e2e4",
                            l));
    CHECK(!l.score.hasWdl);
    CHECK(l.score.expected() > 0.5 && l.score.expected() < 0.53);
    CHECK(ai::parseInfoLine("info depth 20 seldepth 9 multipv 1 score mate -2 wdl 0 0 1000 nodes 5 nps 5 hashfull 0 "
                            "tbhits 0 time 1 pv h7h6 d1d8",
                            l));
    CHECK_EQ(l.score.mate, -2);
    CHECK_EQ(l.score.text(), std::string("-M2"));

    // Lines without a score are rejected, whatever they contain, and leave the outputs alone.
    const char* const rejected[] = {
        "info string Available processors: 0-3",
        "info string Using 1 thread",
        "info string NNUE evaluation using nn-1a298aa575a0.nnue (109MiB, (86896, 1024, 32, 32, 1))",
        "info string Network replica 1: Local memory.",
        "info depth 21 currmove e1g1 currmovenumber 3",  // after 10 M nodes
        "info depth 5 seldepth 7 nodes 100 time 3",
        "bestmove e1g1 ponder f6e4",
        "readyok",
        "",
        "info",
        "info depth 8 score cp",                          // truncated
        "info depth x score cp 5 pv e2e4",                // not a number
        "info depth 8 score cp 99999999999999999999 pv e2e4",  // overflow
        "info depth 8 score centipawns 5 pv e2e4",
        "info depth 8 multipv 0 score cp 5 pv e2e4",
    };
    for (const char* line : rejected) {
        nodes = -7;
        timeMs = -7;
        ai::PvLine untouched;
        untouched.depth = 99;
        CHECK(!ai::parseInfoLine(line, untouched, &nodes, &timeMs));
        CHECK_EQ(untouched.depth, 99);
        CHECK_EQ(nodes, int64_t(-7));
        CHECK_EQ(timeMs, -7);
    }
}

TEST(analysis_scores) {
    ai::Score s;
    s.cp = 125;
    CHECK_EQ(s.text(), std::string("+1.25"));
    s.cp = -40;
    CHECK_EQ(s.text(), std::string("-0.40"));
    s.cp = 0;
    CHECK_EQ(s.text(), std::string("0.00"));
    CHECK_EQ(s.expected(), 0.5);

    // The other side's view.
    ai::Score w;
    w.cp = 58;
    w.hasWdl = true;
    w.win = 154;
    w.draw = 844;
    w.loss = 2;
    const ai::Score b = w.flipped();
    CHECK_EQ(b.cp, -58);
    CHECK_EQ(b.win, 2);
    CHECK_EQ(b.loss, 154);
    CHECK(std::abs(w.expected() + b.expected() - 1.0) < 1e-9);
    CHECK(std::abs(w.expected() - 0.576) < 1e-9);
    CHECK_EQ(w.forWhite(true).cp, 58);
    CHECK_EQ(w.forWhite(false).cp, -58);
    CHECK_EQ(w.flipped().flipped().cp, 58);

    ai::Score mated;
    mated.matedNow = true;
    const ai::Score mates = mated.flipped();
    CHECK(mates.matesNow && !mates.matedNow);
    CHECK_EQ(mates.expected(), 1.0);
    CHECK_EQ(mated.text(), std::string("-M0"));
    CHECK_EQ(mates.text(), std::string("M0"));

    // Total order: mate delivered > mate in 1 > mate in 3 > any cp > mated in 5 > mated in 1 > mated.
    auto cp = [](int v) { ai::Score x; x.cp = v; return x; };
    auto mate = [](int n) { ai::Score x; x.mate = n; return x; };
    const ai::Score order[] = {mates, mate(1), mate(3), cp(20000), cp(150), cp(0), cp(-150), cp(-20000),
                               mate(-5), mate(-1), mated};
    for (size_t i = 0; i + 1 < sizeof(order) / sizeof(order[0]); ++i) {
        CHECK(order[i].sortKey() > order[i + 1].sortKey());
        CHECK(order[i].expected() >= order[i + 1].expected());
        CHECK_EQ(order[i].flipped().sortKey(), -order[i].sortKey());
    }
    CHECK_EQ(mate(3).text(), std::string("M3"));

    ai::Analysis a;
    ai::PvLine l1, l2;
    l1.pv = {"e1g1", "f6e4"};
    l2.multipv = 2;
    l2.pv = {"c3d4"};
    a.lines = {l1, l2};
    CHECK(a.line("c3d4") == &a.lines[1]);
    CHECK(a.line("e1g1") == &a.lines[0]);
    CHECK(a.line("f6e4") == nullptr);
}

// ---------------------------------------------------------------------------------------------
// Embedded Stockfish
// ---------------------------------------------------------------------------------------------
#if defined(SCACELITH_HAS_STOCKFISH)

namespace {

bool waitAnalysis(ai::Engine& e, uint32_t id, ai::Analysis& out, int timeoutMs) {
    auto t0 = SteadyClock::now();
    while (!e.analysisReady(id)) {
        if (msSince(t0) > timeoutMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return e.takeAnalysis(id, out);
}

bool waitIdle(ai::Engine& e, int timeoutMs) {
    auto t0 = SteadyClock::now();
    while (!e.idle()) {
        if (msSince(t0) > timeoutMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

std::string waitMove(ai::Engine& e, int timeoutMs) {
    auto t0 = SteadyClock::now();
    while (!e.moveReady()) {
        if (msSince(t0) > timeoutMs) return "timeout";
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return e.takeMove();
}

ai::AnalysisRequest depthOnly(const std::vector<std::string>& moves, int depth, int multiPV = 3) {
    ai::AnalysisRequest r;
    r.moves = moves;
    r.depth = depth;
    r.moveTimeMs = 0;  // reproducible
    r.multiPV = multiPV;
    return r;
}

ai::EngineSettings analysisHost() {  // the settings whose Threads / Hash the analyses use
    ai::EngineSettings s;
    s.hashMB = 16;
    s.depth = 6;
    return s;
}

// True when `m` is a legal move after startFen ("" = start) + moves.
bool legalAfter(const std::string& startFen, const std::vector<std::string>& moves, const std::string& m) {
    chess::Position p;
    if (!startFen.empty() && !p.setFEN(startFen)) return false;
    for (const auto& s : moves) {
        chess::Move mv = p.parseUCI(s);
        if (!mv.valid()) return false;
        p.makeMove(mv);
    }
    return p.parseUCI(m).valid();
}

}  // namespace

TEST(analysis_multipv) {
    ai::Engine e;
    CHECK(e.start());
    CHECK(e.waitReady(30000));
    e.configure(analysisHost());
    e.newGame();
    auto t0 = SteadyClock::now();
    const uint32_t id = e.requestAnalysis(depthOnly(kItalian, 12));
    CHECK(id > 0);
    CHECK(e.analysisPending(id));
    ai::Analysis a;
    CHECK(waitAnalysis(e, id, a, 30000));
    std::fprintf(stderr, "  Italian MultiPV 3 depth 12: %d ms (engine %d ms, %lld nodes)\n", msSince(t0), a.timeMs,
                 (long long)a.nodes);
    ai::Analysis none;
    CHECK(!e.analysisReady(id));  // taken: the slot is free
    CHECK(!e.analysisPending(id));
    CHECK(!e.takeAnalysis(id, none));
    CHECK_EQ(a.id, id);
    CHECK(a.ok);
    CHECK(a.whiteToMove);
    CHECK(!a.noLegalMove);
    chess::Position italian;
    for (const auto& m : kItalian) italian.makeMove(italian.parseUCI(m));
    CHECK_EQ(a.fen, italian.fen());
    CHECK_EQ(a.lines.size(), size_t(3));
    CHECK_EQ(a.depth, 12);
    CHECK(a.nodes > 0);
    for (size_t i = 0; i < a.lines.size(); ++i) {
        const ai::PvLine& l = a.lines[i];
        CHECK_EQ(l.multipv, int(i + 1));
        CHECK_EQ(l.depth, 12);
        CHECK(!l.pv.empty());
        CHECK(l.score.hasWdl);
        CHECK_EQ(l.score.win + l.score.draw + l.score.loss, 1000);
        CHECK(legalAfter("", kItalian, l.pv.front()));
        if (i > 0) CHECK(a.lines[i - 1].score.sortKey() >= l.score.sortKey());
        CHECK(a.line(l.pv.front()) == &l);
        std::fprintf(stderr, "    %zu: %-6s %s (%d/%d/%d)\n", i + 1, l.pv.front().c_str(), l.score.text().c_str(),
                     l.score.win, l.score.draw, l.score.loss);
    }
    CHECK_EQ(a.bestMove, a.lines[0].pv.front());

    // Depth-limited, one thread, same hash state: the same answer again.
    e.newGame();
    ai::Analysis again;
    CHECK(waitAnalysis(e, e.requestAnalysis(depthOnly(kItalian, 12)), again, 30000));
    CHECK_EQ(again.bestMove, a.bestMove);
    CHECK_EQ(again.nodes, a.nodes);
    CHECK_EQ(again.lines.size(), a.lines.size());
    for (size_t i = 0; i < a.lines.size() && i < again.lines.size(); ++i) {
        CHECK_EQ(again.lines[i].score.cp, a.lines[i].score.cp);
        CHECK(again.lines[i].pv == a.lines[i].pv);
    }

    // More lines asked for than there are legal moves: one line per legal move.
    ai::AnalysisRequest few;
    few.startFen = "8/8/8/8/8/8/k7/2K4R b - - 0 1";  // a lone black king with 3 moves
    few.depth = 8;
    few.moveTimeMs = 0;
    few.multiPV = 64;
    chess::Position fp;
    CHECK(fp.setFEN(few.startFen));
    const size_t legal = fp.legalMoves().size();
    ai::Analysis fa;
    CHECK(waitAnalysis(e, e.requestAnalysis(few), fa, 30000));
    CHECK(fa.ok);
    CHECK(!fa.whiteToMove);
    CHECK_EQ(fa.lines.size(), legal);
    CHECK(legal == 3);
    // Every legal move of a middlegame position ranked (the teaching-blunder picker's request).
    ai::Analysis all;
    t0 = SteadyClock::now();
    CHECK(waitAnalysis(e, e.requestAnalysis(depthOnly(kItalian, 8, 256)), all, 30000));
    CHECK_EQ(all.lines.size(), italian.legalMoves().size());
    std::fprintf(stderr, "  all %zu moves ranked at depth 8: %d ms\n", all.lines.size(), msSince(t0));
    e.shutdown();
}

TEST(analysis_mates_and_terminal_positions) {
    ai::Engine e;
    CHECK(e.start());
    e.configure(analysisHost());
    ai::AnalysisRequest r;
    r.startFen = kMateIn1Fen;
    r.depth = 10;
    r.moveTimeMs = 0;
    ai::Analysis a;
    CHECK(waitAnalysis(e, e.requestAnalysis(r), a, 30000));
    CHECK(a.ok);
    CHECK(!a.lines.empty());
    if (!a.lines.empty()) {
        CHECK_EQ(a.lines[0].score.mate, 1);
        CHECK_EQ(a.lines[0].pv.at(0), std::string("d1d8"));
        CHECK_EQ(a.lines[0].score.expected(), 1.0);
    }
    CHECK_EQ(a.bestMove, std::string("d1d8"));
    // "go mate 1": stops as soon as the mate is proven.
    r.mateIn = 1;
    r.depth = 30;
    CHECK(waitAnalysis(e, e.requestAnalysis(r), a, 30000));
    CHECK(a.ok && !a.lines.empty() && a.lines[0].score.mate == 1);
    CHECK(a.depth < 30);
    // The same mate for Black, from Black's point of view; mated after the move.
    ai::AnalysisRequest bl;
    bl.startFen = "3r2k1/5ppp/8/8/8/8/5PPP/6K1 b - - 0 1";
    bl.depth = 8;
    bl.moveTimeMs = 0;
    CHECK(waitAnalysis(e, e.requestAnalysis(bl), a, 30000));
    CHECK(a.ok && !a.whiteToMove && !a.lines.empty() && a.lines[0].score.mate == 1);
    CHECK_EQ(a.bestMove, std::string("d8d1"));
    if (!a.lines.empty()) CHECK_EQ(a.lines[0].score.forWhite(a.whiteToMove).mate, -1);

    // Checkmated: fool's mate, White to move.
    ai::Analysis m;
    CHECK(waitAnalysis(e, e.requestAnalysis(depthOnly({"f2f3", "e7e5", "g2g4", "d8h4"}, 10)), m, 5000));
    CHECK(m.ok);
    CHECK(m.noLegalMove);
    CHECK(m.whiteToMove);
    CHECK(m.bestMove.empty());
    CHECK_EQ(m.lines.size(), size_t(1));
    if (!m.lines.empty()) {
        CHECK(m.lines[0].score.matedNow);
        CHECK(m.lines[0].pv.empty());
        CHECK_EQ(m.lines[0].score.expected(), 0.0);
        CHECK(m.lines[0].score.forWhite(m.whiteToMove).matedNow);
    }
    // Black checkmated (scholar's mate): from White's side, the mate is delivered.
    CHECK(waitAnalysis(e, e.requestAnalysis(depthOnly({"e2e4", "e7e5", "f1c4", "b8c6", "d1h5", "g8f6", "h5f7"}, 10)),
                       m, 5000));
    CHECK(m.ok && m.noLegalMove && !m.whiteToMove && !m.lines.empty());
    if (!m.lines.empty()) CHECK(m.lines[0].score.forWhite(m.whiteToMove).matesNow);
    // Stalemate: Black to move, no legal move, not in check.
    ai::AnalysisRequest st;
    st.startFen = "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1";
    st.depth = 10;
    CHECK(waitAnalysis(e, e.requestAnalysis(st), m, 5000));
    CHECK(m.ok && m.noLegalMove && !m.whiteToMove);
    CHECK_EQ(m.lines.size(), size_t(1));
    if (!m.lines.empty()) {
        CHECK(!m.lines[0].score.matedNow && !m.lines[0].score.isMate());
        CHECK_EQ(m.lines[0].score.cp, 0);
        CHECK_EQ(m.lines[0].score.draw, 1000);
        CHECK_EQ(m.lines[0].score.expected(), 0.5);
    }
    e.shutdown();
}

TEST(analysis_search_moves) {
    ai::Engine e;
    CHECK(e.start());
    e.configure(analysisHost());
    e.newGame();
    // The pipeline of a move just played: best lines, then the played move re-scored on the same root.
    ai::Analysis before, played;
    auto t0 = SteadyClock::now();
    CHECK(waitAnalysis(e, e.requestAnalysis(depthOnly(kBeforeG6, 14)), before, 30000));
    const int beforeMs = msSince(t0);
    ai::AnalysisRequest r = depthOnly(kBeforeG6, 14);
    r.searchMoves = {"g7g6", "g7g6"};  // duplicates are harmless
    t0 = SteadyClock::now();
    CHECK(waitAnalysis(e, e.requestAnalysis(r), played, 30000));
    std::fprintf(stderr, "  before 4...g6: %d ms, re-score of g6 on the warm hash: %d ms\n", beforeMs, msSince(t0));
    CHECK(before.ok && played.ok);
    CHECK(!before.whiteToMove && !played.whiteToMove);
    CHECK_EQ(played.lines.size(), size_t(1));
    CHECK_EQ(played.bestMove, std::string("g7g6"));
    CHECK(!before.lines.empty() && !played.lines.empty());
    if (!before.lines.empty() && !played.lines.empty()) {
        const ai::PvLine& best = before.lines[0];
        const ai::PvLine& g6 = played.lines[0];
        CHECK_EQ(g6.pv.at(0), std::string("g7g6"));
        CHECK(g6.pv.size() >= 2 && g6.pv[1] == "f3e5");  // the refutation: 5.Nxe5!
        const double loss = best.score.expected() - g6.score.expected();
        std::fprintf(stderr, "  best %s %s, played g6 %s: expected-score loss %.2f\n", best.pv.at(0).c_str(),
                     best.score.text().c_str(), g6.score.text().c_str(), loss);
        CHECK(loss > 0.25);  // a blunder by any level's threshold
        CHECK(before.line("g7g6") == nullptr);
    }

    // An illegal search move fails at once and nothing reaches Stockfish (which would search every
    // move instead and report its best as the answer).
    const std::vector<std::vector<std::string>> bad = {{"a1a8"}, {"e2e4"}, {"g7g6", "b1c3"}, {"g7g6 f3e5"}, {""},
                                                       {"g7g5q"}};
    for (const auto& sm : bad) {
        ai::AnalysisRequest q = depthOnly(kBeforeG6, 10);
        q.searchMoves = sm;
        const uint32_t id = e.requestAnalysis(q);
        CHECK(e.analysisReady(id));  // at once
        CHECK(!e.analysisPending(id));
        ai::Analysis f;
        CHECK(e.takeAnalysis(id, f));
        CHECK(!f.ok);
        CHECK_EQ(f.id, id);
        CHECK(f.lines.empty());
    }
    CHECK(e.idle());
    // Still alive and exact. Accepted spellings are re-emitted as Stockfish expects them.
    r.searchMoves = {"G7G6"};
    CHECK(waitAnalysis(e, e.requestAnalysis(r), played, 30000));
    CHECK(played.ok && played.bestMove == "g7g6");
    e.shutdown();
}

TEST(analysis_invalid_input_survives) {
    // Stockfish 19 ends the process on a FEN or number it cannot use: none of these may reach it.
    ai::Engine e;
    CHECK(e.start());
    e.configure(analysisHost());
    const char* const badFens[] = {
        "8/8/8/8/8/8/8/8 w - - 0 1",                                      // no kings
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1",       // side to move
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - -1 1",      // halfmove clock
        "garbage",
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBN w KQkq - 0 1",        // 7 squares on a rank
        "4k3/8/8/8/8/8/8/4K2R b - - 0 1 extra",                           // 7 fields
        "4k3/4R3/8/8/8/8/8/4K3 w - - 0 1",                                // side not to move in check
        "P3k3/8/8/8/8/8/8/4K3 w - - 0 1",                                 // pawn on the last rank
        "k7/8/8/8/8/8/QQQQQQQQ/QQK5 b - - 0 1",                           // 10 queens: Stockfish refuses
    };
    for (const char* fen : badFens) {
        ai::AnalysisRequest r;
        r.startFen = fen;
        r.depth = 6;
        const uint32_t id = e.requestAnalysis(r);
        ai::Analysis a;
        CHECK(e.takeAnalysis(id, a));  // failed at once
        CHECK(!a.ok);
        e.requestMoveFrom(fen, {}, ai::ClockInfo{});
        CHECK_EQ(waitMove(e, 5000), std::string());
    }
    // Accepted after normalisation: counters beyond Stockfish's range, an en passant square Stockfish
    // would reject (wrong rank), castling rights without their rooks, missing counters.
    const char* const fixable[] = {
        "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 99999 1",
        "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1000000",
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e3 0 1",
        "4k3/8/8/8/8/8/8/4K3 w KQkq - 0 1",
        "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - -",
        "7k/6pp/8/8/8/8/KQ6/QQQQQQQQ w - - 0 1",                          // 9 queens and no pawn: fine
    };
    for (const char* fen : fixable) {
        ai::AnalysisRequest r;
        r.startFen = fen;
        r.depth = 6;
        r.moveTimeMs = 0;
        ai::Analysis a;
        CHECK(waitAnalysis(e, e.requestAnalysis(r), a, 20000));
        CHECK(a.ok);
        CHECK(!a.lines.empty() || a.noLegalMove);
        e.requestMoveFrom(fen, {}, ai::ClockInfo{});
        const std::string m = waitMove(e, 20000);
        CHECK(legalAfter(fen, {}, m));
    }
    // Illegal moves after a valid FEN, a request without any limit ("go" alone never ends; "go mate"
    // alone neither when there is no mate).
    ai::AnalysisRequest r;
    r.startFen = kMateIn1Fen;
    r.moves = {"d1d8", "g8h7"};  // Black is mated: no reply exists
    r.depth = 6;
    ai::Analysis a;
    CHECK(e.takeAnalysis(e.requestAnalysis(r), a));
    CHECK(!a.ok);
    r.moves.clear();
    r.depth = 0;
    r.moveTimeMs = 0;
    r.nodes = 0;
    CHECK(e.takeAnalysis(e.requestAnalysis(r), a));
    CHECK(!a.ok);
    r.mateIn = 3;
    CHECK(e.takeAnalysis(e.requestAnalysis(r), a));
    CHECK(!a.ok);
    // Out-of-range numbers are clamped, never sent as they are.
    r.mateIn = 0;
    r.depth = 1000000;
    r.nodes = 20000;
    r.multiPV = 100000;
    CHECK(waitAnalysis(e, e.requestAnalysis(r), a, 20000));
    CHECK(a.ok && a.bestMove == "d1d8");
    // A node-limited request.
    r.depth = 0;
    r.nodes = 5000;
    r.multiPV = -3;
    CHECK(waitAnalysis(e, e.requestAnalysis(r), a, 20000));
    CHECK(a.ok && a.lines.size() == 1 && a.nodes > 0);

    CHECK(e.available());
    CHECK(e.waitReady(1000));
    e.shutdown();

    // An engine that is not running fails at once too.
    ai::Engine off;
    const uint32_t id = off.requestAnalysis(depthOnly(kItalian, 6));
    CHECK(id > 0);
    CHECK(off.analysisReady(id));
    CHECK(off.takeAnalysis(id, a));
    CHECK(!a.ok);
    CHECK(off.idle());
}

TEST(analysis_queue_order) {
    ai::Engine e;
    CHECK(e.start());
    CHECK(e.waitReady(30000));
    e.configure(analysisHost());
    e.newGame();
    // FIFO among equal priorities; a higher priority overtakes the queued lower ones, never the
    // running search. When a result is ready, every result expected before it is ready already.
    const uint32_t a = e.requestAnalysis(depthOnly(kItalian, 10));          // starts at once
    ai::AnalysisRequest low = depthOnly(kBeforeG6, 10);
    low.priority = -1;
    const uint32_t b = e.requestAnalysis(low);
    low.moves = {"d2d4", "d7d5", "c2c4"};
    const uint32_t c = e.requestAnalysis(low);
    ai::AnalysisRequest high = depthOnly({"e2e4", "c7c5"}, 10);
    high.priority = 1;
    const uint32_t d = e.requestAnalysis(high);
    const uint32_t f = e.requestAnalysis(depthOnly({"e2e4", "e7e6"}, 10));  // priority 0: after d, before b
    CHECK(a < b && b < c && c < d && d < f);
    const std::vector<uint32_t> expected = {a, d, f, b, c};
    std::vector<uint32_t> seen;
    bool orderOk = true;
    auto t0 = SteadyClock::now();
    while (seen.size() < expected.size() && msSince(t0) < 30000) {
        for (size_t i = 0; i < expected.size(); ++i) {
            if (std::find(seen.begin(), seen.end(), expected[i]) != seen.end()) continue;
            if (!e.analysisReady(expected[i])) continue;
            for (size_t j = 0; j < i; ++j) orderOk = orderOk && e.analysisReady(expected[j]);
            seen.push_back(expected[i]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(orderOk);
    CHECK_EQ(seen.size(), expected.size());
    for (uint32_t id : expected) {
        ai::Analysis r;
        CHECK(e.takeAnalysis(id, r));
        CHECK(r.ok && r.depth == 10);
    }
    CHECK(e.idle());

    // A move request (priority 0) overtakes queued background analyses.
    ai::EngineSettings host = analysisHost();
    e.configure(host);
    const uint32_t running = e.requestAnalysis(depthOnly(kItalian, 11));
    low = depthOnly(kBeforeG6, 12);
    low.priority = -1;
    const uint32_t background = e.requestAnalysis(low);
    e.requestMove(kItalian, ai::ClockInfo{});
    std::string m = waitMove(e, 30000);
    CHECK(legalAfter("", kItalian, m));
    CHECK(e.analysisReady(running));
    CHECK(!e.analysisReady(background));
    ai::Analysis r;
    CHECK(waitAnalysis(e, background, r, 30000));
    CHECK(r.ok);
    CHECK(e.takeAnalysis(running, r) && r.ok);
    e.shutdown();
}

TEST(analysis_stop_and_cancel) {
    ai::Engine e;
    CHECK(e.start());
    CHECK(e.waitReady(30000));
    e.configure(analysisHost());
    e.newGame();
    // stopAnalysis stops this analysis only: the one queued behind it completes its full depth.
    ai::AnalysisRequest longOne;
    longOne.moves = kItalian;
    longOne.depth = 0;
    longOne.moveTimeMs = 60000;
    const uint32_t a = e.requestAnalysis(longOne);
    const uint32_t b = e.requestAnalysis(depthOnly(kBeforeG6, 12));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!e.analysisReady(a));
    auto t0 = SteadyClock::now();
    e.stopAnalysis(a);
    ai::Analysis ra, rb;
    CHECK(waitAnalysis(e, a, ra, 3000));
    std::fprintf(stderr, "  analysis stopped after 300 ms: depth %d, %zu lines, stop latency %d ms\n", ra.depth,
                 ra.lines.size(), msSince(t0));
    CHECK(ra.ok);
    CHECK(ra.depth >= 1);
    CHECK(!ra.lines.empty() && !ra.bestMove.empty());
    CHECK(waitAnalysis(e, b, rb, 30000));
    CHECK(rb.ok && rb.depth == 12);
    // Stopped while still queued: it stops once its first iteration is done.
    const uint32_t c = e.requestAnalysis(longOne);
    ai::AnalysisRequest deep = depthOnly(kBeforeG6, 40);
    const uint32_t d = e.requestAnalysis(deep);
    e.stopAnalysis(d);
    CHECK(e.analysisPending(d));
    e.stopAnalysis(c);
    ai::Analysis rd;
    CHECK(waitAnalysis(e, d, rd, 5000));
    CHECK(rd.ok && rd.depth >= 1 && rd.depth < 40);
    CHECK(e.takeAnalysis(c, rd) && rd.ok);

    // cancelAnalysis: running (stopped, result discarded) and queued (dropped); the next one runs.
    const uint32_t x = e.requestAnalysis(longOne);
    const uint32_t y = e.requestAnalysis(depthOnly(kItalian, 9));
    const uint32_t z = e.requestAnalysis(depthOnly(kBeforeG6, 9));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    e.cancelAnalysis(x);
    e.cancelAnalysis(z);
    CHECK(!e.analysisPending(x) && !e.analysisPending(z));
    ai::Analysis ry;
    CHECK(waitAnalysis(e, y, ry, 5000));
    CHECK(ry.ok && ry.depth == 9);
    CHECK(waitIdle(e, 5000));
    CHECK(!e.analysisReady(x) && !e.analysisReady(z));
    ai::Analysis none;
    CHECK(!e.takeAnalysis(x, none));
    // A ready result can be cancelled (discarded) too.
    const uint32_t w = e.requestAnalysis(depthOnly(kItalian, 6));
    CHECK(waitIdle(e, 5000));
    CHECK(e.analysisReady(w));
    e.cancelAnalysis(w);
    CHECK(!e.analysisReady(w));
    // cancelAnalysis(0): everything.
    std::vector<uint32_t> ids = {e.requestAnalysis(longOne), e.requestAnalysis(depthOnly(kItalian, 8)),
                                 e.requestAnalysis(depthOnly(kBeforeG6, 8))};
    e.cancelAnalysis(0);
    CHECK(waitIdle(e, 3000));
    for (uint32_t id : ids) CHECK(!e.analysisReady(id) && !e.analysisPending(id));
    // A move request behind a cancelled analysis is not affected.
    e.requestAnalysis(longOne);
    e.requestMove(kItalian, ai::ClockInfo{});
    e.cancelAnalysis(0);
    CHECK(legalAfter("", kItalian, waitMove(e, 20000)));
    // Nor is a move search by stopAnalysis of another job.
    const uint32_t s = e.requestAnalysis(longOne);
    e.requestMove(kItalian, ai::ClockInfo{});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    e.stopAnalysis(s);
    CHECK(legalAfter("", kItalian, waitMove(e, 20000)));
    CHECK(e.takeAnalysis(s, rd) && rd.ok);
    e.shutdown();
}

TEST(analysis_new_game_drops_analyses) {
    ai::Engine e;
    CHECK(e.start());
    CHECK(e.waitReady(30000));
    e.configure(analysisHost());
    e.newGame();
    const uint32_t ready = e.requestAnalysis(depthOnly(kItalian, 6));
    CHECK(waitIdle(e, 10000));
    CHECK(e.analysisReady(ready));
    ai::AnalysisRequest longOne;
    longOne.moves = kItalian;
    longOne.depth = 0;
    longOne.moveTimeMs = 60000;
    const uint32_t running = e.requestAnalysis(longOne);
    const uint32_t queued = e.requestAnalysis(depthOnly(kBeforeG6, 12));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto t0 = SteadyClock::now();
    e.newGame();
    CHECK(waitIdle(e, 3000));
    std::fprintf(stderr, "  newGame during an analysis: idle after %d ms\n", msSince(t0));
    for (uint32_t id : {ready, running, queued}) CHECK(!e.analysisReady(id) && !e.analysisPending(id));
    // Ids are never reused, and the engine goes on.
    ai::Analysis a;
    const uint32_t next = e.requestAnalysis(depthOnly(kItalian, 8));
    CHECK(next > queued);
    CHECK(waitAnalysis(e, next, a, 20000));
    CHECK(a.ok && a.depth == 8);

    // Shutting down with analyses pending: they fail rather than hang their caller.
    const uint32_t lost = e.requestAnalysis(longOne);
    const uint32_t lost2 = e.requestAnalysis(depthOnly(kItalian, 20));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    e.shutdown();
    CHECK(e.takeAnalysis(lost, a) && !a.ok);
    CHECK(e.takeAnalysis(lost2, a) && !a.ok);
}

TEST(analysis_move_from_fen) {
    // Tutorial exercises start from a FEN: the coach's move search must use it (the standard start +
    // these moves would be an illegal line).
    ai::Engine e;
    CHECK(e.start());
    ai::EngineSettings full;
    full.depth = 8;
    full.hashMB = 16;
    e.configure(full);
    e.newGame();
    e.requestMoveFrom(kMateIn1Fen, {}, ai::ClockInfo{});
    CHECK_EQ(waitMove(e, 20000), std::string("d1d8"));
    e.requestMoveFrom("3r2k1/5ppp/8/8/8/8/5PPP/6K1 b - - 0 1", {}, ai::ClockInfo{});
    CHECK_EQ(waitMove(e, 20000), std::string("d8d1"));
    // Moves played from the FEN: 1.Rd6 Rxd6, and the queen takes back. The thinking time replays
    // them from the FEN too: an obvious recapture is played quickly (at most 600 + 250 x 3.08 ms for
    // the second move of a game; ~1.6 s median otherwise, so 50 draws below that bound all but
    // prove the detection).
    const std::string fen = "4k3/3r4/8/8/8/8/3R4/3QK3 w - - 0 1";
    const std::vector<std::string> moves = {"d2d6", "d7d6"};
    e.requestMoveFrom(fen, moves, ai::ClockInfo{});
    std::string m = waitMove(e, 20000);
    CHECK_EQ(m, std::string("d1d6"));
    ai::ClockInfo untimed;
    int slowest = 0;
    for (int i = 0; i < 50; ++i) slowest = std::max(slowest, e.thinkTimeMs(untimed, int(moves.size()), 20, false));
    CHECK(slowest <= 1371);
    // "" = the standard start, as requestMove().
    e.requestMoveFrom("", kItalian, ai::ClockInfo{});
    CHECK(legalAfter("", kItalian, waitMove(e, 20000)));
    // An illegal move after the FEN fails; so does a FEN game's move list sent as a standard game.
    e.requestMoveFrom(kMateIn1Fen, {"e2e4"}, ai::ClockInfo{});
    CHECK_EQ(waitMove(e, 5000), std::string());
    e.requestMove({"d1d8"}, ai::ClockInfo{});
    CHECK_EQ(waitMove(e, 5000), std::string());
    // Every coach level plays from a FEN (the weak ones included).
    for (int level = 0; level < ai::kCoachLevels; ++level) {
        ai::EngineSettings s = ai::coachLevelSettings(level);
        s.humanize = false;
        e.configure(s);
        ai::ClockInfo blitz;  // the UCI_Elo levels use the clock: keeps the test short
        blitz.timed = true;
        blitz.whiteMs = blitz.blackMs = 8000;
        blitz.moveOverheadMs = 100;
        e.requestMoveFrom(fen, moves, blitz);
        m = waitMove(e, 20000);
        CHECK(legalAfter(fen, moves, m));
    }
    e.shutdown();
}

#endif
