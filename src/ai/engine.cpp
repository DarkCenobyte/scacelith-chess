// UCI client for the embedded Stockfish (see uci_host.h for the in-process transport).
//
// Everything runs on the caller's (game) thread without blocking: commands go into the engine's
// input queue, and every public method first drains the engine's output ("pump"). Requests are
// serialised as a queue of tasks; while a search is running only "stop" is sent, everything else
// waits for its "bestmove", so Stockfish never sees setoption/position/ucinewgame mid-search.
//  * A new move request supersedes the previous one (queued: dropped; running: stopped and its
//    bestmove discarded); the same for evaluation requests. Move and eval requests queue behind
//    each other.
//  * UCI options are sent lazily before each "go", only when they differ from what the engine has.
//  * Two sides may share the engine (AI vs AI): a move search with other strength settings than
//    the previous one clears the hash and the search history first (see Engine::configure).
//  * Stockfish 19 ends the whole process on an illegal move in "position", so every move list is
//    replayed with the game's own rules first; a list that fails never reaches the engine and the
//    request fails like one on a stopped engine.
#include "ai/engine.h"

#include "ai/behavior.h"
#include "ai/uci_host.h"
#include "chess/chess.h"
#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <utility>

namespace ai {
namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr int kMateScore = 100000;
constexpr int kEvalDepth = 12;          // requestEval: fixed depth, full strength ...
constexpr int kEvalMaxMs = 1500;        // ... capped in time
// Untimed game and no depth/nodes/movetime in the settings: search time per move. The humanised
// thinking time (2..12 s) runs concurrently, so this rarely delays the game.
constexpr int kUntimedFullMs = 3000;    // full strength
constexpr int kUntimedLimitedMs = 1000; // Skill / UCI_Elo handicap (Stockfish calibrated UCI_Elo at 60s+0.6s)
constexpr int kUntimedSafetyMs = 30000; // untimed game with only a depth/nodes cap

int msSince(SteadyClock::time_point t) {
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - t).count());
}

std::vector<std::string> split(const std::string& s) {
    std::istringstream is(s);
    std::vector<std::string> out;
    std::string t;
    while (is >> t) out.push_back(t);
    return out;
}

// Index of the first move of `moves` (UCI, from the standard start position) that is not legal
// where it is played, or -1 when the whole line is legal. The game's parser is at least as strict
// as Stockfish's (castling as the king's two-square move, a promotion letter on promotions only),
// so Stockfish accepts every line that passes.
int firstIllegalMove(const std::vector<std::string>& moves) {
    chess::Position pos;
    for (size_t i = 0; i < moves.size(); ++i) {
        const chess::Move m = pos.parseUCI(moves[i]);
        if (!m.valid()) return int(i);
        pos.makeMove(m);
    }
    return -1;
}

struct Job {
    enum Kind { Move, Eval } kind = Move;
    std::vector<std::string> moves;
    ClockInfo clock;
    bool stopRequested = false;  // stop as soon as one iteration has completed
    bool stopSent = false;
    bool superseded = false;     // result is discarded
    int depthSeen = 0;           // deepest completed iteration reported ("info depth N ... score")
    bool haveScore = false;
    int scoreCp = 0;             // multipv 1 score, side to move's point of view
    SteadyClock::time_point sentAt;
};

struct Task {
    std::vector<std::string> lines;  // raw commands (ucinewgame, isready)
    std::unique_ptr<Job> job;        // or a search
};

}  // namespace

struct Engine::Impl {
    const void* owner = nullptr;
    bool started = false;
    bool uciOk = false;
    bool handshakeDone = false;
    SteadyClock::time_point startedAt;
    EngineSettings settings;
    std::map<std::string, std::string> sentOptions;  // what the engine currently has
    std::deque<Task> outbox;
    std::unique_ptr<Job> inFlight;

    bool moveReady = false;
    std::string move;
    int moveEval = 0;
    int lastSearchMs = 0;
    bool evalReady = false;
    int evalCp = 0;

    bool hashWarmedByEval = false;       // an eval search ran since the last move search
    bool haveLastMoveSettings = false;   // settings of the last move search (side switches)
    EngineSettings lastMoveSettings;
    int hashClears = 0;
    int readySent = 0, readyOks = 0;     // isready / readyok count (sync)

    std::vector<std::string> lastMoves;  // position of the last completed move search
    std::string lastBest;                // and its result (recapture detection)
    std::mt19937 rng{std::random_device{}() ^ unsigned(SteadyClock::now().time_since_epoch().count())};

    detail::UciHost& host() { return detail::UciHost::instance(); }
    void send(const std::string& line) {
        if (line == "isready") ++readySent;
        host().send(line);
    }

    void pump() {
        if (!started) return;
        std::string line;
        while (host().poll(line)) handle(line);
        dispatch();
    }

    // Blocking variant for waitReady(): handles lines as they arrive until `done` or timeout.
    template <class Pred> bool pumpUntil(Pred done, int timeoutMs) {
        auto t0 = SteadyClock::now();
        pump();
        while (started && !done()) {
            int left = timeoutMs - msSince(t0);
            if (left <= 0) break;
            std::string line;
            if (host().waitLine(line, std::min(left, 50))) handle(line);
            dispatch();
        }
        return done();
    }

    void handle(const std::string& line) {
        if (line.compare(0, 5, "info ") == 0) {
            if (inFlight) parseInfo(line, *inFlight);
        } else if (line.compare(0, 8, "bestmove") == 0) {
            complete(line);
        } else if (line == "uciok") {
            uciOk = true;
        } else if (line == "readyok") {
            ++readyOks;
            if (!handshakeDone && uciOk) {
                handshakeDone = true;
                LOGI("ai: Stockfish ready in %d ms", msSince(startedAt));
            }
        }
        // id / option / "info string" / other lines are ignored
    }

    void parseInfo(const std::string& line, Job& job) {
        auto t = split(line);
        if (t.size() > 1 && t[1] == "string") return;
        int depth = 0, multipv = 1;
        bool hasScore = false;
        int score = 0;
        for (size_t i = 1; i + 1 < t.size(); ++i) {
            if (t[i] == "depth") depth = std::atoi(t[i + 1].c_str());
            else if (t[i] == "multipv") multipv = std::atoi(t[i + 1].c_str());
            else if (t[i] == "score" && i + 2 < t.size()) {
                int v = std::atoi(t[i + 2].c_str());
                if (t[i + 1] == "cp") { score = v; hasScore = true; }
                else if (t[i + 1] == "mate") { score = v > 0 ? kMateScore : -kMateScore; hasScore = true; }
            } else if (t[i] == "pv") break;
        }
        if (!hasScore) return;  // currmove / hashfull lines
        job.depthSeen = std::max(job.depthSeen, depth);
        if (multipv == 1) {
            job.scoreCp = score;
            job.haveScore = true;
        }
        if (job.stopRequested && !job.stopSent && job.depthSeen >= 1) {
            send("stop");
            job.stopSent = true;
        }
    }

    void complete(const std::string& line) {
        if (!inFlight) return;  // stray bestmove (should not happen)
        std::unique_ptr<Job> job = std::move(inFlight);
        if (job->superseded) return;
        auto t = split(line);
        std::string best = t.size() > 1 && t[1] != "(none)" ? t[1] : std::string();
        int eval = job->haveScore ? job->scoreCp : 0;
        if (job->kind == Job::Move) {
            moveReady = true;
            move = best;
            moveEval = eval;
            lastSearchMs = msSince(job->sentAt);
            lastMoves = std::move(job->moves);
            lastBest = best;
        } else {
            evalReady = true;
            evalCp = eval;
        }
    }

    void dispatch() {
        while (started && !inFlight && !outbox.empty()) {
            Task task = std::move(outbox.front());
            outbox.pop_front();
            for (const auto& l : task.lines) send(l);
            if (task.job) startJob(std::move(task.job));
        }
    }

    void setOption(const char* name, const std::string& value) {
        auto it = sentOptions.find(name);
        if (it != sentOptions.end() && it->second == value) return;
        send(std::string("setoption name ") + name + " value " + value);
        sentOptions[name] = value;
    }

    void syncOptions(const Job& job) {
        const EngineSettings& s = settings;
        const int threads = std::clamp(s.threads, 1, 256);
        // Several search threads (engine.threads): one NUMA node with every CPU, set before the
        // thread pool grows. On a machine with several nodes Stockfish would otherwise bind the
        // threads to nodes and copy the network (115 MB) to each. It never does either with one
        // thread, where the option would only cost a copy of the network (~0.2 s) per session.
        if (threads > 1) setOption("NumaPolicy", "none");
        setOption("Threads", std::to_string(threads));
        setOption("Hash", std::to_string(std::clamp(s.hashMB, 1, 4096)));
        if (job.kind == Job::Eval) {  // honest, full-strength evaluation
            setOption("Skill Level", "20");
            setOption("UCI_LimitStrength", "false");
            setOption("MultiPV", "1");
            return;
        }
        setOption("Skill Level", std::to_string(std::clamp(s.skillLevel, 0, 20)));
        setOption("UCI_LimitStrength", s.limitStrength ? "true" : "false");
        setOption("UCI_Elo", std::to_string(std::clamp(s.elo, 1320, 3190)));
        setOption("MultiPV", std::to_string(std::clamp(s.multiPV, 1, 64)));
        setOption("Move Overhead", std::to_string(std::clamp(job.clock.moveOverheadMs, 0, 5000)));
    }

    std::string goCommand(const Job& job) const {
        std::ostringstream go;
        go << "go";
        if (job.kind == Job::Eval) {
            go << " depth " << kEvalDepth << " movetime " << kEvalMaxMs;
            return go.str();
        }
        const EngineSettings& s = settings;
        const ClockInfo& c = job.clock;
        const bool timed = s.useClock && c.timed;
        if (timed) {
            // wtime 0 would disable Stockfish's time management: always pass at least 1 ms.
            go << " wtime " << std::max<int64_t>(1, c.whiteMs) << " btime " << std::max<int64_t>(1, c.blackMs)
               << " winc " << std::max<int64_t>(0, c.whiteIncMs) << " binc " << std::max<int64_t>(0, c.blackIncMs);
        }
        // No automatic depth cap for the Skill handicap: Stockfish picks its handicapped move after
        // iteration 1 + int(level), but the scores of that shallow multi-PV search lean on the hash
        // table filled by deeper iterations (of this and earlier moves). Measured: Skill Level 0
        // at "movetime 30" beats Skill Level 0 at "depth 1" by ~150 Elo. UCI_Elo was calibrated
        // with normal (deep) searches, so presets only cap the depth where they want that loss.
        const int depth = s.depth;
        if (depth > 0) go << " depth " << depth;
        if (s.nodes > 0) go << " nodes " << s.nodes;
        int movetime = s.moveTimeMs;
        if (!timed && movetime <= 0) {
            if (depth > 0 || s.nodes > 0) movetime = kUntimedSafetyMs;
            else movetime = detail::skillLevel(s) >= 0.0 ? kUntimedLimitedMs : kUntimedFullMs;
        }
        if (movetime > 0) go << " movetime " << movetime;
        return go.str();
    }

    // A request whose move list is not legal: Stockfish would end the process on its "position"
    // command, so nothing is sent and the request fails as on an engine that is not running (empty
    // move, neutral evaluation).
    void fail(const Job& job, int illegalAt) {
        LOGW("ai: move %d (%s) of the requested line is illegal, the request is not sent to Stockfish",
             illegalAt + 1, job.moves[size_t(illegalAt)].c_str());
        if (job.kind == Job::Move) {
            moveReady = true;
            move.clear();
            moveEval = 0;
            lastSearchMs = 0;
            lastMoves = job.moves;
            lastBest.clear();
        } else {
            evalReady = true;
            evalCp = 0;
        }
    }

    void startJob(std::unique_ptr<Job> job) {
        if (int illegalAt = firstIllegalMove(job->moves); illegalAt >= 0) {
            fail(*job, illegalAt);
            return;
        }
        syncOptions(*job);
        if (job->kind == Job::Eval) {
            hashWarmedByEval = true;
        } else {
            // Depth-capped handicaps were calibrated with a hash table that only ever saw their own
            // shallow searches; a deep evaluation in between would make the next move stronger.
            bool clear = hashWarmedByEval && settings.depth > 0 && detail::skillLevel(settings) >= 0.0;
            // The other side of an AI vs AI game searched last: start from a clean table.
            if (haveLastMoveSettings && !detail::sameStrength(lastMoveSettings, settings)) clear = true;
            if (clear) {
                send("setoption name Clear Hash");
                ++hashClears;
            }
            hashWarmedByEval = false;
            haveLastMoveSettings = true;
            lastMoveSettings = settings;
        }
        std::string pos = "position startpos";
        if (!job->moves.empty()) {
            pos += " moves";
            for (const auto& m : job->moves) pos += ' ' + m;
        }
        send(pos);
        send(goCommand(*job));
        job->sentAt = SteadyClock::now();
        inFlight = std::move(job);
    }

    // Drops queued requests of `kind` and stops a running one (its result is discarded).
    void cancel(Job::Kind kind) {
        outbox.erase(std::remove_if(outbox.begin(), outbox.end(),
                                    [kind](const Task& t) { return t.job && t.job->kind == kind; }),
                     outbox.end());
        if (inFlight && inFlight->kind == kind && !inFlight->superseded) {
            inFlight->superseded = true;
            if (!inFlight->stopSent) {
                send("stop");
                inFlight->stopSent = true;
            }
        }
    }

    void enqueue(Job::Kind kind, const std::vector<std::string>& moves, const ClockInfo& clock) {
        auto job = std::make_unique<Job>();
        job->kind = kind;
        job->moves = moves;
        job->clock = clock;
        Task task;
        task.job = std::move(job);
        outbox.push_back(std::move(task));
    }

    void resetSession() {
        uciOk = handshakeDone = false;
        sentOptions.clear();
        outbox.clear();
        inFlight.reset();
        moveReady = evalReady = false;
        hashWarmedByEval = false;
        haveLastMoveSettings = false;
        readySent = readyOks = 0;
        move.clear();
        lastMoves.clear();
        lastBest.clear();
    }
};

Engine::Engine() : impl_(new Impl) { impl_->owner = this; }

Engine::~Engine() {
    shutdown();
    delete impl_;
}

bool Engine::start() {
    Impl& d = *impl_;
    if (d.started) return true;
    if (!d.host().acquire(d.owner)) return false;
    d.resetSession();
    d.started = true;
    d.startedAt = SteadyClock::now();
    d.send("uci");
    d.send("isready");
    return true;
}

void Engine::shutdown() {
    Impl& d = *impl_;
    if (!d.started) return;
    d.send("stop");
    d.host().release(d.owner);  // "quit" + join; the session frees its hash table and network
    d.started = false;
    d.resetSession();
}

bool Engine::available() const { return impl_->started && impl_->host().ownedBy(impl_->owner); }

void Engine::setArchLimit(const std::string& arch) { detail::UciHost::instance().setArchLimit(arch); }

bool Engine::ready() const {
    impl_->pump();
    return impl_->handshakeDone;
}

bool Engine::waitReady(int timeoutMs) {
    Impl& d = *impl_;
    return d.pumpUntil([&d] { return d.handshakeDone; }, timeoutMs);
}

void Engine::newGame() {
    Impl& d = *impl_;
    if (!d.started) return;
    d.cancel(Job::Move);
    d.cancel(Job::Eval);
    d.moveReady = d.evalReady = false;
    d.lastMoves.clear();
    d.lastBest.clear();
    d.haveLastMoveSettings = false;  // ucinewgame clears the hash anyway
    d.hashWarmedByEval = false;
    Task task;
    task.lines = {"ucinewgame", "isready"};
    d.outbox.push_back(std::move(task));
    d.pump();
}

void Engine::configure(const EngineSettings& s) { impl_->settings = s; }

void Engine::clearHash() {
    Impl& d = *impl_;
    if (!d.started) return;
    Task task;
    task.lines = {"setoption name Clear Hash"};
    d.outbox.push_back(std::move(task));
    ++d.hashClears;
    d.pump();
}

int Engine::hashClears() const { return impl_->hashClears; }

bool Engine::sync(int timeoutMs) {
    Impl& d = *impl_;
    if (!d.started) return false;
    Task task;
    task.lines = {"isready"};
    d.outbox.push_back(std::move(task));
    return d.pumpUntil([&d] { return d.outbox.empty() && !d.inFlight && d.readyOks >= d.readySent; }, timeoutMs);
}

const EngineSettings& Engine::settings() const { return impl_->settings; }

void Engine::requestMove(const std::vector<std::string>& uciMoves, const ClockInfo& clock) {
    Impl& d = *impl_;
    d.move.clear();
    if (!d.started) {  // fail at once: moveReady() turns true and takeMove() returns ""
        d.moveReady = true;
        return;
    }
    d.cancel(Job::Move);
    d.moveReady = false;
    d.enqueue(Job::Move, uciMoves, clock);
    d.pump();
}

bool Engine::moveReady() const {
    impl_->pump();
    return impl_->moveReady;
}

std::string Engine::takeMove(int* evalCp) {
    Impl& d = *impl_;
    d.pump();
    if (!d.moveReady) return {};
    d.moveReady = false;
    if (evalCp) *evalCp = d.moveEval;
    return std::move(d.move);
}

void Engine::stopSearch() {
    Impl& d = *impl_;
    d.pump();
    for (auto& t : d.outbox)
        if (t.job) t.job->stopRequested = true;
    if (d.inFlight && !d.inFlight->superseded) {
        Job& j = *d.inFlight;
        j.stopRequested = true;
        // Never stop before one iteration completed: Stockfish would answer with an unsearched
        // (possibly absurd) move. parseInfo() sends the stop once depth 1 has been reported.
        if (!j.stopSent && j.depthSeen >= 1) {
            d.send("stop");
            j.stopSent = true;
        }
    }
}

int Engine::lastSearchMs() const { return impl_->lastSearchMs; }

void Engine::requestEval(const std::vector<std::string>& uciMoves) {
    Impl& d = *impl_;
    if (!d.started) {  // fail at once with a neutral 0
        d.evalReady = true;
        d.evalCp = 0;
        return;
    }
    d.cancel(Job::Eval);
    d.evalReady = false;
    d.enqueue(Job::Eval, uciMoves, ClockInfo{});
    d.pump();
}

bool Engine::evalReady() const {
    impl_->pump();
    return impl_->evalReady;
}

int Engine::takeEval() {
    Impl& d = *impl_;
    d.pump();
    if (!d.evalReady) return 0;
    d.evalReady = false;
    return d.evalCp;
}

int Engine::thinkTimeMs(const ClockInfo& clock, int plyCount, int legalMoveCount, bool inCheck) const {
    Impl& d = *impl_;
    // Recapture detection needs the move that was just computed for this very position.
    bool recapture = !d.lastBest.empty() && int(d.lastMoves.size()) == plyCount &&
                     detail::isRecapture(d.lastMoves, d.lastBest);
    std::normal_distribution<double> gauss(0.0, 1.0);
    return detail::humanThinkTimeMs(d.settings, clock, plyCount, legalMoveCount, inCheck, recapture, gauss(d.rng));
}

bool Engine::acceptsDraw(int evalCp, int plyCount) const { return detail::acceptsDrawOffer(evalCp, plyCount); }

bool Engine::offersDraw(int evalCp, int plyCount, int pliesSinceOwnOffer) const {
    return detail::offersDraw(evalCp, plyCount, pliesSinceOwnOffer);
}

}  // namespace ai
