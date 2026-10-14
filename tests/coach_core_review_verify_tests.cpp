// Tests for the verification of a reason (ReviewInput::verified): the sequence the coach would show
// is confirmed move by move against searches the review asks for one at a time (Review::verifyNext),
// here answered by made-up engines. Two cases the review tests leave aside, on the worked example
// (1.e4 e5 2.Nf3 Nc6 3.Bc4 Nd4 4.Nxe5? Qg5 5.Bxf7+ Kd8 6.O-O Qxe5): a coach move of the line that a
// deeper comparison finds clearly weaker (5...Kd8, when 5...Ke7 is better) is replaced, and the
// reason is looked for again on the new line; and two defences of the player that would lose less,
// whose comparisons are asked for in turn (one search fails, the other finds the defence worse),
// leave the reason standing.
#include "coach_core_review_support.h"

#include "coach/review_internal.h"

#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace chess;
using namespace coach;
using namespace corereview;

namespace {

Game forkGame() { return gameOf(nullptr, {"e4", "e5", "Nf3", "Nc6", "Bc4", "Nd4", "Nxe5"}); }

// The engine's lines before 4.Nxe5 (depth 16): 4...Qg5 forks the knight and g2.
ai::Analysis forkA0() {
    return analysisOf({pvl(138, "f3d4 e5d4 e1g1 c7c6 f1e1 d7d6"), pvl(125, "e1g1 d4f3 d1f3 d8f6 f3g3 d7d6"),
                       pvl(-90, "f3e5 d8g5 c4f7 e8d8 e1g1 g5e5 c2c3 d4e6 d2d3 g7g5")});
}

// The fork's line as A0 has it, from the position after 4.Nxe5.
const std::vector<std::string> kForkLine = {"d8g5", "c4f7", "e8d8", "e1g1", "g5e5", "c2c3", "d4e6", "d2d3", "g7g5"};
// The same line with 5...Ke7 (then 6.O-O Qxe5 7.Bb3 Nxb3 8.axb3).
const std::vector<std::string> kKe7Line = {"d8g5", "c4f7", "e8e7", "e1g1", "g5e5", "f7b3", "d4b3", "a2b3"};

std::string fenAlong(const Game& g, const std::vector<std::string>& line, size_t plies) {
    Position q = g.position();
    for (size_t i = 0; i < plies && i < line.size(); ++i) q.makeMove(q.parseUCI(line[i]));
    return q.fen();
}

// The rest of 'line' from 'fen' (a position along it), empty when the line does not pass there.
std::vector<std::string> restOf(const Game& g, const std::vector<std::string>& line, const std::string& fen) {
    for (size_t i = 0; i <= line.size(); ++i)
        if (fenAlong(g, line, i) == fen) return std::vector<std::string>(line.begin() + long(i), line.end());
    return {};
}

ai::PvLine lineOf(int cp, const std::vector<std::string>& pv) {
    ai::PvLine l = pvl(cp, "");
    l.pv = pv;
    return l;
}

// A search agreeing with 'line' wherever the line passes: that line alone from there; a comparison
// of two moves, the line's move first (0) and the other far behind (-400). A failed search (ok
// false) for anything else, which the test reports.
ai::Analysis agreeing(const Game& g, const std::vector<std::string>& line, const std::string& key, bool& unknown) {
    const size_t bar = key.find(" | ");
    const std::vector<std::string> rest = restOf(g, line, key.substr(0, bar));
    if (rest.empty()) {
        unknown = true;
        return ai::Analysis{};
    }
    std::vector<ai::PvLine> lines = {lineOf(0, rest)};
    if (bar != std::string::npos) {
        std::istringstream ms(key.substr(bar + 3));
        std::string m;
        while (ms >> m)
            if (m != rest.front()) lines.push_back(lineOf(-400, {m}));
    }
    return analysisOf(lines, Reviewer::kVerifyDepth);
}

bool logHas(const Review& r, const std::string& text) {
    for (const std::string& l : r.verifyLog)
        if (l.find(text) != std::string::npos) return true;
    return false;
}

void dumpLog(const Review& r, const char* where) {
    for (const std::string& l : r.verifyLog) std::fprintf(stderr, "  %s: %s\n", where, l.c_str());
}

}  // namespace

TEST(coach_core_review_verify_replaces_a_weaker_coach_move) {
    const ai::Analysis a0 = forkA0();
    const Game g = forkGame();
    // After 5.Bxf7+, the short search prefers 5...Ke7 to the line's 5...Kd8, and the deeper
    // comparison agrees by far: the line goes on with 5...Ke7 6.O-O Qxe5 7.Bb3, which still wins
    // the knight for two pawns. The fork stands, on the changed line.
    const std::string atKing = fenAlong(g, kForkLine, 2);
    const ai::Analysis kingSearch =
        analysisOf({lineOf(150, {kKe7Line.begin() + 2, kKe7Line.end()}), lineOf(-100, {"e8d8", "e1g1", "g5e5", "c2c3"})},
                   Reviewer::kVerifyDepth);
    const std::string kingCompare = detail::compareKey(atKing, "e8d8", "e8e7");
    CHECK_EQ(kingCompare, atKing + " | e8d8 e8e7");
    for (int level : {1, 4}) {
        Reviewer rv;
        rv.reset(level, White);
        std::map<std::string, ai::Analysis> v;
        ReviewInput in;
        in.game = &g;
        in.before = &a0;
        in.verified = &v;
        std::vector<std::string> asked;
        bool unknown = false;
        Review r = rv.probe(in);
        while (!r.verifyNext.empty() && int(asked.size()) < Reviewer::kVerifyMax) {
            const std::string key = r.verifyNext;
            asked.push_back(key);
            if (key == atKing || key == kingCompare) v[key] = kingSearch;
            else if (!restOf(g, kKe7Line, key.substr(0, key.find(" | "))).empty()) v[key] = agreeing(g, kKe7Line, key, unknown);
            else v[key] = agreeing(g, kForkLine, key, unknown);
            r = rv.probe(in);
        }
        CHECK(!unknown);
        CHECK(r.verifyNext.empty());
        // The comparison was asked for right after the search that found 5...Kd8 weaker.
        bool compared = false;
        for (size_t i = 1; i < asked.size(); ++i) compared = compared || (asked[i - 1] == atKing && asked[i] == kingCompare);
        CHECK(compared);
        r = rv.review(in);
        const std::string where = "coach move replaced b" + std::to_string(level);
        checkScript(r.script, where.c_str());
        CHECK_EQ(r.claim.type, ExType::Fork);
        CHECK(r.claim.lineChanged);
        CHECK_EQ(r.claim.lost, 1);
        CHECK(r.refutation.size() >= 3);
        if (r.refutation.size() >= 3) CHECK_EQ(r.refutation[2], std::string("e8e7"));
        CHECK(logHas(r, "coach e8d8"));
        CHECK(logHas(r, "< e8e7"));
        if (!logHas(r, "coach e8d8")) dumpLog(r, where.c_str());
        // A beginner's demonstration plays the better king move.
        bool ke7 = false, kd8 = false;
        for (const Beat& b : r.script) {
            ke7 = ke7 || (b.kind == BeatKind::DemoMove && b.uci == "e8e7");
            kd8 = kd8 || (b.kind == BeatKind::DemoMove && b.uci == "e8d8");
        }
        CHECK_EQ(ke7, level == 1);
        CHECK(!kd8);
    }
}

TEST(coach_core_review_verify_compares_defences_in_turn) {
    const ai::Analysis a0 = forkA0();
    const Game g = forkGame();
    // After 4...Qg5, the short search finds two other defences as good as 5.Bxf7+ that keep the
    // material: 5.Nf3 and 5.Ng4. Each is compared more deeply with 5.Bxf7+, the one that loses
    // least first: the first comparison fails (no line), the second finds 5.Ng4 clearly worse. No
    // escape shown, the fork stands.
    const std::string atFork = fenAlong(g, kForkLine, 1);
    const ai::Analysis forkSearch =
        analysisOf({pvl(-80, "e5f3 d4f3 d1f3 g5f6"), pvl(-85, "e5g4 d7d5 c4b3"), lineOf(-90, {kForkLine.begin() + 1, kForkLine.end()})},
                   Reviewer::kVerifyDepth);
    const std::string nf3 = detail::compareKey(atFork, "e5f3", "c4f7");
    const std::string ng4 = detail::compareKey(atFork, "e5g4", "c4f7");
    ai::Analysis failed;   // a search that ended without a result
    failed.ok = false;
    const ai::Analysis ng4Worse = analysisOf({lineOf(-60, {kForkLine.begin() + 1, kForkLine.end()}), pvl(-300, "e5g4 d7d5")},
                                             Reviewer::kCompareDepth);
    Reviewer rv;
    rv.reset(3, White);
    std::map<std::string, ai::Analysis> v;
    ReviewInput in;
    in.game = &g;
    in.before = &a0;
    in.verified = &v;
    std::vector<std::string> asked;
    bool unknown = false;
    Review r = rv.probe(in);
    while (!r.verifyNext.empty() && int(asked.size()) < Reviewer::kVerifyMax) {
        const std::string key = r.verifyNext;
        asked.push_back(key);
        if (key == atFork) v[key] = forkSearch;
        else if (key == nf3) v[key] = failed;
        else if (key == ng4) v[key] = ng4Worse;
        else v[key] = agreeing(g, kForkLine, key, unknown);
        r = rv.probe(in);
        // While a comparison is wanted, the log says so.
        if (r.verifyNext == nf3 || r.verifyNext == ng4) CHECK(logHas(r, "to compare: " + r.verifyNext));
    }
    CHECK(!unknown);
    CHECK(r.verifyNext.empty());
    // Both comparisons asked for, 5.Nf3's first, each once.
    int nf3At = -1, ng4At = -1, nf3Count = 0;
    for (size_t i = 0; i < asked.size(); ++i) {
        if (asked[i] == nf3) {
            nf3At = int(i);
            ++nf3Count;
        }
        if (asked[i] == ng4) ng4At = int(i);
    }
    CHECK(nf3At >= 0 && ng4At > nf3At);
    CHECK_EQ(nf3Count, 1);
    r = rv.review(in);
    checkScript(r.script, "defences compared");
    CHECK_EQ(r.claim.type, ExType::Fork);
    CHECK_EQ(r.dropped.type, ExType::None);
    CHECK(!r.claim.lineChanged);
    CHECK(logHas(r, "confirmed (2 defences compared)"));
    if (!logHas(r, "confirmed (2 defences compared)")) dumpLog(r, "defences compared");
    CHECK(hasKey(r.script, "ex.fork.b3"));
}
