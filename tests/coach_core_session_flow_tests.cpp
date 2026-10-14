// The coach's session (src/coach/session.*) on the fake Stage and Analyst (tests/coach_fakes.h),
// the paths the other session tests leave out: stop() (the voice silenced, the analyses cancelled,
// nothing done until the next start; a challenge's searches too) and a start over a running
// session; the level suggested after two accurate wins; a resignation before any move (nothing to
// appraise: finished at once); a mate by the human playing Black; the review of a move that is
// gone before its analysis came in; late searches given up (the played move's after 4 s, the
// confirmation of a reason after 6 s) while a background evaluation goes on; the coach's remarks on
// a move that is gone; the analysis cache bounded (an evicted position searched again when the
// game comes back to it); the rules lesson (a move outside an exercise taken back, a chapter
// followed by the next one, the last one ending the lesson, the second idle hint); and a challenge
// routed through the session (the hint offer accepted, H).
#include "test.h"
#include "coach_fakes.h"

#include "coach/catalog.h"
#include "coach/challenge.h"
#include "coach/lesson.h"
#include "coach/session.h"

#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace coach;

namespace {

const char* kHangFen = "6k1/5ppp/4p3/8/8/2N5/5PPP/6K1 w - - 0 1";   // 1.Nd5?? exd5
const char* kMateFen = "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1";      // 1.Ra8#

ai::PvLine pvl(int cp, const char* pv, int mate = 0) {
    ai::PvLine l;
    l.depth = 16;
    l.score.cp = cp;
    l.score.mate = mate;
    std::istringstream in(pv);
    std::string m;
    while (in >> m) l.pv.push_back(m);
    return l;
}

ai::Analysis analysisOf(std::vector<ai::PvLine> lines) {
    ai::Analysis a;
    a.ok = true;
    a.depth = 16;
    for (size_t i = 0; i < lines.size(); ++i) lines[i].multipv = int(i + 1);
    a.lines = lines;
    if (!lines.empty() && !lines[0].pv.empty()) a.bestMove = lines[0].pv[0];
    return a;
}

std::string fenOf(const char* fen) {
    chess::Position p;
    p.setFEN(fen);
    return p.fen();
}

SessionConfig levelConfig(int level, chess::Color human = chess::White) {
    SessionConfig c;
    c.level = level;
    c.human = human;
    return c;
}

struct Table {
    fake::Stage stage;
    fake::Analyst analyst;
    Session session;
    chess::Game game;
    float dt = 1.0f / 60.0f;
    std::vector<Line> lines;   // every line the session queued

    Table() {
        stage.game = &game;
        session.director().setObserver([this](const Beat& b) {
            if (!b.line.key.empty()) lines.push_back(b.line);
        });
    }
    void start(const SessionConfig& cfg) { session.start(stage, analyst, game, cfg); }
    void step() {
        stage.advance(dt);
        if (stage.lessonMoves > 0) {   // the scene reports the coach's lesson moves as moves
            stage.lessonMoves = 0;
            session.onMove(game);
        }
        session.update(game, dt);
    }
    bool until(const std::function<bool()>& cond, float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) {
            if (cond()) return true;
            step();
        }
        return cond();
    }
    void run(float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) step();
    }
    bool quiet(float seconds = 60.0f) {
        return until([&] { return session.director().idle() && !stage.bodyBusy() && !stage.tableBusy(); }, seconds);
    }
    void move(const char* uci) {
        const chess::Move m = game.position().parseUCI(uci);
        CHECK(m.valid());
        if (!m.valid()) return;
        game.play(m);
        session.onMove(game);
    }
    bool reply(const char* uci) {
        if (!until([&] { return session.coachMayMove(); }, 60.0f)) return false;
        move(uci);
        return true;
    }
    int queued(const std::string& key) const {
        int n = 0;
        for (const Line& l : lines) n += l.key == key ? 1 : 0;
        return n;
    }
    bool queuedPrefix(const std::string& prefix) const {
        for (const Line& l : lines)
            if (l.key.rfind(prefix, 0) == 0) return true;
        return false;
    }
    int jobs(const std::string& shape) const {
        int n = 0;
        for (const fake::Analyst::Job& j : analyst.jobs) n += j.shape == shape ? 1 : 0;
        return n;
    }
    // Every queued line renders in English without a placeholder left.
    bool rendersClean() const {
        const Catalog& cat = Catalog::shared();
        for (const Line& l : lines) {
            if (!cat.has(l.key)) return false;
            const std::string text = cat.renderVariant(l, "en", false, 1).text;
            if (text.empty() || text.find('{') != std::string::npos) return false;
        }
        return true;
    }
};

void hangTable(Table& t) {
    t.game.resetFromFEN(kHangFen);
    t.analyst.results[fenOf(kHangFen) + "|A0"] = analysisOf({pvl(20, "g1f1 g8f8"), pvl(-330, "c3d5 e6d5 g1f1")});
}

// The keys of a script's lines.
std::vector<std::string> keysOf(const Script& s) {
    std::vector<std::string> out;
    for (const Beat& b : s)
        if (!b.line.key.empty()) out.push_back(b.line.key);
    return out;
}

// A move the lesson's exercise 'expect' accepts in 'pos': its solution, else one of the moves it
// accepts, else any legal move (the exercises about illegal moves or a check to escape).
std::string answerFor(const Lesson& lesson, int expect, const chess::Position& pos) {
    const Expectation& e = lesson.expectation(expect);
    if (!e.solution.empty()) return e.solution;
    if (!e.accept.empty()) return e.accept.front();
    const std::vector<chess::Move> moves = pos.legalMoves();
    return moves.empty() ? std::string() : pos.toUCI(moves.front());
}

}  // namespace

TEST(coach_core_session_stop_and_start_again) {
    Table t;
    hangTable(t);
    t.analyst.delay = 100000;   // the turn's searches stay out
    t.start(levelConfig(1));
    CHECK(t.until([&] { return t.session.director().speaking(); }, 10.0f));
    CHECK(t.session.playerMayMove(t.game));
    CHECK(!t.analyst.jobs.empty());
    t.session.stop();
    CHECK(t.analyst.jobs.empty());
    CHECK(t.session.director().idle());
    CHECK_EQ(t.stage.count("voice.stop"), 1);
    CHECK(!t.session.playerMayMove(t.game));
    // Stopped: no frame, move or answer does anything.
    const size_t requests = t.stage.reqs.size(), asked = t.analyst.asked.size(), queued = t.lines.size();
    t.run(2.0f);
    t.move("g1f1");
    t.session.onOfferAnswer(t.game, true);
    t.session.onGameOver(t.game, true);
    t.run(1.0f);
    t.session.stop();   // twice: nothing
    CHECK_EQ(t.stage.reqs.size(), requests);
    CHECK_EQ(t.analyst.asked.size(), asked);
    CHECK_EQ(t.lines.size(), queued);
    CHECK(!t.session.handshakeWanted());
    CHECK(!t.session.finished());

    // Started again, and again over the running session: a new game each time.
    t.game.resetFromFEN(kHangFen);
    t.analyst.delay = 3;
    t.start(levelConfig(2));
    CHECK(t.until([&] { return t.queued("event.greet.l2") == 1 && !t.analyst.jobs.empty(); }, 10.0f));
    t.lines.clear();
    t.start(levelConfig(1));
    CHECK(t.analyst.jobs.empty());   // the first one's search cancelled
    CHECK(t.quiet());
    CHECK_EQ(t.queued("event.greet.l1"), 1);
    CHECK_EQ(t.queued("event.greet.l2"), 0);
    CHECK(t.session.playerMayMove(t.game));
    CHECK(t.rendersClean());
}

TEST(coach_core_session_stop_cancels_a_challenge) {
    const Challenge* kq = ChallengeBook::shared().find("kq");
    CHECK(kq != nullptr);
    if (!kq) return;
    CHECK(!kq->positions.empty() && kq->positions.front().playOut());
    Table t;
    t.game.setEndDetection(false);
    t.analyst.delay = 100000;
    SessionConfig cfg = levelConfig(2);
    cfg.challenge = "kq";
    t.start(cfg);
    CHECK(t.session.challengeMode());
    CHECK(t.until([&] { return t.session.playerMayMove(t.game); }, 120.0f));
    CHECK_EQ(t.analyst.jobs.size(), size_t(1));   // the play-out's hint search
    t.session.stop();
    CHECK(t.analyst.jobs.empty());
    CHECK(!t.session.playerMayMove(t.game));
    CHECK(!t.session.hintAvailable(t.game));
}

TEST(coach_core_session_suggests_the_next_level) {
    Table t;
    t.game.resetFromFEN(kMateFen);
    t.analyst.results[fenOf(kMateFen) + "|A0"] = analysisOf({pvl(0, "a1a8", 1), pvl(300, "g1f1 g8f8")});
    SessionConfig cfg = levelConfig(1);
    cfg.history = {GameRecord{1, 1, 99.0}};
    t.start(cfg);
    CHECK(t.quiet());
    CHECK_EQ(t.session.suggestedLevel(), 0);
    t.move("a1a8");
    t.session.onGameOver(t.game, false);
    CHECK(t.until([&] { return t.session.handshakeWanted(); }, 60.0f));
    t.session.onHandshakeDone(t.game);
    CHECK_EQ(t.session.history().size(), size_t(2));
    CHECK_EQ(t.session.suggestedLevel(), 2);
    CHECK(t.until([&] { return t.session.finished(); }, 120.0f));
    CHECK(t.rendersClean());
}

TEST(coach_core_session_resignation_before_any_move) {
    Table t;
    hangTable(t);
    t.start(levelConfig(2));
    CHECK(t.quiet());
    t.game.resign(chess::White);
    t.session.onGameOver(t.game, true);
    CHECK(!t.session.playerMayMove(t.game));
    CHECK(t.until([&] { return t.session.handshakeWanted(); }, 60.0f));
    CHECK_EQ(t.queued("event.end.resigned"), 1);
    const size_t queued = t.lines.size();
    t.session.onHandshakeDone(t.game);
    // No move to appraise: finished at once, the game recorded as a loss.
    CHECK(t.session.finished());
    CHECK_EQ(t.lines.size(), queued);
    CHECK_EQ(t.session.history().size(), size_t(1));
    CHECK_EQ(t.session.history().back().result, -1);
    t.session.onHandshakeDone(t.game);   // once only
    CHECK_EQ(t.session.history().size(), size_t(1));
}

TEST(coach_core_session_human_black_mates) {
    const char* fen = "r5k1/5ppp/8/8/8/8/5PPP/6K1 b - - 0 1";   // 1...Ra1#
    Table t;
    t.game.resetFromFEN(fen);
    t.analyst.results[fenOf(fen) + "|A0"] = analysisOf({pvl(0, "a8a1", 1), pvl(300, "g8f8 g1f1")});
    t.start(levelConfig(3, chess::Black));
    CHECK(t.quiet());
    CHECK(t.session.playerMayMove(t.game));
    t.move("a8a1");
    CHECK(t.game.isOver());
    t.session.onGameOver(t.game, false);
    CHECK(t.until([&] { return t.session.handshakeWanted(); }, 60.0f));
    CHECK_EQ(t.queued("event.end.win"), 1);
    CHECK_EQ(t.queued("event.end.loss"), 0);
    t.session.onHandshakeDone(t.game);
    CHECK_EQ(t.session.history().back().result, 1);
    CHECK(t.rendersClean());
}

TEST(coach_core_session_review_of_a_move_gone) {
    // The move is undone (by the scene, without a word to the session) while its review waits for
    // the turn's search: the review is dropped, nothing is said about it.
    Table t;
    hangTable(t);
    t.analyst.delay = 100000;
    t.start(levelConfig(1));
    CHECK(t.quiet());
    t.move("c3d5");
    CHECK(!t.session.coachMayMove());
    t.run(0.5f);
    CHECK(!t.session.coachMayMove());
    t.game.undo(1);
    t.step();
    CHECK(t.session.coachMayMove());
    CHECK(t.session.playerMayMove(t.game));
    t.run(10.0f);
    CHECK(!t.queuedPrefix("ex."));
    CHECK_EQ(t.analyst.count("A1"), 0);
    CHECK_EQ(t.analyst.count("A0", fenOf(kHangFen)), 1);   // the turn's search, not asked again
}

TEST(coach_core_session_late_searches_given_up) {
    // Level 4. 1.h3: neither of its searches comes back with a line, so the position after it has
    // no evaluation, and once the coach has replied a background evaluation of it starts (and runs
    // on: never back). Then 2.Nd5??: the search of the position after it (A2) is given up after
    // 4 s, the confirmation of the reason (V) after 6 s, the background evaluation left alone; the
    // review is built with what is in. A takeback keeps that evaluation too: its position is still
    // in the game.
    chess::Game g;
    g.resetFromFEN(kHangFen);
    g.play(g.position().parseUCI("h2h3"));
    const std::string afterH3 = g.position().fen();
    g.play(g.position().parseUCI("g8f8"));
    const std::string root = g.position().fen();
    ai::Analysis failed;
    failed.ok = false;
    Table t;
    hangTable(t);
    t.analyst.results[fenOf(kHangFen) + "|A1"] = failed;
    t.analyst.results[afterH3 + "|A2"] = failed;
    t.analyst.results[root + "|A0"] = analysisOf({pvl(20, "g1h2 f8g8"), pvl(-330, "c3d5 e6d5 g1h2")});
    t.start(levelConfig(4));
    CHECK(t.quiet());
    t.move("h2h3");
    CHECK(t.until([&] { return t.jobs("E") == 1; }, 30.0f));   // asked once the review is over
    CHECK(t.reply("g8f8"));
    t.analyst.delay = 100000;
    CHECK(t.until([&] { return t.jobs("A0") == 1 && t.jobs("A3") == 1; }, 5.0f));
    // The turn's searches come back at once, the evaluation never.
    for (fake::Analyst::Job& j : t.analyst.jobs) j.left = j.shape == "E" ? 100000 : 1;
    CHECK(t.until([&] { return t.analyst.jobs.size() == 1; }, 5.0f));
    CHECK_EQ(t.analyst.jobs.front().fen, afterH3);
    CHECK(t.quiet());

    t.move("c3d5");
    CHECK(t.until([&] { return t.jobs("A2") == 1; }, 1.0f));
    CHECK(t.until([&] { return t.jobs("A2") == 0; }, 4.5f));
    CHECK(t.until([&] { return t.jobs("V") == 1; }, 1.0f));
    CHECK_EQ(t.jobs("E"), 1);
    CHECK(!t.session.coachMayMove());
    CHECK(t.until([&] { return t.jobs("V") == 0; }, 6.5f));
    CHECK_EQ(t.analyst.count("V"), 1);
    CHECK_EQ(t.jobs("E"), 1);
    CHECK(t.until([&] { return t.session.offerOpen() || t.session.coachMayMove(); }, 60.0f));
    CHECK(t.queuedPrefix("ex.verdict."));

    if (t.session.offerOpen()) t.session.onOfferAnswer(t.game, false);
    CHECK(t.until([&] { return t.session.canTakeBack(t.game) && t.session.coachMayMove(); }, 60.0f));
    t.session.onTakeBackRequested(t.game);
    CHECK_EQ(t.jobs("E"), 1);
    CHECK(t.until([&] { return t.game.moves().size() == 2 && t.session.playerMayMove(t.game); }, 10.0f));
    CHECK_EQ(t.jobs("E"), 1);
    CHECK(t.rendersClean());
}

TEST(coach_core_session_coach_move_gone_before_its_remark) {
    // The coach's knight-winning threat would be pointed out at level 1, once its search is in; the
    // move is gone first (undone by the scene): nothing is said about it.
    const char* fen = "4k3/4p3/8/8/8/5N2/8/6K1 w - - 0 1";
    chess::Game after;
    after.resetFromFEN(fen);
    after.play(after.position().parseUCI("f3d4"));
    after.play(after.position().parseUCI("e7e5"));
    chess::Position turn = after.position();
    CHECK(turn.passTurn());
    const std::string threat = turn.fen();
    for (bool undone : {false, true}) {
        Table t;
        t.game.resetFromFEN(fen);
        t.analyst.results[fenOf(fen) + "|A0"] = analysisOf({pvl(0, "f3d4 e7e5")});
        t.analyst.results[threat + "|V"] = analysisOf({pvl(330, "e5d4 g1f2")});
        t.start(levelConfig(1));
        CHECK(t.quiet());
        t.move("f3d4");
        CHECK(t.reply("e7e5"));
        if (undone) t.game.undo(1);
        t.run(3.0f);
        CHECK(t.quiet());
        CHECK_EQ(t.analyst.count("V", threat), 1);
        CHECK_EQ(t.queued("threat.piece.b1"), undone ? 0 : 1);
    }
}

TEST(coach_core_session_analysis_cache_bounded) {
    // Level 3 searches every turn's position twice (A0, A3), and keeps the results of the last 32
    // searches: after seventeen moves (eighteen positions searched) the first two positions are
    // forgotten. Taken back to them one move at a time, the session searches those two again, and
    // only those.
    const char* fen = "6k1/7p/8/8/8/8/7P/K7 w - - 0 1";
    const char* walk[] = {"a1b1", "b1c1", "c1d1", "d1e1", "e1f1", "f1f2", "f2f3", "f3f4", "f4f5",
                          "f5e5", "e5d5", "d5c5", "c5b5", "b5a5", "a5a4", "a4a3", "a3a2"};
    Table t;
    t.game.resetFromFEN(fen);
    t.start(levelConfig(3));
    CHECK(t.quiet());
    // The turn's searches asked and back.
    auto searched = [&] {
        const std::string fen = t.game.position().fen();
        return t.session.playerMayMove(t.game) && t.analyst.count("A0", fen) >= 1 && t.analyst.count("A3", fen) >= 1 &&
               t.analyst.idle();
    };
    std::vector<std::string> roots;
    for (int i = 0; i < 17; ++i) {
        CHECK(t.until(searched, 60.0f));
        roots.push_back(t.game.position().fen());
        t.move(walk[i]);
        CHECK(t.reply(i % 2 == 0 ? "g8h8" : "h8g8"));
    }
    CHECK(t.until(searched, 60.0f));
    for (const std::string& r : roots) CHECK_EQ(t.analyst.count("A0", r), 1);
    for (int i = 16; i >= 0; --i) {
        CHECK(t.until([&] { return t.session.canTakeBack(t.game) && t.session.director().idle(); }, 60.0f));
        t.session.onTakeBackRequested(t.game);
        CHECK(t.until([&] { return t.game.moves().size() == size_t(2 * i) && t.session.playerMayMove(t.game); }, 30.0f));
        t.step();
        CHECK_EQ(t.game.position().fen(), roots[size_t(i)]);
    }
    CHECK(t.until([&] { return t.analyst.idle(); }, 30.0f));
    for (size_t i = 0; i < roots.size(); ++i) {
        CHECK_EQ(t.analyst.count("A0", roots[i]), i < 2 ? 2 : 1);
        CHECK_EQ(t.analyst.count("A3", roots[i]), i < 2 ? 2 : 1);
    }

}

TEST(coach_core_session_lesson_move_outside_an_exercise) {
    Table t;
    SessionConfig cfg = levelConfig(0);
    cfg.lessonChapter = 1;
    t.start(cfg);
    // At once, before the chapter's position is even set up (the scene should not let it happen).
    CHECK(!t.session.playerMayMove(t.game));
    t.move("e2e4");
    const auto tb = t.stage.all("takeBack");
    CHECK(tb.size() == 1 && tb[0].n == 1);
    CHECK(t.until([&] { return t.session.playerMayMove(t.game); }, 120.0f));
    CHECK(t.game.position().fen().rfind("7k/8/n7/8/8/8/8/R3K3 w", 0) == 0);
    CHECK(!t.queuedPrefix("why."));
}

TEST(coach_core_session_lesson_chapters_follow_and_end) {
    const Lesson lesson;
    const int chapters = int(lesson.chapters().size());
    CHECK(chapters >= 3);
    {
        // The first chapter has no exercise for the player to answer before the next one starts:
        // played through with Space, the session goes on to chapter 1 by itself.
        Table t;
        t.game.setEndDetection(false);   // as the scene plays the lesson: kings alone are no draw
        t.start(levelConfig(0));
        CHECK_EQ(t.session.lessonChapter(), 0);
        int expect = -1;
        CHECK(t.until([&] {
            if (t.session.director().skippable()) t.session.skip();
            if (t.session.director().waitingMove(&expect) && lesson.chapterOf(expect) == 0 &&
                t.session.playerMayMove(t.game)) {
                const std::string answer = answerFor(lesson, expect, t.game.position());
                if (!answer.empty()) t.move(answer.c_str());
            }
            return t.session.lessonChapter() == 1;
        }, 600.0f));
        CHECK(t.queuedPrefix("event.lesson.next"));
        CHECK(!t.session.lessonCompleted());
    }
    {
        // The last chapter: once it is over the lesson is complete, the handshake, then done.
        Table t;
        t.game.setEndDetection(false);
        SessionConfig cfg = levelConfig(0);
        cfg.lessonChapter = chapters - 1;
        t.start(cfg);
        CHECK_EQ(t.session.lessonChapter(), chapters - 1);
        int expect = -1;
        CHECK(t.until([&] {
            if (t.session.director().skippable()) t.session.skip();
            if (t.session.director().waitingMove(&expect) && t.session.playerMayMove(t.game)) {
                const std::string answer = answerFor(lesson, expect, t.game.position());
                if (!answer.empty()) t.move(answer.c_str());
            }
            return t.session.handshakeWanted();
        }, 600.0f));
        CHECK(t.session.lessonCompleted());
        CHECK_EQ(t.session.lessonChapter(), 0);   // nothing to resume
        t.session.onHandshakeDone(t.game);
        CHECK(t.session.finished());
        CHECK(t.session.history().empty());
    }
}

TEST(coach_core_session_lesson_second_idle_hint) {
    const Lesson lesson;
    Table t;
    t.dt = 1.0f / 30.0f;
    SessionConfig cfg = levelConfig(0);
    cfg.lessonChapter = 1;
    t.start(cfg);
    int expect = -1;
    CHECK(t.until([&] { return t.session.director().waitingMove(&expect) && t.session.director().idle() == false &&
                               !t.session.director().busy(); }, 120.0f));
    const std::vector<std::string> first = keysOf(lesson.idleHint(expect, 1));
    const std::vector<std::string> second = keysOf(lesson.idleHint(expect, 2));
    CHECK(!first.empty() && !second.empty());
    if (first.empty() || second.empty()) return;
    const size_t before = t.lines.size();
    t.run(30.0f);
    CHECK(t.lines.size() > before);
    CHECK_EQ(t.lines[before].key, first.front());
    const size_t hinted = t.lines.size();
    t.run(25.0f);
    CHECK(t.lines.size() > hinted);
    if (t.lines.size() > hinted) CHECK_EQ(t.lines[hinted].key, second.front());
    // No third one.
    const size_t after = t.lines.size();
    t.run(60.0f);
    CHECK_EQ(t.lines.size(), after);
    int still = -1;
    CHECK(t.session.director().waitingMove(&still) && still == expect);
}

TEST(coach_core_session_challenge_routed) {
    // The first set of the game whose first position is a line: three wrong moves bring the hint
    // offer; Yes on its card gives the first hint, H the next one.
    const Challenge* pick = nullptr;
    for (const Challenge& c : ChallengeBook::shared().challenges())
        if (!pick && !c.positions.empty() && !c.positions.front().playOut()) pick = &c;
    CHECK(pick != nullptr);
    if (!pick) return;
    const ChallengePosition& p = pick->positions.front();
    chess::Position start;
    CHECK(p.start(start));
    std::string wrong;
    for (const chess::Move& m : start.legalMoves()) {
        chess::Position after = start;
        after.makeMove(m);
        if (wrong.empty() && !p.accepts(0, start, start.toUCI(m)) && after.hasLegalMove()) wrong = start.toUCI(m);
    }
    CHECK(!wrong.empty());
    Table t;
    t.game.setEndDetection(false);
    SessionConfig cfg = levelConfig(4);
    cfg.challenge = pick->id;
    t.start(cfg);
    const Session& view = t.session;
    for (int i = 0; i < 3; ++i) {
        CHECK(t.until([&] { return t.session.playerMayMove(t.game); }, 120.0f));
        const size_t plies = t.game.moves().size();
        t.move(wrong.c_str());
        CHECK(t.until([&] { return t.game.moves().size() == plies && !view.director().idle(); }, 60.0f));
    }
    CHECK(t.until([&] { return t.session.offerOpen(); }, 60.0f));
    CHECK(t.session.offerIsHint());
    t.session.onOfferAnswer(t.game, true);
    CHECK(!t.session.offerOpen());
    CHECK(t.queuedPrefix("ch.hint."));
    int hints = 0;
    for (const Line& l : t.lines) hints += l.key.rfind("ch.hint.", 0) == 0 ? 1 : 0;
    CHECK_EQ(hints, 1);
    CHECK(t.until([&] { return t.session.hintAvailable(t.game); }, 60.0f));
    t.session.onHintRequested(t.game);
    hints = 0;
    for (const Line& l : t.lines) hints += l.key.rfind("ch.hint.", 0) == 0 ? 1 : 0;
    CHECK_EQ(hints, 2);
    CHECK(!view.director().idle());
    CHECK(t.rendersClean());
}
