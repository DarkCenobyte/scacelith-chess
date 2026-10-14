// Coach mode's challenges, the paths the other challenge tests leave out (src/coach/challenge.cpp,
// challenge_run.cpp): every error the file parser reports (a bad or repeated header, an unknown
// entry, a position outside a set or without a source, a bad line, 'also' or play-out field, a set
// left empty); the task line of a set without a mate (its own text, else the default one); a move
// made outside a wait, taken back; the answers to a line's wrong move (stalemate on the board, no
// engine, no analysis, the coach mating at once, a mate kept but slower or another one, a win kept
// but weaker), the offer closed by a move or by H; the play-outs settled by the board or by the
// engine (a hold drawn by the player's own move, a promotion kept, a hold lost or held to its
// end, no analysis), a judgement that takes too long stopped, a hint asked for before its move is
// known and given once it is, a hint for a move that is not legal saying nothing, and stop()
// cancelling what the run asked of the engine. On the fake Stage and Analyst (tests/coach_fakes.h).
#include "test.h"
#include "coach_fakes.h"

#include "coach/catalog.h"
#include "coach/challenge.h"
#include "coach/challenge_run.h"
#include "coach/director.h"

#include <functional>
#include <string>
#include <vector>

using namespace coach;

namespace {

// A set without a mate (its own task line), a set nobody wrote texts for (the default task line),
// and play-outs for the board's and the engine's verdicts.
const char* kBook =
    "challenge fork tactics 2\n"
    "line t:mate 0 | 4r1k1/5ppp/8/8/8/8/6PP/3R3K w - - 0 1 | - | d1d8\n"
    "line t:stale 0 | 7k/8/6K1/8/8/8/8/5Q2 w - - 0 1 | - | f1f8\n"
    "line t:two 0 | 6k1/8/8/8/8/8/1R6/R5K1 w - - 0 1 | - | b2b7 g8f8 a1a8\n"
    "challenge zz_untold tactics 1\n"
    "line t:one 0 | 6k1/8/8/8/8/8/1R6/R5K1 w - - 0 1 | - | b2b7\n"
    "challenge endgame endgames 2\n"
    "play e:draw | 8/8/8/8/4k3/8/3pK3/8 w - - 0 1 | hold 3\n"
    "play e:queen | 8/4P2k/8/8/8/8/8/4K3 w - - 0 1 | promote\n"
    "play e:hold | 8/8/8/4k3/4p3/8/8/4K3 w - - 0 1 | hold 1\n"
    "challenge kq endgames 1\n"
    "play e:kq | 7k/8/6K1/8/8/8/8/5Q2 w - - 0 1 | mate\n";

ai::Analysis scoreAnalysis(int cp, int mate, const char* best) {
    ai::Analysis a;
    a.ok = true;
    a.depth = 20;
    ai::PvLine l;
    l.depth = 20;
    l.multipv = 1;
    l.score.cp = cp;
    l.score.mate = mate;
    l.pv.push_back(best);
    a.lines.push_back(l);
    a.bestMove = best;
    return a;
}

std::string fenAfter(const char* fen, const char* uci) {
    chess::Position p;
    p.setFEN(fen);
    p.makeMove(p.parseUCI(uci));
    return p.fen();
}

const Challenge& bookChallenge(const ChallengeBook& b, const char* id) {
    const Challenge* c = b.find(id);
    CHECK(c != nullptr);
    static const Challenge none;
    return c ? *c : none;
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
    void pass(float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) step();
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
    // The wrong move's reaction is over: taken back, waiting again (or offering a hint).
    bool back(size_t plies) {
        return until([&] { return game.moves().size() == plies && (run.playerMayMove(game) || run.offerOpen()); },
                     60.0f);
    }
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

}  // namespace

TEST(coach_core_session_challenge_parse_errors) {
    const char* fen = "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1";
    const std::string text = std::string() +
                             "line a 0 | " + fen + " | - | a1a8\n" +                // 1: outside a set
                             "challenge broken mates\n" +                           // 2: no level
                             "challenge one mates 1\n" +                            // 3
                             "line | " + fen + " | - | a1a8\n" +                    // 4: no source
                             "line b 0 | " + fen + " | -\n" +                       // 5: three fields
                             "line c 0 | " + fen + " | - | a1a8 | maybe 0=a1a7\n" + // 6: not 'also'
                             "line d 0 | " + fen + " | - | a1a8 | also 3=a1a7\n" +  // 7: no move 3
                             "line e 0 | " + fen + " | - | a1a8 | also a1a7\n" +    // 8: no index
                             "play f | 7k/8/6K1/8/8/8/8/5Q2 w - - 0 1 | fly\n" +     // 9: unknown goal
                             "play g | 7k/8/6K1/8/8/8/8/5Q2 w - - 0 1 | line\n" +    // 10: a line is no play-out
                             "puzzle h 0 | " + fen + " | - | a1a8\n" +              // 11: unknown entry
                             "line ok 0 | " + fen + " | - | a1a8\n" +               // 12: fine
                             "challenge one tactics 2\n" +                          // 13: twice
                             "line i 0 | " + fen + " | - | a1a8\n" +                // 14: no set open
                             "challenge empty mates 1\n";                           // 15: left empty
    std::vector<std::string> errors;
    const ChallengeBook b = ChallengeBook::parse(text, &errors);
    const std::vector<std::string> want = {
        "line 1: a position outside a challenge",
        "line 2: expected: challenge <id> <group> <level>",
        "line 4: no source",
        "line 5: expected: line <source> <rating> | <fen> | <lead or -> | <moves> [| also ...]",
        "line 6: the fifth field starts with 'also'",
        "line 7: bad 'also' field",
        "line 8: bad 'also' field",
        "line 9: expected: play <source> | <fen> | mate|promote|hold [<moves>]",
        "line 10: expected: play <source> | <fen> | mate|promote|hold [<moves>]",
        "line 11: unknown entry 'puzzle'",
        "line 13: challenge one defined twice",
        "line 14: a position outside a challenge",
        "challenge empty has no position",
    };
    CHECK_EQ(errors.size(), want.size());
    for (size_t i = 0; i < want.size() && i < errors.size(); ++i) CHECK_EQ(errors[i], want[i]);
    // Only the first 'one' stays, with its one good position; the empty set is gone.
    CHECK_EQ(b.challenges().size(), size_t(1));
    CHECK_EQ(b.indexOf("empty"), -1);
    const Challenge& one = bookChallenge(b, "one");
    CHECK_EQ(one.group, std::string("mates"));
    CHECK_EQ(one.positions.size(), size_t(1));
    if (!one.positions.empty()) CHECK_EQ(one.positions[0].source, std::string("ok"));
    // Without a list to fill, the same text parses the same.
    CHECK_EQ(ChallengeBook::parse(text).write(), b.write());
}

TEST(coach_core_session_challenge_task_lines_and_a_move_outside_a_wait) {
    std::vector<std::string> errors;
    const ChallengeBook b = ChallengeBook::parse(kBook, &errors);
    CHECK(errors.empty());
    {
        // A set with its own task text: "Find the fork." (no mate in the line).
        Run t;
        t.start(bookChallenge(b, "fork"));
        CHECK_EQ(t.run.challenge().id, std::string("fork"));
        CHECK_EQ(t.run.positions(), 3);
        // A move before the set-up stands (the scene should not allow it): taken back, unjudged.
        t.move("e2e4");
        CHECK_EQ(t.stage.count("takeBack"), 1);
        CHECK(t.stage.all("takeBack").front().n == 1);
        CHECK(t.analyst.asked.empty());
        CHECK(t.ready());
        CHECK_EQ(t.queued("ch.task.fork"), 1);
        CHECK_EQ(t.queued("ch.task.default"), 0);
        CHECK(t.rendersClean());
    }
    {
        // A set nobody wrote texts for: no introduction to say, the default task.
        Run t;
        t.start(bookChallenge(b, "zz_untold"));
        CHECK(t.ready());
        CHECK_EQ(t.queued("ch.task.default"), 1);
        CHECK_EQ(t.queued("ch.intro.zz_untold"), 1);   // queued, but it has no text
        // Nothing was synthesised for the introduction: the first speech is the task.
        const auto requests = t.stage.all("request");
        CHECK(!requests.empty());
        if (!requests.empty()) CHECK_EQ(requests.front().text, std::string("Find the best move."));
        // The line's only move: solved, and the set is complete (no mate: the plain praise).
        t.move("b2b7");
        CHECK(t.run.completed());
        CHECK_EQ(t.queued("ch.solved"), 1);
        CHECK_EQ(t.queued("ch.solved.mate"), 0);
        CHECK(t.until([&] { return t.run.finished(); }, 60.0f));
    }
}

TEST(coach_core_session_challenge_line_wrong_answers) {
    const ChallengeBook b = ChallengeBook::parse(kBook);
    const char* mateFen = "4r1k1/5ppp/8/8/8/8/6PP/3R3K w - - 0 1";
    const char* twoFen = "6k1/8/8/8/8/8/1R6/R5K1 w - - 0 1";
    Run t;
    // 1.Rd2?? Re1#: the coach's answer mates on the board.
    t.analyst.results[fenAfter(mateFen, "d1d2") + "|A0"] = scoreAnalysis(0, 1, "e8e1");
    // 1.Rd4: the search came back empty.
    ai::Analysis failed;
    failed.ok = false;
    t.analyst.results[fenAfter(mateFen, "d1d4") + "|A0"] = failed;
    t.start(bookChallenge(b, "fork"));
    CHECK(t.ready());

    t.move("d1d2");
    CHECK(t.back(0));
    CHECK_EQ(t.queued("ch.wrong.mate"), 1);
    CHECK_EQ(t.stage.all("demoMove").back().text, std::string("e8e1"));
    // No engine: the move is simply not the one.
    t.analyst.engine = false;
    t.move("d1d3");
    CHECK(t.back(0));
    CHECK_EQ(t.queued("ch.wrong.plain"), 1);
    t.analyst.engine = true;
    // No analysis: the same; the third wrong move brings the offer.
    t.move("d1d4");
    CHECK(t.back(0));
    CHECK_EQ(t.queued("ch.wrong.plain"), 2);
    CHECK(t.until([&] { return t.run.offerOpen(); }, 30.0f));
    // H while the card shows: the card goes, the hint comes (the piece).
    CHECK(t.run.hintAvailable(t.game));
    t.run.requestHint(t.game);
    CHECK(!t.run.offerOpen());
    CHECK_EQ(t.queued("ch.no_hint"), 0);
    CHECK_EQ(t.queued("ch.hint.piece"), 1);
    CHECK(t.ready());
    t.move("d1d8");
    CHECK_EQ(t.queued("ch.solved"), 1);

    // Stalemate on the board: said at once, no engine asked.
    CHECK(t.ready());
    const size_t asked = t.analyst.asked.size();
    t.move("f1f7");
    CHECK(t.back(0));
    CHECK_EQ(t.queued("ch.wrong.stalemate"), 1);
    CHECK_EQ(t.analyst.asked.size(), asked);
    t.move("f1f8");
    CHECK_EQ(t.queued("ch.solved.mate"), 1);

    // A mate kept, but slower than the two moves asked; another mate as fast; a win kept, weaker.
    CHECK(t.ready());
    CHECK_EQ(t.run.position(), 2);
    const std::string wrong = fenAfter(twoFen, "g1h1");
    t.analyst.results[wrong + "|A0"] = scoreAnalysis(0, -2, "g8h8");
    t.move("g1h1");
    CHECK(t.back(0));
    CHECK_EQ(t.queued("ch.wrong.slower"), 1);
    t.analyst.results[wrong + "|A0"] = scoreAnalysis(0, -1, "g8h8");
    t.move("g1h1");
    CHECK(t.back(0));
    CHECK_EQ(t.queued("ch.wrong.other"), 1);
    t.analyst.results[wrong + "|A0"] = scoreAnalysis(-600, 0, "g8h8");
    t.move("g1h1");
    CHECK(t.back(0));
    CHECK_EQ(t.queued("ch.wrong.weaker"), 1);
    CHECK_EQ(t.stage.count("demoMove"), 1);   // only the mate was shown: the others are said
    // The offer of the third wrong move, answered by a move (no touch reported): the card goes.
    CHECK(t.until([&] { return t.run.offerOpen(); }, 30.0f));
    t.move("b2b7");
    CHECK(!t.run.offerOpen());
    CHECK_EQ(t.queued("ch.no_hint"), 0);
    CHECK(t.ready());
    CHECK_EQ(t.queued("ch.good"), 1);
    t.move("a1a8");
    CHECK(t.run.completed());
    CHECK(t.rendersClean());
}

TEST(coach_core_session_challenge_play_outs_settled) {
    const ChallengeBook b = ChallengeBook::parse(kBook);
    const char* queenFen = "8/4P2k/8/8/8/8/8/4K3 w - - 0 1";
    const char* holdFen = "8/8/8/4k3/4p3/8/8/4K3 w - - 0 1";
    Run t;
    // The promotion with the win kept (Black is mated in 8 whatever it does).
    t.analyst.results[fenAfter(queenFen, "e7e8q") + "|A0"] = scoreAnalysis(0, -8, "h7g7");
    // The king walking away: lost (Black mates in 10); the king in front of the pawn: held.
    t.analyst.results[fenAfter(holdFen, "e1f1") + "|A0"] = scoreAnalysis(0, 10, "e5d4");
    t.analyst.results[fenAfter(holdFen, "e1e2") + "|A0"] = scoreAnalysis(0, 0, "e5d5");
    t.start(bookChallenge(b, "endgame"));
    CHECK(t.ready());
    CHECK_EQ(t.queued("ch.task.play.hold"), 1);

    // Taking the last pawn: a draw on the board, the hold solved without the engine's verdict.
    t.move("e2d2");
    CHECK_EQ(t.queued("ch.solved.draw"), 1);
    CHECK_EQ(t.analyst.count("A0", t.game.position().fen()), 0);
    CHECK(t.until([&] { return t.run.position() == 1; }, 30.0f));

    // The promotion: no analysis first (a plain "no"), then the queen with the win kept.
    CHECK(t.ready());
    CHECK_EQ(t.queued("ch.task.play.promote"), 1);
    ai::Analysis failed;
    failed.ok = false;
    t.analyst.results[fenAfter(queenFen, "e1d2") + "|A0"] = failed;
    t.move("e1d2");
    CHECK(t.back(0));
    CHECK_EQ(t.queued("ch.wrong.plain"), 1);
    t.move("e7e8q");
    CHECK(t.until([&] { return t.queued("ch.solved.promote") == 1; }, 30.0f));
    CHECK(t.until([&] { return t.run.position() == 2; }, 30.0f));

    // The hold of one move: lost (its answer shown), then held.
    CHECK(t.ready());
    t.move("e1f1");
    CHECK(t.back(0));
    CHECK_EQ(t.queued("ch.wrong.loses"), 1);
    CHECK_EQ(t.stage.all("demoMove").back().text, std::string("e5d4"));
    t.move("e1e2");
    CHECK(t.until([&] { return t.run.completed(); }, 30.0f));
    CHECK_EQ(t.queued("ch.solved.hold"), 1);
    CHECK(t.until([&] { return t.run.finished(); }, 60.0f));
    CHECK(t.rendersClean());
}

TEST(coach_core_session_challenge_slow_engine) {
    const ChallengeBook b = ChallengeBook::parse(kBook);
    const char* fen = "7k/8/6K1/8/8/8/8/5Q2 w - - 0 1";
    {
        // A hint asked for before the play-out's analysis came back: given once it is.
        Run t;
        t.analyst.delay = 100000;
        t.start(bookChallenge(b, "kq"));
        CHECK(t.ready());
        CHECK(t.run.hintAvailable(t.game));
        t.run.requestHint(t.game);
        CHECK(!t.run.hintAvailable(t.game));   // already asked
        CHECK_EQ(t.analyst.jobs.size(), size_t(1));
        for (fake::Analyst::Job& j : t.analyst.jobs) j.left = 30;   // in half a second
        t.pass(0.4f);
        CHECK_EQ(t.queued("ch.hint."), 0);
        CHECK(t.until([&] { return t.queued("ch.hint.") == 1; }, 1.0f));
        CHECK(t.ready());
        CHECK_EQ(t.game.moves().size(), size_t(0));
    }
    {
        // A judgement that takes too long: stopped after six seconds, its best so far decides.
        Run t;
        t.analyst.results[fenAfter(fen, "f1e2") + "|A0"] = scoreAnalysis(0, -1, "h8g8");
        t.start(bookChallenge(b, "kq"));
        CHECK(t.ready());
        t.analyst.delay = 100000;
        t.move("f1e2");
        t.pass(5.5f);
        CHECK_EQ(t.game.moves().size(), size_t(1));   // still judging
        CHECK(!t.run.playerMayMove(t.game));
        CHECK(t.ready());
        CHECK_EQ(t.game.moves().size(), size_t(2));   // the coach's answer from the stopped search
        CHECK_EQ(t.stage.all("playLessonMove").back().text, std::string("h8g8"));
    }
    {
        // Leaving: the analyses the run asked for are cancelled, nothing more is done.
        Run t;
        t.analyst.delay = 100000;
        t.start(bookChallenge(b, "kq"));
        CHECK(t.ready());
        t.move("f1e2");
        CHECK_EQ(t.analyst.jobs.size(), size_t(2));   // the hint's search and the judgement
        t.run.stop();
        CHECK(t.analyst.jobs.empty());
        t.pass(1.0f);
        CHECK(t.analyst.jobs.empty());
        CHECK_EQ(t.game.moves().size(), size_t(1));
        t.run.stop();   // twice: nothing
    }
}

TEST(coach_core_session_challenge_hint_of_an_illegal_move) {
    // A set made by hand (not read from a file, where the line is checked): a hint about a move
    // that is not legal there says nothing.
    Challenge c;
    c.id = "fork";
    ChallengePosition p;
    p.fen = "6k1/8/8/8/8/8/1R6/R5K1 w - - 0 1";
    p.line = {"b2b9"};
    c.positions.push_back(p);
    Run t;
    t.start(c);
    CHECK(t.ready());
    CHECK(t.run.hintAvailable(t.game));
    const int gestures = t.stage.count("gesture");
    t.run.requestHint(t.game);
    t.pass(1.0f);
    CHECK_EQ(t.queued("ch.hint."), 0);
    CHECK_EQ(t.stage.count("gesture"), gestures);
    CHECK(t.run.playerMayMove(t.game));
}
