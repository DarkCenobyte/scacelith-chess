// The rules lesson (src/coach/lesson.*): every chapter replays on a real chess::Position, every
// line exists in the English catalog with its placeholders and anchors, every exercise accepts
// its solution and answers a wrong move, reactions stay legal on the board, and the reasons given
// for illegal moves on the classic cases. Also the event helpers (src/coach/events.*).
#include "test.h"
#include "coach/catalog.h"
#include "coach/events.h"
#include "coach/lesson.h"
#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

using namespace coach;
using chess::Move;
using chess::Position;
using chess::parseSquare;

namespace {

Catalog& catalog() {
    static Catalog c;
    static bool loaded = c.load();
    (void)loaded;
    return c;
}

std::set<std::string> placeholderNames(const std::string& v) {
    std::set<std::string> out;
    static const std::regex re("\\{([A-Za-z0-9_@]+)(?::[A-Za-z0-9_]+)?\\}");
    for (auto it = std::sregex_iterator(v.begin(), v.end(), re); it != std::sregex_iterator(); ++it) out.insert((*it)[1].str());
    return out;
}

// Every variant (and .spoken override) of a key in English.
std::vector<std::string> templates(const std::string& key) {
    std::vector<std::string> out;
    int n = catalog().variants(key);
    for (int v = 1; v <= n; ++v) {
        std::string k = v == 1 ? key : key + "." + std::to_string(v);
        for (const std::string& kk : {k, k + ".spoken"})
            if (const std::string* t = catalog().find("en", kk)) out.push_back(*t);
    }
    return out;
}

// The line's key exists, every placeholder has an argument (dynamic ones allowed), and every
// anchor a gesture or mark names is in the text.
int g_lines = 0;
void checkLine(const Line& l, const std::string& where, const std::set<std::string>& dynamic = {},
               const std::vector<Gesture>& g = {}, const std::vector<Mark>& m = {}) {
    if (l.empty()) return;
    ++g_lines;
    if (!catalog().has(l.key)) {
        std::fprintf(stderr, "  %s: missing key %s\n", where.c_str(), l.key.c_str());
        CHECK(false);
        return;
    }
    for (const std::string& t : templates(l.key)) {
        std::set<std::string> names = placeholderNames(t);
        for (const std::string& n : names) {
            if (n[0] == '@' || dynamic.count(n) || l.arg(n)) continue;
            std::fprintf(stderr, "  %s: %s has no argument {%s}\n", where.c_str(), l.key.c_str(), n.c_str());
            CHECK(false);
        }
        auto anchored = [&](const std::string& a) {
            if (a.empty() || names.count(a)) return;
            std::fprintf(stderr, "  %s: %s has no anchor %s\n", where.c_str(), l.key.c_str(), a.c_str());
            CHECK(false);
        };
        for (auto& x : g) anchored(x.anchor);
        for (auto& x : m) anchored(x.anchor);
    }
    // It renders in both forms without a leftover brace (the arguments the lesson adds when it
    // says the line stand in as squares).
    Line full = l;
    for (const std::string& d : dynamic)
        if (!full.arg(d)) full.with(d, Arg::ofSquare(parseSquare("e4")));
    for (bool sp : {false, true}) {
        Catalog::Rendered r = catalog().renderVariant(full, "en", sp, 1);
        CHECK(r.text.find('{') == std::string::npos);
        CHECK(!r.text.empty());
    }
}

void checkReply(const LessonReply& r, const std::string& where) {
    checkLine(r.line, where, {"to", "esc"});
    checkLine(r.demoLine, where, {"to", "esc"});
    checkLine(r.after, where, {"to", "esc"});
}

// Plays the beats of a reaction on a board: demonstrations must be legal where they happen and
// every Rewind takes back demonstrations that happened.
void playBeats(const Script& s, Position& pos, std::vector<Position>& demos, const std::string& where) {
    for (const Beat& b : s) {
        if (b.kind == BeatKind::Say || b.kind == BeatKind::OfferTakeback) checkLine(b.line, where, {}, b.gestures, b.marks);
        if (b.kind == BeatKind::DemoMove) {
            Move m = pos.parseUCI(b.uci);
            if (!m.valid()) {
                std::fprintf(stderr, "  %s: demonstration %s is illegal in %s\n", where.c_str(), b.uci.c_str(), pos.fen().c_str());
                CHECK(false);
                continue;
            }
            demos.push_back(pos);
            pos.makeMove(m);
            checkLine(b.line, where);
        }
        if (b.kind == BeatKind::Rewind) {
            CHECK(b.count >= 1 && b.count <= int(demos.size()));
            for (int i = 0; i < b.count && !demos.empty(); ++i) {
                pos = demos.back();
                demos.pop_back();
            }
        }
    }
}

bool hasKey(const Script& s, const std::string& key) {
    for (const Beat& b : s)
        if (b.line.key == key) return true;
    return false;
}

bool hasDemo(const Script& s, const std::string& uci) {
    for (const Beat& b : s)
        if (b.kind == BeatKind::DemoMove && b.uci == uci) return true;
    return false;
}

Move uciMove(const Position& p, const char* uci) { return p.parseUCI(uci); }

std::string renderEn(const Line& l, bool spoken = false) { return catalog().renderVariant(l, "en", spoken, 1).text; }

}  // namespace

// Replays every chapter the way the director will: positions, the coach's real moves, the
// exercises (each position matches its expectation), demonstrations and rewinds.
TEST(coach_lesson_chapters_replay) {
    Lesson lesson;
    CHECK_EQ(lesson.chapters().size(), size_t(10));
    int says = 0, waits = 0, demos = 0;
    std::set<int> expects;
    for (size_t c = 0; c < lesson.chapters().size(); ++c) {
        const LessonChapter& ch = lesson.chapters()[c];
        std::string where = "chapter " + ch.id;
        checkLine(ch.title, where);
        CHECK(!ch.beats.empty() && ch.beats.front().kind == BeatKind::SetPosition);
        Position pos;
        std::vector<Position> demoStack;
        for (const Beat& b : ch.beats) {
            switch (b.kind) {
            case BeatKind::SetPosition:
                CHECK(pos.setFEN(b.fen));
                CHECK(demoStack.empty());
                break;
            case BeatKind::PlayMove: {
                Move m = pos.parseUCI(b.uci);
                CHECK(m.valid());
                if (m.valid()) pos.makeMove(m);
                break;
            }
            case BeatKind::WaitMove: {
                ++waits;
                CHECK(b.expect >= 0 && size_t(b.expect) < lesson.expectations().size());
                if (b.expect < 0 || size_t(b.expect) >= lesson.expectations().size()) break;
                CHECK(!expects.count(b.expect));
                expects.insert(b.expect);
                CHECK_EQ(lesson.chapterOf(b.expect), int(c));
                const Expectation& e = lesson.expectation(b.expect);
                CHECK_EQ(e.fen, pos.fen());
                CHECK_EQ(pos.sideToMove(), chess::White);   // the player is White
                // The model answer continues the chapter.
                std::string model = !e.solution.empty() ? e.solution : e.accept.front();
                Move m = pos.parseUCI(model);
                CHECK(m.valid());
                if (!m.valid()) break;
                LessonReaction r = lesson.judge(b.expect, pos, m, 0);
                CHECK(r.accepted);
                CHECK_EQ(r.undo, 0);
                pos.makeMove(m);
                break;
            }
            case BeatKind::DemoMove:
                ++demos;
                playBeats({b}, pos, demoStack, where);
                break;
            case BeatKind::Rewind:
                playBeats({b}, pos, demoStack, where);
                break;
            case BeatKind::Say:
                ++says;
                checkLine(b.line, where, {}, b.gestures, b.marks);
                // The gaze rule: pointing or marks look at the board, general talk at the player.
                if (b.gestures.empty() && b.marks.empty()) CHECK(b.look == Look::Player);
                for (const Gesture& g : b.gestures) {
                    if (g.kind == GestureKind::PointPiece) CHECK(g.square != chess::NoSquare && !pos.at(g.square).empty());
                    if (g.kind == GestureKind::Trace) CHECK(g.path.size() >= 2);
                    if (g.kind == GestureKind::PointObject) CHECK(g.object != TableObject::None);
                }
                for (const Mark& mk : b.marks)
                    if (mk.kind == Mark::Kind::Piece) CHECK(!pos.at(mk.square).empty());
                break;
            default: break;
            }
        }
        CHECK(demoStack.empty());
        CHECK(!lesson.chapterLines(int(c)).empty());
        for (const Line& l : lesson.chapterLines(int(c))) checkLine(l, where + " (pre-synthesis)");
    }
    CHECK_EQ(expects.size(), lesson.expectations().size());
    std::fprintf(stderr, "  %d spoken beats, %d exercises, %d demonstrations, %d lines checked\n", says, waits, demos, g_lines);
    CHECK(says >= 100);
    CHECK(waits >= 20);
}

// Every exercise accepts at least one legal move, all its lines exist, a wrong move (when one
// exists) is answered and taken back, and every reaction is playable on the board, whatever the
// number of failures.
TEST(coach_lesson_exercises_judge_moves) {
    Lesson lesson;
    for (size_t i = 0; i < lesson.expectations().size(); ++i) {
        const Expectation& e = lesson.expectation(int(i));
        std::string where = "exercise " + std::to_string(i) + " (" + e.fen + ")";
        Position pos;
        CHECK(pos.setFEN(e.fen));
        checkLine(e.ask, where);
        checkLine(e.success, where, {"to"});
        checkLine(e.hint1, where);
        checkLine(e.hint2, where, {}, e.hint2Gestures, e.hint2Marks);
        for (auto& s : e.successFor) checkLine(s.second, where, {"to"});
        for (auto& r : e.acceptOnRetry) checkLine(r.line, where);
        for (auto& r : e.replies) checkReply(r, where);
        for (auto& r : e.illegal) checkReply(r, where);
        if (!e.solution.empty()) CHECK(pos.parseUCI(e.solution).valid());
        for (auto& a : e.accept) CHECK(pos.parseUCI(a).valid());

        int accepted = 0, wrong = 0;
        for (const Move& m : pos.legalMoves()) {
            LessonReaction r = lesson.judge(int(i), pos, m, 0);
            if (r.accepted) {
                ++accepted;
                CHECK_EQ(r.undo, 0);
                CHECK(!r.before.empty());
                Position p = pos;
                std::vector<Position> demos;
                playBeats(r.before, p, demos, where);
                continue;
            }
            ++wrong;
            CHECK_EQ(r.undo, 1);
            CHECK(!r.before.empty());
            for (int failures = 0; failures <= 4; ++failures) {
                LessonReaction rf = lesson.judge(int(i), pos, m, failures);
                if (rf.accepted) break;   // accepted from a later try on
                Position after = pos;
                after.makeMove(m);
                std::vector<Position> demos;
                playBeats(rf.before, after, demos, where + " before");
                CHECK(demos.empty());
                Position back = pos;   // the player's move taken back
                playBeats(rf.after, back, demos, where + " after");
                CHECK(demos.empty());
                if (failures == 2 && !e.solution.empty()) CHECK(hasDemo(rf.after, e.solution));
                if (failures >= 3) CHECK(hasKey(rf.before, "lesson.almost"));
            }
        }
        CHECK(accepted >= 1);
        if (e.kind == Expectation::Kind::AnyOf && e.acceptOnRetry.empty()) CHECK_EQ(accepted, int(e.accept.size()));
        // A typical wrong move is refused (unless every legal move is right: capture the checker).
        if ((e.kind == Expectation::Kind::AnyOf || e.kind == Expectation::Kind::Mates || e.kind == Expectation::Kind::Promotes) &&
            accepted < int(pos.legalMoves().size()))
            CHECK(wrong >= 1);
        if (wrong == 0) {
            // Then an illegal attempt is explained instead (3.4: the king steps away from the queen).
            bool explained = false;
            for (int from = 0; from < 64 && !explained; ++from)
                for (int to = 0; to < 64 && !explained; ++to)
                    if (pos.at(chess::Square(from)).color == chess::White && !pos.at(chess::Square(from)).empty() &&
                        from != to && !pos.findLegal(chess::Square(from), chess::Square(to), chess::Queen).valid() &&
                        whyIllegal(pos, chess::Square(from), chess::Square(to)).reason == IllegalReason::MustAnswerCheck)
                        explained = !lesson.explainIllegal(int(i), pos, chess::Square(from), chess::Square(to)).empty();
            CHECK(explained || e.kind == Expectation::Kind::AnyLegal || e.kind == Expectation::Kind::EscapesCheck);
        }
        // Idle hints: the ask (or hint1), then a hint that points.
        Script h1 = lesson.idleHint(int(i), 1), h2 = lesson.idleHint(int(i), 2);
        CHECK_EQ(h1.size(), size_t(1));
        CHECK_EQ(h2.size(), size_t(1));
        if (!h2.empty()) CHECK(!h2[0].gestures.empty() || !h2[0].marks.empty());
    }
}

// The answers the pedagogy spells out for particular moves.
TEST(coach_lesson_specific_reactions) {
    Lesson lesson;
    auto find = [&](const std::string& fenStart) {
        for (size_t i = 0; i < lesson.expectations().size(); ++i)
            if (lesson.expectation(int(i)).fen.compare(0, fenStart.size(), fenStart) == 0) return int(i);
        return -1;
    };
    auto judge = [&](int e, const char* uci, int failures = 0) {
        Position p;
        p.setFEN(lesson.expectation(e).fen);
        return lesson.judge(e, p, uciMove(p, uci), failures);
    };
    // Chapter 0: the square played is named.
    int kingStep = find("4k3/8/8/8/8/8/8/4K3 w");
    CHECK(kingStep >= 0);
    LessonReaction r0 = judge(kingStep, "e1d1");
    CHECK(!r0.accepted);
    CHECK_EQ(renderEn(r0.before[0].line), std::string("That's d1. We want e2: letter e, number two."));
    CHECK_EQ(renderEn(r0.before[0].line, true), std::string("That's dee one. We want ee two: letter ee, number two."));
    CHECK(judge(kingStep, "e1e2").accepted);
    // Chapter 1.1: rook moves and the king.
    int rook = find("7k/8/n7/8/8/8/8/R3K3 w");
    CHECK(hasKey(judge(rook, "a1a3").before, "lesson.rook.short"));
    CHECK(hasKey(judge(rook, "a1c1").before, "lesson.rook.rank"));
    CHECK(hasKey(judge(rook, "e1d2").before, "lesson.rook.king"));
    CHECK_EQ(renderEn(judge(rook, "a1a6").before[0].line), std::string("Captured! My knight leaves the board."));
    // Chapter 2: the defended knight: shown, rewound, taken back, then "Try the other piece".
    int safety = find("7k/8/4p3/3n4/b7/8/8/3QK3 w");
    LessonReaction bad = judge(safety, "d1d5");
    CHECK(hasKey(bad.before, "lesson.safety.watch"));
    CHECK(hasDemo(bad.before, "e6d5"));
    CHECK(hasKey(bad.before, "lesson.safety.gave"));
    CHECK(hasKey(bad.after, "lesson.safety.other"));
    CHECK_EQ(bad.undo, 1);
    // Chapter 3.3: blocking with the queen is accepted from the second try on.
    int block = find("4r2k/8/8/8/8/8/3P1P2/3QKB2 w");
    CHECK(!judge(block, "d1e2", 0).accepted);
    CHECK(hasKey(judge(block, "d1e2", 0).before, "lesson.block.queen"));
    LessonReaction later = judge(block, "d1e2", 1);
    CHECK(later.accepted);
    CHECK(hasKey(later.before, "lesson.block.queen_ok"));
    CHECK(hasKey(judge(block, "f1e2").before, "lesson.block.ok"));
    // Chapter 3.4: the praise names the capturing piece.
    int attacker = find("7k/8/8/8/8/2N5/4q3/4K3 w");
    CHECK(hasKey(judge(attacker, "e1e2").before, "lesson.capture.king_ok"));
    CHECK(hasKey(judge(attacker, "c3e2").before, "lesson.capture.knight_ok"));
    // Chapter 4.2: a check that is not mate: the king's escape is said and shown.
    int team = find("4k3/7Q/4K3/8/8/8/8/8 w");
    LessonReaction esc = judge(team, "h7d7");
    CHECK(hasKey(esc.before, "lesson.check_escape"));
    CHECK(hasDemo(esc.before, "e8f8"));
    CHECK_EQ(renderEn(esc.before[0].line), std::string("Check, but my king escapes to f8."));
    CHECK(judge(team, "h7e7").accepted);
    CHECK(hasKey(judge(team, "h7h8").before, "lesson.mate2.ok_rank"));
    // Chapter 4.3: Ra8+ and Re7+ have their own answers.
    int ladder = find("4k3/R7/8/8/8/8/8/1R4K1 w");
    CHECK(hasKey(judge(ladder, "a7a8").before, "lesson.mate3.free"));
    CHECK(hasDemo(judge(ladder, "a7e7").before, "e8e7"));
    LessonReaction re1 = judge(ladder, "b1e1");
    CHECK(hasKey(re1.before, "lesson.check_escape"));
    CHECK(judge(ladder, "b1b8").accepted);
    // Chapter 5.1: castling mistakes.
    int castle = find("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w");
    CHECK(hasKey(judge(castle, "h1f1").before, "lesson.castle.rook"));
    CHECK(hasKey(judge(castle, "e1c1").before, "lesson.castle.long"));
    CHECK(hasKey(judge(castle, "e1f1").before, "lesson.castle.one"));
    CHECK(judge(castle, "e1g1").accepted);
    // Chapter 6: en passant, and the chance gone.
    int ep = find("4k3/8/8/3pP3/8/8/8/4K3 w");
    CHECK(ep >= 0);
    CHECK(judge(ep, "e5d6").accepted);
    CHECK(hasKey(judge(ep, "e5e6").before, "lesson.ep.gone"));
    // Chapter 7: an underpromotion is answered first, accepted from the second try on.
    int promo = find("8/4P2k/8/8/8/8/8/4K3 w");
    CHECK(judge(promo, "e7e8q").accepted);
    CHECK(!judge(promo, "e7e8n").accepted);
    CHECK(hasKey(judge(promo, "e7e8n").before, "lesson.promo.under"));
    CHECK(judge(promo, "e7e8n", 1).accepted);
    // Chapter 8.2: stalemate is not mate.
    int stale = find("7k/8/6K1/8/8/8/5Q2/8 w");
    CHECK(hasKey(judge(stale, "f2f7").before, "lesson.stalemate"));
    CHECK(hasKey(judge(stale, "f2a2").before, "lesson.stalemate"));
    CHECK(judge(stale, "f2f8").accepted);
    // Escalation on the back-rank mate: hint with pointing, then the solution shown.
    int back = find("6k1/5ppp/8/8/8/8/8/R5K1 w");
    LessonReaction f1 = judge(back, "g1f1", 1);
    CHECK(!f1.after.empty() && !f1.after.back().gestures.empty());
    LessonReaction f2 = judge(back, "g1f1", 2);
    CHECK(hasDemo(f2.after, "a1a8"));
    CHECK(hasKey(f2.after, "lesson.now_you"));
    CHECK(hasKey(judge(back, "g1f1", 3).before, "lesson.almost"));
}

// Why a move is illegal, on the classic cases.
TEST(coach_lesson_why_illegal) {
    auto why = [](const char* fen, const char* from, const char* to, chess::PieceType promo = chess::NoPiece) {
        Position p;
        bool ok = p.setFEN(fen);
        if (!ok) std::fprintf(stderr, "  bad FEN %s\n", fen);
        CHECK(ok);
        return whyIllegal(p, parseSquare(from), parseSquare(to), promo);
    };
    const char* start = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    // Legal moves have no reason.
    CHECK(why(start, "e2", "e4").reason == IllegalReason::None);
    CHECK(why(start, "g1", "f3").reason == IllegalReason::None);
    // Pinned piece: the knight on e2 shields its king from the rook on e7.
    IllegalInfo pin = why("4k3/4r3/8/8/8/8/4N3/4K3 w - - 0 1", "e2", "c3");
    CHECK(pin.reason == IllegalReason::LeavesKingInCheck);
    CHECK_EQ(pin.culprit, parseSquare("e7"));
    // King into check.
    IllegalInfo into = why("7k/8/8/8/8/8/r7/4K3 w - - 0 1", "e1", "e2");
    CHECK(into.reason == IllegalReason::KingIntoCheck);
    CHECK_EQ(into.culprit, parseSquare("a2"));
    CHECK_EQ(into.square, parseSquare("e2"));
    // The king takes a defended piece.
    IllegalInfo defended = why("7k/8/8/8/8/2n5/4p3/4K3 w - - 0 1", "e1", "e2");
    CHECK(defended.reason == IllegalReason::KingIntoCheck);
    CHECK_EQ(defended.culprit, parseSquare("c3"));
    // Castling through check, into check, out of check, blocked, without the right.
    IllegalInfo through = why("r3k2r/8/8/8/2b5/8/8/R3K2R w KQkq - 0 1", "e1", "g1");
    CHECK(through.reason == IllegalReason::CastlingThroughCheck);
    CHECK_EQ(through.square, parseSquare("f1"));
    CHECK_EQ(through.culprit, parseSquare("c4"));
    CHECK(why("r3k2r/8/8/8/2b5/8/8/R3K2R w KQkq - 0 1", "e1", "c1").reason == IllegalReason::None);
    IllegalInfo landing = why("r3k2r/8/8/8/8/8/6r1/R3K2R w KQkq - 0 1", "e1", "g1");
    CHECK(landing.reason == IllegalReason::KingIntoCheck);
    CHECK_EQ(landing.square, parseSquare("g1"));
    IllegalInfo inCheck = why("4r1k1/8/8/8/8/8/8/R3K2R w KQ - 0 1", "e1", "g1");
    CHECK(inCheck.reason == IllegalReason::CastlingOutOfCheck);
    CHECK_EQ(inCheck.culprit, parseSquare("e8"));
    IllegalInfo blocked = why(start, "e1", "g1");
    CHECK(blocked.reason == IllegalReason::CastlingBlocked);
    CHECK_EQ(blocked.culprit, parseSquare("f1"));
    CHECK(why("r3k2r/8/8/8/8/8/8/R3K2R w kq - 0 1", "e1", "g1").reason == IllegalReason::CastlingNoRights);
    // En passant: only right after the double step.
    IllegalInfo late = why("4k3/8/8/3pP3/8/8/8/4K3 w - - 0 1", "e5", "d6");
    CHECK(late.reason == IllegalReason::EnPassantExpired);
    CHECK_EQ(late.culprit, parseSquare("d5"));
    CHECK(why("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", "e5", "d6").reason == IllegalReason::None);
    // En passant that would open the king's rank (both pawns leave it).
    IllegalInfo epPin = why("8/8/8/K2pP2r/8/8/8/7k w - d6 0 1", "e5", "d6");
    CHECK(epPin.reason == IllegalReason::LeavesKingInCheck);
    CHECK_EQ(epPin.culprit, parseSquare("h5"));
    // Check not answered.
    IllegalInfo check = why("4r2k/8/8/8/8/8/P7/4K3 w - - 0 1", "a2", "a3");
    CHECK(check.reason == IllegalReason::MustAnswerCheck);
    CHECK_EQ(check.culprit, parseSquare("e8"));
    // Geometry, blockers, pawns.
    IllegalInfo path = why(start, "a1", "a3");
    CHECK(path.reason == IllegalReason::OwnPieceOnTarget || path.reason == IllegalReason::PathBlocked);
    CHECK(why("4k3/8/8/8/8/8/P7/R3K3 w - - 0 1", "a1", "a4").reason == IllegalReason::PathBlocked);
    CHECK_EQ(why("4k3/8/8/8/8/8/P7/R3K3 w - - 0 1", "a1", "a4").culprit, parseSquare("a2"));
    CHECK(why(start, "b1", "b3").reason == IllegalReason::WrongGeometry);
    CHECK(why(start, "a1", "a2").reason == IllegalReason::OwnPieceOnTarget);
    CHECK(why(start, "e2", "d3").reason == IllegalReason::PawnCaptureNeedsVictim);
    CHECK(why(start, "e2", "e5").reason == IllegalReason::WrongGeometry);
    IllegalInfo front = why("7k/8/8/4p3/4P3/8/8/4K3 w - - 0 1", "e4", "e5");
    CHECK(front.reason == IllegalReason::PawnForwardBlocked);
    CHECK_EQ(front.culprit, parseSquare("e5"));
    CHECK(why("7k/8/8/8/8/4n3/4P3/4K3 w - - 0 1", "e2", "e4").reason == IllegalReason::PawnForwardBlocked);
    CHECK(why(start, "e7", "e5").reason == IllegalReason::NotYourTurn);
    CHECK(why(start, "e4", "e5").reason == IllegalReason::NoPiece);
    CHECK(why("8/4P2k/8/8/8/8/8/4K3 w - - 0 1", "e7", "e8").reason == IllegalReason::NeedsPromotionPiece);
    CHECK(why("8/4P2k/8/8/8/8/8/4K3 w - - 0 1", "e7", "e8", chess::Queen).reason == IllegalReason::None);
    CHECK_EQ(std::string(illegalReasonName(IllegalReason::KingIntoCheck)), std::string("KingIntoCheck"));
}

// The lesson explains refused moves with the right line, pointing at the culprit.
TEST(coach_lesson_explains_illegal_moves) {
    Lesson lesson;
    auto find = [&](const std::string& fenStart) {
        for (size_t i = 0; i < lesson.expectations().size(); ++i)
            if (lesson.expectation(int(i)).fen.compare(0, fenStart.size(), fenStart) == 0) return int(i);
        return -1;
    };
    auto explain = [&](int e, const char* from, const char* to) {
        Position p;
        p.setFEN(lesson.expectation(e).fen);
        return lesson.explainIllegal(e, p, parseSquare(from), parseSquare(to));
    };
    int danger = find("7k/8/8/8/8/8/r7/4K3 w");
    Script s = explain(danger, "e1", "e2");
    CHECK_EQ(s.size(), size_t(1));
    CHECK_EQ(s[0].line.key, std::string("why.KingIntoCheck"));
    CHECK(s[0].priority == Priority::Urgent);
    CHECK(s[0].look == Look::Target);
    CHECK(!s[0].gestures.empty() && s[0].gestures[0].square == parseSquare("a2"));
    CHECK_EQ(s[0].gestures[0].anchor, std::string("my"));
    CHECK_EQ(renderEn(s[0].line), std::string("Not e2: my rook attacks it. A king never steps into danger."));
    // Its own line where the exercise has one.
    int checkFile = find("4r2k/8/8/8/8/8/8/4K3 w");
    Script f = explain(checkFile, "e1", "e2");
    CHECK_EQ(f[0].line.key, std::string("lesson.check.file"));
    CHECK_EQ(renderEn(f[0].line), std::string("e2 is still on my rook's file. Step off the line."));
    int castle2 = find("r3k2r/8/8/8/2b5/8/8/R3K2R w");
    Script c = explain(castle2, "e1", "g1");
    CHECK_EQ(renderEn(c[0].line), std::string("Your king would cross f1, and my bishop attacks it."));
    CHECK_EQ(renderEn(c[0].line, true), std::string("Your king would cross eff one, and my bishop attacks it."));
    Script pinned = lesson.explainIllegal(-1, [] { Position p; p.setFEN("4k3/4r3/8/8/8/8/4N3/4K3 w - - 0 1"); return p; }(),
                                          parseSquare("e2"), parseSquare("c3"));
    CHECK_EQ(renderEn(pinned[0].line), std::string("Your knight is pinned: moving it would expose your king to my rook."));
    // Every explanation's line exists and has its arguments, from any legal-looking attempt of
    // every exercise.
    for (size_t i = 0; i < lesson.expectations().size(); ++i) {
        Position p;
        p.setFEN(lesson.expectation(int(i)).fen);
        for (int from = 0; from < 64; ++from) {
            if (p.at(chess::Square(from)).empty() || p.at(chess::Square(from)).color != chess::White) continue;
            for (int to = 0; to < 64; ++to) {
                if (from == to || p.findLegal(chess::Square(from), chess::Square(to), chess::Queen).valid()) continue;
                Script x = lesson.explainIllegal(int(i), p, chess::Square(from), chess::Square(to));
                for (const Beat& b : x) checkLine(b.line, "illegal", {"to"}, b.gestures, b.marks);
            }
        }
    }
    // Nothing to say about a missing promotion piece.
    int promo = find("8/4P2k/8/8/8/8/8/4K3 w");
    CHECK(explain(promo, "e7", "e8").empty());
}

// The event helpers (src/coach/events.*): every key exists, lines render, the gaze rule holds.
TEST(coach_events_scripts) {
    Lesson lesson;
    for (const std::string& k : eventKeys()) {
        CHECK(catalog().has(k));
        if (!catalog().has(k)) std::fprintf(stderr, "  missing %s\n", k.c_str());
    }
    // Every events.lang key is one a helper says.
    std::vector<std::string> keys = eventKeys();
    for (const std::string& k : catalog().keys("en", "events")) {
        std::string base = k;
        if (base.size() > 7 && base.compare(base.size() - 7, 7, ".spoken") == 0) base = base.substr(0, base.size() - 7);
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos && dot + 1 < base.size() && std::isdigit((unsigned char)base[dot + 1]))
            base = base.substr(0, dot);
        bool known = std::find(keys.begin(), keys.end(), base) != keys.end();
        if (!known) std::fprintf(stderr, "  unused %s\n", k.c_str());
        CHECK(known);
    }
    std::vector<Script> all = {greetingScript(1, chess::White, true), greetingScript(6, chess::Black, false),
                               levelIntroScript(6), yourMoveScript(12), takeYourTimeScript(12), fillerScript(12),
                               takebackScript(true), takebackScript(false), playOnScript(), drawAnswerScript(true),
                               drawAnswerScript(false), gameEndScript(GameEnd::Win), gameEndScript(GameEnd::Resigned),
                               encouragementScript(Encouragement::AfterMistake), encouragementScript(Encouragement::Behind),
                               encouragementScript(Encouragement::PlayingWell), lessonResumeScript(lesson, 3),
                               lessonNextScript(lesson, 4)};
    for (const Script& s : all) {
        CHECK(!s.empty());
        for (const Beat& b : s) {
            checkLine(b.line, "events", {}, b.gestures, b.marks);
            CHECK(b.look == (b.line.key == "event.filler" ? Look::Board : Look::Player));
        }
    }
    CHECK_EQ(greetingScript(1, chess::White, true).size(), size_t(3));
    CHECK_EQ(greetingScript(3, chess::Black, false).size(), size_t(2));
    CHECK_EQ(greetingScript(3, chess::Black, false)[1].line.key, std::string("event.colour.black"));
    CHECK(takeYourTimeScript(5)[0].priority == Priority::Low);
    CHECK_EQ(takeYourTimeScript(5)[0].ply, 5);
    CHECK_EQ(gameEndScript(GameEnd::Draw).back().line.key, std::string("event.end.handshake"));
    CHECK_EQ(renderEn(lessonResumeScript(lesson, 3)[0].line),
             "Welcome back! Let's continue with " + renderEn(lesson.chapters()[3].title) + ".");
    CHECK(lessonResumeScript(lesson, 99).empty());
}
