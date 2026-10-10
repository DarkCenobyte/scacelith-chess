// challenge_audit: the engine's checks of each candidate (the rules are in audit.h).
#include "audit.h"

#include "ai/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace challenges {

using namespace chess;
using ai::Score;

namespace {

constexpr int kWinCp = 200;          // a winning solution
constexpr int kWorseCp = 150;        // a clearly worse move: this far below the solution...
constexpr double kWorseChances = 0.3;  // ... and this far in Lichess' win chances (-1..1)
constexpr int kEqualCp = 30;         // as good as the solution (last move), or the coach's best defence
constexpr int kLostCp = 80;          // the coach's defence in a lost position: at most this far below
constexpr double kLostChances = 0.02;  // the best, and this close in win chances
constexpr int kHoldCp = -150;        // escape: the move that holds
constexpr int kEscapeLoseCp = -300;  // escape: every other move at most this
constexpr int kEscapeGapCp = 200;    // escape: and this far below the holder
constexpr int kHeavyLossCp = -500;   // escape: a move that clearly loses
constexpr int kEscapeMates = 3;      // escape: at least this many others lose to mate (king in danger)
constexpr int kPromoteCp = 500;      // promote: the start's score
constexpr int kDrawCp = 80;          // hold: the start's score within +-
constexpr int kLosesCp = 300;        // hold: a losing move gives Black this much

// Lichess' win chances (-1..1) of a score; mates at the ends.
double winChances(const Score& s) {
    if (s.matesNow || s.mate > 0) return 1.0;
    if (s.matedNow || s.mate < 0) return -1.0;
    return 2.0 / (1.0 + std::exp(-0.00368208 * double(std::clamp(s.cp, -4000, 4000)))) - 1.0;
}
bool mates(const Score& s) { return s.matesNow || s.mate > 0; }
bool mated(const Score& s) { return s.matedNow || s.mate < 0; }

// A move scoring 'a' is clearly worse than the solution scoring 's' (s winning).
bool clearlyWorse(const Score& s, const Score& a) {
    if (mates(a)) return false;
    if (mated(a)) return true;
    if (mates(s)) return winChances(s) - winChances(a) >= kWorseChances;
    return s.cp - a.cp >= kWorseCp && winChances(s) - winChances(a) >= kWorseChances;
}

// A move scoring 'a' is as good as the solution scoring 's' (the last move of a line).
bool asGood(const Score& s, const Score& a) {
    if (mates(s)) return mates(a);
    if (mates(a) || mated(a)) return false;
    return std::abs(s.cp - a.cp) <= kEqualCp && a.cp >= kWinCp;
}

// 'a' is better than 's' by more than the margin of asGood.
bool better(const Score& s, const Score& a) {
    if (mates(s)) return false;
    if (mates(a)) return true;
    return !mated(a) && a.cp > s.cp + kEqualCp;
}

// The coach's answer scoring 'b' is its best defence (the best scoring 'best'), both from the
// coach's view: the longest mate, or within kEqualCp of the best (kLostCp in a lost position, where
// a few centipawns more or less change nothing: the same win chances within kLostChances).
bool bestDefence(const Score& best, const Score& b) {
    if (mated(best)) return mated(b) && b.mate == best.mate;
    if (mated(b)) return false;
    if (mates(best)) return mates(b);
    if (mates(b)) return true;
    const int gap = best.cp - b.cp;
    return gap <= kEqualCp || (gap <= kLostCp && winChances(best) - winChances(b) <= kLostChances);
}

int legalCount(const Position& p) { return int(p.legalMoves().size()); }

// "code: detail" -> "code: move k, detail".
std::string atMove(const std::string& why, int k) {
    const size_t c = why.find(": ");
    if (c == std::string::npos) return why;
    return why.substr(0, c) + ": move " + std::to_string(k + 1) + ", " + why.substr(c + 2);
}

}  // namespace

// ==== The engine ==================================================================================
struct Oracle::Impl {
    ai::Engine engine;
};

Oracle::Oracle(const Options& o) : opt_(o), impl_(new Impl) {
    ok_ = impl_->engine.start() && impl_->engine.waitReady(60000);
    if (!ok_) return;
    ai::EngineSettings s;
    s.threads = std::max(1, o.threads);
    s.hashMB = std::max(16, o.hashMB);
    s.humanize = false;
    s.useClock = false;
    impl_->engine.configure(s);
}

Oracle::~Oracle() {
    impl_->engine.shutdown();
    delete impl_;
}

void Oracle::newCandidate() { impl_->engine.newGame(); }

void Oracle::startClock(int seconds) {
    timedOut_ = false;
    deadline_ = seconds > 0 ? std::chrono::steady_clock::now() + std::chrono::seconds(seconds)
                            : std::chrono::steady_clock::time_point::max();
}

// One search (ok false on failure, or once the candidate's time has run out).
ai::Analysis Oracle::run(const ai::AnalysisRequest& r) {
    ai::Analysis a;
    if (timedOut_) return a;
    ai::Engine& e = impl_->engine;
    const uint32_t id = e.requestAnalysis(r);
    const auto t0 = std::chrono::steady_clock::now();
    while (!e.analysisReady(id)) {
        const auto now = std::chrono::steady_clock::now();
        if (now > deadline_) timedOut_ = true;
        if (timedOut_ || now - t0 > std::chrono::minutes(20)) {
            e.cancelAnalysis(id);
            return a;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!e.takeAnalysis(id, a)) a.ok = false;
    return a;
}

ai::Analysis Oracle::top(const Position& pos, int lines, int depth) {
    ai::AnalysisRequest r;
    r.startFen = pos.fen();
    r.multiPV = std::max(1, std::min(lines, legalCount(pos)));
    r.depth = depth;
    r.moveTimeMs = 0;   // depth only: reproducible
    return run(r);
}

ai::Analysis Oracle::only(const Position& pos, const std::string& uci, int depth) {
    ai::AnalysisRequest r;
    r.startFen = pos.fen();
    r.multiPV = 1;
    r.depth = depth;
    r.moveTimeMs = 0;
    r.searchMoves = {uci};
    return run(r);
}

// ==== Lines =======================================================================================
namespace {

// The coach's answer 'uci' in 'q' (the coach to move) is its best defence.
std::string checkAnswer(Oracle& o, const Position& q, const std::string& uci, int depth) {
    const ai::Analysis best = o.top(q, 1, depth);
    if (!best.ok || best.lines.empty()) return "engine: no result";
    if (best.bestMove == uci) return std::string();
    const ai::Analysis b = o.only(q, uci, depth);
    if (!b.ok || b.lines.empty()) return "engine: no result";
    if (!bestDefence(best.lines[0].score, b.lines[0].score))
        return "coach answer: " + uci + " (" + b.lines[0].score.text() + ") is not the best defence (" +
               best.bestMove + " " + best.lines[0].score.text() + ")";
    return std::string();
}

// A mate line's player move k: line[2k] mates in exactly 'left' moves and no other move as fast.
std::string checkMateMove(Oracle& o, const Position& p, const std::string& uci, int left, int depth,
                          std::string& note) {
    const ai::Analysis a = o.top(p, 3, depth);
    if (!a.ok || a.lines.empty()) return "engine: no result";
    const ai::PvLine* s = a.line(uci);
    if (!s) return "not best: " + uci + " is not among the engine's best (" + a.bestMove + " " +
                   a.lines[0].score.text() + ")";
    if (s->score.mate != left)
        return "mate length: " + uci + " scores " + s->score.text() + ", not M" + std::to_string(left);
    for (const ai::PvLine& l : a.lines)
        if (!l.pv.empty() && l.pv[0] != uci && mates(l.score) && l.score.mate <= left)
            return "two solutions: " + l.pv[0] + " mates as fast (" + l.score.text() + ")";
    note = s->score.text();
    return std::string();
}

// A non-mate line's player move k (not the last): the solution is the best and the only win.
std::string checkOnlyMove(Oracle& o, const Position& p, const std::string& uci, int depth, std::string& note) {
    const ai::Analysis a = o.top(p, 2, depth);
    if (!a.ok || a.lines.empty()) return "engine: no result";
    if (a.bestMove != uci)
        return "not best: " + uci + " is not the engine's best (" + a.bestMove + " " + a.lines[0].score.text() + ")";
    const Score& s = a.lines[0].score;
    if (!mates(s) && s.cp < kWinCp) return "no win: " + uci + " " + s.text();
    if (a.lines.size() > 1 && !clearlyWorse(s, a.lines[1].score))
        return "two solutions: " + uci + " " + s.text() + ", " + a.lines[1].pv[0] + " " + a.lines[1].score.text();
    note = s.text() + (a.lines.size() > 1 ? " (next " + a.lines[1].score.text() + ")" : std::string(" (forced)"));
    return std::string();
}

// A non-mate line's last player move: the solution is among the best, the moves as good go to
// 'also', every other move is clearly worse.
std::string checkLastMove(Oracle& o, const Position& p, const std::string& uci, int depth,
                          std::vector<std::string>& also, std::string& note) {
    const int legal = legalCount(p);
    // Two lines settle most positions; more only while every line seen is as good.
    for (int want = std::min(2, legal);; want = want < 6 ? std::min(6, legal) : legal) {
        const ai::Analysis a = o.top(p, want, depth);
        if (!a.ok || a.lines.empty()) return "engine: no result";
        const ai::PvLine* sl = a.line(uci);
        if (!sl) return "not best: " + uci + " is not among the engine's best (" + a.bestMove + " " +
                        a.lines[0].score.text() + ")";
        const Score s = sl->score;
        if (!mates(s) && s.cp < kWinCp) return "no win: " + uci + " " + s.text();
        also.clear();
        bool settled = false;   // a clearly worse line was reached: every later one is worse still
        note = s.text() + " (forced)";
        for (const ai::PvLine& l : a.lines) {
            if (l.pv.empty() || l.pv[0] == uci) continue;
            if (better(s, l.score))
                return "not best: " + l.pv[0] + " " + l.score.text() + " is better than " + uci + " " + s.text();
            if (asGood(s, l.score)) {
                also.push_back(l.pv[0]);
                continue;
            }
            if (!clearlyWorse(s, l.score))
                return "unclear alternative: " + l.pv[0] + " " + l.score.text() + ", neither as good as nor clearly worse than " +
                       uci + " " + s.text();
            note = s.text() + " (next " + l.score.text() + ")";
            settled = true;
            break;
        }
        if (settled || int(a.lines.size()) >= legal || want == legal) return std::string();
    }
}

std::string checkLine(Oracle& o, coach::ChallengePosition& pos, std::string& note) {
    const int depth = o.depth();
    const int K = pos.playerMoves();
    const bool mateLine = pos.endsInMate();
    pos.also.clear();
    for (int k = 0; k < K; ++k) {
        Position p;
        if (!pos.beforeMove(k, p)) return "read: bad line";
        const std::string& uci = pos.line[size_t(2 * k)];
        const bool last = k == K - 1;
        std::string why, score = "#";
        std::vector<std::string> also;
        if (mateLine && last) {
            // Any checkmate is accepted (ChallengePosition::accepts); list the others.
            for (const Move& m : p.legalMoves()) {
                Position q = p;
                q.makeMove(m);
                if (q.isCheckmate() && p.toUCI(m) != uci) also.push_back(p.toUCI(m));
            }
        } else if (mateLine) {
            why = checkMateMove(o, p, uci, K - k, depth, score);
        } else if (!last) {
            why = checkOnlyMove(o, p, uci, depth, score);
        } else {
            why = checkLastMove(o, p, uci, depth, also, score);
        }
        if (!why.empty()) return atMove(why, k);
        note += (note.empty() ? "" : ", ") + uci + " " + score;
        if (!also.empty()) {
            pos.also.assign(size_t(K), {});
            pos.also[size_t(k)] = also;
        }
        if (!last) {
            Position q = p;
            q.makeMove(q.parseUCI(uci));
            why = checkAnswer(o, q, pos.line[size_t(2 * k + 1)], depth);
            if (!why.empty()) return atMove(why, k);
        }
    }
    return std::string();
}

// ==== Escapes =====================================================================================
// Escape: 's' holds too, next to the best move scoring 'h'.
bool holdsToo(const Score& h, const Score& s) {
    if (mates(s)) return true;
    if (mated(s)) return false;
    return s.cp > kEscapeLoseCp || (!mates(h) && h.cp - s.cp < kEscapeGapCp);
}

std::string checkEscape(Oracle& o, coach::ChallengePosition& pos, std::string& note) {
    Position p;
    if (!pos.start(p)) return "read: bad FEN";
    const int legal = legalCount(p);
    // Two lines first: most candidates fail there, before the search of every move.
    for (int want : {std::min(2, legal), legal}) {
        const ai::Analysis a = o.top(p, want, o.depth());
        if (!a.ok || a.lines.empty()) return "engine: no result";
        if (int(a.lines.size()) != want) return "engine: not every move scored";
        const ai::PvLine& h = a.lines[0];
        if (mated(h.score) || (!mates(h.score) && h.score.cp < kHoldCp))
            return "escape, none: no move holds (best " + h.pv[0] + " " + h.score.text() + ")";
        int losing = 0, mating = 0;
        for (size_t i = 1; i < a.lines.size(); ++i) {
            const Score& s = a.lines[i].score;
            if (holdsToo(h.score, s))
                return "escape, two: more than one move holds: " + h.pv[0] + " " + h.score.text() + ", " +
                       a.lines[i].pv[0] + " " + s.text();
            if (mated(s) || s.cp <= kHeavyLossCp) ++losing;
            if (mated(s)) ++mating;
        }
        if (want < legal) continue;
        if (losing < 2 || mating < kEscapeMates)
            return "escape, few losses: " + std::to_string(losing) + " lose clearly, " + std::to_string(mating) +
                   " to mate";
        note = h.pv[0] + " " + h.score.text() + ", " + std::to_string(losing) + " of " + std::to_string(legal - 1) +
               " others lose clearly (" + std::to_string(mating) + " to mate)";
        pos.lead.clear();
        pos.goal = coach::ChallengeGoal::Line;
        pos.line = {h.pv[0]};
        pos.also.clear();
        return std::string();
    }
    return "escape, few losses: a single legal move";
}

// ==== Play-outs ===================================================================================
std::string checkPlay(Oracle& o, const coach::ChallengePosition& pos, std::string& note) {
    Position p;
    if (!pos.start(p)) return "read: bad FEN";
    const int depth = o.playDepth();
    if (pos.goal == coach::ChallengeGoal::Mate) {
        const ai::Analysis a = o.top(p, 1, depth);
        if (!a.ok || a.lines.empty()) return "engine: no result";
        if (!mates(a.lines[0].score)) return "play, no mate: " + a.lines[0].score.text();
        note = a.lines[0].pv[0] + " " + a.lines[0].score.text();
        return std::string();
    }
    if (pos.goal == coach::ChallengeGoal::Promote) {
        const ai::Analysis a = o.top(p, 1, depth);
        if (!a.ok || a.lines.empty()) return "engine: no result";
        const Score& s = a.lines[0].score;
        if (!mates(s) && s.cp < kPromoteCp) return "play, no win: " + s.text();
        note = a.lines[0].pv[0] + " " + s.text();
        return std::string();
    }
    // Hold.
    const int legal = legalCount(p);
    const ai::Analysis a = o.top(p, legal, depth);
    if (!a.ok || a.lines.empty()) return "engine: no result";
    const Score& s = a.lines[0].score;
    if (s.isMate() || std::abs(s.cp) > kDrawCp) return "play, no draw: " + s.text();
    if (int(a.lines.size()) != legal) return "engine: not every move scored";
    const Score& worst = a.lines.back().score;
    if (!mated(worst) && worst.cp > -kLosesCp)
        return "play, nothing loses: (worst " + a.lines.back().pv[0] + " " + worst.text() + ")";
    int losing = 0;
    for (const ai::PvLine& l : a.lines) losing += mated(l.score) || l.score.cp <= -kLosesCp;
    note = a.lines[0].pv[0] + " " + s.text() + ", " + std::to_string(losing) + " of " + std::to_string(legal) +
           " moves lose (" + a.lines.back().pv[0] + " " + worst.text() + ")";
    return std::string();
}

}  // namespace

std::string checkCandidate(Oracle& o, const Set& set, const Candidate& c, coach::ChallengePosition& pos,
                           std::string& note) {
    note.clear();
    pos = c.pos;
    if (!c.error.empty()) return "read: " + c.error;
    if (c.escape) return checkEscape(o, pos, note);
    if (pos.playOut()) return checkPlay(o, pos, note);
    if (set.group == "mates" && !pos.endsInMate()) return "no mate: the line does not end in checkmate";
    return checkLine(o, pos, note);
}

}  // namespace challenges
