// The coach's speech catalog and rules lesson (src/coach/catalog.*, lesson.*), the paths the other
// tests leave out. The catalog: a file line without '=' and a key defined twice (reported, the rest
// kept), queries about a language or topic it does not have, the piece letters of written moves in
// another language, and a line rendered without what it needs (a pattern, an argument, a move it
// cannot read, a piece or opening it has no name for, a lichess name without the composing
// resolver), text arguments (a key rendered in place, else literal text). The lesson: the reasons
// an attempt is illegal that it names or keeps quiet about, a move judged outside an exercise or
// not legal at all, and the answer to a check that is no mate (the king takes the checker, or a
// piece blocks).
#include "test.h"

#include "coach/catalog.h"
#include "coach/lesson.h"
#include "core/embedded.h"

#include <string>
#include <vector>

using namespace coach;
using chess::parseSquare;

namespace {

std::string realFile(const std::string& lang, const std::string& topic) {
    return embedded::text(("assets/coach/speech/" + lang + "/" + topic + ".lang").c_str());
}

Line lineOf(const char* key) { return Line{key, {}}; }

std::string written(const Catalog& c, const Line& l, const std::string& lang = "en") {
    return c.renderVariant(l, lang, false, 1).text;
}
std::string spoken(const Catalog& c, const Line& l, const std::string& lang = "en") {
    return c.renderVariant(l, lang, true, 1).text;
}

chess::Move moveOf(const char* from, const char* to) {
    chess::Move m;
    m.from = parseSquare(from);
    m.to = parseSquare(to);
    return m;
}

// The index of the lesson's exercise with this solution (-1: none).
int exerciseSolvedBy(const Lesson& lesson, const std::string& solution) {
    for (size_t i = 0; i < lesson.expectations().size(); ++i)
        if (lesson.expectations()[i].solution == solution) return int(i);
    return -1;
}

}  // namespace

TEST(coach_core_session_catalog_file_problems) {
    Catalog c;
    CHECK(c.addFile("en", "first", "x = One.\n"));
    CHECK(!c.addFile("en", "second", "y = Two.\nthis line has no equals sign\nz = Three.\n"));
    CHECK(!c.addFile("en", "third", "x = Again.\nw = Four.\n"));
    const std::vector<std::string> want = {"en/second: line 2: missing '='", "en/third: duplicate key x (first in first)"};
    CHECK(c.problems() == want);
    // The lines around the bad one are kept; the first definition of x wins.
    CHECK(c.has("y") && c.has("z") && c.has("w"));
    const std::string* x = c.find("en", "x");
    CHECK(x != nullptr && *x == "One.");
    CHECK(c.keys("en", "third") == std::vector<std::string>({"w"}));
    // A language or a topic it does not have.
    CHECK(c.keys("zz", "first").empty());
    CHECK(c.keys("en", "nope").empty());
    CHECK(c.topics("zz").empty());
    CHECK(c.find("zz", "x") == nullptr);
    CHECK(!c.has("zz", "x"));
    CHECK_EQ(c.variants("nope"), 0);
    c.clear();
    CHECK(c.problems().empty());
    CHECK(!c.has("x"));
}

TEST(coach_core_session_catalog_piece_letters) {
    const char* german = "K D T L S";   // König, Dame, Turm, Läufer, Springer
    CHECK_EQ(sanWithLetters("Kxe2", german), std::string("Kxe2"));
    CHECK_EQ(sanWithLetters("Qh5+", german), std::string("Dh5+"));
    CHECK_EQ(sanWithLetters("Rad1", german), std::string("Tad1"));
    CHECK_EQ(sanWithLetters("Bb5", german), std::string("Lb5"));
    CHECK_EQ(sanWithLetters("Nf3", german), std::string("Sf3"));
    CHECK_EQ(sanWithLetters("e8=Q#", german), std::string("e8=D#"));
    CHECK_EQ(sanWithLetters("bxa8N", german), std::string("bxa8=S"));
    CHECK_EQ(sanWithLetters("O-O-O", german), std::string("O-O-O"));
    CHECK_EQ(sanWithLetters("Rd1", "K D T"), std::string("Rd1"));   // not five letters: as it is
}

TEST(coach_core_session_catalog_lines_missing_what_they_need) {
    Catalog c;
    CHECK(c.addFile("en", "test",
                    "go = Go to {sq}.\n"
                    "hi = Hello {who}.\n"
                    "mv = Play {move}.\n"
                    "pc = Take {your}.\n"
                    "op = Know {o}.\n"
                    "tx = Note: {t}\n"
                    "tx2 = {t} is fun.\n"
                    "word = chess\n"
                    "opening.family.italian = Italian Game\n"));
    CHECK(c.addFile("fr", "test", "op = Connais {o}.\n"));
    // Spoken, a square needs the language's patterns (file, rank, square): none here.
    Line go = lineOf("go");
    go.with("sq", Arg::ofSquare(parseSquare("e4")));
    CHECK_EQ(written(c, go), std::string("Go to e4."));
    CHECK_EQ(spoken(c, go), std::string("Go to ."));
    // An argument the line was not given.
    CHECK_EQ(written(c, lineOf("hi")), std::string("Hello ."));
    // A move it cannot read: written as given, not spoken.
    Line mv = lineOf("mv");
    mv.with("move", Arg::ofMove("Zz9", ""));
    CHECK_EQ(written(c, mv), std::string("Play Zz9."));
    CHECK_EQ(spoken(c, mv), std::string("Play ."));
    // A piece without its forms (no common file): its plain English type; no piece at all: nothing.
    Line pc = lineOf("pc");
    pc.with("your", Arg::ofPiece(chess::Knight, chess::White, true, parseSquare("f3")));
    CHECK_EQ(written(c, pc), std::string("Take knight."));
    Line none = lineOf("pc");
    none.with("your", Arg::ofPiece(chess::NoPiece, chess::White, true));
    CHECK_EQ(written(c, none), std::string("Take ."));
    // Openings: a bare id is a family; an unknown one says nothing; a lichess name without the
    // composing resolver only in English.
    Line op = lineOf("op");
    op.with("o", Arg::ofOpening("italian"));
    CHECK_EQ(written(c, op), std::string("Know Italian Game."));
    Line unknown = lineOf("op");
    unknown.with("o", Arg::ofOpening("family:nope"));
    CHECK_EQ(written(c, unknown), std::string("Know ."));
    Line lichess = lineOf("op");
    lichess.with("o", Arg::ofOpening("line:Smith Variation"));
    CHECK_EQ(written(c, lichess), std::string("Know Smith Variation."));
    CHECK_EQ(written(c, lichess, "fr"), std::string("Connais ."));
    // Text arguments: a key rendered in place (capitalised at the start of a sentence), else the
    // text itself.
    Line tx = lineOf("tx");
    tx.with("t", Arg::ofText("word"));
    CHECK_EQ(written(c, tx), std::string("Note: chess"));
    Line literal = lineOf("tx");
    literal.with("t", Arg::ofText("plain words"));
    CHECK_EQ(written(c, literal), std::string("Note: plain words"));
    Line start = lineOf("tx2");
    start.with("t", Arg::ofText("word"));
    CHECK_EQ(written(c, start), std::string("Chess is fun."));
}

TEST(coach_core_session_catalog_piece_falls_back_to_english) {
    // A language without the forms of a piece names it in English (logged) rather than not at all.
    Catalog c;
    CHECK(c.addFile("en", "common", realFile("en", "common")));
    CHECK(c.addFile("en", "test", "pc = Take {your}.\n"));
    CHECK(c.addFile("xx", "test", "pc = Prends {your}.\n"));
    Line pc = lineOf("pc");
    pc.with("your", Arg::ofPiece(chess::Knight, chess::White, true, parseSquare("f3")));
    const std::string en = written(c, pc);
    CHECK_EQ(en, std::string("Take your knight."));
    CHECK_EQ(written(c, pc, "xx"), std::string("Prends your knight."));
}

TEST(coach_core_session_lesson_illegal_reasons) {
    struct Case {
        const char* fen;
        const char* from;
        const char* to;
        chess::PieceType promo;
        IllegalReason reason;
        const char* name;
        bool explained;   // the lesson has words for it
    };
    const Case cases[] = {
        {"4k3/8/8/8/8/8/8/4K3 w - - 0 1", "d4", "d5", chess::NoPiece, IllegalReason::NoPiece, "NoPiece", false},
        {"4k3/8/8/8/8/8/8/4K3 w - - 0 1", "e8", "e7", chess::NoPiece, IllegalReason::NotYourTurn, "NotYourTurn", true},
        {"4k3/8/8/3pP3/8/8/8/4K3 w - - 0 1", "e5", "d6", chess::NoPiece, IllegalReason::EnPassantExpired,
         "EnPassantExpired", true},
        {"4k3/8/8/8/8/8/8/r3K2R w K - 0 1", "e1", "g1", chess::NoPiece, IllegalReason::CastlingOutOfCheck,
         "CastlingOutOfCheck", true},
        {"4k3/P7/8/8/8/8/8/4K3 w - - 0 1", "a7", "a8", chess::NoPiece, IllegalReason::NeedsPromotionPiece,
         "NeedsPromotionPiece", false},
    };
    const Lesson lesson;
    for (const Case& k : cases) {
        chess::Position pos;
        CHECK(pos.setFEN(k.fen));
        const IllegalInfo info = whyIllegal(pos, parseSquare(k.from), parseSquare(k.to), k.promo);
        CHECK(info.reason == k.reason);
        CHECK_EQ(std::string(illegalReasonName(info.reason)), std::string(k.name));
        const Script s = lesson.explainIllegal(-1, pos, parseSquare(k.from), parseSquare(k.to), k.promo);
        CHECK_EQ(s.empty(), !k.explained);
        if (!s.empty()) {
            CHECK_EQ(s.front().line.key, std::string("why.") + k.name);
            CHECK(Catalog::shared().has(s.front().line.key));
            CHECK(s.front().priority == Priority::Urgent);
        }
    }
}

TEST(coach_core_session_lesson_judge_edges) {
    const Lesson lesson;
    chess::Position start;
    CHECK(start.setFEN("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1"));
    // Outside an exercise every move is accepted, silently.
    for (int expect : {-1, int(lesson.expectations().size())}) {
        const LessonReaction r = lesson.judge(expect, start, start.parseUCI("e2e4"), 0);
        CHECK(r.accepted);
        CHECK(r.before.empty() && r.after.empty());
        CHECK_EQ(r.undo, 0);
    }
    // A move that is not legal at an exercise (the rook's): explained, nothing to take back.
    int index = -1;
    for (size_t i = 0; i < lesson.expectations().size(); ++i) {
        const Expectation& e = lesson.expectations()[i];
        if (e.accept.size() == 1 && e.accept.front() == "a1a6") index = int(i);
    }
    CHECK(index >= 0);
    if (index < 0) return;
    chess::Position before;
    CHECK(before.setFEN(lesson.expectation(index).fen));
    const LessonReaction r = lesson.judge(index, before, moveOf("a1", "b2"), 0);
    CHECK(!r.accepted);
    CHECK_EQ(r.undo, 0);
    CHECK(r.after.empty());
    CHECK(!r.before.empty() && r.before.front().line.key == "why.WrongGeometry");
}

TEST(coach_core_session_lesson_answers_a_check) {
    // The back-rank mate exercise answers a check that is no mate with the coach's way out of it.
    const Lesson lesson;
    const int backRank = exerciseSolvedBy(lesson, "a1a8");
    CHECK(backRank >= 0);
    if (backRank < 0) return;
    CHECK(lesson.expectation(backRank).kind == Expectation::Kind::Mates);
    const Catalog& cat = Catalog::shared();
    auto demoOf = [](const LessonReaction& r) {
        for (const Beat& b : r.before)
            if (b.kind == BeatKind::DemoMove) return b.uci;
        return std::string();
    };
    {
        // The king takes the rook that checks it.
        chess::Position before;
        CHECK(before.setFEN("6k1/8/8/8/8/8/8/5RK1 w - - 0 1"));
        const LessonReaction r = lesson.judge(backRank, before, before.parseUCI("f1f8"), 0);
        CHECK(!r.accepted);
        CHECK_EQ(r.undo, 1);
        CHECK(!r.before.empty() && r.before.front().line.key == "lesson.check_takes");
        CHECK_EQ(demoOf(r), std::string("g8f8"));
        if (!r.before.empty()) {
            const Arg* your = r.before.front().line.arg("your");
            CHECK(your && your->piece == chess::Rook && your->square == parseSquare("f8"));
            const std::string text = cat.renderVariant(r.before.front().line, "en", false, 1).text;
            CHECK(!text.empty() && text.find('{') == std::string::npos);
        }
    }
    {
        // No square to go to, nothing to take: the knight blocks.
        chess::Position before;
        CHECK(before.setFEN("7k/4n1pp/8/8/8/8/8/R5K1 w - - 0 1"));
        const LessonReaction r = lesson.judge(backRank, before, before.parseUCI("a1a8"), 0);
        CHECK(!r.accepted);
        CHECK(!r.before.empty() && r.before.front().line.key == "lesson.check_block");
        const std::string demo = demoOf(r);
        CHECK(demo == "e7c8" || demo == "e7g8");
        if (!r.before.empty()) {
            const std::string text = cat.renderVariant(r.before.front().line, "en", false, 1).text;
            CHECK(!text.empty() && text.find('{') == std::string::npos);
        }
    }
}
