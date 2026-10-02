// The coach's end-of-game appraisal (see appraisal.h): lichess game accuracy, the statistics of
// research-pedagogy §5.2 and the appraisal script of §5.3-5.5.
#include "coach/appraisal.h"

#include "coach/review_internal.h"
#include "coach/tactics.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace coach {

using namespace chess;
using detail::bandKey;

// ---- Accuracy -------------------------------------------------------------------------------------

namespace {

double populationStdDev(const std::vector<double>& xs) {
    if (xs.empty()) return 0.0;
    double mean = 0.0;
    for (double x : xs) mean += x;
    mean /= double(xs.size());
    double var = 0.0;
    for (double x : xs) var += (x - mean) * (x - mean);
    return std::sqrt(var / double(xs.size()));
}

}  // namespace

SideAccuracy gameAccuracy(const std::vector<int>& cps, Color firstMover, int startCp) {
    SideAccuracy out;
    if (cps.empty()) return out;
    // lila AccuracyPercent.gameAccuracy: W% series with the start position first.
    std::vector<double> w;
    w.reserve(cps.size() + 1);
    w.push_back(winPercent(startCp));
    for (int cp : cps) w.push_back(winPercent(cp));
    const int n = int(w.size());
    const int window = std::clamp(int(cps.size()) / 10, 2, 8);
    // (window - 2) copies of the first window, then every sliding window (a list shorter than the
    // window slides once, as a whole).
    std::vector<std::vector<double>> windows;
    const std::vector<double> first(w.begin(), w.begin() + std::min(window, n));
    for (int i = 0; i < std::min(window, n) - 2; ++i) windows.push_back(first);
    if (n <= window) windows.push_back(w);
    else
        for (int i = 0; i + window <= n; ++i) windows.emplace_back(w.begin() + i, w.begin() + i + window);
    std::vector<double> weights;
    for (const auto& xs : windows) weights.push_back(std::clamp(populationStdDev(xs), 0.5, 12.0));

    double wSum[2] = {0, 0}, wAcc[2] = {0, 0}, inv[2] = {0, 0};
    int count[2] = {0, 0};
    const size_t moves = std::min(size_t(n - 1), weights.size());
    for (size_t i = 0; i < moves; ++i) {
        const Color c = i % 2 == 0 ? firstMover : opposite(firstMover);
        const double before = c == White ? w[i] : 100.0 - w[i];
        const double after = c == White ? w[i + 1] : 100.0 - w[i + 1];
        const double acc = moveAccuracy(before, after);
        const int s = c == White ? 0 : 1;
        wAcc[s] += acc * weights[i];
        wSum[s] += weights[i];
        inv[s] += 1.0 / std::max(1.0, acc);
        ++count[s];
    }
    for (int s = 0; s < 2; ++s) {
        if (count[s] == 0) continue;
        const double weighted = wAcc[s] / wSum[s];
        const double harmonic = double(count[s]) / inv[s];
        (s == 0 ? out.white : out.black) = (weighted + harmonic) / 2.0;
    }
    return out;
}

double typicalAccuracy(int rating) {
    static const int kR[5] = {600, 1000, 1500, 2000, 2500};
    static const double kA[5] = {65, 71, 79, 86, 91};
    if (rating <= kR[0]) return kA[0];
    for (int i = 1; i < 5; ++i)
        if (rating <= kR[i]) return kA[i - 1] + (kA[i] - kA[i - 1]) * double(rating - kR[i - 1]) / double(kR[i] - kR[i - 1]);
    return kA[4];
}

int levelRating(int level) { return 750 + 300 * (std::clamp(level, 1, 6) - 1); }

int suggestLevel(int level, const std::vector<GameRecord>& games) {
    level = std::clamp(level, 1, 6);
    std::vector<GameRecord> here;   // the games at this level, oldest first
    for (const GameRecord& g : games)
        if (g.level == level) here.push_back(g);
    if (here.empty()) return level;
    const GameRecord& last = here.back();
    if (level < 6 && here.size() >= 2) {
        const size_t from = here.size() >= 3 ? here.size() - 3 : 0;
        int wins = 0;
        bool accurate = true;
        for (size_t i = from; i < here.size(); ++i)
            if (here[i].result > 0) {
                ++wins;
                if (here[i].accuracy < typicalAccuracy(levelRating(level + 1))) accurate = false;
            }
        if (wins >= 2 && accurate && last.result > 0) return level + 1;
    }
    if (level > 1 && here.size() >= 3 && last.result < 0) {
        bool down = true;
        for (size_t i = here.size() - 3; i < here.size(); ++i)
            if (here[i].result >= 0 || here[i].accuracy < 0 || here[i].accuracy >= typicalAccuracy(levelRating(level)))
                down = false;
        if (down) return level - 1;
    }
    return level;
}

// ---- Collection -------------------------------------------------------------------------------------

void Appraisal::reset(int level, Color human) {
    *this = Appraisal();
    level_ = std::clamp(level, 1, 6);
    human_ = human;
}

void Appraisal::setEval(size_t plies, int cpWhite, uint8_t source) {
    if (eval_.size() <= plies) {
        eval_.resize(plies + 1, 0);
        evalKnown_.resize(plies + 1, 0);
    }
    if (evalKnown_[plies] > source) return;
    eval_[plies] = std::clamp(cpWhite, -1000, 1000);
    evalKnown_[plies] = source;
}

void Appraisal::add(const Review& r) {
    const PlyVerdict& v = r.verdict;
    if (v.ply < 0) return;
    if (r.isRetry) {
        ++takebacks_;
        if (r.takeback.fixed) ++fixed_;
    }
    if (v.offered) ++offers_;
    if (humanVerdicts_.size() <= size_t(v.ply)) humanVerdicts_.resize(size_t(v.ply) + 1);
    humanVerdicts_[size_t(v.ply)] = v;
    // The review's own figures (same root, same depth) come before any background evaluation.
    if (v.hasEvalBefore) setEval(size_t(v.ply), v.cpWhiteBefore, 2);
    if (v.hasEvalAfter) setEval(size_t(v.ply) + 1, v.cpWhiteAfter, 2);
}

void Appraisal::addEval(size_t plies, const ai::Analysis& a) {
    // A position without a legal move has no line: the final position is scored from the result.
    if (!a.ok || a.lines.empty()) return;
    setEval(plies, detail::whiteCp(a.lines[0].score, a.whiteToMove), 1);
}

void Appraisal::truncate(size_t plies) {
    if (humanVerdicts_.size() > plies) humanVerdicts_.resize(plies);
    if (eval_.size() > plies + 1) {
        eval_.resize(plies + 1);
        evalKnown_.resize(plies + 1);
    }
}

std::vector<size_t> Appraisal::missingEvals(const Game& g) const {
    std::vector<size_t> out;
    for (size_t k = 1; k <= g.moves().size(); ++k) {
        if (k < evalKnown_.size() && evalKnown_[k]) continue;
        if (k == g.moves().size() && !g.position().hasLegalMove()) continue;   // mate / stalemate: known
        out.push_back(k);
    }
    return out;
}

ai::AnalysisRequest Appraisal::evalRequest(const Game& g, size_t plies) const {
    ai::AnalysisRequest r = detail::requestAt(g, plies);
    r.multiPV = 1;
    r.depth = 14;
    r.moveTimeMs = 400;
    r.priority = -1;
    return r;
}

// ---- Verdicts and statistics ------------------------------------------------------------------------

namespace {

// Evaluation after each position index 0..N (White's view), the final position from the result, the
// gaps filled with the previous value. 'known' tells which were measured.
std::vector<int> evalSeries(const Game& g, const std::vector<int>& eval, const std::vector<uint8_t>& known,
                            std::vector<bool>* measured) {
    const size_t n = g.moves().size();
    std::vector<int> out(n + 1, 0);
    std::vector<bool> m(n + 1, false);
    for (size_t k = 0; k <= n; ++k)
        if (k < known.size() && known[k]) {
            out[k] = eval[k];
            m[k] = true;
        }
    if (g.position().isCheckmate()) {
        out[n] = g.position().sideToMove() == White ? -1000 : 1000;
        m[n] = true;
    } else if (g.status() == GameStatus::Draw && !g.position().hasLegalMove()) {
        out[n] = 0;
        m[n] = true;
    } else if (g.status() == GameStatus::Draw && !m[n]) {
        out[n] = 0;
        m[n] = true;
    }
    if (!m[0]) out[0] = detail::startFenOf(g).empty() ? 15 : 0;
    for (size_t k = 1; k <= n; ++k)
        if (!m[k]) out[k] = out[k - 1];
    if (measured) *measured = m;
    return out;
}

int moveNumberOf(const Game& g, int ply) { return g.positionAt(size_t(ply)).fullmoveNumber(); }

Square captureSquare(const Position& before, const Move& m) {
    if (m.flags & MoveEnPassant) return Square(m.to + (before.sideToMove() == White ? -8 : 8));
    return m.to;
}

}  // namespace

std::vector<PlyVerdict> Appraisal::verdicts(const Game& g) const {
    const size_t n = g.moves().size();
    std::vector<bool> measured;
    const std::vector<int> ev = evalSeries(g, eval_, evalKnown_, &measured);
    const Phases ph = dividePhases(g);
    std::vector<PlyVerdict> out(n);
    for (size_t k = 0; k < n; ++k) {
        const Position& p0 = g.positionAt(k);
        PlyVerdict& v = out[k];
        if (k < humanVerdicts_.size() && humanVerdicts_[k].ply == int(k)) {
            v = humanVerdicts_[k];
            continue;
        }
        v.ply = int(k);
        v.mover = p0.sideToMove();
        v.human = v.mover == human_;
        v.uci = p0.toUCI(g.moves()[k]);
        v.san = g.sanMoves()[k];
        v.phase = uint8_t(phaseOfPly(ph, int(k)));
        v.check = g.positionAt(k + 1).inCheck();
        const Piece cap = p0.at(captureSquare(p0, g.moves()[k]));
        v.captured = cap.empty() || cap.color == v.mover ? 0 : kPiecePoints[cap.type];
        if (v.human) continue;   // a human move without a review stays Unjudged
        // The coach's move, from the neighbouring evaluations (no extra search).
        if (!measured[k] || !measured[k + 1]) continue;
        const int sign = v.mover == White ? 1 : -1;
        v.wBest = winPercent(sign * ev[k]);
        v.wPlayed = winPercent(sign * ev[k + 1]);
        v.delta = std::max(0.0, v.wBest - v.wPlayed);
        v.accuracy = moveAccuracy(v.wBest, v.wPlayed);
        v.cls = p0.legalMoves().size() == 1 ? MoveClass::Forced : classifyDelta(v.delta, false);
        v.cpWhiteBefore = ev[k];
        v.hasEvalBefore = true;
        v.cpWhiteAfter = ev[k + 1];
        v.hasEvalAfter = true;
    }
    return out;
}

AppraisalStats Appraisal::stats(const Game& g) const {
    AppraisalStats st;
    const size_t n = g.moves().size();
    st.plies = int(n);
    st.fullMoves = int((n + 1) / 2);
    st.status = g.status();
    st.reason = g.endReason();
    if (g.status() == GameStatus::WhiteWins) st.result = human_ == White ? 1 : -1;
    else if (g.status() == GameStatus::BlackWins) st.result = human_ == Black ? 1 : -1;
    const std::vector<PlyVerdict> vs = verdicts(g);

    // Per side: classes, top moves, captures, checks.
    for (const PlyVerdict& v : vs) {
        SideStats& s = v.human ? st.human : st.coach;
        s.classes[size_t(v.cls)]++;
        if (v.cls == MoveClass::Unjudged) ++s.unjudged;
        else ++s.judged;
        if (v.cls == MoveClass::Best || v.cls == MoveClass::Excellent) ++s.topMoves;
        s.captured += v.captured;
        if (v.captured > 0) ++s.piecesCaptured;
        if (v.check) ++s.checks;
        if (v.human) ++st.humanMoves;
    }

    // Accuracy from the evaluation series.
    std::vector<bool> measured;
    const std::vector<int> ev = evalSeries(g, eval_, evalKnown_, &measured);
    if (n > 0) {
        const std::vector<int> cps(ev.begin() + 1, ev.end());
        const Color first = g.startPosition().sideToMove();
        const SideAccuracy acc = gameAccuracy(cps, first, ev[0]);
        st.human.accuracy = acc.of(human_);
        st.coach.accuracy = acc.of(opposite(human_));
        // Phase accuracy: the same function on each Divider slice with >= 6 plies of the side.
        const Phases ph = dividePhases(g);
        const int bounds[4] = {0, ph.middlegame >= 0 ? ph.middlegame : int(n),
                               ph.endgame >= 0 ? ph.endgame : int(n), int(n)};
        for (int phase = 0; phase < 3; ++phase) {
            const int from = std::min(bounds[phase], int(n));
            const int to = std::max(from, std::min(bounds[phase + 1], int(n)));
            if (phase == 1 && ph.middlegame < 0) continue;
            if (phase == 2 && ph.endgame < 0) continue;
            if (to - from < 2) continue;
            const std::vector<int> slice(ev.begin() + from + 1, ev.begin() + to + 1);
            const Color sliceFirst = g.positionAt(size_t(from)).sideToMove();
            const SideAccuracy pa = gameAccuracy(slice, sliceFirst, ev[size_t(from)]);
            int humanPlies = 0, coachPlies = 0;
            for (int k = from; k < to; ++k) (vs[size_t(k)].human ? humanPlies : coachPlies)++;
            if (humanPlies >= 6) st.human.phaseAccuracy[size_t(phase)] = pa.of(human_);
            if (coachPlies >= 6) st.coach.phaseAccuracy[size_t(phase)] = pa.of(opposite(human_));
        }
        // The human's W% at the last measured position (for a resignation).
        for (int k = int(n); k >= 0; --k)
            if (measured[size_t(k)]) {
                st.humanWinAtEnd = winPercent(human_ == White ? ev[size_t(k)] : -ev[size_t(k)]);
                break;
            }
    }
    const int unjudged = st.human.unjudged;
    st.numbers = st.humanMoves >= 10 && double(unjudged) <= 0.2 * double(st.humanMoves) && st.human.accuracy >= 0.0;

    // The human's moves: specials, streak, critical moment, turning point, theme.
    int streak = 0;
    double critDelta = 10.0;
    double prevPlayed = -1.0;
    std::map<ExType, int> faults;
    std::vector<ExType> faultOrder;
    for (const PlyVerdict& v : vs) {
        if (!v.human) continue;
        if (v.only) ++st.onlyMoves;
        if (v.brilliant) ++st.brilliant;
        if (v.great) ++st.great;
        if (v.cls == MoveClass::Best || v.cls == MoveClass::Excellent) {
            ++streak;
            if (streak > st.bestStreak) st.bestStreak = streak;
        } else if (v.cls != MoveClass::Book && v.cls != MoveClass::Forced && v.cls != MoveClass::Unjudged) {
            streak = 0;
        }
        const bool judgedMove = v.cls != MoveClass::Unjudged && v.cls != MoveClass::Book && v.cls != MoveClass::Forced;
        if (judgedMove && v.delta >= critDelta && (st.criticalPly < 0 || v.delta > critDelta)) {
            st.criticalPly = v.ply;
            critDelta = v.delta;
        }
        if (judgedMove && st.turningPly < 0) {
            if (v.wBest >= 50.0 && v.wPlayed < 40.0) st.turningPly = v.ply;
            else if (prevPlayed >= 0.0 && prevPlayed <= 50.0 && v.wPlayed > 60.0) st.turningPly = v.ply;
        }
        if (v.cls != MoveClass::Unjudged) prevPlayed = v.wPlayed;
        if ((v.cls == MoveClass::Mistake || v.cls == MoveClass::Blunder) && v.exType != ExType::None) {
            if (!faults.count(v.exType)) faultOrder.push_back(v.exType);
            faults[v.exType]++;
        }
    }
    int themeCount = 1;
    for (ExType t : faultOrder)
        if (faults[t] > themeCount) {
            themeCount = faults[t];
            st.theme = t;
            st.themeRecurring = true;
        }
    if (!st.themeRecurring && st.criticalPly >= 0) st.theme = vs[size_t(st.criticalPly)].exType;

    // Best moment: brilliant > great > only move > best move winning 3+ points > streak >= 5 >
    // good capture > the highest-accuracy phase.
    auto firstHuman = [&](auto pred) {
        for (const PlyVerdict& v : vs)
            if (v.human && pred(v)) return v.ply;
        return -1;
    };
    int ply;
    if ((ply = firstHuman([](const PlyVerdict& v) { return v.brilliant; })) >= 0) {
        st.bestMoment = BestMoment::Brilliant;
        st.bestPly = ply;
    } else if ((ply = firstHuman([](const PlyVerdict& v) { return v.great; })) >= 0) {
        st.bestMoment = BestMoment::Great;
        st.bestPly = ply;
    } else if ((ply = firstHuman([](const PlyVerdict& v) { return v.only; })) >= 0) {
        st.bestMoment = BestMoment::OnlyMove;
        st.bestPly = ply;
    } else if ((ply = firstHuman([](const PlyVerdict& v) {
                    return v.cls == MoveClass::Best && v.materialSwing >= 3;
                })) >= 0) {
        st.bestMoment = BestMoment::WonMaterial;
        st.bestPly = ply;
    } else if (st.bestStreak >= 5) {
        st.bestMoment = BestMoment::Streak;
    } else if ((ply = firstHuman([](const PlyVerdict& v) {
                    return v.goodCapture && (v.cls == MoveClass::Best || v.cls == MoveClass::Excellent);
                })) >= 0) {
        st.bestMoment = BestMoment::GoodCapture;
        st.bestPly = ply;
    } else if (st.numbers) {
        double best = -1.0;
        for (int p = 0; p < 3; ++p)
            if (st.human.phaseAccuracy[size_t(p)] > best) {
                best = st.human.phaseAccuracy[size_t(p)];
                st.bestPhase = p;
            }
        if (st.bestPhase >= 0) st.bestMoment = BestMoment::Phase;
    }

    // Takebacks.
    st.offers = offers_;
    st.takebacks = takebacks_;
    st.fixed = fixed_;

    // A coach move that hung a piece the human did not take (the review explained it as a missed capture).
    for (const PlyVerdict& v : vs) {
        if (!v.human || v.exType != ExType::MissedCapture || v.ply < 1 || v.bestUci.empty()) continue;
        const Position& p0 = g.positionAt(size_t(v.ply));
        const Move bm = p0.parseUCI(v.bestUci);
        if (!bm.valid()) continue;
        const Piece x = p0.at(bm.to);
        if (x.empty() || x.color == human_ || kPiecePoints[x.type] < 3) continue;
        st.coachHungPly = v.ply - 1;
        st.coachHungType = x.type;
        break;
    }
    return st;
}

// ---- The script ---------------------------------------------------------------------------------------

namespace {

struct Part {
    Beat beat;
    int keep = 0;   // higher = dropped last when the level's sentence cap is exceeded
};

int sentenceCap(int level) { return level <= 2 ? 5 : level <= 4 ? 6 : 4; }

Beat say(const std::string& key, Look look) {
    Beat b;
    b.kind = BeatKind::Say;
    b.line.key = key;
    b.look = look;
    b.skippable = true;
    return b;
}

Gesture simple(GestureKind k) {
    Gesture g;
    g.kind = k;
    return g;
}

const char* reasonKey(GameEndReason r) {
    switch (r) {
    case GameEndReason::Stalemate: return "appraisal.reason.stalemate";
    case GameEndReason::InsufficientMaterial:
    case GameEndReason::TimeoutVsInsufficient:
    case GameEndReason::IllegalMovesVsInsufficient: return "appraisal.reason.material";
    case GameEndReason::FivefoldRepetition:
    case GameEndReason::ThreefoldClaim: return "appraisal.reason.repetition";
    case GameEndReason::SeventyFiveMoves:
    case GameEndReason::FiftyMoveClaim: return "appraisal.reason.fifty";
    case GameEndReason::Agreement: return "appraisal.reason.agreement";
    default: return "appraisal.reason.other";
    }
}

const char* phaseKey(int phase) {
    return phase == 0 ? "appraisal.phase.opening" : phase == 1 ? "appraisal.phase.middlegame" : "appraisal.phase.endgame";
}

std::string themeKey(ExType t) { return t == ExType::None ? "theme.general" : std::string("theme.") + exTypeName(t); }

int rounded(double x) { return int(std::lround(x)); }

}  // namespace

Script Appraisal::script(const Game& g, const AppraisalContext& ctx) const {
    Script out;
    if (!g.isOver() || g.moves().empty()) return out;   // abandoned: no appraisal
    const AppraisalStats st = stats(g);
    const std::vector<PlyVerdict> vs = verdicts(g);
    const int L = level_;
    const bool engine = st.humanMoves > 0 && double(st.human.unjudged) <= 0.2 * double(st.humanMoves);
    const bool shortGame = st.humanMoves < 10;
    std::vector<Part> parts;

    // 1. Opener: result-aware, always positive.
    {
        std::string family;
        if (st.result > 0) family = "appraisal.open.win";
        else if (st.result == 0) family = st.reason == GameEndReason::Stalemate ? "appraisal.open.stalemate" : "appraisal.open.draw";
        else if (ctx.humanResigned) family = "appraisal.open.resigned";
        else if ((L == 3 || L == 4) && st.turningPly >= 0 && moveNumberOf(g, st.turningPly) >= 15)
            family = "appraisal.open.loss_close";
        else family = "appraisal.open.loss";
        Beat b = say(bandKey(family, L), Look::Player);
        b.line.with("moves", Arg::ofNumber(st.fullMoves)).with("text", Arg::ofText(reasonKey(st.reason)));
        if (st.result > 0) b.gestures.push_back(simple(GestureKind::Nod));
        parts.push_back({b, 10});
    }
    // A resignation that came early (levels 3+).
    if (ctx.humanResigned && st.result < 0 && L >= 3 && st.humanWinAtEnd >= 40.0) {
        Beat b = say(bandKey("appraisal.resign_early", L), Look::Player);
        const double w = std::clamp(st.humanWinAtEnd, 0.1, 99.9);
        // Back from W% to centipawns (the inverse of the lichess curve), the listener's view.
        const int cp = int(std::lround(std::log(100.0 / w - 1.0) / -0.00368208));
        b.line.with("eval", Arg::ofEval(cp, 0));
        parts.push_back({b, 3});
    }
    // The opening (levels 3+, W10).
    if (!ctx.opening.empty() && L >= 3) {
        Beat b = say(bandKey("appraisal.opening", L), Look::Player);
        b.line.with("opening", Arg::ofOpening(ctx.opening));
        parts.push_back({b, 1});
    }

    // 2. Highlight: the best moment, pointing at the piece when it is still on its square.
    if (engine && st.bestMoment != BestMoment::None) {
        std::string family;
        switch (st.bestMoment) {
        case BestMoment::Brilliant: family = "appraisal.best.brilliant"; break;
        case BestMoment::Great: family = "appraisal.best.great"; break;
        case BestMoment::OnlyMove: family = "appraisal.best.only"; break;
        case BestMoment::WonMaterial: family = "appraisal.best.won"; break;
        case BestMoment::Streak: family = "appraisal.best.streak"; break;
        case BestMoment::GoodCapture: family = "appraisal.best.capture"; break;
        case BestMoment::Phase: family = "appraisal.best.phase"; break;
        case BestMoment::None: break;
        }
        Beat b = say(bandKey(family, L), Look::Board);
        if (st.bestPly >= 0) {
            const PlyVerdict& v = vs[size_t(st.bestPly)];
            const Move m = g.moves()[size_t(st.bestPly)];
            const Position& p0 = g.positionAt(size_t(st.bestPly));
            b.line.with("move", Arg::ofMove(v.san, v.uci)).with("n", Arg::ofNumber(moveNumberOf(g, st.bestPly)));
            b.line.with("pts", Arg::ofNumber(std::max(1, v.materialSwing)));
            const Piece cap = p0.at(captureSquare(p0, m));
            if (!cap.empty() && cap.color != human_) b.line.with("my", Arg::ofPiece(cap.type, cap.color, false));
            else b.line.with("my", Arg::ofPiece(Pawn, opposite(human_), false));
            const Piece moved = p0.at(m.from);
            const Piece now = g.position().at(m.to);
            if (now.color == human_ && (now.type == moved.type || m.promotion != NoPiece)) {
                detail::pointPiece(b, m.to, "move");
                b.look = Look::Target;
            }
        } else if (st.bestMoment == BestMoment::Streak) {
            b.line.with("n", Arg::ofNumber(st.bestStreak));
        } else {
            b.line.with("text", Arg::ofText(phaseKey(st.bestPhase)))
                .with("acc", Arg::ofNumber(rounded(st.human.phaseAccuracy[size_t(st.bestPhase)])));
        }
        parts.push_back({b, 5});
    }

    // 3. Numbers (at most one sentence of numbers at levels 1-3).
    const int mistakes = st.human.count(MoveClass::Mistake), blunders = st.human.count(MoveClass::Blunder);
    const int inaccuracies = st.human.count(MoveClass::Inaccuracy);
    if (!engine || (shortGame && L >= 3)) {
        if (!engine) {
            Beat b = say(bandKey(st.human.checks > 0 ? "appraisal.num.material" : "appraisal.num.material_nochecks", L),
                         Look::Player);
            b.line.with("n", Arg::ofNumber(st.human.piecesCaptured)).with("m", Arg::ofNumber(st.human.checks));
            parts.push_back({b, 4});
        }
    } else if (L <= 3 && st.takebacks > 0 && st.fixed > 0) {
        Beat b = say(bandKey("appraisal.num.takebacks", L), Look::Player);
        b.line.with("n", Arg::ofNumber(st.takebacks)).with("m", Arg::ofNumber(st.fixed));
        parts.push_back({b, 4});
    } else if (L == 1) {
        if (st.human.piecesCaptured > 0) {
            Beat b = say(bandKey(st.human.checks > 0 ? "appraisal.num.material" : "appraisal.num.material_nochecks", L),
                         Look::Player);
            b.line.with("n", Arg::ofNumber(st.human.piecesCaptured)).with("m", Arg::ofNumber(st.human.checks));
            parts.push_back({b, 4});
        }
    } else if (L == 2) {
        Beat b;
        if (mistakes + blunders == 0) b = say("appraisal.num.clean.b2", Look::Player);
        else if (mistakes + blunders <= 2) b = say("appraisal.num.few.b2", Look::Player);
        else b = say("appraisal.num.best.b2", Look::Player);
        b.line.with("n", Arg::ofNumber(mistakes + blunders <= 2 ? mistakes + blunders : st.human.topMoves));
        parts.push_back({b, 4});
    } else if (st.numbers) {
        const int acc = rounded(st.human.accuracy);
        if (L == 3) {
            Beat b = say("appraisal.num.acc.b3", Look::Player);
            b.line.with("acc", Arg::ofNumber(acc)).with("n", Arg::ofNumber(mistakes)).with("m", Arg::ofNumber(blunders));
            parts.push_back({b, 4});
            if (ctx.explainAccuracy) parts.push_back({say("appraisal.num.acc_explain.b3", Look::Player), 2});
        } else if (L == 4) {
            int phase = -1;
            double best = -1.0;
            for (int p = 0; p < 3; ++p)
                if (st.human.phaseAccuracy[size_t(p)] > best) {
                    best = st.human.phaseAccuracy[size_t(p)];
                    phase = p;
                }
            Beat b = say(phase >= 0 ? "appraisal.num.acc_phase.b4" : "appraisal.num.acc.b4", Look::Player);
            b.line.with("acc", Arg::ofNumber(acc));
            if (phase >= 0)
                b.line.with("acc_phase", Arg::ofNumber(rounded(best))).with("text", Arg::ofText(phaseKey(phase)));
            parts.push_back({b, 4});
            if (st.coach.accuracy >= 0.0) {
                Beat c = say("appraisal.num.coach.b4", Look::Player);
                c.line.with("acc_coach", Arg::ofNumber(rounded(st.coach.accuracy)));
                parts.push_back({c, 1});
            }
        } else if (L == 5) {
            Beat b = say("appraisal.num.acc.b5", Look::Player);
            b.line.with("acc", Arg::ofNumber(acc)).with("n", Arg::ofNumber(inaccuracies)).with("m", Arg::ofNumber(mistakes));
            parts.push_back({b, 4});
            if (ctx.outOfBookMove > 0) {
                Beat k = say("appraisal.num.book.b5", Look::Player);
                k.line.with("k", Arg::ofNumber(ctx.outOfBookMove));
                parts.push_back({k, 1});
            }
        } else {
            const bool coach = st.coach.accuracy >= 0.0;
            Beat b = say(coach ? "appraisal.num.acc.b6" : "appraisal.num.acc_solo.b6", Look::Player);
            b.line.with("acc", Arg::ofNumber(acc)).with("n", Arg::ofNumber(inaccuracies)).with("m", Arg::ofNumber(mistakes))
                .with("k", Arg::ofNumber(blunders));
            if (coach) b.line.with("acc_coach", Arg::ofNumber(rounded(st.coach.accuracy)));
            parts.push_back({b, 4});
        }
    }

    // 4. One improvement: the critical moment, else the recurring theme, else "keep doing".
    if (engine) {
        const PlyVerdict* last = nullptr;
        for (const PlyVerdict& v : vs)
            if (v.human) last = &v;
        const bool stalemated = st.reason == GameEndReason::Stalemate && last && last->exType == ExType::Stalemate;
        const PlyVerdict* crit = st.criticalPly >= 0 ? &vs[size_t(st.criticalPly)] : nullptr;
        if (crit && L == 1 && crit->cls != MoveClass::Blunder) crit = nullptr;   // level 1: blunders only
        Beat b;
        if (stalemated) {
            b = say(bandKey("appraisal.improve.stalemate", L), Look::Player);
        } else if (crit) {
            const bool wasWinning = crit->wBest >= 70.0;
            b = say(bandKey(wasWinning ? "appraisal.improve.was_winning"
                                  : (L == 1 ? "appraisal.improve.theme" : "appraisal.improve.critical"),
                       L),
                    Look::Player);
            const int sign = human_ == White ? 1 : -1;
            b.line.with("crit_no", Arg::ofNumber(moveNumberOf(g, crit->ply)))
                .with("crit_move", Arg::ofMove(crit->san, crit->uci))
                .with("crit_best", Arg::ofMove(crit->bestSan, crit->bestUci))
                .with("theme", Arg::ofText(themeKey(crit->exType)))
                .with("eval", Arg::ofEval(sign * crit->cpWhiteBefore, 0));
        } else if (st.themeRecurring) {
            b = say(bandKey("appraisal.improve.theme", L), Look::Player);
            b.line.with("theme", Arg::ofText(themeKey(st.theme)));
        } else if (st.coachHungPly >= 0 && L <= 3) {
            b = say(bandKey("appraisal.improve.coach_hung", L), Look::Player);
            b.line.with("my", Arg::ofPiece(st.coachHungType, opposite(human_), false))
                .with("n", Arg::ofNumber(moveNumberOf(g, st.coachHungPly)));
        } else {
            b = say(bandKey("appraisal.improve.clean", L), Look::Player);
        }
        parts.push_back({b, 8});
        // Level 4: the recurring theme too, when the critical moment was about something else.
        if (crit && !stalemated && L == 4 && st.themeRecurring && st.theme != crit->exType) {
            Beat t = say("appraisal.improve.theme.b4", Look::Player);
            t.line.with("theme", Arg::ofText(themeKey(st.theme)));
            parts.push_back({t, 2});
        }
    }

    // 5. Encouragement and the level suggestion (never "down" after a win).
    {
        const int sug = ctx.suggestedLevel;
        std::string family = "appraisal.end";
        if (sug > L) family = "appraisal.end.up";
        else if (sug > 0 && sug < L && st.result <= 0) family = "appraisal.end.down";
        Beat b = say(bandKey(family, L), Look::Player);
        if (family != "appraisal.end") {
            b.line.with("level", Arg::ofText("appraisal.level.l" + std::to_string(std::clamp(sug, 1, 6))));
            b.gestures.push_back(simple(GestureKind::Open));
        }
        parts.push_back({b, 9});
    }

    // Sentence cap per level: drop the least important parts first, keep the order.
    while (int(parts.size()) > sentenceCap(L)) {
        auto drop = std::min_element(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.keep < b.keep; });
        parts.erase(drop);
    }
    for (const Part& p : parts) out.push_back(p.beat);
    return out;
}

}  // namespace coach
