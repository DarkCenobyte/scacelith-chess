// UCI client for the embedded Stockfish (see uci_host.h for the in-process transport).
//
// Everything runs on the caller's (game) thread without blocking: commands go into the engine's
// input queue, and every public method first drains the engine's output ("pump"). Requests are
// serialised as a queue of tasks; while a search is running only "stop" is sent, everything else
// waits for its "bestmove", so Stockfish never sees setoption/position/ucinewgame mid-search.
//  * A new move request supersedes the previous one (queued: dropped; running: stopped and its
//    bestmove discarded); the same for evaluation requests. Move and eval requests queue behind
//    each other. Analyses (full strength, MultiPV, see analysis.h) never supersede anything: each
//    has an id and its own result slot, and runs in turn by priority (moves and evaluations have
//    priority 0, so a move request overtakes queued background analyses).
//  * UCI options are sent lazily before each "go", only when they differ from what the engine has.
//    Every value is clamped to its range first: Stockfish silently ignores an out-of-range value,
//    and the cache would then lie.
//  * Two sides may share the engine (AI vs AI): a move search with other strength settings than
//    the previous one clears the hash and the search history first (see Engine::configure).
//  * Stockfish 19 ends the whole process on an illegal move or FEN in "position" and on a malformed
//    "go", so every position is replayed with the game's own rules first (FEN checked, then sent
//    as re-emitted by chess::Position), search moves are checked in that position, and "go" only
//    carries numbers formatted here; a request that fails never reaches the engine and fails like
//    one on a stopped engine.
#include "ai/engine.h"

#include "ai/behavior.h"
#include "ai/uci_host.h"
#include "chess/chess.h"
#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iterator>
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
// Analysis limits (AnalysisRequest), clamped: Stockfish's MultiPV option stops at MAX_MOVES (256),
// its iterative deepening at MAX_PLY (246).
constexpr int kMaxMultiPV = 256;
constexpr int kMaxDepth = 245;
constexpr int kMaxMoveTimeMs = 3600 * 1000;
constexpr int64_t kMaxNodes = int64_t(1) << 40;
constexpr int kMaxMateIn = 100;

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

// Stockfish's own FEN checks (Position::set) that chess::Position::setFEN does not share: at most
// as many promoted pieces (beyond 2 knights, bishops, rooks and 1 queen) as pawns missing, per side.
bool stockfishAcceptsMaterial(const chess::Position& p) {
    for (int c = 0; c < 2; ++c) {
        int n[7] = {0, 0, 0, 0, 0, 0, 0};
        for (int s = 0; s < 64; ++s) {
            const chess::Piece pc = p.at(chess::Square(s));
            if (!pc.empty() && pc.color == chess::Color(c)) ++n[pc.type];
        }
        const int extra = std::max(n[chess::Knight] - 2, 0) + std::max(n[chess::Bishop] - 2, 0) +
                          std::max(n[chess::Rook] - 2, 0) + std::max(n[chess::Queen] - 1, 0);
        if (extra > 8 - n[chess::Pawn]) return false;
    }
    return true;
}

// FEN for Stockfish's "position fen": chess::Position's normalised FEN (castling rights and en
// passant square only when real, which also passes Stockfish's checks) with the counters in
// Stockfish's range (rule50 <= 32767, game ply <= 100000; it ends the process beyond). The
// halfmove clock stays above 100 when it was, so the fifty-move rule still counts.
std::string stockfishFen(const chess::Position& p) {
    const std::vector<std::string> f = split(p.fen());
    std::string s;
    for (size_t i = 0; i < 4 && i < f.size(); ++i) s += f[i] + ' ';
    return s + std::to_string(std::clamp(p.halfmoveClock(), 0, 150)) + ' ' +
           std::to_string(std::clamp(p.fullmoveNumber(), 1, 9999));
}

// The "position" command for a start position ("" = standard) + moves (UCI), each re-emitted by
// the game's rules; false with the reason when Stockfish would reject it (and end the process).
// The game's parser is at least as strict as Stockfish's (castling as the king's two-square move,
// a promotion letter on promotions only), so Stockfish accepts every line that passes. `root` is
// the position reached.
bool positionCommand(const std::string& startFen, const std::vector<std::string>& moves, std::string& command,
                     chess::Position& root, std::string& why) {
    chess::Position pos;
    std::string cmd = "position startpos";
    if (!startFen.empty()) {
        if (!pos.setFEN(startFen)) {
            why = "the start position \"" + startFen + "\" is not a valid FEN";
            return false;
        }
        if (!stockfishAcceptsMaterial(pos)) {
            why = "the start position \"" + startFen + "\" has more promoted pieces than missing pawns";
            return false;
        }
        cmd = "position fen " + stockfishFen(pos);
    }
    std::string list;
    for (size_t i = 0; i < moves.size(); ++i) {
        const chess::Move m = pos.parseUCI(moves[i]);
        if (!m.valid()) {
            why = "move " + std::to_string(i + 1) + " (" + moves[i] + ") of the requested line is illegal";
            return false;
        }
        list += ' ' + pos.toUCI(m);
        pos.makeMove(m);
    }
    if (!list.empty()) cmd += " moves" + list;
    command = std::move(cmd);
    root = pos;
    return true;
}

// detail::isRecapture for a custom start position.
bool isRecaptureFrom(const std::string& startFen, const std::vector<std::string>& moves, const std::string& reply) {
    chess::Position pos;
    if (moves.empty() || !pos.setFEN(startFen)) return false;
    chess::Move last;
    for (const auto& s : moves) {
        last = pos.parseUCI(s);
        if (!last.valid()) return false;
        pos.makeMove(last);
    }
    const chess::Move r = pos.parseUCI(reply);
    return (last.flags & chess::MoveCapture) && r.valid() && r.to == last.to;
}

struct Job {
    enum Kind { Move, Eval, Analyse } kind = Move;
    std::string startFen;        // "" = standard start position
    std::vector<std::string> moves;
    ClockInfo clock;
    int priority = 0;            // queue order (Engine::Impl::enqueue)
    bool stopRequested = false;  // stop as soon as one iteration has completed
    bool stopSent = false;
    bool superseded = false;     // result is discarded
    int depthSeen = 0;           // deepest completed iteration reported ("info depth N ... score")
    bool haveScore = false;
    int scoreCp = 0;             // multipv 1 score, side to move's point of view
    SteadyClock::time_point sentAt;
    // Analyse: the checked request (limits clamped, search moves re-emitted) and the result being
    // filled (id, side to move and FEN from the request; lines = the latest report, index = multipv - 1).
    AnalysisRequest req;
    Analysis result;
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

    uint32_t nextAnalysisId = 1;
    std::map<uint32_t, Analysis> analyses;  // ready, not taken yet

    bool hashWarmedByEval = false;       // an eval or analysis search ran since the last move search
    bool haveLastMoveSettings = false;   // settings of the last move search (side switches)
    EngineSettings lastMoveSettings;
    int hashClears = 0;
    int readySent = 0, readyOks = 0;     // isready / readyok count (sync)

    std::string lastStartFen;            // position of the last completed move search ("" = start)
    std::vector<std::string> lastMoves;
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
        if (job.kind == Job::Analyse) {
            parseAnalysisInfo(line, job);
        } else {
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
        }
        if (job.stopRequested && !job.stopSent && job.depthSeen >= 1) {
            send("stop");
            job.stopSent = true;
        }
    }

    // Stockfish prints its MultiPV lines best first, all of them after each iteration (and after a
    // stop), so a line with multipv 1 starts a new report and the result is the last report. Only
    // an iteration stopped at depth 1 leaves lines out (unsearched ones): the report is shorter.
    void parseAnalysisInfo(const std::string& line, Job& job) {
        PvLine l;
        if (!parseInfoLine(line, l, &job.result.nodes, &job.result.timeMs)) return;
        job.depthSeen = std::max(job.depthSeen, l.depth);
        std::vector<PvLine>& lines = job.result.lines;
        if (l.multipv == 1) lines.clear();
        if (l.multipv > kMaxMultiPV) return;
        if (lines.size() < size_t(l.multipv)) lines.resize(size_t(l.multipv), PvLine{0, 0, 0, Score{}, {}});
        lines[size_t(l.multipv - 1)] = std::move(l);
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
            lastStartFen = std::move(job->startFen);
            lastMoves = std::move(job->moves);
            lastBest = best;
        } else if (job->kind == Job::Eval) {
            evalReady = true;
            evalCp = eval;
        } else {
            Analysis& a = job->result;
            a.ok = true;
            a.bestMove = best;
            a.noLegalMove = best.empty();  // positions without legal moves are answered locally
            std::vector<PvLine> lines;     // drop the slots of lines a stopped report left out
            for (size_t i = 0; i < a.lines.size(); ++i)
                if (a.lines[i].multipv == int(i + 1)) lines.push_back(std::move(a.lines[i]));
            a.lines = std::move(lines);
            a.depth = a.lines.empty() ? 0 : a.lines.front().depth;
            if (a.timeMs <= 0) a.timeMs = msSince(job->sentAt);
            const uint32_t id = a.id;
            analyses[id] = std::move(a);
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
        // Threads and Hash always come from the current settings, whatever the kind of job:
        // changing either between jobs would resize the pool / table and clear the hash.
        setOption("Threads", std::to_string(threads));
        setOption("Hash", std::to_string(std::clamp(s.hashMB, 1, 4096)));
        // "wdl W D L" in every info line: analyses use it, the move / eval parser skips it.
        setOption("UCI_ShowWDL", "true");
        if (job.kind != Job::Move) {  // honest, full-strength evaluation / analysis
            const int multiPV = job.kind == Job::Eval ? 1 : std::clamp(job.req.multiPV, 1, kMaxMultiPV);
            setOption("Skill Level", "20");
            setOption("UCI_LimitStrength", "false");
            setOption("MultiPV", std::to_string(multiPV));
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
        if (job.kind == Job::Analyse) {  // limits checked and clamped by requestAnalysis
            const AnalysisRequest& r = job.req;
            if (r.depth > 0) go << " depth " << r.depth;
            if (r.nodes > 0) go << " nodes " << r.nodes;
            if (r.moveTimeMs > 0) go << " movetime " << r.moveTimeMs;
            if (r.mateIn > 0) go << " mate " << r.mateIn;
            if (!r.searchMoves.empty()) {  // must be the last token of the line
                go << " searchmoves";
                for (const auto& m : r.searchMoves) go << ' ' << m;
            }
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

    // A request whose position is not legal: Stockfish would end the process on its "position"
    // command, so nothing is sent and the request fails as on an engine that is not running (empty
    // move, neutral evaluation, analysis not ok).
    void fail(Job& job, const std::string& why) {
        LOGW("ai: %s, the request is not sent to Stockfish", why.c_str());
        if (job.kind == Job::Move) {
            moveReady = true;
            move.clear();
            moveEval = 0;
            lastSearchMs = 0;
            lastStartFen = job.startFen;
            lastMoves = job.moves;
            lastBest.clear();
        } else if (job.kind == Job::Eval) {
            evalReady = true;
            evalCp = 0;
        } else {
            Analysis a;
            a.id = job.result.id;
            analyses[a.id] = std::move(a);
        }
    }

    void startJob(std::unique_ptr<Job> job) {
        std::string position, why;
        chess::Position root;
        if (!positionCommand(job->startFen, job->moves, position, root, why)) {
            fail(*job, why);
            return;
        }
        syncOptions(*job);
        if (job->kind != Job::Move) {
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
        send(position);
        send(goCommand(*job));
        job->sentAt = SteadyClock::now();
        inFlight = std::move(job);
    }

    // Stops the running search and discards its result.
    void supersedeInFlight() {
        if (!inFlight || inFlight->superseded) return;
        inFlight->superseded = true;
        if (!inFlight->stopSent) {
            send("stop");
            inFlight->stopSent = true;
        }
    }

    // Drops queued requests of `kind` and stops a running one (its result is discarded).
    void cancel(Job::Kind kind) {
        outbox.erase(std::remove_if(outbox.begin(), outbox.end(),
                                    [kind](const Task& t) { return t.job && t.job->kind == kind; }),
                     outbox.end());
        if (inFlight && inFlight->kind == kind) supersedeInFlight();
    }

    // The same for analysis `id` (0 = all of them), whose ready result is discarded as well.
    void cancelAnalysis(uint32_t id) {
        auto matches = [id](const Job& j) { return j.kind == Job::Analyse && (id == 0 || j.result.id == id); };
        outbox.erase(std::remove_if(outbox.begin(), outbox.end(), [&](const Task& t) { return t.job && matches(*t.job); }),
                     outbox.end());
        if (inFlight && matches(*inFlight)) supersedeInFlight();
        if (id == 0) analyses.clear();
        else analyses.erase(id);
    }

    // Queues a search. A job goes after every queued raw command (ucinewgame, isready, Clear Hash:
    // what was requested before them must not see what they change) and after every job of equal
    // or higher priority, so equal priorities keep their order.
    void enqueue(std::unique_ptr<Job> job) {
        auto at = outbox.end();
        while (at != outbox.begin()) {
            auto prev = std::prev(at);
            if (!prev->job || prev->job->priority >= job->priority) break;
            at = prev;
        }
        Task task;
        task.job = std::move(job);
        outbox.insert(at, std::move(task));
    }

    void enqueue(Job::Kind kind, const std::string& startFen, const std::vector<std::string>& moves,
                 const ClockInfo& clock) {
        auto job = std::make_unique<Job>();
        job->kind = kind;
        job->startFen = startFen;
        job->moves = moves;
        job->clock = clock;
        enqueue(std::move(job));
    }

    // Analyses that were queued or running when the session ended fail (ok false), so a caller
    // waiting for one of them does not wait forever.
    void failPendingAnalyses() {
        auto failOne = [this](const Job& j) {
            if (j.kind != Job::Analyse || j.superseded) return;
            Analysis a;
            a.id = j.result.id;
            analyses[a.id] = std::move(a);
        };
        for (const Task& t : outbox)
            if (t.job) failOne(*t.job);
        if (inFlight) failOne(*inFlight);
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
        lastStartFen.clear();
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
    d.failPendingAnalyses();
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
    d.cancelAnalysis(0);
    d.moveReady = d.evalReady = false;
    d.lastStartFen.clear();
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
    requestMoveFrom(std::string(), uciMoves, clock);
}

void Engine::requestMoveFrom(const std::string& startFen, const std::vector<std::string>& uciMoves,
                             const ClockInfo& clock) {
    Impl& d = *impl_;
    d.move.clear();
    if (!d.started) {  // fail at once: moveReady() turns true and takeMove() returns ""
        d.moveReady = true;
        return;
    }
    d.cancel(Job::Move);
    d.moveReady = false;
    d.enqueue(Job::Move, startFen, uciMoves, clock);
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
    d.enqueue(Job::Eval, std::string(), uciMoves, ClockInfo{});
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
    const bool samePosition = int(d.lastMoves.size()) == plyCount;
    bool recapture = !d.lastBest.empty() && samePosition &&
                     (d.lastStartFen.empty() ? detail::isRecapture(d.lastMoves, d.lastBest)
                                             : isRecaptureFrom(d.lastStartFen, d.lastMoves, d.lastBest));
    // humanThinkTimeMs tells the side to move (whose clock counts) by the parity of plyCount: from
    // a FEN with Black to move, the first ply is Black's.
    int ply = plyCount;
    chess::Position start;
    if (samePosition && !d.lastStartFen.empty() && start.setFEN(d.lastStartFen) && start.sideToMove() == chess::Black)
        ++ply;
    std::normal_distribution<double> gauss(0.0, 1.0);
    return detail::humanThinkTimeMs(d.settings, clock, ply, legalMoveCount, inCheck, recapture, gauss(d.rng));
}

uint32_t Engine::requestAnalysis(const AnalysisRequest& r) {
    Impl& d = *impl_;
    const uint32_t id = d.nextAnalysisId++;
    if (d.nextAnalysisId == 0) d.nextAnalysisId = 1;
    Analysis failed;
    failed.id = id;
    if (!d.started) {  // fail at once
        d.analyses[id] = failed;
        return id;
    }
    // Everything is checked now, so an invalid request fails at once instead of after the queue.
    auto job = std::make_unique<Job>();
    job->kind = Job::Analyse;
    job->startFen = r.startFen;
    job->moves = r.moves;
    job->priority = r.priority;
    AnalysisRequest& q = job->req;
    q.multiPV = std::clamp(r.multiPV, 1, kMaxMultiPV);
    q.depth = r.depth > 0 ? std::min(r.depth, kMaxDepth) : 0;
    q.moveTimeMs = r.moveTimeMs > 0 ? std::min(r.moveTimeMs, kMaxMoveTimeMs) : 0;
    q.nodes = r.nodes > 0 ? std::min(r.nodes, kMaxNodes) : 0;
    q.mateIn = r.mateIn > 0 ? std::min(r.mateIn, kMaxMateIn) : 0;
    q.priority = r.priority;
    std::string position, why;
    chess::Position root;
    if (!positionCommand(r.startFen, r.moves, position, root, why)) {
        LOGW("ai: analysis %u: %s, the request is not sent to Stockfish", unsigned(id), why.c_str());
        d.analyses[id] = failed;
        return id;
    }
    // A bare "go" (or one with only "mate N") never ends: "go infinite" waits for a stop.
    if (q.depth == 0 && q.moveTimeMs == 0 && q.nodes == 0) {
        LOGW("ai: analysis %u has no depth, movetime or node limit, the request is not sent to Stockfish",
             unsigned(id));
        d.analyses[id] = failed;
        return id;
    }
    // Stockfish drops unknown or illegal search moves silently and, with none left, searches every
    // move: the result would be attributed to the wrong move.
    for (const auto& s : r.searchMoves) {
        const chess::Move m = root.parseUCI(s);
        if (!m.valid()) {
            LOGW("ai: analysis %u: search move %s is not legal, the request is not sent to Stockfish", unsigned(id),
                 s.c_str());
            d.analyses[id] = failed;
            return id;
        }
        const std::string uci = root.toUCI(m);
        if (std::find(q.searchMoves.begin(), q.searchMoves.end(), uci) == q.searchMoves.end())
            q.searchMoves.push_back(uci);
    }
    Analysis& a = job->result;
    a.id = id;
    a.whiteToMove = root.sideToMove() == chess::White;
    a.fen = root.fen();
    if (!root.hasLegalMove()) {
        // Checkmate or stalemate: the answer is known (Stockfish would only print "info depth 0
        // score mate 0" or "score cp 0" and "bestmove (none)").
        const bool mated = root.inCheck();
        a.ok = true;
        a.noLegalMove = true;
        PvLine l;
        l.score.matedNow = mated;
        l.score.hasWdl = true;
        l.score.win = 0;
        l.score.draw = mated ? 0 : 1000;
        l.score.loss = mated ? 1000 : 0;
        a.lines.push_back(std::move(l));
        d.analyses[id] = std::move(a);
        return id;
    }
    d.enqueue(std::move(job));
    d.pump();
    return id;
}

bool Engine::analysisReady(uint32_t id) const {
    Impl& d = *impl_;
    d.pump();
    return d.analyses.count(id) != 0;
}

bool Engine::analysisPending(uint32_t id) const {
    Impl& d = *impl_;
    d.pump();
    if (d.inFlight && d.inFlight->kind == Job::Analyse && d.inFlight->result.id == id && !d.inFlight->superseded)
        return true;
    for (const Task& t : d.outbox)
        if (t.job && t.job->kind == Job::Analyse && t.job->result.id == id) return true;
    return false;
}

bool Engine::takeAnalysis(uint32_t id, Analysis& out) {
    Impl& d = *impl_;
    d.pump();
    auto it = d.analyses.find(id);
    if (it == d.analyses.end()) return false;
    out = std::move(it->second);
    d.analyses.erase(it);
    return true;
}

void Engine::stopAnalysis(uint32_t id) {
    Impl& d = *impl_;
    d.pump();
    for (auto& t : d.outbox)
        if (t.job && t.job->kind == Job::Analyse && t.job->result.id == id) t.job->stopRequested = true;
    if (d.inFlight && d.inFlight->kind == Job::Analyse && d.inFlight->result.id == id && !d.inFlight->superseded) {
        Job& j = *d.inFlight;
        j.stopRequested = true;
        if (!j.stopSent && j.depthSeen >= 1) {  // else parseInfo() stops it after depth 1
            d.send("stop");
            j.stopSent = true;
        }
    }
}

void Engine::cancelAnalysis(uint32_t id) {
    Impl& d = *impl_;
    d.cancelAnalysis(id);
    d.pump();
}

bool Engine::idle() const {
    Impl& d = *impl_;
    d.pump();
    return !d.started || (d.outbox.empty() && !d.inFlight);
}

bool Engine::acceptsDraw(int evalCp, int plyCount) const { return detail::acceptsDrawOffer(evalCp, plyCount); }

bool Engine::offersDraw(int evalCp, int plyCount, int pliesSinceOwnOffer) const {
    return detail::offersDraw(evalCp, plyCount, pliesSinceOwnOffer);
}

}  // namespace ai
