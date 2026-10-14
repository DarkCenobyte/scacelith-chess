// Tests for what the coach's review says about a move that is no fault to explain: the tips (the
// mating technique, the fifty-move rule, a pawn grabbed by the queen, the same piece moved twice, a
// knight on the rim, the centre left to the opponent), each once per game and at the levels that
// teach it, and the praise of a top move (a sacrifice, a great move after a bad one, a trade when
// ahead, castling, a piece saved, and the excellent move a shallow search would have missed, unless
// that search's own move was as good). The engine's lines are made up for each position.
#include "coach_core_review_support.h"

#include <string>
#include <vector>

using namespace chess;
using namespace coach;
using namespace corereview;

namespace {

Square sq(const char* s) { return parseSquare(s); }

// The first beat whose key starts with 'prefix' (a family said at some level), null when none.
const Beat* beatWithPrefix(const Script& s, const std::string& prefix) {
    for (const Beat& b : s)
        if (b.line.key.rfind(prefix, 0) == 0) return &b;
    return nullptr;
}

bool pointsAtPiece(const Beat& b, Square s) { return pointsAt(b, GestureKind::PointPiece, s); }

// A tip at 'level': its key, its type in the verdict, said at Low priority, nothing else said.
void checkTip(const Review& r, int level, const std::string& family, ExType type, const char* where) {
    checkScript(r.script, where);
    const std::string key = family == "tip.mate_technique" || family == "tip.fifty" ? family + ".b1"
                                                                                     : family + ".b" + std::to_string(level);
    const Beat* tip = beatWithKey(r.script, key);
    CHECK(tip != nullptr);
    if (!tip) {
        dumpKeys(r.script, where);
        return;
    }
    CHECK(tip->priority == Priority::Low);
    CHECK(tip->look == Look::Player);
    CHECK(r.verdict.voiced);
    CHECK_EQ(r.verdict.exType, type);
    CHECK(!r.offersTakeback);
    CHECK(!hasPrefix(r.script, "ex.verdict"));
}

}  // namespace

// ---- Tips ---------------------------------------------------------------------------------------

TEST(coach_core_review_tip_mating_technique) {
    // King and rook against a bare king, twenty moves without progress: the engine sees a mate after
    // 1.Ra4 only. 1.Ra2 keeps a large lead, so no fault is said, but beginners hear how to mate.
    const ai::Analysis a0 = analysisOf({pvl(0, "a1a4 e5d5 e1d2", 9), pvl(500, "a1a2 e5d5 e1d2")});
    for (int level = 1; level <= 3; ++level) {
        for (int clock : {20, 10}) {
            const std::string fen = "8/8/8/4k3/8/8/8/R3K3 w - - " + std::to_string(clock) + " 60";
            Game g = gameOf(fen.c_str(), {"Ra2"});
            Reviewer rv;
            rv.reset(level, White);
            const Review r = reviewOf(rv, g, a0);
            const std::string where = "mate technique b" + std::to_string(level) + " clock " + std::to_string(clock);
            checkScript(r.script, where.c_str());
            CHECK(r.verdict.mateMissed);
            // Levels 1-2, once the moves without progress add up (eight of each side's).
            const bool said = level <= 2 && clock >= 16;
            CHECK_EQ(hasKey(r.script, "tip.mate_technique.b1"), said);
            if (said) checkTip(r, level, "tip.mate_technique", ExType::Endgame, where.c_str());
            // Once per game: the same move again says nothing more.
            if (said) CHECK(!hasKey(reviewOf(rv, g, a0).script, "tip.mate_technique.b1"));
        }
    }
}

TEST(coach_core_review_tip_fifty_moves) {
    // A won rook ending where the next move brings the count to 80 plies without a capture or a pawn
    // move: the fifty-move rule is near. A slightly slower rook move (a good move, no fault).
    const ai::Analysis a0 = analysisOf({pvl(400, "a1a7 h7g6 b5b6"), pvl(330, "a1c1 h7g6 b5b6")});
    for (int level = 1; level <= 4; ++level) {
        for (int clock : {79, 70}) {
            const std::string fen = "8/7k/8/1P6/8/8/6K1/R6r w - - " + std::to_string(clock) + " 80";
            Game g = gameOf(fen.c_str(), {"Rc1"});
            Reviewer rv;
            rv.reset(level, White);
            const Review r = reviewOf(rv, g, a0);
            const std::string where = "fifty b" + std::to_string(level) + " clock " + std::to_string(clock);
            CHECK_EQ(r.verdict.cls, MoveClass::Good);
            const bool said = level <= 3 && clock == 79;
            CHECK_EQ(hasKey(r.script, "tip.fifty.b1"), said);
            if (said) checkTip(r, level, "tip.fifty", ExType::Endgame, where.c_str());
            else checkScript(r.script, where.c_str());
        }
    }
}

TEST(coach_core_review_tip_queen_grabs_a_pawn) {
    // 4...Qxb2: the queen takes a pawn on the b-file early on, and the knights chase it.
    const ai::Analysis a0 = analysisOf({pvl(30, "c5d4 e3d4 b8c6"), pvl(-30, "b6b2 c3b5 b8a6")});
    for (int level = 1; level <= 4; ++level) {
        Game g = gameOf(nullptr, {"d4", "Nf6", "Bf4", "c5", "e3", "Qb6", "Nc3", "Qxb2"});
        Reviewer rv;
        rv.reset(level, Black);
        const Review r = reviewOf(rv, g, a0);
        const std::string where = "queen grab b" + std::to_string(level);
        CHECK_EQ(r.verdict.cls, MoveClass::Inaccuracy);
        // Levels 1-3; level 4 only for a costlier grab (10 points).
        CHECK_EQ(hasPrefix(r.script, "tip.queen_grab"), level <= 3);
        if (level <= 3) {
            checkTip(r, level, "tip.queen_grab", ExType::Opening, where.c_str());
            const Beat* tip = beatWithPrefix(r.script, "tip.queen_grab");
            if (tip) {
                const Arg* your = tip->line.arg("your");
                CHECK(your && your->piece == Queen && your->own && your->square == sq("b2"));
                CHECK(pointsAtPiece(*tip, sq("b2")));
            }
        } else {
            checkScript(r.script, where.c_str());
        }
    }
}

TEST(coach_core_review_tip_same_piece_twice) {
    // 2.Ng5: the knight moves again while the other minor pieces wait at home.
    const ai::Analysis a0 = analysisOf({pvl(30, "d2d4 g8f6"), pvl(-5, "f3g5 e7e5")});
    for (int level = 1; level <= 4; ++level) {
        Game g = gameOf(nullptr, {"Nf3", "d5", "Ng5"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        const std::string where = "same piece b" + std::to_string(level);
        CHECK_EQ(r.verdict.cls, MoveClass::Good);
        CHECK_EQ(hasPrefix(r.script, "tip.same_piece"), level <= 3);
        if (level <= 3) {
            checkTip(r, level, "tip.same_piece", ExType::Opening, where.c_str());
            const Beat* tip = beatWithPrefix(r.script, "tip.same_piece");
            if (tip) {
                const Arg* your = tip->line.arg("your");
                CHECK(your && your->piece == Knight && your->square == sq("g5"));
            }
        } else {
            checkScript(r.script, where.c_str());
        }
    }
    // A knight that moves again because it is attacked is no such slip (2.Ng5 h6 3.Nf3).
    const ai::Analysis chased = analysisOf({pvl(30, "g5f3 g8f6"), pvl(-5, "g5h3 g8f6")});
    Game g = gameOf(nullptr, {"Nf3", "d5", "Ng5", "h6", "Nh3"});
    Reviewer rv;
    rv.reset(1, White);
    CHECK(!hasPrefix(reviewOf(rv, g, chased).script, "tip.same_piece"));
}

TEST(coach_core_review_tip_knight_on_the_rim) {
    // 1.Na3: a knight on the rim.
    const ai::Analysis a0 = analysisOf({pvl(30, "e2e4 e7e5"), pvl(-5, "b1a3 e7e5")});
    for (int level = 1; level <= 3; ++level) {
        Game g = gameOf(nullptr, {"Na3"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        const std::string where = "knight rim b" + std::to_string(level);
        checkTip(r, level, "tip.knight_rim", ExType::Opening, where.c_str());
        const Beat* tip = beatWithPrefix(r.script, "tip.knight_rim");
        if (tip) CHECK(pointsAtPiece(*tip, sq("a3")));
        // Said once per game: a second knight to the rim says nothing.
        g.play(g.position().parseSAN("e5"));
        g.play(g.position().parseSAN("Nh3"));
        const ai::Analysis again = analysisOf({pvl(30, "e2e4 g8f6"), pvl(-5, "g1h3 g8f6")});
        CHECK(!hasPrefix(reviewOf(rv, g, again).script, "tip.knight_rim"));
    }
}

TEST(coach_core_review_tip_take_the_centre) {
    // Move five, and no pawn or piece of White's on or watching the four centre squares: the
    // centre tip names e4, the empty square to take.
    const ai::Analysis a0 = analysisOf({pvl(0, "c1b2 f8c5"), pvl(-35, "a1b1 f8c5")});
    for (int level = 1; level <= 3; ++level) {
        Game g = gameOf(nullptr, {"Na3", "d5", "Nh3", "e5", "b3", "Nc6", "g3", "Nf6", "Rb1"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        const std::string where = "centre b" + std::to_string(level);
        checkTip(r, level, "tip.centre", ExType::Opening, where.c_str());
        const Beat* tip = beatWithPrefix(r.script, "tip.centre");
        if (tip) {
            const Arg* s = tip->line.arg("sq");
            CHECK(s && s->square == sq("e4"));
            CHECK(pointsAt(*tip, GestureKind::PointSquare, sq("e4")));
        }
    }
    // With a pawn on d4 the centre is taken: no tip.
    Game g = gameOf(nullptr, {"d4", "d5", "Na3", "e5", "Nh3", "Nc6", "g3", "Nf6", "Rb1"});
    Reviewer rv;
    rv.reset(1, White);
    CHECK(!hasPrefix(reviewOf(rv, g, analysisOf({pvl(0, "c1f4 f8c5"), pvl(-35, "a1b1 f8c5")})).script, "tip.centre"));
}

// ---- Praise -------------------------------------------------------------------------------------

TEST(coach_core_review_praise_sacrifice) {
    // 7.Bxh7+!: the Greek gift, the engine's own move. A bishop given for a pawn, still down two
    // points after Kxh7 Ng5+: a sacrifice (levels 1-2), a brilliant move (levels 3-6), once a game.
    const char* fen = "r1bq1rk1/pppn1ppp/4p3/3pP3/1b1P4/2NB1N2/PPP2PPP/R1BQK2R w KQ - 0 7";
    const ai::Analysis a0 = analysisOf({pvl(250, "d3h7 g8h7 f3g5 h7g8 d1h5 f8e8 h5h7 g8f8"), pvl(60, "e1g1 c7c5"),
                                        pvl(40, "c1d2 c7c5")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(fen, {"Bxh7+"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        const std::string where = "sacrifice b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.cls, MoveClass::Best);
        CHECK(r.verdict.brilliant);
        CHECK(r.verdict.praised);
        const std::string key = level <= 2 ? "praise.sacrifice" : "praise.brilliant.b" + std::to_string(level);
        const Beat* b = beatWithKey(r.script, key);
        CHECK(b != nullptr);
        if (!b) {
            dumpKeys(r.script, where.c_str());
            continue;
        }
        // The pawn the bishop took is named ("my"), and the coach nods.
        const Arg* my = b->line.arg("my");
        CHECK(my && my->piece == Pawn && !my->own);
        bool nod = false;
        for (const Gesture& gs : b->gestures) nod = nod || gs.kind == GestureKind::Nod;
        CHECK(nod);
    }
}

TEST(coach_core_review_praise_great_move_after_a_bad_one) {
    // 1.a3?! leaves White below 45 %; after 1...e5, 2.Nf3 is the only good move by far: a great
    // move from level 2 (a beginner hears "best move").
    const ai::Analysis first = analysisOf({pvl(30, "e2e4 e7e5"), pvl(-80, "a2a3 e7e5")});
    const ai::Analysis root = analysisOf({pvl(300, "g1f3 b8c6"), pvl(-100, "b1c3 g8f6"), pvl(-120, "f1c4 g8f6")});
    for (int level = 1; level <= 4; ++level) {
        Game g = gameOf(nullptr, {"a3"});
        Reviewer rv;
        rv.reset(level, White);
        CHECK(reviewOf(rv, g, first).verdict.wPlayed <= 45.0);
        g.play(g.position().parseSAN("e5"));
        g.play(g.position().parseSAN("Nf3"));
        const Review r = reviewOf(rv, g, root);
        const std::string where = "great b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.great, level >= 2);
        if (level >= 2) CHECK(hasKey(r.script, "praise.great.b" + std::to_string(level)));
        else CHECK(hasKey(r.script, "praise.best.b1"));
    }
    // After a good first move, the same 2.Nf3 is only the only move.
    Game g = gameOf(nullptr, {"e4"});
    Reviewer rv;
    rv.reset(2, White);
    reviewOf(rv, g, analysisOf({pvl(30, "e2e4 e7e5"), pvl(25, "d2d4 d7d5")}));
    g.play(g.position().parseSAN("e5"));
    g.play(g.position().parseSAN("Nf3"));
    const Review r = reviewOf(rv, g, root);
    CHECK(!r.verdict.great);
    CHECK(hasKey(r.script, "praise.only.b2"));
}

TEST(coach_core_review_praise_trade_when_ahead) {
    // A rook up, 1.Nxd5 exd5 trades a pair of knights: well done at levels 1-3.
    const char* fen = "6k1/5ppp/4p3/3n4/8/2N5/P4PPP/1R4K1 w - - 0 1";
    const ai::Analysis a0 = analysisOf({pvl(550, "c3d5 e6d5 b1b5 g8f8"), pvl(540, "a2a4 g8f8 a4a5")});
    for (int level = 1; level <= 4; ++level) {
        Game g = gameOf(fen, {"Nxd5"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        const std::string where = "trade b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.cls, MoveClass::Best);
        const std::string key = "praise.trade.b" + std::to_string(level);
        CHECK_EQ(hasKey(r.script, key), level <= 3);
        if (level <= 3) {
            const Beat* b = beatWithKey(r.script, key);
            const Arg* my = b ? b->line.arg("my") : nullptr;
            CHECK(my && my->piece == Knight && !my->own);
        }
    }
    // Not when level: the same trade with the material even is no lesson.
    const char* even = "6k1/5ppp/4p3/3n4/8/2N5/P4PPP/6K1 w - - 0 1";
    Game g = gameOf(even, {"Nxd5"});
    Reviewer rv;
    rv.reset(1, White);
    CHECK(!hasPrefix(reviewOf(rv, g, analysisOf({pvl(20, "c3d5 e6d5 g1f1"), pvl(10, "a2a4 g8f8")})).script, "praise.trade"));
}

TEST(coach_core_review_praise_castling) {
    // 4.O-O, the engine's move: a beginner is told well done, once a game.
    const ai::Analysis a0 = analysisOf({pvl(30, "e1g1 g8f6"), pvl(25, "c2c3 g8f6")});
    for (int level = 1; level <= 2; ++level) {
        Game g = gameOf(nullptr, {"e4", "e5", "Nf3", "Nc6", "Bc4", "Bc5", "O-O"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        checkScript(r.script, "castle");
        CHECK_EQ(hasKey(r.script, "praise.castle.b1"), level == 1);
    }
    // Black castles too.
    Game g = gameOf(nullptr, {"e4", "e5", "Nf3", "Nc6", "Bc4", "Bc5", "d3", "Nf6", "c3", "O-O"});
    Reviewer rv;
    rv.reset(1, Black);
    CHECK(hasKey(reviewOf(rv, g, analysisOf({pvl(20, "e8g8 d3d4"), pvl(15, "d7d6 e1g1")})).script, "praise.castle.b1"));
}

TEST(coach_core_review_praise_piece_saved) {
    // The knight on d4, attacked by the e5 pawn and unguarded, retreats: saved (level 1), when it
    // survives the move's own line; lost three plies later (1...e4 2.Kf1 exf3), it was not saved.
    const char* fen = "6k1/pp4pp/8/4p3/3N4/8/PP4PP/6K1 w - - 0 1";
    struct Case {
        const char* line;
        bool saved;
    };
    const Case cases[] = {
        {"d4f3 g8f7 f3d2 f7e6", true},          // kept, and it moves on
        {"d4f3 e5e4 g1f1 e4f3 g2f3", false},    // taken later, a pawn back for it
    };
    for (const Case& k : cases) {
        const ai::Analysis a0 = analysisOf({pvl(150, k.line), pvl(140, "d4b3 g8f7 b3d2 f7e6")});
        Game g = gameOf(fen, {"Nf3"});
        Reviewer rv;
        rv.reset(1, White);
        const Review r = reviewOf(rv, g, a0);
        checkScript(r.script, k.line);
        CHECK_EQ(r.verdict.cls, MoveClass::Best);
        CHECK_EQ(hasKey(r.script, "praise.saved.b1"), k.saved);
    }
    // Level 2 does not praise a piece merely saved.
    const ai::Analysis a0 = analysisOf({pvl(150, cases[0].line), pvl(140, "d4b3 g8f7 b3d2 f7e6")});
    Game g = gameOf(fen, {"Nf3"});
    Reviewer rv;
    rv.reset(2, White);
    CHECK(!hasKey(reviewOf(rv, g, a0).script, "praise.saved.b1"));
}

TEST(coach_core_review_praise_excellent_needs_a_harder_move) {
    // Level 3, 1.e4 (the engine's move, a quiet one): praised as excellent when the shallow search
    // wanted another move, unless that move is in A0's lines and as good (the same expected
    // result), which makes 1.e4 no harder to find than it.
    const ai::Analysis a0 = analysisOf({pvlWdl(30, "e2e4 e7e5", 80, 900, 20), pvlWdl(25, "d2d4 d7d5", 75, 905, 20),
                                        pvlWdl(-150, "g2g4 d7d5", 5, 600, 395)});
    struct Case {
        const char* shallow;
        bool praised;
    };
    const Case cases[] = {{"d2d4 d7d5", false}, {"g2g4 d7d5", true}, {"b1c3 d7d5", true}, {"e2e4 e7e5", false}};
    for (const Case& k : cases) {
        const ai::Analysis a3 = analysisOf({pvlWdl(40, k.shallow, 80, 900, 20)}, 6);
        Game g = gameOf(nullptr, {"e4"});
        Reviewer rv;
        rv.reset(3, White);
        ReviewInput in;
        in.game = &g;
        in.before = &a0;
        in.shallow = &a3;
        const Review r = rv.review(in);
        checkScript(r.script, k.shallow);
        CHECK_EQ(r.verdict.cls, MoveClass::Best);
        CHECK_EQ(hasKey(r.script, "praise.excellent.b3"), k.praised);
        if (hasKey(r.script, "praise.excellent.b3") != k.praised) dumpKeys(r.script, k.shallow);
    }
}
