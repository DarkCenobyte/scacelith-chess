// The shaping of the UI's text (src/ui/text_shape.cpp) and the faces behind it (src/ui/ui_font.cpp)
// on the font atlas the "ui" scene builds: kerning and tracking, Arabic joining and the lam-alef
// ligature, bidirectional runs with numbers and mirrored brackets, combining marks, small capitals
// for the Cyrillic of the title face, the '?' of a missing character, carets, the handwriting
// faces by script and UI language, and the font fallbacks.
//
// A file of its own: text_shape.h's ui::text namespace cannot be in a file that includes ui/ui.h
// (its ui::text() function), which the headless harness includes; the App is started by
// uitest::fontsReady() in tests/ui_font_tests.cpp.
#include "test.h"
#include "i18n/i18n.h"
#include "ui/text_shape.h"
#include "ui/ui_font.h"
#include <cmath>
#include <string>
#include <vector>

namespace uitest {
// The shared "ui" App started and its fonts ready (tests/ui_font_tests.cpp).
bool fontsReady();
}  // namespace uitest

using namespace ui;

namespace {

// Restores English (and the glyphs of its strings) when a test switched the language.
struct English {
    ~English() { i18n::setLanguage("en"); }
};

bool nearly(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// "\u0633\u0644\u0627\u0645" (salam, peace): seen, lam, alef, meem.
const char* const kSalam = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";

std::vector<uint32_t> cps(const text::Run& r) {
    std::vector<uint32_t> out;
    for (const text::PlacedGlyph& g : r.glyphs) out.push_back(g.cp);
    return out;
}

}  // namespace

// A Latin line: one glyph per character left to right, the face's kerning between them, the
// tracking added between letters; the caret before each character, and back.
TEST(ui_text_shape_latin_line) {
    REQUIRE(uitest::fontsReady());
    const text::Run r = text::shapeLine("AVAx", font::FACE_TEXT);
    REQUIRE(r.glyphs.size() == 4u);
    CHECK_EQ(r.sourceCount, 4);
    CHECK(!r.rtl);
    float sum = 0.0f, kern = 0.0f;
    for (size_t i = 0; i < r.glyphs.size(); ++i) {
        const text::PlacedGlyph& g = r.glyphs[i];
        CHECK_EQ(g.source, int(i));
        CHECK_EQ(g.face, int(font::FACE_TEXT));
        CHECK(!g.rtl);
        CHECK(!g.mark);
        CHECK(g.glyph != nullptr);
        if (i > 0) CHECK(g.x > r.glyphs[i - 1].x);
        sum += g.advance;
        if (i > 0) kern += font::kerning(font::FACE_TEXT, r.glyphs[i - 1].glyph->index, g.glyph->index);
    }
    CHECK(nearly(r.advance, sum + kern));
    CHECK(kern < 0.0f);   // "AV" and "VA" are kerned closer in EB Garamond
    // A cached kerning pair answers the same; glyph index 0 kerns nothing.
    CHECK_EQ(font::kerning(font::FACE_TEXT, r.glyphs[0].glyph->index, r.glyphs[1].glyph->index),
             font::kerning(font::FACE_TEXT, r.glyphs[0].glyph->index, r.glyphs[1].glyph->index));
    CHECK_EQ(font::kerning(font::FACE_TEXT, 0, r.glyphs[1].glyph->index), 0.0f);
    // Tracking: 0.1 em between each pair of letters (three pairs).
    const text::Run tracked = text::shapeLine("AVAx", font::FACE_TEXT, 0.1f);
    CHECK(nearly(tracked.advance, r.advance + 0.3f));
    // Carets: 0 before the first letter, the whole width after the last, and back.
    CHECK_EQ(text::caretX(r, 0), 0.0f);
    CHECK(nearly(text::caretX(r, 4), r.advance));
    for (int i = 0; i <= 4; ++i) CHECK_EQ(text::caretIndex(r, text::caretX(r, i)), i);
    CHECK_EQ(text::caretIndex(r, -5.0f), 0);
    CHECK_EQ(text::caretIndex(r, 100.0f), 4);
}

// Characters never drawn (joiners, a variation selector, a byte order mark) take no glyph; a
// character no face has is drawn as '?', still standing for its source; an empty line has no
// glyph and the base direction it is given.
TEST(ui_text_shape_invisible_missing_and_empty) {
    REQUIRE(uitest::fontsReady());
    const text::Run r = text::shapeLine("a\xE2\x80\x8D" "b\xEF\xB8\x8F\xEF\xBB\xBF", font::FACE_TEXT);   // a ZWJ b VS16 BOM
    CHECK_EQ(r.sourceCount, 5);
    REQUIRE(r.glyphs.size() == 2u);
    CHECK_EQ(r.glyphs[1].source, 2);
    const text::Run missing = text::shapeLine("x\xEE\x80\x80", font::FACE_TEXT);   // U+E000, private use
    REQUIRE(missing.glyphs.size() == 2u);
    CHECK_EQ(missing.glyphs[1].cp, 0xE000u);
    CHECK(missing.glyphs[1].glyph == font::glyph(font::FACE_TEXT, '?'));
    CHECK_EQ(missing.glyphs[1].source, 1);
    CHECK_EQ(font::resolveFace(font::FACE_TEXT, 0xE000), -1);
    CHECK(font::glyph(font::FACE_TEXT, 0xE000) == nullptr);
    const text::Run empty = text::shapeLine("", font::FACE_TEXT);
    CHECK(empty.glyphs.empty());
    CHECK_EQ(empty.sourceCount, 0);
    CHECK(!empty.rtl);
    CHECK_EQ(empty.advance, 0.0f);
    const text::Run emptyRtl = text::shapeLine("", font::FACE_TEXT, 0.0f, 1);
    CHECK(emptyRtl.rtl);
    CHECK_EQ(text::caretX(emptyRtl, 0), 0.0f);
}

// Arabic: contextual forms (an initial seen, the lam-alef ligature in its final form, an isolated
// meem after the alef that does not join), drawn right to left by the Arabic face at its size
// factor; the caret before the first letter is at the right end.
TEST(ui_text_shape_arabic_joining_and_direction) {
    REQUIRE(uitest::fontsReady());
    const text::Run r = text::shapeLine(kSalam, font::FACE_TEXT);
    CHECK_EQ(r.sourceCount, 4);
    CHECK(r.rtl);
    REQUIRE(r.glyphs.size() == 3u);
    // Visual order, left to right: meem, lam-alef, seen.
    CHECK_EQ(r.glyphs[0].cp, 0xFEE1u);   // meem, isolated
    CHECK_EQ(r.glyphs[1].cp, 0xFEFCu);   // lam-alef, final (joined to the seen)
    CHECK_EQ(r.glyphs[2].cp, 0xFEB3u);   // seen, initial
    CHECK_EQ(r.glyphs[2].source, 0);
    CHECK_EQ(r.glyphs[0].source, 3);
    for (const text::PlacedGlyph& g : r.glyphs) {
        CHECK(g.rtl);
        CHECK_EQ(g.face, int(font::FACE_ARABIC));
        CHECK(nearly(g.scale, font::fallbackScale(font::FACE_TEXT, font::FACE_ARABIC)));
    }
    CHECK(font::fallbackScale(font::FACE_TEXT, font::FACE_ARABIC) > 1.0f);
    // No tracking inside an Arabic word: its letters are joined.
    CHECK(nearly(text::shapeLine(kSalam, font::FACE_TEXT, 0.2f).advance, r.advance));
    // Carets: before the first letter (on the right), after the last (on the left).
    CHECK(nearly(text::caretX(r, 0), r.advance));
    CHECK(nearly(text::caretX(r, 4), 0.0f));
    CHECK_EQ(text::caretIndex(r, r.advance), 0);
    // The forced left-to-right base keeps the word reversed inside its own run.
    const text::Run ltr = text::shapeLine(kSalam, font::FACE_TEXT, 0.0f, 0);
    CHECK(!ltr.rtl);
    CHECK_EQ(cps(ltr), cps(r));
}

// Bidirectional text: an Arabic word inside a left-to-right line is reversed in place; numbers
// after Arabic belong to its run (placed to its left, still read left to right), in a
// right-to-left line too; brackets are mirrored in right-to-left runs.
TEST(ui_text_shape_bidirectional_runs) {
    REQUIRE(uitest::fontsReady());
    const text::Run mixed = text::shapeLine(std::string("Hi ") + kSalam + " ok", font::FACE_TEXT);
    CHECK(!mixed.rtl);
    REQUIRE(mixed.glyphs.size() == 9u);
    CHECK_EQ(mixed.glyphs[0].cp, uint32_t('H'));
    CHECK_EQ(mixed.glyphs[3].cp, 0xFEE1u);   // the word's last letter comes first on screen
    CHECK_EQ(mixed.glyphs[5].cp, 0xFEB3u);
    CHECK_EQ(mixed.glyphs[7].cp, uint32_t('o'));
    CHECK_EQ(mixed.glyphs[8].cp, uint32_t('k'));
    const text::Run digits = text::shapeLine(std::string("Hi ") + kSalam + " 12", font::FACE_TEXT);
    REQUIRE(digits.glyphs.size() == 9u);
    CHECK_EQ(digits.glyphs[3].cp, uint32_t('1'));
    CHECK_EQ(digits.glyphs[4].cp, uint32_t('2'));
    CHECK_EQ(digits.glyphs[8].cp, 0xFEB3u);   // the word's first letter ends the line
    const text::Run numbers = text::shapeLine(std::string(kSalam) + " 123", font::FACE_TEXT);
    CHECK(numbers.rtl);
    REQUIRE(numbers.glyphs.size() >= 4u);
    CHECK_EQ(numbers.glyphs[0].cp, uint32_t('1'));   // on the left, still read 1, 2, 3
    CHECK_EQ(numbers.glyphs[1].cp, uint32_t('2'));
    CHECK_EQ(numbers.glyphs[2].cp, uint32_t('3'));
    const text::Run brackets = text::shapeLine(std::string("(") + kSalam + ")", font::FACE_TEXT);
    REQUIRE(brackets.glyphs.size() == 5u);
    CHECK_EQ(brackets.glyphs.back().source, 0);
    CHECK_EQ(brackets.glyphs.back().cp, uint32_t(')'));   // the opening bracket, mirrored, on the right
    CHECK_EQ(brackets.glyphs.front().cp, uint32_t('('));
    // A caret in the middle of the mixed line: after "Hi " it stands at the Arabic word's right end.
    CHECK(text::caretX(mixed, 3) > mixed.glyphs[5].x);
}

// Combining marks take no advance and sit centred over their base (Latin and Arabic); a mark with
// no base stays where the pen is.
TEST(ui_text_shape_combining_marks) {
    REQUIRE(uitest::fontsReady());
    const text::Run e = text::shapeLine("e", font::FACE_TEXT);
    const text::Run acute = text::shapeLine("e\xCC\x81", font::FACE_TEXT);   // e + U+0301
    REQUIRE(acute.glyphs.size() == 2u);
    const text::PlacedGlyph& base = acute.glyphs[0];
    const text::PlacedGlyph& mark = acute.glyphs[1];
    CHECK(mark.mark);
    CHECK_EQ(mark.advance, 0.0f);
    CHECK_EQ(mark.source, 1);
    CHECK(nearly(acute.advance, e.advance));
    const float baseCentre = base.x + 0.5f * (base.glyph->x0 + base.glyph->x1) * base.scale;
    const float markCentre = mark.x + 0.5f * (mark.glyph->x0 + mark.glyph->x1) * mark.scale;
    CHECK(nearly(baseCentre, markCentre, 1e-3f));
    // Carets skip the mark: after "e" and after "e + mark" are the same place.
    CHECK(nearly(text::caretX(acute, 2), acute.advance));
    const text::Run fatha = text::shapeLine("\xD8\xA8\xD9\x8E", font::FACE_TEXT);   // beh + fatha
    REQUIRE(fatha.glyphs.size() == 2u);
    int marks = 0;
    for (const text::PlacedGlyph& g : fatha.glyphs) marks += g.mark ? 1 : 0;
    CHECK_EQ(marks, 1);
    const text::Run lone = text::shapeLine("\xCC\x81" "a", font::FACE_TEXT);
    REQUIRE(lone.glyphs.size() == 2u);
    CHECK(lone.glyphs[0].mark);
    CHECK_EQ(lone.glyphs[0].x, 0.0f);
}

// The title face (Cinzel) has no Cyrillic: the text face's capitals stand in, sized on Cinzel's
// capitals, and its lowercase letters become small capitals.
TEST(ui_text_shape_title_face_small_capitals) {
    REQUIRE(uitest::fontsReady());
    const text::Run r = text::shapeLine("\xD0\x9F\xD1\x80\xD0\xB8", font::FACE_TITLE);   // "При"
    REQUIRE(r.glyphs.size() == 3u);
    CHECK_EQ(r.glyphs[0].cp, 0x41Fu);
    CHECK_EQ(r.glyphs[1].cp, 0x420u);   // р drawn as a small Р
    CHECK_EQ(r.glyphs[2].cp, 0x418u);
    CHECK(r.glyphs[0].face != int(font::FACE_TITLE));
    CHECK(r.glyphs[1].scale < r.glyphs[0].scale);
    CHECK(nearly(r.glyphs[0].scale, font::metrics(font::FACE_TITLE).capHeight / font::metrics(r.glyphs[0].face).capHeight));
    // Latin stays in Cinzel at its own size.
    const text::Run latin = text::shapeLine("Ab", font::FACE_TITLE);
    for (const text::PlacedGlyph& g : latin.glyphs) {
        CHECK_EQ(g.face, int(font::FACE_TITLE));
        CHECK_EQ(g.scale, 1.0f);
    }
}

// Handwriting: Latin and Cyrillic in the player's style, every other script in the hand of its
// own script; Han characters in the hand of the UI language (Japanese, Traditional, else
// Simplified Chinese shapes), and the same order for the UI faces' fallbacks.
TEST(ui_text_shape_handwriting_faces_by_script_and_language) {
    REQUIRE(uitest::fontsReady());
    English english;
    CHECK_EQ(std::string(font::handStyleName(font::HAND_CAVEAT)), std::string("Caveat"));
    CHECK_EQ(std::string(font::handStyleName(font::HAND_MARCK)), std::string("Marck Script"));
    CHECK_EQ(std::string(font::handStyleName(font::HAND_BADSCRIPT)), std::string("Bad Script"));
    CHECK_EQ(std::string(font::handStyleName(42)), std::string("Caveat"));
    CHECK(font::isHandwritingFace(font::FACE_HAND_MARCK));
    CHECK(!font::isHandwritingFace(font::FACE_TEXT));
    CHECK(!font::isHandwritingFace(font::FACE_ARABIC));
    for (int style = 0; style < font::HAND_STYLE_COUNT; ++style) {
        CHECK_EQ(font::handwritingFace(style, 'A'), int(font::FACE_HAND_CAVEAT) + style);
        CHECK_EQ(font::handwritingFace(style, 0x416), int(font::FACE_HAND_CAVEAT) + style);   // Ж
        CHECK_EQ(font::handwritingFace(style, 0x628), int(font::FACE_HAND_ARABIC));
        CHECK_EQ(font::handwritingFace(style, 0x3055), int(font::FACE_HAND_JA));            // さ
    }
    CHECK_EQ(font::handwritingFace(-3, 'A'), int(font::FACE_HAND_CAVEAT));   // clamped
    CHECK_EQ(font::handwritingFace(1, 0xE000), int(font::FACE_HAND_MARCK));  // no face has it: the style's
    const uint32_t wang = 0x738B;   // 王
    CHECK_EQ(font::handwritingFace(0, wang), int(font::FACE_HAND_SC));
    CHECK_EQ(font::handwritingFace(0, 0x3002), int(font::FACE_HAND_SC));     // 。
    CHECK_EQ(font::resolveFace(font::FACE_TEXT, wang), int(font::FACE_HAND_SC));
    REQUIRE(i18n::setLanguage("ja"));
    CHECK_EQ(font::handwritingFace(0, wang), int(font::FACE_HAND_JA));
    CHECK_EQ(font::resolveFace(font::FACE_TEXT, wang), int(font::FACE_HAND_JA));
    REQUIRE(i18n::setLanguage("zh-Hant"));
    CHECK_EQ(font::handwritingFace(0, wang), int(font::FACE_HAND_TC));
    CHECK_EQ(font::resolveFace(font::FACE_ITALIC, wang), int(font::FACE_HAND_TC));
    // A whole name in one hand.
    const text::Run hand = text::shapeHandwriting("Li \xE7\x8E\x8B", font::HAND_MARCK);
    REQUIRE(hand.glyphs.size() == 4u);
    CHECK_EQ(hand.glyphs[0].face, int(font::FACE_HAND_MARCK));
    CHECK_EQ(hand.glyphs[3].face, int(font::FACE_HAND_TC));
    CHECK_EQ(hand.glyphs[3].scale, 1.0f);   // no size factor in a hand
    i18n::setLanguage("en");
    CHECK_EQ(font::handwritingFace(0, wang), int(font::FACE_HAND_SC));
    // The kana of a UI face come from the Japanese hand; a hand falls back to the other hands.
    CHECK_EQ(font::resolveFace(font::FACE_TEXT, 0x3055), int(font::FACE_HAND_JA));
    int used = -1;
    CHECK(font::glyph(font::FACE_HAND_ARABIC, 0x3055, &used) != nullptr);
    CHECK_EQ(used, int(font::FACE_HAND_JA));
    // Out-of-range faces read as the text face.
    CHECK(font::glyph(-4, 'A', &used) != nullptr);
    CHECK_EQ(used, int(font::FACE_TEXT));
    CHECK_EQ(font::resolveFace(99, 'A'), int(font::FACE_TEXT));
    CHECK_EQ(font::metrics(99).capHeight, font::metrics(font::FACE_TEXT).capHeight);
    CHECK_EQ(font::fallbackScale(font::FACE_TEXT, font::FACE_TEXT), 1.0f);
    CHECK_EQ(font::fallbackScale(font::FACE_TEXT, -1), 1.0f);
    CHECK_EQ(font::fallbackScale(font::FACE_HAND_CAVEAT, font::FACE_ARABIC), 1.0f);
    CHECK(font::ready());
    CHECK(font::atlasTexture() != 0);
}
