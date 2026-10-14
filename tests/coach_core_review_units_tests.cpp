// Tests for the small building blocks of the coach's review that the review tests leave aside: the
// move classes' names, the result the engine expects (assessment.h: resultOf, eDrop, changesResult),
// board facts (an epaulette mate, the pawns sheltering a king, a knight's path with its long leg
// sideways), the announcement of a double check, the coach's warnings after its own move (a mate
// threat, a piece attacked by a piece the move uncovered), the hint after a free piece missed, and a
// score of an unknown kind in an engine's info line.
#include "coach_core_review_support.h"

#include "coach/assessment.h"
#include "coach/tactics.h"

#include <cmath>
#include <cstring>

using namespace chess;
using namespace coach;
using namespace corereview;

namespace {

Square sq(const char* s) { return parseSquare(s); }

Position fen(const char* f) {
    Position p;
    CHECK(p.setFEN(f));
    return p;
}

}  // namespace

TEST(coach_core_review_move_class_names) {
    // The names the logs and the audits print, one per class.
    const std::pair<MoveClass, const char*> names[] = {
        {MoveClass::Unjudged, "unjudged"}, {MoveClass::Book, "book"},           {MoveClass::Forced, "forced"},
        {MoveClass::Best, "best"},         {MoveClass::Excellent, "excellent"}, {MoveClass::Good, "good"},
        {MoveClass::Inaccuracy, "inaccuracy"}, {MoveClass::Mistake, "mistake"}, {MoveClass::Blunder, "blunder"},
    };
    for (const auto& n : names) CHECK_EQ(std::string(moveClassName(n.first)), std::string(n.second));
}

TEST(coach_core_review_expected_result) {
    // The result the engine expects with best play: a win from 50 % of wins, a loss from 50 % of
    // losses, a draw in between; forced mates are a win or a loss.
    Outcome o;
    o.win = 500;
    o.draw = 500;
    o.loss = 0;
    CHECK(resultOf(o) == ResultClass::Win);
    o.win = 499;
    o.draw = 1;
    o.loss = 500;
    CHECK(resultOf(o) == ResultClass::Loss);
    o.win = 300;
    o.draw = 400;
    o.loss = 300;
    CHECK(resultOf(o) == ResultClass::Draw);
    ai::Score mates;
    mates.mate = 3;
    CHECK(resultOf(outcomeOf(mates)) == ResultClass::Win);
    mates.mate = -2;
    CHECK(resultOf(outcomeOf(mates)) == ResultClass::Loss);

    // A win (WDL 600/400/0) thrown into a draw (0/1000/0): the class changes and E falls by 0.3.
    const std::vector<ai::PvLine> a0 = {pvlWdl(250, "e2e4", 600, 400, 0), pvlWdl(20, "d2d4", 0, 1000, 0)};
    const Assessment drawn = assess(a0, pvlWdl(0, "a2a3", 0, 1000, 0).score, false);
    CHECK(drawn.wdl);
    CHECK(std::fabs(drawn.eDrop() - 0.3) < 1e-9);
    CHECK(drawn.changesResult());
    // The best move itself drops nothing and changes nothing.
    const Assessment best = assess(a0, a0[0].score, true);
    CHECK_EQ(best.eDrop(), 0.0);
    CHECK(!best.changesResult());
    // A win kept (550/450/0): the same class, however much E falls.
    const Assessment kept = assess(a0, pvlWdl(150, "a2a3", 550, 450, 0).score, false);
    CHECK(kept.eDrop() > 0.0);
    CHECK(!kept.changesResult());
    // A move better than the "best": no negative drop.
    CHECK_EQ(assess(a0, pvlWdl(400, "a2a3", 800, 200, 0).score, false).eDrop(), 0.0);
    // Without the engine's WDL the classes come from the centipawns: no result change is claimed.
    const Assessment noWdl = assess({pvl(250, "e2e4")}, pvl(-300, "a2a3").score, false);
    CHECK(!noWdl.wdl);
    CHECK(noWdl.eDrop() > 0.15);
    CHECK(!noWdl.changesResult());
}

TEST(coach_core_review_epaulette_mate) {
    // Qe6#: the king on e8 is boxed in by its own rooks on d8 and f8, the queen checks along its file.
    CHECK(classifyMate(fen("3rkr2/8/4Q3/8/8/8/8/4K3 b - - 0 1")) == MatePattern::Epaulette);
    // The same net with White's bishop on f8 (guarded by the rook on h8): the king's own piece on
    // one side only, no epaulette.
    CHECK(classifyMate(fen("3rkB1R/8/4Q3/8/8/8/8/4K3 b - - 0 1")) == MatePattern::Other);
}

TEST(coach_core_review_king_shelter) {
    // The king's own pawns on its file and the two next to it, one and two ranks up.
    CHECK_EQ(kingShelter(fen("6k1/5ppp/8/8/8/8/5PPP/6K1 w - - 0 1"), White), 3);
    CHECK_EQ(kingShelter(fen("6k1/5p1p/6p1/8/8/7P/5PP1/6K1 w - - 0 1"), White), 3);   // h3 and g6: two ranks up
    CHECK_EQ(kingShelter(fen("6k1/5p1p/6p1/8/8/7P/5PP1/6K1 w - - 0 1"), Black), 3);
    CHECK_EQ(kingShelter(fen("6k1/8/5p2/8/8/5P2/8/6K1 w - - 0 1"), White), 1);       // f3 only
    CHECK_EQ(kingShelter(fen("7k/8/8/8/4K3/8/8/8 w - - 0 1"), White), 0);            // a king in the open
    // The edge of the board: a king on h1 has two files to count.
    CHECK_EQ(kingShelter(fen("6k1/8/8/8/8/8/6PP/7K w - - 0 1"), White), 2);
    // The other side's pawns are no shelter.
    CHECK_EQ(kingShelter(fen("6k1/8/8/8/8/8/5ppp/6K1 w - - 0 1"), White), 0);
}

TEST(coach_core_review_knight_path_long_leg_sideways) {
    // g1-e2: the long leg runs along the rank first (f1 e1), then the step up to e2.
    const std::vector<Square> p = movePath(Knight, sq("g1"), sq("e2"));
    REQUIRE(p.size() == 3u);
    CHECK_EQ(p[0], sq("f1"));
    CHECK_EQ(p[1], sq("e1"));
    CHECK_EQ(p[2], sq("e2"));
    // Black's b8-d7 the same way, mirrored.
    const std::vector<Square> b = movePath(Knight, sq("b8"), sq("d7"));
    REQUIRE(b.size() == 3u);
    CHECK_EQ(b[0], sq("c8"));
    CHECK_EQ(b[1], sq("d8"));
    CHECK_EQ(b[2], sq("d7"));
    // The trace turns on that corner.
    const std::vector<Square> t = tracePath(Knight, sq("g1"), sq("e2"));
    REQUIRE(t.size() == 3u);
    CHECK_EQ(t[1], sq("e1"));
}

TEST(coach_core_review_double_check_announced) {
    // Nd6+ checks with the knight and uncovers the rook on e1: a double check, said as such from
    // level 2 (by the player, or by the coach); level 1 says a plain "check".
    const char* fen = "4k3/8/8/8/4N3/8/8/K3R3 w - - 0 1";
    for (int level = 1; level <= 3; ++level) {
        for (Color human : {White, Black}) {
            Game g = gameOf(fen, {"Nd6+"});
            REQUIRE(squareCount(g.position().checkers()) == 2);
            Reviewer rv;
            rv.reset(level, human);
            const Script s = rv.announce(g);
            checkScript(s, "double check");
            REQUIRE(!s.empty());
            const std::string twice = human == White ? "ann.double_check.human" : "ann.double_check.coach";
            if (level >= 2) {
                CHECK_EQ(s.size(), size_t(1));
                CHECK_EQ(s[0].line.key, twice);
                CHECK(s[0].priority == Priority::Urgent);
                CHECK(s[0].look == Look::Board);
            } else {
                CHECK(!hasKey(s, twice));
                CHECK(hasKey(s, human == White ? "ann.check.human.b1" : "ann.check.coach"));
            }
        }
    }
}

TEST(coach_core_review_mate_threat_warning) {
    // 1.e4 e5 2.Qh5 Nc6 3.Bc4: the coach (White) now threatens Qxf7#. Levels 1-2 warn the player,
    // level 1 pointing at f7; level 3 says nothing.
    for (int level = 1; level <= 3; ++level) {
        Game g = gameOf(nullptr, {"e4", "e5", "Qh5", "Nc6", "Bc4"});
        Reviewer rv;
        rv.reset(level, Black);
        const Script s = rv.coachMoved(g);
        checkScript(s, "mate threat");
        if (level == 3) {
            CHECK(s.empty());
            continue;
        }
        const Beat* b = beatWithKey(s, "threat.mate.b" + std::to_string(level));
        REQUIRE(b != nullptr);
        CHECK(b->priority == Priority::Low);
        REQUIRE(b->line.arg("sq") != nullptr);
        CHECK_EQ(b->line.arg("sq")->square, sq("f7"));
        REQUIRE(b->line.arg("my") != nullptr);
        CHECK_EQ(b->line.arg("my")->piece, Queen);   // the queen mates on f7
        CHECK_EQ(pointsAt(*b, GestureKind::PointSquare, sq("f7")), level == 1);
    }
    // A threat search that finds no mate (the coach would rather take a pawn) silences the warning.
    Game g = gameOf(nullptr, {"e4", "e5", "Qh5", "Nc6", "Bc4"});
    Reviewer rv;
    rv.reset(1, Black);
    REQUIRE(!rv.threatKey(g).empty());
    const ai::Analysis threat = analysisOf({pvl(80, "h5e5 c6e5")});
    CHECK(!hasPrefix(rv.coachMoved(g, nullptr, &threat), "threat.mate"));
}

TEST(coach_core_review_threat_by_an_uncovered_piece) {
    // ...Nb4 opens the long diagonal: the bishop on b7, not the knight that moved, now attacks the
    // loose rook on f3. The warning names and traces the bishop.
    Game g = gameOf("4k3/1b6/8/3n4/8/5R2/8/4K3 b - - 0 1", {"Nb4"});
    Reviewer rv;
    rv.reset(1, White);
    const Script s = rv.coachMoved(g);
    checkScript(s, "uncovered attacker");
    const Beat* b = beatWithKey(s, "threat.piece.b1");
    REQUIRE(b != nullptr);
    REQUIRE(b->line.arg("my") != nullptr);
    CHECK_EQ(b->line.arg("my")->piece, Bishop);
    CHECK_EQ(b->line.arg("my")->square, sq("b7"));
    REQUIRE(b->line.arg("your") != nullptr);
    CHECK_EQ(b->line.arg("your")->piece, Rook);
    bool traced = false;
    for (const Gesture& gs : b->gestures)
        if (gs.kind == GestureKind::Trace && !gs.path.empty() && gs.path.front() == sq("b7") && gs.path.back() == sq("f3"))
            traced = true;
    CHECK(traced);
    CHECK(pointsAt(*b, GestureKind::PointPiece, sq("f3")));
}

TEST(coach_core_review_free_piece_missed_hint) {
    // The coach's knight on d5 is free and the player plays h3: offered back at level 1, the hint
    // says that something could be taken, without naming it.
    const char* fen = "6k1/5ppp/8/3n4/8/2N5/1P3PPP/6K1 w - - 0 1";
    Game g = gameOf(fen, {"h3"});
    Reviewer rv;
    rv.reset(1, White);
    const Review r = reviewOf(rv, g, analysisOf({pvl(350, "c3d5 g8f8"), pvl(30, "h2h3 d5e7")}));
    REQUIRE(r.offersTakeback);
    CHECK_EQ(r.verdict.exType, ExType::MissedCapture);
    g.undo(1);
    const Script hint = rv.takebackAccepted(g);
    checkScript(hint, "missed hint");
    CHECK_EQ(hint.size(), size_t(1));
    CHECK(hasKey(hint, "tb.hint.missed.b1"));
    // The replay that takes the knight is the fix.
    g.play(g.position().parseSAN("Nxd5"));
    const Review again = reviewOf(rv, g, analysisOf({pvl(350, "c3d5 g8f8"), pvl(30, "h2h3 d5e7")}));
    CHECK(again.isRetry);
    CHECK(again.takeback.fixed);
    CHECK(!again.takeback.same);
}

TEST(coach_core_review_unknown_score_kind_rejected) {
    // "score" must be followed by cp or mate: anything else (a later Stockfish's new kind, a damaged
    // line) is no score, and the line is rejected whole, leaving the output alone.
    ai::PvLine l;
    l.depth = 7;
    CHECK(!ai::parseInfoLine("info depth 9 multipv 1 score wdl 500 500 0 pv e2e4", l));
    CHECK(!ai::parseInfoLine("info depth 9 score centi 25 pv e2e4", l));
    CHECK_EQ(l.depth, 7);
    CHECK(ai::parseInfoLine("info depth 9 score cp 25 pv e2e4", l));
    CHECK_EQ(l.score.cp, 25);
}
