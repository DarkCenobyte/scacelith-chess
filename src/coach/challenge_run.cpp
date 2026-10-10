// A challenge run (challenge_run.h).
#include "challenge_run.h"
#include "catalog.h"
#include "../chess/chess.h"
#include "../core/log.h"
#include <algorithm>

namespace coach {
using namespace chess;

namespace {

// Analyses (full strength, through the Analyst): the answer to a wrong move of a line, the judgement
// of a play-out move (with the coach's reply), and the move a play-out hint points at.
constexpr int kAnswerDepth = 16, kAnswerMs = 1200;
constexpr int kPlayDepth = 22, kPlayMs = 1500;
constexpr int kHintDepth = 20, kHintMs = 1200;
// Expected scores (0..1, White's view, Score::expected) for play-outs: below kWinKept the win is gone
// (mate, promote), above kDrawLost for the coach the draw is lost (hold). A line's wrong move that
// keeps at least kStillWinning is "playable, but there is stronger".
constexpr double kWinKept = 0.65, kDrawLost = 0.70, kStillWinning = 0.80;

Line line(const std::string& key) {
    Line l;
    l.key = key;
    return l;
}

Gesture gesture(GestureKind k) {
    Gesture g;
    g.kind = k;
    g.at = 0.3f;
    return g;
}

Beat say(const Line& l, Look look = Look::Player, std::vector<Gesture> g = {}, std::vector<Mark> m = {}) {
    Beat b;
    b.kind = BeatKind::Say;
    b.line = l;
    b.look = look;
    b.gestures = std::move(g);
    b.marks = std::move(m);
    return b;
}

Beat tableBeat(BeatKind kind, const std::string& uci = std::string()) {
    Beat b;
    b.kind = kind;
    b.uci = uci;
    b.look = Look::Board;
    if (kind == BeatKind::Rewind) {
        b.count = 1;
        b.skippable = false;
    }
    return b;
}

Arg moveArg(const Position& p, const std::string& uci) {
    Move m = p.parseUCI(uci);
    return Arg::ofMove(m.valid() ? p.toSAN(m) : uci, uci);
}

// The score of an analysis from White's side.
ai::Score whiteScore(const ai::Analysis& a) {
    ai::Score s = a.lines.empty() ? ai::Score() : a.lines.front().score;
    return a.whiteToMove ? s : s.flipped();
}

}  // namespace

struct ChallengeRun::Impl {
    enum class React { None, Before, Undo };
    enum class Judge { None, Line, PlayOut };

    Challenge ch;
    Director* director = nullptr;
    Stage* stage = nullptr;
    Analyst* analyst = nullptr;
    bool running = false;

    int pos = 0;              // the position played
    int move = 0;             // the player's move in it (lines: index k of line[2k]; play-outs: moves made)
    int failures = 0;         // wrong moves at this move
    int hintStep = 0;         // hints given at this move
    bool leadSaid = false;    // "These positions come from real games" (once)
    int waitTag = -1;         // the expectation of the WaitMove of this move
    Position waitPos;         // the position it waits in

    // A move being judged by the engine (a line's wrong move, any play-out move).
    Judge judge = Judge::None;
    uint32_t job = 0;
    float jobWait = 0.0f;
    Position judged;          // after the player's move, the coach to move
    std::string played;       // the player's move (UCI)
    bool promoted = false;

    // The reaction to a wrong move: said with the move on the board, the move taken back, then 'after'.
    React react = React::None;
    uint64_t reactScript = 0;
    int reactFrames = 0;
    Script reactAfter;

    // Hints.
    uint32_t hintJob = 0;     // play-outs: the analysis of the waiting position
    int hintRetries = 0;      // that analysis came back without a move: asked again (once)
    std::string hintMove;     // the move a hint points at ("" while unknown)
    bool hintWanted = false;  // asked for before the play-out's analysis came back
    uint64_t hintScript = 0;
    bool hintDemo = false;    // that hint shows the move on the board (no move of the player meanwhile)
    bool offer = false;       // the hint offer is queued or shown
    uint64_t offerScript = 0;

    bool completed = false;
    uint64_t closingScript = 0;

    const ChallengePosition& cur() const { return ch.positions[size_t(pos)]; }
    int tag() const { return pos * 256 + std::min(move, 255); }

    void play(const Script& s) { director->play(s); }

    void cancelJobs() {
        if (job) analyst->cancelAnalysis(job);
        if (hintJob) analyst->cancelAnalysis(hintJob);
        job = hintJob = 0;
        judge = Judge::None;
    }

    // ---- Positions --------------------------------------------------------------------------------
    Line taskLine(const ChallengePosition& p) const {
        Line l;
        if (p.playOut()) {
            l.key = std::string("ch.task.play.") + challengeGoalName(p.goal);
            if (p.goal == ChallengeGoal::Hold) l.with("n", Arg::ofNumber(p.moves));
        } else if (p.endsInMate()) {
            l.key = "ch.task.mate";
            l.with("n", Arg::ofNumber(p.playerMoves()));
        } else {
            l.key = "ch.task." + ch.id;
            if (!Catalog::shared().has(l.key)) l.key = "ch.task.default";
        }
        return l;
    }

    Beat waitBeat() {
        Beat b;
        b.kind = BeatKind::WaitMove;
        b.expect = tag();
        b.look = Look::Board;
        waitTag = b.expect;
        return b;
    }

    // The move ahead: its counters, its WaitMove, and for a play-out the analysis a hint needs.
    void beginMove(const Position& p, Script& s) {
        failures = 0;
        hintStep = 0;
        hintWanted = false;
        waitPos = p;
        hintMove.clear();
        if (hintJob) analyst->cancelAnalysis(hintJob);
        hintJob = 0;
        if (!cur().playOut()) {
            if (size_t(2 * move) < cur().line.size()) hintMove = cur().line[size_t(2 * move)];
        } else {
            hintRetries = 0;
            askHintMove();
        }
        s.push_back(waitBeat());
    }

    void askHintMove() {
        ai::AnalysisRequest r;
        r.startFen = waitPos.fen();
        r.multiPV = 1;
        r.depth = kHintDepth;
        r.moveTimeMs = kHintMs;
        hintJob = analyst->analyse(r);
    }

    void startPosition(bool first) {
        move = 0;
        const ChallengePosition& p = cur();
        Script s;
        if (first) {
            Line intro = line("ch.intro." + ch.id);
            intro.with("n", Arg::ofNumber(int(ch.positions.size())));
            s.push_back(say(intro, Look::Player, {gesture(GestureKind::Open)}));
        } else {
            s.push_back(say(line(pos + 1 == int(ch.positions.size()) ? "ch.last" : "ch.next")));
        }
        Beat set;
        set.kind = BeatKind::SetPosition;
        set.fen = p.fen;
        set.look = Look::Board;
        s.push_back(set);
        if (!p.lead.empty()) {
            if (!leadSaid) s.push_back(say(line("ch.lead.first")));
            leadSaid = true;
            s.push_back(tableBeat(BeatKind::PlayMove, p.lead));
        }
        s.push_back(say(taskLine(p), Look::Player, {gesture(GestureKind::Open)}));
        Position start;
        p.start(start);
        beginMove(start, s);
        play(s);
    }

    void solved(const std::string& key) {
        Script s;
        s.push_back(say(line(key), Look::Player, {gesture(GestureKind::Nod)}));
        play(s);
        if (hintJob) analyst->cancelAnalysis(hintJob);
        hintJob = 0;
        hintMove.clear();
        waitTag = -1;
        if (pos + 1 < int(ch.positions.size())) {
            ++pos;
            startPosition(false);
        } else {
            completed = true;
            LOGI("coach challenge %s: completed", ch.id.c_str());
            play(Script{say(line("ch.complete"), Look::Player, {gesture(GestureKind::Nod)})});
            closingScript = director->lastScript();
        }
    }

    // ---- The player's move ------------------------------------------------------------------------
    void onPlayerMove(const chess::Game& game) {
        const size_t n = game.moves().size();
        const Position& before = game.positionAt(n - 1);
        int expect = -1;
        if (react != React::None || judge != Judge::None || (!director->waitingMove(&expect) && !director->jumpToWait()) ||
            !director->waitingMove(&expect) || expect != waitTag) {
            // Not asked for a move (the scene should not let it happen): back it goes, silently.
            LOGW("coach challenge: a move outside a wait, taken back");
            stage->takeBack(1);
            return;
        }
        if (offer) closeOffer();
        played = before.toUCI(game.moves().back());
        judged = game.position();
        promoted = game.moves().back().promotion != NoPiece;
        const ChallengePosition& p = cur();
        if (!p.playOut()) {
            if (p.accepts(move, before, played)) {
                director->endWait();
                ++move;
                if (move >= p.playerMoves() || !judged.hasLegalMove()) {
                    solved(judged.isCheckmate() ? "ch.solved.mate" : "ch.solved");
                    return;
                }
                Script s;
                s.push_back(say(line("ch.good"), Look::Player, {gesture(GestureKind::Nod)}));
                s.push_back(tableBeat(BeatKind::PlayMove, p.line[size_t(2 * move - 1)]));
                Position next = judged;
                Move answer = next.parseUCI(p.line[size_t(2 * move - 1)]);
                if (answer.valid()) next.makeMove(answer);
                beginMove(next, s);
                play(s);
                return;
            }
            if (judged.isStalemate()) {
                wrong(Script{say(line("ch.wrong.stalemate"), Look::Board)});
                return;
            }
            askJudgement(Judge::Line, kAnswerDepth, kAnswerMs);
            return;
        }
        // A play-out: the board settles some moves at once, the engine the others.
        if (judged.isCheckmate()) {
            director->endWait();
            solved("ch.solved.mate");
            return;
        }
        const bool drawn = judged.isStalemate() || judged.hasInsufficientMaterial();
        if (drawn) {
            if (p.goal == ChallengeGoal::Hold) {
                director->endWait();
                solved("ch.solved.draw");
            } else {
                wrong(Script{say(line(judged.isStalemate() ? "ch.wrong.stalemate" : "ch.wrong.plain"), Look::Board)});
            }
            return;
        }
        // The hint's analysis of the position left gives way (its best so far stays the hint's move).
        if (hintJob) analyst->stopAnalysis(hintJob);
        askJudgement(Judge::PlayOut, kPlayDepth, kPlayMs);
    }

    void askJudgement(Judge kind, int depth, int ms) {
        ai::AnalysisRequest r;
        r.startFen = judged.fen();
        r.multiPV = 1;
        r.depth = depth;
        r.moveTimeMs = ms;
        job = analyst->analyse(r);
        jobWait = 0.0f;
        judge = kind;
        if (!job) {   // no engine: the move is simply not the one
            judge = Judge::None;
            wrong(Script{say(line("ch.wrong.plain"), Look::Player)});
        }
    }

    // The coach's answer, demonstrated by hand while 'key' is said ({move}), then taken back.
    Script answerScript(const std::string& key, const std::string& reply) {
        Script s;
        Beat demo = tableBeat(BeatKind::DemoMove, reply);
        demo.line = line(key);
        demo.line.with("move", moveArg(judged, reply));
        s.push_back(demo);
        s.push_back(tableBeat(BeatKind::Rewind));
        return s;
    }

    void judged_(const ai::Analysis& a) {
        const ChallengePosition& p = cur();
        const bool have = a.ok && !a.bestMove.empty() && judged.parseUCI(a.bestMove).valid() && !a.lines.empty();
        const ai::Score w = have ? whiteScore(a) : ai::Score();
        if (judge == Judge::Line) {
            judge = Judge::None;
            if (!have) {
                wrong(Script{say(line("ch.wrong.plain"), Look::Player)});
                return;
            }
            Position after = judged;
            after.makeMove(judged.parseUCI(a.bestMove));
            if (after.isCheckmate()) {
                wrong(answerScript("ch.wrong.mate", a.bestMove));
                return;
            }
            if (w.mate > 0) {
                // Still a forced mate: longer than the position asks, else as fast (a way the audit
                // did not list: true, but not the line the coach answers).
                const bool longer = move + 1 + w.mate > p.playerMoves();
                wrong(Script{say(line(longer ? "ch.wrong.slower" : "ch.wrong.other"), Look::Player)});
                return;
            }
            if (w.expected() >= kStillWinning) {
                wrong(Script{say(line("ch.wrong.weaker"), Look::Player)});
                return;
            }
            wrong(answerScript("ch.wrong.reply", a.bestMove));
            return;
        }
        // A play-out move.
        judge = Judge::None;
        if (!have) {
            wrong(Script{say(line("ch.wrong.plain"), Look::Player)});
            return;
        }
        const double e = w.expected();
        const bool winning = w.mate > 0 || (w.mate == 0 && e >= kWinKept);
        const bool lost = w.mate < 0 || (w.mate == 0 && 1.0 - e >= kDrawLost);
        switch (p.goal) {
        case ChallengeGoal::Mate:
        case ChallengeGoal::Promote:
            if (!winning) {
                wrong(answerScript(lost ? "ch.wrong.loses" : "ch.wrong.draw", a.bestMove));
                return;
            }
            if (p.goal == ChallengeGoal::Promote && promoted) {
                director->endWait();
                solved("ch.solved.promote");
                return;
            }
            break;
        case ChallengeGoal::Hold:
            if (lost) {
                wrong(answerScript("ch.wrong.loses", a.bestMove));
                return;
            }
            if (move + 1 >= p.moves) {
                director->endWait();
                ++move;
                solved("ch.solved.hold");
                return;
            }
            break;
        case ChallengeGoal::Line: break;
        }
        // Kept: the coach answers with the engine's move, and the next move waits.
        director->endWait();
        ++move;
        Script s;
        s.push_back(tableBeat(BeatKind::PlayMove, a.bestMove));
        Position next = judged;
        next.makeMove(next.parseUCI(a.bestMove));
        beginMove(next, s);
        play(s);
    }

    // A wrong move: 'before' with the move on the board, the move taken back, then the same wait,
    // with the hint offer after every kHintOfferAfter wrong moves at this move.
    void wrong(const Script& before) {
        ++failures;
        LOGI("coach challenge %s, position %d: %s is not the move (%d)", ch.id.c_str(), pos + 1, played.c_str(), failures);
        reactScript = 0;
        if (!before.empty()) {
            director->playNext(before);
            reactScript = director->lastScript();
        }
        react = React::Before;
        reactAfter.clear();
        if (failures % kHintOfferAfter == 0 && hintStep < hintSteps()) {
            Beat o;
            o.kind = BeatKind::OfferTakeback;
            o.line = line("ch.offer");
            o.look = Look::Player;
            o.gestures = {gesture(GestureKind::Open)};
            reactAfter.push_back(o);
        } else {
            reactAfter.push_back(say(line("ch.try_again"), Look::Player));
        }
    }

    // The hints this move has: the offer stops once the move has been shown (H still shows it again).
    int hintSteps() const {
        if (hintMove.empty()) return cur().playOut() ? 3 : 0;   // a play-out's move is not known yet
        return challengeHintSteps(waitPos, hintMove);
    }

    void updateReaction() {
        if (react == React::Before && !director->pending(reactScript)) {
            stage->takeBack(1);
            react = React::Undo;
            reactFrames = 0;
        } else if (react == React::Undo && ++reactFrames > 1 && !stage->tableBusy()) {
            react = React::None;
            const bool offers = !reactAfter.empty() && reactAfter.front().kind == BeatKind::OfferTakeback;
            director->playNext(reactAfter);
            if (offers) {
                offer = true;
                offerScript = director->lastScript();
            }
            reactAfter.clear();
        }
    }

    void closeOffer() {
        director->closeOffer();
        offer = false;
    }

    // ---- Hints ----------------------------------------------------------------------------------------
    void giveHint() {
        hintWanted = false;
        if (hintMove.empty()) return;
        ++hintStep;
        const ChallengeHint h = challengeHint(waitPos, hintMove, hintStep);
        Script s;
        switch (h.kind) {
        case ChallengeHint::Kind::Piece: {
            Line l = line("ch.hint.piece");
            const Piece pc = waitPos.at(h.piece);
            l.with("your", Arg::ofPiece(pc.type, pc.color, true, h.piece));
            Gesture g;
            g.kind = GestureKind::PointPiece;
            g.square = h.piece;
            g.anchor = "your";
            Mark m;
            m.kind = Mark::Kind::Piece;
            m.square = h.piece;
            m.anchor = "your";
            s.push_back(say(l, Look::Target, {g}, {m}));
            break;
        }
        case ChallengeHint::Kind::Square: {
            Line l = line("ch.hint.square");
            l.with("sq", Arg::ofSquare(h.square));
            Gesture g;
            g.kind = GestureKind::PointSquare;
            g.square = h.square;
            g.anchor = "sq";
            Mark m;
            m.kind = Mark::Kind::Square;
            m.square = h.square;
            m.anchor = "sq";
            s.push_back(say(l, Look::Target, {g}, {m}));
            break;
        }
        case ChallengeHint::Kind::Show:
            s.push_back(say(line("ch.hint.show"), Look::Board));
            s.push_back(tableBeat(BeatKind::DemoMove, hintMove));
            s.push_back(tableBeat(BeatKind::Rewind));
            s.push_back(say(line("ch.hint.now"), Look::Player, {gesture(GestureKind::Open)}));
            break;
        case ChallengeHint::Kind::None: return;
        }
        LOGI("coach challenge %s, position %d: hint %d (%s)", ch.id.c_str(), pos + 1, hintStep, hintMove.c_str());
        play(s);
        hintScript = director->lastScript();
        hintDemo = h.kind == ChallengeHint::Kind::Show;
    }

    bool waitingHere() const {
        int expect = -1;
        return running && !completed && director->waitingMove(&expect) && expect == waitTag;
    }
};

ChallengeRun::ChallengeRun() : d_(new Impl) {}
ChallengeRun::~ChallengeRun() = default;

void ChallengeRun::start(const Challenge& challenge, Director& director, Stage& stage, Analyst& analyst, int from) {
    stop();
    Impl& d = *d_;
    d = Impl();
    d.ch = challenge;
    d.director = &director;
    d.stage = &stage;
    d.analyst = &analyst;
    d.running = !d.ch.positions.empty();
    if (!d.running) return;
    d.pos = std::clamp(from, 0, int(d.ch.positions.size()) - 1);
    LOGI("coach challenge %s: %d positions (from %d)", d.ch.id.c_str(), int(d.ch.positions.size()), d.pos + 1);
    d.startPosition(true);
}

void ChallengeRun::stop() {
    Impl& d = *d_;
    if (!d.running) return;
    d.cancelJobs();
    d.running = false;
}

void ChallengeRun::update(const chess::Game&, float dt) {
    Impl& d = *d_;
    if (!d.running) return;
    d.updateReaction();
    if (d.job) {
        ai::Analysis a;
        d.jobWait += dt;
        if (d.analyst->takeAnalysis(d.job, a)) {
            d.job = 0;
            d.judged_(a);
        } else if (d.jobWait > kChallengeAnswerWait) {
            d.analyst->stopAnalysis(d.job);   // its best so far comes back on a following update
            d.jobWait = -1e9f;
        }
    }
    if (d.hintJob) {
        ai::Analysis a;
        if (d.analyst->takeAnalysis(d.hintJob, a)) {
            d.hintJob = 0;
            if (a.ok && !a.bestMove.empty() && d.waitPos.parseUCI(a.bestMove).valid()) d.hintMove = a.bestMove;
            else if (d.hintRetries++ < 1) d.askHintMove();   // stopped before it began: once more
            if (d.hintWanted && !d.hintMove.empty() && d.waitingHere()) d.giveHint();
            if (!d.hintJob && d.hintMove.empty()) d.hintWanted = false;   // no move to hint at
        }
    }
    // The offer was dropped (skipped, cleared): no card any more.
    if (d.offer && !d.director->pending(d.offerScript)) d.offer = false;
}

void ChallengeRun::onMove(const chess::Game& game) {
    Impl& d = *d_;
    const size_t n = game.moves().size();
    if (!d.running || d.completed || n == 0) return;
    if (game.positionAt(n - 1).sideToMove() == White) {
        d.onPlayerMove(game);
        return;
    }
    // The coach's own move (the lead, an answer): a play-out drawn on the board after it is held.
    const ChallengePosition& p = d.cur();
    const Position& now = game.position();
    if (p.goal == ChallengeGoal::Hold && (now.isStalemate() || now.hasInsufficientMaterial()) && d.waitingHere()) {
        d.director->endWait();
        d.solved("ch.solved.draw");
    }
}

void ChallengeRun::onPlayerActive() {
    if (d_->offer) answerOffer(false);
}

void ChallengeRun::answerOffer(bool accept) {
    Impl& d = *d_;
    if (!d.offer) return;
    d.closeOffer();
    if (accept) {
        d.hintWanted = true;
        if (!d.hintMove.empty()) d.giveHint();
    } else {
        d.play(Script{say(line("ch.no_hint"), Look::Player)});
    }
}

void ChallengeRun::requestHint(const chess::Game& game) {
    Impl& d = *d_;
    if (!hintAvailable(game)) return;
    if (d.offer) d.closeOffer();
    if (d.hintMove.empty()) {
        d.hintWanted = true;   // a play-out's analysis is on its way
        return;
    }
    d.giveHint();
}

bool ChallengeRun::playerMayMove(const chess::Game& game) const {
    const Impl& d = *d_;
    return d.waitingHere() && d.react == Impl::React::None && d.judge == Impl::Judge::None &&
           game.position().sideToMove() == White && !d.director->offerOpen() &&
           !(d.hintDemo && d.director->pending(d.hintScript));
}

bool ChallengeRun::hintAvailable(const chess::Game& game) const {
    const Impl& d = *d_;
    if (!d.waitingHere() || d.react != Impl::React::None || d.judge != Impl::Judge::None || d.hintWanted) return false;
    if (game.position().sideToMove() != White || !game.position().samePosition(d.waitPos)) return false;
    if (d.director->pending(d.hintScript)) return false;
    return !d.hintMove.empty() || d.hintJob != 0;
}

bool ChallengeRun::offerOpen() const { return d_->offer && d_->director->offerOpen(); }
bool ChallengeRun::completed() const { return d_->completed; }
bool ChallengeRun::finished() const { return d_->completed && !d_->director->pending(d_->closingScript); }
int ChallengeRun::position() const { return d_->pos; }
int ChallengeRun::positions() const { return int(d_->ch.positions.size()); }
const Challenge& ChallengeRun::challenge() const { return d_->ch; }

}  // namespace coach
