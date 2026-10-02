// The coach's review: classification, voicing, praise and takeback policy, script assembly,
// announcements and threat warnings (see review.h). The explanation detectors live in
// review_explain.cpp.
#include "coach/review.h"

#include "coach/review_internal.h"
#include "coach/tactics.h"

#include <algorithm>
#include <cmath>

namespace coach {

using namespace chess;
using namespace detail;

// ---- Win percentage, accuracy, classes ------------------------------------------------------------

double winPercent(int cp) {
    cp = std::clamp(cp, -1000, 1000);
    return 50.0 + 50.0 * (2.0 / (1.0 + std::exp(-0.00368208 * cp)) - 1.0);
}

namespace {
bool mates(const ai::Score& s) { return s.mate > 0 || s.matesNow; }
bool mated(const ai::Score& s) { return s.mate < 0 || s.matedNow; }
int mateMoves(const ai::Score& s) { return s.matesNow || s.matedNow ? 0 : std::abs(s.mate); }
}  // namespace

double winPercent(const ai::Score& s) {
    if (mates(s)) return winPercent(1000);
    if (mated(s)) return winPercent(-1000);
    return winPercent(s.cp);
}

double moveAccuracy(double wBefore, double wAfter) {
    if (wAfter >= wBefore) return 100.0;
    const double raw = 103.1668100711649 * std::exp(-0.04354415386753951 * (wBefore - wAfter)) - 3.166924740191411;
    return std::clamp(raw + 1.0, 0.0, 100.0);
}

const char* moveClassName(MoveClass c) {
    switch (c) {
    case MoveClass::Unjudged: return "unjudged";
    case MoveClass::Book: return "book";
    case MoveClass::Forced: return "forced";
    case MoveClass::Best: return "best";
    case MoveClass::Excellent: return "excellent";
    case MoveClass::Good: return "good";
    case MoveClass::Inaccuracy: return "inaccuracy";
    case MoveClass::Mistake: return "mistake";
    case MoveClass::Blunder: return "blunder";
    }
    return "";
}

MoveClass classifyDelta(double delta, bool playedIsBest) {
    if (playedIsBest || delta < 0.5) return MoveClass::Best;
    if (delta < 2.0) return MoveClass::Excellent;
    if (delta < 5.0) return MoveClass::Good;
    if (delta < 10.0) return MoveClass::Inaccuracy;
    if (delta < 15.0) return MoveClass::Mistake;
    return MoveClass::Blunder;
}

Judgement judge(const ai::Score& best, const ai::Score& played, bool playedIsBest) {
    Judgement j;
    j.wBest = winPercent(best);
    j.wPlayed = winPercent(played);
    j.delta = std::max(0.0, j.wBest - j.wPlayed);
    // Lichess Advice.scala: mate advice first, then the win-chance delta.
    if (mates(best) && !mates(played)) {
        j.mate = MateChange::Lost;
        if (mated(played)) j.cls = MoveClass::Blunder;
        else j.cls = played.cp > 999 ? MoveClass::Inaccuracy : played.cp > 700 ? MoveClass::Mistake : MoveClass::Blunder;
        return j;
    }
    if (!mates(best) && !mated(best) && mated(played)) {
        j.mate = MateChange::Created;
        j.cls = best.cp < -999 ? MoveClass::Inaccuracy : best.cp < -700 ? MoveClass::Mistake : MoveClass::Blunder;
        return j;
    }
    if (mates(best) && mates(played) && mateMoves(played) > mateMoves(best)) j.mate = MateChange::Delayed;
    j.cls = classifyDelta(j.delta, playedIsBest);
    return j;
}

const char* exTypeName(ExType t) {
    switch (t) {
    case ExType::None: return "none";
    case ExType::MateAllowed: return "mate_allowed";
    case ExType::MateMissed: return "mate_missed";
    case ExType::Stalemate: return "stalemate";
    case ExType::Fork: return "fork";
    case ExType::Discovered: return "discovered";
    case ExType::Skewer: return "skewer";
    case ExType::Pin: return "pin";
    case ExType::Trapped: return "trapped";
    case ExType::BackRank: return "back_rank";
    case ExType::Hanging: return "hanging";
    case ExType::Exchange: return "exchange";
    case ExType::MissedCapture: return "missed_capture";
    case ExType::MissedFork: return "missed_fork";
    case ExType::PromotionRace: return "promotion";
    case ExType::KingSafety: return "king_safety";
    case ExType::BadTrade: return "bad_trade";
    case ExType::Opening: return "opening";
    case ExType::Endgame: return "endgame";
    case ExType::Positional: return "positional";
    }
    return "none";
}

Band band(int level) {
    level = std::clamp(level, 1, 6);
    // level, demo, mate line, missed mate, lookahead, sentences, remarks/10, praise every, offer cap, pause
    static const Band kBands[6] = {
        {1, 1, 1, 1, 2, 3, 5, 3, -1, 0.80f},
        {2, 2, 1, 2, 3, 3, 4, 4, -1, 0.60f},
        {3, 3, 3, 2, 4, 4, 3, 5, -1, 0.50f},
        {4, 4, 3, 3, 5, 4, 3, 6, 5, 0.45f},
        {5, 6, 5, 4, 7, 3, 2, 8, 3, 0.35f},
        {6, 8, 7, 5, 9, 2, 2, 8, 2, 0.35f},
    };
    return kBands[level - 1];
}

// ---- Beat helpers (review_internal.h) ----------------------------------------------------------------

namespace detail {

std::string bandKey(const std::string& family, int level) { return family + ".b" + std::to_string(level); }

Beat sayBeat(const std::string& key, Look look, int ply, Priority pr) {
    Beat b;
    b.kind = BeatKind::Say;
    b.line.key = key;
    b.look = look;
    b.ply = ply;
    b.priority = pr;
    return b;
}

Arg pieceArg(const Position& p, Square s, Color listener) {
    const Piece pc = p.at(s);
    return Arg::ofPiece(pc.type, pc.color, pc.color == listener, s);
}

Arg moveArg(const Position& before, const Move& m) { return Arg::ofMove(before.toSAN(m), before.toUCI(m)); }
Arg moveArg(const LineStep& s) { return Arg::ofMove(s.san, s.uci); }

namespace {
Gesture gesture(GestureKind k, Square s, const std::string& anchor) {
    Gesture g;
    g.kind = k;
    g.square = s;
    g.anchor = anchor;
    return g;
}
bool isPointing(GestureKind k) {
    return k == GestureKind::PointPiece || k == GestureKind::PointSquare || k == GestureKind::Trace;
}
}  // namespace

void markSquare(Beat& b, Square s, const std::string& anchor) {
    if (s == NoSquare) return;
    Mark m;
    m.kind = Mark::Kind::Square;
    m.square = s;
    m.anchor = anchor;
    b.marks.push_back(m);
}

void markPiece(Beat& b, Square s, const std::string& anchor) {
    if (s == NoSquare) return;
    Mark m;
    m.kind = Mark::Kind::Piece;
    m.square = s;
    m.anchor = anchor;
    b.marks.push_back(m);
}

void markArrow(Beat& b, PieceType t, Square from, Square to, const std::string& anchor) {
    if (from == NoSquare || to == NoSquare) return;
    Mark m;
    m.kind = Mark::Kind::Arrow;
    m.from = from;
    m.to = to;
    m.via = t == Knight ? knightCorner(from, to) : NoSquare;
    m.anchor = anchor;
    b.marks.push_back(m);
}

void pointPiece(Beat& b, Square s, const std::string& anchor, bool emphasis) {
    if (s == NoSquare) return;
    Gesture g = gesture(GestureKind::PointPiece, s, anchor);
    g.emphasis = emphasis;
    b.gestures.push_back(g);
    markPiece(b, s, anchor);
}

void pointSquare(Beat& b, Square s, const std::string& anchor) {
    if (s == NoSquare) return;
    b.gestures.push_back(gesture(GestureKind::PointSquare, s, anchor));
    markSquare(b, s, anchor);
}

void traceMove(Beat& b, PieceType t, Square from, Square to, const std::string& anchor) {
    if (from == NoSquare || to == NoSquare) return;
    Gesture g = gesture(GestureKind::Trace, to, anchor);
    g.path = tracePath(t, from, to);
    b.gestures.push_back(g);
    markArrow(b, t, from, to, anchor);
}

void limitPointing(Beat& b) {
    int n = 0;
    std::vector<Gesture> kept;
    for (const Gesture& g : b.gestures) {
        if (isPointing(g.kind) && ++n > 3) continue;   // its mark stays: a highlight without the finger
        kept.push_back(g);
    }
    b.gestures.swap(kept);
}

int Ctx::lossWithin(int plies) const {
    int worst = 0;
    const int n = std::min<int>(plies, int(r.size()));
    for (int i = 0; i < n; ++i) {
        int l = base - r[size_t(i)].balance;
        if (i + 1 < int(r.size())) l = std::min(l, base - r[size_t(i + 1)].balance);
        worst = std::max(worst, l);
    }
    return worst;
}

namespace {

Line narration(const Ctx& c, const Position& before, const LineStep& st, int i) {
    Line l;
    if (c.level >= 4) {
        l.key = "demo.move";
        l.with("move", moveArg(st));
        return l;
    }
    const Square capSq = (st.move.flags & MoveEnPassant) ? Square(st.move.to + (st.mover == White ? -8 : 8)) : st.move.to;
    if (st.mover == c.coach) {
        l.with("my", pieceArg(before, st.move.from, c.human)).with("sq", Arg::ofSquare(st.move.to));
        if (st.mate) l.key = "demo.my.mate";
        else if (st.captured != NoPiece) {
            l.key = "demo.my.take";
            l.with("your", Arg::ofPiece(st.captured, c.human, true, capSq));
        } else if (st.check) l.key = "demo.my.check";
        else l.key = "demo.my.move";
    } else {
        l.with("your", pieceArg(before, st.move.from, c.human)).with("sq", Arg::ofSquare(st.move.to));
        if (before.inCheck() && st.piece == King) l.key = "demo.your.king";
        else if (st.captured != NoPiece && i > 0 && c.r[size_t(i - 1)].move.to == st.move.to) l.key = "demo.your.back";
        else if (st.captured != NoPiece) {
            l.key = "demo.your.take";
            l.with("my", Arg::ofPiece(st.captured, c.coach, false, capSq));
        } else l.key = "demo.your.move";
    }
    return l;
}

}  // namespace

void appendDemo(const Ctx& c, const Explanation& ex, Script& s) {
    int shown = 0;
    Position pos = c.p1;
    for (int i = 0; i < ex.demoPlies && i < int(c.r.size()) && i < c.b.demoPlies; ++i) {
        const LineStep& st = c.r[size_t(i)];
        if (st.mover == c.human && st.promotion != NoPiece) {
            // The human's spare queen is out of the coach's reach: point at the square instead.
            Beat b = sayBeat("demo.promote", Look::Target, c.ply);
            b.line.with("sq", Arg::ofSquare(st.move.to)).with("your", pieceArg(pos, st.move.from, c.human));
            pointSquare(b, st.move.to, "sq");
            s.push_back(b);
            break;
        }
        Beat d;
        d.kind = BeatKind::DemoMove;
        d.uci = st.uci;
        d.line = narration(c, pos, st, i);
        d.look = Look::Board;
        d.ply = c.ply;
        s.push_back(d);
        Beat pause;
        pause.kind = BeatKind::Pause;
        pause.seconds = c.b.demoPause;
        pause.look = Look::Board;
        pause.ply = c.ply;
        s.push_back(pause);
        pos.makeMove(st.move);
        ++shown;
    }
    if (shown == 1)
        for (const Beat& t : ex.tail) s.push_back(t);
    if (shown > 0) {
        Beat r;
        r.kind = BeatKind::Rewind;
        r.count = shown;
        r.look = Look::Board;
        r.ply = c.ply;
        r.skippable = false;   // a skipped rewind still runs, faster
        if (c.level <= 4) r.line.key = bandKey("ex.rewind", c.level);
        s.push_back(r);
    }
}

}  // namespace detail

// ---- Reviewer ------------------------------------------------------------------------------------

namespace detail {

std::string startFenOf(const Game& g) {
    const Position& s = g.startPosition();
    const Position standard;
    return s.samePosition(standard) && s.fullmoveNumber() == 1 && s.halfmoveClock() == 0 ? std::string() : s.fen();
}

ai::AnalysisRequest requestAt(const Game& g, size_t plies) {
    ai::AnalysisRequest r;
    r.startFen = startFenOf(g);
    std::vector<std::string> moves = g.uciMoves();
    moves.resize(std::min(plies, moves.size()));
    r.moves = moves;
    return r;
}

int whiteCp(const ai::Score& s, bool whiteToMove) {
    const int cp = mates(s) ? 1000 : mated(s) ? -1000 : std::clamp(s.cp, -1000, 1000);
    return whiteToMove ? cp : -cp;
}

Arg evalArg(const ai::Score& s) { return Arg::ofEval(s.cp, s.matesNow ? 1 : s.matedNow ? -1 : s.mate); }

}  // namespace detail

namespace {

// A synthetic line for a move that ends the game (the engine has nothing to search after it).
ai::PvLine terminalLine(const Position& p1, const std::string& uci) {
    ai::PvLine l;
    l.pv = {uci};
    if (p1.isCheckmate()) l.score.mate = 1;
    else l.score.cp = 0;
    return l;
}

}  // namespace

void Reviewer::reset(int level, Color human, bool offersEnabled) {
    *this = Reviewer();
    level_ = std::clamp(level, 1, 6);
    human_ = human;
    offersEnabled_ = offersEnabled;
}

bool Reviewer::remarkAllowed() const {
    int n = 0;
    for (int m : remarks_)
        if (humanMoves_ - m < 10) ++n;
    return n < band(level_).remarksPer10;
}

void Reviewer::noteRemark() {
    remarks_.push_back(humanMoves_);
    while (!remarks_.empty() && humanMoves_ - remarks_.front() >= 10) remarks_.pop_front();
}

ai::AnalysisRequest Reviewer::beforeRequest(const Game& g) const {
    ai::AnalysisRequest r = requestAt(g, g.moves().size());
    r.multiPV = 3;
    r.depth = level_ <= 3 ? 16 : 18;
    r.moveTimeMs = 2500;
    return r;
}

bool Reviewer::needsPlayedRequest(const ai::Analysis& before, const std::string& playedUci) {
    return before.ok && !before.noLegalMove && before.line(playedUci) == nullptr;
}

ai::AnalysisRequest Reviewer::playedRequest(const Game& g, const ai::Analysis& before) const {
    const size_t n = g.moves().size();
    ai::AnalysisRequest r = requestAt(g, n > 0 ? n - 1 : 0);
    r.multiPV = 1;
    r.depth = before.depth > 0 ? before.depth : 16;
    r.moveTimeMs = 1500;
    if (n > 0) r.searchMoves = {g.positionAt(n - 1).toUCI(g.moves()[n - 1])};
    return r;
}

ai::AnalysisRequest Reviewer::afterRequest(const Game& g) const {
    ai::AnalysisRequest r = requestAt(g, g.moves().size());
    r.multiPV = 2;
    r.depth = 14;
    r.moveTimeMs = 1500;
    return r;
}

ai::AnalysisRequest Reviewer::shallowRequest(const Game& g) const {
    ai::AnalysisRequest r = requestAt(g, g.moves().size());
    r.multiPV = 1;
    r.depth = 6;
    r.moveTimeMs = 0;
    return r;
}

Script Reviewer::announce(const Game& g) {
    Script s;
    if (g.moves().empty()) return s;
    const int ply = int(g.moves().size()) - 1;
    const Position& p = g.position();
    const Color mover = g.positionAt(size_t(ply)).sideToMove();
    const bool byHuman = mover == human_;
    if (p.isCheckmate()) {
        Beat b = sayBeat(byHuman ? (level_ <= 3 ? "ann.mate.human.b1" : "ann.mate.human")
                                 : (level_ <= 2 ? "ann.mate.coach.b1" : "ann.mate.coach"),
                         Look::Player, ply, Priority::Urgent);
        if (byHuman) b.gestures.push_back(Gesture{GestureKind::Nod, NoSquare, {}, "", 0.0f, false});
        s.push_back(b);
        return s;
    }
    if (!p.inCheck()) return s;
    if (level_ >= 2 && squareCount(p.checkers()) >= 2) {
        s.push_back(sayBeat(byHuman ? "ann.double_check.human" : "ann.double_check.coach", Look::Board, ply,
                            Priority::Urgent));
        return s;
    }
    if (byHuman) {
        s.push_back(sayBeat(level_ <= 2 ? "ann.check.human.b1" : "ann.check.human", Look::Board, ply, Priority::Urgent));
        return s;
    }
    s.push_back(sayBeat("ann.check.coach", Look::Board, ply, Priority::Urgent));
    if (level_ <= 2 && !coachCheckExplained_) {
        coachCheckExplained_ = true;
        Beat b = sayBeat("ann.check.coach.first", Look::Target, ply, Priority::Urgent);
        const Square k = p.kingSquare(human_);
        b.line.with("your", pieceArg(p, k, human_));
        pointPiece(b, k, "your", true);
        s.push_back(b);
    }
    return s;
}

Review Reviewer::review(const ReviewInput& in) {
    Review out;
    if (!in.game || in.game->moves().empty()) return out;
    const Game& g = *in.game;
    const Band bd = band(level_);

    Ctx c;
    c.b = bd;
    c.level = level_;
    c.human = human_;
    c.coach = opposite(human_);
    c.g = &g;
    c.ply = int(g.moves().size()) - 1;
    c.p0 = g.positionAt(size_t(c.ply));
    c.p1 = g.position();
    c.played = g.moves()[size_t(c.ply)];
    c.playedUci = c.p0.toUCI(c.played);
    c.playedSan = g.sanMoves()[size_t(c.ply)];
    c.f = analyzeMove(c.p0, c.played);
    c.base = materialBalance(c.p0, human_);
    // Moves taken back (an offer accepted or not) leave the W% history.
    humanW_.resize(size_t(c.ply), -1.0);

    PlyVerdict& v = out.verdict;
    v.ply = c.ply;
    v.mover = c.p0.sideToMove();
    v.human = true;
    v.uci = c.playedUci;
    v.san = c.playedSan;
    v.phase = uint8_t(phaseOfPly(dividePhases(g), c.ply));
    v.check = c.p1.inCheck();
    v.captured = points(c.f.captured);

    const bool retry = retryPending_ && retry_.ply == c.ply;
    retryPending_ = false;
    if (!retry) ++humanMoves_;
    if (retry) {
        out.isRetry = true;
        out.takeback = retry_;
        out.takeback.same = retry_.firstUci == c.playedUci;
    }

    // ---- Engine figures ----
    const ai::Analysis* a0 = (in.before && in.before->ok && !in.before->lines.empty()) ? in.before : nullptr;
    ai::PvLine terminal;
    if (a0) {
        c.l1 = &a0->lines[0];
        if (a0->lines.size() > 1) c.l2 = &a0->lines[1];
        c.lp = a0->line(c.playedUci);
        if (!c.lp && in.played && in.played->ok && !in.played->lines.empty() && !in.played->lines[0].pv.empty() &&
            in.played->lines[0].pv[0] == c.playedUci)
            c.lp = &in.played->lines[0];
        if (!c.lp && !c.p1.hasLegalMove()) {
            terminal = terminalLine(c.p1, c.playedUci);
            c.lp = &terminal;
        }
    }
    if (!c.l1 || c.l1->pv.empty() || !c.lp) {
        // No judgement without the engine (research-pedagogy §1.6.8): the announcements and the
        // threat warnings are rules-based and run anyway.
        if (retry) out.takeback.fixed = false;
        return out;
    }
    c.isBest = c.l1->pv[0] == c.playedUci;
    c.j = judge(c.l1->score, c.lp->score, c.isBest);
    const bool forced = c.p0.legalMoves().size() == 1;
    MoveClass cls = c.j.cls;
    if (forced) cls = MoveClass::Forced;
    else if (in.inBook && c.j.delta < 15.0) cls = MoveClass::Book;

    v.cls = cls;
    v.wBest = c.j.wBest;
    v.wPlayed = c.j.wPlayed;
    v.delta = c.j.delta;
    v.accuracy = moveAccuracy(c.j.wBest, c.j.wPlayed);
    v.cpWhiteAfter = whiteCp(c.lp->score, c.p0.sideToMove() == White);
    v.hasEvalAfter = true;
    v.cpWhiteBefore = whiteCp(c.l1->score, c.p0.sideToMove() == White);
    v.hasEvalBefore = true;
    v.bestUci = c.l1->pv[0];
    {
        const Move bm = c.p0.parseUCI(v.bestUci);
        v.bestSan = bm.valid() ? c.p0.toSAN(bm) : std::string();
    }
    v.mateAllowed = c.j.mate == MateChange::Created;
    v.mateMissed = c.j.mate == MateChange::Lost;

    // ---- Lines replayed on the board ----
    std::vector<std::string> refutation(c.lp->pv.begin() + (c.lp->pv.empty() ? 0 : 1), c.lp->pv.end());
    if (in.after && in.after->ok && !in.after->lines.empty()) {
        const std::vector<std::string>& pv = in.after->lines[0].pv;
        if (pv.size() > refutation.size() && (refutation.empty() || pv[0] == refutation[0])) refutation = pv;
    }
    c.r = replayLine(c.p1, refutation, human_, 16);
    c.best = replayLine(c.p0, c.l1->pv, human_, 16);
    c.playedLine = replayLine(c.p0, c.lp->pv, human_, 16);
    for (size_t i = 0; i < c.r.size(); ++i) {
        const int l = std::min(c.base - c.r[i].balance, i + 1 < c.r.size() ? c.base - c.r[i + 1].balance : 1 << 20);
        if (l >= 2) {
            c.gain = int(i) + 1;
            break;
        }
    }
    c.loss = c.lossWithin(bd.lookahead);
    if (!c.playedLine.empty()) {
        const size_t k = std::min<size_t>(3, c.playedLine.size()) - 1;
        v.materialSwing = c.playedLine[k].balance - c.base;
    }

    // ---- What to say ----
    Explanation ex;
    const bool found = findExplanation(c, ex);
    if (found) v.exType = ex.type;
    const bool topMove = cls == MoveClass::Best || cls == MoveClass::Excellent ||
                         ((cls == MoveClass::Book || cls == MoveClass::Forced) && c.j.delta < 2.0);
    const bool fault = cls == MoveClass::Inaccuracy || cls == MoveClass::Mistake || cls == MoveClass::Blunder;
    const bool decidedWin = c.j.wBest >= 90.0 && c.j.wPlayed >= 80.0;
    const bool decidedLoss = c.j.wBest <= 10.0;
    const bool mateType = ex.type == ExType::MateAllowed || ex.type == ExType::MateMissed;
    const bool missedPiece = ex.type == ExType::MissedCapture && ex.offer;   // levels 1-2, a piece of 3+

    bool voice = false;
    bool isRemark = false;   // counts against the unsolicited-remarks cap
    if (found && ex.type == ExType::Stalemate) voice = true;
    else if (found && ex.type == ExType::MateMissed) voice = true;   // the detector checks the band's limit
    else if (missedPiece) voice = true;
    else if (fault) {
        switch (level_) {
        case 1: voice = cls == MoveClass::Blunder && found && ex.concrete; break;
        case 2:
            voice = (cls == MoveClass::Blunder && found && ex.concrete) ||
                    (cls == MoveClass::Mistake && found && ex.concrete && c.lossWithin(2) >= 1);
            isRemark = cls != MoveClass::Blunder;
            break;
        case 3: voice = cls != MoveClass::Inaccuracy; isRemark = cls == MoveClass::Mistake; break;
        case 4:
            // Inaccuracies only with a clear, concrete principle (and within the remarks cap).
            voice = cls != MoveClass::Inaccuracy || (found && ex.type != ExType::Positional);
            isRemark = cls != MoveClass::Blunder;
            break;
        case 5: voice = cls != MoveClass::Inaccuracy || c.j.delta >= 7.0; isRemark = cls != MoveClass::Blunder; break;
        default: voice = true; isRemark = cls != MoveClass::Blunder; break;
        }
    }
    if (voice && decidedWin && !(ex.type == ExType::Stalemate || ex.type == ExType::MateMissed)) voice = false;
    if (voice && decidedLoss && !(level_ <= 2 && ex.type == ExType::MateAllowed && ex.concrete)) voice = false;
    if (voice && (cls == MoveClass::Forced || cls == MoveClass::Book) && !mateType && ex.type != ExType::Stalemate) voice = false;
    if (voice && isRemark && !remarkAllowed()) voice = false;
    if (voice && retry && out.takeback.same) voice = false;
    // A principle instead (opening, technique): never a fault claim, Low priority, once per game each.
    Explanation tipEx;
    bool tip = false;
    if (!voice && !(retry && out.takeback.same) && !decidedLoss && !decidedWin && remarkAllowed())
        tip = findTip(c, tipsSaid_, tipEx) && (level_ <= 3 || c.j.delta >= 10.0);

    Script s;
    const int ply = c.ply;

    // A replayed move after a takeback.
    if (retry) {
        if (out.takeback.same) {
            s.push_back(sayBeat("tb.same", Look::Player, ply));
        } else {
            out.takeback.fixed = c.j.delta < 5.0;
            if (out.takeback.fixed) {
                Beat b = sayBeat(level_ <= 2 ? "tb.fixed.b1" : "tb.fixed", Look::Player, ply);
                b.gestures.push_back(Gesture{GestureKind::Nod, NoSquare, {}, "", 0.0f, false});
                s.push_back(b);
            }
        }
    }

    if (voice) {
        v.voiced = true;
        if (isRemark) noteRemark();
        // Offer policy (research-pedagogy §1.4, §1.6, §2.9, §2.10): every voiced blunder, and the
        // cases the detector flags per level (a mate within the band's limit, stalemate when
        // winning, a free piece of 3+ points missed at levels 1-2).
        bool offer = cls == MoveClass::Blunder || ex.offer;
        if (bd.offerCap >= 0 && offers_ >= bd.offerCap && !(ex.type == ExType::MateAllowed || ex.type == ExType::MateMissed))
            offer = false;
        if (!offersEnabled_) offer = false;
        if (retry && out.takeback.same) offer = false;
        if (offered_.ply == ply && offersAtPly_ >= 2) offer = false;

        // Sentence budget: verdict, cause, tail, tip, better move (demo narration, rewind and offer excluded).
        const bool wantBetter = level_ >= 3 && !ex.includesBest && !c.isBest && !c.best.empty() &&
                                ex.type != ExType::MateMissed && ex.type != ExType::MissedCapture &&
                                ex.type != ExType::MissedFork;
        std::string verdictKey;
        bool shake = false;
        // No verdict for a move that is not a fault (a slower mate at levels 5-6, a free piece
        // missed with a small loss at levels 1-2 gets the "missed" one): the cause says it all.
        if (ex.missed && level_ <= 2) verdictKey = bandKey("ex.verdict.missed", level_);
        else if (cls == MoveClass::Blunder) {
            verdictKey = bandKey("ex.verdict.blunder", level_);
            shake = true;
        } else if (cls == MoveClass::Mistake) {
            if (level_ >= 3) verdictKey = bandKey("ex.verdict.mistake", level_);   // level 2: one sentence only
        } else if (cls == MoveClass::Inaccuracy) {
            if (level_ >= 4 && (!found || ex.type == ExType::Positional)) verdictKey = bandKey("ex.verdict.inaccuracy", level_);
            else if (level_ >= 4) verdictKey = bandKey("ex.verdict.mistake", level_);
        }
        const bool verdictNamesBest = verdictKey.rfind("ex.verdict.inaccuracy", 0) == 0;
        int budget = bd.sentences - (verdictKey.empty() ? 0 : 1);
        std::vector<Beat> cause = ex.cause, tail = ex.tail, tip = ex.tip;
        if (level_ == 2 && cls == MoveClass::Mistake) {   // "one sentence"
            if (cause.size() > 1) cause.resize(1);
            tail.clear();
            tip.clear();
        }
        const bool better = wantBetter && !verdictNamesBest && (cause.empty() || budget >= 2);
        if (better) --budget;
        std::vector<Beat> useCause, useTail, useTip;
        for (size_t i = 0; i < cause.size() && budget > 0 && i < 1; ++i, --budget) useCause.push_back(cause[i]);
        const bool demoOnePly = std::min({ex.demoPlies, int(c.r.size()), bd.demoPlies}) == 1 &&
                                !(c.r.size() > 0 && c.r[0].mover == human_);
        if (demoOnePly)
            for (size_t i = 0; i < tail.size() && budget > 0; ++i, --budget) useTail.push_back(tail[i]);
        for (size_t i = 1; i < cause.size() && budget > 0; ++i, --budget) useCause.push_back(cause[i]);
        for (size_t i = 0; i < tip.size() && budget > 0; ++i, --budget) useTip.push_back(tip[i]);

        if (!verdictKey.empty()) {
            Beat vb = sayBeat(verdictKey, Look::Player, ply);
            vb.line.with("move", Arg::ofMove(c.playedSan, c.playedUci));
            if (!c.best.empty()) vb.line.with("best", moveArg(c.best[0]));
            if (verdictNamesBest && !c.best.empty()) {
                vb.look = Look::Target;
                traceMove(vb, c.best[0].piece, c.best[0].move.from, c.best[0].move.to, "best");
            } else if (shake) {
                vb.gestures.push_back(Gesture{GestureKind::ShakeHead, NoSquare, {}, "", 0.0f, false});
            }
            s.push_back(vb);
        }
        const bool demo = ex.demoPlies > 0 && !c.r.empty();
        for (Beat b : useCause) {
            if (demo)
                for (Mark& m : b.marks) m.untilRewind = true;
            limitPointing(b);
            s.push_back(b);
        }
        if (demo) {
            Explanation shown = ex;
            shown.tail = useTail;
            for (Beat& t : shown.tail) limitPointing(t);
            appendDemo(c, shown, s);
        }
        for (Beat b : useTip) {
            limitPointing(b);
            s.push_back(b);
        }
        if (better) {
            // {line} is what follows {best} ("Nf3, with the idea d4 c5"), never {best} again; level 5
            // names that idea, so without a continuation it says the level-4 line instead.
            std::string idea = sanLine(c.best, 1, size_t(std::max(1, std::min(4, bd.demoPlies) - 1)));
            Beat bb = sayBeat(bandKey("ex.better", idea.empty() && level_ == 5 ? 4 : level_), Look::Target, ply);
            bb.line.with("best", moveArg(c.best[0]))
                .with("move", Arg::ofMove(c.playedSan, c.playedUci))
                .with("line", Arg::ofMoves(idea))
                .with("eval", evalArg(c.l1->score));
            traceMove(bb, c.best[0].piece, c.best[0].move.from, c.best[0].move.to, "best");
            s.push_back(bb);
        }
        if (offer) {
            Beat ob;
            ob.kind = BeatKind::OfferTakeback;
            ob.line.key = bandKey("ex.offer", level_);
            ob.look = Look::Player;
            ob.ply = ply;
            ob.gestures.push_back(Gesture{GestureKind::Open, NoSquare, {}, "", 0.0f, false});
            s.push_back(ob);
            ++offers_;
            if (offered_.ply == ply) ++offersAtPly_;
            else offersAtPly_ = 1;
            offered_ = TakebackRecord{};
            offered_.ply = ply;
            offered_.firstUci = c.playedUci;
            offered_.firstType = ex.type;
            offeredHint_ = ex.hintSquare;
            out.offersTakeback = true;
            v.offered = true;
        }
    } else if (tip) {
        // A principle, said as a tip (Low priority).
        for (Beat b : tipEx.cause) {
            b.priority = Priority::Low;
            limitPointing(b);
            s.push_back(b);
        }
        if (tipEx.tipBit >= 0) tipsSaid_ |= 1u << tipEx.tipBit;
        noteRemark();
        v.voiced = true;
        if (v.exType == ExType::None) v.exType = tipEx.type;
    } else if (topMove && !(retry && !s.empty())) {
        // ---- Praise (research-pedagogy §1.5): top moves only ----
        const int legal = int(c.p0.legalMoves().size());
        const double w1 = c.j.wBest, w2 = c.l2 ? winPercent(c.l2->score) : w1;
        const bool decided = w1 >= 90.0 && !(c.p1.isCheckmate() || (c.l1->score.mate > 0 && c.isBest));
        const bool recapture = isRecapture(g, size_t(c.ply));
        const bool tricky = in.shallow && in.shallow->ok && !in.shallow->bestMove.empty() && in.shallow->bestMove != c.playedUci;
        auto lineGain = [&](size_t plies) {
            int bestGain = 0;
            for (size_t i = 0; i < c.playedLine.size() && i < plies; ++i) {
                int gnow = c.playedLine[i].balance - c.base;
                if (i + 1 < c.playedLine.size()) gnow = std::min(gnow, c.playedLine[i + 1].balance - c.base);
                bestGain = std::max(bestGain, gnow);
            }
            return bestGain;
        };
        bool promoSoon = false;
        for (size_t i = 0; i < c.playedLine.size() && i < 2; ++i)
            if (c.playedLine[i].promotion != NoPiece) promoSoon = true;
        const bool sacrifice = !promoSoon && (c.f.seeCp <= -200 ||
                                              (c.playedLine.size() >= 2 && c.playedLine[1].balance <= c.base - 2));
        const bool only = c.isBest && legal >= 2 && c.l2 && w1 - w2 >= 15.0 && w1 >= 25.0 && !(c.p0.inCheck() && legal <= 2);
        double lastW = -1.0;   // the human's W% after their previous judged move
        for (size_t i = humanW_.size(); i-- > 0 && lastW < 0.0;) lastW = humanW_[i];
        // Lost to a mate either way, but sooner after this move: Best by the numbers, never praised.
        const bool hastensMate =
            mated(c.lp->score) && mated(c.l1->score) && mateMoves(c.lp->score) < mateMoves(c.l1->score);
        std::string key;
        if (!c.p1.isCheckmate() && !recapture && !decided && !hastensMate && cls != MoveClass::Forced) {
            if (!brilliantDone_ && c.j.delta < 2.0 && sacrifice && w1 <= 85.0 && c.j.wPlayed >= 50.0) {
                key = level_ <= 2 ? "praise.sacrifice" : bandKey("praise.brilliant", level_);
                brilliantDone_ = true;
                v.brilliant = true;
            } else if (level_ >= 2 && c.isBest && w1 >= 60.0 && c.l2 && w1 - w2 >= 20.0 && lastW >= 0.0 && lastW <= 45.0) {
                key = bandKey("praise.great", level_);
                v.great = true;
            } else if (level_ >= 2 && only) {
                key = bandKey("praise.only", level_);
            } else if (level_ >= 2 && squareCount(c.f.forks) >= 2 && lineGain(4) >= 2) {
                key = level_ == 2 ? "praise.fork.b2" : "praise.fork.b3";
            } else if (level_ <= 2 && c.f.captured != NoPiece && c.f.seeCp >= 200) {
                key = bandKey("praise.capture", level_);
                v.goodCapture = true;
            } else if (level_ <= 3 && c.isBest && (lineGain(3) >= 2 || c.l1->score.mate > 0 || only) && (level_ < 3 || tricky)) {
                key = bandKey("praise.best", level_);
            } else if (level_ <= 3 && c.base >= 3 && c.f.captured != NoPiece && points(c.f.captured) >= 3 &&
                       c.f.seeCp >= 0 && c.f.seeCp < 200) {
                key = bandKey("praise.trade", level_);
            } else if ((level_ == 3 || level_ == 4) && c.j.delta < 2.0 && c.f.captured == NoPiece && !c.f.check && tricky) {
                key = bandKey("praise.excellent", level_);
            } else if (level_ == 1 && (c.f.castleKing || c.f.castleQueen) && !castlePraised_) {
                key = "praise.castle.b1";
                castlePraised_ = true;
            } else if (level_ == 1 && hasBit(hangingPieces(c.p0, human_, true), c.played.from) &&
                       !hasBit(hangingPieces(c.p1, human_, true), c.played.to)) {
                key = "praise.saved.b1";
            } else if (level_ <= 2 && in.inBook && bookPraises_ < 2) {
                key = "praise.book.b1";
                ++bookPraises_;
            }
        }
        v.only = only;
        v.goodCapture = v.goodCapture || (c.f.captured != NoPiece && c.f.seeCp >= 200);
        if (!key.empty() && humanMoves_ - lastPraise_ >= bd.praiseEvery && remarkAllowed() && !retry) {
            Beat b = sayBeat(key, Look::Player, ply);
            if (c.f.captured != NoPiece) b.line.with("my", Arg::ofPiece(c.f.captured, c.coach, false, NoSquare));
            b.line.with("move", Arg::ofMove(c.playedSan, c.playedUci));
            b.gestures.push_back(Gesture{GestureKind::Nod, NoSquare, {}, "", 0.0f, false});
            s.push_back(b);
            lastPraise_ = humanMoves_;
            noteRemark();
            v.praised = true;
        }
    }

    humanW_.push_back(c.j.wPlayed);
    out.script = s;
    return out;
}

Script Reviewer::coachMoved(const Game& g, const ai::Analysis* before) {
    Script s;
    if (g.moves().empty() || level_ > 2 || g.isOver()) return s;
    const int ply = int(g.moves().size()) - 1;
    const Position& now = g.position();
    const Position& prev = g.positionAt(size_t(ply));
    if (prev.sideToMove() == human_) return s;   // the last move was not the coach's
    const Color coach = opposite(human_);

    // "My last move was a mistake. Can you punish it?" (level 1, once, confirmed by A0).
    if (level_ == 1 && !punishHintDone_ && before && before->ok && !before->lines.empty() && !before->lines[0].pv.empty()) {
        const uint64_t hung = hangingPieces(now, coach, true) & ~hangingPieces(prev, coach, true);
        const Move bm = now.parseUCI(before->lines[0].pv[0]);
        if (bm.valid() && hasBit(hung, bm.to) && points(now.at(bm.to).type) >= 3 && remarkAllowed()) {
            Beat b = sayBeat("threat.punish.b1", Look::Player, ply, Priority::Low);
            b.gestures.push_back(Gesture{GestureKind::Open, NoSquare, {}, "", 0.0f, false});
            s.push_back(b);
            punishHintDone_ = true;
            noteRemark();
            return s;
        }
    }
    if (!remarkAllowed()) return s;
    // A mate threat first.
    Move mate;
    if (mateThreat(now, coach, &mate)) {
        Beat b = sayBeat(bandKey("threat.mate", level_), Look::Target, ply, Priority::Low);
        b.line.with("sq", Arg::ofSquare(mate.to)).with("my", pieceArg(now, mate.from, human_));
        if (level_ == 1) pointSquare(b, mate.to, "sq");
        s.push_back(b);
        noteRemark();
        return s;
    }
    // New threats against the human's pieces (level 1: 3+ points; level 2: the queen).
    const uint64_t fresh = hangingPieces(now, human_, true) & ~hangingPieces(prev, human_, true);
    Square target = NoSquare;
    for (Square t : squaresOf(fresh)) {
        const PieceType tt = now.at(t).type;
        if (level_ == 2 && tt != Queen) continue;
        if (points(tt) < 3) continue;
        if (target == NoSquare || points(tt) > points(now.at(target).type)) target = t;
    }
    if (target == NoSquare) return s;
    // The attacker: the coach's moved piece when it is one of them, else the cheapest.
    const uint64_t attackers = now.attackersTo(target, coach);
    Square att = hasBit(attackers, g.moves().back().to) ? g.moves().back().to : NoSquare;
    if (att == NoSquare)
        for (Square a : squaresOf(attackers))
            if (att == NoSquare || points(now.at(a).type) < points(now.at(att).type)) att = a;
    Beat b = sayBeat(bandKey("threat.piece", level_), Look::Target, ply, Priority::Low);
    b.line.with("my", pieceArg(now, att, human_)).with("your", pieceArg(now, target, human_));
    if (level_ == 1) traceMove(b, now.at(att).type, att, target, "my");
    pointPiece(b, target, "your", true);
    limitPointing(b);
    s.push_back(b);
    noteRemark();
    return s;
}

Script Reviewer::takebackAccepted(const Game& g) {
    Script s;
    if (offered_.ply < 0) return s;
    retryPending_ = true;
    retry_ = offered_;
    if (level_ > 2) return s;
    const int ply = int(g.moves().size());
    const Position& p = g.position();
    const ExType t = offered_.firstType;
    if (t == ExType::MateMissed) {
        s.push_back(sayBeat("tb.hint.mate_missed.b1", Look::Player, ply));
    } else if (t == ExType::MissedCapture || t == ExType::MissedFork) {
        s.push_back(sayBeat("tb.hint.missed.b1", Look::Player, ply));
    } else if (t == ExType::MateAllowed || t == ExType::BackRank || t == ExType::KingSafety) {
        Beat b = sayBeat("tb.hint.king.b1", Look::Target, ply);
        const Square k = p.kingSquare(human_);
        b.line.with("your", pieceArg(p, k, human_));
        pointPiece(b, k, "your");
        s.push_back(b);
    } else if (offeredHint_ != NoSquare && !p.at(offeredHint_).empty() && p.at(offeredHint_).color == human_) {
        Beat b = sayBeat("tb.hint.b1", Look::Target, ply);
        b.line.with("your", pieceArg(p, offeredHint_, human_));
        pointPiece(b, offeredHint_, "your");
        s.push_back(b);
    } else {
        s.push_back(sayBeat("tb.hint.general.b1", Look::Player, ply));
    }
    return s;
}

void Reviewer::takebackDeclined() {
    retryPending_ = false;
}

}  // namespace coach
