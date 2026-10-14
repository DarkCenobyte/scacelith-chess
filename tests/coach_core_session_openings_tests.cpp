// The coach's openings (src/coach/openings.*, repertoire.*), the paths the other opening tests
// leave out: a book built from malformed metadata (each error it reports) and from lines it cannot
// read (counted, left out); the repertoire table checked against a book that lacks its moves, and
// the coach's opening move from such a book; the announcer naming a rare opening (with and without
// its lichess name), a transposition into White's, Black's or both sides' family, a variation that
// becomes a side's name after the summary, both sides' variations at once, a summary of Black's
// opening alone, a new summary after a takeback, and reset() forgetting the comments a newGame()
// keeps; the composed names of lichess variations in French and German (a proper name, or nothing)
// and the dashes of compound names in speech.
#include "test.h"

#include "coach/openings.h"
#include "coach/repertoire.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace coach;

namespace {

const char* kTsvHeader = "eco\tname\tpgn\n";

std::string family(const std::string& id, const std::string& prefix, const std::string& umbrella = "") {
    return "{\"id\":\"" + id + "\",\"lichess\":[\"" + prefix +
           "\"],\"side\":\"white\",\"tier\":1,\"generic\":true,\"umbrella\":\"" + umbrella +
           "\",\"answer\":\"\",\"structure\":\"closed\",\"character\":\"quiet\"}";
}

std::string metadata(const std::string& families, const std::string& variations = "") {
    return "{\"families\":[" + families + "],\"variations\":[" + variations + "]}";
}

// A book of one line, 1.d4 (and lines it cannot read).
const std::string kQueensPawn = family("qp", "Queen's Pawn Game");

// Plays the moves (SAN) one by one, asking the announcer after each: what it says, by ply.
using Said = std::vector<std::pair<int, std::vector<OpeningLine>>>;
Said announce(OpeningAnnouncer& a, chess::Game& g, const std::string& sans) {
    Said said;
    std::istringstream in(sans);
    std::string t;
    while (in >> t) {
        const chess::Move m = g.position().parseSAN(t);
        CHECK(m.valid());
        if (!m.valid()) break;
        g.play(m);
        const std::vector<OpeningLine> lines = a.update(g);
        if (!lines.empty()) said.push_back({int(g.moves().size()), lines});
    }
    return said;
}

const OpeningLine* lineWith(const Said& said, const std::string& key, int* ply = nullptr) {
    for (const auto& s : said)
        for (const OpeningLine& l : s.second)
            if (l.key == key) {
                if (ply) *ply = s.first;
                return &l;
            }
    return nullptr;
}

std::string argOf(const OpeningLine* l, const std::string& name) {
    if (!l) return std::string();
    for (const auto& a : l->args)
        if (a.first == name) return a.second.text;
    return std::string();
}

int count(const Said& said, const std::string& key) {
    int n = 0;
    for (const auto& s : said)
        for (const OpeningLine& l : s.second) n += l.key == key ? 1 : 0;
    return n;
}

OpeningAnnouncer announcer(int level, chess::Color human) {
    OpeningAnnouncer a;
    a.setLevel(level);
    a.setHumanColor(human);
    return a;
}

}  // namespace

TEST(coach_core_session_openings_book_metadata_errors) {
    const std::vector<std::string> tsv = {std::string(kTsvHeader) + "A40\tQueen's Pawn Game\t1. d4\n"};
    struct Case {
        std::string json, error;
    };
    const Case cases[] = {
        {metadata("{\"id\":\"\",\"lichess\":[]}"), "family without id or lichess prefixes"},
        {metadata(family("qp", "Queen's Pawn Game", "nope")), "unknown umbrella family nope"},
        {metadata(kQueensPawn, "{\"id\":\"v1\",\"family\":\"nope\",\"side\":\"white\",\"lichess\":[\"X\"],\"beginner\":false}"),
         "bad notable variation v1"},
        {metadata(""), "no opening families"},
    };
    for (const Case& k : cases) {
        OpeningBook b;
        std::string error;
        CHECK(!b.build(tsv, k.json, &error));
        CHECK_EQ(error, k.error);
        CHECK(b.empty());
        CHECK(!b.build(tsv, k.json));   // without an error string too
    }
    // Not JSON at all.
    OpeningBook b;
    std::string error;
    CHECK(!b.build(tsv, "{ families", &error));
    CHECK(!error.empty());
}

TEST(coach_core_session_openings_book_unreadable_lines) {
    const std::vector<std::string> tsv = {std::string(kTsvHeader) +
                                          "A40\tQueen's Pawn Game\t1. d4\n"
                                          "a line without its tabs\n"
                                          "A40\tQueen's Pawn Game: Broken\t1. d4 Zz9\n"
                                          "A40\tQueen's Pawn Game: Empty\t\n"};
    OpeningBook b;
    std::string error;
    CHECK(b.build(tsv, metadata(kQueensPawn), &error));
    CHECK(error.empty());
    CHECK_EQ(b.rows(), size_t(3));          // the three lines with their tabs
    CHECK_EQ(b.badTokens(), size_t(2));     // the line without tabs, the move that is no move
    CHECK_EQ(b.positions(), size_t(1));     // only 1.d4 is in the book
    chess::Position p;
    p.makeMove(p.parseSAN("d4"));
    OpeningBook::Hit hit;
    CHECK(b.lookup(p.hash(), &hit));
    CHECK(hit.name >= 0 && b.name(hit.name) == "Queen's Pawn Game");
    CHECK(!hit.hasDeeper);   // the broken line was dropped whole: nothing goes deeper
    // No line it can read: an error.
    OpeningBook none;
    CHECK(!none.build({std::string(kTsvHeader) + "A40\tQueen's Pawn Game\t1. Zz9\n"}, metadata(kQueensPawn), &error));
    CHECK_EQ(error, std::string("no opening lines"));
    CHECK(none.empty());
}

TEST(coach_core_session_openings_repertoire_against_a_small_book) {
    OpeningBook b;
    CHECK(b.build({std::string(kTsvHeader) + "A40\tQueen's Pawn Game\t1. d4\n"}, metadata(kQueensPawn)));
    // The table's first choice, 1.e4, is not in this book.
    std::string error;
    CHECK(!checkRepertoireTable(b, &error));
    CHECK_EQ(error, std::string("choice e4 leaves the book after \"\""));
    CHECK(!checkRepertoireTable(b));
    // The coach's first move from it: the table's choice is out of book, the one teaching line is 1.d4.
    chess::Game g;
    for (int level = 1; level <= 6; ++level) {
        const chess::Move m = repertoireMove(g, level, 7, b);
        CHECK(m.valid());
        CHECK_EQ(g.position().toUCI(m), std::string("d2d4"));
    }
    // After it, nothing (the book ends there).
    g.play(g.position().parseSAN("d4"));
    CHECK(!repertoireMove(g, 3, 7, b).valid());
}

TEST(coach_core_session_openings_rare_openings) {
    {
        // An opening the book has no family for, unnamed at level 1.
        OpeningAnnouncer a = announcer(1, chess::White);
        chess::Game g;
        const Said said = announce(a, g, "Nh3 d5 g3 e5 f4 Bxh3 Bxh3 exf4");
        int ply = -1;
        CHECK(lineWith(said, "opening.say.rare", &ply) != nullptr);
        CHECK_EQ(ply, 8);
        CHECK(lineWith(said, "opening.say.rare_named") == nullptr);
        CHECK_EQ(count(said, "opening.say.rare"), 1);
    }
    {
        // Named after its lichess line from level 3.
        OpeningAnnouncer a = announcer(3, chess::White);
        chess::Game g;
        const Said said = announce(a, g, "a3 a5 b4");
        int ply = -1;
        const OpeningLine* l = lineWith(said, "opening.say.rare_named", &ply);
        CHECK(l != nullptr);
        CHECK_EQ(ply, 3);
        CHECK_EQ(argOf(l, "line"), std::string("line:Anderssen's Opening"));
        if (l) CHECK(OpeningTexts::instance().render(*l, "en", false).find("Anderssen") != std::string::npos);
    }
}

TEST(coach_core_session_openings_transpositions) {
    {
        // A French set-up that White turns into the King's Indian Attack.
        OpeningAnnouncer a = announcer(3, chess::White);
        chess::Game g;
        const Said said = announce(a, g, "e4 e6 d3 d5 Nd2 Nf6 Ngf3 c5 g3 Nc6 Bg2 Be7 O-O O-O Re1");
        const OpeningLine* first = lineWith(said, "opening.say.pw.both");
        CHECK(first != nullptr);
        CHECK_EQ(argOf(first, "black"), std::string("family:french"));
        int ply = -1;
        const OpeningLine* t = lineWith(said, "opening.say.transposed", &ply);
        CHECK_EQ(argOf(t, "opening"), std::string("family:kings_indian_attack"));
        CHECK_EQ(ply, 15);
    }
    {
        // A Catalan-like start that Black's moves make a Queen's Gambit Declined.
        OpeningAnnouncer a = announcer(4, chess::White);
        chess::Game g;
        const Said said =
            announce(a, g, "d4 d5 c4 e6 Nf3 c5 cxd5 exd5 g3 Nc6 Bg2 Nf6 O-O Be7 Nc3 O-O Bg5 Be6 Rc1 b6");
        int ply = -1;
        const OpeningLine* t = lineWith(said, "opening.say.transposed", &ply);
        CHECK_EQ(argOf(t, "opening"), std::string("family:qgd"));
        CHECK_EQ(ply, 20);
    }
    {
        // Knights first: the Four Knights, a family of both sides.
        OpeningAnnouncer a = announcer(3, chess::Black);
        chess::Game g;
        const Said said = announce(a, g, "Nc3 Nf6 Nf3 Nc6 e4 e5");
        const OpeningLine* t = lineWith(said, "opening.say.transposed");
        CHECK_EQ(argOf(t, "opening"), std::string("family:four_knights"));
    }
}

TEST(coach_core_session_openings_variations_named) {
    {
        // The English, then White's set-up gets its own name: the King's English.
        OpeningAnnouncer a = announcer(3, chess::White);
        chess::Game g;
        const Said said = announce(a, g, "c4 g6 Nc3 Bg7 g3 Nf6 Bg2 O-O e4 d6 Nge2 e5 O-O c6 d3 a6");
        int ply = -1;
        CHECK_EQ(argOf(lineWith(said, "opening.say.pw.white", &ply), "white"), std::string("family:english"));
        CHECK_EQ(ply, 12);
        const OpeningLine* now = lineWith(said, "opening.say.now_variation", &ply);
        CHECK_EQ(argOf(now, "variation"), std::string("variation:kings_english"));
        CHECK_EQ(ply, 16);
    }
    {
        // Both sides' variations in one sentence: the Exchange QGD met with the Semi-Tarrasch.
        OpeningAnnouncer a = announcer(4, chess::White);
        chess::Game g;
        const Said said = announce(a, g, "d4 d5 c4 e6 Nc3 Nf6 cxd5 Nxd5 e4 Nxc3 bxc3 c5 Nf3 cxd4 cxd4 Bb4+ Bd2 Qa5");
        int ply = -1;
        CHECK(lineWith(said, "opening.say.pw.declined", &ply) != nullptr);
        const OpeningLine* both = lineWith(said, "opening.say.variations");
        CHECK_EQ(argOf(both, "white"), std::string("variation:qgd_exchange"));
        CHECK_EQ(argOf(both, "black"), std::string("variation:semi_tarrasch"));
        if (both) CHECK(!OpeningTexts::instance().render(*both, "en", false).empty());
    }
    {
        // Only Black's opening has a name (White's first moves have none at level 2).
        OpeningAnnouncer a = announcer(2, chess::Black);
        chess::Game g;
        const Said said = announce(a, g, "g3 c5 e4 Na6");
        const OpeningLine* l = lineWith(said, "opening.say.pb.black");
        CHECK_EQ(argOf(l, "black"), std::string("family:sicilian"));
        CHECK(lineWith(said, "opening.say.pb.both") == nullptr);
    }
}

TEST(coach_core_session_openings_new_summary_after_a_takeback) {
    // The Caro-Kann named; the last move taken back and another played: a new summary with White's
    // new label, the Two Knights variation (no transposition news after a takeback).
    OpeningAnnouncer a = announcer(3, chess::White);
    chess::Game g;
    const Said first = announce(a, g, "e4 c6 Nc3 d5 Qf3");
    int ply = -1;
    const OpeningLine* both = lineWith(first, "opening.say.pw.both", &ply);
    CHECK_EQ(argOf(both, "black"), std::string("family:caro_kann"));
    CHECK_EQ(ply, 5);
    g.undo(1);
    CHECK(a.update(g).empty());
    const Said again = announce(a, g, "Nf3 dxe4 Ng5");
    const OpeningLine* l = lineWith(again, "opening.say.pw.var_white", &ply);
    CHECK(l != nullptr);
    CHECK_EQ(ply, 7);
    CHECK_EQ(argOf(l, "white"), std::string("variation:ck_two_knights"));
    CHECK_EQ(argOf(l, "black"), std::string("family:caro_kann"));
    CHECK(lineWith(again, "opening.say.transposed") == nullptr);
}

TEST(coach_core_session_openings_reset_forgets_comments) {
    // The comment on an opening is said once per session: a new game keeps that memory, reset()
    // forgets it.
    const char* najdorf = "e4 c5 Nf3 d6 d4 cxd4 Nxd4 Nf6 Nc3 a6 Be3 e5";
    auto comments = [](const Said& said) {
        std::vector<std::string> out;
        for (const auto& s : said)
            for (const OpeningLine& l : s.second)
                if (l.key.size() > 9 && l.key.compare(l.key.size() - 9, 9, ".beginner") == 0) out.push_back(l.key);
        return out;
    };
    OpeningAnnouncer a = announcer(1, chess::White);
    chess::Game g1;
    const std::vector<std::string> first = comments(announce(a, g1, najdorf));
    CHECK(!first.empty());
    a.newGame();
    chess::Game g2;
    const std::vector<std::string> second = comments(announce(a, g2, najdorf));
    for (const std::string& k : first)
        for (const std::string& s : second) CHECK(k != s);
    a.reset();
    chess::Game g3;
    const std::vector<std::string> third = comments(announce(a, g3, najdorf));
    CHECK(third == first);
}

TEST(coach_core_session_openings_composed_names) {
    const OpeningTexts& t = OpeningTexts::instance();
    // French, German and Spanish compose "<Proper> <Type>" when the first part is a proper name.
    CHECK_EQ(t.arg("line:Smith Variation", "", "fr", false), std::string("variante Smith"));
    CHECK_EQ(t.arg("line:Smith-Jones Variation", "", "fr", false), std::string("variante Smith-Jones"));
    CHECK(t.arg("line:Smith Attack", "", "de", false).find("Smith") != std::string::npos);
    CHECK(t.arg("line:Smith Gambit", "", "es", false).find("Smith") != std::string::npos);
    // Not a proper name: lower case, a possessive, a digit, a common word.
    for (const char* ref : {"line:smith Variation", "line:Smith's Variation", "line:Smiths' Variation",
                            "line:Smith2 Variation", "line:Amazon Variation"})
        CHECK(t.arg(ref, "", "fr", false).empty());
    // Other languages only through their table of eponyms.
    CHECK(t.arg("line:Smith Variation", "", "ru", false).empty());
    // Speech: " — " between two capitalised words becomes a hyphen; not after another script's
    // character or before a lower-case word.
    CHECK_EQ(fixNameDashesForSpeech("Каро — Канн"), std::string("Каро-Канн"));
    CHECK_EQ(fixNameDashesForSpeech("Caro — Kann"), std::string("Caro-Kann"));
    CHECK_EQ(fixNameDashesForSpeech("中 — Канн"), std::string("中 — Канн"));
    CHECK_EQ(fixNameDashesForSpeech("Caro — kann"), std::string("Caro — kann"));
}
