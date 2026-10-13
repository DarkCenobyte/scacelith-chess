// coach_audit review --verify D: the engine's verdict on every move a review shows or says. A line
// the coach plays on the table or names is "what would happen", so each of its moves should be one
// the players would play: a move that throws away a piece or a mate the engine sees at once is a
// line the player cannot believe (the queen left en prise, a defence that walks into mate).
#include "audit.h"

#include "ai/engine.h"
#include "coach/review.h"
#include "core/files.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

namespace audit {

using namespace chess;

namespace {
// The least cost (win percentage points) any finding counts: such a move is searched for again.
constexpr double kRecheck = 5.0;
}  // namespace

struct MoveJudge::Impl {
    ai::Engine engine;
    int depth = 14;
    bool ok = false;
    std::string cachePath;
    std::map<std::string, std::pair<double, std::string>> costs;   // "fen|uci" -> cost (rechecked), best move
    std::map<std::string, std::pair<ai::Score, std::string>> best;  // fen -> best score, best move
    int unsaved = 0;

    // The whole cache, rewritten (mode 0644 whatever the umask: files::create).
    void save() {
        if (cachePath.empty() || unsaved == 0) return;
        if (FILE* f = files::create(cachePath.c_str())) {
            for (const auto& kv : costs)
                std::fprintf(f, "%s|%.2f|%s|c\n", kv.first.c_str(), kv.second.first, kv.second.second.c_str());
            std::fclose(f);
        }
        unsaved = 0;
    }

    bool analyse(const Position& p, const std::string& only, ai::Analysis& out) {
        ai::AnalysisRequest r;
        r.startFen = p.fen();
        r.multiPV = 1;
        r.depth = depth;
        r.moveTimeMs = 0;   // depth only: reproducible
        if (!only.empty()) r.searchMoves = {only};
        const uint32_t id = engine.requestAnalysis(r);
        const auto t0 = std::chrono::steady_clock::now();
        while (!engine.analysisReady(id)) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::minutes(5)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return engine.takeAnalysis(id, out) && out.ok && !out.lines.empty();
    }
};

MoveJudge::MoveJudge(int depth, int threads, const std::string& cachePath) : d(new Impl) {
    d->depth = depth;
    d->cachePath = cachePath;
    if (!cachePath.empty()) {
        std::ifstream in(cachePath);
        std::string line;
        while (std::getline(in, line)) {
            // fen|uci|cost|best|c ("c": a cost of kRecheck or more was checked again, cost())
            std::istringstream ls(line);
            std::string fen, uci, cost, bestUci, checked;
            if (std::getline(ls, fen, '|') && std::getline(ls, uci, '|') && std::getline(ls, cost, '|') &&
                std::getline(ls, bestUci, '|')) {
                std::getline(ls, checked);
                const double c = std::atof(cost.c_str());
                if (c < kRecheck || checked == "c") d->costs[fen + "|" + uci] = {c, bestUci};
            }
        }
    }
    d->ok = d->engine.start() && d->engine.waitReady(60000);
    if (d->ok) {
        ai::EngineSettings s;
        s.hashMB = 64;
        s.threads = threads;
        d->engine.configure(s);
    }
}

MoveJudge::~MoveJudge() {
    d->save();
    if (d->ok) d->engine.shutdown();
}

bool MoveJudge::ok() const { return d->ok; }

double MoveJudge::cost(const Position& p, const Move& m, std::string* bestUci) {
    const std::string fen = p.fen(), uci = p.toUCI(m), key = fen + "|" + uci;
    auto hit = d->costs.find(key);
    if (hit != d->costs.end()) {
        if (bestUci) *bestUci = hit->second.second;
        return hit->second.first;
    }
    if (!d->ok) return -1.0;
    auto b = d->best.find(fen);
    if (b == d->best.end()) {
        ai::Analysis a;
        if (!d->analyse(p, "", a) || a.lines[0].pv.empty()) return -1.0;
        b = d->best.emplace(fen, std::make_pair(a.lines[0].score, a.lines[0].pv[0])).first;
    }
    double c = 0.0;
    std::string bestHere = b->second.second;
    if (bestHere != uci) {
        ai::Analysis a;
        if (!d->analyse(p, uci, a)) return -1.0;
        c = std::max(0.0, coach::winPercent(b->second.first) - coach::winPercent(a.lines[0].score));
    }
    // A move that costs something is searched for once more from a cleared hash table: at a fixed
    // depth the search depends on what the table holds, and a move the engine picks itself from a
    // clean start is its own choice at that depth (the lines the coach says are the engine's, from
    // searches about as deep), no mistake.
    if (c >= kRecheck) {
        d->engine.clearHash();
        ai::Analysis a;
        if (d->analyse(p, "", a) && !a.lines[0].pv.empty() && a.lines[0].pv[0] == uci) {
            c = 0.0;
            bestHere = uci;
        }
    }
    d->costs[key] = {c, bestHere};
    if (++d->unsaved >= 200) d->save();
    if (bestUci) *bestUci = bestHere;
    return c;
}

}  // namespace audit
