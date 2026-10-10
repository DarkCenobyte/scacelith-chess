// The game review (see review.h): the positions of a game and their evaluations, the order of the
// searches (quick pass, deep pass, the board's surroundings first), the verdicts and their symbols,
// the evaluation bar, each side's summary with lichess's game accuracy, and the key a review is
// cached under.
#include "analysis/review.h"

#include "analysis/review_facts.h"
#include "coach/appraisal.h"
#include "coach/openings.h"
#include "coach/review_internal.h"
#include "coach/tactics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace analysis {

using namespace chess;

// ---- Symbols and colours ------------------------------------------------------------------------

const char* nagSymbol(Nag n) {
    switch (n) {
    case Nag::None: return "";
    case Nag::Good: return "!";
    case Nag::Mistake: return "?";
    case Nag::Brilliant: return "!!";
    case Nag::Blunder: return "??";
    case Nag::Interesting: return "!?";
    case Nag::Dubious: return "?!";
    }
    return "";
}

namespace {

// An sRGB colour (0xRRGGBB) in linear RGB: the 3D marks blend in linear space.
Rgb linearOf(uint32_t srgb) {
    auto channel = [](uint32_t v) {
        const double c = double(v & 0xFF) / 255.0;
        return float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
    };
    return Rgb{channel(srgb >> 16), channel(srgb >> 8), channel(srgb)};
}

}  // namespace

// The symbols' sRGB colours, shared with the 2D overlay: ?? #CA3431, ? #E58F2A, ?! #F7C045,
// !? #B57FD6, ! #5C8BB0, !! #1BACA6, none #9A9A9A; the better move #81B64C.
Rgb nagColor(Nag n) {
    switch (n) {
    case Nag::Blunder: return linearOf(0xCA3431);
    case Nag::Mistake: return linearOf(0xE58F2A);
    case Nag::Dubious: return linearOf(0xF7C045);
    case Nag::Interesting: return linearOf(0xB57FD6);
    case Nag::Good: return linearOf(0x5C8BB0);
    case Nag::Brilliant: return linearOf(0x1BACA6);
    case Nag::None: break;
    }
    return linearOf(0x9A9A9A);
}

Rgb betterMoveColor() { return linearOf(0x81B64C); }

// ---- Board facts (review_facts.h) ------------------------------------------------------------------

namespace detail {

Given materialGiven(const Position& before, const Move& m) {
    Given g;
    const Move legal = before.findLegal(m.from, m.to, m.promotion);
    if (!legal.valid()) return g;
    const Color us = before.sideToMove(), them = opposite(us);
    Position after = before;
    after.makeMove(legal);
    const int base = coach::materialBalance(before, us);
    const int now = coach::materialBalance(after, us);
    // What the opponent could already win before the move (it was not to move: every exchange it
    // could start on the board, statically).
    const int threat = coach::bestCapturePoints(before, them);
    for (const Move& c : after.legalMoves()) {
        if (!(c.flags & MoveCapture) || (c.promotion != NoPiece && c.promotion != Queen)) continue;
        const int lost = base - (now - coach::seePoints(after, c));
        if (lost < 2 || lost < threat + 2) continue;
        const Square sq = (c.flags & MoveEnPassant) ? Square(c.to + (them == White ? -8 : 8)) : c.to;
        const PieceType t = after.at(sq).type;
        if (!g.any() || lost > g.points || (lost == g.points && coach::kPiecePoints[t] > coach::kPiecePoints[g.piece])) {
            g.square = sq;
            g.piece = t;
            g.points = lost;
        }
    }
    return g;
}

int sensibleMoves(const Position& p) {
    int n = 0;
    for (const Move& m : p.legalMoves())
        if (coach::seePoints(p, m) >= 0) ++n;
    return n;
}

}  // namespace detail

// ---- The review --------------------------------------------------------------------------------------

namespace {

constexpr int kAround = 3;     // positions on each side of the board's one served first
constexpr size_t kMaxPv = 24;  // plies of a principal variation kept (comments, cache)

bool analysed(const PositionEval& e) { return !e.failed && (e.terminal || e.depth > 0); }

bool mates(const ai::Score& s) { return s.mate > 0 || s.matesNow; }
bool mated(const ai::Score& s) { return s.mate < 0 || s.matedNow; }

// The legal prefix of a line of UCI moves from p, at most kMaxPv plies, in the engine's spelling.
std::vector<std::string> legalPrefix(const Position& p, const std::vector<std::string>& uci) {
    std::vector<std::string> out;
    Position q = p;
    for (const std::string& u : uci) {
        if (out.size() >= kMaxPv) break;
        const Move m = q.parseUCI(u);
        if (!m.valid()) break;
        out.push_back(q.toUCI(m));
        q.makeMove(m);
    }
    return out;
}

// What an analysis says of position p, or false when it cannot be this position's search (the
// other side to move, no line, a first move that is not legal here).
bool fromAnalysis(const Position& p, const ai::Analysis& a, PositionEval& out) {
    if (a.noLegalMove || a.lines.empty()) return false;
    if (a.whiteToMove != (p.sideToMove() == White)) return false;
    const ai::PvLine& l1 = a.lines[0];
    out.pv = legalPrefix(p, l1.pv);
    if (out.pv.empty()) return false;
    out.depth = std::max(1, a.depth > 0 ? a.depth : l1.depth);
    out.best = l1.score;
    out.bestUci = out.pv[0];
    out.hasSecond = false;
    if (a.lines.size() > 1 && !a.lines[1].pv.empty()) {
        const Move m2 = p.parseUCI(a.lines[1].pv[0]);
        if (m2.valid() && p.toUCI(m2) != out.bestUci) {
            out.hasSecond = true;
            out.second = a.lines[1].score;
            out.secondUci = p.toUCI(m2);
        }
    }
    return true;
}

// The cached evaluation of a position, made safe: scores within reach, the moves legal here (a
// damaged file, or another game under the same key, never reaches the verdicts).
bool sanitized(const Position& p, const PositionEval& in, PositionEval& out) {
    auto sane = [](const ai::Score& s) { return std::abs(s.cp) <= 100000 && std::abs(s.mate) <= 500; };
    if (in.depth < 1 || in.depth > 245 || !sane(in.best)) return false;
    const Move bm = p.parseUCI(in.bestUci);
    if (!bm.valid()) return false;
    out = PositionEval();
    out.depth = in.depth;
    out.best = in.best;
    out.best.matedNow = out.best.matesNow = false;   // only a position without legal moves is mated now
    out.bestUci = p.toUCI(bm);
    out.pv = legalPrefix(p, in.pv);
    if (out.pv.empty() || out.pv[0] != out.bestUci) out.pv = {out.bestUci};
    if (in.hasSecond && sane(in.second)) {
        const Move m2 = p.parseUCI(in.secondUci);
        if (m2.valid() && p.toUCI(m2) != out.bestUci) {
            out.hasSecond = true;
            out.second = in.second;
            out.second.matedNow = out.second.matesNow = false;
            out.secondUci = p.toUCI(m2);
        }
    }
    return true;
}

// White's winning chances in a position, 0..100 (mates 100 / 0, a stalemate 50).
double whiteWin(const Position& p, const PositionEval& e) {
    return coach::winPercent(e.best.forWhite(p.sideToMove() == White));
}

std::string barText(int cp) {
    const int tenths = (std::abs(cp) + 5) / 10;   // one decimal, halves away from zero
    if (tenths == 0) return "0.0";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%c%d.%d", cp > 0 ? '+' : '-', tenths / 10, tenths % 10);
    return buf;
}

}  // namespace

void GameReview::reset(const Position& start, const std::vector<Move>& moves, const Settings& s) {
    *this = GameReview();
    settings_ = s;
    settings_.quickDepth = std::clamp(settings_.quickDepth, 1, 245);
    settings_.deepDepth = std::clamp(settings_.deepDepth, settings_.quickDepth, 245);
    settings_.multiPV = std::clamp(settings_.multiPV, 1, 8);
    settings_.quickMoveTimeMs = std::max(0, settings_.quickMoveTimeMs);
    settings_.deepMoveTimeMs = std::max(0, settings_.deepMoveTimeMs);

    // The record as given: the moves up to the first one that is not legal. A record that goes on
    // after an ending the rules apply by themselves (a dead position, a fivefold repetition) is
    // kept whole, without them.
    auto build = [&](bool endDetection) {
        game_ = Game();
        game_.setEndDetection(endDetection);
        if (!start.isStandardStart()) game_.resetFromFEN(start.fen());
        for (const Move& m : moves)
            if (!game_.play(m)) break;
    };
    build(true);
    if (game_.moves().size() < moves.size() && game_.isOver()) build(false);

    moves_ = game_.moves();
    const size_t n = moves_.size() + 1;
    positions_.clear();
    for (size_t i = 0; i < n; ++i) positions_.push_back(game_.positionAt(i));
    evals_.assign(n, PositionEval());
    pending_.assign(n, false);
    inBook_.assign(n, false);
    const coach::OpeningBook& book = coach::OpeningBook::instance();
    for (size_t i = 0; i < n; ++i) {
        const Position& p = positions_[i];
        inBook_[i] = !book.empty() && book.lookup(p.hash());
        PositionEval& e = evals_[i];
        e.legalMoves = int(p.legalMoves().size());
        if (e.legalMoves == 0) {
            // Known without a search, as final as the deep pass.
            e.terminal = true;
            e.final = true;
            e.depth = settings_.deepDepth;
            e.best = ai::Score();
            e.best.matedNow = p.inCheck();
        }
    }
    facts_.assign(moves_.size(), PlyFacts());
    for (size_t k = 0; k < moves_.size(); ++k) {
        const Position& before = positions_[k];
        facts_[k].gives = detail::materialGiven(before, moves_[k]).any();
        facts_[k].recapture = coach::isRecapture(game_, k);
        facts_[k].forcedReply = before.inCheck() && detail::sensibleMoves(before) <= 1;
        const Move legal = before.findLegal(moves_[k].from, moves_[k].to, moves_[k].promotion);
        facts_[k].takesGift = k > 0 && facts_[k - 1].gives && legal.valid() && (legal.flags & MoveCapture);
    }
}

void GameReview::clear() { *this = GameReview(); }

const Position& GameReview::positionAt(int i) const {
    static const Position kStart;
    if (positions_.empty()) return kStart;
    return positions_[size_t(std::clamp(i, 0, int(positions_.size()) - 1))];
}

const PositionEval& GameReview::position(int i) const {
    static const PositionEval kNone;
    if (i < 0 || i >= int(evals_.size())) return kNone;
    return evals_[size_t(i)];
}

bool GameReview::nextRequest(int focus, ai::AnalysisRequest& out, int& position) {
    const int n = int(evals_.size());
    if (n == 0) return false;
    focus = std::clamp(focus, 0, n - 1);
    for (const bool deep : {false, true}) {
        auto wanted = [&](int i) {
            if (i < 0 || i >= n || pending_[size_t(i)]) return false;
            const PositionEval& e = evals_[size_t(i)];
            if (e.final || e.failed) return false;
            return deep ? e.depth > 0 : e.depth == 0;
        };
        int pick = -1;
        // The board's position and its surroundings (focus, focus + 1, focus - 1, focus + 2 ...),
        // then the rest from the start.
        for (int d = 0; d <= kAround && pick < 0; ++d) {
            if (wanted(focus + d)) pick = focus + d;
            else if (d > 0 && wanted(focus - d)) pick = focus - d;
        }
        for (int i = 0; i < n && pick < 0; ++i)
            if (wanted(i)) pick = i;
        if (pick < 0) continue;
        out = coach::detail::requestAt(game_, size_t(pick));
        out.multiPV = settings_.multiPV;
        out.depth = deep ? settings_.deepDepth : settings_.quickDepth;
        out.moveTimeMs = deep ? settings_.deepMoveTimeMs : settings_.quickMoveTimeMs;
        out.priority = -1;   // background work: never ahead of a move search
        pending_[size_t(pick)] = true;
        position = pick;
        return true;
    }
    return false;
}

void GameReview::accept(int i, const ai::Analysis& a) {
    if (i < 0 || i >= int(evals_.size())) return;
    pending_[size_t(i)] = false;
    PositionEval& e = evals_[size_t(i)];
    if (e.final || e.failed) return;   // known already (no search needed, restored, a late answer)
    if (!a.ok) {
        fail(i);
        return;
    }
    // The pass the request was for: a position never analysed gets the quick one (nextRequest).
    const bool quick = e.depth == 0;
    PositionEval got;
    if (!fromAnalysis(positions_[size_t(i)], a, got)) {
        fail(i);
        return;
    }
    if (!quick && got.depth < e.depth) {   // a deep search cut short below the quick one: keep that
        e.final = true;
        return;
    }
    got.legalMoves = e.legalMoves;
    got.final = !quick || got.depth >= settings_.deepDepth;
    e = got;
}

void GameReview::fail(int i) {
    if (i < 0 || i >= int(evals_.size())) return;
    pending_[size_t(i)] = false;
    PositionEval& e = evals_[size_t(i)];
    if (e.final) return;
    // The deep pass refused: the quick result stands as the last word. Never analysed: left out.
    if (e.depth > 0) e.final = true;
    else e.failed = true;
}

void GameReview::forget(int i) {
    if (i < 0 || i >= int(pending_.size())) return;
    pending_[size_t(i)] = false;
}

Verdict GameReview::verdict(int ply) const {
    Verdict v;
    if (ply < 0 || ply >= plies()) return v;
    const size_t k = size_t(ply);
    const Position& before = positions_[k];
    const Position& after = positions_[k + 1];
    v.mover = before.sideToMove();
    v.uci = before.toUCI(moves_[k]);
    v.san = game_.sanMoves()[k];
    v.book = inBook_[k + 1];
    const PositionEval& e0 = evals_[k];
    const PositionEval& e1 = evals_[k + 1];
    if (!analysed(e0) || !analysed(e1)) return v;
    v.known = true;
    v.final = e0.final && e1.final;
    // The played move's value is the next position's score from the mover's side; the engine's
    // own move is judged against itself (another search's score would make it a mate missed).
    const ai::Score best = e0.best, played = e1.best.flipped();
    v.playedBest = !e0.bestUci.empty() && e0.bestUci == v.uci;
    const coach::Judgement j = coach::judge(best, v.playedBest ? best : played, v.playedBest);
    v.wBest = coach::winPercent(best);
    v.wPlayed = coach::winPercent(played);
    v.loss = v.playedBest ? 0.0 : std::max(0.0, v.wBest - v.wPlayed);
    v.cls = j.cls;
    if (e0.legalMoves == 1) v.cls = coach::MoveClass::Forced;
    else if (v.book && v.cls != coach::MoveClass::Blunder) v.cls = coach::MoveClass::Book;
    if (!v.playedBest && !e0.bestUci.empty()) {
        const Move bm = before.parseUCI(e0.bestUci);
        if (bm.valid()) {
            v.betterUci = e0.bestUci;
            v.betterSan = before.toSAN(bm);
        }
    }
    {
        const ai::Score w = e1.best.forWhite(after.sideToMove() == White);
        v.whiteCpAfter = mates(w) ? 10000 : mated(w) ? -10000 : w.cp;
    }

    const PlyFacts& f = facts_[k];
    v.sacrifice = f.gives;
    const bool sound = v.cls == coach::MoveClass::Best || v.cls == coach::MoveClass::Excellent ||
                       v.cls == coach::MoveClass::Good;
    v.onlyMove = sound && v.playedBest && e0.hasSecond && e0.legalMoves > 1 &&
                 v.wBest - coach::winPercent(e0.second) >= 12.0 && !f.recapture && !f.takesGift && !f.forcedReply &&
                 v.wBest >= 8.0 && v.wBest <= 92.0;
    switch (v.cls) {
    case coach::MoveClass::Blunder: v.nag = Nag::Blunder; break;
    case coach::MoveClass::Mistake: v.nag = Nag::Mistake; break;
    case coach::MoveClass::Inaccuracy: v.nag = Nag::Dubious; break;
    default:
        if (!sound) break;
        if ((v.playedBest || v.loss < 2.0) && f.gives && v.wPlayed >= 50.0 && v.wBest < 90.0 && !f.recapture)
            v.nag = Nag::Brilliant;
        else if (v.onlyMove)
            v.nag = Nag::Good;
        else if (!v.playedBest && v.loss < 5.0 && f.gives)
            v.nag = Nag::Interesting;
        break;
    }
    return v;
}

EvalBar GameReview::bar(int i) const {
    EvalBar b;
    if (i < 0 || i >= int(evals_.size())) return b;
    const PositionEval& e = evals_[size_t(i)];
    if (!analysed(e)) return b;
    b.known = true;
    const bool whiteToMove = positions_[size_t(i)].sideToMove() == White;
    if (e.terminal) {
        if (e.best.matedNow) {
            b.white = whiteToMove ? 0.0f : 1.0f;
            b.text = whiteToMove ? "0-1" : "1-0";
        } else {
            b.white = 0.5f;
            b.text = "\xC2\xBD-\xC2\xBD";   // "½-½"
        }
        return b;
    }
    const ai::Score s = e.best.forWhite(whiteToMove);
    if (mates(s)) {
        b.white = 1.0f;
        b.mateWhite = std::max(1, s.mate);
        b.text = "M" + std::to_string(b.mateWhite);
    } else if (mated(s)) {
        b.white = 0.0f;
        b.mateWhite = -std::max(1, -s.mate);
        b.text = "-M" + std::to_string(-b.mateWhite);
    } else {
        b.white = float(coach::winPercent(s.cp) / 100.0);
        b.text = barText(s.cp);
    }
    return b;
}

float GameReview::progress() const {
    if (evals_.empty()) return 1.0f;
    // A failed position counts as done: no search will ever come for it.
    int done = 0;
    for (const PositionEval& e : evals_) done += (e.final || e.failed) ? 1 : 0;
    return float(done) / float(evals_.size());
}

bool GameReview::complete() const {
    for (const PositionEval& e : evals_)
        if (!e.final && !e.failed) return false;
    return true;
}

SideSummary GameReview::summary(Color c) const {
    SideSummary s;
    const int n = plies();
    if (n == 0) return s;
    // White's W% of every position (one not analysed yet keeps the last known value) for the
    // volatility weights; one weight per ply.
    std::vector<double> w(size_t(n) + 1);
    double last = 50.0;
    for (int i = 0; i <= n; ++i) {
        if (analysed(evals_[size_t(i)])) last = whiteWin(positions_[size_t(i)], evals_[size_t(i)]);
        w[size_t(i)] = last;
    }
    const std::vector<double> weights = coach::accuracyWeights(w);
    double wSum = 0.0, wAcc = 0.0, inv = 0.0;
    for (int k = 0; k < n; ++k) {
        if (positions_[size_t(k)].sideToMove() != c) continue;
        const Verdict v = verdict(k);
        if (!v.known) continue;
        // A book move counts as perfect; the engine's own move as lossless (Verdict::loss).
        const double acc = v.cls == coach::MoveClass::Book ? 100.0 : coach::moveAccuracy(v.wBest, v.wBest - v.loss);
        const double weight = size_t(k) < weights.size() ? weights[size_t(k)] : 0.5;
        ++s.moves;
        ++s.count[int(v.nag)];
        wAcc += acc * weight;
        wSum += weight;
        inv += 1.0 / std::max(1.0, acc);
    }
    if (s.moves > 0) s.accuracy = (wAcc / wSum + double(s.moves) / inv) / 2.0;
    return s;
}

// FNV-1a (64 bits) of a version tag, the start position's FEN and the moves in UCI: the same game
// gives the same key on every platform and in every version that keeps the tag.
std::string GameReview::key() const {
    uint64_t h = 1469598103934665603ULL;
    auto feed = [&h](const std::string& s) {
        for (const unsigned char ch : s) {
            h ^= ch;
            h *= 1099511628211ULL;
        }
    };
    feed("scacelith-analysis-1\n");
    feed(positionAt(0).fen());
    for (size_t k = 0; k < moves_.size(); ++k) feed(" " + positions_[k].toUCI(moves_[k]));
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return buf;
}

void GameReview::restore(const std::vector<PositionEval>& evals, int savedDeepDepth) {
    if (evals.size() != evals_.size()) return;
    const bool sameTarget = savedDeepDepth >= settings_.deepDepth;
    for (size_t i = 0; i < evals.size(); ++i) {
        PositionEval& e = evals_[i];
        // A position without moves is known already; one handed out waits for its own answer.
        if (e.terminal || e.final || pending_[i]) continue;
        const PositionEval& saved = evals[i];
        const bool keptFinal = sameTarget && saved.final;
        if (saved.failed || saved.terminal || (saved.depth < settings_.quickDepth && !keptFinal) || saved.depth < e.depth)
            continue;
        PositionEval got;
        if (!sanitized(positions_[i], saved, got)) continue;
        got.legalMoves = e.legalMoves;
        got.final = got.depth >= settings_.deepDepth || keptFinal;
        e = got;
    }
}

}  // namespace analysis
