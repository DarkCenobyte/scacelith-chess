// coach_audit record: the analyses the coach's review asks for (coach/review.h: A0, A1, A2, A3), run
// with the embedded Stockfish on every move of the "human" side of some games, saved to a text file
// that `coach_audit review` replays through the Reviewer at every level without the engine.
//
// Games: PGN files (the human is the side not named "Coach", else both sides), or games played here
// between a weak "human" (Stockfish at a random weak setting, sometimes a random move from a shallow
// MultiPV) and the coach at one of its levels (ai::coachLevelSettings).
#include "audit.h"

#include "ai/engine.h"
#include "chess/pgn.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

namespace audit {

using namespace chess;

namespace {

bool waitFor(ai::Engine& e, uint32_t id, ai::Analysis& out, int timeoutMs) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!e.analysisReady(id)) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeoutMs)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return e.takeAnalysis(id, out);
}

ai::Analysis analyse(ai::Engine& e, ai::AnalysisRequest r) {
    r.moveTimeMs = 0;   // depth only: reproducible
    ai::Analysis a;
    if (!waitFor(e, e.requestAnalysis(r), a, 600000)) a.ok = false;
    return a;
}

std::string waitMove(ai::Engine& e, const Game& g) {
    ai::ClockInfo clock;
    const Position& s = g.startPosition();
    e.requestMoveFrom(s.isStandardStart() ? std::string() : s.fen(), g.uciMoves(), clock);
    const auto t0 = std::chrono::steady_clock::now();
    while (!e.moveReady()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120)) return std::string();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return e.takeMove();
}

// Every analysis of one human move, before and after it is played (g holds the moves before it).
Record recordMove(ai::Engine& e, const Game& before, const Move& m, int depth) {
    Record rec;
    rec.startFen = before.startPosition().isStandardStart() ? std::string() : before.startPosition().fen();
    rec.moves = before.uciMoves();
    rec.played = before.position().toUCI(m);
    rec.human = before.position().sideToMove();
    coach::Reviewer rv;
    rv.reset(1, rec.human);
    ai::AnalysisRequest q0 = rv.beforeRequest(before);
    q0.depth = depth;
    rec.a0 = analyse(e, q0);
    rec.a3 = analyse(e, rv.shallowRequest(before));
    Game after = before;
    after.play(m);
    if (rec.a0.ok && coach::Reviewer::needsPlayedRequest(rec.a0, rec.played)) {
        rec.a1 = analyse(e, rv.playedRequest(after, rec.a0));
        rec.hasA1 = true;
    }
    if (after.position().hasLegalMove()) {
        rec.a2 = analyse(e, rv.afterRequest(after));
        rec.hasA2 = true;
    }
    return rec;
}

ai::EngineSettings humanSettings(std::mt19937& rng) {
    ai::EngineSettings s;
    s.humanize = false;
    s.useClock = false;
    s.hashMB = 16;
    switch (std::uniform_int_distribution<int>(0, 3)(rng)) {
    case 0: s.skillLevel = 0; s.depth = 1 + int(rng() % 4); s.multiPV = 1 + int(rng() % 4); break;
    case 1: s.skillLevel = int(rng() % 6); s.depth = 4 + int(rng() % 4); break;
    case 2: s.limitStrength = true; s.elo = 1320 + int(rng() % 500); s.moveTimeMs = 150; break;
    default: s.skillLevel = 3 + int(rng() % 8); s.depth = 6 + int(rng() % 4); break;
    }
    return s;
}

}  // namespace

int recordMain(const Options& o) {
    ai::Engine e;
    if (!e.start() || !e.waitReady(60000)) {
        std::fprintf(stderr, "engine unavailable\n");
        return 1;
    }
    std::ofstream out(o.out, std::ios::app);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", o.out.c_str());
        return 1;
    }
    int written = 0;
    auto emit = [&](const Record& r) {
        out << writeRecord(r);
        out.flush();
        ++written;
    };

    // PGN games.
    for (const std::string& path : o.pgns) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const auto res = pgn::read(ss.str());
        for (const pgn::ParsedGame& pg : res.games) {
            if (!pg.ok()) continue;
            const bool coachWhite = pg.record.tag("White") == "Coach", coachBlack = pg.record.tag("Black") == "Coach";
            Game g;
            if (!pg.record.fen.empty() && !g.resetFromFEN(pg.record.fen)) continue;
            const std::vector<Position> pos = pg.record.positions();
            ai::EngineSettings host;
            host.hashMB = 64;
            host.threads = o.threads;
            e.configure(host);
            e.newGame();
            for (size_t i = 0; i < pg.record.plies.size(); ++i) {
                const Move m = g.position().parseSAN(pg.record.plies[i].san);
                if (!m.valid()) break;
                const Color mover = g.position().sideToMove();
                const bool human = mover == White ? !coachWhite : !coachBlack;
                if (human) emit(recordMove(e, g, m, o.depth));
                if (!g.play(m)) break;
            }
            std::fprintf(stderr, "pgn %s: %d records\n", path.c_str(), written);
        }
    }

    // Games played here.
    std::mt19937 rng(o.seed);
    for (int game = 0; game < o.selfplay; ++game) {
        const int level = 1 + int(rng() % 6);
        const Color human = (rng() & 1) ? White : Black;
        ai::EngineSettings hs = humanSettings(rng);
        ai::EngineSettings cs = ai::coachLevelSettings(level);
        cs.humanize = false;
        cs.useClock = false;
        cs.hashMB = 16;
        if (!cs.depth && !cs.moveTimeMs && !cs.nodes) cs.moveTimeMs = 200;
        hs.threads = cs.threads = o.threads;
        const double randomMove = (rng() % 3 == 0) ? 0.08 : 0.0;   // some games get the odd wild move
        Game g;
        e.newGame();
        int plies = 0;
        while (!g.isOver() && plies < o.maxPlies) {
            const bool humanTurn = g.position().sideToMove() == human;
            Move m;
            if (humanTurn && randomMove > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < randomMove) {
                const std::vector<Move> legal = g.position().legalMoves();
                m = legal[rng() % legal.size()];
            } else {
                e.configure(humanTurn ? hs : cs);
                const std::string uci = waitMove(e, g);
                m = g.position().parseUCI(uci);
            }
            if (!m.valid()) break;
            if (humanTurn) emit(recordMove(e, g, m, o.depth));
            g.play(m);
            ++plies;
        }
        std::fprintf(stderr, "selfplay game %d (coach level %d, human %s): %d plies, %d records\n", game, level,
                     human == White ? "White" : "Black", plies, written);
    }
    e.shutdown();
    return 0;
}

}  // namespace audit
