// Tests for the coach's review (src/coach/review.h): win percentage and move classes, the
// explanation chosen for hand-built positions and analyses, demonstrations (depth per level, never
// past the player's promotion, rewind of exactly the moves shown), the English lines (every key the
// scripts use exists, every placeholder has an argument, every gesture and mark anchor is a
// placeholder of every variant, at most three pointing gestures, the sentence budget per level),
// praise only for the best moves and its rate limit, takebacks (and their count at levels 4-6),
// announcements, threat warnings, analysis requests; and, with the embedded engine, the worked
// example (the fork of 4...Qg5).
#include "test.h"

#include "ai/analysis.h"
#include "ai/engine.h"
#include "chess/chess.h"
#include "coach/appraisal.h"
#include "coach/review.h"
#include "coach/review_internal.h"
#include "coach/tactics.h"
#include "core/embedded.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace chess;
using namespace coach;

namespace {

Square sq(const char* s) { return parseSquare(s); }

// ---- The English lines -------------------------------------------------------------------------

struct Lines {
    std::map<std::string, std::vector<std::string>> variants;   // message key -> its phrasings
};

// "ex.fork.b1.2" -> "ex.fork.b1"; "praise.sacrifice.2" -> "praise.sacrifice".
std::string messageKey(const std::string& key) {
    const size_t dot = key.rfind('.');
    if (dot == std::string::npos || dot + 1 >= key.size()) return key;
    for (size_t i = dot + 1; i < key.size(); ++i)
        if (key[i] < '0' || key[i] > '9') return key;
    return std::atoi(key.c_str() + dot + 1) >= 2 ? key.substr(0, dot) : key;
}

const Lines& english() {
    static const Lines lines = [] {
        Lines l;
        for (const char* file : {"assets/coach/speech/en/review.lang", "assets/coach/speech/en/appraisal.lang"}) {
            std::vector<std::pair<std::string, std::string>> entries;
            std::string err;
            const bool ok = i18n::parse(embedded::text(file), entries, &err);
            if (!ok) std::fprintf(stderr, "  %s: %s\n", file, err.c_str());
            CHECK(ok);
            for (const auto& e : entries) l.variants[messageKey(e.first)].push_back(e.second);
        }
        return l;
    }();
    return lines;
}

std::set<std::string> placeholders(const std::string& text) {
    std::set<std::string> out;
    size_t p = 0;
    while ((p = text.find('{', p)) != std::string::npos) {
        const size_t q = text.find('}', p);
        if (q == std::string::npos) break;
        std::string name = text.substr(p + 1, q - p - 1);
        const size_t colon = name.find(':');
        if (colon != std::string::npos) name = name.substr(0, colon);
        out.insert(name);
        p = q + 1;
    }
    return out;
}

bool isPointing(const Gesture& g) {
    return g.kind == GestureKind::PointSquare || g.kind == GestureKind::PointPiece || g.kind == GestureKind::Trace;
}

int pointing(const Beat& b) {
    int n = 0;
    for (const Gesture& g : b.gestures) n += isPointing(g) ? 1 : 0;
    return n;
}

// Keys used by a script, for the tests below.
std::vector<std::string> keysOf(const Script& s) {
    std::vector<std::string> k;
    for (const Beat& b : s)
        if (!b.line.key.empty()) k.push_back(b.line.key);
    return k;
}

bool hasKey(const Script& s, const std::string& key) {
    for (const Beat& b : s)
        if (b.line.key == key) return true;
    return false;
}

const Beat* beatWithKey(const Script& s, const std::string& key) {
    for (const Beat& b : s)
        if (b.line.key == key) return &b;
    return nullptr;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// Say lines counted against the level's sentence budget: demonstration narration (and the
// promotion stop), the rewind, the offer and the retry reactions are not.
// The Say lines a band's sentence budget counts: not the demonstration's own lines (from its first
// move to the rewind: the narration, what the table now shows, what falls next).
int sentences(const Script& s) {
    int n = 0;
    bool inDemo = false;
    for (const Beat& b : s) {
        if (b.kind == BeatKind::DemoMove) inDemo = true;
        if (b.kind == BeatKind::Rewind) inDemo = false;
        if (b.kind == BeatKind::Say && !inDemo && !b.line.key.empty() && !startsWith(b.line.key, "demo.") &&
            !startsWith(b.line.key, "tb."))
            ++n;
    }
    return n;
}

int demoMoves(const Script& s) {
    int n = 0;
    for (const Beat& b : s) n += b.kind == BeatKind::DemoMove ? 1 : 0;
    return n;
}

// Every rule a script must follow whatever the scenario.
void checkScript(const Script& s, const char* where) {
    const Lines& en = english();
    int pendingDemo = 0;
    for (const Beat& b : s) {
        for (const Gesture& g : b.gestures) {
            if (isPointing(g)) CHECK(g.square != NoSquare || g.path.size() >= 2);
            if (g.kind == GestureKind::Trace) CHECK(g.path.size() >= 2 && g.path.size() <= 3);
        }
        if (pointing(b) > 3) {
            std::fprintf(stderr, "  %s: %s has %d pointing gestures\n", where, b.line.key.c_str(), pointing(b));
            CHECK(false);
        }
        if (b.kind == BeatKind::DemoMove) {
            ++pendingDemo;
            CHECK(!b.uci.empty());
        }
        if (b.kind == BeatKind::Rewind) {
            if (b.count != pendingDemo)
                std::fprintf(stderr, "  %s: rewind of %d after %d demo moves\n", where, b.count, pendingDemo);
            CHECK_EQ(b.count, pendingDemo);
            CHECK(!b.skippable);
            pendingDemo = 0;
        }
        if (b.kind == BeatKind::OfferTakeback) CHECK(b.look == Look::Player);
        if (b.line.key.empty()) {
            for (const Gesture& g : b.gestures) CHECK(g.anchor.empty());
            for (const Mark& m : b.marks) CHECK(m.anchor.empty());
            continue;
        }
        auto it = en.variants.find(b.line.key);
        if (it == en.variants.end()) {
            std::fprintf(stderr, "  %s: key %s missing from the English lines\n", where, b.line.key.c_str());
            CHECK(false);
            continue;
        }
        for (const std::string& text : it->second) {
            const std::set<std::string> ph = placeholders(text);
            for (const std::string& p : ph)
                if (!b.line.arg(p)) {
                    std::fprintf(stderr, "  %s: %s \"%s\": no argument for {%s}\n", where, b.line.key.c_str(),
                                 text.c_str(), p.c_str());
                    CHECK(false);
                }
            // "you've lost {pts:point}", "{reply} wins {pts:point}": a loss of material, never 0 or less.
            if (ph.count("pts") && b.line.arg("pts") && b.line.arg("pts")->number < 1) {
                std::fprintf(stderr, "  %s: %s \"%s\": {pts} is %d\n", where, b.line.key.c_str(), text.c_str(),
                             b.line.arg("pts")->number);
                CHECK(false);
            }
            for (const Gesture& g : b.gestures)
                if (!g.anchor.empty() && !ph.count(g.anchor)) {
                    std::fprintf(stderr, "  %s: %s \"%s\": gesture anchor {%s} not in the line\n", where,
                                 b.line.key.c_str(), text.c_str(), g.anchor.c_str());
                    CHECK(false);
                }
            for (const Mark& m : b.marks)
                if (!m.anchor.empty() && !ph.count(m.anchor)) {
                    std::fprintf(stderr, "  %s: %s \"%s\": mark anchor {%s} not in the line\n", where,
                                 b.line.key.c_str(), text.c_str(), m.anchor.c_str());
                    CHECK(false);
                }
        }
    }
    CHECK_EQ(pendingDemo, 0);   // every demonstration is taken back
    // "Let's play on": last, after a demonstration taken back, never with a takeback card.
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i].line.key != "demo.play_on") continue;
        bool rewound = false, offered = false;
        for (const Beat& b : s) {
            rewound = rewound || b.kind == BeatKind::Rewind;
            offered = offered || b.kind == BeatKind::OfferTakeback;
        }
        if (i + 1 != s.size() || !rewound || offered) std::fprintf(stderr, "  %s: misplaced demo.play_on\n", where);
        CHECK(i + 1 == s.size() && rewound && !offered);
    }
}

void dump(const Script& s) {
    for (const Beat& b : s) {
        std::fprintf(stderr, "    kind %d key %s uci %s count %d gestures %zu marks %zu\n", int(b.kind),
                     b.line.key.c_str(), b.uci.c_str(), b.count, b.gestures.size(), b.marks.size());
    }
}

// ---- Games and analyses ------------------------------------------------------------------------

Game gameOf(const char* fen, std::initializer_list<const char*> sans) {
    Game g;
    if (fen) CHECK(g.resetFromFEN(fen));
    for (const char* s : sans) {
        const Move m = g.position().parseSAN(s);
        if (!m.valid()) std::fprintf(stderr, "  bad SAN in test: %s\n", s);
        CHECK(m.valid());
        if (!m.valid()) break;
        g.play(m);
    }
    return g;
}

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

Review reviewOf(Reviewer& rv, const Game& g, const ai::Analysis& a0, bool inBook = false) {
    ReviewInput in;
    in.game = &g;
    in.before = &a0;
    in.inBook = inBook;
    return rv.review(in);
}

// The worked example: 1.e4 e5 2.Nf3 Nc6 3.Bc4 Nd4 4.Nxe5, refuted by 4...Qg5 (fork of e5 and g2).
Game forkGame() { return gameOf(nullptr, {"e4", "e5", "Nf3", "Nc6", "Bc4", "Nd4", "Nxe5"}); }
// The engine's lines (depth 16): 4...Qg5 forks the knight and g2; 5.Bxf7+ Kd8 6.O-O Qxe5 wins the
// knight for two pawns.
ai::Analysis forkA0() {
    return analysisOf({pvl(138, "f3d4 e5d4 e1g1 c7c6 f1e1 d7d6"), pvl(125, "e1g1 d4f3 d1f3 d8f6 f3g3 d7d6"),
                       pvl(-90, "f3e5 d8g5 c4f7 e8d8 e1g1 g5e5 c2c3 d4e6 d2d3 g7g5")});
}

// A knight left en prise: 1.Nd5?? exd5.
const char* kHangFen = "6k1/5ppp/4p3/8/8/2N5/5PPP/6K1 w - - 0 1";
ai::Analysis hangA0() { return analysisOf({pvl(20, "g1f1 g8f8"), pvl(-330, "c3d5 e6d5 g1f1")}); }

}  // namespace

// ---- Classification ------------------------------------------------------------------------------

TEST(coach_review_win_percent) {
    CHECK(std::fabs(winPercent(0) - 50.0) < 1e-9);
    CHECK(std::fabs(winPercent(100) - 59.1) < 0.1);   // lichess: +1 pawn ~ 59 %
    CHECK(std::fabs(winPercent(-100) - 40.9) < 0.1);
    CHECK(std::fabs(winPercent(1000) - winPercent(5000)) < 1e-9);   // clamped
    ai::Score mate;
    mate.mate = 3;
    CHECK(std::fabs(winPercent(mate) - winPercent(1000)) < 1e-9);
    mate.mate = -2;
    CHECK(std::fabs(winPercent(mate) - winPercent(-1000)) < 1e-9);
    CHECK(std::fabs(moveAccuracy(60.0, 60.0) - 100.0) < 1e-9);
    CHECK(std::fabs(moveAccuracy(60.0, 70.0) - 100.0) < 1e-9);
    const double a10 = moveAccuracy(60.0, 50.0), a30 = moveAccuracy(60.0, 30.0);
    CHECK(a10 < 100.0 && a30 < a10 && a30 > 0.0);
    CHECK(std::fabs(a10 - (103.1668100711649 * std::exp(-0.4354415386753951) - 3.166924740191411 + 1.0)) < 1e-9);
}

TEST(coach_review_classification_thresholds) {
    CHECK_EQ(classifyDelta(0.0, false), MoveClass::Best);
    CHECK_EQ(classifyDelta(0.49, false), MoveClass::Best);
    CHECK_EQ(classifyDelta(0.5, false), MoveClass::Excellent);
    CHECK_EQ(classifyDelta(30.0, true), MoveClass::Best);   // the engine's move
    CHECK_EQ(classifyDelta(1.99, false), MoveClass::Excellent);
    CHECK_EQ(classifyDelta(2.0, false), MoveClass::Good);
    CHECK_EQ(classifyDelta(4.99, false), MoveClass::Good);
    CHECK_EQ(classifyDelta(5.0, false), MoveClass::Inaccuracy);
    CHECK_EQ(classifyDelta(9.99, false), MoveClass::Inaccuracy);
    CHECK_EQ(classifyDelta(10.0, false), MoveClass::Mistake);
    CHECK_EQ(classifyDelta(14.99, false), MoveClass::Mistake);
    CHECK_EQ(classifyDelta(15.0, false), MoveClass::Blunder);

    auto cp = [](int v) { ai::Score s; s.cp = v; return s; };
    auto mate = [](int n) { ai::Score s; s.mate = n; return s; };
    Judgement j = judge(cp(0), cp(-100), false);   // 50 -> 40.9: an inaccuracy
    CHECK(std::fabs(j.delta - (50.0 - winPercent(-100))) < 1e-9);
    CHECK_EQ(j.cls, MoveClass::Inaccuracy);
    CHECK_EQ(judge(cp(0), cp(-300), false).cls, MoveClass::Blunder);
    CHECK_EQ(judge(cp(50), cp(20), false).cls, MoveClass::Good);
    CHECK_EQ(judge(cp(300), cp(500), false).delta, 0.0);   // better than the "best": never negative

    // Mates (lichess Advice.scala).
    j = judge(mate(3), cp(300), false);
    CHECK_EQ(j.mate, MateChange::Lost);
    CHECK_EQ(j.cls, MoveClass::Blunder);
    CHECK_EQ(judge(mate(3), cp(800), false).cls, MoveClass::Mistake);
    CHECK_EQ(judge(mate(3), cp(1200), false).cls, MoveClass::Inaccuracy);
    CHECK_EQ(judge(mate(3), mate(-2), false).cls, MoveClass::Blunder);
    j = judge(cp(0), mate(-2), false);
    CHECK_EQ(j.mate, MateChange::Created);
    CHECK_EQ(j.cls, MoveClass::Blunder);
    CHECK_EQ(judge(cp(-800), mate(-2), false).cls, MoveClass::Mistake);
    CHECK_EQ(judge(cp(-1200), mate(-2), false).cls, MoveClass::Inaccuracy);
    j = judge(mate(2), mate(5), false);
    CHECK_EQ(j.mate, MateChange::Delayed);
    CHECK_EQ(j.cls, MoveClass::Best);   // both win: not judged by lichess, Δ = 0
    CHECK_EQ(judge(mate(-2), mate(-1), false).mate, MateChange::None);
}

TEST(coach_review_bands) {
    // Levels 1-3 play the whole sequence on the table (up to 5, 7, 7 plies); levels 4-6 play up to
    // the problem (4 plies at most) and point at what comes next. The lines said in words grow with
    // the level.
    const bool full[6] = {true, true, true, false, false, false};
    const int demo[6] = {5, 7, 7, 4, 4, 4};
    const int words[6] = {1, 3, 3, 4, 6, 8};
    for (int l = 1; l <= 6; ++l) {
        CHECK_EQ(band(l).level, l);
        CHECK_EQ(band(l).fullDemo, full[l - 1]);
        CHECK_EQ(band(l).demoPlies, demo[l - 1]);
        CHECK_EQ(band(l).linePlies, words[l - 1]);
        CHECK(band(l).mateLinePlies >= 3);   // a mate in two is always shown in full
        CHECK(band(l).sentences >= 2 && band(l).sentences <= 4);
    }
    CHECK_EQ(band(0).level, 1);
    CHECK_EQ(band(9).level, 6);
}

// ---- Explanations --------------------------------------------------------------------------------

TEST(coach_review_play_on_after_a_demonstration_without_offer) {
    // 1.Nd5?? exd5 while still clearly ahead (W% 92 -> 76): a blunder, shown at level 3 with no
    // takeback card (none from 75 on). The coach says that its real move comes next.
    const char* fen = "6k1/5ppp/4p3/8/8/2N5/5PPP/R5K1 w - - 0 1";
    Game g = gameOf(fen, {"Nd5"});
    Reviewer rv;
    rv.reset(3, White);
    const Review r = reviewOf(rv, g, analysisOf({pvl(650, "g1f1 g8f8"), pvl(320, "c3d5 e6d5 g1f1")}));
    checkScript(r.script, "play on");
    CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
    CHECK(!r.offersTakeback);
    CHECK(demoMoves(r.script) > 0);
    CHECK(!r.script.empty() && r.script.back().line.key == "demo.play_on");
    // With the card (a blunder that loses the game's balance), the card ends the script.
    Game h = gameOf(kHangFen, {"Nd5"});
    Reviewer rh;
    rh.reset(3, White);
    const Review o = reviewOf(rh, h, hangA0());
    CHECK(o.offersTakeback);
    CHECK(!hasKey(o.script, "demo.play_on"));
}

TEST(coach_review_mistake_explained_without_demonstration) {
    // 1.Nd5? exd5 when already losing: a mistake by the numbers, not a blunder. Said in words (the
    // verdict at levels 3-6, then why), without a demonstration, a card or "Now I play my move": the
    // coach's next move is its real one.
    for (int level = 2; level <= 6; ++level) {
        Game g = gameOf(kHangFen, {"Nd5"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, analysisOf({pvl(-400, "g1f1 g8f8"), pvl(-730, "c3d5 e6d5 g1f1")}));
        char where[32];
        std::snprintf(where, sizeof where, "mistake b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.cls, MoveClass::Mistake);
        CHECK_EQ(r.verdict.exType, ExType::Hanging);
        CHECK(r.verdict.voiced);
        CHECK(!r.offersTakeback);
        CHECK_EQ(demoMoves(r.script), 0);
        CHECK(!hasKey(r.script, "demo.play_on"));
        CHECK_EQ(hasKey(r.script, "ex.verdict.mistake.b" + std::to_string(level)), level >= 3);
        const Beat* cause = nullptr;
        for (const Beat& b : r.script)
            if (startsWith(b.line.key, "ex.hanging")) cause = &b;
        CHECK(cause != nullptr);
        if (cause) {
            bool pointed = false;
            for (const Gesture& gs : cause->gestures)
                if (gs.kind == GestureKind::PointPiece && gs.square == sq("d5")) pointed = true;
            CHECK(pointed);
            for (const Mark& m : cause->marks) CHECK(!m.untilRewind);   // no rewind to wait for
        }
    }
}

TEST(coach_review_bishop_for_a_pawn_is_explained) {
    // From a recorded game (depth 18): 8...Bxh3?? 9.Bxh3 gives the bishop for a pawn, and the engine
    // counts 7.5 pawns with the pin it leaves on c8. The two points are the reason at every level.
    const ai::Analysis a0 = analysisOf({pvl(-19, "g4d7 b3d1 a5a4 f1g2 e8g8"),
                                        pvl(-773, "g4h3 f1h3 b8d7 a1d1 c8d8 e3c1 e8g8 a2a4 c7c6 e2e3")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf("rnq1k2r/1pp1ppbp/3p1np1/p7/2PP2b1/1QN1BNPP/PP2PP2/R3KB1R b KQkq - 0 8", {"Bxh3"});
        Reviewer rv;
        rv.reset(level, Black);
        const Review r = reviewOf(rv, g, a0);
        char where[32];
        std::snprintf(where, sizeof where, "bishop for a pawn b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.exType, ExType::Exchange);
        CHECK(r.offersTakeback);
    }
}

TEST(coach_review_a_trade_is_not_the_piece_lost) {
    // From a recorded game (depth 18): 24...Re6?? 25.Bxf8 hxg4 26.hxg4 Bxg4 27.f3 Rxe2+ 28.Kxe2 loses
    // the bishop on f8 for two pawns; the rooks are only traded. No line says the rook is what falls:
    // a material reason names the bishop and two points.
    const ai::Analysis a0 = analysisOf({pvl(413, "e7e2 c3e2 f5e4 f2f3 e4f3 e1g1"),
                                        pvl(-249, "e7e6 a3f8 h5g4 h3g4 f5g4 f2f3 e6e2 e1e2 g4e6 f8d6")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf("r3kb2/p3r3/1pp3p1/5b1p/P5P1/B1N4P/4RP2/4K2R b Kq - 0 24", {"Re6"});
        Reviewer rv;
        rv.reset(level, Black);
        const Review r = reviewOf(rv, g, a0);
        char where[32];
        std::snprintf(where, sizeof where, "trade b%d", level);
        checkScript(r.script, where);
        for (const Beat& b : r.script) {
            if (const Arg* t = b.line.arg("t1")) CHECK(t->piece != Rook);
            if (b.line.key.rfind("ex.material.", 0) != 0) continue;
            CHECK(b.line.arg("t1") && b.line.arg("t1")->piece == Bishop && b.line.arg("t1")->square == sq("f8"));
            if (const Arg* pts = b.line.arg("pts")) CHECK_EQ(pts->number, 2);
        }
    }
}

TEST(coach_review_a_loss_the_better_move_suffers_too_is_no_reason) {
    // From a mined game (depth 18): 14.Nf6+ Kg7? leaves the bishop on f3 to Bxf3, but 14...exf6 gives it
    // up too (and wins the knight): the bishop is no reason Kg7 is bad.
    const ai::Analysis a0 = analysisOf({pvl(-79, "e7f6 g2f3 a8c8 f1d1 a5c6 f3c6 c8c6 d1d4 f8e8 c1d2"),
                                        pvl(-549, "g8g7 g2f3 a8c8 f6e4 d6d5 e4d2 d5c4 b3b4 a5c6 c1c4")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf("r2q1rk1/p3pp1p/1p1p1Np1/n7/2Pp4/1P3bP1/P3PPBP/R1Q2RK1 b - - 0 14", {"Kg7"});
        Reviewer rv;
        rv.reset(level, Black);
        const Review r = reviewOf(rv, g, a0);
        char where[32];
        std::snprintf(where, sizeof where, "given up anyway b%d", level);
        checkScript(r.script, where);
        CHECK(r.verdict.exType != ExType::Hanging);
        CHECK(r.verdict.exType != ExType::Material);
        for (const Beat& b : r.script) CHECK(b.kind != BeatKind::DemoMove);
    }
}

TEST(coach_review_a_gambit_pawn_is_no_material_lost) {
    // From a mined game: 3.f4 exf4 4.Nf3. White could take back (exf4) and nothing captures at once; the
    // engine prefers the gambit. "f4 loses material: after exf4, you've lost 1 point" would be wrong.
    const ai::Analysis a0 = analysisOf({pvl(10, "b1c3 g8f6 g1f3 f8b4 d1c2 d7d6 d2d3 e8g8 c1d2 f8e8"),
                                        pvl(-127, "f2f4 e5f4 g1f3 g8f6 d2d4 f4e3 f1d3 d7d5 e1g1 f8e7")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf("r1bqkbnr/pppp1ppp/2n5/4p3/2P5/4P3/PP1P1PPP/RNBQKBNR w KQkq - 0 3", {"f4"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        char where[32];
        std::snprintf(where, sizeof where, "gambit b%d", level);
        checkScript(r.script, where);
        CHECK(r.verdict.exType != ExType::Material);
        for (const Beat& b : r.script) CHECK(b.line.key.rfind("ex.material.", 0) != 0);
    }
}

TEST(coach_review_endgame_king_is_not_exposed_by_pawns_around_it) {
    // From a mined game: 49.Re7? lets the d-pawn run (Rd6, d2, d1=Q). Black's pawns and rook around the
    // white king on c3 are no attack on it: never "your king is exposed".
    const ai::Analysis a0 = analysisOf({pvl(549, "d1f3 d8c8 f7b7 g8f8 b7b5 f8e7 f3d5 e7f6 b5b7 f6f5"),
                                        pvl(54, "f7e7 d8d6 g3g4 h5g4 d1g4 d3d2 e7b7 d2d1q g4d1 d6e6")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf("3r2k1/5R2/4P1p1/1p5p/2p4P/2Kp2P1/8/3B4 w - - 3 49", {"Re7"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        char where[32];
        std::snprintf(where, sizeof where, "endgame king b%d", level);
        checkScript(r.script, where);
        CHECK(r.verdict.exType != ExType::KingSafety);
    }
}

TEST(coach_review_level5_line_is_more_than_the_move) {
    // 10...Qd6? (a quiet move: the same trades as the better 10...Qd7, nothing lost). A level-5
    // sentence that names a line ("Qd7 was stronger: Qd7 bxc3 Nxc6") never names the better move
    // alone as its line ("Qd7 was stronger: Qd7.").
    const ai::Analysis a0 = analysisOf({pvl(-149, "d8d7 b2c3 b8c6 d1f3 a8c8 e1g1 f8e7 d4c5 e7c5 f1d1"),
                                        pvl(-330, "d8d6 b2c3 b8c6 d1f3 a8c8 c1f4 d6d5 f3d5 e6d5")});
    Game g = gameOf("rn1qkb1r/p4ppp/1pN1p3/1Bp5/3P4/2n5/PPP2PPP/R1BQK2R b KQkq - 0 10", {"Qd6"});
    Reviewer rv;
    rv.reset(5, Black);
    const Review r = reviewOf(rv, g, a0);
    checkScript(r.script, "line b5");
    bool positional = false;
    for (const Beat& b : r.script) {
        if (b.line.key.rfind("ex.positional.", 0) != 0) continue;
        positional = true;
        const Arg* l = b.line.arg("line");
        // The level-5 sentences name the line from the better move on; level 4's do not.
        if (b.line.key.rfind("ex.positional.b5", 0) == 0) CHECK(l && l->san.find(' ') != std::string::npos);
    }
    CHECK(positional);
}

TEST(coach_review_hanging_level1) {
    Game g = gameOf(kHangFen, {"Nd5"});
    Reviewer rv;
    rv.reset(1, White);
    const ai::Analysis a0 = hangA0();
    Review r = reviewOf(rv, g, a0);
    CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
    CHECK_EQ(r.verdict.exType, ExType::Hanging);
    CHECK(r.verdict.voiced);
    CHECK(r.offersTakeback);
    checkScript(r.script, "hanging b1");
    const std::vector<std::string> keys = keysOf(r.script);
    const std::vector<std::string> want = {"ex.verdict.blunder.b1", "ex.hanging.b1", "demo.my.take", "ex.rewind.b1",
                                           "ex.hanging.tip.b1", "ex.offer.b1"};
    CHECK(keys == want);
    if (keys != want) dump(r.script);
    // The cause points at the knight and traces the pawn's capture.
    const Beat* cause = beatWithKey(r.script, "ex.hanging.b1");
    CHECK(cause != nullptr);
    if (cause) {
        CHECK(cause->look == Look::Target);
        bool knight = false, trace = false;
        for (const Gesture& gs : cause->gestures) {
            if (gs.kind == GestureKind::PointPiece && gs.square == sq("d5") && gs.anchor == "your") knight = true;
            if (gs.kind == GestureKind::Trace && gs.path.front() == sq("e6") && gs.path.back() == sq("d5")) trace = true;
        }
        CHECK(knight && trace);
        for (const Mark& m : cause->marks) CHECK(m.untilRewind);   // lit through the demonstration
    }
    CHECK(r.script.front().look == Look::Player);    // verdict: at the player
    CHECK(r.script.back().kind == BeatKind::OfferTakeback);   // the offer ends the script
    CHECK_EQ(demoMoves(r.script), 1);
    CHECK_EQ(r.script[2].uci, std::string("e6d5"));
    CHECK(r.verdict.materialSwing <= -2);
}

TEST(coach_review_fork_worked_example) {
    const ai::Analysis a0 = forkA0();
    for (int level = 1; level <= 6; ++level) {
        Game g = forkGame();
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, a0);
        char where[32];
        std::snprintf(where, sizeof where, "fork b%d", level);
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK_EQ(r.verdict.exType, ExType::Fork);   // before king safety / positional (ExType order)
        CHECK(r.verdict.voiced);
        CHECK(r.offersTakeback);
        checkScript(r.script, where);
        const std::string key = "ex.fork.b" + std::to_string(level);
        const Beat* cause = beatWithKey(r.script, key);
        CHECK(cause != nullptr);
        if (!cause) {
            dump(r.script);
            continue;
        }
        const Arg* t1 = cause->line.arg("t1");
        const Arg* t2 = cause->line.arg("t2");
        CHECK(t1 && t2);
        if (t1 && t2) {
            CHECK_EQ(t1->square, sq("e5"));   // the knight first (more valuable)
            CHECK_EQ(t2->square, sq("g2"));
            CHECK(t1->own && t2->own);
        }
        // Levels 1-3: the whole sequence, Qg5 Bxf7+ Kd8 O-O Qxe5, then what it cost (a knight for two
        // pawns: one point). Levels 4-6: Qg5, then the coach points at what falls.
        const int shown = demoMoves(r.script);
        CHECK_EQ(shown, level <= 3 ? 5 : 1);
        bool took = false;
        for (const Beat& b : r.script)
            if (b.kind == BeatKind::DemoMove && b.uci == "g5e5") took = true;
        CHECK_EQ(took, level <= 3);
        if (level <= 3) {
            const Beat* res = beatWithKey(r.script, "ex.result.b" + std::to_string(level));
            CHECK(res != nullptr);
            if (res && res->line.arg("pts")) CHECK_EQ(res->line.arg("pts")->number, 1);
        } else {
            CHECK(hasKey(r.script, "ex.point.two"));
        }
        if (level == 4) {
            const Arg* pts = cause->line.arg("pts");
            CHECK(pts && pts->number == 1);
        }
        CHECK(sentences(r.script) <= band(level).sentences);
        if (level == 1) {
            CHECK(hasKey(r.script, "ex.fork.tail.b1"));
            CHECK(hasKey(r.script, "ex.offer.b1"));
            const Arg* my = cause->line.arg("my");
            CHECK(my && my->piece == Queen && !my->own && my->square == sq("d8"));
            int point = 0;
            for (const Gesture& gs : cause->gestures) point += isPointing(gs) ? 1 : 0;
            CHECK_EQ(point, 3);
        }
        if (level >= 3) CHECK(hasKey(r.script, "ex.better.b" + std::to_string(level)) || level == 6);
    }
}

TEST(coach_review_mate_allowed_first) {
    // 3...Nf6?? allows Qxf7#: a capture of a pawn too, but the mate explains it (ExType order).
    Game g = gameOf(nullptr, {"e4", "e5", "Bc4", "Nc6", "Qh5", "Nf6"});
    const ai::Analysis a0 =
        analysisOf({pvl(-30, "g7g6 h5f3 g8f6"), pvl(-40, "d8e7 g1f3"), pvl(0, "g8f6 h5f7", -1)});
    for (int level = 1; level <= 6; ++level) {
        Reviewer rv;
        rv.reset(level, Black);
        Review r = reviewOf(rv, g, a0);
        char where[32];
        std::snprintf(where, sizeof where, "mate allowed b%d", level);
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK(r.verdict.mateAllowed);
        CHECK_EQ(r.verdict.exType, ExType::MateAllowed);
        CHECK(r.offersTakeback);   // a mate in 1 is within every level's limit
        CHECK(hasKey(r.script, "ex.mate_allowed.b" + std::to_string(level)));
        CHECK_EQ(demoMoves(r.script), 1);   // the mating move, then the rewind
        CHECK(sentences(r.script) <= band(level).sentences);
        checkScript(r.script, where);
        if (level == 1) CHECK(hasKey(r.script, "ex.mate_allowed.tail.b1"));
    }
}

TEST(coach_review_mate_missed) {
    // Rd8 mates; the player shuffles the king instead.
    const char* fen = "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1";
    const ai::Analysis a0 = analysisOf({pvl(0, "d1d8", 1), pvl(600, "d1d7 g8f8"), pvl(550, "g1f1 g8f8")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(fen, {"Kf1"});
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, a0);
        char where[32];
        std::snprintf(where, sizeof where, "mate missed b%d", level);
        CHECK(r.verdict.mateMissed);
        CHECK_EQ(r.verdict.exType, ExType::MateMissed);
        CHECK(r.verdict.voiced);
        CHECK(hasKey(r.script, "ex.mate_missed.b" + std::to_string(level)));
        CHECK_EQ(demoMoves(r.script), 0);   // the table shows the position after the move: trace only
        checkScript(r.script, where);
        if (level <= 2) CHECK(hasKey(r.script, "ex.verdict.missed.b" + std::to_string(level)));
        // Still winning by far after Kf1: from level 3, the mate is said without a verdict or an offer.
        if (level >= 3)
            for (const std::string& k : keysOf(r.script)) CHECK(!startsWith(k, "ex.verdict."));
        CHECK_EQ(r.offersTakeback, level <= 2);
        if (level == 1) {
            g.undo(1);
            Script hint = rv.takebackAccepted(g);
            CHECK(hasKey(hint, "tb.hint.mate_missed.b1"));
            checkScript(hint, "mate missed hint");
        }
    }
    // A mate deeper than the level's limit is not voiced as a missed mate at level 1.
    Game g = gameOf(fen, {"Kf1"});
    Reviewer rv;
    rv.reset(1, White);
    const ai::Analysis deep = analysisOf({pvl(0, "d1d7 g8f8 d7d8", 3), pvl(550, "g1f1 g8f8")});
    Review r = reviewOf(rv, g, deep);
    CHECK(r.verdict.exType != ExType::MateMissed);
}

TEST(coach_review_missed_free_piece) {
    // The coach's knight on d5 is free; the player plays h3 instead, and the knight escapes.
    const char* fen = "6k1/5ppp/8/3n4/8/2N5/1P3PPP/6K1 w - - 0 1";
    Game g = gameOf(fen, {"h3"});
    Reviewer rv;
    rv.reset(1, White);
    const ai::Analysis a0 = analysisOf({pvl(350, "c3d5 g8f8"), pvl(30, "h2h3 d5e7")});
    Review r = reviewOf(rv, g, a0);
    CHECK_EQ(r.verdict.exType, ExType::MissedCapture);
    CHECK(r.verdict.voiced);
    CHECK(r.offersTakeback);   // levels 1-2: the student makes the capture themselves
    CHECK(hasKey(r.script, "ex.missed_capture.b1"));
    CHECK(hasKey(r.script, "ex.verdict.missed.b1"));
    CHECK_EQ(demoMoves(r.script), 0);
    checkScript(r.script, "missed capture");
}

TEST(coach_review_demo_stops_before_promotion) {
    // Level 6 (demonstrations up to 8 plies): the line reaches the player's promotion on its second
    // ply. The coach shows its own move, points at b8, and takes back exactly the one move shown.
    detail::Ctx c;
    c.level = 6;
    c.b = band(6);
    c.human = White;
    c.coach = Black;
    c.ply = 20;
    CHECK(c.p1.setFEN("4k3/1P6/8/8/8/7r/8/4K3 b - - 0 1"));
    c.r = replayLine(c.p1, {"h3h2", "b7b8q", "e8d7", "b8b5"}, White);
    CHECK_EQ(c.r.size(), size_t(4));
    detail::Explanation ex;
    ex.demoPlies = 4;
    Script s;
    detail::appendDemo(c, ex, s);
    checkScript(s, "promotion stop");
    CHECK_EQ(demoMoves(s), 1);
    for (const Beat& b : s) CHECK(b.uci != "b7b8q");
    const Beat* stop = beatWithKey(s, "demo.promote");
    CHECK(stop != nullptr);
    if (stop) {
        CHECK(!stop->gestures.empty());
        if (!stop->gestures.empty()) {
            CHECK(stop->gestures[0].kind == GestureKind::PointSquare);
            CHECK_EQ(stop->gestures[0].square, sq("b8"));
        }
    }
    CHECK(!s.empty() && s.back().kind == BeatKind::Rewind);
    if (!s.empty()) CHECK_EQ(s.back().count, 1);

    // Never deeper than the level: level 4 shows at most 4 plies of a 5-ply line.
    c.level = 4;
    c.b = band(4);
    CHECK(c.p1.setFEN("4k3/8/8/8/8/7r/1P6/4K3 b - - 0 1"));
    c.r = replayLine(c.p1, {"h3h4", "b2b4", "e8d7", "b4b5", "d7c7"}, White);
    ex.demoPlies = 5;
    Script s2;
    detail::appendDemo(c, ex, s2);
    checkScript(s2, "depth cap");
    CHECK_EQ(demoMoves(s2), 4);
    if (!s2.empty()) CHECK_EQ(s2.back().count, 4);
}

TEST(coach_review_demo_plan_per_level) {
    // One loss: 1...Rh2 attacks the knight, 2.Ka7 Rxb2 takes it. Levels 1-3 play the whole sequence;
    // levels 4-6 play the rook's move, then point at the knight; a sequence longer than the level's
    // demonstrations is played up to the problem instead.
    detail::Ctx c;
    c.human = White;
    c.coach = Black;
    c.ply = 20;
    CHECK(c.p1.setFEN("K3k3/8/8/8/8/7r/1N6/8 b - - 0 1"));
    c.base = materialBalance(c.p1, White);
    c.r = replayLine(c.p1, {"h3h2", "a8a7", "h2b2", "a7a6"}, White);
    CHECK_EQ(c.r.size(), size_t(4));
    for (int level = 1; level <= 6; ++level) {
        c.level = level;
        c.b = band(level);
        detail::Explanation ex;
        ex.fullPlies = 3;     // the knight taken, for good
        ex.threatPlies = 1;   // the rook attacks it
        ex.lost = 3;
        ex.targets = {sq("b2")};
        detail::planDemo(c, ex);
        CHECK_EQ(ex.full, level <= 3);
        CHECK_EQ(ex.demoPlies, level <= 3 ? 3 : 1);
        Script s;
        detail::appendDemo(c, ex, s);
        checkScript(s, "demo plan");
        CHECK_EQ(demoMoves(s), ex.demoPlies);
        if (level <= 3) {
            CHECK(hasKey(s, "ex.result.b" + std::to_string(level)));
        } else {
            const Beat* point = beatWithKey(s, "ex.point.piece");
            CHECK(point != nullptr);
            if (point) {
                const Arg* t1 = point->line.arg("t1");
                CHECK(t1 && t1->square == sq("b2") && t1->piece == Knight);
            }
        }
        CHECK(!s.empty() && s.back().kind == BeatKind::Rewind);
    }
    // Where the sequence ends, the player could still take a pawn back: the line says so, with
    // what is lost even then.
    c.level = 1;
    c.b = band(1);
    {
        detail::Ctx d = c;
        CHECK(d.p1.setFEN("K3k3/8/8/2p5/3P4/7r/1N6/8 b - - 0 1"));
        d.base = materialBalance(d.p1, White);
        d.r = replayLine(d.p1, {"h3h2", "a8a7", "h2b2", "d4c5"}, White);
        detail::Explanation back;
        back.fullPlies = 3;
        back.threatPlies = 1;
        back.lost = 2;
        back.targets = {sq("b2")};
        detail::planDemo(d, back);
        Script sb;
        detail::appendDemo(d, back, sb);
        checkScript(sb, "result back");
        CHECK_EQ(demoMoves(sb), 3);
        CHECK(hasKey(sb, "ex.result_back.b1"));
    }
    // A sequence longer than the level's demonstrations: up to the problem, then the pointing.
    c.level = 1;
    c.b = band(1);
    detail::Explanation lng;
    lng.fullPlies = 7;
    lng.threatPlies = 1;
    lng.lost = 3;
    lng.targets = {sq("b2")};
    detail::planDemo(c, lng);
    CHECK(!lng.full);
    CHECK_EQ(lng.demoPlies, 1);
    // A mate in one is shown at every level, nothing else to point at.
    for (int level : {1, 4, 6}) {
        c.level = level;
        c.b = band(level);
        detail::Explanation m;
        m.fullPlies = 1;
        m.mate = true;
        detail::planDemo(c, m);
        CHECK(m.full);
        CHECK_EQ(m.demoPlies, 1);
    }
    // Nothing that stands on the board to point at, no threat line: words only (levels 4-6).
    c.level = 5;
    c.b = band(5);
    detail::Explanation none;
    none.fullPlies = 3;
    none.threatPlies = 0;
    detail::planDemo(c, none);
    CHECK_EQ(none.demoPlies, 0);
}

TEST(coach_review_demo_length_caps) {
    detail::Ctx c;
    c.human = White;
    c.coach = Black;
    CHECK(c.p1.setFEN("4k3/8/8/8/8/7r/1P6/4K3 b - - 0 1"));
    c.r = replayLine(c.p1, {"h3h4", "b2b4", "e8d7", "b4b5", "d7c7"}, White);
    detail::Explanation ex;
    ex.demoPlies = 5;
    const int want[6] = {5, 5, 5, 4, 4, 4};
    for (int level = 1; level <= 6; ++level) {
        c.level = level;
        c.b = band(level);
        CHECK_EQ(detail::demoLength(c, ex), want[level - 1]);
    }
    // A mating line goes as far as the level's mates (level 4: up to a mate in four).
    c.level = 4;
    c.b = band(4);
    ex.mate = true;
    CHECK_EQ(detail::demoLength(c, ex), 5);
    // Never past the end of the line.
    ex.mate = false;
    ex.demoPlies = 9;
    c.level = 2;
    c.b = band(2);
    CHECK_EQ(detail::demoLength(c, ex), 5);
}

TEST(coach_review_mate_in_two_shown_at_level_2) {
    // 1.a3?? allows 1...Rd1+ 2.Rxd1 Rxd1#: level 2 announces the mate in two, plays it on the
    // table, names the back-rank mate on the final position, then puts the pieces back.
    const char* fen = "3r2k1/3r1ppp/8/8/8/8/P4PPP/2R3K1 w - - 0 1";
    const ai::Analysis a0 =
        analysisOf({pvl(-20, "h2h3 d7d2"), pvl(-30, "g2g3 d7d2"), pvl(0, "a2a3 d7d1 c1d1 d8d1", -2)});
    Game g = gameOf(fen, {"a3"});
    Reviewer rv;
    rv.reset(2, White);
    Review r = reviewOf(rv, g, a0);
    CHECK_EQ(r.verdict.exType, ExType::MateAllowed);
    checkScript(r.script, "mate in two b2");
    CHECK_EQ(demoMoves(r.script), 3);
    const std::vector<std::string> keys = keysOf(r.script);
    auto at = [&](const std::string& k) {
        return int(std::find(keys.begin(), keys.end(), k) - keys.begin());
    };
    const int cause = at("ex.mate_allowed.b2"), mate = at("demo.my.mate"), name = at("ex.mate_allowed.pattern"),
              rewind = at("ex.rewind.b2");
    CHECK(cause < mate && mate < name && name < rewind && rewind < int(keys.size()));
    if (!(cause < mate && mate < name && name < rewind && rewind < int(keys.size()))) dump(r.script);
    const Beat* named = beatWithKey(r.script, "ex.mate_allowed.pattern");
    CHECK(named && named->line.arg("text") && named->line.arg("text")->text == "name.pattern.back_rank");
    CHECK(r.offersTakeback);
}

namespace {

struct Scenario {
    const char* name;
    const char* fen;                   // nullptr: the standard start
    std::vector<const char*> sans;     // the last one is the player's move
    Color human;
    std::vector<ai::PvLine> lines;     // A0 on the position before the player's move
    ExType expected;
    int from, to;                      // levels where 'expected' is the explanation
};

Game gameOf(const char* fen, const std::vector<const char*>& sans) {
    Game g;
    if (fen) CHECK(g.resetFromFEN(fen));
    for (const char* s : sans) {
        const Move m = g.position().parseSAN(s);
        if (!m.valid()) std::fprintf(stderr, "  bad SAN in test: %s\n", s);
        CHECK(m.valid());
        if (!m.valid()) break;
        g.play(m);
    }
    return g;
}

std::vector<Scenario> scenarios() {
    return {
        {"discovered", "6k1/1b3ppp/8/3n4/8/8/PP6/1K5R w - - 0 1", {"Kc2"}, White,
         {pvl(0, "h1e1 g8f8"), pvl(-250, "b1c2 d5e3 c2d3 b7h1")}, ExType::Discovered, 1, 6},
        {"skewer", "r5k1/5ppp/8/8/7R/2K5/5PP1/8 w - - 0 1", {"Kc4"}, White,
         {pvl(-50, "c3d3 g8f8"), pvl(-600, "c3c4 a8a4 c4d5 a4h4")}, ExType::Skewer, 1, 6},
        {"pin", "r5k1/5ppp/8/8/8/2N5/7P/4K3 w - - 0 1", {"Ne4"}, White,
         {pvl(150, "e1d2 g8f8"), pvl(-200, "c3e4 a8e8 e1f2 e8e4")}, ExType::Pin, 1, 6},
        {"trapped", "2k5/ppp2ppp/8/8/8/4B3/5PPP/6K1 w - - 0 1", {"Bxa7"}, White,
         {pvl(100, "g1f1 c8d7"), pvl(-250, "e3a7 b7b6 g1f1 c8b7 f1e2 b7a7")}, ExType::Trapped, 1, 6},
        {"exchange", "6k1/5ppp/4p3/3p4/8/8/3Q1PPP/3R2K1 w - - 0 1", {"Qxd5"}, White,
         {pvl(900, "h2h3 g8f8"), pvl(150, "d2d5 e6d5 d1d5")}, ExType::Exchange, 1, 6},
        {"missed fork", "r3k3/8/8/1N6/8/8/5PPP/6K1 w - - 0 1", {"h3"}, White,
         {pvl(500, "b5c7 e8d7 c7a8 d7c8"), pvl(0, "h2h3 e8d7")}, ExType::MissedFork, 1, 6},
        {"promotion race", "8/8/5k2/8/7P/p2K4/8/8 w - - 0 1", {"h5"}, White,
         {pvl(300, "d3c2 a3a2 c2b2"), pvl(-800, "h4h5 a3a2 d3c2 a2a1q")}, ExType::PromotionRace, 1, 6},
        {"passed pawn", "8/8/8/1P6/3k4/8/8/6K1 w - - 0 1", {"Kh2"}, White,
         {pvl(900, "b5b6 d4c5 b6b7"), pvl(0, "g1h2 d4c5")}, ExType::Endgame, 1, 6},
        {"positional", nullptr, {"e4", "e5", "Nf3", "Nc6", "a3"}, White,
         {pvl(30, "f1c4 g8f6"), pvl(-120, "a2a3 g8f6")}, ExType::Positional, 4, 6},
        {"stalemate", "7k/5K2/8/6Q1/8/8/8/8 w - - 0 1", {"Qg6"}, White,
         {pvl(0, "g5g7", 1), pvl(900, "g5h6 h8g8")}, ExType::Stalemate, 1, 6},
        {"early queen", nullptr, {"e4", "e5", "Qh5"}, White,
         {pvl(40, "g1f3 b8c6"), pvl(-10, "d1h5 b8c6 f1c4 g7g6")}, ExType::Opening, 1, 3},
        {"threat ignored", "4k3/4p3/8/8/3N4/8/7P/6K1 b - - 0 1", {"e5", "h3"}, White,
         {pvl(250, "d4f5 e8d7"), pvl(-80, "h2h3 e5d4")}, ExType::Hanging, 1, 6},
        {"guard removed", "6k1/5ppp/8/8/1b6/2N5/5PPP/2R3K1 w - - 0 1", {"Re1"}, White,
         {pvl(0, "g1f1 b4c3 c1c3"), pvl(-330, "c1e1 b4c3")}, ExType::Hanging, 1, 6},
        {"slower mate", "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1", {"Rd7"}, White,
         {pvl(0, "d1d8", 1), pvl(0, "d1d7 h7h6", 3)}, ExType::MateMissed, 5, 6},
    };
}

}  // namespace

TEST(coach_review_explanation_table) {
    for (const Scenario& sc : scenarios()) {
        const ai::Analysis a0 = analysisOf(sc.lines);
        for (int level = 1; level <= 6; ++level) {
            Game g = gameOf(sc.fen, sc.sans);
            Reviewer rv;
            rv.reset(level, sc.human);
            Review r = reviewOf(rv, g, a0);
            char where[64];
            std::snprintf(where, sizeof where, "%s b%d", sc.name, level);
            checkScript(r.script, where);
            CHECK(sentences(r.script) <= band(level).sentences);
            CHECK(demoMoves(r.script) <= band(level).demoPlies);
            if (level < sc.from || level > sc.to) continue;
            if (r.verdict.exType != sc.expected || !r.verdict.voiced) {
                std::fprintf(stderr, "  %s: %s (voiced %d), wanted %s; class %s\n", where,
                             exTypeName(r.verdict.exType), int(r.verdict.voiced), exTypeName(sc.expected),
                             moveClassName(r.verdict.cls));
                dump(r.script);
            }
            CHECK_EQ(r.verdict.exType, sc.expected);
            CHECK(r.verdict.voiced);
            // The line of that explanation is said (a tip or an "ex." family).
            bool said = false;
            for (const std::string& k : keysOf(r.script))
                if (startsWith(k, "tip.") || (startsWith(k, "ex.") && !startsWith(k, "ex.verdict.") &&
                                             !startsWith(k, "ex.offer.") && !startsWith(k, "ex.rewind.")))
                    said = true;
            CHECK(said);
        }
    }
    // Families by level: the guard (levels 1-4) and the ignored threat (levels 1-3) have their own lines.
    for (int level = 1; level <= 6; ++level) {
        for (const Scenario& sc : scenarios()) {
            const bool guard = std::strcmp(sc.name, "guard removed") == 0;
            const bool threat = std::strcmp(sc.name, "threat ignored") == 0;
            if (!guard && !threat) continue;
            Game g = gameOf(sc.fen, sc.sans);
            Reviewer rv;
            rv.reset(level, sc.human);
            Review r = reviewOf(rv, g, analysisOf(sc.lines));
            const std::string lv = ".b" + std::to_string(level);
            if (guard) CHECK(hasKey(r.script, (level <= 4 ? "ex.hanging_guard" : "ex.hanging") + lv));
            if (threat) CHECK(hasKey(r.script, (level <= 3 ? "ex.hanging_threat" : "ex.hanging") + lv));
        }
    }
}

// ---- Claims that hold on the board ---------------------------------------------------------------

namespace {

Game gameOfUci(const char* moves) {
    Game g;
    std::istringstream in(moves);
    std::string u;
    while (in >> u) {
        const Move m = g.position().parseUCI(u);
        if (!m.valid()) std::fprintf(stderr, "  bad UCI in test: %s\n", u.c_str());
        CHECK(m.valid());
        if (!m.valid()) break;
        g.play(m);
    }
    return g;
}

// A human move recorded by tools/coach_audit (its records file, audit.h): "rec <w|b> <played>
// <startfen|startpos> ; <moves...>", then "an <slot> <ok> <wtm> <nolegal> <depth> <best> <lines>"
// with its "ln <multipv> <depth> <cp> <mate> <matedNow> <matesNow> <bound> <pv...>" lines, "end".
struct Recorded {
    Game g;   // the played move is the last one
    Color human = White;
    ai::Analysis a0, a1, a2, a3;
    bool hasA1 = false, hasA2 = false;
};

Recorded recorded(const char* text) {
    Recorded rc;
    std::istringstream in(text);
    std::string line, played;
    ai::Analysis* an = nullptr;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "rec") {
            std::string side, fen, m;
            ls >> side >> played;
            rc.human = side == "w" ? White : Black;
            while (ls >> m && m != ";") fen += (fen.empty() ? "" : " ") + m;
            if (fen != "startpos") CHECK(rc.g.resetFromFEN(fen));
            while (ls >> m) {
                const Move mv = rc.g.position().parseUCI(m);
                CHECK(mv.valid());
                if (mv.valid()) rc.g.play(mv);
            }
        } else if (tag == "an") {
            std::string slot, best;
            int ok = 0, wtm = 0, nl = 0;
            size_t n = 0;
            ai::Analysis a;
            ls >> slot >> ok >> wtm >> nl >> a.depth >> best >> n;
            a.ok = ok != 0;
            a.whiteToMove = wtm != 0;
            a.noLegalMove = nl != 0;
            a.bestMove = best == "-" ? std::string() : best;
            an = slot == "a0" ? &rc.a0 : slot == "a1" ? &rc.a1 : slot == "a2" ? &rc.a2 : &rc.a3;
            rc.hasA1 = rc.hasA1 || slot == "a1";
            rc.hasA2 = rc.hasA2 || slot == "a2";
            *an = a;
        } else if (tag == "ln" && an) {
            ai::PvLine l;
            int mn = 0, ms = 0;
            char b = 'E';
            ls >> l.multipv >> l.depth >> l.score.cp >> l.score.mate >> mn >> ms >> b;
            l.score.matedNow = mn != 0;
            l.score.matesNow = ms != 0;
            l.score.bound = b == 'L' ? ai::Score::Bound::Lower : b == 'U' ? ai::Score::Bound::Upper : ai::Score::Bound::Exact;
            std::string m;
            while (ls >> m) l.pv.push_back(m);
            an->lines.push_back(l);
        }
    }
    const Move m = rc.g.position().parseUCI(played);
    CHECK(m.valid());
    if (m.valid()) rc.g.play(m);
    return rc;
}

// The review at 'level', with the analyses a Coach game asks for there (as coach_audit replays it).
Review reviewRecorded(Reviewer& rv, const Recorded& rc, int level) {
    rv.reset(level, rc.human);
    ReviewInput in;
    in.game = &rc.g;
    in.before = &rc.a0;
    in.played = rc.hasA1 ? &rc.a1 : nullptr;
    in.after = level >= 4 && rc.hasA2 ? &rc.a2 : nullptr;
    in.shallow = level == 3 || level == 4 ? &rc.a3 : nullptr;
    return rv.review(in);
}

bool hasKeyPrefix(const Script& s, const char* prefix) {
    for (const std::string& k : keysOf(s))
        if (startsWith(k, prefix)) return true;
    return false;
}

// A game of a student's (coach level 2, White): 11.g3?? opens the long diagonal, and 11...Nxd2 trades
// the knight for the bishop while uncovering the queen on d5 against the rook on h1.
const char* kG3Game =
    "d2d4 d7d5 e2e3 g8f6 f1b5 c8d7 b5d7 b8d7 f2f4 a8c8 c2c4 d5c4 b1a3 d7b6 c1d2 d8d5 b2b3 c4b3 a2b3 f6e4 g2g3";
ai::Analysis g3A0(const char* played) {
    return analysisOf({pvl(-60, "g1f3 e4d2 d1d2"), pvl(-90, "g1e2 e4d2 d1d2"), pvl(-620, played)});
}

// Olivier's game, as recorded (coach level 2, depth 18): 11.g3?? Nxd2, and the engine's line goes on
// 12.Nf3 Nxf3+, the bishop given up: 12.Qxd2 would uncover the rook on h1 to ...Qxh1.
const char* kG3Record =
    "rec w g2g3 startpos ; d2d4 d7d5 e2e3 g8f6 f1b5 c8d7 b5d7 b8d7 f2f4 a8c8 c2c4 d5c4 b1a3 d7b6 c1d2 d8d5 b2b3 c4b3 a2b3 f6e4\n"
    "an a0 1 1 0 18 g1f3 3\n"
    "ln 1 18 -171 0 0 0 E g1f3 a7a6 d1c2 e4d2 c2d2 e7e6 e1g1 f8e7 d2d3 e8g8 a3c4 d5d8 c4b6 c7b6 g2g4 d8d5 g1f2 b6b5 g4g5\n"
    "ln 2 18 -194 0 0 0 E a3c4 b6c4 b3c4 d5c4 g1f3 a7a6 f3e5 c4d5 a1a5 b7b5 d2b4 c7c5 b4c5 f7f6 e5d3 e4c5 d3c5 d5g2 h1f1 e7e6 d1e2 g2e2 e1e2 f8c5 d4c5 c8c6 f1a1 e8f7 a5a6 c6c5\n"
    "ln 3 18 -202 0 0 0 E b3b4 e7e6 g1f3 f8e7 d1c2 e4d2 c2d2 e8g8 e1g1 d5e4 f1b1 b6d5 a3c4 a7a6 b1b3 b7b5 c4e5 d5b4 b3b4 e7b4\n"
    "an a1 1 1 0 18 g2g3 1\n"
    "ln 1 18 -793 0 0 0 E g2g3 e4d2 g1f3 d2f3 e1f2 f3d4 d1d4 d5d4 e3d4 e7e6 a3c4 c8b8 a1a7 b6d5 h1c1 g7g6 f2f3\n"
    "an a2 1 0 0 14 e4d2 2\n"
    "ln 1 14 819 0 0 0 E e4d2 g1f3 d2f3 e1f2 f3d4 d1d4 d5d4 e3d4 e7e6 a3c4 c8a8 c4b6 c7b6 a1c1 f8d6 d4d5 d6c5 c1c5 b6c5\n"
    "ln 2 14 575 0 0 0 E e4g3 h2g3 d5h1 e1f2 g7g5 f4g5 h1h2 f2f3 h7h6 g5g6 f7g6 g1e2 h2h5 g3g4\n"
    "an a3 1 1 0 6 g1f3 1\n"
    "ln 1 6 -171 0 0 0 E g1f3 a7a6 d1c2 e4d2 c2d2 e7e6 e1g1 f8e7 d2d3\n"
    "end\n";

// From Olivier's games (depth 18): 21.Kd2?? Rxd3+ 22.Kxd3 Qxa1 loses the rook, but 21.Kc2 (the best
// move) loses as much after ...Qxh2+ and ...hxg5: no material reason holds, and the king's safety
// would not be the reason either.
const char* kKd2Record =
    "rec w c1d2 startpos ; e2e4 e7e5 f2f3 g8f6 g1e2 d7d5 d2d3 f8c5 e2c3 c8e6 e4d5 e6h3 g2h3 e8g8 c3b5 f6d5 d1e2 d8h4 e1d1 f8d8 b1c3 c7c6 b5a3 a7a5 c3d5 d8d5 c2c4 d5d6 e2e5 b8a6 c1g5 h4h5 d1c2 a6b4 c2c1 h5f3 e5c5 b4d3 f1d3 f3h1\n"
    "an a0 1 1 0 18 c1c2 3\n"
    "ln 1 18 241 0 0 0 E c1c2 h1h2 c2c3 h7h6 a1d1 h6g5 c5d6 h2d6 d3h7 g8h7 d1d6 a8e8 c3d2 f7f5 a3c2 f5f4 c2e1 e8e3 d6d3 e3e8 d3a3\n"
    "ln 2 18 -173 0 0 0 E c1d2 d6d3 d2d3 h1a1 d3c2 a1f1 h3h4 f1g2 g5d2 g2e4 c2c1 h7h6 c5f2 a5a4 d2c3 a8d8 b2b3 d8d3 c1b2 a4b3 a2b3 d3h3\n"
    "ln 3 18 -1199 0 0 0 E c5g1 h1g1 c1c2 g1f2 g5d2 d6d3 a1d1 a8d8 a3b1 a5a4 a2a3 h7h6 c2c1 f2d4 h3h4 d4c4 b1c3\n"
    "an a2 1 0 0 14 d6d3 2\n"
    "ln 1 14 173 0 0 0 E d6d3 d2d3 h1a1 d3c2 a1f1 h3h4 f1g2 g5d2 g2e4 c2c1 h7h6 c5f2 a5a4 d2c3 a8d8\n"
    "ln 2 14 -250 0 0 0 E h1h2 d2c3 h7h6 a1d1 h6g5 c5d6 h2d6 d3h7 g8h7 d1d6 a8e8 c3d2 f7f5 a3c2 f5f4 c2e1 e8e3 d6d3 e3e8 d3a3 h7g6 e1f3 e8h8 f3e5 g6f6 e5g4 f6g6 g4f2\n"
    "an a3 1 1 0 6 c1c2 1\n"
    "ln 1 6 241 0 0 0 E c1c2 h1h2 c2c3 b7b5 c4b5 d6d5 c5e3 c6b5 a3b5\n"
    "end\n";

}  // namespace

TEST(coach_review_small_loss_never_explains_a_collapse) {
    // The discovered attack of the table, but the engine says the move loses nine pawns: the exchange
    // it wins back (two points, at most eight pawns of a drop) is not why. No material reason is
    // given for it, at any level.
    const ai::Analysis a0 = analysisOf({pvl(0, "h1e1 g8f8"), pvl(-900, "b1c2 d5e3 c2d3 b7h1")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf("6k1/1b3ppp/8/3n4/8/8/PP6/1K5R w - - 0 1", {"Kc2"});
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, a0);
        char where[32];
        std::snprintf(where, sizeof where, "collapse b%d", level);
        checkScript(r.script, where);
        CHECK(r.verdict.exType != ExType::Discovered && r.verdict.exType != ExType::Material &&
              r.verdict.exType != ExType::Hanging && r.verdict.exType != ExType::Exchange);
        for (const Beat& b : r.script)
            if (const Arg* pts = b.line.arg("pts")) CHECK(pts->number >= 3);
    }
}

TEST(coach_review_rook_lost_in_a_lost_position) {
    // From a recorded game (depth 18): 16.Kg2? Nxg3+ 17.Kxg3 Qxh1 loses the exchange and more, in a
    // position already lost (W% 16 -> 10: an inaccuracy by the numbers). A dubious move gets a remark
    // and nothing more: "that move looks dubious" (levels 3-6; "a bit better" would play the rook
    // down), "... maybe you can do better?" with the takeback card (levels 1-2). Never a verdict, a
    // reason or a demonstration.
    const std::vector<const char*> moves = {"d4", "d5", "e3", "Nf6", "Bb5+", "Bd7", "Bxd7+", "Nbxd7", "f4", "Rc8",
                                            "c4", "dxc4", "Na3", "Nb6", "Bd2", "Qd5", "b3", "cxb3", "axb3", "Ne4",
                                            "g3", "e6", "Bc1", "Qa5+", "Kf1", "Qd5", "Nc4", "Rd8", "Ne5", "a6", "Kg2"};
    const ai::Analysis a0 = analysisOf({pvl(-447, "d1f3 f8d6 e5g4 c7c5 a1a5 f7f5 g4f2 e8g8 f1g2 b6d7"),
                                        pvl(-468, "d1c2 f7f6 e5f3 c7c5 a1a5 g7g5 f4g5 f6g5"),
                                        pvl(-486, "e5f3 f8b4 f1g2 e8g8 d1c2 c7c5 d4c5 d8c8")});
    const ai::Analysis a1 = analysisOf({pvl(-582, "f1g2 e4g3 g2g3 d5h1 e5f3 g7g5 f4g5 h7h6 e3e4 h8g8 a1a2 h6g5")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(nullptr, moves);
        Reviewer rv;
        rv.reset(level, White);
        ReviewInput in;
        in.game = &g;
        in.before = &a0;
        in.played = &a1;
        Review r = rv.review(in);
        char where[32];
        std::snprintf(where, sizeof where, "rook lost b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.cls, MoveClass::Inaccuracy);
        CHECK(r.verdict.voiced);
        CHECK_EQ(demoMoves(r.script), 0);
        CHECK_EQ(r.script.size(), size_t(1));
        if (r.script.empty()) continue;
        if (level <= 2) {
            CHECK(r.offersTakeback);
            CHECK(r.script[0].kind == BeatKind::OfferTakeback);
            CHECK_EQ(r.script[0].line.key, "ex.dubious.b" + std::to_string(level));
        } else {
            CHECK(!r.offersTakeback);
            CHECK_EQ(r.script[0].line.key, std::string("ex.verdict.dubious"));
        }
    }
}

TEST(coach_review_dubious_move_invites_a_retry_at_levels_1_2) {
    // 1.a4 gives the b4 pawn to the rook: W% 59 -> 50, an inaccuracy. Levels 3-6: the better move
    // named in one line. Levels 1-2: "that move is dubious, maybe you can do better?" with the
    // takeback card, the move kept back: nothing said when the offer is answered, the coach's praise
    // when the replay finds the best move, and not again on a replay.
    const char* fen = "1r4k1/5ppp/8/8/1P6/P7/5PPP/6K1 w - - 0 1";
    const ai::Analysis a0 = analysisOf({pvl(100, "g1f1 g8f8"), pvl(0, "a3a4 b8b4 g1f1")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(fen, {"a4"});
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, a0);
        char where[32];
        std::snprintf(where, sizeof where, "dubious b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.cls, MoveClass::Inaccuracy);
        CHECK_EQ(demoMoves(r.script), 0);
        CHECK_EQ(r.script.size(), size_t(1));
        if (r.script.empty()) continue;
        if (level >= 3) {
            CHECK(!r.offersTakeback);
            CHECK(startsWith(r.script[0].line.key, "ex.verdict.inaccuracy.b"));
            continue;
        }
        CHECK(r.offersTakeback);
        CHECK_EQ(r.script[0].line.key, "ex.dubious.b" + std::to_string(level));
        CHECK(r.script[0].line.arg("best") == nullptr);
        // Accepted: no hint. The replay finds the best move: praised.
        CHECK(g.undo(1));
        CHECK(rv.takebackAccepted(g).empty());
        g.play(g.position().parseSAN("Kf1"));
        Review found = reviewOf(rv, g, a0);
        CHECK(found.isRetry && found.takeback.found);
        CHECK(!found.offersTakeback);
        CHECK(!found.script.empty());
        if (!found.script.empty()) CHECK_EQ(found.script.front().line.key, std::string("tb.found.b1"));
        checkScript(found.script, "dubious found");
        // Replayed as a dubious move again: no second offer.
        Game g2 = gameOf(fen, {"a4"});
        Reviewer rv2;
        rv2.reset(level, White);
        CHECK(reviewOf(rv2, g2, a0).offersTakeback);
        CHECK(g2.undo(1));
        rv2.takebackAccepted(g2);
        g2.play(g2.position().parseSAN("a4"));
        Review same = reviewOf(rv2, g2, a0);
        CHECK(same.isRetry && same.takeback.same);
        CHECK(!same.offersTakeback);
        // Declined: nothing said, the move not named.
        Game g3 = gameOf(fen, {"a4"});
        Reviewer rv3;
        rv3.reset(level, White);
        CHECK(reviewOf(rv3, g3, a0).offersTakeback);
        CHECK(rv3.takebackDeclined().empty());
        // No offers in this mode: nothing said (the line invites a retry).
        Game g4 = gameOf(fen, {"a4"});
        Reviewer rv4;
        rv4.reset(level, White, false);
        Review off = reviewOf(rv4, g4, a0);
        CHECK(!off.offersTakeback);
        CHECK(!hasKeyPrefix(off.script, "ex.dubious"));
    }
}

TEST(coach_review_discovered_attack_behind_an_even_trade) {
    // The engine's full line: Nxd2 Qxd2 Qxh1. The trade on d2 is even (3 points each, whatever the
    // centipawns say); the rook lost to the discovered attack is the cause, at every level.
    for (const char* played : {"g2g3 e4d2 d1d2 d5h1 e1f1", "g2g3 e4d2 e1d2 d5h1"}) {
        for (int level = 1; level <= 6; ++level) {
            Game g = gameOfUci(kG3Game);
            Reviewer rv;
            rv.reset(level, White);
            Review r = reviewOf(rv, g, g3A0(played));
            char where[64];
            std::snprintf(where, sizeof where, "g3 full b%d", level);
            checkScript(r.script, where);
            CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
            if (r.verdict.exType != ExType::Discovered) {
                std::fprintf(stderr, "  %s: %s\n", where, exTypeName(r.verdict.exType));
                dump(r.script);
            }
            CHECK_EQ(r.verdict.exType, ExType::Discovered);
            CHECK(r.verdict.voiced);
            CHECK(!hasKeyPrefix(r.script, "ex.exchange"));
            CHECK(!hasKeyPrefix(r.script, "ex.retake"));   // the line itself shows the take-back
            const Beat* cause = beatWithKey(r.script, "ex.discovered.b" + std::to_string(level));
            CHECK(cause != nullptr);
            if (cause && cause->line.arg("t1")) CHECK_EQ(cause->line.arg("t1")->square, sq("h1"));
        }
    }
}

TEST(coach_review_failed_take_back_is_explained) {
    // Where the engine's line leaves the bishop on d2 (the human does not take back), the coach says
    // why the take-back fails: "if you take back with Qxd2, Qxh1 takes your rook". Levels 1-3 at the
    // end of the demonstration (before the rewind), levels 4-6 right after the cause.
    const Recorded rc = recorded(kG3Record);
    for (int level = 1; level <= 6; ++level) {
        Reviewer rv;
        const Review r = reviewRecorded(rv, rc, level);
        char where[48];
        std::snprintf(where, sizeof where, "g3 retake b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK(r.verdict.voiced);
        int retake = -1, cause = -1, lastDemo = -1, rewind = -1;
        for (size_t i = 0; i < r.script.size(); ++i) {
            const Beat& b = r.script[i];
            if (b.line.key == "ex.retake.b" + std::to_string(level)) retake = int(i);
            if (b.line.key == "ex.material.b" + std::to_string(level)) cause = int(i);
            if (b.kind == BeatKind::DemoMove) lastDemo = int(i);
            if (b.kind == BeatKind::Rewind) rewind = int(i);
            if (b.kind == BeatKind::DemoMove) CHECK_EQ(b.uci, std::string("e4d2"));   // never the take-back
        }
        CHECK(retake >= 0 && cause >= 0);
        if (retake < 0) {
            dump(r.script);
            continue;
        }
        if (level <= 3) CHECK(lastDemo >= 0 && lastDemo < retake && retake < rewind);
        else CHECK_EQ(retake, cause + 1);
        const Line& l = r.script[size_t(retake)].line;
        CHECK(l.arg("move") && l.arg("move")->uci == "d1d2");
        CHECK(l.arg("reply") && l.arg("reply")->uci == "d5h1");
        CHECK(l.arg("your2") && l.arg("your2")->piece == Queen && l.arg("your2")->square == sq("d1"));
        CHECK(l.arg("my") && l.arg("my")->piece == Queen && l.arg("my")->square == sq("d5"));
        CHECK(l.arg("your") && l.arg("your")->piece == Rook && l.arg("your")->square == sq("h1"));
    }
}

TEST(coach_review_take_back_answer_is_the_strongest_capture) {
    // Recorded moves (tools/coach_audit corpora) where the take-back the line never plays fails:
    // the capture that answers it is a mate when one does it, else one with check when it takes as
    // much ("Rxd7+", not "Nxd7"), and never one the human answers with a capture and check.
    struct Case {
        const char* what;
        const char* rec;
        const char* reply;   // the answer said ("": no take-back line at all)
    };
    const Case cases[] = {
        // 19...Qb7? f4 Nd7 Bxc5+ Ne7 Bxd6: 24...Qxb5 fails to Qxe7#, not merely to Bxe7.
        {"m_309_50",
         "rec b e7b7 startpos ; c2c3 d7d6 d1c2 e7e5 c3c4 e5e4 c2e4 d8e7 e4f4 g7g5 f4f3 b8c6 f3g3 c6d8 b1c3 f8g7 f2f3 g7e5 g3f2 c7c5 a1b1 c8f5 b1a1 a8b8 e1d1 d8c6 e2e4 e5c3 e4f5 c3g7 d2d3 g5g4 f2e2 b7b5 c4b5 c6e5 c1e3\n"
         "an a0 1 0 0 18 g8h6 3\n"
         "ln 1 18 34 0 0 0 E g8h6 e3h6 g7h6 f3g4 e8g8 h2h4 h6g7 g4g5 f8e8 h1h3 e7d7 f5f6 e5g4 e2d2 g7f8 h3g3 d6d5\n"
         "ln 2 18 2 0 0 0 E g8f6 e3g5 e8g8 f3f4 b8b5 f4e5 d6e5 a1c1 e5e4 b2b3 f8d8 e2g4 b5a5 a2a4 e4d3 g1f3 d3d2 c1b1 f6g4 g5e7 d8d7 f1b5 g4e3 d1e2 d7e7 e2d2 c5c4 b1e1 g7h6 d2c3 c4b3 c3b3\n"
         "ln 3 18 -67 0 0 0 E h7h5 d3d4 c5d4 e3d4 g8h6 e2e4 e8g8 f1d3 d6d5 f5f6 d5e4 f6e7 f8e8 d3e4 e8e7 g1e2 b8d8 d1c2 f7f5 d4e5 g7e5\n"
         "an a1 1 0 0 18 e7b7 1\n"
         "ln 1 18 -223 0 0 0 E e7b7 f3f4 e5d7 e3c5 g8e7 c5d6 e8g8 d6b8 d7b8 f5f6 g7f6 e2g4 f6g7 d3d4 f8d8 g4f3 d8d4 d1e1 d4e4 g1e2 e7f5 a1d1\n"
         "an a2 1 1 0 14 f3f4 2\n"
         "ln 1 14 223 0 0 0 E f3f4 e5d7 e3c5 g8e7 c5d6 e8g8 d6b8 d7b8 f5f6 g7f6 e2g4 f6g7 d3d4 f8d8 g4f3 e7d5\n"
         "ln 2 14 42 0 0 0 E h2h3 g4f3 g1f3 g8e7 f3e5 g7e5 e3c5 b7b5 d3d4 b5e2 f1e2 d6c5 d4e5 e7f5 b2b3\n"
         "an a3 1 0 0 6 g8h6 1\n"
         "ln 1 6 37 0 0 0 E g8h6 f5f6 e7f6 f3f4 h6f5 e3c1 e8g8 f4e5 d6e5\n"
         "end\n",
         "e2e7"},
        // 18...Bc8? cxd7 Bxd7 Nxa8 Nf6 Nxb6: 21...Nxe4 fails to Rxd7+ (the knight takes the bishop too).
        {"g_345_17",
         "rec b b7c8 startpos ; e2e4 c7c5 g1f3 a7a6 f1e2 e7e6 d2d4 c5d4 e1g1 d8c7 d1d4 b7b6 c2c4 d7d6 b1c3 c8b7 f1d1 c7c5 c1f4 c5d4 d1d4 e6e5 f3e5 d6e5 d4d8 e8d8 a1d1 d8e8 c3d5 e5f4 d5c7 e8e7 c4c5 b8d7 c5c6\n"
         "an a0 1 0 0 18 b7c6 3\n"
         "ln 1 18 827 0 0 0 E b7c6 c7a8 c6a8 e2a6 g8f6 a6b5 d7e5 b2b3 f6e4 h2h4 e4c5 b3b4 c5e4\n"
         "ln 2 18 709 0 0 0 E g8f6 c6b7 a8b8 c7a6 b8b7 g2g4 d7e5 a6b4 b7d7 g4g5 d7d1 e2d1 f6d7\n"
         "ln 3 18 581 0 0 0 E d7c5 c6b7 a8b8 c7d5 e7d8 b2b4 c5d7 e2a6 f8d6 e4e5 d7e5 d5b6 g8e7 g2g3 d8c7\n"
         "an a1 1 0 0 18 b7c8 1\n"
         "ln 1 18 364 0 0 0 E b7c8 c6d7 c8d7 c7a8 g8f6 a8b6 d7b5 e2b5 a6b5 e4e5 f6g4 b6d5 e7e6 d5c7 e6e5 d1d5 e5f6 c7b5 g7g5 d5d8\n"
         "an a2 1 1 0 14 c6d7 2\n"
         "ln 1 14 -364 0 0 0 E c6d7 c8d7 c7a8 d7e6 e2a6 g8f6 a8b6 g7g6 a2a4 f8g7 a4a5 h8d8 d1e1 f6d7 b6d5 e6d5 e4d5 e7d6 b2b4 g7c3\n"
         "ln 2 14 -441 0 0 0 E c7a8 g8f6 e4e5 d7e5 a8b6 e5c6 b6c8 e7e6 e2c4 e6e5 d1e1 f6e4 c8b6 f8c5 b6d7 e5d6 e1e4 d6d7 e4f4 d7d6 c4a6 h8b8 f4f7 b8b2\n"
         "an a3 1 0 0 6 b7c6 1\n"
         "ln 1 6 827 0 0 0 E b7c6 c7a8 c6a8 e2a6 d7e5\n"
         "end\n",
         "d1d7"},
        // 7...Qd7? Rxh4 Qd6 exd5: after 9...Qxd5, Bxh6 would allow Qxg2+ and Qxg1+ (Bf3 is what wins).
        {"m_316_5",
         "rec b d8d7 startpos ; e2e4 b7b5 c2c3 g8h6 f1b5 h8g8 b5e2 d7d5 d2d3 g7g5 h2h4 g5h4 e1f1\n"
         "an a0 1 0 0 18 d5e4 3\n"
         "ln 1 18 -162 0 0 0 E d5e4 d3e4 d8d1 e2d1 c8a6 d1e2 a6e2 g1e2 h6g4 f2f3 g4e5 h1h4 b8c6 b1d2 e8c8 h4h7 a7a5 h7h5 e7e6 e2d4 c6d4 c3d4 e5c6 d2b3\n"
         "ln 2 18 -192 0 0 0 E d8d6 e4d5 h6g4 d1a4 c8d7 a4d4 h7h5 g1f3 f8g7 d4e4 c7c6 d5c6 d7c6 e4f5 e7e6 f5f4 e6e5 f4b4 d6b4 c3b4 g7f6\n"
         "ln 3 18 -193 0 0 0 E e7e5 e4d5 h6g4 d1a4 c7c6 d5c6 d8b6 d3d4 b8c6 b1d2 c8d7 d2c4 b6d8 a4c2 e5d4 g1f3 d8f6 h1h4 e8c8 c3d4 c6d4\n"
         "an a1 1 0 0 18 d8d7 1\n"
         "ln 1 18 -440 0 0 0 E d8d7 h1h4 d7d6 e4d5 b8d7 b1a3 d6g6 g2g3 d7f6 e2f3 h6f5 h4h1 c8b7 d1a4 e8d8 c1f4 f6d5 a3c4 d5f4\n"
         "an a2 1 1 0 14 h1h4 2\n"
         "ln 1 14 440 0 0 0 E h1h4 d7d6 e4d5 b8d7 b1a3 d6g6 g2g3 d7f6 e2f3 h6f5 h4h1 c8b7 d1a4 e8d8 c1f4 f6d5\n"
         "ln 2 14 364 0 0 0 E b1d2 d7c6 e4d5 c6d5 d2e4 b8d7 h1h4 h6f5 h4h5\n"
         "an a3 1 0 0 6 d5e4 1\n"
         "ln 1 6 -169 0 0 0 E d5e4 d3e4 d8d1 e2d1 c8a6 d1e2 a6e2 g1e2\n"
         "end\n",
         ""},
    };
    for (const Case& k : cases) {
        const Recorded rc = recorded(k.rec);
        int said = 0;
        for (int level = 1; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[48];
            std::snprintf(where, sizeof where, "%s b%d", k.what, level);
            checkScript(r.script, where);
            for (const Beat& b : r.script) {
                if (b.line.key.rfind("ex.retake.", 0) != 0) continue;
                ++said;
                const Arg* reply = b.line.arg("reply");
                CHECK(reply && reply->uci == k.reply);
                if (!reply || reply->uci != k.reply) dump(r.script);
            }
        }
        CHECK(*k.reply == 0 || said > 0);
    }
}

TEST(coach_review_bad_move_series_cases) {
    // Bad moves played in recorded positions (coach_audit record --positions): reasons the audit
    // found wrong, each fixed.
    {
        // 31...Bg4? The rook on a8 already hung to Qxa8+, and dxe5 cannot save it either (it takes
        // the knight instead): never "your rook has no protector", at any level.
        const Recorded rc = recorded(
         "rec b d7g4 startpos ; e2e4 c7c5 d2d3 g7g6 d3d4 c5d4 c2c3 d4c3 b1c3 f8g7 c1g5 d8a5 g1f3 b8c6 g5d2 a5d8 f1c4 g8f6 e1g1 c6a5 c4e2 a5c6 f1e1 d7d6 h2h3 b7b5 e2b5 c8d7 d2g5 e8g8 a1b1 d8b8 b1c1 a7a5 d1d2 c6e5 f3e5 d7b5 a2a4 b5e8 e5f3 b8b7 f3d4 b7b6 b2b3 e8d7 e1d1 f8d8 d2e2 d8c8 g5e3 g7f8 e2f3 f8g7 d4f5 c8c3 f5e7 g8f8 e4e5 c3e3 f2e3\n"
         "an a0 1 0 0 16 d6e5 3\n"
         "ln 1 16 -51 0 0 0 E d6e5 e7g6 h7g6 f3a8 d7e8 g1h1 b6b3 a8a5 b3a4 a5c5 f8g8 d1d8 g8h7 c5e5 e8d7\n"
         "ln 2 16 -51 0 0 0 E f6e8 f3a8 f8e7 e5d6 e8d6 g1h1 g7b2 c1c2 b2e5 a8d5 f7f6 d5c5 b6c5 c2c5 d6b7 c5c4\n"
         "ln 3 16 -359 0 0 0 E f8e7 e5f6 g7f6 f3a8 b6e3 g1h1 e3b3 d1f1 b3a4 c1c7 a4a2 a8e4 f6e5 e4h4 e7e8 h4h7 e8e7\n"
         "an a1 1 0 0 16 d7g4 1\n"
         "ln 1 16 -781 0 0 0 E d7g4 f3a8 f6e8 e7d5 b6b3 h3g4 g7e5 a8c8 e5g3 c8c4 b3b2 c4f1 b2b8 f1f3 g3h2 g1h1\n"
         "an a2 1 1 0 14 f3a8 2\n"
         "ln 1 14 868 0 0 0 E f3a8 f6e8 e7d5 b6b3 h3g4 g7h6 d1e1 b3a4 a8d8 f8g7 d8e7 a4g4\n"
         "ln 2 14 668 0 0 0 E h3g4 d6e5 e7c8 e5e4 f3f4 b6b3 d1d8 f6e8 f4e4\n"
         "an a3 1 0 0 6 f6e8 1\n"
         "ln 1 6 -51 0 0 0 E f6e8 f3a8 f8e7 e5d6 e8d6 g1h1 g7b2 c1c2 b2e5 a8d5 f7f6\n"
         "end\n");
        for (int level = 1; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[32];
            std::snprintf(where, sizeof where, "Bg4 b%d", level);
            checkScript(r.script, where);
            CHECK(!hasKeyPrefix(r.script, "ex.hanging"));
            for (const Beat& b : r.script) {
                if (startsWith(b.line.key, "ex.hint.") || startsWith(b.line.key, "ex.reveal.") ||
                    startsWith(b.line.key, "ex.better."))
                    continue;
                for (const auto& a : b.line.args) CHECK(!(a.second.kind == Arg::Kind::Piece && a.second.square == sq("a8")));
            }
        }
    }
    {
        // 2.f5? d5 e4 dxe4 Nc3 Bxf5: two pawns for a drop of three. Levels 4-6 stop the table at d5:
        // "a discovered attack on your pawn" alone would say one; the line and its two points instead.
        const Recorded rc = recorded(
         "rec w f4f5 startpos ; f2f4 c7c6\n"
         "an a0 1 1 0 16 d2d3 3\n"
         "ln 1 16 6 0 0 0 E d2d3 d7d5 e2e4 d5e4 d3e4 d8d1 e1d1 g8f6 e4e5 f6d5 g1f3 g7g6 c1d2 c8f5 b1c3 d5c3 d2c3 e7e6 f1d3 f5d3 c2d3 c6c5\n"
         "ln 2 16 6 0 0 0 E e2e4 d7d5 d2d3 d5e4 d3e4 d8d1 e1d1 g8f6 e4e5 f6d5 g1f3 g7g6 c1d2 c8f5 b1c3 d5c3 d2c3\n"
         "ln 3 16 -3 0 0 0 E g1f3 d7d5 d2d3 g8f6 b1c3 c8f5 h2h3 h7h6 g2g4 f5h7 f4f5 e7e6\n"
         "an a1 1 1 0 16 f4f5 1\n"
         "ln 1 16 -288 0 0 0 E f4f5 d7d5 e2e4 d5e4 b1c3 c8f5 g2g4 f5e6 d1e2 b8d7 g4g5 h7h6 c3e4 h6g5 e4g5 e6d5 g1f3 e7e6 d2d4\n"
         "an a2 1 0 0 14 d7d5 2\n"
         "ln 1 14 278 0 0 0 E d7d5 e2e4 d5e4 b1c3 c8f5 g2g4 f5g6 d1e2 b8a6 c3e4\n"
         "ln 2 14 178 0 0 0 E g8f6 d2d3 d7d5 g2g4 g7g6 g4g5 f6h5 f1h3 c8f5 h3f5 g6f5 e2e4 h5g7 d1e2 f5e4 d3e4\n"
         "an a3 1 1 0 6 e2e4 1\n"
         "ln 1 6 14 0 0 0 E e2e4 d7d5 d2d3\n"
         "end\n");
        for (int level = 4; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[32];
            std::snprintf(where, sizeof where, "f5 b%d", level);
            checkScript(r.script, where);
            CHECK(!hasKeyPrefix(r.script, "ex.discovered"));
        }
    }
    {
        // 13.g4? 13.Bxb5 wins a pawn, but 13.g4's own line gives White material too (e3 fxe3 ...
        // Nxh2 Nxh2 Bxh2+ Kxh2 Qh4+: two points for an attack, the exchange on h2 read where it ends).
        // "It wins material" would not tell the moves apart: the hint names the bishop, no more.
        const Recorded rc = recorded(
         "rec w g2g4 startpos ; c2c3 d7d5 g1f3 b8c6 e2e3 g8f6 d1a4 c8d7 a4c2 e7e5 d2d4 e5e4 f3d2 a7a5 c3c4 b7b5 c4d5 c6b4 c2d1 c7c5 b1c3 c5d4 e3d4 b4d5\n"
         "an a0 1 1 0 16 f1b5 3\n"
         "ln 1 16 7 0 0 0 E f1b5 f8b4 b5d7 d8d7 c3d5 d7d5 e1g1 e8g8 d2b1 a8b8 b1c3 b4c3 b2c3 d5c4 d1c2\n"
         "ln 2 16 -12 0 0 0 E c3d5 f6d5 d1b3 d5c7 f1e2 a5a4 b3g3 a8a6 d4d5 e4e3 g3e3 f8e7 e3d4 e8g8 d2e4 d7f5 e1g1 d8d5 d4d5 c7d5 e2b5\n"
         "ln 3 16 -30 0 0 0 E c3e4 f6e4 d2e4 f8b4 c1d2 e8g8 f1d3 d5f4 d2b4 a5b4 e1g1 b4b3 a2a3 b5b4 d1d2 f4d3 d2d3 b4a3 d3b3 d7e6 b3c2 a3a2 e4c5 d8d4\n"
         "an a1 1 1 0 16 g2g4 1\n"
         "ln 1 16 -215 0 0 0 E g2g4 e4e3 f2e3 a8c8 f1d3 f6g4 e1g1 d5c3 b2c3 f8d6 d2f3 g4h2 f3h2 d6h2 g1h2 d8h4 h2g1 c8c3 d1f3\n"
         "an a2 1 0 0 14 e4e3 2\n"
         "ln 1 14 224 0 0 0 E e4e3 f2e3 f8b4 d1f3 d5c3 g4g5 a8a6 b2c3 b4c3 a1b1 e8g8 f1b5 a6b6 b5d7 b6b1 e1g1\n"
         "ln 2 14 216 0 0 0 E d5c3 b2c3 e4e3 f2e3 f6g4 d1f3 a8a6 h2h3 a6f6 f3b7 f6b6 b7f3 g4e3 f3e3 b6e6 d2e4\n"
         "an a3 1 1 0 6 f1b5 1\n"
         "ln 1 6 7 0 0 0 E f1b5 f8b4 b5d7 d8d7 c3d5 d7d5\n"
         "end\n");
        for (int level = 1; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[32];
            std::snprintf(where, sizeof where, "g4 b%d", level);
            checkScript(r.script, where);
            CHECK(!hasKeyPrefix(r.script, "ex.hint.gain"));
        }
    }
    {
        // 14.Nb1? hxg4 Rc3 f5 Qc2 Nd4 Rxc7+ Qxc7 Qxc7+ Kxc7: the move's own line loses the exchange
        // (the refutation only two plies past what it reads): never "e3 keeps a better position".
        const Recorded rc = recorded(
         "rec w c3b1 startpos ; c2c4 e7e5 b2b3 g8f6 c1b2 b8c6 b1c3 d7d5 c4d5 f6d5 g1f3 c8f5 h2h3 e5e4 g2g4 f5g6 f3h2 h7h5 a1c1 d8d7 f1g2 d5f4 g2e4 e8c8 e4g6 f4g6\n"
         "an a0 1 1 0 16 e2e3 3\n"
         "ln 1 16 -357 0 0 0 E e2e3 c6b4 d2d4 h5g4 a2a3 h8h3 a3b4 f8d6 e1d2 d6h2 d2c2 h2d6 h1g1 f7f5 b4b5 c8b8 d1e2 g6h4\n"
         "ln 2 16 -381 0 0 0 E g4h5 h8h5 h2f3 g6f4 h3h4 d8e8 e1f1 d7g4 h1h2 f8d6 e2e3 f4d3 h2g2 g4f5 c1b1 g7g6 a2a3 c6e5\n"
         "ln 3 16 -409 0 0 0 E f2f3 f8e7 c3e4 h5g4 h3g4 e7d6 b2g7 h8h2 h1h2 d6h2 g7f6 d8e8 d1c2 g6e5 f6e5 h2e5 e4c5 d7e7 c2f5 c8b8 e2e3\n"
         "an a1 1 1 0 16 c3b1 1\n"
         "ln 1 16 -626 0 0 0 E c3b1 h5g4 c1c3 f7f5 d1c2 c6d4 c3c7 d7c7 c2c7 c8c7 h3g4 c7b8 b2d4 d8d4 g4f5 g6f4\n"
         "an a2 1 0 0 14 h5g4 2\n"
         "ln 1 14 642 0 0 0 E h5g4 c1c3 g6f4 h3g4 f8d6 e2e3 f4d5 f2f4 d5c3 b2c3 f7f6 d1f3 d6b4 h2f1 h8h1 f3h1\n"
         "ln 2 14 470 0 0 0 E d8e8 c1c4 b7b5 c4c2 c6b4 c2c3 g6f4 e1f1 b4d5\n"
         "an a3 1 1 0 6 e2e3 1\n"
         "ln 1 6 -357 0 0 0 E e2e3 c6b4 d2d4 h5g4 h2g4 f7f5\n"
         "end\n");
        for (int level = 4; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[32];
            std::snprintf(where, sizeof where, "Nb1 b%d", level);
            checkScript(r.script, where);
            CHECK(r.verdict.exType != ExType::Positional);
            CHECK(!hasKeyPrefix(r.script, "ex.positional"));
        }
    }
    {
        // 16...Bb7? Rxg4: the pawn falls after 16...Nb4 too (... Rxg4 e5 Kc3 exd4 exd4 Rxd4 Rxd4
        // Bxd4 Kxd4, the exchange on d4 read where it ends): never "I can win your pawn" as the
        // reason, at any level.
        const Recorded rc = recorded(
         "rec b c6b7 startpos ; c2c4 g8f6 b1c3 d7d5 d1a4 c8d7 a4b3 d5d4 c3d5 e7e6 d5f6 d8f6 b3b7 d7c6 b7c7 g7g5 c7c8 f6d8 c8d8 e8d8 h2h4 g5g4 e2e3 f8g7 h4h5 d4e3 d2e3 b8a6 h5h6 g7e5 h1h4\n"
         "an a0 1 0 0 16 a6b4 3\n"
         "ln 1 16 -60 0 0 0 E a6b4 e1d2 d8e7 g1e2 a8d8 e2d4 e5f6 h4g4 e6e5 d2c3 e5d4 e3d4 d8d4 g4d4 f6d4 c3d4 b4c2 d4d3\n"
         "ln 2 16 -65 0 0 0 E f7f5 g1e2 a6b4 e1d2 e5f6 h4h1 c6e4 e2d4 d8e7 f2f3 e4b7 d2e2 h8d8 f3g4 f5g4 c1d2 b4c6 d2c3 c6d4 e3d4 f6d4 h1h4 d4c3\n"
         "ln 3 16 -69 0 0 0 E h8g8 g1e2 a6b4 e1d2 c6e4 e2d4 e5d4 e3d4 b4c2 a1b1 c2a3 b2a3 e4b1 d2c3 a8b8 c1f4\n"
         "an a1 1 0 0 16 c6b7 1\n"
         "ln 1 16 -239 0 0 0 E c6b7 h4g4 a6b4 e1e2 f7f5 g4d4 d8e7 d4d2 e5f6 g1f3 a7a5 f3d4 a5a4 f2f3 h8g8 e2f2 g8g6 f2g1 g6h6 a2a3\n"
         "an a2 1 1 0 14 h4g4 2\n"
         "ln 1 14 234 0 0 0 E h4g4 a6b4 e1e2 f7f5 g4d4 e5d4 e3d4 h8g8 c1d2 b4c2 d2a5 d8e7 a1d1 a8c8\n"
         "ln 2 14 210 0 0 0 E c1d2 f7f5 a1d1 d8e7 g1e2 a6c5 d2b4 h8c8 e2c1 e7f6 b4c5 c8c5 c1d3 c5c7 d3e5 f6e5 b2b4 b7e4 h4h5 a8b8 b4b5 e5f6 d1d4\n"
         "an a3 1 0 0 6 a6b4 1\n"
         "ln 1 6 -60 0 0 0 E a6b4 e1d2 d8e7 g1e2 a8d8 e2d4\n"
         "end\n");
        for (int level = 1; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[32];
            std::snprintf(where, sizeof where, "Bb7 b%d", level);
            checkScript(r.script, where);
            for (const Beat& b : r.script) {
                if (startsWith(b.line.key, "ex.hint.") || startsWith(b.line.key, "ex.reveal.") ||
                    startsWith(b.line.key, "ex.better."))
                    continue;
                for (const auto& a : b.line.args) CHECK(!(a.second.kind == Arg::Kind::Piece && a.second.square == sq("g4")));
            }
        }
    }
    {
        // Two knights against a pawn: 65...Kh8? allows a mate in 8, too long for levels 1-2 to show.
        // "Your pawn could run to h1" would hide the mate: no endgame reason there, the mate from
        // level 3.
        const Recorded rc = recorded(
         "rec b g7h8 startpos ; d2d4 g8f6 e2e3 e7e6 c2c4 f8b4 c1d2 a7a5 g1f3 f6e4 h2h3 c7c5 d4c5 e4d2 f3d2 a5a4 h3h4 b4c5 g2g3 d8b6 d1c1 b8c6 c1c2 c6b4 c2c3 d7d5 f1h3 d5d4 c3c1 b4a2 h3f5 a8a7 f5e6 h8g8 a1a2 b6c6 e6d5 c6h6 b2b4 a4b3 a2a3 d4e3 a3a7 c5a7 d5f3 e3f2 e1e2 h6e3 e2f1 c8f5 c1c3 g7g5 f3e2 g5h4 c4c5 e3c3 e2b5 e8f8 h1h2 f5g6 b1c3 b3b2 h2h4 f8g7 d2e4 b2b1n c3b1 g8c8 b1d2 f7f5 e4f2 a7c5 h4f4 c5e3 f1e1 c8c2 d2f1 e3f4 b5d7 f4e5 g3g4 f5g4 f2g4 e5a1 d7a4 c2c3 g4e5 g6h5 e1d2 g7h8 f1h2 c3c7 a4d1 a1c3 d2d3 h5d1 e5c4 c3g7 c4d6 g7d4 d6f5 d4a1 d3e3 c7e7 f5e7 d1c2 h2f3 c2e4 e3e4 a1b2 f3d4 b2c3 e4d5 c3d2 d4b5 d2c3 b5c3 h8g7 c3e2 b7b5 e2g3 g7f7 e7c8 f7g7 c8a7 g7f7 a7b5 f7g7 d5d6\n"
         "an a0 1 0 0 16 h7h5 3\n"
         "ln 1 16 -86 0 0 0 E h7h5 d6e7 g7g6 e7e6 g6g5 e6e5 g5g4 g3f1 g4f3 b5d4 f3f2 f1h2 f2e3 d4f5 e3d3 f5h4 d3c4 h2f1 c4c5 f1e3 c5b6 e5d5 b6b7 e3c4 b7c7 d5c5 c7d7 c4e5 d7e6\n"
         "ln 2 16 -97 0 0 0 E g7g6 d6e5 h7h5 b5d6 g6g5 g3f1 g5g4 d6f5 g4f3 f5d4 f3f2 f1h2 f2e3 d4f5 e3d3 f5h4 d3c4 h2f3 c4c3\n"
         "ln 3 16 -119 0 0 0 E g7f6 b5d4 f6g5 d6e5 h7h5 g3f1 g5g6 d4f3 g6f7 f3h4 f7e7 f1g3 e7d7 e5d5 d7c7 g3e4 c7d7 e4f6 d7c7 d5c5 c7b7 f6d5\n"
         "an a1 1 0 0 16 g7h8 1\n"
         "ln 1 16 0 -8 0 0 E g7h8 g3h5 h8g8 d6e7 h7h6 b5d4 g8h7 e7f7 h7h8 d4f5 h8h7 f5h4 h7h8 h5f6 h6h5 h4g6\n"
         "an a2 1 1 0 14 g3h5 2\n"
         "ln 1 14 0 8 0 0 E g3h5 h8g8 d6e7 h7h6 b5d4 g8h7 e7f7 h7h8 d4f5 h8h7 f5h4 h7h8 h5f6 h6h5 h4g6\n"
         "ln 2 14 399 0 0 0 E g3f5 h7h5 f5h4 h8g8 d6e7 g8h7 b5d4 h7h6 e7f6 h6h7 d4f5 h7g8\n"
         "an a3 1 0 0 6 h7h5 1\n"
         "ln 1 6 -78 0 0 0 E h7h5 d6e7 h5h4 g3f1 g7g6 e7e6\n"
         "end\n");
        for (int level = 1; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[32];
            std::snprintf(where, sizeof where, "Kh8 b%d", level);
            checkScript(r.script, where);
            CHECK(r.verdict.exType != ExType::Endgame);
            CHECK(!hasKeyPrefix(r.script, "ex.push_passed"));
            if (level >= 3) CHECK_EQ(r.verdict.exType, ExType::MateAllowed);
        }
    }
}

TEST(coach_review_human_context_cases) {
    // Blunders from real games (Lichess puzzles, CC0: the move a player blundered into each, coach_audit
    // record --puzzles): reasons the audit found wrong, each fixed.
    {
        // Puzzle imBYj: 17...Nxd5? fxg7+ (a discovered check) Kxc7 gxh8=Q. Without the demonstration
        // (levels 4-6), "Nxd5 allows the discovered attack fxg7+" would leave out the rook: the
        // material reason says the line and the seven points.
        const Recorded rc = recorded(
         "rec b e7d5 1rbk1b1r/1pNpn1pp/p4P2/q1pN2B1/4P3/8/P1P1KPPP/2R2B1R b - - 0 17 ;\n"
         "tag puzzle imBYj 1443 advancedPawn advantage discoveredAttack discoveredCheck middlegame promotion short\n"
         "an a0 1 0 0 16 g7f6 3\n"
         "ln 1 16 648 0 0 0 E g7f6 g5f6 h8g8 f2f4 g8g6 f6e5 g6c6\n"
         "ln 2 16 -110 0 0 0 E d7d6 f6e7 f8e7 g5e7 d8d7 g2g3 b7b5 f1h3 d7c6 h3c8 h8c8 c7e6 a5a2 e2f3\n"
         "ln 3 16 -380 0 0 0 E e7d5 f6g7 d8c7 g7h8q d5c3 e2f3 c5c4 h2h4 d7d5 g5d8 c7d8 h8f8 d8d7 g2g3 d7c6 f8h6 c6c5 f3g2 c3e4\n"
         "an a2 1 1 0 14 f6g7 2\n"
         "ln 1 14 378 0 0 0 E f6g7 d8c7 g7h8q d5c3 e2f3 c5c4 h2h4 d7d5 g5d8 c7d8 h8f8 d8d7 g2g3 d7c6 f8h6 c6c5\n"
         "ln 2 14 -557 0 0 0 E c7d5 d8e8 g5d2 a5a4 e2d1 b7b5 f6g7 f8g7\n"
         "an a3 1 0 0 6 g7f6 1\n"
         "ln 1 6 648 0 0 0 E g7f6 g5f6 h8g8 e2e3 g8g6 d5e7 f8e7\n"
         "end\n");
        for (int level = 1; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[32];
            std::snprintf(where, sizeof where, "Nxd5 b%d", level);
            checkScript(r.script, where);
            if (level >= 4) CHECK_EQ(r.verdict.exType, ExType::Material);
        }
    }
    {
        // Puzzle kJcKa: 10.g3? Bxg3 Qe2 Nxf2 Bd4 Nxh1+. The rook falls after 10.hxg4 Qxh1 too (for the
        // knight): never "now your rook can't be saved" or the rook named as the loss, at any level
        // (a demonstration of the line still takes it).
        const Recorded rc = recorded(
         "rec w g2g3 r1b1k2B/ppp2p1p/3b4/3p4/4p1nq/1P2P2P/P1PP1PP1/RN1QKB1R w KQq - 1 10 ;\n"
         "tag puzzle kJcKa 2032 advantage attackingF2F7 middlegame short\n"
         "an a0 1 1 0 16 h3g4 3\n"
         "ln 1 16 555 0 0 0 E h3g4 h4h1 b1c3 c7c6 d2d3 h1h4 g2g3 h4h6 h8d4 h6g5 d3e4 c8g4 d1d3 c6c5 d4h8\n"
         "ln 2 16 270 0 0 0 E f1b5 c7c6 h1f1 g4h2 b5e2 h2f1 e2f1 f7f6 c2c4 e8e7 c4d5 c6d5 d1e2 a7a6\n"
         "ln 3 16 233 0 0 0 E d1e2 g4f2 e2f2 d6g3 f2g3 h4g3 e1d1 c7c5 b1c3 g3d6 c3b5 d6g3 a2a4 a7a6 b5c3 g3d6 f1e2 c8d7 h1f1\n"
         "an a1 1 1 0 16 g2g3 1\n"
         "ln 1 16 -350 0 0 0 E g2g3 d6g3 d1e2 g4f2 h8d4 f2h1 e1d1 g3d6 d1c1 c8d7 a2a4 c7c5 d4b2 d7c6 b1c3 h1g3 e2f2 a7a6 a4a5\n"
         "an a2 1 0 0 14 d6g3 2\n"
         "ln 1 14 341 0 0 0 E d6g3 d1e2 g4f2 h8d4 f2h1 e1d1 g3d6 d1c1 c8d7 a2a4 e8c8\n"
         "ln 2 14 -658 0 0 0 E h4h6 d1e2 f7f6 b1c3 g4e5 c3d5 e5f3 e2f3\n"
         "an a3 1 1 0 6 h3g4 1\n"
         "ln 1 6 555 0 0 0 E h3g4 h4h1 b1c3 c7c6 d2d3 h1h4\n"
         "end\n");
        for (int level = 1; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[32];
            std::snprintf(where, sizeof where, "g3 b%d", level);
            checkScript(r.script, where);
            for (const Beat& b : r.script) {
                if (b.kind != BeatKind::Say || startsWith(b.line.key, "ex.hint.") ||
                    startsWith(b.line.key, "ex.reveal.") || startsWith(b.line.key, "ex.better."))
                    continue;
                for (const auto& a : b.line.args) CHECK(!(a.second.kind == Arg::Kind::Piece && a.second.square == sq("h1")));
            }
        }
    }
    {
        // Puzzle Ircl5: 42...Ng3? gxh7. Qxh7 (or Kxh7) takes the pawn back: never "if your knight
        // takes back, my pawn takes your rook", about Nxf5 hxg8=Q#.
        const Recorded rc = recorded(
         "rec b h5g3 6rk/1p2q2r/1p1p2P1/1Pp1pQ1n/2P2p1P/P2P1P1K/8/R5R1 b - - 0 42 ;\n"
         "tag puzzle Ircl5 1967 advancedPawn equality middlegame short\n"
         "an a0 1 0 0 16 h7h6 3\n"
         "ln 1 16 611 0 0 0 E h7h6 f5f7 e7f6 g1g5 g8g6 f7f6 h5f6 a1g1 g6g5 g1g5 f6h7 g5g1 h7f8\n"
         "ln 2 16 472 0 0 0 E g8f8 f5g5 e7g5 h4g5 h7g7 g1h1 h5g3 h3g4 g3h1 a1h1 h8g8 h1h6 d6d5 c4d5 f8d8 g4f5 d8d5\n"
         "ln 3 16 437 0 0 0 E e7d7 f5d7 h7d7 g1g5 h5g3 h4h5 h8g7 h3h4 g7h6 a3a4 g8f8\n"
         "an a1 1 0 0 16 h5g3 1\n"
         "ln 1 16 22 0 0 0 E h5g3 g6h7 g8g7 f5c8 h8h7 h3h2 e7f6 a1a2 f6g6 a2d2 g6h5 d2f2 h5h4 c8h3 h4h3 h2h3 h7g8 f2a2\n"
         "an a2 1 1 0 14 g6h7 2\n"
         "ln 1 14 -20 0 0 0 E g6h7 g8g7 f5c8 h8h7 h3h2 e7f6 c8h3 f6g6 a1d1 g6h5 h2g2 h7h8 h3c8 g7g8 c8h3 g3e2 g2h1 e2g1 d1g1 g8g1 h1g1 h5g6 h3g2\n"
         "ln 2 14 -942 0 0 0 E f5g4 g8g6 g1g3 e7h4 h3g2 h4g3 g4g3 g6g3 g2f2 h7h2 f2f1 g3f3 f1e1 f3g3 e1f1 h2h3\n"
         "an a3 1 0 0 6 h7h6 1\n"
         "ln 1 6 611 0 0 0 E h7h6 f5f7 e7f6 a3a4 h6g6 g1g6 g8g6 f7f6 h5f6\n"
         "end\n");
        for (int level = 1; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[32];
            std::snprintf(where, sizeof where, "Ng3 b%d", level);
            checkScript(r.script, where);
            CHECK(!hasKeyPrefix(r.script, "ex.retake"));
        }
    }
    {
        // Puzzle TxYos: 51...h3? Rxa6 Kd8 Ra8+ Ke7 Rxg8. The king has e7 when the check comes: the
        // rook falls to a skewer, never "your king has no escape square" (no back-rank reason).
        const Recorded rc = recorded(
         "rec b h4h3 2k3r1/8/pKP5/6p1/R4p1p/8/8/8 b - - 1 51 ;\n"
         "tag puzzle TxYos 1487 crushing endgame exposedKing long rookEndgame skewer\n"
         "an a0 1 0 0 16 c8d8 3\n"
         "ln 1 16 425 0 0 0 E c8d8 b6b7 d8e7 c6c7 h4h3 a4a2 g5g4 a2e2 e7f6 e2d2 g4g3 d2d6 f6e5 d6d8 g8g7 d8e8 e5f5 e8f8 f5e4\n"
         "ln 2 16 0 0 0 0 E g8g6 a4a6 c8d8 b6b7 g6g7 b7b6\n"
         "ln 3 16 0 0 0 0 E c8b8 a4a6 g8g6 a6a5 h4h3 a5f5 g6g8 f5a5 g8g6\n"
         "an a1 1 0 0 16 h4h3 1\n"
         "ln 1 16 -1514 0 0 0 E h4h3 a4a6 c8d8 a6a8 d8e7 a8g8 e7f6 c6c7 h3h2 g8h8 h2h1q h8h1 f6e7 h1h2 e7d7 h2h8 d7e7 c7c8q\n"
         "an a2 1 1 0 14 a4a6 2\n"
         "ln 1 14 1519 0 0 0 E a4a6 c8d8 a6a8 d8e7 a8g8 e7f6 c6c7 h3h2 g8h8 h2h1q h8h1 f6e7 h1h8 e7f6 c7c8q f6e5 c8c5 e5f6 h8h1\n"
         "ln 2 14 -299 0 0 0 E a4a3 h3h2 a3h3 g5g4 h3h7 g4g3 h7c7 c8d8\n"
         "an a3 1 0 0 6 c8d8 1\n"
         "ln 1 6 349 0 0 0 E c8d8 a4a5 d8e7 c6c7 e7f6 a5a4\n"
         "end\n");
        for (int level = 1; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[32];
            std::snprintf(where, sizeof where, "h3 b%d", level);
            checkScript(r.script, where);
            CHECK(r.verdict.exType != ExType::BackRank);
            CHECK(!hasKeyPrefix(r.script, "ex.back_rank"));
        }
    }
    {
        // Game tolm2f3k (Lichess, 2013): 18...h4? Na5. Two pawns still shelter the king on b8, it
        // stands on its first rank and no checks come early: never "your king is exposed".
        const Recorded rc = recorded(
         "rec b h5h4 startpos ; d2d4 d7d5 g1f3 g8f6 e2e3 c8g4 f1e2 b8c6 e1g1 d8d7 b2b3 e8c8 c1b2 e7e6 a2a3 f8d6 b1d2 h7h5 f3e5 d7e8 f2f4 g4e2 d1e2 e8e7 b3b4 f6d7 e5c6 b7c6 e2a6 c8b8 a6c6 d7b6 c2c4 d5c4 d2c4\n"
         "tag game https://lichess.org/tolm2f3k 1732/1863 300+0 ply 36 drop 22.7\n"
         "an a0 1 0 0 16 b6c4 3\n"
         "ln 1 16 -307 0 0 0 E b6c4 c6c4 e7d7 a1c1 h5h4 h2h3 h8h5 e3e4 h5b5 g1h1 f7f5 d4d5 e6d5 e4d5 d8e8 f1e1 e8e1 c1e1 a7a5 b2c3\n"
         "ln 2 16 -524 0 0 0 E e7e8 c4a5 e8c6 a5c6 b8b7 c6d8 h8d8 a1e1 f7f5 f1f2 b6d5 f2c2\n"
         "ln 3 16 -535 0 0 0 E f7f5 c4a5 b8c8 c6b7 c8d7 a5c6 e7f6 c6d8 h8d8 e3e4 f5e4 b7e4 f6f5 a1e1 d8f8 b2c1 h5h4\n"
         "an a1 1 0 0 16 h5h4 1\n"
         "ln 1 16 -674 0 0 0 E h5h4 c4a5 b8c8 e3e4 h4h3 g2g3 e7e8 c6b7 c8d7 e4e5 d6f8 a1c1 d8c8 f4f5 e6f5 d4d5 h8h5 b7a6 b6d5\n"
         "an a2 1 1 0 14 c4a5 2\n"
         "ln 1 14 674 0 0 0 E c4a5 b8c8 e3e4 h4h3 g2g3 e7e8 c6b7 c8d7 e4e5 d6f8 a1c1 d8c8 f4f5 e6f5 d4d5 h8h5\n"
         "ln 2 14 307 0 0 0 E h2h3 b6c4 c6c4 e7d7 a1c1 f7f6 e3e4 f6f5 e4e5 d6e7 f1d1\n"
         "an a3 1 0 0 6 b6c4 1\n"
         "ln 1 6 -307 0 0 0 E b6c4 c6c4 e7d7 a1c1 h5h4 h2h3\n"
         "end\n");
        for (int level = 1; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[32];
            std::snprintf(where, sizeof where, "h4 b%d", level);
            checkScript(r.script, where);
            CHECK(r.verdict.exType != ExType::KingSafety);
            CHECK(!hasKeyPrefix(r.script, "ex.king_exposed"));
        }
    }
}

TEST(coach_review_blunder_without_a_reliable_reason_is_hinted) {
    // A blunder whose reason the coach cannot give reliably: the verdict and the piece that had the
    // better move, never the move itself while the takeback is offered.
    const Recorded rc = recorded(kKd2Record);
    for (int level = 1; level <= 6; ++level) {
        Reviewer rv;
        const Review r = reviewRecorded(rv, rc, level);
        char where[48];
        std::snprintf(where, sizeof where, "Kd2 hint b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK_EQ(r.verdict.exType, ExType::None);
        CHECK(r.offersTakeback);
        CHECK(hasKey(r.script, "ex.verdict.blunder.b" + std::to_string(level)));
        const Beat* h = beatWithKey(r.script, "ex.hint.b" + std::to_string(level));
        CHECK(h != nullptr);
        if (h && h->line.arg("your")) {
            CHECK_EQ(h->line.arg("your")->piece, King);
            CHECK_EQ(h->line.arg("your")->square, sq("d2"));   // where the king stands now
        }
        CHECK(!hasKeyPrefix(r.script, "ex.better") && !hasKeyPrefix(r.script, "ex.reveal"));
        for (const Beat& b : r.script)
            for (const Gesture& gs : b.gestures) CHECK(gs.path.empty() || gs.path.back() != sq("c2"));
    }
    for (int level : {1, 4}) {
        // Accepted: the hint points at the king, back on c1 (levels 1-2); the better move found is
        // praised as the one the coach had in mind.
        Recorded game = recorded(kKd2Record);
        Reviewer rv;
        CHECK(reviewRecorded(rv, game, level).offersTakeback);
        CHECK(game.g.undo(1));
        const Script hint = rv.takebackAccepted(game.g);
        if (level <= 2) {
            const Beat* hb = beatWithKey(hint, "tb.hint.b1");
            CHECK(hb != nullptr);
            if (hb && hb->line.arg("your")) CHECK_EQ(hb->line.arg("your")->square, sq("c1"));
        }
        game.g.play(game.g.position().parseUCI("c1c2"));
        Review found = reviewOf(rv, game.g, game.a0);
        CHECK(found.isRetry && found.takeback.found);
        CHECK(!found.script.empty());
        if (!found.script.empty())
            CHECK_EQ(found.script.front().line.key, std::string(level <= 2 ? "tb.found.b1" : "tb.found"));
        checkScript(found.script, "Kd2 found");

        // Declined: the better move is said then.
        Reviewer rv2;
        reviewRecorded(rv2, rc, level);
        const Script reveal = rv2.takebackDeclined();
        checkScript(reveal, "Kd2 reveal");
        CHECK_EQ(reveal.size(), size_t(1));
        if (level <= 2) {
            const Beat* b = beatWithKey(reveal, "ex.reveal.b" + std::to_string(level));
            CHECK(b != nullptr);
            if (b) CHECK(b->line.arg("sq") && b->line.arg("sq")->square == sq("c2"));
        } else {
            const Beat* b = beatWithKey(reveal, "ex.better.b" + std::to_string(level));
            CHECK(b != nullptr);
            if (b) CHECK(b->line.arg("best") && b->line.arg("best")->uci == "c1c2");
        }
        CHECK(rv2.takebackDeclined().empty());   // said once
    }
}

namespace {

// A self-play game (depth 16): 19.Be5?? runs into 19...d3, hitting the queen on e2, and the queen
// on d6 gets away; 19.Bxd6 took it for the bishop, and the rook on c4 fell next. 19.Be5 wins and
// loses nothing along its own line.
const char* kBe5Record =
    "rec w f4e5 startpos ; c2c4 c7c5 g2g3 b8c6 d2d3 e7e6 b1c3 h7h6 e2e4 g7g6 c1e3 b7b6 f1g2 g8f6 g1e2 c8b7 d3d4 f6g4 e3f4 c6d4 e2d4 c5d4 c3b5 f8b4 e1f1 d8e7 b5c7 e8d8 d1g4 a8c8 c7b5 c8c4 g4e2 e7c5 b5d6 c5d6\n"
    "an a0 1 1 0 16 f4d6 3\n"
    "ln 1 16 826 0 0 0 E f4d6 b4d6 e2c4 e6e5 a2a3 g6g5 f1e1 d8e7 c4e2 h8c8 e2d3 f7f5 a1d1\n"
    "ln 2 16 591 0 0 0 E e2c4 d6c5 c4c5 b6c5 a2a3 b4a5 b2b4 a5b6 b4c5 b6c5 f4e5\n"
    "ln 3 16 -83 0 0 0 E b2b3 d4d3 e2b2 d6d4 b2d4 c4d4 f4e5 b4c3 e5f6 d8e8 a1d1 h8f8 a2a3 g6g5\n"
    "an a1 1 1 0 16 f4e5 1\n"
    "ln 1 16 -625 0 0 0 E f4e5 d4d3 e5f6 d8c7 e2d1 h8c8 a2a3 b4c5 f6c3 f7f6 a1c1 c7b8 e4e5 f6e5 c3e5 d6e5 c1c4 c8f8\n"
    "an a2 1 0 0 14 d4d3 2\n"
    "ln 1 14 591 0 0 0 E d4d3 e5f6 d8c7 e2d1 h8c8 a2a3 e6e5 a3b4 d6f6 d1d3 c7b8 d3d7\n"
    "ln 2 14 474 0 0 0 E d6c6 f1g1 h8f8 e5g7 f7f5 a2a3 b4e7 h2h4 f5e4 g7f8 e7f8 g1h2\n"
    "an a3 1 1 0 6 f4d6 1\n"
    "ln 1 6 826 0 0 0 E f4d6 b4d6 e2c4 e6e5 a2a3 g6g5\n"
    "end\n";

// A self-play game (depth 16): 18...g5? 19.fxg5 Nh7 20.Nd5 Qxg5 21.Qf3 Bb7 22.Nxc7+ costs the pawn
// on c7; the pawn on g5 that 19.fxg5 takes at once is won back on move 20.
const char* kG5Record =
    "rec b g6g5 startpos ; e2e4 e7e5 g1f3 g8f6 b1c3 f8d6 f1e2 h7h6 d2d4 e5d4 f3d4 d6e5 d4f5 e5c3 b2c3 g7g6 f5g3 d7d6 h2h3 d8e7 c1b2 h6h5 c3c4 h5h4 g3f1 b8c6 a1b1 h8g8 e2d3 b7b6 f1e3 c6e5 f2f4 e5d3 c2d3\n"
    "an a0 1 0 0 16 c8b7 3\n"
    "ln 1 16 -177 0 0 0 E c8b7 d1a4 e7d7 a4a3 f6h5 e1g1 f7f5 e3d5 a7a5 a3b3 b7d5 c4d5 g6g5 f4g5 g8g5 f1f5 g5f5 e4f5\n"
    "ln 2 16 -258 0 0 0 E a7a5 d1c2 c8b7 e1g1 e8c8 b2d4 f6d7 e3d5 e7e6 c2b2 b7c6 f4f5 g6f5 e4f5 e6e8 f1e1 d7e5 d4b6 c6d5 c4d5 c7b6 b2b6 e5f3 g1f1 e8e1 b1e1 f3e1 b6c6 c8b8\n"
    "ln 3 16 -268 0 0 0 E g8f8 e1g1 c8b7 a2a4 a7a5 b2d4 e8c8 d1b3 f6d7 e3d5 b7d5 c4d5 d7c5 b3c4 f7f6 f1c1 c8b7 d4c5 d6c5 c4c5 e7c5 c1c5\n"
    "an a1 1 0 0 16 g6g5 1\n"
    "ln 1 16 -365 0 0 0 E g6g5 f4g5 f6h7 e3d5 e7g5 d1f3 c8b7 d5c7 e8d8 c7a8 g5a5 e1f1 a5a2 f1f2 g8g3 f3e2\n"
    "an a2 1 1 0 14 f4g5 2\n"
    "ln 1 14 390 0 0 0 E f4g5 f6d7 e1g1 e7g5 e3g4 d7e5 g4f6 e8d8 f6g8 c8h3 f1f2 h3g4\n"
    "ln 2 14 214 0 0 0 E h1f1 c8e6 f4f5 e6d7 b2f6 e7f6 e3d5 f6d8 f5f6 g5g4 d5e7 g8h8 h3g4 h4h3 g2h3 h8h3 g4g5\n"
    "an a3 1 0 0 6 c8b7 1\n"
    "ln 1 6 -177 0 0 0 E c8b7 d1a4 e7d7 a4a3 d7e7\n"
    "end\n";

// A self-play game (depth 18): 40...Kc7?? 41.e6+, a discovered check. The after-move search (depth
// 14) answers 41...Qd6 42.Qxd6+ Kxd6 43.exf7, the rook lost; the deeper line for the move answers
// 41...Rf4 and loses less, and the better 40...Ke7 loses a knight too.
const char* kKc7Record =
    "rec b d8c7 startpos ; g1f3 c7c6 c2c4 d7d6 e2e4 e7e5 d2d3 d8a5 c1d2 a5a6 b2b4 c8g4 f1e2 c6c5 b4c5 b8d7 e1g1 d6c5 b1c3 g8f6 a2a4 a6c6 f3g5 h7h6 f2f3 g4h5 g2g4 f8e7 g5h7 h8h7 g4h5 g7g6 a4a5 a7a6 h5g6 h7h8 g6f7 e8f8 c3d5 h8h7 d1e1 h7g7 g1h1 b7b5 f3f4 f6d5 f4e5 d5c7 f1g1 b5b4 e1f2 e7g5 f2g3 c7e6 d2g5 h6g5 a1f1 b4b3 e2g4 b3b2 g3h3 f8e7 g1g2 b2b1q f7f8r e6f4 f8f4 b1b3 f4f5 b3c3 f1b1 a8f8 f5f8 d7f8 h3f3 g7f7 f3g3 e7d8 b1b8\n"
    "an a0 1 0 0 18 d8e7 3\n"
    "ln 1 18 -31 0 0 0 E d8e7 b8b1 f8g6 g4f5 c3e5 g3g5 c6f6 g5g6 f6g6 g2g6 e5f4 b1b7 e7f8\n"
    "ln 2 18 -593 0 0 0 E d8c7 e5e6 f7f4 e6e7 c3a1 g2g1 a1g1 h1g1 c6a4 b8b1 f8g6 g4h5 g6e7 g3g5\n"
    "ln 3 18 -1199 0 0 0 E c6c8 b8c8 d8e7 c8c7 e7d8 c7f7 f8g6 e5e6 c3a1 g2g1 a1e5 f7d7 d8e8 g3e5 g6e5 g4h5 e8f8\n"
    "an a2 1 1 0 14 e5e6 2\n"
    "ln 1 14 760 0 0 0 E e5e6 c6d6 g3d6 c7d6 e6f7 f8d7 f7f8q d7f8 b8f8 d6e7 f8f1 c3d3 g2e2 d3c4\n"
    "ln 2 14 568 0 0 0 E b8c8 c7b7 c8c6 f7f1 g2g1 f1g1 g3g1 b7c6 g1f1 f8d7 e5e6 c3f6 f1f6 d7f6 e6e7 c6d6 h1g2 d6e7 g2f3 f6d7 g4d7 e7d7 f3e2\n"
    "an a3 1 0 0 6 d8e7 1\n"
    "ln 1 6 -31 0 0 0 E d8e7 b8b1 f8g6 g4f5 c3e5 g3g5 c6f6\n"
    "end\n";

// A self-play game (depth 16): 17.Qc2? cxd3 takes a pawn and forks the queen and the knight; the
// bishop behind the pawn then takes the pawn on b3 (17...cxd3 18.Qa2 Bxb3), before 20...dxe2.
const char* kQc2Record =
    "rec w d1c2 startpos ; c2c4 d7d6 b1c3 c7c6 c3e4 e7e5 e2e3 f7f5 e4c3 c8e6 d1c2 g8f6 c2d3 c6c5 a2a4 b8c6 c3d5 g7g6 d3b3 f8g7 d2d3 c6a5 d5f6 g7f6 b3d1 e8g8 g1e2 d6d5 b2b3 f5f4 f2f3 d5c4\n"
    "an a0 1 1 0 16 b3c4 3\n"
    "ln 1 16 -456 0 0 0 E b3c4 f4e3 a1a3 d8b6 e2c3 b6b4 e1e2 a8d8 c3e4 a5c4 d1b3 f6e7 b3a2 d8d4 a3b3 c4d2 b3b4 e6a2 b4d4\n"
    "ln 2 16 -553 0 0 0 E d3c4 d8b6 e3e4 a8d8 d1c2 a5b3 a1b1 b6a5 e1f2 b3c1 c2c1 d8d2 b1b7 f8d8 b7b2 d2d3\n"
    "ln 3 16 -755 0 0 0 E e3f4 c4d3 a1b1 d3e2 f1e2 e5f4 e1g1 f6d4 g1h1 a5c6 c1a3 g6g5\n"
    "an a1 1 1 0 16 d1c2 1\n"
    "ln 1 16 -1010 0 0 0 E d1c2 c4d3 c2a2 e6b3 a2b1 d3e2 f1e2 f6h4 g2g3 h4e7\n"
    "an a2 1 0 0 14 c4d3 2\n"
    "ln 1 14 979 0 0 0 E c4d3 c2a2 a5b3 a1b1 c5c4 c1d2 f6h4 g2g3 f4e3\n"
    "ln 2 14 935 0 0 0 E a5b3 d3c4 b3a1 c2a2 f4e3 c1e3 e5e4 e2f4 f6c3 e1f2 d8d1 f4e6 d1e1 f2g1 e1e3 a2f2 e3f2 g1f2\n"
    "an a3 1 1 0 6 b3c4 1\n"
    "ln 1 6 -456 0 0 0 E b3c4 f4e3 a1a3 d8b6 e2c3 b6b4\n"
    "end\n";

// A self-play game (depth 16): 15...a6? Bxd7+ Kxd7 dxe6+ Kc7 Rxd4 costs ten points, but the queen
// on d4 was lost anyway: 15...Qxd1+ gives it up for a rook and the bishop on b5.
const char* kA6Record =
    "rec b a7a6 startpos ; e2e4 c7c5 b1c3 b8c6 g1f3 g8f6 d2d4 d7d5 c3d5 f6e4 f1c4 e7e6 d5c3 d8a5 e1g1 e4c3 b2c3 c5d4 c3d4 a5c3 d1e2 c3a1 d4d5 c6d4 f3d4 a1d4 c4b5 c8d7 f1d1\n"
    "an a0 1 0 0 16 d4d1 3\n"
    "ln 1 16 298 0 0 0 E d4d1 e2d1 d7b5 d5e6 b5c6 d1h5 e8c8 e6f7 d8d7 c1b2 b7b6 h2h4 c8b7 h5f5 d7d1 g1h2 d1d5 f5g4\n"
    "ln 2 16 0 0 0 0 E d4c5 b5d7 e8d7 d5e6 d7e8 e6f7 e8f7 e2f3 f7e8 c1a3 c5e5 f3b7 a8d8 b7c6 e8f7 c6b7\n"
    "ln 3 16 -188 0 0 0 E d4b4 b5d7 e8d7 d5e6 d7c6 a2a3 b4b5 e2e4 c6b6 a3a4 b5b4 c1e3 b6c7 e3f4 c7b6 e4d3 a7a5 f4d2 b4b2 d2e3 f8c5 e3c5 b6c5\n"
    "an a1 1 0 0 16 a7a6 1\n"
    "ln 1 16 -918 0 0 0 E a7a6 b5d7 e8d7 d5e6 d7c7 d1d4 a8e8 c1f4 c7b6 f4e3 e8e6\n"
    "an a2 1 1 0 14 b5d7 2\n"
    "ln 1 14 916 0 0 0 E b5d7 e8d7 d5e6 d7c7 d1d4 a8e8 c1f4 c7b6 f4e3 f8c5\n"
    "ln 2 14 532 0 0 0 E d1d4 d7b5 c2c4 b5d7 c1b2\n"
    "an a3 1 0 0 6 d4d1 1\n"
    "ln 1 6 186 0 0 0 E d4d1 e2d1 d7b5 d5e6 b5c6 d1h5\n"
    "end\n";

}  // namespace

TEST(coach_review_hint_says_the_better_move_wins_material) {
    // 19.Be5?? with no reason the coach can give: the hint says the bishop had a move that wins
    // material (19.Bxd6 takes the queen), without naming it.
    const Recorded rc = recorded(kBe5Record);
    for (int level = 1; level <= 6; ++level) {
        Reviewer rv;
        const Review r = reviewRecorded(rv, rc, level);
        char where[48];
        std::snprintf(where, sizeof where, "Be5 gain b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK(r.offersTakeback);
        const Beat* h = beatWithKey(r.script, "ex.hint.gain.b" + std::to_string(level));
        CHECK(h != nullptr);
        if (h && h->line.arg("your")) {
            CHECK_EQ(h->line.arg("your")->piece, Bishop);
            CHECK_EQ(h->line.arg("your")->square, sq("e5"));   // the bishop that moved, where it stands
        }
        CHECK(!hasKey(r.script, "ex.hint.b" + std::to_string(level)));
        CHECK(!hasKeyPrefix(r.script, "ex.better") && !hasKeyPrefix(r.script, "ex.reveal"));
    }
    // Kd2: the better move wins nothing either (both lose the rook): the plain hint.
    const Recorded kd2 = recorded(kKd2Record);
    for (int level = 1; level <= 6; ++level) {
        Reviewer rv;
        CHECK(!hasKeyPrefix(reviewRecorded(rv, kd2, level).script, "ex.hint.gain"));
    }
}

TEST(coach_review_level2_mistake_names_what_falls_at_once) {
    // 18...g5? 19.fxg5 takes a pawn at once, won back on move 20; what the move costs is the pawn
    // on c7, seven plies on. Level 2 says a mistake in words only when what falls at once stays
    // lost: "this move lets me win your pawn" with the arrow on fxg5 would point at the wrong pawn.
    const Recorded rc = recorded(kG5Record);
    Reviewer rv;
    const Review r = reviewRecorded(rv, rc, 2);
    checkScript(r.script, "g5 b2");
    CHECK_EQ(r.verdict.cls, MoveClass::Mistake);
    CHECK(!r.verdict.voiced);
    CHECK(!hasKeyPrefix(r.script, "ex.material"));
    // Level 3 says the whole line, as it goes.
    Reviewer rv3;
    const Review r3 = reviewRecorded(rv3, rc, 3);
    checkScript(r3.script, "g5 b3");
    CHECK(hasKeyPrefix(r3.script, "ex.material.b3"));
}

TEST(coach_review_loss_the_better_move_shares_holds_on_the_deeper_line) {
    // 40...Kc7??: the after-move line's defence loses the rook (5 points), but the better move loses
    // a knight as well, and the deeper line for the move loses less: no material reason (nor the
    // discovered check that would come with it), at any level.
    const Recorded rc = recorded(kKc7Record);
    for (int level = 1; level <= 6; ++level) {
        Reviewer rv;
        const Review r = reviewRecorded(rv, rc, level);
        char where[32];
        std::snprintf(where, sizeof where, "Kc7 b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        CHECK(r.verdict.exType != ExType::Material && r.verdict.exType != ExType::Discovered);
        CHECK(!hasKeyPrefix(r.script, "ex.material") && !hasKeyPrefix(r.script, "ex.discovered"));
    }
}

TEST(coach_review_motif_target_behind_the_reply_capture) {
    // 17.Qc2? cxd3: the reply takes a pawn itself; the pawn on b3 the bishop behind it takes next is
    // no more than that. "A discovered attack hits your pawn" would not say what the move costs.
    const Recorded rc = recorded(kQc2Record);
    for (int level = 1; level <= 6; ++level) {
        Reviewer rv;
        const Review r = reviewRecorded(rv, rc, level);
        char where[32];
        std::snprintf(where, sizeof where, "Qc2 b%d", level);
        checkScript(r.script, where);
        CHECK(!hasKeyPrefix(r.script, "ex.discovered"));
        // Levels 3-6 say the line and what it costs.
        if (level >= 3) CHECK(hasKeyPrefix(r.script, ("ex.material.b" + std::to_string(level)).c_str()));
    }
}

TEST(coach_review_material_never_names_a_piece_the_better_move_gives_up) {
    // 15...a6?: the line takes the queen, but so does the better move's ("Qxd1+ Qxd1"): never "now
    // I can win your queen", nor the queen pointed at. Levels 3-6 say the line and its ten points.
    const Recorded rc = recorded(kA6Record);
    for (int level = 1; level <= 6; ++level) {
        Reviewer rv;
        const Review r = reviewRecorded(rv, rc, level);
        char where[32];
        std::snprintf(where, sizeof where, "a6 b%d", level);
        checkScript(r.script, where);
        for (const Beat& b : r.script) {
            if (b.line.key.rfind("ex.material.", 0) != 0) continue;
            for (const auto& a : b.line.args) CHECK(!(a.second.kind == Arg::Kind::Piece && a.second.piece == Queen));
        }
        if (level >= 3) CHECK(hasKeyPrefix(r.script, ("ex.material.b" + std::to_string(level)).c_str()));
    }
}

TEST(coach_review_recorded_reasons_hold) {
    // Recorded moves (tools/coach_audit corpora, depth 18) the coach once explained wrong.
    struct Case {
        const char* what;
        const char* rec;
        ExType want, never;   // want None: any reason, never None: no type excluded
        int pts;              // the points the cause names (0: not checked)
    };
    const Case cases[] = {
        // 4...Qg4?? 5.Qxg4: the queen itself falls, never "a pin" of a smaller piece behind it.
        {"mat_all_580",
         "rec b h4g4 startpos ; e2e4 e7e5 f1c4 f8c5 b1c3 d8h4 d1f3\n"
         "an a0 1 0 0 18 g8f6 3\n"
         "ln 1 18 -44 0 0 0 E g8f6 h2h3 b8c6 g1e2 e8g8 g2g3 h4h5 g3g4 h5h4 d2d3 d7d6 c1d2 c6b4 e1c1 h4f2 f3f2 c5f2 d1f1\n"
         "ln 2 18 -163 0 0 0 E h4f6 d2d4 f6f3 g1f3 e5d4 c3d5 c5b6 c1f4 d7d6 e4e5 b8c6 d5b6 a7b6 e5d6 c7d6 e1c1 c8g4 h1e1 e8d7 f3d4 g4d1\n"
         "ln 3 18 -206 0 0 0 E e8d8 f3f7 h4f6 g1f3 f6f7 c4f7 b8c6 d2d3 g8f6 e1g1 d7d6 c3a4 c5b6 f7b3 b6a5 a4c3 c8g4 f3g5 d8d7 f2f3 g4h5 c1e3 h7h6\n"
         "an a1 1 0 0 18 h4g4 1\n"
         "ln 1 18 -996 0 0 0 E h4g4 f3g4 g8e7 g4g7 h8f8 g7e5 b8c6 e5c5 d7d6 c5e3\n"
         "an a2 1 1 0 14 f3g4 2\n"
         "ln 1 14 1006 0 0 0 E f3g4 g8e7 g4g7 h8f8 c3b5 b8a6 d2d4 c5b4 c2c3\n"
         "ln 2 14 936 0 0 0 E c4f7 e8f8 f3g4 g8f6 g4f3 d7d5 f7d5 c7c6 c3a4 c8g4\n"
         "an a3 1 0 0 6 g8f6 1\n"
         "ln 1 6 -44 0 0 0 E g8f6 h2h3 b8c6 g1e2 e8g8 g2g3\n"
         "end\n",
         ExType::Hanging, ExType::Pin, 9},
        // 22...Rxg2+?? gives up the rook, but so does the best line (22...e5 23.dxe5 ... Rxg2):
        // no exchange reason, only the verdict and a hint.
        {"mat_all_14",
         "rec b g8g2 startpos ; g1f3 d7d5 e2e3 c7c6 c2c4 e7e6 b2b3 h7h6 c1b2 b7b6 c4d5 c6d5 f3e5 a7a6 d1f3 g8f6 f3g3 c8b7 f1d3 b8d7 h2h4 d7e5 b2e5 f6d7 e5d4 d7c5 d4c5 f8c5 b3b4 c5d6 g3g7 e8e7 f2f4 a8c8 e1g1 h8g8 g7d4 c8c4 d3c4 e7f8 c4d3 d6e7 f4f5\n"
         "an a0 1 0 0 18 e6e5 3\n"
         "ln 1 18 -262 0 0 0 E e6e5 d4e5 e7d6 e5d4 d8c7 d4f6 d6h2 g1f2 g8g2 f2g2 c7g3 g2h1 d5d4 e3e4 g3h3 f6h8 f8e7 f5f6 e7d7 f1f2 b7c6\n"
         "ln 2 18 -369 0 0 0 E e7f6 d4f4 e6e5 f4h2 e5e4 d3e2 f6a1 b1c3 a1b2 h2f4 d5d4 c3a4 d4d3 e2h5 b2f6 f1c1 b7a8\n"
         "ln 3 18 -696 0 0 0 E b7c8 f5e6 c8e6 d3f5 e6f5 f1f5 d8c8 d4f4 c8c1 f4f1 c1f1 f5f1\n"
         "an a1 1 0 0 18 g8g2 1\n"
         "ln 1 18 -806 0 0 0 E g8g2 g1g2 e6e5 d4e5 e7f6 e5f4 f8g7 f1f3 h6h5 b1c3 g7h7 g2f1 d5d4\n"
         "an a2 1 1 0 14 g1g2 2\n"
         "ln 1 14 796 0 0 0 E g1g2 e6e5 d4e5 e7f6 e5f4 f8e7 g2h3 d8d6 f4d6 e7d6 b1a3 f6e5 f1g1 b7c8 a3b5 a6b5 a1f1\n"
         "ln 2 14 258 0 0 0 E g1h1 e6e5 d4e5 e7f6 e5f4 g2g7 b1c3 d5d4 d3e4 f6e5 f4h6 d4c3 e4b7 d8d2\n"
         "an a3 1 0 0 6 e6e5 1\n"
         "ln 1 6 -262 0 0 0 E e6e5 d4e5 e7d6 e5d4 d8c7 d4f6\n"
         "end\n",
         ExType::None, ExType::Exchange, 0},
        // 7.Bh6?? Nxh6: the bishop hangs, even where the engine's line takes a pawn back for a while
        // (8.Nbd2 e5 9.dxe5, the pawn won back by ...Nxe5 Nxe5 Qxe5).
        {"mat_all_156",
         "rec w f4h6 startpos ; d2d4 e7e6 c2c3 c7c6 g1f3 d7d5 c1f4 b8d7 e2e3 f8e7 f1d3 a7a5\n"
         "an a0 1 1 0 18 b1d2 3\n"
         "ln 1 18 93 0 0 0 E b1d2 g8f6 h2h4 d8b6 a1b1 c6c5 d1e2 f6h5 f4h2 h5f6 f3e5 e8g8\n"
         "ln 2 18 93 0 0 0 E h2h4 d8b6 b1d2 g8f6 a1b1 c6c5 h4h5 e8g8 f3e5 c5d4 e3d4 d7e5 d4e5 f6d7 h5h6 g7g6 e1g1 f7f6 e5f6 d7f6 d1e2\n"
         "ln 3 18 87 0 0 0 E a2a4 g8f6 h2h4 b7b6 d1e2 c8b7 b1d2 d8c8 h4h5 h7h6 f3e5 e8g8 d2f3 c6c5\n"
         "an a1 1 1 0 18 f4h6 1\n"
         "ln 1 18 -779 0 0 0 E f4h6 g8h6 b1d2 d8c7 g2g4 c6c5 g4g5 h6f5 h2h4 f5d6 h4h5 c5c4 d3c2 b7b5 a2a4\n"
         "an a2 1 0 0 14 g8h6 2\n"
         "ln 1 14 782 0 0 0 E g8h6 b1d2 e6e5 d4e5 d8c7 d3c2 d7e5 f3e5 c7e5 e3e4 e5e6 d1e2 e8g8 e4d5 c6d5 e2e6 f7e6 e1c1\n"
         "ln 2 14 674 0 0 0 E g7h6 b1d2 g8f6 e1g1 d8c7 d1e2 h8g8 c3c4 d5c4 d2c4 g8g7 e3e4 b7b5 c4e5 c8b7\n"
         "an a3 1 1 0 6 h2h4 1\n"
         "ln 1 6 89 0 0 0 E h2h4 g8f6 b1d2 d8b6 a1b1 c6c5\n"
         "end\n",
         ExType::Hanging, ExType::None, 3},
        // 11.Qg2? Bxe5 dxe5 Qxe5 costs a pawn, but so does the better 11.Qh5+ (it wins one back on
        // c7 later): no "miscount on e5".
        {"m_308_23",
         "rec w f3g2 startpos ; c2c3 g8f6 e2e3 a7a6 g1f3 b7b6 e3e4 e7e6 f3e5 f6e4 d1e2 f7f5 g2g3 d8f6 d2d4 d7d5 e2f3 f8d6 f1h3 a6a5\n"
         "an a0 1 1 0 18 f3h5 3\n"
         "ln 1 18 -265 0 0 0 E f3h5 g7g6 h5h6 d6e5 d4e5 f6e5 c1f4 e5f6 f4c7 c8a6 b1d2 b8d7 d2e4 f5e4 c7f4 d7c5 e1c1 c5d3 c1b1 d3f4 g3f4 a6d3 b1a1 e8c8\n"
         "ln 2 18 -293 0 0 0 E f3d1 b8d7 f2f4 c7c5 e1g1 f6e7 b1d2 e8g8 d2e4 d5e4 c1e3 d7f6 d4c5 b6c5\n"
         "ln 3 18 -297 0 0 0 E h3f1 e8g8 f3e3 d6e5 d4e5 f6e5 f2f3 e4g5 e1f2 e5e3 c1e3 g5f7 c3c4 c7c5 c4d5 e6d5 e3f4\n"
         "an a1 1 1 0 18 f3g2 1\n"
         "ln 1 18 -489 0 0 0 E f3g2 d6e5 d4e5 f6e5 e1g1 e5f6 f2f3 e4c5 g2c2 c8a6 f1d1 b8d7 b1a3 e8g8 c1e3 c7c6 c3c4 h7h6 h3f1 f6f7 f3f4 c5e4 c4d5 c6d5\n"
         "an a2 1 0 0 14 d6e5 2\n"
         "ln 1 14 458 0 0 0 E d6e5 d4e5 f6e5 c1e3 e8g8 b1d2 e5f6 g3g4\n"
         "ln 2 14 303 0 0 0 E c8a6 f2f3 e4g5 c1g5 f6g5 f3f4 g5h6 e5f3 e8g8 b1d2 b8d7 e1c1 d7f6 h1e1\n"
         "an a3 1 1 0 6 f3h5 1\n"
         "ln 1 6 -265 0 0 0 E f3h5 g7g6 h5h6 d6e5 d4e5 f6e5\n"
         "end\n",
         ExType::None, ExType::Exchange, 0},
        // 39.Rf5? loses material along the engine's line: never "a better position" for it.
        {"mis_b_160",
         "rec w f2f5 startpos ; e2e4 c7c5 g1e2 d7d6 d2d4 g8f6 e4e5 f6d7 e5e6 f7e6 e2f4 d7f6 a2a4 b8c6 f4e2 g7g6 f2f4 f8h6 d4c5 e8g8 c5d6 e6e5 b1c3 f6g4 d6d7 c8d7 c3a2 d8c7 h2h3 g4f6 d1d7 c7a5 d7d2 a5b6 a4a5 b6c7 e2c3 e5f4 f1c4 g8h8 e1g1 c6a5 c4d3 c7c5 f1f2 a5c4 b2b4 c5c7 h3h4 c4d2 c1b2 f4f3 f2d2 h6e3 g1h1 f3g2 d2g2 e3d4 c3b5 c7d7 a2c3 d4c3 b5c3 d7h3 g2h2 h3f3 h1g1 f3e3 h2f2 a8d8 c3e4 h8g7 a1f1 d8d5 g1g2 d5d4\n"
         "an a0 1 1 0 18 f2f4 3\n"
         "ln 1 18 -502 0 0 0 E f2f4 d4b4 b2c3 b4e4 f4e4 e3c5 c3d4 c5c6 g2g1 f8f7 f1f2 g7g8 e4f4 g8f8 d4b2 b7b5 d3b5\n"
         "ln 2 18 -533 0 0 0 E f2f3 d4e4 f3e3 e4e3 f1a1 a7a6 b4b5 a6b5 a1a5 g7f7 b2d4 e3e1 a5b5 f8c8\n"
         "ln 3 18 -533 0 0 0 E b2c1 e3e4 d3e4 d4e4 h4h5 f8c8 b4b5 e4g4 g2h3 c8c3 f2f3 g4c4 h5h6 g7f7 c1g5 c3f3 f1f3 c4c2\n"
         "an a1 1 1 0 18 f2f5 1\n"
         "ln 1 18 -936 0 0 0 E f2f5 d4e4 f5f3 e4g4 g2h3 e3e6 b2c3 g4h4 h3g2 h4g4 g2f2 e6d7 d3e2 g7g8\n"
         "an a2 1 0 0 14 d4e4 2\n"
         "ln 1 14 961 0 0 0 E d4e4 d3e4 e3e4 f5f3 f8c8 f1f2 g7g8 b4b5 c8c2 b2f6 c2f2 g2f2 e7f6 f3g3 e4h4\n"
         "ln 2 14 879 0 0 0 E g6f5 b2c1 e3e4 d3e4 d4e4 g2h3 g7f7 f1f5 e4b4 f5g5 b4e4 g5g3 b7b5\n"
         "an a3 1 1 0 6 f2f4 1\n"
         "ln 1 6 -496 0 0 0 E f2f4 d4d3 c2d3 e3d3 f1f3 d3c2 f3f2\n"
         "end\n",
         ExType::None, ExType::Positional, 0},
        // 8...Qe7?? 9.Qxc8+: the bishop on c8 is simply taken, with check (no back-rank reason).
        {"mat_all_1020",
         "rec b d8e7 startpos ; c2c3 c7c6 e2e4 d7d6 a2a4 g8f6 g1e2 f6e4 e2f4 g7g6 d2d3 e4f6 h2h3 e7e5 d1g4\n"
         "an a0 1 0 0 18 c8g4 3\n"
         "ln 1 18 1012 0 0 0 E c8g4 h3g4 e5f4 f1e2 g6g5 e1g1 f8g7 e2d1 b7b5 f1e1 e8f8 c1d2 a7a6 d1e2\n"
         "ln 2 18 989 0 0 0 E f6g4 h3g4 e5f4 g4g5 d8g5 f1e2 e8d7 e1d2 g5g2\n"
         "ln 3 18 832 0 0 0 E f8g7 g4f3 e5f4 c1f4 e8g8 b1d2 c6c5 f4e3 b8c6\n"
         "an a1 1 0 0 18 d8e7 1\n"
         "ln 1 18 -600 0 0 0 E d8e7 g4c8 e7d8 c8d8 e8d8 f4e2 b8d7 d3d4 f8g7 g2g3 e5d4 c3d4 f6d5 e1d1 a7a5 a1a3 d7b6 a3f3 f7f5 b1c3\n"
         "an a2 1 1 0 14 g4c8 2\n"
         "ln 1 14 600 0 0 0 E g4c8 e7d8 c8d8 e8d8 f4e2 b8d7 d3d4 f8g7 g2g3 e5d4 c3d4 f6d5 e1d1 a7a5\n"
         "ln 2 14 -681 0 0 0 E f4g6 c8g4 g6e7 g4e6 e7c6 b8c6 b1a3 f6d5 g2g4 h7h5 g4g5 d5e7\n"
         "an a3 1 0 0 6 c8g4 1\n"
         "ln 1 6 1012 0 0 0 E c8g4 h3g4 e5f4 f1e2 g6g5 c3c4\n"
         "end\n",
         ExType::Hanging, ExType::BackRank, 3},
        // 34.c6?? Kxc6: the rook is lost for the bishop (no back-rank reason).
        {"mat_all_29",
         "rec w c5c6 startpos ; d2d4 c7c6 b1c3 h7h5 e2e4 g8f6 g1f3 d7d5 e4e5 f6g8 h2h3 g7g6 f1e2 c8f5 f3h4 f5c8 c1f4 e7e6 h4f3 a7a5 e1g1 g8h6 c3a4 h6f5 e2d3 b7b6 f4g5 f8e7 d1d2 b8d7 g5e7 d8e7 f1d1 f5h4 f3e1 e7b4 d2f4 h4g2 e1g2 g6g5 c2c3 h8g8 b2b3 g5f4 c3b4 f4f3 d1c1 b6b5 a4c5 d7c5 c1c5 g8g2 g1h1 g2f2 a2a4 f2d2 c5c3 e8e7 b4a5 b5b4 c3c5 e7d7 d3f1 d2b2\n"
         "an a0 1 1 0 18 a1c1 3\n"
         "ln 1 18 -347 0 0 0 E a1c1 c8b7 a5a6 b7a6 c5c6 a6f1 c6c7 d7e8 c1f1 b2b3 c7b7 e8f8 h3h4 b3a3 a4a5 a8a5 b7b8 f8e7 b8b7 e7d8 b7b4 a5a7 h1h2 d8e7 h2g3 f3f2 g3f2 a3h3 f2g2 h3h4\n"
         "ln 2 18 -364 0 0 0 E c5c1 b2b3 c1b1 b3b1 a1b1 d7c7 b1b4 a8a5 h1g1 c8a6 h3h4 a6f1 g1f1 a5a6 f1f2 a6b6 b4b6 c7b6 f2f3 c6c5 d4c5 b6c5 f3e3\n"
         "ln 3 18 -365 0 0 0 E h3h4 b2b3 a1c1 c8b7 a5a6 b7a6 c5c6 a6f1 c6c7 d7e8 c1f1 a8b8 h1g1 e8f8\n"
         "an a1 1 1 0 18 c5c6 1\n"
         "ln 1 18 -754 0 0 0 E c5c6 d7c6 a1c1 c6d7 f1b5 d7d8 c1c6 b2b1 h1h2 a8a5 c6d6 d8c7 d6c6 c7b7 c6c5 b1b2 h2g3\n"
         "an a2 1 0 0 14 d7c6 2\n"
         "ln 1 14 750 0 0 0 E d7c6 a1c1 c6d7 f1b5 d7d8 c1g1 a8a5 g1g7 b2b3 h1g1 a5a7 g7g8 d8c7 g8g7\n"
         "ln 2 14 -33 0 0 0 E d7e8 a5a6 b2b3 f1b5 e8d8 a1c1 b3c3 c6c3 b4c3\n"
         "an a3 1 1 0 6 a1c1 1\n"
         "ln 1 6 -347 0 0 0 E a1c1 c8b7 a5a6 b7a6 c5c6 a6f1\n"
         "end\n",
         ExType::Exchange, ExType::BackRank, 0},
        // 14...a5? cxd4: White's Qxh5 stood before the move, so it is no reason a take-back fails.
        {"mis_a_229",
         "rec b a7a5 startpos ; h2h3 c7c5 b1a3 g8f6 d2d3 b8c6 c2c3 f6d5 c1g5 h7h6 g5c1 h8g8 a3c4 h6h5 e2e4 d5b6 c1f4 g7g5 f4g3 b6c4 b2b3 d7d5 d3c4 d5d4 g3h2 f8g7 a1c1\n"
         "an a0 1 0 0 18 d8a5 3\n"
         "ln 1 18 470 0 0 0 E d8a5 f1d3 d4c3 g1e2 c8e6 e1g1 e8c8 e4e5 g5g4 h3h4 g7e5 h2e5 c6e5 c1c3 e5f3 g1h1 a5c7 g2f3 g4f3 e2g3\n"
         "ln 2 18 325 0 0 0 E g5g4 d1d2 g4h3 g2h3 e7e5 g1e2 g7f8 h1g1 g8g1 e2g1 c8e6 f1g2 a7a5 g1f3 f7f6 h2g3 d8d7 g3h4 d7g7 e1f1 a5a4 h4g3\n"
         "ln 3 18 315 0 0 0 E c8e6 c3d4 d8a5 d1d2 a5d2 e1d2 g7d4 f1e2 e8c8 d2e1 h5h4 g1f3 d4b2 c1d1 d8d1 e2d1\n"
         "an a1 1 0 0 18 a7a5 1\n"
         "ln 1 18 236 0 0 0 E a7a5 c3d4 a5a4 d4c5 d8a5 d1d2 a5d2 e1d2 a4b3 a2b3 c6d4 c1b1 a8a2 d2c3 d4c6 e4e5 c6e5 h2e5 g7e5 c3b4 e5c7 b4c3 c8f5 b1d1 c7a5 b3b4 a2a3 c3b2 a5b4 d1d5\n"
         "an a2 1 1 0 14 c3d4 2\n"
         "ln 1 14 -237 0 0 0 E c3d4 a5a4 d4c5 d8a5 d1d2 a4b3 a2b3 a5d2 e1d2 c6d4 c1b1 a8a2 d2d1 a2f2 g1e2 d4e2 f1e2 f2g2 b3b4 g7d4 b1b3 g5g4 b3d3\n"
         "ln 2 14 -282 0 0 0 E f1e2 a5a4 e2h5 g8h8 g2g4 c8e6 g1f3 d4c3 e1g1\n"
         "an a3 1 0 0 6 d8a5 1\n"
         "ln 1 6 470 0 0 0 E d8a5 f1d3 d4c3 g1e2 c8e6 e1g1\n"
         "end\n",
         ExType::None, ExType::None, 0},
        // 11...Ne7? fxe5: a pawn takes the queen. The knight on g6 that could not take back (pinned
        // to the king) would not have saved it either: no pinned-defender reason.
        {"m_349_40",
         "rec b c6e7 startpos ; e2e4 f7f6 g1f3 b8c6 f1c4 a7a5 b1c3 e7e6 a2a4 g8e7 e1g1 g7g5 f3g5 f6g5 d1h5 e7g6 d2d3 d8f6 c1g5 f6e5 f2f4\n"
         "an a0 1 0 0 18 e5c5 3\n"
         "ln 1 18 -572 0 0 0 E e5c5 g1h1 f8e7 f4f5 c5e5 f5g6 h7g6 h5g6 e8d8 g5f4 e5h5 g6h5 h8h5 c3e2 d7d6 f4d2 h5g5\n"
         "ln 2 18 -606 0 0 0 E e5g7 c3b5 d7d5 e4d5 c6d4 d5e6 c8e6 f4f5 e6c4 a1e1 e8d7 d3c4 f8c5 g1h1 d4b5 f5g6 h7g6\n"
         "ln 3 18 -639 0 0 0 E f8c5 g1h1 e5g7 c3b5 c5b6 f4f5 h8g8 g5h6 g7h8 h6f4 d7d6 f5e6 c6e5 f4g5\n"
         "an a1 1 0 0 18 c6e7 1\n"
         "ln 1 18 -1097 0 0 0 E c6e7 f4e5 c7c6 f1f6 e8d8 f6g6 d8c7 g6f6 e7g6 f6f8 g6f8 a1f1 f8g6\n"
         "an a2 1 1 0 14 f4e5 2\n"
         "ln 1 14 1095 0 0 0 E f4e5 c7c6 f1f6 e8d8 f6g6 d8c7 g6f6 e7g6 f6f8 g6f8 a1f1 f8g6 f1f6 c7b6 g5e3 c6c5\n"
         "ln 2 14 587 0 0 0 E g1h1 e5c5 f4f5 f8g7 f5f6 g7f8 c3b5 e8d8 b2b3 e7c6 d3d4 c6d4 f6f7 f8e7 g5e7 c5e7 b5d4\n"
         "an a3 1 0 0 6 e5c5 1\n"
         "ln 1 6 -572 0 0 0 E e5c5 g1h1 f8g7 f4f5 b7b6 c3b5\n"
         "end\n",
         ExType::Exchange, ExType::Pin, 0},
        // 11...Be6? Bxd5 Be7 Bxe6 fxe6 Qxe6: four points down, but 11...Qb6 gives up the bishop on d6
        // too. "You've lost 4 points" for a pawn more would say too much.
        {"h_355_26",
         "rec b c8e6 startpos ; c2c4 e7e6 g1f3 d7d5 d2d4 d5c4 e2e3 c7c5 f1c4 g8f6 b1c3 a7a6 d4d5 e6d5 c3d5 f8e7 d1b3 e7d6 e3e4 f6d5 e4e5\n"
         "an a0 1 0 0 18 d8b6 3\n"
         "ln 1 18 -43 0 0 0 E d8b6 e5d6 b6b3 c4b3 c8e6 c1g5 b8d7 e1g1 e8g8 a1d1 d5f6 b3e6 f7e6 f1e1 h7h6 g5f4 g8f7 e1e2 a8c8 h2h4 c5c4 f3e5 d7e5 f4e5\n"
         "ln 2 18 -54 0 0 0 E e8g8 c4d5 d6e7 e1g1 b8c6 f1d1 d8c7 c1f4 c6d8 b3e3 d8e6 h2h4 c8d7 f4g5 e6g5 f3g5 e7g5 h4g5\n"
         "ln 3 18 -55 0 0 0 E d6e7 c4d5 e8g8 e1g1 b8c6 c1d2 d8c7 a1e1 c8f5 e5e6 f7e6 d5e6 g8h8 e6f5 f8f5 e1e4 a8d8 f1e1 c7d7 d2c3 e7f6 c3f6 f5f6 f3g5 h7h6\n"
         "an a1 1 0 0 18 c8e6 1\n"
         "ln 1 18 -218 0 0 0 E c8e6 c4d5 d6e7 d5e6 f7e6 b3e6 b8c6 f3g5 d8d3 e6f7 e8d7 c1e3 a8f8 f7e6 d7c7 a1d1 d3f5 g2g4 f5c2 e1g1 c2e4\n"
         "an a2 1 1 0 14 c4d5 2\n"
         "ln 1 14 211 0 0 0 E c4d5 d6e7 d5e6 f7e6 b3e6 b8c6 f3g5 d8d3 e6f7 e8d7 c1e3 a8f8 f7e6 d7c7 a1d1 d3f5 g2g4 f5c2 e1g1 c7b8\n"
         "ln 2 14 45 0 0 0 E e5d6 d8d6 b3b7 d6c6 b7b3 e8g8 f3g5 b8d7 e1g1 d7b6 c4d3 h7h6 g5e6 c6e6\n"
         "an a3 1 0 0 6 d8b6 1\n"
         "ln 1 6 -43 0 0 0 E d8b6 e5d6 b6b3 c4b3 c8e6 e1g1\n"
         "end\n",
         ExType::None, ExType::Material, 0},
        // 25...Kg5? Nxe6+ (a discovered check) Kg6 Nxc7: the mistake is said in words, so the reason
        // names the queen that falls, not only the check.
        {"m_1036_18",
         "rec b g6g5 startpos ; e2e3 b7b5 f1b5 g8f6 a2a3 c7c5 b1c3 a7a6 b5a4 d8b6 d1e2 e7e6 e3e4 h8g8 g1h3 h7h6 e4e5 b6d8 e5f6 c8b7 e1g1 b8c6 e2h5 f8e7 f6g7 c6e5 f2f3 d8c7 d2d3 e7f6 a1b1 g8g7 c3e4 g7g2 g1g2 e8e7 h5h6 b7e4 h6f6 e7f6 f3e4 f6g7 c1d2 a8a7 d2f4 g7f6 f4e3 f6g6 h3f4\n"
         "an a0 1 0 0 16 g6h7 3\n"
         "ln 1 16 -493 0 0 0 E g6h7 f4h5 f7f5 e3f4 d7d6 f4e5 d6e5 e4f5 c5c4 f1f3 c7a5 a4e8 a5d2 f3f2\n"
         "ln 2 16 -576 0 0 0 E g6g7 g2h1 g7f8 b2b4 c5c4 b1d1 e5g4 e3c5 d7d6\n"
         "ln 3 16 -911 0 0 0 E g6f6 f4d5 f6g7 d5c7 a7c7 g2h1 f7f6 f1g1 g7h8 c2c4 e5d3\n"
         "an a1 1 0 0 16 g6g5 1\n"
         "ln 1 16 -989 0 0 0 E g6g5 f4e6 g5g6 e6c7 a7c7 g2h1 g6h7 f1f4 d7d5 f4h4 h7g8\n"
         "an a2 1 1 0 14 f4e6 2\n"
         "ln 1 14 965 0 0 0 E f4e6 g5g6 e6c7 a7c7 g2h1 c7c6 f1g1 g6h7 g1g5 e5f3 g5g3 c6g6\n"
         "ln 2 14 931 0 0 0 E f4d5 g5g6 d5c7 e5g4 e3f4 a7c7 f4c7 g4e3 g2f3 e3f1 b1f1 g6g7 c7e5 g7h7\n"
         "an a3 1 0 0 6 g6h7 1\n"
         "ln 1 6 -488 0 0 0 E g6h7 g2h1 a7a8 f4h3 a8h8 f1g1\n"
         "end\n",
         ExType::Material, ExType::Discovered, 0},
        // 8.Bg3? Qb6 Nbd2 Qxb2: a mistake said in words ("I win your pawn"); "if you take back with
        // exd6, Qxc3 takes your pawn" would stand after moves never said.
        {"g_1041_7",
         "rec w f4g3 startpos ; c2c3 c7c6 e2e4 g7g6 d2d4 a7a5 g1f3 a5a4 e4e5 d7d6 c1f4 c8f5 h2h3 f8h6\n"
         "an a0 1 1 0 16 f4h6 3\n"
         "ln 1 16 99 0 0 0 E f4h6 g8h6 d1c1 h6g8 f1c4 b8d7 e1g1 d6d5 c4e2 e7e6 c1e3 h7h6 g2g4 f5e4 b1d2 g8e7\n"
         "ln 2 16 8 0 0 0 E f4h2 d6e5 f3e5 b8d7 e5g4 h6g7 b1a3 f5g4 h3g4 h7h5 g4h5 h8h5 f1e2 h5h8\n"
         "ln 3 16 8 0 0 0 E f4d2 d6e5 d4e5 f5e4 f1c4 e4f3 d1f3 e7e6 f3e2\n"
         "an a1 1 1 0 16 f4g3 1\n"
         "ln 1 16 -44 0 0 0 E f4g3 d8b6 b1d2 b6b2 a1c1 d6d5 g3h4 h6d2 d1d2 a4a3 f1e2 b8d7 e1g1 b2d2 f3d2 h7h5 c3c4 d5c4 d2c4 f5e6 f1e1\n"
         "an a2 1 0 0 14 d8b6 2\n"
         "ln 1 14 28 0 0 0 E d8b6 b1d2 b6b2 f1e2 d6d5 a1c1 b2a2 e1g1 a2b2 e5e6 g8f6 e6f7 e8f7 g3h2\n"
         "ln 2 14 -5 0 0 0 E d6e5 f3e5 g8f6 f1d3 f5e6 e1g1 b8d7\n"
         "an a3 1 1 0 6 f4h6 1\n"
         "ln 1 6 53 0 0 0 E f4h6 g8h6 d1c1 h6g8 f1e2 d6e5 f3e5\n"
         "end\n",
         ExType::Pin, ExType::None, 0},
        // 26...Kg8? Rxf4 Nxf4 Rxd8+ Rxd8: the rook takes the queen however it is defended; the check
        // on the back rank is not why.
        {"g_1041_189",
         "rec b f7g8 startpos ; c2c3 g8f6 d2d4 d7d5 h2h3 b7b6 g1f3 c8b7 c1f4 f6e4 f3e5 f7f6 e5d3 b8c6 f2f3 e7e5 e2e3 e4c5 f4h2 c5d7 b1d2 f8d6 e3e4 e5d4 h2d6 d4c3 d2b3 c3b2 d3b2 c6e7 d6a3 c7c5 f1e2 d7e5 f3f4 e5g6 e2b5 e8f7 e1g1 d5e4 b2c4 e7d5 d1d2 a7a6 b5a4 e4e3 d2e1 d5f4 a1d1 e3e2 d1d7\n"
         "an a0 1 0 0 16 d8d7 3\n"
         "ln 1 16 -2 0 0 0 E d8d7 a4d7 e2f1r e1f1 b7g2 f1d1 h8d8 d1d6 g2h3 c4b6 d8d7 b6d7 a8e8 d7b6\n"
         "ln 2 16 -504 0 0 0 E f7g8 f1f4 g6f4 d7d8 a8d8 b3c1 b6b5 c1e2 d8e8 a4d1 b5c4 e1d2 f4d3 e2f4 b7e4 f4d3 c4d3\n"
         "ln 3 16 -630 0 0 0 E f7f8 f1f2 d8e8 c4b6 f8g8 b3c5 h7h6 c5b7 e8e3 g1h2 e3b6 a3c5 b6b2\n"
         "an a2 1 1 0 14 f1f4 2\n"
         "ln 1 14 504 0 0 0 E f1f4 g6f4 d7d8 a8d8 b3c1 b6b5 c1e2 d8e8 a4d1 b5c4 e1d2 f4d3 e2f4 b7e4 f4d3\n"
         "ln 2 14 421 0 0 0 E f1f2 d8e8 a3c1 a8d8 c1f4 g6f4 f2f4 d8d7 a4d7 e8d7 e1e2 h7h6 f4f2 d7c7 e2e6 g8h7\n"
         "an a3 1 0 0 6 d8d7 1\n"
         "ln 1 6 -99 0 0 0 E d8d7 a4d7 e2f1q e1f1\n"
         "end\n",
         ExType::Material, ExType::BackRank, 0},
        // 45...Nc3? d8=Q Rxd8 Rxc3: the new queen is taken at once (the rook guards d8), so no
        // "your king is too far"; the promotion is the in-between move before the knight falls.
        {"g_369_149",
         "rec b b5c3 startpos ; d2d4 e7e6 c2c4 f8b4 b1c3 c7c5 d4c5 g8f6 g1f3 b8a6 d1c2 d8c7 a2a3 b4c3 b2c3 a6c5 a3a4 d7d6 g2g3 c8d7 f3d2 d7c6 e2e4 a7a6 f2f3 h7h5 a4a5 a8d8 d2b3 c6a4 a1b1 c5b3 c2a2 b3c5 a2a4 c5a4 b1b4 c7d7 b4b7 a4c5 b7d7 c5d7 e1f2 h8f8 c1g5 e6e5 c4c5 d8a8 g5f6 d7f6 f1a6 a8a6 c5d6 a6d6 a5a6 e8e7 a6a7 d6d4 c3d4 f8a8 d4d5 e7d6 h1a1 f6e8 f2e3 d6c7 f3f4 c7b7 e3f3 b7b6 f4e5 e8c7 g3g4 h5g4 f3g3 c7b5 e5e6 f7f6 e6e7 b6c7 a1c1 c7d6 g3f4 d6e7 d5d6 e7e6 d6d7 g7g5 f4e3\n"
         "an a0 1 0 0 18 e6d7 3\n"
         "ln 1 18 406 0 0 0 E e6d7 c1b1 b5d6 b1a1 d6c8 a1d1 d7e7 d1c1 c8a7 c1a1 e7f7 a1a6 f7g6 e4e5 a8f8 a6a7 f8e8 a7a6 e8e5 e3f2 e5e4 f2g1 e4b4\n"
         "ln 2 18 298 0 0 0 E b5a7 c1a1 e6d7 a1a5 d7c7 a5a6 f6f5 e4f5 a8e8 e3f2 a7c6 a6a4 c6e5 a4a7 c7d6 a7a6 d6e7\n"
         "ln 3 18 -410 0 0 0 E a8d8 c1c8 b5a7 c8d8 a7c6 d8a8 e6d7 a8a6 g4g3 h2h3 f6f5 e4f5 c6e5 e3e4 g3g2 a6a1 e5c4 a1g1 d7e7 g1g2 e7f6 g2a2 c4d6 e4f3 f6f5 a2a5 f5f6 f3g4\n"
         "an a1 1 0 0 18 b5c3 1\n"
         "ln 1 18 -581 0 0 0 E b5c3 d7d8q a8d8 c1c3 g4g3 h2g3 d8a8 c3c6 e6e7 c6a6 e7d8 e3f2 d8c7 a6f6 a8a7 f6f7 c7b8 f7a7 b8a7 f2f3 a7b6 f3g4 b6c6 g4g5\n"
         "an a2 1 1 0 14 d7d8r 2\n"
         "ln 1 14 570 0 0 0 E d7d8r a8d8 c1c3 g4g3 h2g3 f6f5 c3c6 e6d7 c6a6 f5f4 g3f4 g5f4 e3f4 d8f8 f4e5 f8a8 e5f6 a8f8 f6g6 f8a8 g6f7 d7c7 e4e5 c7b7 a6a3 a8a7 a3a7 b7a7 f7g6 a7b7 e5e6\n"
         "ln 2 14 567 0 0 0 E d7d8q a8d8 c1c3 g4g3 h2g3 f6f5 c3a3 f5f4 g3f4 g5f4 e3f4 d8f8 f4e3 f8c8\n"
         "an a3 1 0 0 6 e6d7 1\n"
         "ln 1 6 406 0 0 0 E e6d7 c1b1 b5d6 b1a1 d6c8 a1d1 d7e7\n"
         "end\n",
         ExType::None, ExType::PromotionRace, 0},
    };
    for (const Case& k : cases) {
        const Recorded rc = recorded(k.rec);
        for (int level = 1; level <= 6; ++level) {
            Reviewer rv;
            const Review r = reviewRecorded(rv, rc, level);
            char where[64];
            std::snprintf(where, sizeof where, "%s b%d", k.what, level);
            checkScript(r.script, where);
            if (k.want != ExType::None && r.verdict.voiced) CHECK_EQ(r.verdict.exType, k.want);
            if (k.never != ExType::None) CHECK(r.verdict.exType != k.never);
            CHECK(!hasKeyPrefix(r.script, "ex.retake"));
            if (k.pts > 0 && r.verdict.voiced)
                for (const Beat& b : r.script)
                    if (const Arg* pts = b.line.arg("pts")) CHECK_EQ(pts->number, k.pts);
            if ((k.want != ExType::None && r.verdict.exType != k.want) ||
                (k.never != ExType::None && r.verdict.exType == k.never)) {
                std::fprintf(stderr, "  %s: %s\n", where, exTypeName(r.verdict.exType));
                dump(r.script);
            }
        }
    }
}

TEST(coach_review_short_refutation_is_completed_on_the_board) {
    // A0 stopped when the student moved: its line for 11.g3 ends with 11...Nxd2, before White takes
    // back. Counting the bishop as lost there made the coach say "your bishop is worth 3 points, my
    // knight only 3: this exchange costs you a lot". The natural recapture and the capture it allows
    // are added on the board: the discovered attack on h1 is found, and no exchange is blamed.
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOfUci(kG3Game);
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, g3A0("g2g3 e4d2"));
        char where[64];
        std::snprintf(where, sizeof where, "g3 short b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        if (r.verdict.exType != ExType::Discovered) {
            std::fprintf(stderr, "  %s: %s\n", where, exTypeName(r.verdict.exType));
            dump(r.script);
        }
        CHECK_EQ(r.verdict.exType, ExType::Discovered);
        CHECK(!hasKeyPrefix(r.script, "ex.exchange"));
    }
    // Without the capture behind it (no rook on h1), the short line proves nothing: no exchange is
    // blamed and, at levels 1-2, no cause is given rather than a false one (at most the verdict and
    // a hint).
    const char* noRook = "2r1kb1r/ppp1pppp/1n6/3q4/3PnP2/NP2P3/3B2PP/R2QK1N1 w Qk - 0 11";
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(noRook, {"g3"});
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, analysisOf({pvl(-60, "g1f3 e4d2 d1d2"), pvl(-300, "g2g3 e4d2")}));
        char where[64];
        std::snprintf(where, sizeof where, "no rook b%d", level);
        checkScript(r.script, where);
        CHECK(r.verdict.exType != ExType::Exchange);
        CHECK(!hasKeyPrefix(r.script, "ex.exchange"));
        if (level <= 2) CHECK(!r.verdict.voiced || r.verdict.exType == ExType::None);
    }
}

TEST(coach_review_even_trade_is_no_exchange_loss) {
    // 1.a3 lets the knight take the bishop on d2, and the king takes back: knight for bishop, 3
    // points each (330 and 320 centipawns). Whether the engine's line shows the recapture or stops
    // before it, the coach never says the bishop is lost to a cheaper piece.
    const char* fen = "6k1/ppp5/8/8/4n3/8/PPPB4/2K5 w - - 0 1";
    for (const char* played : {"a2a3 e4d2 c1d2 g8f7", "a2a3 e4d2"}) {
        for (int level = 1; level <= 6; ++level) {
            Game g = gameOf(fen, {"a3"});
            Reviewer rv;
            rv.reset(level, White);
            Review r = reviewOf(rv, g, analysisOf({pvl(10, "c2c3 g8f7"), pvl(0, "b2b3 g8f7"), pvl(-150, played)}));
            char where[64];
            std::snprintf(where, sizeof where, "even trade b%d", level);
            checkScript(r.script, where);
            CHECK(r.verdict.cls != MoveClass::Blunder);
            CHECK(r.verdict.exType != ExType::Exchange);
            CHECK(!hasKeyPrefix(r.script, "ex.exchange"));
            CHECK(!hasKeyPrefix(r.script, "ex.hanging"));
        }
    }
}

TEST(coach_review_cheaper_attacker_still_explained) {
    // 1.Rd4?? puts the rook, defended by the c3 pawn, where the knight takes it: 5 points for 3.
    const char* fen = "6k1/5ppp/2n5/8/8/2P5/5PPP/3R2K1 w - - 0 1";
    for (const char* played : {"d1d4 c6d4 c3d4 g8f8", "d1d4 c6d4"}) {
        for (int level = 1; level <= 6; ++level) {
            Game g = gameOf(fen, {"Rd4"});
            Reviewer rv;
            rv.reset(level, White);
            Review r = reviewOf(rv, g, analysisOf({pvl(50, "g1f1 g8f8"), pvl(-200, played)}));
            char where[64];
            std::snprintf(where, sizeof where, "cheaper attacker b%d", level);
            checkScript(r.script, where);
            if (r.verdict.exType != ExType::Exchange) dump(r.script);
            CHECK_EQ(r.verdict.exType, ExType::Exchange);
            CHECK(r.verdict.voiced);
            const Beat* b = beatWithKey(r.script, "ex.exchange_cheap.b" + std::to_string(level));
            CHECK(b != nullptr);
            if (!b) continue;
            CHECK(b->line.arg("n") && b->line.arg("n")->number == 5);
            CHECK(b->line.arg("n2") && b->line.arg("n2")->number == 3);
            CHECK(b->line.arg("pts") && b->line.arg("pts")->number == 2);
        }
    }
}

TEST(coach_review_queen_taken_by_a_pawn_is_the_cheaper_attacker) {
    // From a mined game: 33.Qc4? dxc4. Two attackers against one defender is true, but a pawn takes the
    // queen whatever defends it: the reason is the cheaper attacker, not the count.
    const ai::Analysis a0 = analysisOf({pvl(-519, "f4d3 b5d3 c3d3 a5a4 b3c2 a3d3 c2d3 b7b2 a1b1 a4a3"),
                                        pvl(-995, "c3c4 d5c4 b3c2 e7d6 e1g1 g7f8 c2e4 a5a4 g2g3 e6e5")});
    for (int level = 2; level <= 6; ++level) {
        Game g = gameOf("1r6/1r2bpk1/2p1p1p1/pb1p4/3P1N1p/qBQ1P2P/P4PP1/R3R2K w - - 4 33", {"Qc4"});
        Reviewer rv;
        rv.reset(level, White);
        const Review r = reviewOf(rv, g, a0);
        char where[32];
        std::snprintf(where, sizeof where, "queen to a pawn b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.exType, ExType::Exchange);
        CHECK(beatWithKey(r.script, "ex.exchange_cheap.b" + std::to_string(level)) != nullptr);
    }
}

TEST(coach_review_battery_counts_in_the_exchange) {
    // 1.c4?? leaves the bishop on d4 defended by the knight alone, against the bishop on b6 and the
    // queen behind it: Bxd4 Nxd4 Qxd4 wins it. The count says 2 attackers against 1 defender.
    const char* fen = "6k1/q4ppp/1b6/8/3B4/1NP5/6PP/6K1 w - - 0 1";
    for (const char* played : {"c3c4 b6d4 b3d4 a7d4 g1f1", "c3c4 b6d4"}) {
        for (int level = 1; level <= 6; ++level) {
            Game g = gameOf(fen, {"c4"});
            Reviewer rv;
            rv.reset(level, White);
            Review r = reviewOf(rv, g, analysisOf({pvl(0, "g1f1 g8f8"), pvl(-10, "h2h3 g8f8"), pvl(-300, played)}));
            char where[64];
            std::snprintf(where, sizeof where, "battery b%d", level);
            checkScript(r.script, where);
            if (r.verdict.exType != ExType::Exchange) dump(r.script);
            CHECK_EQ(r.verdict.exType, ExType::Exchange);
            CHECK(r.verdict.voiced);
            const Beat* b = beatWithKey(r.script, "ex.exchange_count.b" + std::to_string(level));
            CHECK(b != nullptr);
            if (!b) continue;
            CHECK(b->line.arg("n") && b->line.arg("n")->number == 2);
            CHECK(b->line.arg("n2") && b->line.arg("n2")->number == 1);
            CHECK(b->line.arg("pts") && b->line.arg("pts")->number == 3);
        }
    }
}

TEST(coach_review_line_ending_in_check_proves_the_loss) {
    // 1.Bf4?? (attacking the rook on b8) takes the guard off the knight on c3: 1...Qxc3+ wins it, and
    // White, in check, cannot take the rook. Where A0's line stops, the bishop's capture is no
    // answer to the check: the knight is lost, and said so at every level (with the offer at 1-2).
    const char* fen = "1r4k1/5p1p/6p1/q7/8/2N5/P2B1PPP/4K3 w - - 0 1";
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(fen, {"Bf4"});
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, analysisOf({pvl(0, "e1f1 g8g7"), pvl(-10, "h2h3 g8g7"), pvl(-300, "d2f4 a5c3")}));
        char where[48];
        std::snprintf(where, sizeof where, "loss in check b%d", level);
        checkScript(r.script, where);
        if (r.verdict.exType != ExType::Hanging) dump(r.script);
        CHECK_EQ(r.verdict.exType, ExType::Hanging);
        CHECK(r.verdict.voiced);
        if (level <= 2) CHECK(r.offersTakeback);
    }
}

TEST(coach_review_guessed_reply_never_contradicts_the_engine) {
    // 1.a3?? allows 1...Nxd2, uncovering the bishop on a8 against the queen on h1. A0's line stops
    // after 1...Nxd2: the guessed recapture 2.Kxd2 would let 2...Bxh1 take the queen, but White
    // moves the queen instead (the engine says -3, a bishop): the coach never claims the queen falls.
    const char* fen = "b2r2k1/5ppp/3p4/8/4n3/8/PPPB1P1P/4K2Q w - - 0 1";
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(fen, {"a3"});
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, analysisOf({pvl(0, "f2f3 e4c5"), pvl(-20, "d2e3 g8f8"), pvl(-300, "a2a3 e4d2")}));
        char where[48];
        std::snprintf(where, sizeof where, "guessed reply b%d", level);
        checkScript(r.script, where);
        CHECK(r.verdict.exType != ExType::Discovered);
        CHECK(!hasKeyPrefix(r.script, "ex.discovered"));
        for (const Beat& b : r.script) CHECK(b.uci != "e1d2" && b.uci != "a8h1");
    }
    // The guesses themselves: dropped against a score of -3, kept when the engine sees the queen lost.
    Position p1;
    CHECK(p1.setFEN("b2r2k1/5ppp/3p4/8/4n3/P7/1PPB1P1P/4K2Q b - - 0 1"));
    for (int cp : {-300, -900}) {
        std::vector<LineStep> r = replayLine(p1, {"e4d2"}, White);
        ai::Score best, played;
        played.cp = cp;
        detail::extendRefutation(r, p1, White, 4, materialBalance(p1, White), detail::guessLossBound(best, played));
        CHECK_EQ(r.size(), size_t(cp == -300 ? 1 : 3));
        if (r.size() == 3) CHECK(r[1].guessed && r[2].guessed && r[2].uci == "a8h1");
    }
}

TEST(coach_review_mate_lines_only_when_the_engine_shows_them) {
    // 1.a3?? allows 1...Rd1+ 2.Rxd1 Rxd1#. A0 stopped after 1...Rd1+: levels 3, 4 and 6 would say
    // "this allows mate in 2: Rd1+" (or "after a3, Rd1+ is mate"), so they say the mate without moves.
    const char* fen = "3r2k1/3r1ppp/8/8/8/8/P4PPP/2R3K1 w - - 0 1";
    for (bool full : {true, false}) {
        const ai::Analysis a0 = analysisOf(
            {pvl(-20, "h2h3 d7d2"), pvl(-30, "g2g3 d7d2"), pvl(0, full ? "a2a3 d7d1 c1d1 d8d1" : "a2a3 d7d1", -2)});
        for (int level = 3; level <= 6; ++level) {
            Game g = gameOf(fen, {"a3"});
            Reviewer rv;
            rv.reset(level, White);
            Review r = reviewOf(rv, g, a0);
            char where[48];
            std::snprintf(where, sizeof where, "mate allowed %s b%d", full ? "full" : "short", level);
            checkScript(r.script, where);
            CHECK_EQ(r.verdict.exType, ExType::MateAllowed);
            CHECK(hasKey(r.script, full ? "ex.mate_allowed.b" + std::to_string(level) : std::string("ex.mate_allowed.b5")));
        }
    }
    // 1.h3 misses 1.Rd8+ Rxd8 2.Rxd8#. With the best line cut after 1.Rd8+, the lines that show the
    // mate ("{best} mates in 2: Rd8+") give way to those that name its first move only.
    const char* mateFen = "2r3k1/p4ppp/8/8/8/8/3R1PPP/3R2K1 w - - 0 1";
    for (bool full : {true, false}) {
        const ai::Analysis a0 = analysisOf({pvl(0, full ? "d2d8 c8d8 d1d8" : "d2d8", 2), pvl(300, "h2h3 a7a5")});
        for (int level = 3; level <= 6; ++level) {
            Game g = gameOf(mateFen, {"h3"});
            Reviewer rv;
            rv.reset(level, White);
            Review r = reviewOf(rv, g, a0);
            char where[48];
            std::snprintf(where, sizeof where, "mate missed %s b%d", full ? "full" : "short", level);
            checkScript(r.script, where);
            CHECK_EQ(r.verdict.exType, ExType::MateMissed);
            CHECK(hasKey(r.script, full ? "ex.mate_missed.b" + std::to_string(level) : std::string("ex.mate_missed.b2")));
        }
    }
}

TEST(coach_review_stalemate_trick_needs_the_sacrifice) {
    // 1.Kd7?? (winning: f7-f8 queens) allows 1...Rc7+ 2.Kxc7, stalemate: the rook given away, the
    // coach's king has no move. The trick is explained at every level.
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf("2r4k/5P2/4KPP1/8/8/8/8/8 w - - 0 1", {"Kd7"});
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, analysisOf({pvl(900, "e6e7 c8c7 e7e8"), pvl(0, "e6d7 c8c7 d7c7")}));
        char where[48];
        std::snprintf(where, sizeof where, "stalemate trick b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.exType, ExType::Stalemate);
        CHECK(hasKey(r.script, "ex.stalemate_trick.b" + std::to_string(level)));
    }
    // A stalemate the human walks into without taking anything ("I can give away my pieces" would be
    // false): 1.Kf7 b6 2.a3 stalemates the coach's king, nothing sacrificed.
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf("7k/1p2K3/6P1/1P6/8/8/P7/8 w - - 0 1", {"Kf7"});
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, analysisOf({pvl(900, "g6g7 h8g8"), pvl(0, "e7f7 b7b6 a2a3")}));
        char where[48];
        std::snprintf(where, sizeof where, "no trick b%d", level);
        checkScript(r.script, where);
        CHECK(!hasKeyPrefix(r.script, "ex.stalemate_trick"));
    }
}

TEST(coach_review_inaccuracy_is_not_called_a_mistake) {
    // 1.a4 leaves the b4 pawn to the rook: an inaccuracy (the appraisal counts it so). It is never
    // called a mistake, never played out on the table nor offered back; level 6 says the better move
    // in one short line, levels 1-5 only for a larger slip (this one is small).
    const char* fen = "1r4k1/5ppp/8/8/1P6/P7/5PPP/6K1 w - - 0 1";
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(fen, {"a4"});
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, analysisOf({pvl(100, "g1f1 g8f8"), pvl(30, "a3a4 b8b4 g1f1")}));
        char where[32];
        std::snprintf(where, sizeof where, "inaccuracy b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.cls, MoveClass::Inaccuracy);
        CHECK(!hasKeyPrefix(r.script, "ex.verdict.mistake"));
        CHECK(!hasKeyPrefix(r.script, "ex.verdict.blunder"));
        CHECK_EQ(demoMoves(r.script), 0);
        CHECK(!r.offersTakeback);
        CHECK_EQ(r.script.size(), size_t(level == 6 ? 1 : 0));
        if (level == 6) {
            const Beat* slip = beatWithKey(r.script, "ex.verdict.inaccuracy.b6");
            CHECK(slip != nullptr);
            if (slip) {
                const Arg* best = slip->line.arg("best");
                CHECK(best && best->uci == "g1f1");
            }
        }
    }
}

TEST(coach_review_score_bound_is_rescored) {
    // A0 was stopped when the player moved: its line for the move holds only a bound. The move is
    // re-scored (A1) and judged on that exact score.
    Game g = gameOf(nullptr, {"e4", "e5", "Nf3"});
    ai::Analysis a0 = analysisOf({pvl(30, "g1f3 b8c6"), pvl(20, "b1c3 g8f6"), pvl(-600, "g1f3 b8c6")});
    a0.lines[2].pv = {"g1f3"};
    a0.lines[2].score.bound = ai::Score::Bound::Upper;
    a0.lines[0].pv = {"f1c4", "g8f6"};
    CHECK(Reviewer::needsPlayedRequest(a0, "g1f3"));
    CHECK(!Reviewer::needsPlayedRequest(a0, "f1c4"));
    const ai::Analysis a1 = analysisOf({pvl(25, "g1f3 b8c6")});
    Reviewer rv;
    rv.reset(3, White);
    ReviewInput in;
    in.game = &g;
    in.before = &a0;
    in.played = &a1;
    Review r = rv.review(in);
    CHECK(r.verdict.cls != MoveClass::Blunder && r.verdict.cls != MoveClass::Mistake);
}

TEST(coach_review_best_line_bound_is_not_rescored) {
    // The player finds 1.Rd8+ (mate in 2), A0's best line, stopped with a bound. It is judged against
    // that same line: no re-score is asked, and another search's score (a cp one here) never turns
    // the engine's own move into "you missed mate in 2, starting with Rd8+".
    const char* fen = "2r3k1/p4ppp/8/8/8/8/3R1PPP/3R2K1 w - - 0 1";
    ai::Analysis a0 = analysisOf({pvl(0, "d2d8 c8d8 d1d8", 2), pvl(300, "h2h3 a7a5")});
    a0.lines[0].score.bound = ai::Score::Bound::Lower;
    CHECK(!Reviewer::needsPlayedRequest(a0, "d2d8"));
    const ai::Analysis a1 = analysisOf({pvl(600, "d2d8 c8d8 d1d8")});
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(fen, {"Rd8+"});
        Reviewer rv;
        rv.reset(level, White);
        ReviewInput in;
        in.game = &g;
        in.before = &a0;
        in.played = &a1;
        Review r = rv.review(in);
        char where[48];
        std::snprintf(where, sizeof where, "best bound b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.cls, MoveClass::Best);
        CHECK(!r.verdict.mateMissed);
        CHECK(std::fabs(r.verdict.accuracy - 100.0) < 1e-9);
        CHECK(!hasKeyPrefix(r.script, "ex."));
    }
}

TEST(coach_review_praise_capture_only_for_a_free_piece) {
    // 1.exd5 takes a rook the e6 pawn defends: a good capture, but not "for free".
    const char* fen = "6k1/5ppp/4p3/3r4/4P3/8/5PPP/6K1 w - - 0 1";
    for (int level = 1; level <= 2; ++level) {
        Game g = gameOf(fen, {"exd5"});
        Reviewer rv;
        rv.reset(level, White);
        Review r = reviewOf(rv, g, analysisOf({pvl(400, "e4d5 e6d5"), pvl(0, "g1f1 d5d2")}));
        checkScript(r.script, "defended rook");
        CHECK_EQ(r.verdict.cls, MoveClass::Best);
        CHECK(!hasKeyPrefix(r.script, "praise.capture"));
        CHECK(!r.verdict.goodCapture);
    }
}

TEST(coach_review_demo_names_only_what_happens) {
    detail::Ctx c;
    c.level = 2;
    c.b = band(2);
    c.human = White;
    c.coach = Black;
    c.ply = 20;
    detail::Explanation ex;
    ex.demoPlies = 3;
    // An underpromotion is said as the pawn's move, not as "a new queen".
    CHECK(c.p1.setFEN("4k3/1P6/8/8/8/7r/8/4K3 b - - 0 1"));
    c.r = replayLine(c.p1, {"h3h2", "b7b8n", "e8d7"}, White);
    Script s;
    detail::appendDemo(c, ex, s);
    checkScript(s, "underpromotion");
    CHECK(!hasKey(s, "demo.promote"));
    CHECK(hasKey(s, "demo.your.move"));
    // "Your king has to move" only when nothing else answers the check.
    for (bool knight : {true, false}) {
        CHECK(c.p1.setFEN(knight ? "3q2k1/8/8/8/8/8/2N3PP/6K1 b - - 0 1" : "3q2k1/8/8/8/8/8/6PP/6K1 b - - 0 1"));
        c.r = replayLine(c.p1, {"d8d4", "g1h1", "g8f8"}, White);
        CHECK_EQ(c.r.size(), size_t(3));
        Script k;
        detail::appendDemo(c, ex, k);
        checkScript(k, "king move");
        CHECK_EQ(hasKey(k, "demo.your.king"), !knight);
    }
}

// ---- Praise, rate limits, takebacks ---------------------------------------------------------------

TEST(coach_review_praise_only_top_moves) {
    const char* fen = "6k1/5ppp/8/3n4/8/2N5/5PPP/6K1 w - - 0 1";
    // The capture is the engine's move: praised (level 1: "you took my knight for free").
    {
        Game g = gameOf(fen, {"Nxd5"});
        Reviewer rv;
        rv.reset(1, White);
        const ai::Analysis a0 = analysisOf({pvl(350, "c3d5 g8f8"), pvl(0, "g1f1 d5c3")});
        Review r = reviewOf(rv, g, a0);
        CHECK_EQ(r.verdict.cls, MoveClass::Best);
        CHECK(r.verdict.praised);
        CHECK(hasKey(r.script, "praise.capture.b1"));
        checkScript(r.script, "praise capture");
        const Beat* b = beatWithKey(r.script, "praise.capture.b1");
        if (b) CHECK(b->look == Look::Player);
        // Rate limit: at most one praise per 3 of the player's moves at level 1.
        int praised = 0;
        for (int i = 0; i < 5; ++i) praised += reviewOf(rv, g, a0).verdict.praised ? 1 : 0;
        CHECK_EQ(praised, 1);   // the 4th review (3 moves after the first praise) is praised again
    }
    // The same capture when something clearly better existed: no praise.
    {
        Game g = gameOf(fen, {"Nxd5"});
        Reviewer rv;
        rv.reset(1, White);
        const ai::Analysis a0 = analysisOf({pvl(500, "g1f1 g8f8"), pvl(350, "c3d5 g8f8")});
        Review r = reviewOf(rv, g, a0);
        CHECK(r.verdict.cls != MoveClass::Best && r.verdict.cls != MoveClass::Excellent);
        CHECK(!r.verdict.praised);
        for (const std::string& k : keysOf(r.script)) CHECK(!startsWith(k, "praise."));
    }
    // Level 3: a quiet best move is praised only when a shallow search (A3) would have missed it.
    {
        const ai::Analysis a0 = analysisOf({pvl(30, "e2e4 e7e5"), pvl(25, "d2d4 d7d5")});
        ai::Analysis shallow = analysisOf({pvl(40, "g1f3 d7d5")});
        for (bool tricky : {false, true}) {
            Game g = gameOf(nullptr, {"e4"});
            Reviewer rv;
            rv.reset(3, White);
            ReviewInput in;
            in.game = &g;
            in.before = &a0;
            if (tricky) in.shallow = &shallow;
            Review r = rv.review(in);
            CHECK_EQ(r.verdict.cls, MoveClass::Best);
            CHECK_EQ(r.verdict.praised, tricky);
            if (tricky) CHECK(hasKey(r.script, "praise.excellent.b3"));
            checkScript(r.script, "praise excellent");
        }
    }
    // Across every level and every scenario of this file, praise comes only with top classes.
    for (int level = 1; level <= 6; ++level) {
        Reviewer rv;
        rv.reset(level, White);
        Game g1 = forkGame();
        Review r1 = reviewOf(rv, g1, forkA0());
        for (const std::string& k : keysOf(r1.script)) CHECK(!startsWith(k, "praise."));
        Game g2 = gameOf(kHangFen, {"Nd5"});
        Review r2 = reviewOf(rv, g2, hangA0());
        for (const std::string& k : keysOf(r2.script)) CHECK(!startsWith(k, "praise."));
    }
}

TEST(coach_review_takeback_flow) {
    Game g = gameOf(kHangFen, {"Nd5"});
    Reviewer rv;
    rv.reset(1, White);
    Review r = reviewOf(rv, g, hangA0());
    CHECK(r.offersTakeback);
    CHECK_EQ(rv.offersMade(), 1);
    // Accepted: the move is taken back, the hint points at the knight on c3.
    CHECK(g.undo(1));
    Script hint = rv.takebackAccepted(g);
    checkScript(hint, "takeback hint");
    const Beat* h = beatWithKey(hint, "tb.hint.b1");
    CHECK(h != nullptr);
    if (h) {
        CHECK(!h->gestures.empty());
        if (!h->gestures.empty()) CHECK_EQ(h->gestures[0].square, sq("c3"));
    }
    // The replay: a safe move, judged against the first one.
    g.play(g.position().parseSAN("Kf1"));
    Review fixed = reviewOf(rv, g, hangA0());
    CHECK(fixed.isRetry);
    CHECK(fixed.takeback.fixed);
    CHECK(!fixed.takeback.same);
    CHECK_EQ(fixed.takeback.firstUci, std::string("c3d5"));
    CHECK(!fixed.script.empty());
    if (!fixed.script.empty()) CHECK_EQ(fixed.script.front().line.key, std::string("tb.fixed.b1"));
    checkScript(fixed.script, "retry fixed");
    CHECK_EQ(rv.humanMoves(), 1);   // a retry is the same move of the game

    // The same move again: said once, never offered twice.
    Game g2 = gameOf(kHangFen, {"Nd5"});
    Reviewer rv2;
    rv2.reset(1, White);
    CHECK(reviewOf(rv2, g2, hangA0()).offersTakeback);
    g2.undo(1);
    rv2.takebackAccepted(g2);
    g2.play(g2.position().parseSAN("Nd5"));
    Review same = reviewOf(rv2, g2, hangA0());
    CHECK(same.isRetry && same.takeback.same);
    CHECK(!same.offersTakeback);
    CHECK(hasKey(same.script, "tb.same"));
    checkScript(same.script, "retry same");

    // Declined: nothing is remembered, the next move is an ordinary one.
    Game g3 = gameOf(kHangFen, {"Nd5"});
    Reviewer rv3;
    rv3.reset(1, White);
    reviewOf(rv3, g3, hangA0());
    rv3.takebackDeclined();
    Review next = reviewOf(rv3, g3, hangA0());
    CHECK(!next.isRetry);

    // No offers when the mode forbids them.
    Game g4 = gameOf(kHangFen, {"Nd5"});
    Reviewer rv4;
    rv4.reset(1, White, false);
    Review noOffer = reviewOf(rv4, g4, hangA0());
    CHECK(noOffer.verdict.voiced);
    CHECK(!noOffer.offersTakeback);
    for (const Beat& b : noOffer.script) CHECK(b.kind != BeatKind::OfferTakeback);
}

TEST(coach_review_takeback_cap_counts_accepted_takebacks) {
    // Level 6 allows two takebacks a game (Band::offerCap). Only those the player accepts count: a
    // card declined costs nothing, and a mate is offered and taken back whatever the count. Past the
    // cap, a blunder is shown without a card and the coach says once that no takeback is left,
    // where the card would have been: before "Now I play my move", which stays last.
    const int cap = band(6).offerCap;
    CHECK_EQ(cap, 2);
    CHECK_EQ(band(4).offerCap, 5);
    CHECK_EQ(band(5).offerCap, 3);
    for (int level = 1; level <= 3; ++level) {
        Reviewer rv;
        rv.reset(level, White);
        CHECK_EQ(rv.takebacksLeft(), -1);   // no cap
    }
    Reviewer off;
    off.reset(6, White, false);
    CHECK_EQ(off.takebacksLeft(), -1);   // no offers at all

    // Two forks at their own plies (two offers per ply is another limit): 4.Nxe5 Qg5 (ply 6) and
    // 1.d4 exd3 e.p. (ply 0). Two mates: 1.a3 Rd1+ 2.Rxd1 Rxd1# (ply 0), and 1.f3 a6 2.h3, missing
    // 2.Rd8+ Rxd8 3.Rxd8# (ply 2).
    const char* epFen = "6k1/4rppp/8/8/4p3/8/2RPN1PP/6K1 w - - 0 1";
    const ai::Analysis epA0 = analysisOf({pvl(0, "g1f1 g8f8"), pvl(-380, "d2d4 e4d3 c2c1 d3e2")});
    const char* mateFen = "3r2k1/3r1ppp/8/8/8/8/P4PPP/2R3K1 w - - 0 1";
    const ai::Analysis mateA0 =
        analysisOf({pvl(-20, "h2h3 d7d2"), pvl(-30, "g2g3 d7d2"), pvl(0, "a2a3 d7d1 c1d1 d8d1", -2)});
    const char* missFen = "2r3k1/p4ppp/8/8/8/8/3R1PPP/3R2K1 w - - 0 1";
    const ai::Analysis missA0 = analysisOf({pvl(0, "d2d8 c8d8 d1d8", 2), pvl(100, "h2h3 a6a5")});
    struct Step {
        const char* name;
        int kind;          // 0: the fork after the opening, 1: the en passant fork, 2: the mate allowed, 3: the mate missed
        bool accept;
        bool offered;
        int leftAfter;
    };
    const Step steps[] = {
        {"fork declined", 0, false, true, 2},
        {"en passant fork declined", 1, false, true, 2},
        {"third card, accepted", 0, true, true, 1},
        {"fourth card, accepted", 1, true, true, 0},
        {"fork past the cap", 0, false, false, 0},
        {"mate allowed past the cap", 2, true, true, 0},
        {"mate missed past the cap", 3, true, true, 0},
        {"fork again past the cap", 0, false, false, 0},
    };
    Reviewer rv;
    rv.reset(6, White);
    CHECK_EQ(rv.takebacksLeft(), cap);
    int noneLeft = 0;
    for (const Step& st : steps) {
        Game g = st.kind == 0   ? forkGame()
                 : st.kind == 1 ? gameOf(epFen, {"d4"})
                 : st.kind == 2 ? gameOf(mateFen, {"a3"})
                                : gameOf(missFen, {"f3", "a6", "h3"});
        const ai::Analysis a0 = st.kind == 0 ? forkA0() : st.kind == 1 ? epA0 : st.kind == 2 ? mateA0 : missA0;
        const int before = rv.takebacksLeft();
        const Review r = reviewOf(rv, g, a0);
        checkScript(r.script, st.name);
        CHECK(sentences(r.script) <= band(6).sentences);
        if (r.offersTakeback != st.offered) {
            std::fprintf(stderr, "  %s: offer %d\n", st.name, int(r.offersTakeback));
            dump(r.script);
        }
        CHECK_EQ(r.offersTakeback, st.offered);
        CHECK_EQ(rv.takebacksLeft(), before);   // an offer made uses nothing
        if (st.kind == 2) CHECK_EQ(r.verdict.exType, ExType::MateAllowed);
        if (st.kind == 3) CHECK_EQ(r.verdict.exType, ExType::MateMissed);
        if (r.offersTakeback) {
            CHECK_EQ(rv.offerUsesTakeback(), st.kind <= 1);
            if (st.accept) {
                CHECK(g.undo(1));
                rv.takebackAccepted(g);
            } else {
                rv.takebackDeclined();
            }
        }
        CHECK_EQ(rv.takebacksLeft(), st.leftAfter);
        // Past the cap, a fork: shown, then the coach's move; "no takeback left" said the first
        // time only, right before it.
        if (!st.offered) {
            CHECK(demoMoves(r.script) > 0);
            CHECK(!r.script.empty() && r.script.back().line.key == "demo.play_on");
            const bool said = hasKey(r.script, "tb.none_left");
            if (said) {
                CHECK(r.script.size() >= 2 && r.script[r.script.size() - 2].line.key == "tb.none_left");
                CHECK(r.script[r.script.size() - 2].look == Look::Player);
            }
            noneLeft += said ? 1 : 0;
        } else {
            CHECK(!hasKey(r.script, "tb.none_left"));
        }
    }
    CHECK_EQ(noneLeft, 1);
    CHECK_EQ(rv.offersMade(), 6);

    // The pause menu's takeback of an offered move (fromOffer false) is judged the same way, but
    // uses none of the level's takebacks.
    Reviewer menu;
    menu.reset(6, White);
    Game g = gameOf(epFen, {"d4"});
    CHECK(reviewOf(menu, g, epA0).offersTakeback);
    menu.takebackDeclined();
    CHECK(g.undo(1));
    menu.takebackAccepted(g, false);
    CHECK_EQ(menu.takebacksLeft(), cap);
    g.play(g.position().parseSAN("Kf1"));
    CHECK(reviewOf(menu, g, epA0).isRetry);
}

TEST(coach_review_unjudged_without_engine) {
    Game g = gameOf(kHangFen, {"Nd5"});
    Reviewer rv;
    rv.reset(1, White);
    ReviewInput in;
    in.game = &g;
    Review r = rv.review(in);
    CHECK_EQ(r.verdict.cls, MoveClass::Unjudged);
    CHECK(r.script.empty());
    ai::Analysis failed;
    failed.ok = false;
    in.before = &failed;
    CHECK_EQ(rv.review(in).verdict.cls, MoveClass::Unjudged);
}

// ---- Announcements and threat warnings -----------------------------------------------------------

TEST(coach_review_announcements) {
    {
        Game g = gameOf("4k3/8/8/8/8/8/8/R3K3 w - - 0 1", {"Ra8+"});
        Reviewer rv;
        rv.reset(1, White);
        Script s = rv.announce(g);
        CHECK(hasKey(s, "ann.check.human.b1"));
        CHECK(!s.empty() && s[0].priority == Priority::Urgent);
        checkScript(s, "check human");
        rv.reset(4, White);
        CHECK(hasKey(rv.announce(g), "ann.check.human"));
    }
    {
        // The coach checks: explained once at levels 1-2, pointing at the player's king.
        Game g = gameOf("4k3/8/8/8/8/8/8/R3K3 w - - 0 1", {"Ra8+"});
        Reviewer rv;
        rv.reset(1, Black);
        Script s = rv.announce(g);
        CHECK(hasKey(s, "ann.check.coach"));
        CHECK(hasKey(s, "ann.check.coach.first"));
        checkScript(s, "check coach");
        const Beat* first = beatWithKey(s, "ann.check.coach.first");
        if (first && !first->gestures.empty()) CHECK_EQ(first->gestures[0].square, sq("e8"));
        Script again = rv.announce(g);
        CHECK(hasKey(again, "ann.check.coach"));
        CHECK(!hasKey(again, "ann.check.coach.first"));
    }
    {
        Game g = gameOf("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1", {"Rd8#"});
        Reviewer rv;
        rv.reset(1, White);
        Script s = rv.announce(g);
        CHECK(hasKey(s, "ann.mate.human.b1"));
        checkScript(s, "mate human");
        rv.reset(5, Black);
        Script c = rv.announce(g);
        CHECK(hasKey(c, "ann.mate.coach"));
        checkScript(c, "mate coach");
    }
    {
        // No announcement after a quiet move.
        Game g = gameOf(nullptr, {"e4"});
        Reviewer rv;
        rv.reset(1, White);
        CHECK(rv.announce(g).empty());
    }
}

TEST(coach_review_threat_warnings) {
    // The coach's pawn attacks the player's undefended knight (level 1: said and traced).
    Game g = gameOf("4k3/4p3/8/8/3N4/8/8/6K1 b - - 0 1", {"e5"});
    Reviewer rv;
    rv.reset(1, White);
    Script s = rv.coachMoved(g);
    CHECK(hasKey(s, "threat.piece.b1"));
    checkScript(s, "threat piece");
    const Beat* b = beatWithKey(s, "threat.piece.b1");
    if (b) {
        CHECK(b->priority == Priority::Low);
        bool target = false;
        for (const Gesture& gs : b->gestures)
            if (gs.kind == GestureKind::PointPiece && gs.square == sq("d4")) target = true;
        CHECK(target);
    }
    // Level 2 warns only about the queen; level 3 not at all.
    rv.reset(2, White);
    CHECK(rv.coachMoved(g).empty());
    rv.reset(3, White);
    CHECK(rv.coachMoved(g).empty());
    // A mate threat (level 1): the coach's queen threatens Qxg2#... here Qh2# after ...Qh4.
    Game m = gameOf("6k1/8/8/8/8/6P1/5P1P/q4NK1 b - - 0 1", {"Qa7"});
    rv.reset(1, White);
    Script ms = rv.coachMoved(m);
    checkScript(ms, "threat mate");
}

// ---- Regressions ---------------------------------------------------------------------------------

TEST(coach_review_great_praise_forgets_taken_back_moves) {
    // "Great move" needs the human's previous move to have left them at W% <= 45. A blunder taken
    // back from the menu (no offer: the review is not told) must not count as that previous move.
    const ai::Analysis first = analysisOf({pvl(30, "e2e4 e7e5"), pvl(25, "d2d4 d7d5")});
    const ai::Analysis root = analysisOf({pvl(300, "g1f3"), pvl(-100, "b1c3"), pvl(-120, "f1c4")});
    const ai::Analysis queen = analysisOf({pvl(-500, "d1h5")});
    for (bool takeback : {false, true}) {
        Game g = gameOf(nullptr, {"e4"});
        Reviewer rv;
        rv.reset(2, White);
        reviewOf(rv, g, first);
        g.play(g.position().parseSAN("e5"));
        if (takeback) {
            g.play(g.position().parseSAN("Qh5"));
            ReviewInput in;
            in.game = &g;
            in.before = &root;
            in.played = &queen;
            CHECK_EQ(rv.review(in).verdict.cls, MoveClass::Blunder);
            CHECK(g.undo(1));
        }
        g.play(g.position().parseSAN("Nf3"));
        const Review r = reviewOf(rv, g, root);
        CHECK(!r.verdict.great);
        CHECK(hasKey(r.script, "praise.only.b2"));
        CHECK(!hasKey(r.script, "praise.great.b2"));
    }
}

TEST(coach_review_no_praise_for_a_quicker_mate) {
    // Lost to a forced mate either way: grabbing the rook allows mate in one instead of three. The
    // class stays Best (lichess), but the coach does not praise it; at the same distance it does.
    const char* fen = "3q2k1/5ppp/8/3N4/1r6/8/6PP/4R2K w - - 0 1";
    for (int bestMate : {-3, -1}) {
        Game g = gameOf(fen, {"Nxb4"});
        Reviewer rv;
        rv.reset(1, White);
        const ai::Analysis a0 = analysisOf({pvl(0, "e1e8 d8e8", bestMate), pvl(0, "d5b4 d8d1", -1)});
        const Review r = reviewOf(rv, g, a0);
        CHECK_EQ(r.verdict.cls, MoveClass::Best);
        CHECK(r.verdict.goodCapture);
        CHECK_EQ(hasKey(r.script, "praise.capture.b1"), bestMate == -1);
        checkScript(r.script, "quicker mate");
    }
}

TEST(coach_review_quicker_mate_is_no_best_moment) {
    // Nxb4 wins the rook and allows ...Qb1# at once, where Ne3 would have lasted a move longer: the
    // appraisal does not make that capture the best moment either. At the same distance it does.
    const char* fen = "6k1/5ppp/8/3N4/1r6/8/q5PP/7K w - - 0 1";
    for (int bestMate : {-2, -1}) {
        Game g = gameOf(fen, {"Nxb4"});
        Reviewer rv;
        rv.reset(1, White);
        Appraisal ap;
        ap.reset(1, White);
        const ai::Analysis a0 = analysisOf({pvl(0, "d5e3 a2a1 e3f1 a1f1", bestMate), pvl(0, "d5b4 a2b1", -1)});
        const Review r = reviewOf(rv, g, a0);
        CHECK_EQ(r.verdict.cls, MoveClass::Best);
        CHECK_EQ(r.verdict.hastensMate, bestMate == -2);
        ap.add(r);
        g.play(g.position().parseSAN("Qb1#"));
        CHECK(g.isOver());
        const AppraisalStats st = ap.stats(g);
        CHECK_EQ(st.bestMoment == BestMoment::None, bestMate == -2);
        CHECK_EQ(st.bestPly, bestMate == -2 ? -1 : 0);
        bool highlight = false;
        for (const std::string& k : keysOf(ap.script(g, AppraisalContext{})))
            highlight = highlight || startsWith(k, "appraisal.best.");
        CHECK_EQ(highlight, bestMate == -1);
    }
}

TEST(coach_review_pin_defender_freed_by_the_capture) {
    // The knight on e4 is pinned by the rook, but ...Rxd6 leaves the e-file and Nxd6 takes back: the
    // pin does not stop it defending d6, so the coach must not say it does.
    const char* fen = "6k1/pp6/3Br3/8/4N3/8/P6P/4K3 w - - 0 1";
    for (int level : {1, 3, 5}) {
        Game g = gameOf(fen, {"a3"});
        Reviewer rv;
        rv.reset(level, White);
        const ai::Analysis a0 = analysisOf({pvl(0, "d6c5 e6e4"), pvl(-400, "a2a3 e6d6 e4d6 b7b6")});
        const Review r = reviewOf(rv, g, a0);
        CHECK(r.verdict.exType != ExType::Pin);
        for (const std::string& k : keysOf(r.script)) CHECK(!startsWith(k, "ex.pin_defender"));
        checkScript(r.script, "pin defender");
    }
    // ...Bxc5 instead: the rook stays on the e-file, the knight still cannot take back.
    for (int level : {1, 3, 5}) {
        Game g = gameOf("4rbk1/8/8/2B5/4N3/8/P7/4K3 w - - 0 1", {"a3"});
        Reviewer rv;
        rv.reset(level, White);
        const ai::Analysis a0 = analysisOf({pvl(0, "c5f8 e8f8"), pvl(-330, "a2a3 f8c5 e1d1")});
        const Review r = reviewOf(rv, g, a0);
        CHECK(r.verdict.exType == ExType::Pin);
        CHECK(hasKey(r.script, "ex.pin_defender.b" + std::to_string(level)));
        checkScript(r.script, "pin defender held");
    }
}

TEST(coach_review_in_between_move_is_explained) {
    // 24...Qa4?? 25.Nxe7+ Kh8 26.Nxa4: the knight takes the bishop with check before the queen. The
    // demonstration plays all three moves and says why the queen waits.
    const char* fen = "3r1rk1/pp1qbbp1/n4p1p/2pP1N2/1P2PP2/P1N3P1/4QR1P/R1B3K1 b - - 1 24";
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(fen, {"Qa4"});
        Reviewer rv;
        rv.reset(level, Black);
        const ai::Analysis a0 = analysisOf({pvl(-90, "f7g6 f5e7 d7e7 b4b5 a6b8"),
                                            pvl(-1050, "d7a4 f5e7 g8h8 c3a4 f8e8 a4c5")});
        const Review r = reviewOf(rv, g, a0);
        char where[48];
        std::snprintf(where, sizeof where, "in-between b%d", level);
        checkScript(r.script, where);
        CHECK_EQ(r.verdict.cls, MoveClass::Blunder);
        std::vector<std::string> demo;
        for (const Beat& b : r.script)
            if (b.kind == BeatKind::DemoMove) demo.push_back(b.uci);
        // Every demonstration that starts with the check goes on to the queen.
        if (!demo.empty()) {
            const std::vector<std::string> want = {"f5e7", "g8h8", "c3a4"};
            CHECK(demo == want);
            CHECK(hasKey(r.script, level <= 3 ? "demo.my.first.check" : "demo.first"));
        }
        if (level <= 3) CHECK(!demo.empty());
    }
    // The narration alone, from the line: "my knight takes your bishop, and it's check! Your queen
    // can wait"; the coach's queen attacked first: "my queen is attacked, so it takes ... first".
    detail::Ctx c;
    c.human = Black;
    c.coach = White;
    c.ply = 47;
    CHECK(c.p1.setFEN("3r1rk1/pp2bbp1/n4p1p/2pP1N2/qP2PP2/P1N3P1/4QR1P/R1B3K1 w - - 2 25"));
    c.r = replayLine(c.p1, {"f5e7", "g8h8", "c3a4"}, Black);
    detail::Explanation ex;
    ex.demoPlies = 3;
    for (int level : {2, 5}) {
        c.level = level;
        c.b = band(level);
        Script s;
        detail::appendDemo(c, ex, s);
        checkScript(s, "in-between narration");
        const Beat* first = beatWithKey(s, level == 2 ? "demo.my.first.check" : "demo.first");
        CHECK(first != nullptr);
        if (first) {
            CHECK(first->line.arg("t1") != nullptr);
            CHECK_EQ(first->uci, std::string("f5e7"));
        }
        if (level == 2) CHECK(hasKey(s, "demo.my.take"));
    }
    // The coach's rook on a8 is attacked by the queen: it takes the knight on a3 first, then ...exd5.
    CHECK(c.p1.setFEN("r5k1/5ppp/4p3/3Q4/8/N7/5PPP/6K1 b - - 0 1"));
    c.human = White;
    c.coach = Black;
    c.level = 2;
    c.b = band(2);
    c.r = replayLine(c.p1, {"a8a3", "g1h1", "e6d5"}, White);
    {
        Script s;
        detail::appendDemo(c, ex, s);
        checkScript(s, "in-between attacked");
        CHECK(hasKey(s, "demo.my.first.attacked"));
    }
    // A quiet move first: "first, my king goes to f8; your queen can wait".
    c.r = replayLine(c.p1, {"g8f8", "g1h1", "e6d5"}, White);
    {
        Script s;
        detail::appendDemo(c, ex, s);
        checkScript(s, "in-between quiet");
        CHECK(hasKey(s, "demo.my.first.move"));
    }
}

TEST(coach_review_never_shows_a_queen_left_standing) {
    // 1.h3?? leaves the queen on d5 to ...exd5 and the knight on a3 to ...Rxa3 (1.Qa2 saved both).
    // The engine's line takes the knight and lets the queen walk away (2.Qb7): whatever its reason,
    // the table would show the coach ignoring a free queen. The coach never plays that capture.
    const char* fen = "r5k1/5ppp/4p3/3Q4/8/N7/5PPP/6K1 w - - 0 1";
    for (int level = 1; level <= 6; ++level) {
        Game g = gameOf(fen, {"h3"});
        Reviewer rv;
        rv.reset(level, White);
        const ai::Analysis a0 = analysisOf({pvl(20, "d5a2 g8f8"), pvl(-330, "h2h3 a8a3 d5b7 a3a1 g1h2")});
        const Review r = reviewOf(rv, g, a0);
        char where[48];
        std::snprintf(where, sizeof where, "queen left b%d", level);
        checkScript(r.script, where);
        for (const Beat& b : r.script) CHECK(b.uci != "a8a3" && b.uci != "d5b7");
    }
}

TEST(coach_review_promotion_race_names_the_queening_pawn) {
    // ...cxb2 and ...b1=Q: the pawn that queens stands on c3 now (b7 is another pawn on the b-file).
    for (int level : {2, 3}) {
        Game g = gameOf("6k1/1p6/8/8/8/2p5/1P6/3N2K1 w - - 0 1", {"Kf1"});
        Reviewer rv;
        rv.reset(level, White);
        const ai::Analysis a0 = analysisOf({pvl(150, "g1f2"), pvl(-700, "g1f1 c3b2 f1e2 b2b1q")});
        const Review r = reviewOf(rv, g, a0);
        CHECK(r.verdict.exType == ExType::PromotionRace);
        const Beat* b = beatWithKey(r.script, "ex.promotion.b" + std::to_string(level));
        CHECK(b != nullptr);
        if (!b) continue;
        CHECK(b->line.arg("my") && b->line.arg("my")->square == sq("c3"));
        CHECK(b->line.arg("n") && b->line.arg("n")->number == 2);   // c3xb2, b2-b1
        bool arrow = false;
        for (const Mark& m : b->marks)
            if (m.kind == Mark::Kind::Arrow) arrow = m.from == sq("c3") && m.to == sq("b1");
        CHECK(arrow);
        checkScript(r.script, "promotion pawn");
    }
}

TEST(coach_review_promotion_race_knows_the_runner_moved) {
    // Best was exd6 (the knight is pinned) and d7, d8=Q. Pushing that same pawn to e6 instead is not
    // "your pawn could have run to d8", although it leaves the d-file; a king move is.
    const char* fen = "8/8/B2p4/1n2P3/8/3k4/8/7K w - - 0 1";
    for (int level = 2; level <= 6; ++level) {   // d8=Q is the fifth ply: lookahead 3 and up
        for (bool push : {true, false}) {
            Game g = gameOf(fen, {push ? "e6" : "Kg1"});
            Reviewer rv;
            rv.reset(level, White);
            const ai::Analysis a0 =
                analysisOf({pvl(800, "e5d6 d3e4 d6d7 e4e5 d7d8q"),
                            pvl(0, push ? "e5e6 d3e4 e6e7 b5c7" : "h1g1 d3e4 g1f2 d6e5")});
            const Review r = reviewOf(rv, g, a0);
            CHECK_EQ(r.verdict.exType == ExType::PromotionRace, !push);
            CHECK_EQ(hasKey(r.script, "ex.promotion_missed.b" + std::to_string(level)), !push);
            checkScript(r.script, "promotion runner");
        }
    }
}

TEST(coach_review_promotion_race_checks_the_square) {
    // The b-pawn queens because the bishop covers c2, not because the king on d1 is outside its
    // square: no rule-of-the-square line. With the king on g1 it really is outside: the line is said.
    for (int level = 1; level <= 6; ++level) {
        for (bool outside : {false, true}) {
            // White: Kd1 (or Kg1), h2. Black: Kh8, Be4 (covers c2), b3.
            Game g = gameOf(outside ? "7k/8/8/8/4b3/1p6/7P/6K1 w - - 0 1" : "7k/8/8/8/4b3/1p6/7P/3K4 w - - 0 1",
                            {"h3"});
            Reviewer rv;
            rv.reset(level, White);
            const ai::Analysis a0 =
                analysisOf({pvl(-150, outside ? "g1f2" : "d1c1"),
                            pvl(-900, outside ? "h2h3 b3b2 g1f2 b2b1q" : "h2h3 b3b2 d1e2 b2b1q")});
            const Review r = reviewOf(rv, g, a0);
            CHECK_EQ(hasKey(r.script, "ex.promotion.b" + std::to_string(level)), outside);
            if (!outside)
                for (const std::string& k : keysOf(r.script)) CHECK(!startsWith(k, "ex.promotion"));
            checkScript(r.script, "promotion square");
        }
    }
}

TEST(coach_review_promotion_race_counts_the_double_step) {
    // The a7 pawn is six ranks from a1 but needs five moves (a7-a5 first); the king on g2 needs six.
    // The line's numbers are the ones the rule of the square compares: 5 and 6, not 6 and 6.
    for (int level : {5, 6}) {   // a1=Q is the ninth ply: lookahead 7 and up
        Game g = gameOf("7k/p7/8/8/8/8/8/6K1 w - - 0 1", {"Kg2"});
        Reviewer rv;
        rv.reset(level, White);
        const ai::Analysis a0 = analysisOf(
            {pvl(0, "g1f2"), pvl(-800, "g1g2 a7a5 g2f3 a5a4 f3e3 a4a3 e3d3 a3a2 d3c2 a2a1q")});
        const Review r = reviewOf(rv, g, a0);
        CHECK(r.verdict.exType == ExType::PromotionRace);
        const Beat* b = beatWithKey(r.script, "ex.promotion.b" + std::to_string(level));
        CHECK(b != nullptr);
        if (!b) continue;
        CHECK(b->line.arg("n") && b->line.arg("n")->number == 5);
        CHECK(b->line.arg("n2") && b->line.arg("n2")->number == 6);
        checkScript(r.script, "promotion double step");
    }
}

TEST(coach_review_repetition_tip_is_not_a_stalemate) {
    // A winning player repeats the position (level 1): the repetition tip is said, but the move is
    // not recorded as a stalemate fault, so the appraisal gives no stalemate advice about it.
    Game g = gameOf("6k1/5ppp/8/8/8/8/5PPP/3Q2K1 w - - 0 1", {});
    Reviewer rv;
    rv.reset(1, White);
    Appraisal ap;
    ap.reset(1, White);
    const char* sans[] = {"Qd2", "Kh8", "Qd1", "Kg8", "Qd2"};
    const char* best[] = {"d1d5", "", "d2d5", "", "d1d5"};
    Review last;
    for (int i = 0; i < 5; ++i) {
        const Move m = g.position().parseSAN(sans[i]);
        const std::string uci = g.position().toUCI(m);
        g.play(m);
        if (i % 2) continue;
        last = reviewOf(rv, g, analysisOf({pvl(900, best[i]), pvl(i == 4 ? 0 : 880, uci.c_str())}));
        ap.add(last);
    }
    CHECK_EQ(g.repetitionCount(), 2);
    CHECK(hasKey(last.script, "tip.repetition.b1"));
    CHECK(last.verdict.exType != ExType::Stalemate);
    g.resign(Black);
    CHECK(ap.stats(g).theme != ExType::Stalemate);
    for (const Beat& b : ap.script(g, AppraisalContext{}))
        if (const Arg* t = b.line.arg("theme")) CHECK(t->text != "theme.stalemate");
}

// ---- Requests ------------------------------------------------------------------------------------

TEST(coach_review_requests) {
    Game g = gameOf(nullptr, {"e4", "e5", "Nf3"});
    Reviewer rv;
    rv.reset(3, Black);
    ai::AnalysisRequest a0 = rv.beforeRequest(g);
    CHECK_EQ(a0.multiPV, 3);
    CHECK(a0.startFen.empty());
    CHECK_EQ(a0.moves.size(), size_t(3));
    g.play(g.position().parseSAN("Nc6"));
    ai::Analysis before = analysisOf({pvl(0, "b8c6"), pvl(-10, "d7d6"), pvl(-20, "g8f6")});
    CHECK(!Reviewer::needsPlayedRequest(before, "b8c6"));
    CHECK(Reviewer::needsPlayedRequest(before, "a7a6"));
    ai::AnalysisRequest a1 = rv.playedRequest(g, before);
    CHECK_EQ(a1.moves.size(), size_t(3));   // same root as A0
    CHECK_EQ(a1.searchMoves.size(), size_t(1));
    if (!a1.searchMoves.empty()) CHECK_EQ(a1.searchMoves[0], std::string("b8c6"));
    CHECK_EQ(a1.depth, before.depth);
    ai::AnalysisRequest a2 = rv.afterRequest(g);
    CHECK_EQ(a2.moves.size(), size_t(4));
    ai::AnalysisRequest a3 = rv.shallowRequest(g);
    CHECK_EQ(a3.moves.size(), size_t(4));
    CHECK_EQ(a3.depth, 6);
    CHECK_EQ(a3.multiPV, 1);
    // A custom start position is sent as a FEN.
    Game c = gameOf(kHangFen, {"Nd5"});
    CHECK_EQ(rv.beforeRequest(c).startFen, std::string(kHangFen));
}

// ---- The catalog ---------------------------------------------------------------------------------

TEST(coach_review_every_key_exists) {
    const Lines& en = english();
    // Every message the review can say, per level range (families with a ".b<level>" suffix).
    struct Family { const char* key; int from, to; };
    const Family families[] = {
        {"ex.verdict.blunder", 1, 6}, {"ex.verdict.mistake", 3, 6}, {"ex.verdict.inaccuracy", 3, 6},
        {"ex.verdict.missed", 1, 2}, {"ex.better", 3, 6}, {"ex.rewind", 1, 4}, {"ex.offer", 1, 6},
        {"ex.dubious", 1, 2},
        {"ex.result", 1, 3}, {"ex.result_back", 1, 3}, {"ex.material", 1, 6},
        {"ex.mate_allowed", 1, 6}, {"ex.mate_missed", 1, 6}, {"ex.mate_delayed", 5, 6}, {"ex.stalemate", 1, 6},
        {"ex.stalemate_trick", 1, 6}, {"ex.fork", 1, 6}, {"ex.discovered", 1, 6}, {"ex.skewer", 1, 6},
        {"ex.pin", 1, 6}, {"ex.pin_defender", 1, 6}, {"ex.trapped", 1, 6}, {"ex.back_rank", 1, 6},
        {"ex.hanging", 1, 6}, {"ex.hanging_guard", 1, 4}, {"ex.hanging_threat", 1, 3},
        {"ex.exchange_capture", 1, 6}, {"ex.exchange_count", 1, 6}, {"ex.exchange_cheap", 1, 6},
        {"ex.missed_capture", 1, 6}, {"ex.missed_fork", 1, 6}, {"ex.promotion", 1, 6},
        {"ex.promotion_missed", 1, 6}, {"ex.king_shield", 1, 6}, {"ex.king_exposed", 1, 6},
        {"ex.bad_trade", 3, 6}, {"ex.bishop_pair", 5, 6}, {"ex.opposition", 3, 6}, {"ex.push_passed", 1, 6},
        {"ex.king_active", 1, 6}, {"ex.positional", 4, 6}, {"tip.early_queen", 1, 3}, {"tip.queen_grab", 1, 4},
        {"tip.same_piece", 1, 3}, {"tip.castle", 1, 3}, {"tip.knight_rim", 1, 3}, {"tip.flank", 1, 3},
        {"tip.centre", 1, 3}, {"praise.brilliant", 3, 6}, {"praise.great", 2, 6}, {"praise.only", 2, 6},
        {"praise.capture", 1, 2}, {"praise.best", 1, 3}, {"praise.trade", 1, 3}, {"praise.excellent", 3, 4},
        {"threat.mate", 1, 2}, {"threat.piece", 1, 2},
    };
    const char* fixed[] = {
        "ann.check.human.b1", "ann.check.human", "ann.check.coach", "ann.check.coach.first", "ann.double_check.coach", "ann.double_check.human",
        "ann.mate.human.b1", "ann.mate.human", "ann.mate.coach.b1", "ann.mate.coach", "demo.move", "demo.my.move",
        "demo.my.take", "demo.my.check", "demo.my.mate", "demo.your.move", "demo.your.take", "demo.your.back",
        "demo.my.first.take", "demo.my.first.check", "demo.my.first.check_move", "demo.my.first.move",
        "demo.my.first.attacked", "demo.first", "demo.play_on",
        "demo.your.king", "demo.promote", "ex.mate_allowed.tail.b1", "ex.mate_allowed.pattern",
        "ex.mate_allowed.tip.b3", "ex.stalemate.tip.b1", "ex.fork.tail.b1", "ex.fork.tail_king", "ex.fork.more.b3",
        "ex.fork.tip.knight", "ex.point.piece", "ex.point.two", "ex.point.mate",
        "ex.trapped.tip.b1", "ex.back_rank.tip.b1", "ex.hanging.tip.b1", "name.pattern.back_rank",
        "name.pattern.smothered", "name.pattern.support", "name.pattern.ladder", "name.pattern.epaulette",
        "tip.mate_technique.b1", "tip.fifty.b1", "tip.repetition.b1", "praise.sacrifice", "praise.fork.b2",
        "praise.fork.b3", "praise.castle.b1", "praise.saved.b1", "praise.book.b1", "threat.punish.b1",
        "tb.hint.b1", "tb.hint.king.b1", "tb.hint.missed.b1", "tb.hint.mate_missed.b1", "tb.hint.general.b1",
        "tb.fixed.b1", "tb.fixed", "tb.same", "ex.verdict.dubious",
    };
    for (const Family& f : families)
        for (int l = f.from; l <= f.to; ++l) {
            const std::string k = std::string(f.key) + ".b" + std::to_string(l);
            if (!en.variants.count(k)) std::fprintf(stderr, "  missing %s\n", k.c_str());
            CHECK(en.variants.count(k) == 1);
        }
    for (const char* k : fixed) {
        if (!en.variants.count(k)) std::fprintf(stderr, "  missing %s\n", k);
        CHECK(en.variants.count(k) == 1);
    }
    // Often-said messages have 3 phrasings, the others at least 2.
    for (const auto& kv : en.variants) {
        // Names said inside other lines have one form.
        if (startsWith(kv.first, "name.") || startsWith(kv.first, "theme.") || startsWith(kv.first, "appraisal.phase.") ||
            startsWith(kv.first, "appraisal.reason.") || startsWith(kv.first, "appraisal.level."))
            continue;
        // "key.spoken" is the spoken form of the phrasing "key", not a message of its own.
        if (kv.first.size() > 7 && kv.first.compare(kv.first.size() - 7, 7, ".spoken") == 0) continue;
        if (kv.second.size() < 2) std::fprintf(stderr, "  %s has one phrasing\n", kv.first.c_str());
        CHECK(kv.second.size() >= 2);
    }
    for (const char* k : {"ex.verdict.blunder.b1", "ex.offer.b1", "ex.rewind.b1", "demo.my.move", "ann.check.coach",
                          "praise.best.b1", "ex.hanging.b1", "ex.fork.b1"})
        CHECK(en.variants.count(k) && en.variants.at(k).size() >= 3);
}

TEST(coach_review_level1_lines_are_short) {
    // Levels 1-2: at most about 15 words per sentence (a placeholder counts as two words).
    for (const auto& kv : english().variants) {
        if (kv.first.find(".b1") == std::string::npos && kv.first.find(".b2") == std::string::npos) continue;
        for (const std::string& text : kv.second) {
            int words = 0;
            bool inWord = false;
            for (size_t i = 0; i < text.size(); ++i) {
                const char ch = text[i];
                if (ch == '{') {
                    const size_t q = text.find('}', i);
                    words += 2;
                    i = q == std::string::npos ? text.size() : q;
                    inWord = false;
                } else if (ch == '.' || ch == '!' || ch == '?') {
                    if (words > 15) std::fprintf(stderr, "  %s: \"%s\" (%d words)\n", kv.first.c_str(), text.c_str(), words);
                    CHECK(words <= 15);
                    words = 0;
                    inWord = false;
                } else if (ch == ' ' || ch == ',' || ch == ':' || ch == ';') {
                    inWord = false;
                } else if (!inWord) {
                    inWord = true;
                    ++words;
                }
            }
            CHECK(words <= 15);
        }
    }
}

// ---- With the embedded engine --------------------------------------------------------------------

#if defined(SCACELITH_HAS_STOCKFISH)

namespace {

bool waitFor(ai::Engine& e, uint32_t id, ai::Analysis& out, int timeoutMs) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!e.analysisReady(id)) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeoutMs)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return e.takeAnalysis(id, out);
}

}  // namespace

TEST(coach_review_engine_worked_example) {
    ai::Engine e;
    CHECK(e.start());
    CHECK(e.waitReady(30000));
    ai::EngineSettings host;
    host.hashMB = 16;
    host.depth = 6;
    e.configure(host);
    e.newGame();

    Game g = gameOf(nullptr, {"e4", "e5", "Nf3", "Nc6", "Bc4", "Nd4"});
    Reviewer rv;
    rv.reset(1, White);
    ai::AnalysisRequest q0 = rv.beforeRequest(g);
    q0.moveTimeMs = 0;   // depth only: reproducible
    q0.depth = 16;
    ai::Analysis a0, a1, a2;
    CHECK(waitFor(e, e.requestAnalysis(q0), a0, 60000));
    CHECK(a0.ok);
    g.play(g.position().parseSAN("Nxe5"));
    ReviewInput in;
    in.game = &g;
    in.before = &a0;
    if (Reviewer::needsPlayedRequest(a0, "f3e5")) {
        ai::AnalysisRequest q1 = rv.playedRequest(g, a0);
        q1.moveTimeMs = 0;
        CHECK(waitFor(e, e.requestAnalysis(q1), a1, 60000));
        in.played = &a1;
    }
    ai::AnalysisRequest q2 = rv.afterRequest(g);
    q2.moveTimeMs = 0;
    CHECK(waitFor(e, e.requestAnalysis(q2), a2, 60000));
    in.after = &a2;
    Review r = rv.review(in);
    std::fprintf(stderr, "  4.Nxe5: %s, W%% %.1f -> %.1f, explanation %s\n", moveClassName(r.verdict.cls),
                 r.verdict.wBest, r.verdict.wPlayed, exTypeName(r.verdict.exType));
    for (const std::string& k : keysOf(r.script)) std::fprintf(stderr, "    %s\n", k.c_str());
    CHECK(r.verdict.cls == MoveClass::Blunder || r.verdict.cls == MoveClass::Mistake);
    CHECK_EQ(r.verdict.exType, ExType::Fork);
    CHECK(r.verdict.voiced);
    checkScript(r.script, "engine fork");
    const Beat* cause = beatWithKey(r.script, "ex.fork.b1");
    CHECK(cause != nullptr);
    if (cause) {
        const Arg* t1 = cause->line.arg("t1");
        const Arg* t2 = cause->line.arg("t2");
        CHECK(t1 && t2);
        if (t1 && t2) {
            std::set<Square> targets = {t1->square, t2->square};
            CHECK(targets == std::set<Square>({sq("e5"), sq("g2")}));
        }
        const Arg* my = cause->line.arg("my");
        CHECK(my && my->piece == Queen);
    }
    // Level 1: the whole sequence on the table, the queen's move first and the knight taken last.
    std::vector<std::string> demo;
    for (const Beat& b : r.script)
        if (b.kind == BeatKind::DemoMove) demo.push_back(b.uci);
    CHECK(demo.size() >= 3 && demo.size() <= size_t(band(1).demoPlies));
    if (!demo.empty()) {
        CHECK_EQ(demo.front(), std::string("d8g5"));
        CHECK(demo.back().size() >= 4 && demo.back().compare(2, 2, "e5") == 0);
    }
    CHECK(hasKey(r.script, "ex.result.b1"));
}

#endif
