// Coach speech catalog (src/coach/catalog.*): loading, variants, placeholders spoken and written,
// grammatical forms, anchors, Arabic left-to-right marks, the Chinese speech fallback, and the
// parity of every language's files with English.
#include "test.h"
#include "coach/catalog.h"
#include "core/embedded.h"
#include "i18n/i18n.h"
#include "tts/tts.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <regex>
#include <set>

using coach::Arg;
using coach::Catalog;
using coach::Line;

namespace {

const std::string kLrm = "\xE2\x80\x8E";

// Placeholders of a template: name -> forms used ("" = no form). {@} markers included.
std::map<std::string, std::set<std::string>> placeholders(const std::string& v) {
    std::map<std::string, std::set<std::string>> out;
    static const std::regex re("\\{([A-Za-z0-9_@]+)(?::([A-Za-z0-9_]+))?\\}");
    for (auto it = std::sregex_iterator(v.begin(), v.end(), re); it != std::sregex_iterator(); ++it)
        out[(*it)[1].str()].insert((*it)[2].matched ? (*it)[2].str() : std::string());
    return out;
}

std::set<std::string> names(const std::map<std::string, std::set<std::string>>& p) {
    std::set<std::string> s;
    for (auto& e : p) s.insert(e.first);
    return s;
}

bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool startsWith(const std::string& s, const std::string& pre) { return s.compare(0, pre.size(), pre) == 0; }

// Parity of one language with English (the rules of the files' headers). Returns the problems.
std::vector<std::string> parityProblems(const Catalog& c, const std::string& lang) {
    std::vector<std::string> out;
    for (const std::string& topic : c.topics("en")) {
        if (topic == "openings") continue;   // the openings files, tested by tests/openings_tests.cpp
        std::vector<std::string> enKeys = c.keys("en", topic), keys = c.keys(lang, topic);
        std::set<std::string> have(keys.begin(), keys.end());
        std::set<std::string> want;
        for (auto& k : enKeys)
            if (topic == "common" || !endsWith(k, ".spoken")) want.insert(k);
        for (auto& k : want) {
            if (!have.count(k)) { out.push_back(lang + "/" + topic + ": missing " + k); continue; }
            const std::string* ev = c.find("en", k);
            const std::string* lv = c.find(lang, k);
            if (lv->empty()) out.push_back(lang + "/" + topic + ": empty " + k);
            if (names(placeholders(*ev)) != names(placeholders(*lv)))
                out.push_back(lang + "/" + topic + ": placeholders differ in " + k);
            if (startsWith(k, "count.")) {
                size_t forms = 1 + size_t(std::count(lv->begin(), lv->end(), '|'));
                if (forms != size_t(i18n::pluralForms(lang)))
                    out.push_back(lang + "/" + topic + ": " + k + " needs " + std::to_string(i18n::pluralForms(lang)) +
                                  " plural forms");
            }
        }
        if (topic == "common") continue;   // grammar keys and .spoken overrides may be added
        for (auto& k : keys) {
            if (endsWith(k, ".spoken")) {
                std::string base = k.substr(0, k.size() - 7);
                if (!have.count(base)) out.push_back(lang + "/" + topic + ": " + k + " without " + base);
                else if (names(placeholders(*c.find(lang, k))) != names(placeholders(*c.find(lang, base))))
                    out.push_back(lang + "/" + topic + ": placeholders differ in " + k);
            } else if (!want.count(k)) {
                out.push_back(lang + "/" + topic + ": key not in English " + k);
            }
        }
    }
    for (auto& t : c.topics(lang))
        if (c.keys("en", t).empty() && t != "openings") out.push_back(lang + ": topic not in English " + t);
    return out;
}

// Every form a language's sentences ask for must exist: a counted noun (count.<form>), an opening
// form (opening.*.<form>), or a piece form that resolves for all six pieces and both owners. The
// Analysis mode names a piece by its side (Arg::ofSidePiece): every form its lines ask for, the
// nominative included, must also resolve for the owners "white" and "black".
std::vector<std::string> formProblems(const Catalog& c, const std::string& lang) {
    std::vector<std::string> out;
    std::set<std::string> forms, sideForms, openingForms;
    for (const std::string& topic : c.topics(lang)) {
        for (const std::string& k : c.keys(lang, topic)) {
            if (topic == "openings") {
                size_t dot = k.rfind('.');
                if (dot != std::string::npos) openingForms.insert(k.substr(dot + 1));
                continue;
            }
            for (auto& p : placeholders(*c.find(lang, k)))
                for (auto& f : p.second) {
                    if (!f.empty()) forms.insert(f);
                    if (topic == "analysis") sideForms.insert(f.empty() ? "nom" : f);
                }
        }
    }
    static const char* const kTypes[] = {"pawn", "knight", "bishop", "rook", "queen", "king"};
    auto check = [&](const std::set<std::string>& fs, std::initializer_list<const char*> owners) {
        for (const std::string& f : fs) {
            if (c.has(lang, "count." + f) || openingForms.count(f) || f == "letters" || f == "nomate") continue;
            std::string missing;
            for (std::string owner : owners) {
                for (std::string type : kTypes) {
                    if (c.has(lang, "phrase." + owner + "." + type + "." + f)) continue;
                    const std::string* g = c.find(lang, "piece." + type + ".gender");
                    bool noun = c.has(lang, "piece." + type + "." + f);
                    bool pat = c.has(lang, "owner." + owner + "." + f) ||
                               (g && c.has(lang, "owner." + owner + "." + f + "." + *g));
                    if (!noun || !pat) missing = owner + " " + type;
                }
            }
            if (!missing.empty()) out.push_back(lang + ": form '" + f + "' is missing for " + missing);
        }
    };
    check(forms, {"your", "my"});
    check(sideForms, {"white", "black"});
    return out;
}

Line line(const std::string& key) { Line l; l.key = key; return l; }

std::string realFile(const std::string& lang, const std::string& topic) {
    return embedded::text(("assets/coach/speech/" + lang + "/" + topic + ".lang").c_str());
}

// A catalog with the real English building blocks and test lines of its own.
void testCatalog(Catalog& c, const std::string& extra) {
    c.addFile("en", "common", realFile("en", "common"));
    c.addFile("en", "test", extra);
}

std::string written(const Catalog& c, const Line& l, const std::string& lang = "en") {
    return c.renderVariant(l, lang, false, 1).text;
}
std::string spoken(const Catalog& c, const Line& l, const std::string& lang = "en") {
    return c.renderVariant(l, lang, true, 1).text;
}

}  // namespace

TEST(coach_catalog_speech_language) {
    CHECK_EQ(coach::speechLanguage("en"), std::string("en"));
    CHECK_EQ(coach::speechLanguage("fr"), std::string("fr"));
    CHECK_EQ(coach::speechLanguage("ja"), std::string("ja"));
    CHECK_EQ(coach::speechLanguage("ar"), std::string("ar"));
    CHECK_EQ(coach::speechLanguage("zh-Hans"), std::string("en"));
    CHECK_EQ(coach::speechLanguage("zh-Hant"), std::string("en"));
    CHECK_EQ(coach::speechLanguage("pt"), std::string("en"));
    CHECK(!coach::speechSupported("zh-Hans"));
}

// The coach speaks the voice's languages (tts::languageSupported, the one list): the same answer
// for every interface language and for codes beyond them.
TEST(coach_catalog_speech_supported_is_the_voice_list) {
    std::vector<std::string> codes = {"", "pt", "EN", "en-GB", "zh"};
    for (const i18n::Language& l : i18n::languages()) codes.push_back(l.code);
    int spoken = 0;
    for (const std::string& c : codes) {
        CHECK_EQ(coach::speechSupported(c), tts::languageSupported(c));
        spoken += coach::speechSupported(c) ? 1 : 0;
    }
    CHECK_EQ(spoken, 8);   // en fr de es ru uk ar ja: every interface language but the two Chinese
}

TEST(coach_catalog_loads_every_topic) {
    Catalog c;
    auto t0 = std::chrono::steady_clock::now();
    CHECK(c.load());
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "  catalog load: %.2f ms, %d languages\n", ms, int(c.languages().size()));
    for (auto& p : c.problems()) std::fprintf(stderr, "  problem: %s\n", p.c_str());
    CHECK(c.problems().empty());
    std::vector<std::string> topics = c.topics("en");
    for (const char* t : {"common", "events", "lesson"})
        CHECK(std::find(topics.begin(), topics.end(), std::string(t)) != topics.end());
    CHECK(c.has("event.your_move"));
    CHECK(c.has("lesson.welcome.hello"));
    CHECK(c.has("piece.knight.nom"));
    CHECK_EQ(c.variants("event.your_move"), 3);
    CHECK_EQ(c.variants("lesson.welcome.hello"), 1);
    CHECK_EQ(c.variants("no.such.key"), 0);
    // number.2.spoken is no variant of anything: variants need their base key.
    CHECK(!c.has("number.2"));
    // Glyphs for the subtitles: figurines and the language's letters.
    std::vector<uint32_t> cps = c.codepoints("en");
    CHECK(std::binary_search(cps.begin(), cps.end(), 0x2658u));
    CHECK(std::binary_search(cps.begin(), cps.end(), uint32_t('k')));
    // The shared instance is the same data.
    CHECK(Catalog::shared().has("event.your_move"));
}

TEST(coach_catalog_seeded_variants_do_not_repeat) {
    Catalog c;
    testCatalog(c, "two = A\ntwo.2 = B\nthree = A\nthree.2 = B\nthree.3 = C\none = only\n");
    CHECK_EQ(c.variants("two"), 2);
    CHECK_EQ(c.variants("three"), 3);
    // Same seed, same pick: the spoken and the written rendering agree.
    for (uint32_t seed = 1; seed < 40; ++seed) {
        Catalog::Rendered s = c.render(line("three"), "en", true, seed);
        Catalog::Rendered w = c.render(line("three"), "en", false, seed);
        CHECK_EQ(s.variant, w.variant);
        CHECK_EQ(s.text, w.text);
    }
    c.resetHistory();
    int last = 0;
    for (uint32_t seed = 100; seed < 160; ++seed) {
        int v = c.pickVariant("two", seed);
        CHECK(v != last);   // two phrasings alternate
        last = v;
    }
    std::vector<int> picks;
    std::set<int> seen;
    for (uint32_t seed = 500; seed < 600; ++seed) {
        int v = c.pickVariant("three", seed);
        seen.insert(v);
        if (picks.size() >= 1) CHECK(v != picks[picks.size() - 1]);
        if (picks.size() >= 2) CHECK(v != picks[picks.size() - 2]);
        picks.push_back(v);
    }
    CHECK_EQ(int(seen.size()), 3);
    CHECK_EQ(c.pickVariant("one", 7), 1);
    // Deterministic: the same seed sequence gives the same picks.
    Catalog d;
    testCatalog(d, "three = A\nthree.2 = B\nthree.3 = C\n");
    for (uint32_t seed = 500; seed < 600; ++seed) CHECK_EQ(d.pickVariant("three", seed), picks[seed - 500]);
    // An explicit variant is clamped to the English count.
    CHECK_EQ(c.renderVariant(line("two"), "en", false, 9).text, std::string("B"));
}

TEST(coach_catalog_pieces_squares_numbers) {
    Catalog c;
    testCatalog(c,
        "p = This is {your}, and that is {my}.\n"
        "cap = {my} attacks {sq}.\n"
        "n = {n} and {n:point}.\n"
        "t = Next: {x}.\n");
    Line l = line("p");
    l.with("your", Arg::ofPiece(chess::Knight, chess::White, true, chess::parseSquare("g1")))
        .with("my", Arg::ofPiece(chess::Bishop, chess::Black, false));
    CHECK_EQ(written(c, l), std::string("This is your knight, and that is my bishop."));
    CHECK_EQ(spoken(c, l), std::string("This is your knight, and that is my bishop."));
    // A piece at the start of a sentence is capitalised, a square never.
    Line k = line("cap");
    k.with("my", Arg::ofPiece(chess::Rook, chess::Black, false)).with("sq", Arg::ofSquare(chess::parseSquare("e4")));
    CHECK_EQ(written(c, k), std::string("My rook attacks e4."));
    CHECK_EQ(spoken(c, k), std::string("My rook attacks ee four."));
    Line a4 = line("cap");
    a4.with("my", Arg::ofPiece(chess::Queen, chess::Black, false)).with("sq", Arg::ofSquare(chess::parseSquare("a4")));
    CHECK_EQ(spoken(c, a4), std::string("My queen attacks ay four."));
    // Numbers: digits written, words spoken; counted nouns take the plural of the count.
    auto num = [&](int n) { Line x = line("n"); x.with("n", Arg::ofNumber(n)); return x; };
    CHECK_EQ(written(c, num(1)), std::string("1 and 1 point."));
    CHECK_EQ(spoken(c, num(1)), std::string("one and one point."));
    CHECK_EQ(written(c, num(3)), std::string("3 and 3 points."));
    CHECK_EQ(spoken(c, num(3)), std::string("three and three points."));
    CHECK_EQ(spoken(c, num(21)), std::string("twenty-one and twenty-one points."));
    CHECK_EQ(spoken(c, num(40)), std::string("forty and forty points."));
    CHECK_EQ(spoken(c, num(105)), std::string("one hundred and five and one hundred and five points."));
    CHECK_EQ(spoken(c, num(-2)), std::string("minus two and minus two points."));
    CHECK_EQ(spoken(c, num(0)), std::string("zero and zero points."));
    CHECK_EQ(spoken(c, num(1234)), std::string("1234 and 1234 points."));   // beyond the words: digits
    // Text: a catalog key rendered in place, or literal text.
    Line t = line("t");
    t.with("x", Arg::ofText("piece.queen.nom"));
    CHECK_EQ(written(c, t), std::string("Next: queen."));
    Line lit = line("t");
    lit.with("x", Arg::ofText("anything else"));
    CHECK_EQ(written(c, lit), std::string("Next: anything else."));
}

TEST(coach_catalog_moves_and_lines) {
    Catalog c;
    testCatalog(c, "m = Play {move}.\nl = The line: {line}.\nw = Like {move:letters}.\n"
                   "n = Then {move:nomate} is checkmate.\nk = Mate after {line:nomate}.\n");
    auto mv = [&](const std::string& san) { Line x = line("m"); x.with("move", Arg::ofMove(san, "")); return x; };
    CHECK_EQ(written(c, mv("Nf3")), std::string("Play \xE2\x99\x98" "f3."));
    CHECK_EQ(spoken(c, mv("Nf3")), std::string("Play knight eff three."));
    CHECK_EQ(written(c, mv("e4")), std::string("Play e4."));
    CHECK_EQ(spoken(c, mv("e4")), std::string("Play ee four."));
    CHECK_EQ(spoken(c, mv("exd5")), std::string("Play ee takes dee five."));
    CHECK_EQ(spoken(c, mv("Nbd2")), std::string("Play knight bee dee two."));
    CHECK_EQ(spoken(c, mv("R1e2")), std::string("Play rook one ee two."));
    CHECK_EQ(spoken(c, mv("Qh4xe1")), std::string("Play queen aitch four takes ee one."));
    CHECK_EQ(spoken(c, mv("O-O")), std::string("Play castles kingside."));
    CHECK_EQ(spoken(c, mv("O-O-O+")), std::string("Play castles queenside, check."));
    CHECK_EQ(written(c, mv("O-O")), std::string("Play O-O."));
    CHECK_EQ(written(c, mv("e8=Q+")), std::string("Play e8=\xE2\x99\x95+."));
    CHECK_EQ(written(c, mv("e8Q")), std::string("Play e8=\xE2\x99\x95."));
    CHECK_EQ(spoken(c, mv("e8=Q+")), std::string("Play ee eight, promoting to a queen, check."));
    CHECK_EQ(spoken(c, mv("Qxf7#")), std::string("Play queen takes eff seven, checkmate."));
    CHECK_EQ(written(c, mv("Qxf7#")), std::string("Play \xE2\x99\x95xf7#."));
    CHECK_EQ(written(c, mv("Bxc4")), std::string("Play \xE2\x99\x97xc4."));
    // "nomate": the sentence says the mate, the voice does not repeat it (written moves keep "#").
    Line nm = line("n");
    nm.with("move", Arg::ofMove("Qxf7#", ""));
    CHECK_EQ(spoken(c, nm), std::string("Then queen takes eff seven is checkmate."));
    CHECK_EQ(written(c, nm), std::string("Then \xE2\x99\x95xf7# is checkmate."));
    Line nc = line("n");
    nc.with("move", Arg::ofMove("Qxf7+", ""));
    CHECK_EQ(spoken(c, nc), std::string("Then queen takes eff seven, check is checkmate."));
    Line nl = line("k");
    nl.with("line", Arg::ofMoves("Qh5 g6 Qxf7#"));
    CHECK_EQ(spoken(c, nl), std::string("Mate after queen aitch five, gee six, queen takes eff seven."));
    CHECK_EQ(written(c, mv("bxc4")), std::string("Play bxc4."));
    Line let = line("w");
    let.with("move", Arg::ofMove("Nf3", "g1f3"));
    CHECK_EQ(written(c, let), std::string("Like Nf3."));
    CHECK_EQ(spoken(c, let), std::string("Like knight eff three."));
    // A line: figurine SAN written, move by move spoken, a recapture said shortly.
    Line ln = line("l");
    ln.with("line", Arg::ofMoves("Nxe5 dxe5 Qg4"));
    CHECK_EQ(written(c, ln), std::string("The line: \xE2\x99\x98xe5 dxe5 \xE2\x99\x95g4."));
    CHECK_EQ(spoken(c, ln), std::string("The line: knight takes ee five, pawn takes, queen gee four."));
    Line numbered = line("l");
    numbered.with("line", Arg::ofMoves("12. Nf3 Nc6 13. Bb5"));
    CHECK_EQ(written(c, numbered), std::string("The line: \xE2\x99\x98" "f3 \xE2\x99\x98" "c6 \xE2\x99\x97" "b5."));
    // Written SAN with the language's letters (the scoresheet's).
    CHECK_EQ(coach::sanWithLetters("Nxe5+", "R D T F C"), std::string("Cxe5+"));
    CHECK_EQ(coach::sanWithLetters("e8=Q", "R D T F C"), std::string("e8=D"));
    CHECK_EQ(coach::figurineSan("Kxe2"), std::string("\xE2\x99\x94xe2"));
}

TEST(coach_catalog_evaluations) {
    Catalog c;
    testCatalog(c, "e = {best}: {eval}.\n");
    auto ev = [&](int cp, int mate) {
        Line x = line("e");
        x.with("best", Arg::ofMove("Nf3", "g1f3")).with("eval", Arg::ofEval(cp, mate));
        return x;
    };
    CHECK_EQ(written(c, ev(130, 0)), std::string("\xE2\x99\x98" "f3: +1.3."));
    CHECK_EQ(spoken(c, ev(130, 0)), std::string("knight eff three: plus one point three."));
    CHECK_EQ(written(c, ev(-84, 0)), std::string("\xE2\x99\x98" "f3: \xE2\x88\x92" "0.8."));
    CHECK_EQ(spoken(c, ev(-84, 0)).find("minus zero point eight") != std::string::npos, true);
    CHECK_EQ(written(c, ev(2, 0)), std::string("\xE2\x99\x98" "f3: 0.0."));
    CHECK_EQ(spoken(c, ev(2, 0)).find(": level.") != std::string::npos, true);
    CHECK_EQ(written(c, ev(0, 4)), std::string("\xE2\x99\x98" "f3: mate in 4."));
    CHECK_EQ(spoken(c, ev(0, 4)).find("mate in four") != std::string::npos, true);
    CHECK_EQ(written(c, ev(0, -2)).find("mated in 2") != std::string::npos, true);
    CHECK_EQ(written(c, ev(1250, 0)).find("+12.5") != std::string::npos, true);
}

TEST(coach_catalog_grammatical_forms) {
    // A made-up language with French-like genders, German-like cases and an Arabic-like phrase.
    Catalog c;
    c.addFile("en", "common", realFile("en", "common"));
    c.addFile("en", "test", "take = Take {my:acc}!\nsee = {your} sees {my}.\n");
    c.addFile("xx", "common",
              "piece.pawn.nom = pion\npiece.knight.nom = cavalier\npiece.bishop.nom = fou\n"
              "piece.rook.nom = tour\npiece.queen.nom = dame\npiece.king.nom = roi\n"
              "piece.rook.gender = f\npiece.queen.gender = f\n"
              "piece.pawn.acc = pionA\npiece.knight.acc = cavalierA\npiece.bishop.acc = fouA\n"
              "piece.rook.acc = tourA\npiece.queen.acc = dameA\npiece.king.acc = roiA\n"
              "owner.your.nom = ton {piece}\nowner.your.nom.f = ta {piece}\n"
              "owner.my.nom = mon {piece}\nowner.my.nom.f = ma {piece}\n"
              "owner.your.acc = tonA {piece}\nowner.my.acc = monA {piece}\nphrase.my.queen.acc = la dame à moi\n");
    c.addFile("xx", "test", "take = Prends {my:acc} !\nsee = {your} voit {my}.\n");
    Line take = line("take");
    take.with("my", Arg::ofPiece(chess::Knight, chess::Black, false));
    CHECK_EQ(written(c, take, "xx"), std::string("Prends monA cavalierA !"));
    Line takeQ = line("take");
    takeQ.with("my", Arg::ofPiece(chess::Queen, chess::Black, false));
    CHECK_EQ(written(c, takeQ, "xx"), std::string("Prends la dame à moi !"));
    Line see = line("see");
    see.with("your", Arg::ofPiece(chess::Rook, chess::White, true)).with("my", Arg::ofPiece(chess::Queen, chess::Black, false));
    CHECK_EQ(written(c, see, "xx"), std::string("Ta tour voit ma dame."));
    Line see2 = line("see");
    see2.with("your", Arg::ofPiece(chess::Knight, chess::White, true)).with("my", Arg::ofPiece(chess::Pawn, chess::Black, false));
    CHECK_EQ(written(c, see2, "xx"), std::string("Ton cavalier voit mon pion."));
    CHECK_EQ(written(c, take), std::string("Take my knight!"));   // English has no "acc": the nominative
    // The form check finds nothing missing here, and finds a missing form.
    CHECK(formProblems(c, "xx").empty());
    c.addFile("xx", "more", "gen = {your:gen}\n");
    std::vector<std::string> fp = formProblems(c, "xx");
    CHECK_EQ(int(fp.size()), 1);
}

TEST(coach_catalog_anchors_land_on_their_words) {
    Catalog c;
    testCatalog(c,
        "a = Übung für Anfänger: {@}hier steht {your} auf {sq}, dann {@2}dort.\n"
        "b = Le {@}fou attaque {sq} — {my} répond.\n");
    Line l = line("a");
    l.with("your", Arg::ofPiece(chess::Knight, chess::White, true)).with("sq", Arg::ofSquare(chess::parseSquare("f3")));
    for (bool sp : {false, true}) {
        Catalog::Rendered r = c.renderVariant(l, "en", sp, 1);
        CHECK(r.text.find('{') == std::string::npos);
        CHECK_EQ(int(r.anchors.size()), 4);
        CHECK_EQ(r.text.substr(size_t(r.anchor("@")), 4), std::string("hier"));
        CHECK_EQ(r.text.substr(size_t(r.anchor("your")), 4), std::string("your"));
        CHECK_EQ(r.text.substr(size_t(r.anchor("sq")), 2), std::string(sp ? "ef" : "f3"));
        CHECK_EQ(r.text.substr(size_t(r.anchor("@2")), 4), std::string("dort"));
        // Anchors come in text order.
        for (size_t i = 1; i < r.anchors.size(); ++i) CHECK(r.anchors[i - 1].offset <= r.anchors[i].offset);
    }
    Line m = line("b");
    m.with("my", Arg::ofPiece(chess::Rook, chess::Black, false)).with("sq", Arg::ofSquare(chess::parseSquare("c4")));
    Catalog::Rendered r = c.renderVariant(m, "en", true, 1);
    CHECK_EQ(r.text.substr(size_t(r.anchor("@")), 3), std::string("fou"));
    CHECK_EQ(r.text.substr(size_t(r.anchor("my")), 2), std::string("my"));
    CHECK_EQ(r.anchor("nothing"), -1);
}

// The words the voice misreads (respell.* in common.lang) are respelled in speech only: whole words
// in any case, ".pause" ones only before a pause; the anchors follow the longer words.
TEST(coach_catalog_respellings) {
    Catalog c;
    c.addFile("en", "common", realFile("en", "common"));
    c.addFile("en", "test", "a = {n:move} {@}x\nb = {n} {@}x\nc = {my} {sq} x\n");
    c.addFile("fr", "common", realFile("fr", "common"));
    c.addFile("fr", "test",
              "a = Mat en {n:move}, puis {@}mat. Matériel, maté, échec et mat\xC2\xA0!\n"
              "b = Huit coups, au coup {n}. Mon {@}huit, dix-huit\xC2\xA0!\n"
              "c = {my} va en {sq}, et c’est mat.\n");
    Line a = line("a");
    a.with("n", Arg::ofNumber(2));
    Catalog::Rendered ra = c.renderVariant(a, "fr", true, 1);
    CHECK_EQ(ra.text, std::string("Matte en deux coups, puis matte. Matériel, maté, échec et matte\xC2\xA0!"));
    CHECK_EQ(ra.text.substr(size_t(ra.anchor("@")), 6), std::string("matte."));
    CHECK_EQ(ra.text.substr(size_t(ra.anchor("n")), 4), std::string("deux"));
    CHECK_EQ(written(c, a, "fr"), std::string("Mat en 2 coups, puis mat. Matériel, maté, échec et mat\xC2\xA0!"));
    Line b = line("b");
    b.with("n", Arg::ofNumber(8));
    Catalog::Rendered rb = c.renderVariant(b, "fr", true, 1);
    CHECK_EQ(rb.text, std::string("Huit coups, au coup huite. Mon huite, dix-huite\xC2\xA0!"));
    CHECK_EQ(rb.text.substr(size_t(rb.anchor("@")), 6), std::string("huite,"));
    Line l = line("c");
    l.with("my", Arg::ofPiece(chess::Rook, chess::Black, false)).with("sq", Arg::ofSquare(chess::parseSquare("e8")));
    Catalog::Rendered rc = c.renderVariant(l, "fr", true, 1);
    CHECK_EQ(rc.text, std::string("Ma tour va en eu huite, et c’est matte."));
    CHECK_EQ(rc.text.substr(size_t(rc.anchor("sq")), 8), std::string("eu huite"));
    // Other languages keep their words.
    c.addFile("de", "test", "a = Matt in {n:move}, {@}mat.\nb = x\nc = x\n");
    CHECK(spoken(c, a, "de").find(" mat.") != std::string::npos);
}

TEST(coach_catalog_arabic_and_chinese) {
    Catalog c;
    c.addFile("en", "common", realFile("en", "common"));
    c.addFile("en", "test", "go = Play {move} to {sq}.\n");
    c.addFile("ar", "test", "go = العب {move} إلى {sq}.\n");
    c.addFile("zh-Hans", "test", "go = 走{move}到{sq}。\n");
    Line l = line("go");
    l.with("move", Arg::ofMove("Nf3", "g1f3")).with("sq", Arg::ofSquare(chess::parseSquare("f3")));
    // Arabic subtitles keep moves and squares left to right; the voice gets no marks.
    std::string w = written(c, l, "ar");
    CHECK(w.find(kLrm + "\xE2\x99\x98" "f3" + kLrm) != std::string::npos);
    CHECK(w.find(kLrm + "f3" + kLrm) != std::string::npos);
    Catalog::Rendered ra = c.renderVariant(l, "ar", false, 1);
    CHECK_EQ(ra.text.substr(size_t(ra.anchor("sq")), 3), kLrm);   // the anchor is the rendering's start
    CHECK(spoken(c, l, "ar").find(kLrm) == std::string::npos);
    CHECK(written(c, l, "en").find(kLrm) == std::string::npos);
    // Chinese: Chinese subtitles, English speech (the same variant).
    CHECK_EQ(written(c, l, "zh-Hans"), std::string("走\xE2\x99\x98" "f3到f3。"));
    CHECK_EQ(spoken(c, l, "zh-Hans"), spoken(c, l, "en"));
    CHECK_EQ(spoken(c, l, "zh-Hans"), std::string("Play knight eff three to eff three."));
    // An unknown language falls back to English.
    CHECK_EQ(written(c, l, "pt"), written(c, l, "en"));
}

TEST(coach_catalog_openings) {
    Catalog c;
    c.addFile("en", "common", realFile("en", "common"));
    c.addFile("en", "test", "o = You opened with {white:def}.\nv = This is the {v}.\nd = {line}\n");
    c.addFile("en", "openings",
              "opening.family.sicilian = Sicilian Defense\nopening.family.sicilian.def = the Sicilian Defense\n"
              "opening.variation.najdorf = Najdorf Variation\nopening.variation.najdorf.spoken = Nigh-dorf Variation\n");
    Line o = line("o");
    o.with("white", Arg::ofOpening("family:sicilian"));
    CHECK_EQ(written(c, o), std::string("You opened with the Sicilian Defense."));
    Line v = line("v");
    v.with("v", Arg::ofOpening("variation:najdorf"));
    CHECK_EQ(written(c, v), std::string("This is the Najdorf Variation."));
    CHECK_EQ(spoken(c, v), std::string("This is the Nigh-dorf Variation."));
    Line d = line("d");
    d.with("line", Arg::ofOpening("line:English Attack"));
    CHECK_EQ(written(c, d), std::string("English Attack"));
    // The composing resolver (OpeningTexts::arg) is asked first.
    c.setOpeningResolver([](const std::string& ref, const std::string& form, const std::string& lang, bool sp) {
        return ref == "line:English Attack" ? std::string(sp ? "the English attack (spoken)" : "the English attack")
                                            : std::string();
    });
    CHECK_EQ(written(c, d), std::string("The English attack"));
    CHECK_EQ(spoken(c, d), std::string("The English attack (spoken)"));
    CHECK_EQ(written(c, o), std::string("You opened with the Sicilian Defense."));
}

// Every language folder has English's keys, variants and placeholders (vacuous while only English
// exists; it guards the translations), and every form its sentences use.
TEST(coach_catalog_language_parity) {
    Catalog c;
    CHECK(c.load());
    for (const std::string& lang : c.languages()) {
        if (lang == "en") continue;
        // A language with only its openings file has no speech lines yet: it falls back to
        // English as a whole, which is consistent. Once it has one speech file it needs them all.
        std::vector<std::string> topics = c.topics(lang);
        if (std::all_of(topics.begin(), topics.end(), [](const std::string& t) { return t == "openings"; })) continue;
        for (auto& p : parityProblems(c, lang)) {
            std::fprintf(stderr, "  %s\n", p.c_str());
            CHECK(false);
        }
    }
    for (const std::string& lang : c.languages()) {
        for (auto& p : formProblems(c, lang)) {
            std::fprintf(stderr, "  %s\n", p.c_str());
            CHECK(false);
        }
    }
    // The checks themselves: a language with a missing key, a missing variant, a renamed
    // placeholder, a wrong plural count, a stray key and an orphan .spoken key.
    Catalog t;
    t.addFile("en", "common", "count.move = {n} move|{n} moves\npiece.pawn.nom = pawn\n");
    t.addFile("en", "events", "a = Hello {your}.\na.2 = Hi {your}.\nb = Bye.\nc = x\nc.spoken = y\n");
    t.addFile("ru", "common", "count.move = {n} ход|{n} хода\npiece.pawn.nom = пешка\npiece.pawn.acc = пешку\n");
    t.addFile("ru", "events", "a = Привет, {you}.\nb = Пока.\nb.spoken = Пока!\nd = ?\nc = x\nq.spoken = z\n");
    std::vector<std::string> p = parityProblems(t, "ru");
    auto has = [&](const std::string& s) {
        for (auto& x : p)
            if (x.find(s) != std::string::npos) return true;
        return false;
    };
    CHECK(has("placeholders differ in a"));
    CHECK(has("missing a.2"));
    CHECK(has("needs 3 plural forms"));
    CHECK(has("key not in English d"));
    CHECK(has("q.spoken without q"));
    CHECK(!has("c.spoken"));        // an English-only .spoken override is not required
    CHECK(!has("b.spoken"));        // nor is a language's own
    CHECK(!has("piece.pawn.acc"));  // grammar keys may be added
    CHECK_EQ(int(p.size()), 5);
}

// The closing words of a game are followed by the handshake line (gameEndScript), each variant
// picked on its own: no sentence of a handshake line is already in a closing line, in any language
// ("All right. Thank you for the game." then "Thank you for the game.").
TEST(coach_catalog_closing_words_do_not_repeat_the_handshake) {
    Catalog c;
    CHECK(c.load());
    auto variantKey = [](const std::string& key, int v) { return v == 1 ? key : key + "." + std::to_string(v); };
    // The sentences of a line, without their end punctuation.
    auto sentences = [](const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        auto flush = [&] {
            size_t b = cur.find_first_not_of(' '), e = cur.find_last_not_of(' ');
            if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
            cur.clear();
        };
        for (size_t i = 0; i < s.size();) {
            size_t len = 0;   // . ! ? and the full-width and Arabic marks
            for (const char* end : {".", "!", "?", "\xE3\x80\x82", "\xEF\xBC\x81", "\xEF\xBC\x9F", "\xD8\x9F"})
                if (s.compare(i, std::strlen(end), end) == 0) len = std::strlen(end);
            if (len) {
                flush();
                i += len;
            } else {
                cur += s[i++];
            }
        }
        flush();
        return out;
    };
    int pairs = 0;
    for (const std::string& lang : c.languages()) {
        for (int h = 1; h <= c.variants("event.end.handshake"); ++h) {
            const std::string* hands = c.find(lang, variantKey("event.end.handshake", h));
            if (!hands) continue;
            for (const char* key : {"event.end.win", "event.end.loss", "event.end.draw", "event.end.resigned"}) {
                for (int v = 1; v <= c.variants(key); ++v) {
                    const std::string* closing = c.find(lang, variantKey(key, v));
                    if (!closing) continue;
                    ++pairs;
                    for (const std::string& s : sentences(*hands)) {
                        if (closing->find(s) == std::string::npos) continue;
                        std::fprintf(stderr, "  %s: \"%s\" then \"%s\"\n", lang.c_str(), closing->c_str(),
                                     hands->c_str());
                        CHECK(false);
                    }
                }
            }
        }
    }
    CHECK(pairs >= 10 * 11);   // 11 closing lines in each of the 10 languages
}

// The English lines of this package render cleanly: no placeholder left, no bare square or SAN
// in a static line (the voice would spell it), at most two subtitle lines.
TEST(coach_catalog_english_lines_render) {
    Catalog c;
    CHECK(c.load());
    std::regex bareSquare("(^|[^A-Za-z0-9])[a-h][1-8]([^A-Za-z0-9]|$)");
    int lines = 0;
    for (const char* topic : {"events", "lesson"}) {
        for (const std::string& k : c.keys("en", topic)) {
            if (endsWith(k, ".spoken")) continue;
            const std::string* v = c.find("en", k);
            CHECK(!std::regex_search(*v, bareSquare));
            CHECK(v->find("O-O") == std::string::npos);
            // Fill every placeholder with a typical value.
            Line l;
            size_t dot = k.rfind('.');
            bool variant = dot != std::string::npos && std::isdigit((unsigned char)k[dot + 1]);
            l.key = variant ? k.substr(0, dot) : k;
            int n = variant ? std::atoi(k.c_str() + dot + 1) : 1;
            for (auto& p : placeholders(*v)) {
                const std::string& name = p.first;
                if (name[0] == '@') continue;
                if (name == "your") l.with(name, Arg::ofPiece(chess::Knight, chess::White, true));
                else if (name == "my") l.with(name, Arg::ofPiece(chess::Bishop, chess::Black, false));
                else if (name == "move") l.with(name, Arg::ofMove("Nf3", "g1f3"));
                else if (name == "chapter") l.with(name, Arg::ofText("lesson.title.castling"));
                else l.with(name, Arg::ofSquare(chess::parseSquare("e4")));
            }
            for (bool sp : {false, true}) {
                Catalog::Rendered r = c.renderVariant(l, "en", sp, n);
                CHECK_EQ(r.variant, n);
                CHECK(!r.text.empty());
                CHECK(r.text.find('{') == std::string::npos);
                if (!sp && r.text.size() > 140) {
                    std::fprintf(stderr, "  too long for two subtitle lines: %s\n", k.c_str());
                    CHECK(false);
                }
                if (sp) CHECK(!std::regex_search(r.text, bareSquare));
            }
            ++lines;
        }
    }
    CHECK(lines > 150);
}
