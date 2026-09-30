// Tests for the coach's board facts (src/coach/tactics.h): material and SEE, hanging and trapped
// pieces, forks, pins, skewers, discovered attacks, mates and the back rank, pawns, phases, move
// facts and gesture paths. Each detector on hand-built positions, positive and negative.
#include "test.h"

#include "chess/chess.h"
#include "coach/tactics.h"

#include <string>
#include <vector>

using namespace chess;
using namespace coach;

namespace {

Square sq(const char* s) { return parseSquare(s); }
uint64_t bits(std::initializer_list<const char*> names) {
    uint64_t b = 0;
    for (const char* n : names) b |= squareBit(parseSquare(n));
    return b;
}

Position fen(const char* f) {
    Position p;
    const bool ok = p.setFEN(f);
    if (!ok) std::fprintf(stderr, "  invalid FEN in test: %s\n", f);
    CHECK(ok);
    return p;
}

Position afterSan(std::initializer_list<const char*> sans) {
    Position p;
    for (const char* s : sans) {
        const Move m = p.parseSAN(s);
        if (!m.valid()) std::fprintf(stderr, "  bad SAN in test: %s\n", s);
        CHECK(m.valid());
        if (!m.valid()) break;
        p.makeMove(m);
    }
    return p;
}

Move san(const Position& p, const char* s) {
    const Move m = p.parseSAN(s);
    if (!m.valid()) std::fprintf(stderr, "  bad SAN in test: %s\n", s);
    CHECK(m.valid());
    return m;
}

}  // namespace

TEST(coach_tactics_material_and_see) {
    Position start;
    CHECK_EQ(material(start, White), 39);
    CHECK_EQ(materialBalance(start, Black), 0);
    CHECK_EQ(majorsAndMinors(start), 14);
    // The two chessprogramming.org SEE examples.
    Position a = fen("1k1r4/1pp4p/p7/4p3/8/P5P1/1PP4P/2K1R3 w - - 0 1");
    CHECK_EQ(see(a, san(a, "Rxe5")), 100);
    Position b = fen("1k1r3q/1ppn3p/p4b2/4p3/8/P2N2P1/1PP1R1BP/2K1Q3 w - - 0 1");
    CHECK_EQ(see(b, san(b, "Nxe5")), -220);
    // A quiet move into a pawn's attack loses the piece (1.e4 e6 2.Qf3 Nc6 3.Qf6?? gxf6).
    Position q = afterSan({"e4", "e6", "Qf3", "Nc6"});
    CHECK(see(q, san(q, "Qf6")) < -700);   // gxf6
    CHECK_EQ(see(q, san(q, "Qf4")), 0);    // safe
    // A capture that promotes.
    Position pr = fen("1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    CHECK_EQ(see(pr, pr.parseUCI("a7b8q")), 500 + 900 - 100);
    // seeSquare: after 1.e4 e5 2.Nf3 d5 the e5 pawn falls, d5 does not (exd5 Qxd5 is even).
    Position h = afterSan({"e4", "e5", "Nf3", "d5"});
    CHECK_EQ(seeSquare(h, sq("e5"), White), 100);
    CHECK(seeSquare(h, sq("d5"), White) <= 0);
    CHECK_EQ(seeSquare(h, sq("e4"), White), 0);   // own piece
    CHECK_EQ(seeSquare(h, sq("a7"), White), 0);   // no attacker
}

TEST(coach_tactics_hanging) {
    Position h = afterSan({"e4", "e5", "Nf3", "d5"});
    CHECK_EQ(hangingPieces(h, Black, false), bits({"e5"}));
    CHECK_EQ(hangingPieces(h, Black, true), bits({"e5"}));
    CHECK(isUndefended(h, sq("e5")));
    CHECK(!isUndefended(h, sq("d5")));
    Position safe = afterSan({"e4", "e5", "Nf3", "Nc6"});
    CHECK_EQ(hangingPieces(safe, Black, false), uint64_t(0));
    CHECK_EQ(hangingPieces(safe, Black, true), uint64_t(0));
    // A defended knight attacked by a pawn: lost by exchange, but not "undefended".
    Position k = fen("4k3/8/8/3n4/2P1P3/8/8/4K3 w - - 0 1");
    CHECK_EQ(hangingPieces(k, Black, false), bits({"d5"}));
    Position kd = fen("4k3/8/4p3/3n4/2P5/8/8/4K3 w - - 0 1");
    CHECK_EQ(hangingPieces(kd, Black, false), bits({"d5"}));  // c4xd5 exd5 still wins 2 points
    CHECK_EQ(hangingPieces(kd, Black, true), uint64_t(0));    // but it has a protector
}

TEST(coach_tactics_fork) {
    // The worked example: 1.e4 e5 2.Nf3 Nc6 3.Bc4 Nd4 4.Nxe5 Qg5 forks e5 and g2.
    Position w = fen("r1b1kbnr/pppp1ppp/8/4N1q1/2BnP3/8/PPPP1PPP/RNBQK2R w KQkq - 1 5");
    CHECK_EQ(forkTargets(w, sq("g5")), bits({"e5", "g2"}));
    // A knight's royal fork: Nc7+ attacks a8 and e8.
    Position n = fen("r3k3/2N5/8/8/8/8/8/4K3 b - - 1 1");
    CHECK_EQ(forkTargets(n, sq("c7")), bits({"a8", "e8"}));
    // Negative: the knight on c7 can be taken for free.
    Position nb = fen("r3k3/2N5/1b6/8/8/8/8/4K3 b - - 1 1");
    CHECK_EQ(forkTargets(nb, sq("c7")), uint64_t(0));
    // Negative: the knight on e5 defended by a pawn, g2 by a rook: nothing to win.
    Position wd = fen("4k3/8/8/4N1q1/3P4/8/6P1/4K1R1 w - - 0 1");
    CHECK_EQ(forkTargets(wd, sq("g5")), uint64_t(0));
    // Negative: the queen itself stands en prise to a pawn.
    Position wp = fen("r1b1kbnr/pppp1ppp/8/4N1q1/2BnPP2/8/PPPP2PP/RNBQK2R w KQkq - 1 5");
    CHECK_EQ(forkTargets(wp, sq("g5")), uint64_t(0));
}

TEST(coach_tactics_pins) {
    // Absolute: Bb5 pins the knight c6 to the king.
    Position a = fen("r1bqkbnr/ppp2ppp/2np4/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 4");
    std::vector<Pin> pa = pins(a, Black);
    CHECK_EQ(pa.size(), size_t(1));
    if (!pa.empty()) {
        CHECK_EQ(pa[0].pinner, sq("b5"));
        CHECK_EQ(pa[0].pinned, sq("c6"));
        CHECK_EQ(pa[0].behind, sq("e8"));
        CHECK(pa[0].absolute);
    }
    CHECK(pins(a, White).empty());
    // Relative: 1.e4 e5 2.Nf3 d6 3.d4 Bg4 pins the knight f3 to the queen d1.
    Position r = afterSan({"e4", "e5", "Nf3", "d6", "d4", "Bg4"});
    std::vector<Pin> pr = pins(r, White);
    CHECK_EQ(pr.size(), size_t(1));
    if (!pr.empty()) {
        CHECK_EQ(pr[0].pinned, sq("f3"));
        CHECK_EQ(pr[0].behind, sq("d1"));
        CHECK(!pr[0].absolute);
    }
    // Negative: a rook "pinned" by a rook along the file can take it.
    Position rr = fen("4r1k1/8/8/8/8/8/4R3/4K3 w - - 0 1");
    CHECK(pins(rr, White).empty());
    // Negative: a queen in front of a knight is not pinned to it.
    Position qn = fen("4k3/8/8/8/1b6/8/3Q4/4N1K1 w - - 0 1");
    CHECK(pins(qn, White).empty());
}

TEST(coach_tactics_skewer) {
    // The bishop on a1 checks the king on d4; the rook on g7 stands behind it.
    Position s = fen("8/6r1/8/8/3k4/8/8/B3K3 b - - 0 1");
    std::vector<Skewer> sk = skewers(s, Black);
    CHECK_EQ(sk.size(), size_t(1));
    if (!sk.empty()) {
        CHECK_EQ(sk[0].attacker, sq("a1"));
        CHECK_EQ(sk[0].front, sq("d4"));
        CHECK_EQ(sk[0].behind, sq("g7"));
    }
    // Negative: a knight in front of the rook is a pin, not a skewer.
    Position p = fen("4k3/6r1/8/8/3n4/8/8/B3K3 w - - 0 1");
    CHECK(skewers(p, Black).empty());
    CHECK_EQ(pins(p, Black).size(), size_t(1));
    // Negative: only a pawn behind the king.
    Position pw = fen("8/6p1/8/8/3k4/8/8/B3K3 b - - 0 1");
    CHECK(skewers(pw, Black).empty());
}

TEST(coach_tactics_discovered) {
    // The knight leaves e4 with check: the rook e1 now hits the queen on e8.
    Position d = fen("4q1k1/8/8/8/4N3/8/8/4R1K1 w - - 0 1");
    std::vector<Discovery> dv = discoveredAttacks(d, san(d, "Nf6+"));
    CHECK_EQ(dv.size(), size_t(1));
    if (!dv.empty()) {
        CHECK_EQ(dv[0].slider, sq("e1"));
        CHECK_EQ(dv[0].target, sq("e8"));
        CHECK(!dv[0].check);
    }
    // Discovered check.
    Position c = fen("4k3/8/8/8/4N3/8/8/4R1K1 w - - 0 1");
    std::vector<Discovery> dc = discoveredAttacks(c, san(c, "Nc5+"));
    CHECK_EQ(dc.size(), size_t(1));
    if (!dc.empty()) CHECK(dc[0].check);
    const MoveFacts f = analyzeMove(c, san(c, "Nc5+"));
    CHECK(f.check && f.discoveredCheck && !f.doubleCheck);
    // Negative: the rook moving itself uncovers nothing; a knight off the line neither.
    CHECK(discoveredAttacks(d, san(d, "Rf1")).empty());
    Position n = fen("4q1k1/8/8/8/3N4/8/8/4R1K1 w - - 0 1");
    CHECK(discoveredAttacks(n, san(n, "Nf5")).empty());
    // Double check.
    Position dd = fen("4k3/8/8/1B6/8/8/8/4RK2 b - - 0 1");
    CHECK(isDoubleCheck(dd));
    CHECK(!isDoubleCheck(c));
}

TEST(coach_tactics_mates_and_back_rank) {
    Position m = fen("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1");
    Move mv;
    CHECK(mateInOne(m, &mv));
    CHECK_EQ(m.toUCI(mv), std::string("d1d8"));
    CHECK(backRankWeak(m, Black));
    CHECK(!backRankWeak(m, White));   // Black has no rook or queen
    CHECK_EQ(escapeSquares(m, Black), bits({"f8", "h8"}));
    Position luft = fen("6k1/5pp1/7p/8/8/8/5PPP/3R2K1 w - - 0 1");
    CHECK(!mateInOne(luft));
    CHECK(!backRankWeak(luft, Black));
    // The threat seen before White's turn.
    Position t = fen("6k1/5ppp/8/8/8/8/5PPP/3R2K1 b - - 0 1");
    CHECK(mateThreat(t, White, &mv));
    CHECK(!mateThreat(t, Black));
    CHECK_EQ(checkingMoves(m).size(), size_t(1));
    // Escape squares ignore squares behind the king on the checking line.
    Position e = fen("4k3/8/8/8/8/8/8/4RK2 b - - 0 1");
    CHECK_EQ(escapeSquares(e, Black), bits({"d8", "f8", "d7", "f7"}));
    // Mate patterns.
    CHECK(classifyMate(fen("3R2k1/5ppp/8/8/8/8/5PPP/6K1 b - - 1 1")) == MatePattern::BackRank);
    CHECK(classifyMate(fen("6rk/5Npp/8/8/8/8/8/6K1 b - - 0 1")) == MatePattern::Smothered);
    CHECK(classifyMate(fen("6k1/6Q1/5P2/8/8/8/8/6K1 b - - 0 1")) == MatePattern::Support);
    CHECK(classifyMate(fen("1R4k1/R7/8/8/8/8/8/6K1 b - - 0 1")) == MatePattern::Ladder);
    CHECK(classifyMate(m) == MatePattern::None);   // not mate
}

TEST(coach_tactics_trapped_and_pawns) {
    // Bxa7?? b6 style: the bishop on a7, the rook takes it and every exit loses it.
    Position t = fen("r3k3/B1p5/1p6/8/8/8/8/4K3 w - - 0 1");
    CHECK(isTrapped(t, sq("a7")));
    Position f = fen("r3k3/B7/1p6/8/8/8/8/4K3 w - - 0 1");   // Bxb6 gets out
    CHECK(!isTrapped(f, sq("a7")));
    // Works for the side not to move too.
    Position tb = fen("r3k3/B1p5/1p6/8/8/8/8/4K3 b - - 0 1");
    CHECK(isTrapped(tb, sq("a7")));
    // Passed pawns and the rule of the square.
    Position p = fen("8/8/3k4/P7/8/8/8/4K3 w - - 0 1");
    CHECK(isPassed(p, sq("a5")));
    CHECK(!outsideSquare(p, sq("a5"), false));   // Kd6 is inside: a6 Kc7 a7 Kb7
    Position far = fen("8/8/4k3/P7/8/8/8/4K3 w - - 0 1");
    CHECK(outsideSquare(far, sq("a5"), false));  // White to move: a6 and it queens
    CHECK(!outsideSquare(far, sq("a5"), true));  // Black to move: Kd7 reaches the square
    CHECK_EQ(promotionSquare(p, sq("a5")), sq("a8"));
    Position blocked = fen("8/8/1p1k4/P7/8/8/8/4K3 w - - 0 1");
    CHECK(!isPassed(blocked, sq("a5")));
    // Development.
    Position start;
    CHECK_EQ(undevelopedMinors(start, White), 4);
    CHECK_EQ(undevelopedMinors(afterSan({"Nf3", "e5", "e3"}), White), 3);
    CHECK(!isForced(start));
    CHECK(isForced(fen("7k/8/8/8/8/8/6R1/K5R1 b - - 0 1")));  // only Kh7
    // Recapture.
    Game g;
    for (const char* s : {"e4", "d5", "exd5", "Qxd5"}) CHECK(g.play(g.position().parseSAN(s)));
    CHECK(isRecapture(g, 3));
    CHECK(!isRecapture(g, 2));
    CHECK(!isRecapture(g, 0));
    // King zone.
    CHECK_EQ(kingZoneAttacks(start, White), 0);
    CHECK(kingZoneAttacks(fen("3R2k1/5ppp/8/8/8/8/5PPP/6K1 b - - 1 1"), Black) >= 1);   // f8
}

TEST(coach_tactics_phases) {
    Game g;
    CHECK_EQ(dividePhases(g).middlegame, -1);
    CHECK_EQ(phaseOfPly(dividePhases(g), 0), 0);
    Game e;
    CHECK(e.resetFromFEN("4k3/8/8/8/8/8/4P3/R3K3 w - - 0 1"));
    const Phases ph = dividePhases(e);
    CHECK_EQ(ph.endgame, 0);
    CHECK_EQ(ph.middlegame, -1);
    CHECK_EQ(phaseOfPly(ph, 0), 2);
    // A middlegame once White's back rank thins out (fewer than 4 pieces after 9.O-O-O).
    Game m;
    for (const char* s : {"e4", "d5", "exd5", "Qxd5", "Nc3", "Qa5", "d4", "Nf6", "Nf3", "Bf5", "Bc4", "e6", "Bd2",
                          "c6", "Qe2", "Bb4", "O-O-O", "Nbd7"})
        CHECK(m.play(m.position().parseSAN(s)));
    const Phases pm = dividePhases(m);
    CHECK(pm.middlegame > 0);
    CHECK_EQ(pm.endgame, -1);
    CHECK_EQ(phaseOfPly(pm, 0), 0);
    CHECK_EQ(phaseOfPly(pm, int(m.moves().size())), 1);
}

TEST(coach_tactics_move_facts_and_lines) {
    // 4...Qg5 in the worked example.
    Position p1 = fen("r1bqkbnr/pppp1ppp/8/4N3/2BnP3/8/PPPP1PPP/RNBQK2R b KQkq - 0 4");
    const Move qg5 = san(p1, "Qg5");
    const MoveFacts f = analyzeMove(p1, qg5);
    CHECK_EQ(f.piece, Queen);
    CHECK_EQ(f.forks, bits({"e5", "g2"}));
    CHECK(f.newlyAttacked & bits({"e5"}));
    CHECK(f.newlyAttacked & bits({"g2"}));
    CHECK(!f.check);
    CHECK_EQ(f.captured, NoPiece);
    // Removal of the guard: the knight on f6 guarded h7; after Ng4 it no longer does.
    Position rg = fen("6k1/7p/5n2/8/8/8/8/6K1 b - - 0 1");
    const MoveFacts g = analyzeMove(rg, san(rg, "Ng4"));
    CHECK_EQ(g.undefended, bits({"h7"}));
    // A capture en prise.
    Position c = afterSan({"e4", "d5"});
    const MoveFacts x = analyzeMove(c, san(c, "exd5"));
    CHECK_EQ(x.captured, Pawn);
    CHECK_EQ(x.capturedOn, sq("d5"));
    CHECK_EQ(x.seeCp, 0);   // Qxd5 takes back
    // Replaying a line: 4...Qg5 5.Nxf7 Qxg2 6.Rf1 Qxe4+.
    const std::vector<LineStep> line = replayLine(p1, {"d8g5", "e5f7", "g5g2", "h1f1", "g2e4", "zzzz"}, White);
    CHECK_EQ(line.size(), size_t(5));
    CHECK_EQ(sanLine(line, 0, 3), std::string("Qg5 Nxf7 Qxg2"));
    CHECK_EQ(line[1].captured, Pawn);
    CHECK_EQ(line[2].captured, Pawn);
    CHECK_EQ(line[4].check, true);
    CHECK_EQ(line[0].balance, materialBalance(p1, White));
    CHECK_EQ(line[4].balance, materialBalance(p1, White) + 1 - 2);
}

TEST(coach_tactics_paths) {
    const std::vector<Square> n = movePath(Knight, sq("g1"), sq("f3"));
    CHECK_EQ(n.size(), size_t(3));
    if (n.size() == 3) {
        CHECK_EQ(n[0], sq("g2"));
        CHECK_EQ(n[1], sq("g3"));
        CHECK_EQ(n[2], sq("f3"));
    }
    const std::vector<Square> t = tracePath(Knight, sq("g1"), sq("f3"));
    CHECK_EQ(t.size(), size_t(3));
    if (t.size() == 3) CHECK_EQ(t[1], sq("g3"));
    CHECK_EQ(knightCorner(sq("b8"), sq("d7")), sq("d8"));
    CHECK_EQ(knightCorner(sq("b8"), sq("d6")), NoSquare);
    const std::vector<Square> b = movePath(Bishop, sq("c1"), sq("f4"));
    CHECK_EQ(b.size(), size_t(3));
    if (b.size() == 3) {
        CHECK_EQ(b[0], sq("d2"));
        CHECK_EQ(b[2], sq("f4"));
    }
    CHECK_EQ(movePath(Pawn, sq("e2"), sq("e4")).size(), size_t(2));
    CHECK_EQ(movePath(Pawn, sq("e2"), sq("e3")).size(), size_t(1));
    CHECK_EQ(movePath(King, sq("e1"), sq("g1")).size(), size_t(2));
    CHECK_EQ(movePath(Rook, sq("a8"), sq("a1")).size(), size_t(7));
    CHECK_EQ(tracePath(Rook, sq("a8"), sq("a1")).size(), size_t(2));
}
