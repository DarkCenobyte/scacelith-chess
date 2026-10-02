// The coach's session (src/coach/session.*) on the fake Stage and Analyst (tests/coach_fakes.h):
// a blunder reviewed, demonstrated and offered back (accepted: Stage::takeBack(1), the retry judged
// against it; declined: the answer and the encouragement), the coach's reply held while the review
// runs, "Your move." after an explanation without an offer, the pause menu's takeback, opening
// names, turn-taking lines, the end of a game (closing words, handshake, appraisal, Space skipping
// it), and a chapter of the rules lesson (a wrong move answered and taken back, an illegal attempt
// explained, the right move, an idle hint). Every line the session emits renders, in every
// language that has its key, without a leftover placeholder.
#include "test.h"
#include "coach_fakes.h"

#include "coach/catalog.h"
#include "coach/lesson.h"
#include "coach/session.h"

#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace coach;
using chess::parseSquare;

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
    // The coach's reply, played as soon as the session lets it.
    bool reply(const char* uci) {
        if (!until([&] { return session.coachMayMove(); }, 60.0f)) return false;
        move(uci);
        return true;
    }
    bool queued(const std::string& key) const {
        for (const Line& l : lines)
            if (l.key == key) return true;
        return false;
    }
    bool queuedPrefix(const std::string& prefix) const {
        for (const Line& l : lines)
            if (l.key.rfind(prefix, 0) == 0) return true;
        return false;
    }
    // Heard: one of the queued lines with this key started on the voice.
    bool said(const std::string& key) const { return saidAt(key) >= 0.0; }
    double saidAt(const std::string& key) const {
        const Catalog& cat = Catalog::shared();
        for (const fake::Stage::Ev& e : stage.ev) {
            if (e.kind != "voice.start") continue;
            for (const Line& l : lines) {
                if (l.key != key) continue;
                for (int v = 1; v <= std::max(1, cat.variants(key)); ++v)
                    if (cat.renderVariant(l, "en", true, v).text == e.text) return e.t;
            }
        }
        return -1.0;
    }
};

// Every line renders, written and spoken, every variant, in English and in every language that
// has the key itself, without a placeholder left.
void checkRenders(const std::vector<Line>& lines) {
    const Catalog& cat = Catalog::shared();
    const std::vector<std::string> langs = cat.languages();
    CHECK(langs.size() >= 10);
    int rendered = 0;
    for (const Line& l : lines) {
        CHECK(cat.has(l.key));
        for (const std::string& lang : langs) {
            // A language without the key falls back to the English template (clean too, but the
            // catalog logs every noun form it lacks: the test output would drown).
            if (lang != "en" && !cat.has(lang, l.key)) continue;
            for (int v = 1; v <= std::max(1, cat.variants(l.key)); ++v) {
                for (bool spoken : {false, true}) {
                    const std::string text = cat.renderVariant(l, lang, spoken, v).text;
                    ++rendered;
                    if (text.empty() || text.find('{') != std::string::npos) {
                        std::fprintf(stderr, "  %s [%s, %s, v%d]: \"%s\"\n", l.key.c_str(), lang.c_str(),
                                     spoken ? "spoken" : "written", v, text.c_str());
                        CHECK(false);
                    }
                }
            }
        }
    }
    CHECK(rendered > 0);
}

SessionConfig levelConfig(int level, chess::Color human = chess::White) {
    SessionConfig c;
    c.level = level;
    c.human = human;
    return c;
}

// The hanging-knight position with its A0 (the level-1 example of a hanging piece).
void hangTable(Table& t, int level = 1) {
    t.game.resetFromFEN(kHangFen);
    t.analyst.results[fenOf(kHangFen) + "|A0"] = analysisOf({pvl(20, "g1f1 g8f8"), pvl(-330, "c3d5 e6d5 g1f1")});
    (void)level;
}

}  // namespace

TEST(coach_session_blunder_offer_accepted_and_retried) {
    Table t;
    hangTable(t);
    t.start(levelConfig(1));
    CHECK(t.quiet());
    CHECK(t.queued("event.greet.l1"));
    const std::string root = fenOf(kHangFen);
    CHECK_EQ(t.analyst.count("A0", root), 1);   // at the start of the human's turn
    CHECK(t.session.playerMayMove(t.game));
    CHECK(!t.session.canTakeBack(t.game));

    t.move("c3d5");
    CHECK(!t.session.coachMayMove());
    bool held = true;
    CHECK(t.until([&] {
        held = held && !t.session.coachMayMove();
        return t.session.offerOpen();
    }, 60.0f));
    CHECK(held);   // the coach's reply waits for the review
    CHECK(t.said("ex.verdict.blunder.b1"));
    CHECK(t.said("ex.hanging.b1"));
    CHECK(t.said("ex.offer.b1"));
    const auto demos = t.stage.all("demoMove");
    CHECK(demos.size() == 1 && demos[0].text == "e6d5");
    const auto rewinds = t.stage.all("rewindDemo");
    CHECK(rewinds.size() == 1 && rewinds[0].n == 1);
    CHECK(t.stage.all("offer").back().flag);
    CHECK(!t.session.playerMayMove(t.game));
    CHECK_EQ(t.analyst.count("A1"), 0);   // A0 held the played move

    // Accepted: the move goes back by hand, and the hint follows.
    t.session.onOfferAnswer(t.game, true);
    const auto tb = t.stage.all("takeBack");
    CHECK(tb.size() == 1 && tb[0].n == 1);
    CHECK(!t.session.offerOpen());
    CHECK(!t.stage.all("offer").back().flag);
    CHECK(t.until([&] { return !t.stage.tableBusy(); }, 5.0f));   // undone when the table action starts
    CHECK_EQ(t.game.moves().size(), size_t(0));
    CHECK(t.quiet());
    CHECK(t.said("event.takeback.taken"));
    CHECK(t.queuedPrefix("tb.hint"));
    CHECK_EQ(t.analyst.count("A0", root), 1);   // the same root: its A0 is reused
    CHECK(t.session.playerMayMove(t.game));

    // The retry is judged against the move taken back.
    t.move("g1f1");
    CHECK(t.until([&] { return t.session.coachMayMove() && t.session.director().idle(); }, 60.0f));
    CHECK(t.queued("tb.fixed.b1"));
    CHECK(t.reply("g8f8"));
    CHECK(t.until([&] { return t.analyst.count("A0", t.game.position().fen()) == 1; }, 5.0f));
    CHECK(t.session.canTakeBack(t.game));
    checkRenders(t.lines);
}

TEST(coach_session_offer_declined) {
    Table t;
    hangTable(t);
    t.start(levelConfig(1));
    CHECK(t.quiet());
    t.move("c3d5");
    CHECK(t.until([&] { return t.session.offerOpen(); }, 60.0f));
    CHECK(!t.session.coachMayMove());
    t.session.onOfferAnswer(t.game, false);
    CHECK(t.stage.all("takeBack").empty());
    t.step();
    CHECK(t.session.coachMayMove());   // the review is over: the reply may be played
    CHECK(t.quiet());
    CHECK(t.said("event.takeback.declined"));
    CHECK(t.said("event.encourage.mistake"));
    CHECK(t.reply("e6d5"));
    CHECK(t.quiet());
    CHECK(!t.queued("event.your_move"));   // the offer's answer said it all
    checkRenders(t.lines);

    // Touching a piece while the card shows: playing on.
    Table u;
    hangTable(u);
    u.start(levelConfig(1));
    CHECK(u.quiet());
    u.move("c3d5");
    CHECK(u.until([&] { return u.session.offerOpen(); }, 60.0f));
    u.session.onPlayerActive();
    CHECK(!u.session.offerOpen());
    CHECK(u.quiet());
    CHECK(u.said("event.play_on"));
    checkRenders(u.lines);
}

TEST(coach_session_explanation_without_offer_then_your_move) {
    Table t;
    hangTable(t);
    SessionConfig cfg = levelConfig(1);
    cfg.offersEnabled = false;
    t.start(cfg);
    CHECK(t.quiet());
    t.move("c3d5");
    bool held = true;
    double heard = -1.0;
    CHECK(t.until([&] {
        held = held && (!t.session.coachMayMove() || t.said("ex.rewind.b1"));
        heard = t.saidAt("ex.hanging.b1");
        return t.session.coachMayMove();
    }, 60.0f));
    CHECK(held);
    CHECK(heard >= 0.0);
    CHECK(!t.queued("ex.offer.b1"));
    CHECK(t.stage.all("offer").empty());
    CHECK(t.session.director().idle());   // the whole explanation, rewind included, before the reply
    CHECK(t.reply("e6d5"));
    CHECK(t.quiet());
    CHECK(t.said("event.your_move"));
    checkRenders(t.lines);
}

TEST(coach_session_takeback_requested) {
    Table t;
    hangTable(t);
    t.start(levelConfig(2));
    CHECK(t.quiet());
    t.move("g1f1");
    CHECK(t.reply("g8f8"));
    CHECK(t.until([&] { return t.analyst.count("A0", t.game.position().fen()) == 1; }, 5.0f));
    CHECK(t.session.canTakeBack(t.game));
    t.session.onTakeBackRequested(t.game);
    const auto tb = t.stage.all("takeBack");
    CHECK(tb.size() == 1 && tb[0].n == 2);   // the reply and the human's move
    CHECK(t.until([&] { return !t.stage.tableBusy(); }, 5.0f));
    CHECK_EQ(t.game.moves().size(), size_t(0));
    CHECK(t.quiet());
    CHECK(t.said("event.takeback.taken"));
    CHECK_EQ(t.analyst.count("A0", fenOf(kHangFen)), 1);
    CHECK(!t.session.canTakeBack(t.game));
    checkRenders(t.lines);
}

TEST(coach_session_menu_takeback_of_an_offered_move) {
    // The offer declined, then that move taken back from the pause menu: the hint follows, and the
    // replay is judged against the move taken back (not reviewed and offered again).
    Table t;
    hangTable(t);
    t.start(levelConfig(1));
    CHECK(t.quiet());
    t.move("c3d5");
    CHECK(t.until([&] { return t.session.offerOpen(); }, 60.0f));
    t.session.onOfferAnswer(t.game, false);
    CHECK(t.reply("e6d5"));
    CHECK(t.quiet());
    CHECK(t.session.canTakeBack(t.game));
    t.session.onTakeBackRequested(t.game);
    CHECK(t.queuedPrefix("tb.hint"));
    CHECK(t.until([&] { return t.game.moves().empty() && t.session.playerMayMove(t.game); }, 10.0f));
    CHECK(t.quiet());
    auto offers = [&] {
        int n = 0;
        for (const Line& l : t.lines) n += l.key == "ex.offer.b1" ? 1 : 0;
        return n;
    };
    CHECK_EQ(offers(), 1);
    t.move("c3d5");
    CHECK(t.until([&] { return t.session.coachMayMove() && t.session.director().idle(); }, 60.0f));
    CHECK(t.queued("tb.same"));
    CHECK_EQ(offers(), 1);
    checkRenders(t.lines);
}

TEST(coach_session_no_turn_while_a_takeback_waits) {
    // The pause menu's Take back after the coach's reply: until the table undoes the moves, the
    // position on the board is going away, so no human turn begins on it (no A0 nor A3 asked).
    Table t;
    hangTable(t);
    t.stage.takeBackWait = 30;
    t.analyst.delay = 150;
    t.start(levelConfig(3));
    CHECK(t.quiet());
    t.move("g1f1");
    CHECK(t.reply("g8f8"));
    t.step();
    const std::string doomed = t.game.position().fen();
    CHECK_EQ(t.analyst.count("A0", doomed), 1);
    CHECK_EQ(t.analyst.count("A3", doomed), 1);
    t.session.onTakeBackRequested(t.game);
    CHECK(t.until([&] { return !t.stage.tableBusy(); }, 10.0f));
    CHECK_EQ(t.game.moves().size(), size_t(0));
    CHECK_EQ(t.analyst.count("A0", doomed), 1);
    CHECK_EQ(t.analyst.count("A3", doomed), 1);
    CHECK(t.until([&] { return t.session.playerMayMove(t.game); }, 10.0f));
}

TEST(coach_session_pause_menu_takeback_keeps_the_voice) {
    // Esc while the coach speaks, then the pause menu's Take back: once resumed, the coach is heard
    // again (its pause does not outlive the line it held).
    Table t;
    hangTable(t);
    t.start(levelConfig(2));
    CHECK(t.quiet());
    t.move("g1f1");
    CHECK(t.reply("g8f8"));
    CHECK(t.quiet());
    t.session.onDrawAnswer(false);
    CHECK(t.until([&] { return t.session.director().speaking(); }, 5.0f));
    t.session.setPaused(true);
    t.step();
    t.session.onTakeBackRequested(t.game);
    t.session.setPaused(false);
    const int heard = t.stage.count("voice.end");
    CHECK(t.quiet());
    CHECK(t.said("event.takeback.taken"));
    CHECK_EQ(t.stage.count("voice.end"), heard + 1);   // heard to its end
}

TEST(coach_session_openings_and_turn_taking) {
    // The opening is named once it is settled (level 1: the Italian after 3.Bc4).
    Table t;
    t.start(levelConfig(1));
    CHECK(t.quiet());
    const char* moves[] = {"e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5", "c2c3", "g8f6", "d2d3", "d7d6"};
    for (size_t i = 0; i < sizeof moves / sizeof *moves; i += 2) {
        CHECK(t.until([&] { return t.session.playerMayMove(t.game); }, 10.0f));
        t.move(moves[i]);
        CHECK(t.reply(moves[i + 1]));
        CHECK(t.quiet());
    }
    CHECK(t.queuedPrefix("opening."));
    CHECK_EQ(t.analyst.count("E"), 0);   // every evaluation came with the reviews
    // "Take your time." after a minute without a move (levels 1-3), once.
    t.run(61.0f);
    CHECK(t.quiet());
    CHECK(t.said("event.take_time"));
    checkRenders(t.lines);

    // A filler when the engine alone holds the coach's move for 6 s.
    Table u;
    u.start(levelConfig(2, chess::Black));
    CHECK(u.quiet());
    for (float s = 0.0f; s < 8.0f; s += u.dt) {
        u.session.onCoachThinking(s + 0.01f);
        u.step();
    }
    CHECK(u.said("event.filler"));
    u.session.onCoachThinking(0.0f);
    checkRenders(u.lines);
}

TEST(coach_session_background_evaluations) {
    // A review without its analysis leaves the evaluation after the move unknown: the appraisal's
    // evaluation is asked in the background, when the analyst is idle, at a low priority.
    Table t;
    t.analyst.results[chess::Game().position().fen() + "|A0"] = ai::Analysis{};   // the engine failed
    t.start(levelConfig(2));
    CHECK(t.quiet());
    t.move("e2e4");
    CHECK(t.until([&] { return t.analyst.count("E") > 0; }, 10.0f));
    CHECK(t.analyst.asked.back().req.priority < 0);
    CHECK(t.reply("e7e5"));
    CHECK(t.quiet());
    const int evals = t.analyst.count("E");
    t.run(5.0f);
    CHECK_EQ(t.analyst.count("E"), evals);   // asked once
    checkRenders(t.lines);
}

TEST(coach_session_game_end_handshake_appraisal) {
    for (bool space : {false, true}) {
        Table t;
        t.game.resetFromFEN(kMateFen);
        t.analyst.results[fenOf(kMateFen) + "|A0"] = analysisOf({pvl(0, "a1a8", 1), pvl(300, "g1f1 g8f8")});
        SessionConfig cfg = levelConfig(3);
        cfg.history = {GameRecord{3, 1, 80.0}};
        t.start(cfg);
        CHECK(t.quiet());
        t.move("a1a8");
        CHECK(t.game.isOver());
        t.session.onGameOver(t.game, false);
        CHECK(!t.session.handshakeWanted());
        CHECK(t.until([&] { return t.session.handshakeWanted(); }, 60.0f));
        CHECK(t.said("ann.mate.human.b1"));
        CHECK(t.said("event.end.win"));
        CHECK(t.said("event.end.handshake"));
        CHECK(!t.session.finished());
        t.run(1.0f);
        CHECK(t.session.handshakeWanted());   // until the scene says it was played
        t.session.onHandshakeDone(t.game);
        CHECK(!t.session.handshakeWanted());
        CHECK_EQ(t.session.history().size(), size_t(2));
        CHECK_EQ(t.session.history().back().result, 1);
        if (space) {
            CHECK(t.until([&] { return t.session.director().speaking(); }, 10.0f));
            t.session.skip();   // Space skips the whole appraisal
            t.run(0.1f);
            CHECK(t.session.finished());
            CHECK_EQ(t.stage.count("voice.stop"), 1);
        } else {
            CHECK(t.until([&] { return t.session.finished(); }, 120.0f));
            int appraisal = 0;
            for (const Line& l : t.lines) appraisal += l.key.rfind("appraisal.", 0) == 0 ? 1 : 0;
            CHECK(appraisal >= 2);
        }
        CHECK(t.queuedPrefix("appraisal.open.win"));
        checkRenders(t.lines);
    }
}

// A mate while the greeting is still being said: the mate is announced at once, the rest of the
// greeting ("You play White") is dropped, the closing words follow.
TEST(coach_session_game_over_drops_the_greeting) {
    Table t;
    t.game.resetFromFEN(kMateFen);
    t.analyst.results[fenOf(kMateFen) + "|A0"] = analysisOf({pvl(0, "a1a8", 1), pvl(300, "g1f1 g8f8")});
    t.start(levelConfig(3));
    CHECK(t.until([&] { return t.session.director().speaking(); }, 10.0f));
    t.move("a1a8");
    CHECK(t.game.isOver());
    t.session.onGameOver(t.game, false);
    CHECK(t.until([&] { return t.session.handshakeWanted(); }, 60.0f));
    CHECK(t.said("ann.mate.human.b1"));
    CHECK(t.said("event.end.win"));
    CHECK(t.queued("event.colour.white"));
    CHECK(!t.said("event.colour.white"));
}

// The human's mate before the turn's A0 is in: the review of that move uses the A0 stopped when
// it was played, not a fresh full search asked once the game is over.
TEST(coach_session_game_over_keeps_the_review_analyses) {
    Table t;
    t.game.resetFromFEN(kMateFen);
    t.analyst.delay = 150;
    t.start(levelConfig(3));
    t.run(0.2f);
    const std::string root = fenOf(kMateFen);
    CHECK_EQ(t.analyst.count("A0", root), 1);
    CHECK_EQ(t.analyst.count("A3", root), 1);
    t.move("a1a8");
    t.session.onGameOver(t.game, false);
    CHECK(t.until([&] { return t.session.handshakeWanted(); }, 60.0f));
    CHECK(t.said("event.end.win"));
    CHECK_EQ(t.analyst.count("A0", root), 1);

    // A resignation abandons the review: its A1 / A2 go too.
    Table u;
    hangTable(u);
    u.analyst.delay = 150;
    u.start(levelConfig(4));
    CHECK(u.quiet());
    u.move("g2g3");   // not among A0's lines: A1, and A2 at level 4
    CHECK(u.until([&] { return u.analyst.count("A1") == 1 && u.analyst.count("A2") == 1; }, 10.0f));
    u.session.onGameOver(u.game, true);
    for (const fake::Analyst::Job& j : u.analyst.jobs) CHECK(j.shape != "A1" && j.shape != "A2");
    CHECK(u.until([&] { return u.session.handshakeWanted(); }, 60.0f));
    CHECK(u.said("event.end.resigned"));
}

TEST(coach_session_rules_lesson_chapter) {
    Table t;
    SessionConfig cfg = levelConfig(0);
    cfg.lessonChapter = 1;   // "pieces": the rook first
    t.start(cfg);
    const Lesson lesson;
    int expect = -1;
    CHECK(t.until([&] { return t.session.director().waitingMove(&expect); }, 120.0f));
    CHECK(t.said("event.lesson.resume"));
    CHECK(t.game.position().fen().rfind("7k/8/n7/8/8/8/8/R3K3 w", 0) == 0);
    CHECK_EQ(lesson.chapterOf(expect), 1);
    CHECK_EQ(t.session.lessonChapter(), 1);
    bool slow = true, prefetched = false;
    for (const fake::Stage::Req& r : t.stage.reqs) {
        slow = slow && std::fabs(r.speed - kLessonSpeechSpeed) < 1e-6f;
        prefetched = prefetched || r.priority == 0;   // the next chapter, ahead
    }
    CHECK(slow);
    CHECK(prefetched);
    CHECK(t.session.playerMayMove(t.game));

    // A wrong move: answered with the move on the board, taken back by hand, then the same wait.
    t.move("a1a3");
    CHECK(!t.session.playerMayMove(t.game));
    CHECK(t.until([&] { return t.stage.count("takeBack") == 1; }, 30.0f));
    CHECK(t.said("lesson.rook.short"));
    CHECK(t.saidAt("lesson.rook.short") < t.stage.all("takeBack").front().t);
    CHECK(t.until([&] { return !t.stage.tableBusy(); }, 5.0f));
    CHECK_EQ(t.game.moves().size(), size_t(0));
    int again = -1;
    CHECK(t.until([&] { return t.session.playerMayMove(t.game); }, 30.0f));
    CHECK(t.session.director().waitingMove(&again) && again == expect);

    // An illegal attempt is explained at once.
    t.session.onIllegalAttempt(t.game, parseSquare("a1"), parseSquare("b2"));
    CHECK(t.until([&] { return t.said("why.WrongGeometry"); }, 10.0f));

    // The right move: praised, and the lesson goes on with the next position.
    t.move("a1a6");
    CHECK(t.until([&] { return t.said("lesson.rook.ok"); }, 30.0f));
    CHECK(t.until([&] {
        for (const fake::Stage::Ev& e : t.stage.all("setPosition"))
            if (e.text == "7k/8/n7/8/P7/8/8/R3K3 w - - 0 1") return true;
        return false;
    }, 60.0f));

    // The next exercise: after 20 s without a touch, its question again (Low), once.
    int next = -1;
    CHECK(t.until([&] { return t.session.director().waitingMove(&next) && next != expect && t.session.director().idle() == false && !t.session.director().busy(); }, 120.0f));
    const size_t asked = t.lines.size();
    t.run(21.0f);
    CHECK(t.lines.size() > asked);
    CHECK(t.lines.back().key == lesson.expectation(next).ask.key || t.lines.back().key == lesson.expectation(next).hint1.key);
    t.run(5.0f);
    const size_t hinted = t.lines.size();
    t.session.onPlayerActive();
    t.run(10.0f);
    CHECK_EQ(t.lines.size(), hinted);   // nothing new before the second hint's 45 s
    CHECK(!t.session.lessonCompleted());
    checkRenders(t.lines);
}
