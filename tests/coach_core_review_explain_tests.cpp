// Tests for the reasons the coach's review gives that the review tests leave aside. Four come with
// the embedded engine's own analyses, frozen here (depth 14, MultiPV 3; A3 the shallow search at
// depth 6), so that the facts are the engine's and the test needs no engine: a royal fork whose
// reply is forced (the tail "your king must get out of check, so I take your rook"), a back-rank
// combination (with the level-1 tip), a rook trade into a lost pawn ending, and a fork missed by a
// move that took one of its targets (the square is marked, not the piece that stands there now).
// The others are made-up engine facts on positions built for one rule each: a king left open by a
// pawn move in front of it or by a walk with the queens on, the bishop pair given up, the king
// that should come forward in an ending, and the hint that names no piece when the piece that had
// the better move left its square (castling).
#include "coach_core_review_support.h"

#include <string>
#include <vector>

using namespace chess;
using namespace coach;
using namespace corereview;

namespace {

Square sq(const char* s) { return parseSquare(s); }

// The review of the game's last move with the analyses a Coach game hands it (null: not searched).
Review reviewWith(Reviewer& rv, const Game& g, const ai::Analysis& a0, const ai::Analysis* a1,
                  const ai::Analysis* a2, const ai::Analysis* a3) {
    ReviewInput in;
    in.game = &g;
    in.before = &a0;
    in.played = a1;
    in.after = a2;
    in.shallow = a3;
    return rv.review(in);
}

bool gestureAt(const Beat& b, GestureKind kind, Square s, const char* anchor) {
    for (const Gesture& g : b.gestures)
        if (g.kind == kind && g.square == s && g.anchor == anchor) return true;
    return false;
}

bool arrow(const Beat& b, Square from, Square to) {
    for (const Mark& m : b.marks)
        if (m.kind == Mark::Kind::Arrow && m.from == from && m.to == to) return true;
    return false;
}

bool pieceArgIs(const Beat& b, const char* name, PieceType t, bool own, Square on) {
    const Arg* a = b.line.arg(name);
    return a && a->kind == Arg::Kind::Piece && a->piece == t && a->own == own && a->square == on;
}

std::vector<std::string> uciLine(std::initializer_list<const char*> moves) {
    std::vector<std::string> out;
    for (const char* m : moves) out.emplace_back(m);
    return out;
}

// ---- The royal fork (frozen): 1.h3?? Nc2+ 2.Kd2 Nxa1 --------------------------------------------

const char* kRoyalFen = "4k3/8/6b1/8/3n4/8/7P/R3K3 w - - 0 1";

ai::Analysis royalA0() {
    return analysisOf({
        pvlWdl(13, "a1a6 g6e4 e1f2 e4c6 a6c6 d4c6 f2e2", 8, 990, 2),
        pvlWdl(9, "e1f2 e8f7 h2h4 f7f6 a1a6 f6g7 a6a7 g7f6", 6, 992, 2),
        pvlWdl(6, "e1d1 d4f3 d1c1 g6f5 a1a5 f5g6 a5a6 g6h5 a6f6", 6, 991, 3),
    }, 14);
}
ai::Analysis royalA1() {
    return analysisOf({pvlWdl(-454, "h2h3 d4c2 e1d2 c2a1 h3h4 a1c2 d2c3 e8f7 c3c4 f7f6 c4b5 g6d3 b5b6", 0, 0, 1000)}, 14);
}
ai::Analysis royalA2() {
    return analysisOf({
        pvlWdl(428, "d4c2 e1d2 c2a1 h3h4 e8f7 d2c3 a1c2 h4h5", 1000, 0, 0),
        pvlWdl(-5, "d4f3 e1f2 f3g5 a1a7 g5e4 f2e3 e4g3", 3, 992, 5),
    }, 14);
}
ai::Analysis royalA3() { return analysisOf({pvlWdl(9, "e1f2 e8f7 h2h4 f7g7 a1a7 g7f6", 6, 992, 2)}, 6); }

}  // namespace

TEST(coach_core_review_royal_fork_with_a_forced_reply) {
    const ai::Analysis a0 = royalA0(), a1 = royalA1(), a2 = royalA2(), a3 = royalA3();
    // Levels 3-4, where the shallow search says which equal move is the natural one (Kf2: the
    // engine's Ra6 gives the rook for the knight too): the fork, and since the check leaves the
    // king no choice but to step away, the tail says that the rook falls because of it.
    for (int level = 3; level <= 4; ++level) {
        Game g = gameOf(kRoyalFen, {"h3"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewWith(rv, g, a0, &a1, level >= 4 ? &a2 : nullptr, &a3);
        const std::string where = "royal fork b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK_EQ(r.claim.type, ExType::Fork);
        CHECK_EQ(r.claim.lost, 5);
        CHECK_EQ(r.claim.plies, 3);
        CHECK(r.claim.line == uciLine({"d4c2", "e1d2", "c2a1"}));
        CHECK(r.offersTakeback);
        const Beat* cause = beatWithKey(r.script, "ex.fork.b" + std::to_string(level));
        const Beat* tail = beatWithKey(r.script, "ex.fork.tail_king");
        CHECK(cause && tail);
        if (!cause || !tail) {
            dumpKeys(r.script, where.c_str());
            continue;
        }
        CHECK(pieceArgIs(*cause, "t1", King, true, sq("e1")));
        CHECK(pieceArgIs(*cause, "t2", Rook, true, sq("a1")));
        CHECK(arrow(*cause, sq("d4"), sq("c2")));
        // "Your king must get out of check, so I take your rook": the king and the rook pointed at.
        CHECK(pieceArgIs(*tail, "your", King, true, sq("e1")));
        CHECK(pieceArgIs(*tail, "t1", Rook, true, sq("a1")));
        CHECK(gestureAt(*tail, GestureKind::PointPiece, sq("e1"), "your"));
        CHECK(gestureAt(*tail, GestureKind::PointPiece, sq("a1"), "t1"));
        // The better move named is the natural one, not the engine's first.
        const Beat* better = beatWithKey(r.script, "ex.better.b" + std::to_string(level));
        CHECK(better != nullptr);
        if (better) CHECK(arrow(*better, sq("e1"), sq("f2")));
        // Level 3 also teaches the fork square.
        CHECK_EQ(hasKey(r.script, "ex.fork.more.b3"), level == 3);
    }
    // Without the shallow search the better move is the engine's Ra6, which loses the rook as well:
    // no fork is said ("the rook had a better move"), the verdict and the offer stay.
    for (int level : {1, 3, 6}) {
        Game g = gameOf(kRoyalFen, {"h3"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewWith(rv, g, a0, &a1, nullptr, nullptr);
        const std::string where = "royal fork, no A3, b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK_EQ(r.claim.type, ExType::None);
        CHECK(!hasPrefix(r.script, "ex.fork"));
        const Beat* hint = beatWithKey(r.script, "ex.hint.b" + std::to_string(level));
        CHECK(hint != nullptr);
        if (hint) CHECK(pieceArgIs(*hint, "your", Rook, true, sq("a1")));
        if (!hint) dumpKeys(r.script, where.c_str());
    }
}

// ---- The back rank (frozen): 1.Rd7?? Re1+ 2.Qf1 Rxf1+ 3.Kxf1 -----------------------------------

namespace {

const char* kBackRankFen = "r3r1k1/5ppp/8/1Q6/8/8/5PPP/3R2K1 w - - 0 1";

ai::Analysis backA0() {
    return analysisOf({
        pvlWdl(694, "h2h3 h7h6 g1h2 a8b8 b5d5 b8a8 h3h4 a8a4 g2g3 a4e4 h2g2 e4e6 d5f3 e6f6 f3d3 f6e6 d3d5 e6e4 d5a5 e4e5 a5c7 e5e7 d1d7 e7d7 c7d7", 1000, 0, 0),
        pvlWdl(684, "b5d5 g7g6 d5d2 a8c8 h2h3 g8g7 d2d7 g7g8 g1h2 e8f8 d7a7", 1000, 0, 0),
        pvlWdl(678, "h2h4 a8b8 b5c5 b8c8 c5d5 c8a8 g1h2 a8b8 d5c6", 1000, 0, 0),
    }, 14);
}
ai::Analysis backA1() {
    return analysisOf({pvlWdl(-3, "d1d7 e8e1 b5f1 e1f1 g1f1 g7g5 d7d1 g8g7 g2g4 a8a4 h2h3", 4, 991, 5)}, 14);
}
ai::Analysis backA2() {
    return analysisOf({
        pvlWdl(3, "e8e1 b5f1 e1f1 g1f1 g7g5 g2g3 g8g7 h2h3 a8a2", 5, 991, 4),
        pvlWdl(1, "a8a1 b5f1 a1f1 g1f1 g7g5 d7d1 e8e5 h2h3 e5e7 g2g3 g8g7", 5, 991, 4),
    }, 14);
}
ai::Analysis backA3() { return analysisOf({pvlWdl(686, "h2h4 a8b8 b5a4 h7h5 g1h2 g7g6", 1000, 0, 0)}, 6); }

}  // namespace

TEST(coach_core_review_back_rank) {
    const ai::Analysis a0 = backA0(), a1 = backA1(), a2 = backA2(), a3 = backA3();
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(kBackRankFen, {"Rd7"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewWith(rv, g, a0, &a1, level >= 4 ? &a2 : nullptr,
                                    level == 3 || level == 4 ? &a3 : nullptr);
        const std::string where = "back rank b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK_EQ(r.verdict.exType, ExType::BackRank);
        CHECK_EQ(r.claim.type, ExType::BackRank);
        CHECK_EQ(r.claim.lost, 4);   // the queen for the rook
        CHECK_EQ(r.claim.plies, 4);
        CHECK(r.claim.line == uciLine({"e8e1", "b5f1", "e1f1", "g1f1"}));
        const Beat* cause = beatWithKey(r.script, "ex.back_rank.b" + std::to_string(level));
        CHECK(cause != nullptr);
        if (!cause) {
            dumpKeys(r.script, where.c_str());
            continue;
        }
        // The king without air, the rook that checks on e1 (its move traced on the board).
        CHECK(pieceArgIs(*cause, "your", King, true, sq("g1")));
        CHECK(pieceArgIs(*cause, "my", Rook, false, sq("e8")));
        const Arg* at = cause->line.arg("sq");
        CHECK(at && at->square == sq("e1"));
        CHECK(gestureAt(*cause, GestureKind::PointPiece, sq("g1"), "your"));
        CHECK(arrow(*cause, sq("e8"), sq("e1")));
        bool traced = false;
        for (const Gesture& gs : cause->gestures)
            if (gs.kind == GestureKind::Trace && gs.square == sq("e1")) traced = gs.anchor == (level <= 2 ? "my" : "line");
        CHECK(traced);
        // A beginner hears how to prevent it: a window for the king.
        CHECK_EQ(hasKey(r.script, "ex.back_rank.tip.b1"), level == 1);
    }
}

// ---- A rook trade into a lost pawn ending (frozen): 1.Rxd8?? Kxd8 ------------------------------

namespace {

const char* kTradeFen = "3r4/4kppp/8/p7/8/4PKP1/5P1P/3R4 w - - 0 1";

ai::Analysis tradeA0() {
    return analysisOf({
        pvlWdl(-4, "d1a1 d8a8 a1a4 e7d6 f3e2 d6c6 e2d2 c6b5 a4d4 a5a4 d2c2 a4a3 c2b1 a3a2 b1a1", 3, 992, 5),
        pvlWdl(-72, "d1b1 a5a4 b1a1 d8a8 f3e4 a4a3 a1a2 h7h5 e4d4 g7g5 f2f3 e7e6 h2h4 g5g4 f3g4 h5g4 d4c4 e6e5 c4b3 e5e4 a2a3 a8e8", 0, 823, 177),
        pvlWdl(-100, "d1e1 a5a4 e1a1 d8a8 f3e4 a4a3 a1a2 h7h5 e4d4 g7g5 f2f3 e7e6 h2h4 g5g4 f3g4 h5g4 d4c4 e6e5 c4b3 e5e4", 0, 500, 500),
    }, 14);
}
ai::Analysis tradeA1() {
    return analysisOf({pvlWdl(-241, "d1d8 e7d8 f3e4 d8c7 g3g4 c7c6 e4d4 c6b5 d4c3 a5a4 g4g5 b5c5 h2h4 h7h6 g5h6 g7h6 h4h5 c5d5 c3b4 d5e4 b4a4", 0, 0, 1000)}, 14);
}
ai::Analysis tradeA2() {
    return analysisOf({
        pvlWdl(249, "e7d8 f3e4 d8c7 g3g4 c7c6 e4d4 c6b5 d4c3 a5a4 g4g5 b5c5 f2f3 c5b5", 1000, 0, 0),
        pvlWdl(-738, "g7g6 d8d4 h7h6 d4d1 a5a4 f3f4 e7f6", 0, 0, 1000),
    }, 14);
}
ai::Analysis tradeA3() { return analysisOf({pvlWdl(-4, "d1a1 d8a8 a1a4 e7d6 f3e2 d6c6", 3, 992, 5)}, 6); }

}  // namespace

TEST(coach_core_review_bad_trade_into_a_pawn_ending) {
    const ai::Analysis a0 = tradeA0(), a1 = tradeA1(), a2 = tradeA2(), a3 = tradeA3();
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(kTradeFen, {"Rxd8"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewWith(rv, g, a0, &a1, level >= 4 ? &a2 : nullptr,
                                    level == 3 || level == 4 ? &a3 : nullptr);
        const std::string where = "bad trade b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        if (level >= 3) {
            // The trade itself is the reason: a pawn ending the engine calls lost (0/0/1000).
            CHECK_EQ(r.claim.type, ExType::BadTrade);
            CHECK_EQ(r.claim.lost, 0);
            CHECK(r.claim.line == uciLine({"e7d8"}));
            const Beat* cause = beatWithKey(r.script, "ex.bad_trade.b" + std::to_string(level));
            CHECK(cause != nullptr);
            if (cause) CHECK(cause->look == Look::Board);
            if (!cause) dumpKeys(r.script, where.c_str());
            CHECK(r.offersTakeback);
        } else {
            // Levels 1-2 never speak of pawn endings: the rook had a better move.
            CHECK_EQ(r.claim.type, ExType::None);
            CHECK(!hasPrefix(r.script, "ex.bad_trade"));
            CHECK(hasKey(r.script, "ex.hint.b" + std::to_string(level)));
        }
    }
}

// ---- A fork missed by a move that took one of its targets (frozen): 1.Qxc8? (1.Ne7+) -----------

TEST(coach_core_review_missed_fork_whose_target_was_taken) {
    const char* fen = "r1q2rk1/5ppp/8/3N4/8/7P/2Q2PP1/3R2K1 w - - 0 1";
    const ai::Analysis a0 = analysisOf({
        pvlWdl(708, "d5e7 g8h8 e7c8 a8c8 c2f5 c8c7 g2g3 c7e7 h3h4 f8b8", 1000, 0, 0),
        pvlWdl(-13, "c2c8 a8c8 d5e7 g8h8 e7c8 f8c8 h3h4 g7g6 g1h2 f7f6 d1d6 c8a8 d6f6", 2, 989, 9),
        pvlWdl(-336, "c2e4 c8c5 d5e7 g8h8 e4e2 c5c3 g2g3 a8a1 d1a1 c3a1 g1g2", 0, 0, 1000),
    }, 14);
    const ai::Analysis a2 = analysisOf({
        pvlWdl(60, "a8c8 d5e7 g8h8 e7c8 f8c8", 104, 896, 0),
        pvlWdl(-7, "f8c8 d5e7 g8h8 e7c8 a8c8 g1h2 f7f6 d1a1 f6f5", 3, 990, 7),
    }, 14);
    const ai::Analysis a3 = analysisOf({pvlWdl(708, "d5e7 g8h8 e7c8 a8c8 c2f5 c8c7", 1000, 0, 0)}, 6);
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(fen, {"Qxc8"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewWith(rv, g, a0, nullptr, level >= 4 ? &a2 : nullptr,
                                    level == 3 || level == 4 ? &a3 : nullptr);
        const std::string where = "missed fork b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK_EQ(r.claim.type, ExType::MissedFork);
        const Beat* cause = beatWithKey(r.script, "ex.missed_fork.b" + std::to_string(level));
        CHECK(cause != nullptr);
        if (!cause) {
            dumpKeys(r.script, where.c_str());
            continue;
        }
        // Ne7+ forked my king and my queen; the queen is gone from c8 (the player's queen stands
        // there now): its square is marked, the finger does not point at the piece on it.
        CHECK(pieceArgIs(*cause, "t1", King, false, sq("g8")));
        CHECK(pieceArgIs(*cause, "t2", Queen, false, sq("c8")));
        CHECK(arrow(*cause, sq("d5"), sq("e7")));
        CHECK(gestureAt(*cause, GestureKind::PointPiece, sq("g8"), "t1"));
        CHECK(marks(*cause, Mark::Kind::Square, sq("c8")));
        CHECK(!marks(*cause, Mark::Kind::Piece, sq("c8")));
        for (const Gesture& gs : cause->gestures) CHECK(!(gs.kind == GestureKind::PointPiece && gs.square == sq("c8")));
        CHECK_EQ(g.position().at(sq("c8")), (Piece{Queen, White}));
    }
}

// ---- King safety (made-up engine facts) ---------------------------------------------------------

namespace {

// White: two rooks and a bishop against the queen, the king on g1 without an f-pawn. The engine's
// lines are made up: after 1.g3 (or 1.Kf2) the queen gives perpetual check.
const char* kOpenFen = "6k1/pp1q2pp/8/8/8/QP6/P5PP/RRB3K1 w - - 0 1";

}  // namespace

TEST(coach_core_review_king_shield_pawn_moved) {
    // 1.g3?: the pawn in front of the castled king moved, and the queen checks on d4, e4, d4...
    const ai::Analysis a0 = analysisOf({pvl(400, "h2h3 d7d4 g1h2 d4e5 g2g3"),
                                        pvl(0, "g2g3 d7d4 g1h1 d4e4 h1g1 e4d4 g1h1 d4e4")});
    for (int level : {1, 3, 5}) {
        Game g = gameOf(kOpenFen, {"g3"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        const std::string where = "king shield b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK_EQ(r.verdict.exType, ExType::KingSafety);
        const Beat* cause = beatWithKey(r.script, "ex.king_shield.b" + std::to_string(level));
        CHECK(cause != nullptr);
        if (!cause) {
            dumpKeys(r.script, where.c_str());
            continue;
        }
        // The square the pawn left, the king it sheltered, and the checks that follow.
        const Arg* at = cause->line.arg("sq");
        CHECK(at && at->square == sq("g2"));
        CHECK(pieceArgIs(*cause, "your", King, true, sq("g1")));
        CHECK(gestureAt(*cause, GestureKind::PointPiece, sq("g1"), "your"));
        CHECK(marks(*cause, Mark::Kind::Square, sq("g2")));
        const Arg* line = cause->line.arg("line");
        CHECK(line && line->san.rfind("Qd4+", 0) == 0);
    }
}

TEST(coach_core_review_king_walk_with_queens_on) {
    // 1.Kf2?: the king walks out with the queens on, and the queen checks on d4, d3, d4...
    const ai::Analysis a0 = analysisOf({pvl(400, "h2h3 d7d4 g1h2 d4e5 g2g3"),
                                        pvl(0, "g1f2 d7d4 f2f1 d4d3 f1f2 d3d4 f2f1 d4d3")});
    for (int level : {1, 2, 4}) {
        Game g = gameOf(kOpenFen, {"Kf2"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        const std::string where = "king walk b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.exType, ExType::KingSafety);
        const Beat* cause = beatWithKey(r.script, "ex.king_exposed.b" + std::to_string(level));
        CHECK(cause != nullptr);
        if (!cause) {
            dumpKeys(r.script, where.c_str());
            continue;
        }
        // An exposed king: the square named is the king's own, nothing marked where it came from.
        const Arg* at = cause->line.arg("sq");
        CHECK(at && at->square == sq("f2"));
        CHECK(pieceArgIs(*cause, "your", King, true, sq("f2")));
        CHECK(!marks(*cause, Mark::Kind::Square, sq("g1")));
    }
}

TEST(coach_core_review_king_behind_its_pawns_is_not_open) {
    // Neither a pawn moved in front of the king nor the king itself (1.a3), but three squares next
    // to it attacked (f1 by the bishop, f2 by the queen, h2 by both) and one check, 1...Bc5+. The
    // king behind its pawns (f3, g2, h2) is not called open; the same king with only the h-pawn is.
    const char* sheltered = "r5k1/p1p2ppp/b1pb4/8/7q/1Q3P2/PPP3PP/RNB3K1 w - - 0 1";
    const char* bare = "r5k1/p1p2ppp/b1pb4/8/7q/1Q6/PPP4P/RNB3K1 w - - 0 1";
    const ai::Analysis a0 = analysisOf({pvl(300, "b1d2 d6c5 g1h1 h4h5"), pvl(-100, "a2a3 d6c5 g1h1 h4h5 b1c3 a8e8")});
    for (bool shelter : {true, false}) {
        for (int level : {1, 2, 4}) {
            Game g = gameOf(shelter ? sheltered : bare, {"a3"});
            Reviewer rv;
            rv.reset(level, White);
            const Review r = reviewOf(rv, g, a0);
            const std::string where = std::string(shelter ? "sheltered" : "bare") + " king b" + std::to_string(level);
            checkScript(r.script, where.c_str());
            CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
            // Levels 2-3 want two checks ("I can give many checks"): one is not enough there.
            const bool open = !shelter && level != 2;
            CHECK_EQ(r.verdict.exType == ExType::KingSafety, open);
            if (open) {
                const Beat* cause = beatWithKey(r.script, "ex.king_exposed.b" + std::to_string(level));
                CHECK(cause != nullptr);
                if (cause) CHECK(pieceArgIs(*cause, "your", King, true, sq("g1")));
            }
            if (r.verdict.exType == ExType::KingSafety && !open) dumpKeys(r.script, where.c_str());
        }
    }
}

// ---- The bishop pair (made-up engine facts) ----------------------------------------------------

TEST(coach_core_review_bishop_pair_given_up) {
    // 7.Bxf6 Bxf6: a bishop for a knight, the bishop pair handed over for a modest drop (1.2 pawns).
    // Levels 5-6 say so; below, the bishop pair is no reason the coach gives.
    const ai::Analysis a0 = analysisOf({pvl(80, "c2c3 c8g4 h2h3 g4h5"), pvl(-40, "g5f6 e7f6 c2c3 c8g4 h2h3")});
    for (int level = 3; level <= 6; ++level) {
        Game g = gameOf(nullptr, {"e4", "e5", "Nf3", "Nc6", "Bc4", "Nf6", "d3", "Be7", "O-O", "O-O", "Bg5", "d6", "Bxf6"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        const std::string where = "bishop pair b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.cls, MoveClass::Mistake);
        CHECK_EQ(r.verdict.exType == ExType::BadTrade, level >= 5);
        CHECK_EQ(hasKey(r.script, "ex.bishop_pair.b" + std::to_string(level)), level >= 5);
        if (level >= 5 && !hasKey(r.script, "ex.bishop_pair.b" + std::to_string(level))) dumpKeys(r.script, where.c_str());
    }
    // Not when the better move wins more material (7.c3 and 9.Qxb7 wins a pawn): 7.Bxf6 is no
    // bishop-pair question then.
    const ai::Analysis more =
        analysisOf({pvl(80, "c2c3 c8g4 d1b3 d8d7 b3b7 g4f3 g2f3 a8b8"), pvl(-40, "g5f6 e7f6 c2c3 c8g4 h2h3")});
    Game g = gameOf(nullptr, {"e4", "e5", "Nf3", "Nc6", "Bc4", "Nf6", "d3", "Be7", "O-O", "O-O", "Bg5", "d6", "Bxf6"});
    Reviewer rv;
    rv.reset(5, White);
    const Review r = reviewOf(rv, g, more);
    CHECK(!hasPrefix(r.script, "ex.bishop_pair"));
}

// ---- The king forward in a pawn ending (made-up engine facts) ----------------------------------

TEST(coach_core_review_king_forward_in_a_pawn_ending) {
    // 1.h3? while Black's king stands in the centre: 1.Kf2, a step forward and nearer the middle,
    // was the move.
    const char* fen = "8/pp4pp/4k3/8/8/8/PP4PP/6K1 w - - 0 1";
    const ai::Analysis a0 = analysisOf({pvl(200, "g1f2 e6d5 f2e3 d5c4 e3d2"), pvl(-50, "h2h3 e6d5 g1f2 d5c4 f2e2 c4b3")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(fen, {"h3"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        const std::string where = "king forward b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK_EQ(r.verdict.exType, ExType::Endgame);
        const Beat* cause = nullptr;
        for (const Beat& b : r.script)
            if (b.line.key.rfind("ex.king_active.b", 0) == 0) cause = &b;
        CHECK(cause != nullptr);
        if (!cause) {
            dumpKeys(r.script, where.c_str());
            continue;
        }
        const Arg* at = cause->line.arg("sq");
        CHECK(at && at->square == sq("f2"));
        // Beginners see the king pointed at; above, the move traced.
        if (level <= 2) CHECK(gestureAt(*cause, GestureKind::PointPiece, sq("g1"), "your"));
        else CHECK(arrow(*cause, sq("g1"), sq("f2")));
    }
    // A step back toward the middle is no "forward" (1...Kf7 from e8 would be, 1.Kf1 is not).
    const ai::Analysis back = analysisOf({pvl(200, "g1f1 e6d5 f1e2 d5c4"), pvl(-50, "h2h3 e6d5 g1f2 d5c4 f2e2 c4b3")});
    Game g = gameOf("8/pp4pp/4k3/8/8/8/PP4PP/6K1 w - - 0 1", {"h3"});
    Reviewer rv;
    rv.reset(1, White);
    const Review r = reviewOf(rv, g, back);
    CHECK(!hasPrefix(r.script, "ex.king_active"));
}

// ---- The hint that names no piece (made-up engine facts) ---------------------------------------

TEST(coach_core_review_hint_after_castling_names_no_piece) {
    // 1.O-O?? when 1.Rh8+ wins the queen behind the king: no reason the board can show (a skewer
    // missed has no detector), so a beginner hears the hint before the offer. The piece that had
    // the better move, the rook on h1, left its square by castling: the hint names no piece.
    const char* fen = "q1k5/8/8/8/8/8/1PPP4/4K2R w K - 0 1";
    const ai::Analysis a0 = analysisOf({pvl(700, "h1h8 c8b7 h8a8 b7a8 e1d1"), pvl(-300, "e1g1 a8e4 d2d3 e4e2")});
    Game g = gameOf(fen, {"O-O"});
    Reviewer rv;
    rv.reset(1, White);
    const Review r = reviewOf(rv, g, a0);
    checkScript(r.script, "hint after castling");
    CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
    CHECK_EQ(r.claim.type, ExType::None);
    CHECK(r.offersTakeback);
    const Beat* hint = beatWithKey(r.script, "ex.hint.move");
    CHECK(hint != nullptr);
    if (hint) CHECK(hint->look == Look::Player);
    CHECK(!hasKey(r.script, "ex.hint.b1"));
    if (!hint) dumpKeys(r.script, "hint after castling");
    // A rook move instead (1.Rf1??): the rook stands on the table, so the hint names it, with what
    // its better move wins.
    Game h = gameOf(fen, {"Rf1"});
    const ai::Analysis a1 = analysisOf({pvl(700, "h1h8 c8b7 h8a8 b7a8 e1d1"), pvl(-300, "h1f1 a8e4 d2d3 e4e2")});
    Reviewer rh;
    rh.reset(1, White);
    const Review s = reviewOf(rh, h, a1);
    const Beat* named = beatWithKey(s.script, "ex.hint.gain.b1");
    CHECK(named != nullptr);
    if (named) CHECK(pieceArgIs(*named, "your", Rook, true, sq("f1")));
    if (!named) dumpKeys(s.script, "hint after a rook move");
    CHECK(!hasKey(s.script, "ex.hint.move"));
}
