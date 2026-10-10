// Coach mode's challenges (src/coach/challenge.*, challenge_run.*, the session's routing): the file
// format (parse, write, the errors it reports), the mirror of a position, the hint steps, the moves
// a line accepts; a run on the fake Stage and Analyst (tests/coach_fakes.h): positions one after
// the other with the coach's move into them, a wrong move answered with the move on the board and
// taken back, no hint unless asked (the offer after three wrong moves, its card, H at any time: the
// piece, the square, the move shown), a play-out judged by the engine and by the board (stalemate,
// mate), the coach's answer that ends a play-out on the board (a hold solved once, nothing left
// waiting; a mate said as one), a play-out's hint never given late (asked for, then a move made
// before its analysis came back) nor offered without a move to show; the embedded book: it parses
// with no error, every set has its texts, every line ends with the player's move (in checkmate for
// the mate sets); a whole set solved through the session: completed, the handshake wanted, nothing
// recorded.
#include "test.h"
#include "coach_fakes.h"

#include "coach/catalog.h"
#include "coach/challenge.h"
#include "coach/challenge_run.h"
#include "coach/director.h"
#include "coach/session.h"
#include "core/embedded.h"

#include <functional>
#include <string>
#include <vector>

using namespace coach;
using chess::parseSquare;

namespace {

const char* kBook =
    "# a comment\n"
    "challenge mate1 mates 1\n"
    "line lichess:00001 900 | 6k1/5ppp/8/8/8/8/n4PPP/R5K1 b - - 0 1 | a2c3 | a1a8\n"
    "line scacelith:t1 0 | 6k1/8/8/8/8/8/1R6/R5K1 w - - 0 1 | - | b2b7 g8f8 a1a8 | also 1=a1a7\n"
    "\n"
    "challenge kq endgames 2\n"
    "play scacelith:kq1 | 7k/8/6K1/8/8/8/8/5Q2 w - - 0 1 | mate\n"
    "play scacelith:hold1 | 8/8/8/4k3/4p3/8/8/4K3 w - - 0 1 | hold 10\n";

ai::Analysis mateAnalysis(int mate, const char* best) {
    ai::Analysis a;
    a.ok = true;
    a.depth = 20;
    ai::PvLine l;
    l.depth = 20;
    l.multipv = 1;
    l.score.mate = mate;
    l.pv.push_back(best);
    a.lines.push_back(l);
    a.bestMove = best;
    return a;
}

// A level line (no mate, cp 0): the draw holds, whoever is to move; 'best' is the coach's answer.
ai::Analysis levelAnalysis(const char* best) {
    ai::Analysis a = mateAnalysis(0, best);
    a.depth = a.lines.front().depth = 22;
    return a;
}

std::string fenAfter(const char* fen, const char* uci) {
    chess::Position p;
    p.setFEN(fen);
    p.makeMove(p.parseUCI(uci));
    return p.fen();
}

// A challenge run on the fake world, as the scene drives it (the coach's moves reported as moves).
struct Run {
    fake::Stage stage;
    fake::Analyst analyst;
    Director director;
    ChallengeRun run;
    chess::Game game;
    float dt = 1.0f / 60.0f;
    std::vector<Line> lines;

    Run() {
        stage.game = &game;
        game.setEndDetection(false);
        DirectorConfig c;
        c.uiLanguage = "en";
        director.reset(&stage, c);
        director.setObserver([this](const Beat& b) {
            if (!b.line.key.empty()) lines.push_back(b.line);
        });
    }
    void start(const Challenge& ch, int from = 0) { run.start(ch, director, stage, analyst, from); }
    void step() {
        stage.advance(dt);
        if (stage.lessonMoves > 0) {
            stage.lessonMoves = 0;
            run.onMove(game);
        }
        run.update(game, dt);
        director.update(dt, int(game.moves().size()));
    }
    bool until(const std::function<bool()>& cond, float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) {
            if (cond()) return true;
            step();
        }
        return cond();
    }
    bool ready() { return until([&] { return run.playerMayMove(game); }, 120.0f); }
    void move(const char* uci) {
        const chess::Move m = game.position().parseUCI(uci);
        CHECK(m.valid());
        if (!m.valid()) return;
        game.play(m);
        run.onMove(game);
    }
    int queued(const std::string& prefix) const {
        int n = 0;
        for (const Line& l : lines)
            if (l.key.rfind(prefix, 0) == 0) ++n;
        return n;
    }
    // The wrong move's reaction is over: taken back, waiting again.
    bool backAndWaiting(size_t plies) {
        return until([&] { return game.moves().size() == plies && run.playerMayMove(game); }, 60.0f);
    }
};

// Every line renders, written and spoken, every variant, in every language that has the key,
// without a placeholder left.
void checkRenders(const std::vector<Line>& lines) {
    const Catalog& cat = Catalog::shared();
    int rendered = 0;
    for (const Line& l : lines) {
        CHECK(cat.has(l.key));
        for (const std::string& lang : cat.languages()) {
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

const Challenge& bookChallenge(const ChallengeBook& b, const char* id) {
    const Challenge* c = b.find(id);
    CHECK(c != nullptr);
    static const Challenge none;
    return c ? *c : none;
}

}  // namespace

TEST(challenge_book_parse_and_write) {
    std::vector<std::string> errors;
    const ChallengeBook b = ChallengeBook::parse(kBook, &errors);
    CHECK(errors.empty());
    CHECK_EQ(b.challenges().size(), size_t(2));
    const Challenge& m = bookChallenge(b, "mate1");
    CHECK_EQ(m.group, std::string("mates"));
    CHECK_EQ(m.level, 1);
    CHECK_EQ(m.positions.size(), size_t(2));
    CHECK_EQ(m.positions[0].lead, std::string("a2c3"));
    CHECK_EQ(m.positions[0].rating, 900);
    CHECK(m.positions[0].endsInMate());
    CHECK_EQ(m.positions[1].playerMoves(), 2);
    CHECK(m.positions[1].lead.empty());
    const Challenge& kq = bookChallenge(b, "kq");
    CHECK(kq.positions[0].playOut());
    CHECK(kq.positions[0].goal == ChallengeGoal::Mate);
    CHECK(kq.positions[1].goal == ChallengeGoal::Hold);
    CHECK_EQ(kq.positions[1].moves, 10);
    CHECK_EQ(b.indexOf("kq"), 1);
    CHECK_EQ(b.indexOf("nope"), -1);
    CHECK(b.groups() == std::vector<std::string>({"mates", "endgames"}));
    // Written back, it reads the same.
    std::vector<std::string> again;
    const ChallengeBook c = ChallengeBook::parse(b.write(), &again);
    CHECK(again.empty());
    CHECK_EQ(c.write(), b.write());
}

TEST(challenge_book_reports_bad_lines) {
    std::vector<std::string> errors;
    const ChallengeBook b = ChallengeBook::parse(
        "challenge x tactics 1\n"
        "line a 0 | 6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1 | a2c3 | a1a8\n"              // a lead with White to move
        "line b 0 | 6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1 | - | a1a8 g8f8\n"            // ends with the coach's move
        "line c 0 | 6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1 | - | a1a9\n"                 // not a move
        "play d | 7k/8/6K1/8/8/8/8/5Q2 w - - 0 1 | hold\n"                             // hold without moves
        "play f | 7k/8/6K1/8/8/8/8/5B2 w - - 0 1 | mate\n"                             // no mate possible
        "line e 0 | 6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1 | - | a1a8\n",
        &errors);
    CHECK_EQ(errors.size(), size_t(5));
    CHECK_EQ(b.challenges().size(), size_t(1));
    CHECK_EQ(b.challenges()[0].positions.size(), size_t(1));
    CHECK_EQ(b.challenges()[0].positions[0].source, std::string("e"));
}

TEST(challenge_mirror) {
    CHECK_EQ(mirrorUci("e2e4"), std::string("e7e5"));
    CHECK_EQ(mirrorUci("a7a8q"), std::string("a2a1q"));
    // Ranks reversed, colours and castling rights swapped, the en passant square follows.
    CHECK_EQ(mirrorFen("rnbqkbnr/pppp1ppp/8/8/4Pp2/8/PPPP2PP/RNBQKBNR b Kq e3 0 3"),
             std::string("rnbqkbnr/pppp2pp/8/4pP2/8/8/PPPP1PPP/RNBQKBNR w Qk e6 0 3"));
    const std::string fen = "r1bq1rk1/1ppn1p2/p2bN2p/3n4/2BQP2B/8/PP3PPP/R4RK1 b - - 1 15";
    CHECK_EQ(mirrorFen(mirrorFen(fen)), fen);
}

TEST(challenge_hints) {
    chess::Position p;
    // Several pieces can move: the piece, the square, then the move.
    p.setFEN("6k1/8/8/8/8/8/1R6/R5K1 w - - 0 1");
    CHECK_EQ(challengeHintSteps(p, "b2b7"), 3);
    ChallengeHint h = challengeHint(p, "b2b7", 1);
    CHECK(h.kind == ChallengeHint::Kind::Piece);
    CHECK_EQ(h.piece, parseSquare("b2"));
    h = challengeHint(p, "b2b7", 2);
    CHECK(h.kind == ChallengeHint::Kind::Square);
    CHECK_EQ(h.square, parseSquare("b7"));
    h = challengeHint(p, "b2b7", 3);
    CHECK(h.kind == ChallengeHint::Kind::Show);
    CHECK(challengeHint(p, "b2b7", 7).kind == ChallengeHint::Kind::Show);
    // Only the king can move: naming it says nothing, the square first.
    p.setFEN("k7/8/8/2Q5/8/8/8/7K b - - 0 1");
    CHECK_EQ(challengeHintSteps(p, "a8b8"), 2);
    h = challengeHint(p, "a8b8", 1);
    CHECK(h.kind == ChallengeHint::Kind::Square);
    CHECK_EQ(h.square, parseSquare("b8"));
    CHECK(challengeHint(p, "a8b8", 2).kind == ChallengeHint::Kind::Show);
}

TEST(challenge_accepted_moves) {
    const ChallengeBook b = ChallengeBook::parse(kBook);
    const ChallengePosition& two = bookChallenge(b, "mate1").positions[1];
    chess::Position p;
    CHECK(two.beforeMove(0, p));
    CHECK(two.accepts(0, p, "b2b7"));
    CHECK(!two.accepts(0, p, "a1a7"));   // 'also' is for the last move only
    CHECK(two.beforeMove(1, p));
    CHECK(two.accepts(1, p, "a1a8"));
    CHECK(two.accepts(1, p, "a1a7"));    // also
    CHECK(!two.accepts(1, p, "a1a6"));
    // Any checkmate is accepted, in the line or not.
    const ChallengePosition& one = bookChallenge(b, "mate1").positions[0];
    CHECK(one.start(p));
    CHECK(one.accepts(0, p, "a1a8"));
}

TEST(challenge_run_line_wrong_moves_and_hints) {
    const ChallengeBook b = ChallengeBook::parse(kBook);
    Run t;
    t.start(bookChallenge(b, "mate1"));
    // The set's introduction, the set-up, the coach's move into it, the task, then the wait.
    CHECK(t.ready());
    CHECK(t.queued("ch.intro.mate1") == 1);
    CHECK(t.queued("ch.lead.first") == 1);
    CHECK_EQ(t.stage.all("setPosition").front().text, std::string("6k1/5ppp/8/8/8/8/n4PPP/R5K1 b - - 0 1"));
    CHECK_EQ(t.stage.all("playLessonMove").front().text, std::string("a2c3"));
    CHECK(t.queued("ch.task.mate") == 1);
    CHECK_EQ(t.run.position(), 0);
    CHECK(t.run.hintAvailable(t.game));
    // Solved at once: the next position (no lead: White moves at once).
    t.move("a1a8");
    CHECK(!t.run.playerMayMove(t.game));
    CHECK(t.ready());
    CHECK(t.queued("ch.solved.mate") == 1);
    CHECK(t.queued("ch.last") == 1);
    CHECK_EQ(t.run.position(), 1);
    CHECK_EQ(t.game.moves().size(), size_t(0));

    // Wrong moves: the move stays on the board while the coach answers, then goes back; no hint,
    // no offer before the third one.
    for (int i = 0; i < 2; ++i) {
        t.move("g1h1");
        CHECK(!t.run.playerMayMove(t.game));
        CHECK(t.backAndWaiting(0));
    }
    CHECK_EQ(t.stage.count("takeBack"), 2);
    CHECK_EQ(t.stage.count("demoMove"), 2);   // its answer, by hand
    CHECK(t.queued("ch.wrong.reply") == 2);
    CHECK(t.queued("ch.offer") == 0);
    CHECK(t.queued("ch.hint.") == 0);
    // The third: the offer, a question with its card. Touching a piece declines it.
    t.move("g1h1");
    CHECK(t.until([&] { return t.run.offerOpen(); }, 60.0f));
    CHECK(t.queued("ch.offer") == 1);
    CHECK(!t.run.playerMayMove(t.game));
    t.run.onPlayerActive();
    CHECK(!t.run.offerOpen());
    CHECK(t.queued("ch.no_hint") == 1);
    CHECK(t.queued("ch.hint.") == 0);
    CHECK(t.ready());
    // Three more: offered again, accepted: the piece, pointed at.
    for (int i = 0; i < 3; ++i) {
        t.move("g1h1");
        if (i < 2) CHECK(t.backAndWaiting(0));
    }
    CHECK(t.until([&] { return t.run.offerOpen(); }, 60.0f));
    CHECK(t.queued("ch.offer") == 2);
    t.run.answerOffer(true);
    CHECK(t.queued("ch.hint.piece") == 1);
    auto pointed = [&](const char* sq) {
        for (const fake::Stage::Ev& e : t.stage.all("gesture"))
            if (e.gesture.square == parseSquare(sq)) return true;
        return false;
    };
    CHECK(t.until([&] { return pointed("b2"); }, 30.0f));
    // H: the square, then the move shown by hand and taken back (no move of the player meanwhile).
    CHECK(t.until([&] { return t.run.hintAvailable(t.game); }, 60.0f));
    t.run.requestHint(t.game);
    CHECK(t.queued("ch.hint.square") == 1);
    CHECK(t.until([&] { return pointed("b7"); }, 30.0f));
    CHECK(t.until([&] { return t.run.hintAvailable(t.game); }, 60.0f));
    t.run.requestHint(t.game);
    CHECK(t.queued("ch.hint.show") == 1);
    CHECK(!t.run.playerMayMove(t.game));
    CHECK(t.until([&] {
        const auto demos = t.stage.all("demoMove");
        return !demos.empty() && demos.back().text == "b2b7";
    }, 30.0f));
    CHECK(t.ready());
    CHECK(t.queued("ch.hint.now") == 1);
    CHECK_EQ(t.game.moves().size(), size_t(0));

    // The solution: a nod and the coach's answer from the line, then the mate.
    t.move("b2b7");
    CHECK(t.ready());
    CHECK(t.queued("ch.good") == 1);
    CHECK_EQ(t.game.moves().size(), size_t(2));
    CHECK_EQ(t.stage.all("playLessonMove").back().text, std::string("g8f8"));
    CHECK(t.run.hintAvailable(t.game));
    t.move("a1a7");   // 'also' at the last move: accepted
    CHECK(t.run.completed());
    CHECK(t.until([&] { return t.run.finished(); }, 60.0f));
    CHECK(t.queued("ch.complete") == 1);
    CHECK(!t.run.hintAvailable(t.game));
    CHECK(!t.run.playerMayMove(t.game));
    checkRenders(t.lines);
}

TEST(challenge_run_line_mated) {
    // A wrong move that lets the coach mate by force: how soon, its first move shown, taken back.
    const ChallengeBook b = ChallengeBook::parse(kBook);
    const std::string led = fenAfter("6k1/5ppp/8/8/8/8/n4PPP/R5K1 b - - 0 1", "a2c3");
    Run t;
    t.analyst.results[fenAfter(led.c_str(), "a1b1") + "|A0"] = mateAnalysis(2, "c3e2");
    t.start(bookChallenge(b, "mate1"));
    CHECK(t.ready());
    t.move("a1b1");
    CHECK(t.backAndWaiting(1));   // the coach's lead move stays
    CHECK(t.queued("ch.wrong.mated") == 1);
    CHECK(t.queued("ch.wrong.reply") == 0);
    CHECK_EQ(t.stage.all("demoMove").back().text, std::string("c3e2"));
    bool counted = false;
    for (const Line& l : t.lines)
        if (l.key == "ch.wrong.mated" && l.arg("m") && l.arg("m")->number == 2) counted = true;
    CHECK(counted);
    checkRenders(t.lines);
}

TEST(challenge_run_play_out) {
    const ChallengeBook b = ChallengeBook::parse(kBook);
    const char* fen = "7k/8/6K1/8/8/8/8/5Q2 w - - 0 1";
    Run t;
    // The kept move: the coach answers with the engine's move (scripted: mated in 1).
    t.analyst.results[fenAfter(fen, "f1e2") + "|A0"] = mateAnalysis(-1, "h8g8");
    t.start(bookChallenge(b, "kq"));
    CHECK(t.ready());
    CHECK(t.queued("ch.task.play.mate") == 1);
    CHECK(t.stage.count("playLessonMove") == 0);   // no lead
    // Stalemate on the board: said without the engine, taken back.
    t.move("f1f7");
    CHECK(t.backAndWaiting(0));
    CHECK(t.queued("ch.wrong.stalemate") == 1);
    // A move that lets the win go (the fake engine's level line): its answer shown, taken back.
    t.move("f1a1");
    CHECK(t.backAndWaiting(0));
    CHECK(t.queued("ch.wrong.draw") == 1);
    // Kept: the coach plays its best defence, the next move waits.
    t.move("f1e2");
    CHECK(t.ready());
    CHECK_EQ(t.game.moves().size(), size_t(2));
    CHECK_EQ(t.stage.all("playLessonMove").back().text, std::string("h8g8"));
    // Mate on the board: solved; the next position (hold).
    t.move("e2e8");
    CHECK(t.until([&] { return t.run.position() == 1; }, 30.0f));
    CHECK(t.queued("ch.solved.mate") == 1);
    CHECK(t.ready());
    CHECK(t.queued("ch.task.play.hold") == 1);
    CHECK(!t.run.completed());
    checkRenders(t.lines);
}

// N03: a hold whose last position the coach's answer stalemates (the board has no move left for
// the player): solved once, with no wait on that position, and the challenge completed.
TEST(challenge_run_hold_coach_stalemates_last_position) {
    const char* fen = "8/8/8/8/4k3/8/4p3/5K2 w - - 0 1";
    const ChallengeBook b = ChallengeBook::parse(std::string("challenge kp_hold endgames 3\nplay t:hold | ") + fen + " | hold 10\n");
    Run t;
    t.analyst.results[fenAfter(fen, "f1e1") + "|A0"] = levelAnalysis("e4e3");
    t.start(bookChallenge(b, "kp_hold"));
    CHECK(t.ready());
    t.move("f1e1");
    CHECK(t.until([&] { return t.run.finished(); }, 120.0f));
    CHECK(t.run.completed());
    CHECK_EQ(t.stage.all("playLessonMove").back().text, std::string("e4e3"));
    CHECK(t.game.position().isStalemate());
    // Nothing waits any more, and nothing is said twice.
    t.until([] { return false; }, 10.0f);
    CHECK(!t.run.playerMayMove(t.game));
    CHECK(!t.director.waitingMove());
    CHECK(t.director.idle());
    CHECK_EQ(t.queued("ch.solved"), 1);
    CHECK_EQ(t.queued("ch.solved.draw"), 1);
    CHECK_EQ(t.queued("ch.complete"), 1);
    CHECK_EQ(t.queued("ch.wrong"), 0);
    CHECK_EQ(t.run.position(), 0);
    checkRenders(t.lines);
}

// N03: the same stalemate with a position after it: solved once, then the next position waits for
// the player's move, on its own board.
TEST(challenge_run_hold_coach_stalemates_next_position) {
    const char* fen = "8/8/8/8/4k3/8/4p3/5K2 w - - 0 1";
    const char* second = "8/8/8/4p3/4k3/8/8/3K4 w - - 0 1";
    const ChallengeBook b = ChallengeBook::parse(std::string("challenge kp_hold endgames 3\nplay t:hold | ") + fen +
                                                 " | hold 10\nplay t:hold2 | " + second + " | hold 10\n");
    Run t;
    t.analyst.results[fenAfter(fen, "f1e1") + "|A0"] = levelAnalysis("e4e3");
    t.start(bookChallenge(b, "kp_hold"));
    CHECK(t.ready());
    t.move("f1e1");
    CHECK(t.until([&] { return t.run.position() == 1 && t.run.playerMayMove(t.game); }, 120.0f));
    CHECK(!t.run.completed());
    chess::Position start;
    start.setFEN(second);
    CHECK(t.game.position().samePosition(start));
    CHECK(t.game.position().hasLegalMove());
    CHECK_EQ(t.queued("ch.solved"), 1);
    CHECK_EQ(t.queued("ch.solved.draw"), 1);
    CHECK_EQ(t.queued("ch.last"), 1);
    CHECK_EQ(t.queued("ch.task.play.hold"), 2);
    CHECK_EQ(t.queued("ch.complete"), 0);
    checkRenders(t.lines);
}

// N03: the coach's answer leaves no mating material (K+B v K): a draw on the board, the hold
// solved once, with no wait after it.
TEST(challenge_run_hold_coach_leaves_no_mating_material) {
    const char* fen = "4k3/8/8/1b6/8/8/P7/4K3 w - - 0 1";
    const ChallengeBook b = ChallengeBook::parse(std::string("challenge kp_hold endgames 3\nplay t:hold | ") + fen + " | hold 10\n");
    Run t;
    t.analyst.results[fenAfter(fen, "a2a4") + "|A0"] = levelAnalysis("b5a4");
    t.start(bookChallenge(b, "kp_hold"));
    CHECK(t.ready());
    t.move("a2a4");
    CHECK(t.until([&] { return t.run.finished(); }, 120.0f));
    CHECK(t.game.position().hasInsufficientMaterial());
    CHECK(t.game.position().hasLegalMove());
    t.until([] { return false; }, 10.0f);
    CHECK(!t.run.playerMayMove(t.game));
    CHECK(!t.director.waitingMove());
    CHECK_EQ(t.queued("ch.solved"), 1);
    CHECK_EQ(t.queued("ch.solved.draw"), 1);
    CHECK_EQ(t.queued("ch.complete"), 1);
    checkRenders(t.lines);
}

// A mate play-out whose kept move the coach answers into a dead draw (the engine said the win was
// kept): the win is gone on the board, so the move is wrong: answer shown, taken back, waiting
// again, never a wait on the drawn board.
TEST(challenge_run_play_out_answer_draws_on_the_board) {
    const char* fen = "7k/8/8/8/8/8/8/K5Q1 w - - 0 1";
    const ChallengeBook b = ChallengeBook::parse(std::string("challenge kq endgames 1\nplay t:kq | ") + fen + " | mate\n");
    Run t;
    t.analyst.results[fenAfter(fen, "g1g8") + "|A0"] = mateAnalysis(-5, "h8g8");
    t.start(bookChallenge(b, "kq"));
    CHECK(t.ready());
    t.move("g1g8");
    CHECK(t.backAndWaiting(0));
    CHECK_EQ(t.queued("ch.wrong.draw"), 1);
    CHECK_EQ(t.stage.all("demoMove").back().text, std::string("h8g8"));
    CHECK_EQ(t.stage.count("playLessonMove"), 0);
    CHECK_EQ(t.queued("ch.solved"), 0);
    CHECK(!t.run.completed());
    checkRenders(t.lines);
}

// A play-out move the coach answers with checkmate (a back-rank mate): said as a mate, proved by
// the board ("would be checkmate"), whatever the goal; the move taken back, waiting again.
TEST(challenge_run_play_out_answer_mates_on_the_board) {
    const char* fen = "r5k1/5ppp/8/7N/8/8/5PPP/6K1 w - - 0 1";
    for (const char* goal : {"mate", "hold 10"}) {
        const char* id = std::string(goal) == "mate" ? "kq" : "kp_hold";
        const ChallengeBook b =
            ChallengeBook::parse(std::string("challenge ") + id + " endgames 2\nplay t:m | " + fen + " | " + goal + "\n");
        Run t;
        t.analyst.results[fenAfter(fen, "h5f4") + "|A0"] = mateAnalysis(1, "a8a1");   // the coach mates in 1
        t.start(bookChallenge(b, id));
        CHECK(t.ready());
        t.move("h5f4");
        CHECK(t.backAndWaiting(0));
        CHECK_EQ(t.queued("ch.wrong.mate"), 1);
        CHECK_EQ(t.queued("ch.wrong.loses"), 0);
        CHECK_EQ(t.stage.all("demoMove").back().text, std::string("a8a1"));
        CHECK_EQ(t.stage.count("playLessonMove"), 0);
        CHECK(!t.run.completed());
        checkRenders(t.lines);
    }
}

// A hint asked for while the play-out's analysis runs, then a move made before it came back: the
// hint was about the position left, so it is dropped (never said while the move is judged, nor
// after the coach's answer, on another board); H gives one at the next wait.
TEST(challenge_run_play_out_hint_asked_then_moved) {
    const ChallengeBook b = ChallengeBook::parse(kBook);
    const char* fen = "7k/8/6K1/8/8/8/8/5Q2 w - - 0 1";
    Run t;
    t.analyst.results[fenAfter(fen, "f1e2") + "|A0"] = mateAnalysis(-1, "h8g8");
    t.analyst.delay = 1 << 20;   // the hint's analysis of the start position does not come back
    t.start(bookChallenge(b, "kq"));
    CHECK(t.ready());
    CHECK(t.run.hintAvailable(t.game));
    t.run.requestHint(t.game);
    CHECK(!t.run.hintAvailable(t.game));   // asked for, on its way
    t.analyst.delay = 3;
    t.move("f1e2");   // the move stops the hint's analysis: its move comes back at once
    CHECK(t.ready());
    CHECK_EQ(t.game.moves().size(), size_t(2));
    t.until([] { return false; }, 5.0f);
    CHECK_EQ(t.queued("ch.hint"), 0);
    CHECK_EQ(t.stage.count("demoMove"), 0);
    // At the next wait, H gives the hint of the new position.
    CHECK(t.until([&] { return t.run.hintAvailable(t.game); }, 60.0f));
    t.run.requestHint(t.game);
    CHECK(t.until([&] { return t.queued("ch.hint") == 1; }, 30.0f));
    checkRenders(t.lines);
}

// A play-out whose hint analysis came back without a move (twice): no hint can be given, so none
// is offered after the wrong moves (the offer would get no hint after a yes).
TEST(challenge_run_play_out_no_hint_offer_without_a_move) {
    const ChallengeBook b = ChallengeBook::parse(kBook);
    const char* fen = "7k/8/6K1/8/8/8/8/5Q2 w - - 0 1";
    chess::Position start;
    start.setFEN(fen);
    Run t;
    ai::Analysis failed;
    failed.ok = false;
    t.analyst.results[start.fen() + "|A0"] = failed;
    t.start(bookChallenge(b, "kq"));
    CHECK(t.ready());
    CHECK(t.until([&] { return t.analyst.count("A0", start.fen()) == 2 && t.analyst.idle(); }, 30.0f));
    CHECK(!t.run.hintAvailable(t.game));
    for (int i = 0; i < kHintOfferAfter; ++i) {
        t.move("f1a1");   // the win goes (the fake engine's level line)
        CHECK(t.backAndWaiting(0));
    }
    t.until([] { return false; }, 5.0f);
    CHECK_EQ(t.queued("ch.wrong.draw"), kHintOfferAfter);
    CHECK_EQ(t.queued("ch.offer"), 0);
    CHECK_EQ(t.queued("ch.try_again"), kHintOfferAfter);
    CHECK(!t.run.offerOpen());
    CHECK(t.run.playerMayMove(t.game));
    checkRenders(t.lines);
}

TEST(challenge_book_embedded) {
    const ChallengeBook& b = ChallengeBook::shared();
    std::vector<std::string> errors;
    const ChallengeBook again = ChallengeBook::parse(embedded::text("assets/coach/challenges/challenges.txt"), &errors);
    for (const std::string& e : errors) std::fprintf(stderr, "  %s\n", e.c_str());
    CHECK(errors.empty());
    CHECK(!b.challenges().empty());
    CHECK_EQ(again.challenges().size(), b.challenges().size());
    const Catalog& cat = Catalog::shared();
    for (const Challenge& c : b.challenges()) {
        CHECK(!c.positions.empty());
        CHECK(c.level >= 1 && c.level <= 5);
        if (!cat.has("ch.intro." + c.id)) {
            std::fprintf(stderr, "  no ch.intro.%s\n", c.id.c_str());
            CHECK(false);
        }
        for (const ChallengePosition& p : c.positions) {
            chess::Position at;
            CHECK(p.start(at));
            CHECK(at.sideToMove() == chess::White);
            if (p.playOut()) continue;
            CHECK(p.line.size() % 2 == 1);
            CHECK(p.beforeMove(p.playerMoves() - 1, at));
            if (c.group == "mates" && !p.endsInMate()) {
                std::fprintf(stderr, "  %s: %s does not end in mate\n", c.id.c_str(), p.source.c_str());
                CHECK(false);
            }
        }
    }
}

TEST(challenge_session_whole_set) {
    // The first set of the embedded book made of lines only, solved through the session.
    const ChallengeBook& b = ChallengeBook::shared();
    const Challenge* pick = nullptr;
    for (const Challenge& c : b.challenges()) {
        bool lines = true;
        for (const ChallengePosition& p : c.positions) lines = lines && !p.playOut();
        if (lines) {
            pick = &c;
            break;
        }
    }
    CHECK(pick != nullptr);
    if (!pick) return;
    fake::Stage stage;
    fake::Analyst analyst;
    Session session;
    chess::Game game;
    game.setEndDetection(false);
    stage.game = &game;
    std::vector<Line> lines;
    session.director().setObserver([&](const Beat& bt) {
        if (!bt.line.key.empty()) lines.push_back(bt.line);
    });
    SessionConfig cfg;
    cfg.level = 3;
    cfg.human = chess::Black;   // ignored: the challenges are played with White
    cfg.challenge = pick->id;
    session.start(stage, analyst, game, cfg);
    CHECK(session.challengeMode());
    CHECK_EQ(session.challengePositions(), int(pick->positions.size()));
    CHECK(session.offerIsHint());
    const float dt = 1.0f / 60.0f;
    auto step = [&] {
        stage.advance(dt);
        if (stage.lessonMoves > 0) {
            stage.lessonMoves = 0;
            session.onMove(game);
        }
        session.update(game, dt);
    };
    auto until = [&](const std::function<bool()>& cond, float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) {
            if (cond()) return true;
            step();
        }
        return cond();
    };
    for (size_t i = 0; i < pick->positions.size(); ++i) {
        const ChallengePosition& p = pick->positions[i];
        for (int k = 0; k < p.playerMoves(); ++k) {
            CHECK(until([&] { return session.playerMayMove(game); }, 120.0f));
            CHECK_EQ(session.challengePosition(), int(i));
            CHECK(session.hintAvailable(game));
            CHECK(!session.canTakeBack(game));
            CHECK(session.coachMayMove());
            const chess::Move m = game.position().parseUCI(p.line[size_t(2 * k)]);
            CHECK(m.valid());
            if (!m.valid()) return;
            game.play(m);
            session.onMove(game);
        }
    }
    CHECK(session.challengeCompleted());
    CHECK(until([&] { return session.handshakeWanted(); }, 120.0f));
    session.onHandshakeDone(game);
    CHECK(session.finished());
    CHECK(session.history().empty());
    CHECK_EQ(analyst.asked.size(), size_t(0));   // right moves need no engine
    checkRenders(lines);
}

TEST(challenge_speech_renders_everywhere) {
    // Every line of the challenges' speech, with the arguments the run gives (a count of 1, 2 and
    // 5, a mating move and a quiet one, one of the player's pieces, a square).
    const Catalog& cat = Catalog::shared();
    const std::vector<std::string> keys = cat.keys("en", "challenge");
    CHECK(keys.size() > 40);
    std::vector<Line> lines;
    for (const std::string& k : keys) {
        if (k.find(".spoken") != std::string::npos) continue;
        for (int n : {1, 2, 5}) {
            for (const char* san : {"Qxh2#", "Nf3"}) {
                Line l;
                l.key = k;
                l.with("n", Arg::ofNumber(n));
                l.with("m", Arg::ofNumber(n));
                l.with("move", Arg::ofMove(san, "h4h2"));
                l.with("your", Arg::ofPiece(chess::Knight, chess::White, true, parseSquare("g1")));
                l.with("sq", Arg::ofSquare(parseSquare("e4")));
                lines.push_back(l);
            }
        }
    }
    checkRenders(lines);
}
