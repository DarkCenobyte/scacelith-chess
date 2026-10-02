// Tests for the coach's review (src/coach/review.h): win percentage and move classes, the
// explanation chosen for hand-built positions and analyses, demonstrations (depth per level, never
// past the player's promotion, rewind of exactly the moves shown), the English lines (every key the
// scripts use exists, every placeholder has an argument, every gesture and mark anchor is a
// placeholder of every variant, at most three pointing gestures, the sentence budget per level),
// praise only for the best moves and its rate limit, takebacks, announcements, threat warnings,
// analysis requests; and, with the embedded engine, the worked example of research-pedagogy §2.0.
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
int sentences(const Script& s) {
    int n = 0;
    for (const Beat& b : s)
        if (b.kind == BeatKind::Say && !b.line.key.empty() && !startsWith(b.line.key, "demo.") &&
            !startsWith(b.line.key, "tb."))
            ++n;
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
ai::Analysis forkA0() {
    return analysisOf({pvl(40, "f3d4 e5d4 e1g1 g8f6"), pvl(20, "e1g1 d7d6 f3d4 e5d4"),
                       pvl(-350, "f3e5 d8g5 e5f7 g5g2 h1f1 g2e4 c4e2 d4f3")});
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
    // Demonstration depth per level (design §1): 1 at level 1 ... 8 at level 6.
    const int demo[6] = {1, 2, 3, 4, 6, 8};
    for (int l = 1; l <= 6; ++l) {
        CHECK_EQ(band(l).level, l);
        CHECK_EQ(band(l).demoPlies, demo[l - 1]);
        CHECK(band(l).sentences >= 2 && band(l).sentences <= 4);
    }
    CHECK_EQ(band(0).level, 1);
    CHECK_EQ(band(9).level, 6);
}

// ---- Explanations --------------------------------------------------------------------------------

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
        CHECK_EQ(r.verdict.exType, ExType::Fork);   // before king safety / positional (§2.0 order)
        CHECK(r.verdict.voiced);
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
        // Demonstration: 1 ply at level 1, then up to 3 plies capped by the level's depth.
        const int shown = demoMoves(r.script);
        CHECK(shown >= 1 && shown <= band(level).demoPlies);
        CHECK_EQ(shown, level == 1 ? 1 : std::min(3, band(level).demoPlies));
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
    // 3...Nf6?? allows Qxf7#: a capture of a pawn too, but the mate explains it (§2.0 order).
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
        CHECK(r.offersTakeback);
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

    // Never deeper than the level: level 2 shows at most 2 plies of a 4-ply line.
    c.level = 2;
    c.b = band(2);
    CHECK(c.p1.setFEN("4k3/8/8/8/8/7r/1P6/4K3 b - - 0 1"));
    c.r = replayLine(c.p1, {"h3h4", "b2b4", "e8d7", "b4b5"}, White);
    Script s2;
    detail::appendDemo(c, ex, s2);
    checkScript(s2, "depth cap");
    CHECK_EQ(demoMoves(s2), 2);
    if (!s2.empty()) CHECK_EQ(s2.back().count, 2);
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
         {pvl(0, "h1e1 g8f8"), pvl(-600, "b1c2 d5e3 c2d3 b7h1")}, ExType::Discovered, 1, 6},
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
        {"ex.verdict.blunder", 1, 6}, {"ex.verdict.mistake", 3, 6}, {"ex.verdict.inaccuracy", 4, 6},
        {"ex.verdict.missed", 1, 2}, {"ex.better", 3, 6}, {"ex.rewind", 1, 4}, {"ex.offer", 1, 6},
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
        "demo.your.king", "demo.promote", "ex.mate_allowed.tail.b1", "ex.mate_allowed.pattern",
        "ex.mate_allowed.tip.b3", "ex.stalemate.tip.b1", "ex.fork.tail.b1", "ex.fork.more.b3", "ex.fork.tip.knight",
        "ex.trapped.tip.b1", "ex.back_rank.tip.b1", "ex.hanging.tip.b1", "name.pattern.back_rank",
        "name.pattern.smothered", "name.pattern.support", "name.pattern.ladder", "name.pattern.epaulette",
        "tip.mate_technique.b1", "tip.fifty.b1", "tip.repetition.b1", "praise.sacrifice", "praise.fork.b2",
        "praise.fork.b3", "praise.castle.b1", "praise.saved.b1", "praise.book.b1", "threat.punish.b1",
        "tb.hint.b1", "tb.hint.king.b1", "tb.hint.missed.b1", "tb.hint.mate_missed.b1", "tb.hint.general.b1",
        "tb.fixed.b1", "tb.fixed", "tb.same",
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
    CHECK_EQ(demoMoves(r.script), 1);
    bool qg5 = false;
    for (const Beat& b : r.script)
        if (b.kind == BeatKind::DemoMove && b.uci == "d8g5") qg5 = true;
    CHECK(qg5);
}

#endif
