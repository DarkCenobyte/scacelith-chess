// A Coach-mode game or the rules lesson, seen from the coach (session.h). The flows follow the
// integrator notes of the brain's headers: coach/review.h, coach/appraisal.h, coach/openings.h,
// coach/lesson.h and coach/events.h.
//
// Analyses (all through the Analyst, full strength):
//   A0  when the human's turn begins: MultiPV 3 of the position (cached by FEN, so the retry after a
//       takeback reuses it); A3 with it at levels 3-4. Stopped (finished early) when the human moves.
//   A1  after the human's move, when A0 does not hold the played move; A2 at levels 4-6.
//   evaluations of the positions the appraisal still lacks, in the background when the Analyst is
//       idle and no review waits.
// The review holds the coach's move (coachMayMove) from the human's move until its script is over
// (an offer: until it is answered).
#include "session.h"
#include "catalog.h"
#include "events.h"
#include "lesson.h"
#include "openings.h"
#include "review.h"
#include "../core/log.h"
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

namespace coach {
namespace {

constexpr float kA0Wait = 3.0f;           // the review waits this long for a stopped A0
constexpr float kA12Wait = 4.0f;          // ... and for A1 / A2
constexpr float kCoachRemarkWait = 1.5f;  // the coach's remarks wait this long for A0
constexpr float kTakeYourTime = 60.0f;    // "Take your time." after this long without a move (levels 1-3)
constexpr float kFiller = 6.0f;           // a filler once the engine holds the coach's move this long
constexpr int kMistakeEncourageGap = 6;   // human moves between two "after a mistake" encouragements
constexpr int kBehindCp = -300;           // "behind": below -3 pawns for the player...
constexpr int kBehindMoves = 3;           // ... for this many human moves in a row (once per game)
constexpr int kWellMoves = 4;             // "playing well": this many Best/Excellent moves in a row...
constexpr int kWellGap = 8;               // ... spaced by this many human moves
constexpr size_t kCacheRoots = 32;
const char* const kAccuracyExplain = "appraisal.num.acc_explain.b3";

// The opening texts (OpeningTexts) resolve the opening names the catalog cannot compose itself
// (line: references), and give the spoken forms of every name.
void installOpeningResolver() {
    static std::once_flag once;
    std::call_once(once, [] {
        Catalog::shared().setOpeningResolver(
            [](const std::string& ref, const std::string& form, const std::string& lang, bool spoken) {
                return OpeningTexts::instance().arg(ref, form, lang, spoken);
            });
    });
}

Script openingScript(const std::vector<OpeningLine>& lines, int ply) {
    Script s;
    for (const OpeningLine& ol : lines) {
        Beat b;
        b.kind = BeatKind::Say;
        b.line.key = ol.key;
        for (const auto& a : ol.args)
            b.line.with(a.first, a.second.kind == OpeningArg::Kind::Number ? Arg::ofNumber(a.second.number)
                                                                         : Arg::ofOpening(a.second.text));
        b.look = Look::Board;
        b.priority = Priority::Normal;
        b.ply = ply;
        s.push_back(b);
    }
    return s;
}

bool hasKey(const Script& s, const std::string& key) {
    for (const Beat& b : s)
        if (b.line.key == key) return true;
    return false;
}

// An explanation of a fault (not a praise, a tip or a retry line) without a takeback offer.
bool explainsWithoutOffer(const Review& r) {
    bool explained = false;
    for (const Beat& b : r.script) {
        if (b.kind == BeatKind::OfferTakeback) return false;
        if (b.kind == BeatKind::Say && b.priority == Priority::Normal && b.line.key.rfind("ex.", 0) == 0) explained = true;
    }
    return explained;
}

void append(Script& s, const Script& more) { s.insert(s.end(), more.begin(), more.end()); }

}  // namespace

struct Session::Impl {
    enum class JobKind { A0, A1, A2, A3, Eval };
    struct Job {
        uint32_t id = 0;
        JobKind kind = JobKind::A0;
        std::string fen;      // analysed position
        size_t plies = 0;     // its ply in the game
    };
    enum class ReviewState { None, WaitA0, WaitA12, Playing };
    enum class React { None, Before, Undo };

    // Everything that lives for one game (start() resets it).
    struct Game {
        bool started = false;
        int level = 1;
        chess::Color human = chess::White;
        std::vector<Job> jobs;
        std::map<std::string, ai::Analysis> a0, a3;   // by root FEN
        std::vector<std::string> roots;                // cache order
        // The human's turn.
        int turnPly = -1;             // plies when the current human turn began (-1: none)
        float turnTime = 0.0f;
        bool tookTime = false;
        // The coach's last move: its remarks wait for A0.
        int coachMovedPly = -1;
        float coachMovedWait = 0.0f;
        bool yourMoveAfterCoach = false;
        // The review of the human's move.
        ReviewState review = ReviewState::None;
        int reviewPly = -1;
        std::string reviewRoot, playedUci;
        float reviewWait = 0.0f;
        bool a1Asked = false, a2Asked = false;
        ai::Analysis a1, a2;
        bool a1Done = false, a2Done = false;
        uint64_t reviewScript = 0;
        MoveClass lastClass = MoveClass::Unjudged;
        int offerPly = -1;            // the human ply the last offer was about
        // Encouragement.
        int lastMistakeEncourage = -100;
        int behindRun = 0;
        bool behindSaid = false;
        int wellRun = 0;
        int lastWell = -100;
        // Openings.
        bool announcerWaiting = false;
        // The engine thinking on the coach's move.
        bool thinking = false;
        float lastThinking = 0.0f;
        float engineHold = 0.0f;
        bool fillerSaid = false;
        // Greeting and opening names: what is still queued of them is dropped at game over.
        std::vector<uint64_t> chatScripts;
        // The end.
        bool over = false, resigned = false, endPending = false;
        GameEnd end = GameEnd::Draw;
        uint64_t endScript = 0;
        bool handshakeDone = false;
        uint64_t appraisalScript = 0;
        bool explainPending = false;  // the appraisal explains accuracy: accuracyExplained once it is said
        bool done = false;            // finished()
        int suggested = 0;
        // The lesson.
        int chapter = 0;
        uint64_t chapterScript = 0;
        bool chapterStarted = false;
        bool lessonDone = false;
        int failures = 0;
        int waitExpect = -1;
        float idle = 0.0f;
        int hintStage = 0;
        React react = React::None;
        uint64_t reactScript = 0;
        int reactUndo = 0;
        Script reactAfter;
        int reactFrames = 0;
        uint64_t illegalScript = 0;
        int takebackTo = -1;          // a takeback asked of the stage: the game goes back to this many plies
    };

    SessionConfig config;
    Director director;
    Stage* stage = nullptr;
    Analyst* analyst = nullptr;
    Reviewer reviewer;
    Appraisal appraisal;
    OpeningAnnouncer announcer;
    std::unique_ptr<Lesson> lesson;
    std::vector<GameRecord> history;
    bool accuracyExplained = false;
    Game g;

    // ---- Analyses -------------------------------------------------------------------------------------
    bool hasJob(JobKind k, const std::string& fen) const {
        for (const Job& j : g.jobs)
            if (j.kind == k && j.fen == fen) return true;
        return false;
    }
    bool hasJob(JobKind k) const {
        for (const Job& j : g.jobs)
            if (j.kind == k) return true;
        return false;
    }

    bool ask(JobKind kind, const ai::AnalysisRequest& r, const std::string& fen, size_t plies) {
        const uint32_t id = analyst->analyse(r);
        if (!id) return false;
        g.jobs.push_back(Job{id, kind, fen, plies});
        return true;
    }

    void remember(std::map<std::string, ai::Analysis>& cache, const std::string& fen, const ai::Analysis& a) {
        cache[fen] = a;
        g.roots.push_back(fen);
        if (g.roots.size() > kCacheRoots) {
            const std::string old = g.roots.front();
            g.roots.erase(g.roots.begin());
            if (std::find(g.roots.begin(), g.roots.end(), old) == g.roots.end()) {
                g.a0.erase(old);
                g.a3.erase(old);
            }
        }
    }

    const ai::Analysis* cachedA0(const std::string& fen) const {
        auto it = g.a0.find(fen);
        return it == g.a0.end() ? nullptr : &it->second;
    }

    void stopJobs(JobKind k, const std::string& fen) {
        for (const Job& j : g.jobs)
            if (j.kind == k && j.fen == fen) analyst->stopAnalysis(j.id);
    }

    // The game is back at 'plies' moves: analyses of positions that are gone are cancelled.
    void cancelBeyond(const chess::Game& game, size_t plies) {
        for (auto it = g.jobs.begin(); it != g.jobs.end();) {
            const bool gone = it->kind == JobKind::A1 || it->kind == JobKind::A2 || it->plies > plies ||
                              it->plies > game.moves().size() || game.positionAt(it->plies).fen() != it->fen;
            if (gone) {
                analyst->cancelAnalysis(it->id);
                it = g.jobs.erase(it);
            } else {
                ++it;
            }
        }
    }

    void pollJobs(const chess::Game& game) {
        for (size_t i = 0; i < g.jobs.size();) {
            ai::Analysis a;
            if (!analyst->takeAnalysis(g.jobs[i].id, a)) {
                ++i;
                continue;
            }
            const Job j = g.jobs[i];
            g.jobs.erase(g.jobs.begin() + long(i));
            // Still about the game on the table?
            const bool current = j.plies <= game.moves().size() && game.positionAt(j.plies).fen() == j.fen;
            switch (j.kind) {
            case JobKind::A0:
                remember(g.a0, j.fen, a);
                if (current && a.ok) appraisal.addEval(j.plies, a);
                break;
            case JobKind::A3: remember(g.a3, j.fen, a); break;
            case JobKind::A1:
                if (int(j.plies) == g.reviewPly && g.review == ReviewState::WaitA12) {
                    g.a1 = a;
                    g.a1Done = true;
                }
                break;
            case JobKind::A2:
                if (int(j.plies) == g.reviewPly + 1 && g.review == ReviewState::WaitA12) {
                    g.a2 = a;
                    g.a2Done = true;
                }
                break;
            case JobKind::Eval:
                if (current && a.ok) appraisal.addEval(j.plies, a);
                break;
            }
        }
    }

    // ---- The game (levels 1-6) ------------------------------------------------------------------------
    void beginHumanTurn(const chess::Game& game) {
        const size_t n = game.moves().size();
        g.turnPly = int(n);
        g.turnTime = 0.0f;
        g.tookTime = false;
        const std::string fen = game.position().fen();
        if (!game.position().hasLegalMove()) return;
        if (!cachedA0(fen) && !hasJob(JobKind::A0, fen) && !ask(JobKind::A0, reviewer.beforeRequest(game), fen, n)) {
            ai::Analysis none;   // no engine: the review is rules-based
            remember(g.a0, fen, none);
        }
        if ((g.level == 3 || g.level == 4) && !g.a3.count(fen) && !hasJob(JobKind::A3, fen))
            ask(JobKind::A3, reviewer.shallowRequest(game), fen, n);
    }

    void play(const Script& s) { director.play(s); }

    void announceOpenings(const chess::Game& game, bool canSpeak) {
        const std::vector<OpeningLine> lines = announcer.update(game, canSpeak);
        g.announcerWaiting = !canSpeak;
        if (lines.empty()) return;
        play(openingScript(lines, int(game.moves().size()) - 1));
        g.chatScripts.push_back(director.lastScript());
    }

    void onMove(const chess::Game& game) {
        const size_t n = game.moves().size();
        if (g.over || n == 0) return;
        play(reviewer.announce(game));
        const chess::Color mover = game.positionAt(n - 1).sideToMove();
        if (mover == g.human) {
            // The review of this move: A0 of its root (stopped now if still searching), then A1/A2.
            g.review = ReviewState::WaitA0;
            g.reviewPly = int(n) - 1;
            g.reviewRoot = game.positionAt(n - 1).fen();
            g.playedUci = game.positionAt(n - 1).toUCI(game.moves().back());
            g.reviewWait = 0.0f;
            g.a1Asked = g.a2Asked = g.a1Done = g.a2Done = false;
            g.a1 = g.a2 = ai::Analysis();
            stopJobs(JobKind::A0, g.reviewRoot);
            stopJobs(JobKind::A3, g.reviewRoot);
            g.turnPly = -1;
            g.engineHold = 0.0f;
            g.fillerSaid = false;
            g.coachMovedPly = -1;
        } else {
            g.coachMovedPly = int(n) - 1;
            g.coachMovedWait = 0.0f;
            g.engineHold = 0.0f;
            g.fillerSaid = false;
            if (g.yourMoveAfterCoach && !game.isOver()) play(yourMoveScript(int(n) - 1));
            g.yourMoveAfterCoach = false;
        }
        announceOpenings(game, director.idle() && g.review == ReviewState::None);
    }

    void buildReview(const chess::Game& game) {
        const ai::Analysis* a0 = cachedA0(g.reviewRoot);
        auto a3 = g.a3.find(g.reviewRoot);
        ReviewInput in;
        in.game = &game;
        in.before = a0;
        in.played = g.a1Done ? &g.a1 : nullptr;
        in.after = g.a2Done ? &g.a2 : nullptr;
        in.shallow = a3 != g.a3.end() ? &a3->second : nullptr;
        in.inBook = OpeningBook::instance().lookup(game.position().hash());
        Review r = reviewer.review(in);
        appraisal.add(r);
        g.lastClass = r.verdict.cls;
        if (r.offersTakeback) g.offerPly = r.verdict.ply;
        g.yourMoveAfterCoach = explainsWithoutOffer(r);
        const int ply = r.verdict.ply;
        g.review = ReviewState::None;
        if (!r.script.empty()) {
            play(r.script);
            g.reviewScript = director.lastScript();
            g.review = ReviewState::Playing;
        }

        // Encouragement (events.h).
        const int moves = reviewer.humanMoves();
        if (r.verdict.hasEvalAfter) {
            const int cp = g.human == chess::White ? r.verdict.cpWhiteAfter : -r.verdict.cpWhiteAfter;
            g.behindRun = cp < kBehindCp ? g.behindRun + 1 : 0;
            if (g.behindRun >= kBehindMoves && !g.behindSaid) {
                play(encouragementScript(Encouragement::Behind, ply));
                g.behindSaid = true;
            }
        }
        switch (r.verdict.cls) {
        case MoveClass::Best:
        case MoveClass::Excellent: ++g.wellRun; break;
        case MoveClass::Book:
        case MoveClass::Forced:
        case MoveClass::Unjudged: break;
        default: g.wellRun = 0; break;
        }
        if (g.wellRun >= kWellMoves && moves - g.lastWell >= kWellGap && r.script.empty()) {
            play(encouragementScript(Encouragement::PlayingWell, ply));
            g.lastWell = moves;
            g.wellRun = 0;
        }
    }

    void updateReview(const chess::Game& game, float dt) {
        if (g.review == ReviewState::None) return;
        if (g.review != ReviewState::Playing && int(game.moves().size()) != g.reviewPly + 1) {
            g.review = ReviewState::None;   // the move is gone (taken back meanwhile)
            return;
        }
        g.reviewWait += dt;
        if (g.review == ReviewState::WaitA0) {
            const ai::Analysis* a0 = cachedA0(g.reviewRoot);
            if (!a0 && hasJob(JobKind::A0, g.reviewRoot) && g.reviewWait < kA0Wait) return;
            if (!a0 && !hasJob(JobKind::A0, g.reviewRoot) && g.reviewWait <= dt) {
                // The move came before the turn's A0 was asked for (a resumed game): ask now.
                chess::Game root = game;
                root.undo(1);
                if (ask(JobKind::A0, reviewer.beforeRequest(root), g.reviewRoot, size_t(g.reviewPly))) return;
            }
            if (a0 && a0->ok) {
                if (Reviewer::needsPlayedRequest(*a0, g.playedUci))
                    g.a1Asked = ask(JobKind::A1, reviewer.playedRequest(game, *a0), g.reviewRoot, size_t(g.reviewPly));
                if (g.level >= 4)
                    g.a2Asked = ask(JobKind::A2, reviewer.afterRequest(game), game.position().fen(), game.moves().size());
            }
            g.review = ReviewState::WaitA12;
            g.reviewWait = 0.0f;
        }
        if (g.review == ReviewState::WaitA12) {
            const bool waitA1 = g.a1Asked && !g.a1Done, waitA2 = g.a2Asked && !g.a2Done;
            if ((waitA1 || waitA2) && g.reviewWait < kA12Wait) return;
            for (auto it = g.jobs.begin(); it != g.jobs.end();) {   // late ones are no use now
                if (it->kind == JobKind::A1 || it->kind == JobKind::A2) {
                    analyst->cancelAnalysis(it->id);
                    it = g.jobs.erase(it);
                } else {
                    ++it;
                }
            }
            buildReview(game);
            return;
        }
        if (g.review == ReviewState::Playing && !director.pending(g.reviewScript)) g.review = ReviewState::None;
    }

    void answerOffer(const chess::Game* game, bool accept, bool playedOn) {
        if (g.level == 0 || !director.offerOpen()) return;
        director.closeOffer();
        const int ply = g.offerPly;
        if (g.over || (game && game->isOver())) accept = false;   // nothing to take back in a finished game
        if (accept && game) {
            const size_t n = game->moves().size();
            // Back to the position before the human's move (the coach's reply was held).
            director.clear();
            stage->takeBack(1);
            g.takebackTo = int(n) - 1;
            chess::Game after = *game;
            if (after.moves().size() == n && n > 0) after.undo(1);
            appraisal.truncate(after.moves().size());
            cancelBeyond(after, after.moves().size());
            g.review = ReviewState::None;
            g.yourMoveAfterCoach = false;
            Script s = takebackScript(true, ply);
            append(s, reviewer.takebackAccepted(after));
            g.offerPly = -1;   // the offered move is gone: a later takeback at this ply is of the retry
            play(s);
            g.turnPly = -1;   // the human's turn again: A0 comes from the cache
        } else {
            reviewer.takebackDeclined();
            Script s = playedOn ? playOnScript(ply) : takebackScript(false, ply);
            const int moves = reviewer.humanMoves();
            if ((g.lastClass == MoveClass::Mistake || g.lastClass == MoveClass::Blunder) &&
                moves - g.lastMistakeEncourage >= kMistakeEncourageGap) {
                append(s, encouragementScript(Encouragement::AfterMistake, ply));
                g.lastMistakeEncourage = moves;
            }
            play(s);
        }
    }

    // The human's last move in the game (-1: none).
    int lastHumanPly(const chess::Game& game) const {
        for (int i = int(game.moves().size()) - 1; i >= 0; --i)
            if (game.positionAt(size_t(i)).sideToMove() == g.human) return i;
        return -1;
    }

    void takenBack(const chess::Game& game) {
        director.clear();
        const size_t n = game.moves().size();
        appraisal.truncate(n);
        cancelBeyond(game, n);
        g.review = ReviewState::None;
        g.coachMovedPly = -1;
        g.turnPly = -1;
        g.yourMoveAfterCoach = false;
        if (g.offerPly >= int(n)) g.offerPly = -1;
    }

    void backgroundEvals(const chess::Game& game) {
        if (hasJob(JobKind::Eval) || !analyst->idle() || g.review != ReviewState::None) return;
        if (g.handshakeDone) return;
        if (g.takebackTo >= 0) return;   // not of plies a takeback asked of the stage is about to undo
        const bool humanTurn = game.position().sideToMove() == g.human;
        for (size_t k : appraisal.missingEvals(game)) {
            if (k == game.moves().size() && humanTurn && !g.over) continue;   // A0 gives it
            ask(JobKind::Eval, appraisal.evalRequest(game, k), game.positionAt(k).fen(), k);
            return;
        }
    }

    void updateGame(const chess::Game& game, float dt) {
        pollJobs(game);
        const size_t n = game.moves().size();
        if (g.takebackTo >= 0 && int(n) <= g.takebackTo) g.takebackTo = -1;
        const bool humanTurn = game.position().sideToMove() == g.human;
        // Not on a position a takeback asked of the stage is about to undo.
        if (!g.over && humanTurn && g.review == ReviewState::None && g.turnPly != int(n) && !game.isOver() &&
            g.takebackTo < 0)
            beginHumanTurn(game);

        // The coach's remarks on its own move (threats), once that turn's A0 is in.
        if (g.coachMovedPly >= 0) {
            g.coachMovedWait += dt;
            const ai::Analysis* a0 = int(n) == g.coachMovedPly + 1 ? cachedA0(game.position().fen()) : nullptr;
            if (int(n) != g.coachMovedPly + 1 || g.over) {
                g.coachMovedPly = -1;
            } else if (a0 || g.coachMovedWait >= kCoachRemarkWait) {
                play(reviewer.coachMoved(game, a0 && a0->ok ? a0 : nullptr));
                g.coachMovedPly = -1;
            }
        }

        updateReview(game, dt);

        if (g.announcerWaiting && director.idle() && g.review == ReviewState::None && !g.over)
            announceOpenings(game, true);

        backgroundEvals(game);

        // Turn-taking lines.
        if (!g.over && humanTurn && g.turnPly == int(n)) {
            g.turnTime += dt;
            if (g.level <= 3 && !g.tookTime && g.turnTime >= kTakeYourTime && director.idle()) {
                play(takeYourTimeScript(int(n) - 1));
                g.tookTime = true;
            }
        }
        if (!g.over && !humanTurn && g.thinking && coachMayMove()) {
            g.engineHold += dt;
            if (!g.fillerSaid && g.engineHold >= kFiller && director.idle()) {
                play(fillerScript(int(n) - 1));
                g.fillerSaid = true;
            }
        }

        // The end: closing words once the last review is over, then the handshake.
        if (g.endPending && g.review == ReviewState::None) {
            g.endPending = false;
            play(gameEndScript(g.end));
            g.endScript = director.lastScript();
        }
        // Accuracy is explained once that line is said (Space may skip the appraisal before it).
        const std::string* said = director.runningKey();
        if (g.explainPending && said && *said == kAccuracyExplain) {
            accuracyExplained = true;
            g.explainPending = false;
        }
    }

    bool coachMayMove() const {
        if (g.level == 0) return true;
        return g.review == ReviewState::None && !director.offerOpen() && g.takebackTo < 0;
    }

    void gameOver(const chess::Game& game, bool humanResigned) {
        if (g.over || g.level == 0) return;
        g.over = true;
        g.resigned = humanResigned;
        if (humanResigned) {   // no more explanations of a game the player gave up
            director.clear();
            g.review = ReviewState::None;
        }
        // An offer open or still queued (a draw agreed or claimed meanwhile): nothing to take back
        // any more, the card closes without a word.
        if (director.closeOffer()) reviewer.takebackDeclined();
        for (uint64_t s : g.chatScripts) director.dropQueued(s);   // "You play White" after the mate
        g.chatScripts.clear();
        g.coachMovedPly = -1;
        g.announcerWaiting = false;
        // No human turn follows: its analyses go, except the review's own input (the A0 / A3 of the
        // last move's root, stopped when it was played). A resignation abandons the review: A1 / A2 too.
        const bool reviewing = g.review == ReviewState::WaitA0 || g.review == ReviewState::WaitA12;
        for (auto it = g.jobs.begin(); it != g.jobs.end();) {
            const bool turn = it->kind == JobKind::A0 || it->kind == JobKind::A3;
            const bool review = it->kind == JobKind::A1 || it->kind == JobKind::A2;
            if ((turn && !(reviewing && it->fen == g.reviewRoot)) || (review && humanResigned)) {
                analyst->cancelAnalysis(it->id);
                it = g.jobs.erase(it);
            } else {
                ++it;
            }
        }
        if (humanResigned) {
            g.end = GameEnd::Resigned;
        } else {
            switch (game.status()) {
            case chess::GameStatus::WhiteWins: g.end = g.human == chess::White ? GameEnd::Win : GameEnd::Loss; break;
            case chess::GameStatus::BlackWins: g.end = g.human == chess::Black ? GameEnd::Win : GameEnd::Loss; break;
            default: g.end = GameEnd::Draw; break;
            }
        }
        g.endPending = true;
    }

    std::string openingRef(const chess::Game& game, int* outOfBook) const {
        *outOfBook = 0;
        const OpeningBook& book = OpeningBook::instance();
        const OpeningState st = classify(game, book);
        if (!st.standardStart) return std::string();
        if (st.leftBookPly > 0) *outOfBook = (st.leftBookPly + 1) / 2;
        if (st.family < 0 || size_t(st.family) >= book.families().size()) return std::string();
        const OpeningFamily& f = book.families()[size_t(st.family)];
        return f.generic ? std::string() : "family:" + f.id;
    }

    void handshakeDone(const chess::Game& game) {
        if (g.handshakeDone) return;
        g.handshakeDone = true;
        if (g.level == 0) {
            g.done = true;
            return;
        }
        for (const Job& j : g.jobs) analyst->cancelAnalysis(j.id);
        g.jobs.clear();
        const AppraisalStats st = appraisal.stats(game);
        history.push_back(GameRecord{g.level, st.result, st.human.accuracy});
        const int sug = suggestLevel(g.level, history);
        g.suggested = sug != g.level ? sug : 0;
        AppraisalContext ctx;
        ctx.humanResigned = g.resigned;
        ctx.explainAccuracy = !accuracyExplained;
        ctx.opening = openingRef(game, &ctx.outOfBookMove);
        ctx.suggestedLevel = sug;
        const Script s = appraisal.script(game, ctx);
        g.explainPending = hasKey(s, kAccuracyExplain);
        if (s.empty()) {
            g.done = true;
            return;
        }
        play(s);
        g.appraisalScript = director.lastScript();
    }

    // ---- The rules lesson (level 0) ---------------------------------------------------------------------
    void startChapter(int c) {
        const auto& chapters = lesson->chapters();
        g.chapter = c;
        g.failures = 0;
        g.waitExpect = -1;
        g.hintStage = 0;
        g.idle = 0.0f;
        play(chapters[size_t(c)].beats);
        g.chapterScript = director.lastScript();
        g.chapterStarted = true;
        if (size_t(c) + 1 < chapters.size()) director.prefetch(lesson->chapterLines(c + 1));
    }

    void lessonMove(const chess::Game& game) {
        const size_t n = game.moves().size();
        if (n == 0) return;
        const chess::Position& before = game.positionAt(n - 1);
        if (before.sideToMove() != chess::White) return;   // the coach's own lesson move
        int expect = -1;
        if (g.react != React::None || (!director.waitingMove(&expect) && !director.jumpToWait()) ||
            !director.waitingMove(&expect)) {
            // Not asked for a move (the scene should not let it happen): back it goes, silently.
            LOGW("coach lesson: a move outside an exercise, taken back");
            stage->takeBack(1);
            return;
        }
        g.idle = 0.0f;
        const LessonReaction r = lesson->judge(expect, before, game.moves().back(), g.failures);
        if (r.accepted) {
            g.failures = 0;
            g.hintStage = 0;
            director.playNext(r.before);
            director.endWait();
            return;
        }
        ++g.failures;
        g.reactScript = 0;
        if (!r.before.empty()) {
            director.playNext(r.before);
            g.reactScript = director.lastScript();
        }
        g.react = React::Before;
        g.reactUndo = r.undo;
        g.reactAfter = r.after;
    }

    void updateLesson(const chess::Game&, float dt) {
        // A wrong move: its answer, the move back by hand, then the help, and the same wait.
        if (g.react == React::Before && !director.pending(g.reactScript)) {
            if (g.reactUndo > 0) {
                stage->takeBack(g.reactUndo);
                g.react = React::Undo;
                g.reactFrames = 0;
            } else {
                g.react = React::None;
                director.playNext(g.reactAfter);
            }
        } else if (g.react == React::Undo && ++g.reactFrames > 1 && !stage->tableBusy()) {
            g.react = React::None;
            director.playNext(g.reactAfter);
        }

        // Chapters one after the other; after the last one the handshake.
        if (!g.lessonDone && g.chapterStarted && g.react == React::None && !director.pending(g.chapterScript)) {
            if (size_t(g.chapter) + 1 < lesson->chapters().size()) {
                play(lessonNextScript(*lesson, g.chapter + 1));
                startChapter(g.chapter + 1);
            } else {
                g.lessonDone = true;
            }
        }

        // Idle hints while an exercise waits (never while the coach speaks).
        int expect = -1;
        if (director.waitingMove(&expect) && g.react == React::None) {
            if (expect != g.waitExpect) {
                g.waitExpect = expect;
                g.hintStage = 0;
                g.idle = 0.0f;
            }
            if (!director.busy()) g.idle += dt;
            if (g.hintStage == 0 && g.idle >= kLessonIdleHint1) {
                play(lesson->idleHint(expect, 1));
                g.hintStage = 1;
            } else if (g.hintStage == 1 && g.idle >= kLessonIdleHint2) {
                play(lesson->idleHint(expect, 2));
                g.hintStage = 2;
            }
        }
    }
};

Session::Session() : d_(new Impl) {}
Session::~Session() { delete d_; }

void Session::start(Stage& stage, Analyst& analyst, const chess::Game& game, const SessionConfig& config) {
    Impl& d = *d_;
    if (d.g.started) stop();
    d.g = Impl::Game();
    d.g.started = true;
    d.config = config;
    d.stage = &stage;
    d.analyst = &analyst;
    d.history = config.history;
    d.accuracyExplained = config.accuracyExplained;
    d.g.level = std::clamp(config.level, 0, 6);
    d.g.human = d.g.level == 0 ? chess::White : config.human;
    if (d.g.level == 0) d.config.director.speed = kLessonSpeechSpeed;
    installOpeningResolver();
    d.director.reset(&stage, d.config.director);

    if (d.g.level == 0) {
        if (!d.lesson) d.lesson.reset(new Lesson());
        const int chapters = int(d.lesson->chapters().size());
        const int chapter = std::clamp(config.lessonChapter, 0, chapters - 1);
        if (chapter > 0) d.play(lessonResumeScript(*d.lesson, chapter));
        d.startChapter(chapter);
        return;
    }
    d.reviewer.reset(d.g.level, d.g.human, config.offersEnabled);
    d.appraisal.reset(d.g.level, d.g.human);
    d.announcer.newGame();
    d.announcer.setLevel(d.g.level);
    d.announcer.setHumanColor(d.g.human);
    d.announcer.setLanguages(config.director.uiLanguage, speechLanguage(config.director.uiLanguage));
    if (!game.moves().empty()) d.announcer.catchUp(game);
    d.play(greetingScript(d.g.level, d.g.human, config.introduceLevel));
    d.g.chatScripts.push_back(d.director.lastScript());
}

void Session::stop() {
    Impl& d = *d_;
    if (!d.g.started) return;
    d.director.clear();
    if (d.analyst) d.analyst->cancelAnalysis(0);
    d.g.jobs.clear();
    d.g.started = false;
}

void Session::update(const chess::Game& game, float dt) {
    Impl& d = *d_;
    if (!d.g.started) return;
    if (d.g.level == 0) d.updateLesson(game, dt);
    else d.updateGame(game, dt);
    d.director.update(dt, int(game.moves().size()));
}

void Session::setPaused(bool paused) { d_->director.setPaused(paused); }

void Session::onMove(const chess::Game& game) {
    Impl& d = *d_;
    if (!d.g.started) return;
    if (d.g.level == 0) d.lessonMove(game);
    else d.onMove(game);
}

void Session::onPlayerActive() {
    Impl& d = *d_;
    if (!d.g.started) return;
    d.director.playerActed();
    d.g.idle = 0.0f;
    if (d.director.offerOpen()) d.answerOffer(nullptr, false, true);   // touching a piece: play on
}

void Session::onIllegalAttempt(const chess::Game& game, chess::Square from, chess::Square to) {
    Impl& d = *d_;
    if (!d.g.started || d.g.level != 0 || d.director.pending(d.g.illegalScript)) return;
    int expect = -1;
    if (!d.director.waitingMove(&expect)) expect = -1;
    d.g.idle = 0.0f;
    const Script s = d.lesson->explainIllegal(expect, game.position(), from, to);
    if (s.empty()) return;
    d.play(s);
    d.g.illegalScript = d.director.lastScript();
}

void Session::onOfferAnswer(const chess::Game& game, bool accept) {
    if (d_->g.started) d_->answerOffer(&game, accept, false);
}

bool Session::canTakeBack(const chess::Game& game) const {
    const Impl& d = *d_;
    return d.g.started && d.g.level > 0 && !d.g.over && !game.isOver() && d.lastHumanPly(game) >= 0;
}

void Session::onTakeBackRequested(const chess::Game& game) {
    Impl& d = *d_;
    if (!canTakeBack(game)) return;
    const int human = d.lastHumanPly(game);
    const int n = int(game.moves().size());
    const int plies = n - human;   // the human's move and, when the coach has answered, its reply
    const bool offered = d.g.offerPly == human;   // (takenBack() forgets it)
    d.director.clear();
    d.stage->takeBack(plies);
    chess::Game after = game;
    if (int(after.moves().size()) == n) after.undo(plies);
    d.takenBack(after);
    d.g.takebackTo = human;
    Script s = takebackScript(true, human);
    if (offered) append(s, d.reviewer.takebackAccepted(after));   // the replay is judged against it
    d.play(s);
}

void Session::onDrawAnswer(bool accepted) {
    if (d_->g.started) d_->play(drawAnswerScript(accepted));
}

void Session::onGameOver(const chess::Game& game, bool humanResigned) {
    if (d_->g.started) d_->gameOver(game, humanResigned);
}

void Session::skip() {
    Impl& d = *d_;
    if (!d.g.started) return;
    // A lesson chapter is one script: Space skips the line being said, never the exercises after it.
    if (d.g.level == 0) d.director.skipCurrent();
    else d.director.skip();
}

void Session::onCoachThinking(float seconds) {
    Impl& d = *d_;
    if (seconds <= 0.0f) {
        d.g.thinking = false;
    } else {
        if (!d.g.thinking || seconds < d.g.lastThinking) d.g.engineHold = 0.0f;   // a new search
        d.g.thinking = true;
    }
    d.g.lastThinking = seconds;
}

bool Session::coachMayMove() const { return d_->coachMayMove(); }

bool Session::playerMayMove(const chess::Game& game) const {
    const Impl& d = *d_;
    if (!d.g.started) return false;
    if (d.g.level == 0)
        return d.g.react == Impl::React::None && game.position().sideToMove() == chess::White && d.director.waitingMove();
    return !d.g.over && !game.isOver() && game.position().sideToMove() == d.g.human && !d.director.offerOpen();
}

bool Session::handshakeWanted() const {
    const Impl& d = *d_;
    if (!d.g.started || d.g.handshakeDone) return false;
    if (d.g.level == 0) return d.g.lessonDone && !d.director.pending(d.g.chapterScript);
    return d.g.endScript != 0 && !d.director.pending(d.g.endScript);
}

void Session::onHandshakeDone(const chess::Game& game) {
    if (d_->g.started && handshakeWanted()) d_->handshakeDone(game);
}

bool Session::finished() const {
    const Impl& d = *d_;
    if (!d.g.handshakeDone) return false;
    return d.g.done || !d.director.pending(d.g.appraisalScript);
}

bool Session::offerOpen() const { return d_->director.offerOpen(); }

const std::vector<GameRecord>& Session::history() const { return d_->history; }
bool Session::accuracyExplained() const { return d_->accuracyExplained; }
int Session::suggestedLevel() const { return d_->g.suggested; }
int Session::lessonChapter() const { return d_->g.lessonDone ? 0 : d_->g.chapter; }
bool Session::lessonCompleted() const { return d_->g.lessonDone; }
Director& Session::director() { return d_->director; }
const Director& Session::director() const { return d_->director; }

}  // namespace coach
