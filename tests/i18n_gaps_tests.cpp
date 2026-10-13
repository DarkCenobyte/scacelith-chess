// Languages and the Unicode logic of the text shaper, the paths tests/i18n_tests.cpp leaves out:
// escapes and errors of the .lang parser, trOr, ltr in a right-to-left language, a plural key
// found nowhere; strong right-to-left characters, the bidi classes of the less common blocks
// (Arabic digits and signs, CJK and full-width punctuation, Hebrew and Arabic presentation forms,
// the supplementary right-to-left planes), terminators joined to numbers (W5), separators inside
// a line (L1), every mirrored pair, the lam-alef ligatures and the Greek and Cyrillic upper case.
#include "test.h"
#include "i18n/i18n.h"
#include "i18n/unicode.h"

#include <string>
#include <utility>
#include <vector>

namespace {

// Restores the language a test found when it ends.
struct KeepLanguage {
    std::string code = i18n::language();
    ~KeepLanguage() { i18n::setLanguage(code); }
};

}  // namespace

TEST(i18n_gaps_parse_escapes_and_errors) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string err;
    CHECK(i18n::parse("a = x\\ny\\tz\\\\w\\=\nb = ends with \\\n", out, &err));
    CHECK(err.empty());
    REQUIRE(out.size() == 2);
    CHECK_EQ(out[0].second, std::string("x\ny\tz\\w="));
    CHECK_EQ(out[1].second, std::string("ends with \\"));
    // The first error is the one reported; the good lines are still read.
    out.clear();
    CHECK(!i18n::parse("ok = 1\nno equals sign here\n = empty key\nlast = 2", out, &err));
    CHECK_EQ(err, std::string("line 2: missing '='"));
    CHECK(out.size() == 2 && out[0].first == "ok" && out[1].first == "last");
    out.clear();
    err.clear();
    CHECK(!i18n::parse("# comment\n= v\nk = v", out, &err));
    CHECK_EQ(err, std::string("line 2: empty key"));
    CHECK(out.size() == 1 && out[0].first == "k");
    out.clear();
    CHECK(!i18n::parse("broken", out));   // no error text asked for
    CHECK(out.empty());
}

TEST(i18n_gaps_tr_or_ltr_and_missing_plural) {
    KeepLanguage keep;
    CHECK(i18n::setLanguage("en"));
    CHECK_EQ(i18n::trOr("gaps.no.such.key", "fallback"), std::string("fallback"));
    CHECK_EQ(i18n::trOr("reason.checkmate", "fallback"), i18n::tr(std::string("reason.checkmate")));
    CHECK_EQ(i18n::trn("gaps.no.such.plural", 3), std::string("gaps.no.such.plural"));
    CHECK_EQ(i18n::ltr("1-0"), std::string("1-0"));
    CHECK(i18n::setLanguage("ar"));
    CHECK_EQ(i18n::ltr("1-0"), std::string("\xE2\x80\x8E" "1-0\xE2\x80\x8E"));
    CHECK_EQ(i18n::ltr(""), std::string());
    CHECK_EQ(i18n::trOr("gaps.no.such.key", "fallback"), std::string("fallback"));
    CHECK_EQ(i18n::trn("gaps.no.such.plural", 11), std::string("gaps.no.such.plural"));
}

TEST(unicode_gaps_strong_right_to_left) {
    for (char32_t c : {char32_t(0x05D0), char32_t(0x0627), char32_t(0xFB1D), char32_t(0xFB50), char32_t(0xFE70),
                       char32_t(0x10800), char32_t(0x1E800), char32_t(0x1EE00), char32_t(0x200F)})
        CHECK(uni::isRtl(c));
    for (char32_t c : {char32_t('a'), char32_t('1'), char32_t(0x0660), char32_t(0x06F0), char32_t(0x3042), char32_t(0x200E),
                       char32_t(0xFD3E), char32_t(0xFEFF)})
        CHECK(!uni::isRtl(c));
    CHECK(!uni::containsRtl(U""));
    CHECK(!uni::containsRtl(U"abc 123 ٠"));
    CHECK(uni::containsRtl(U"abc א"));
}

TEST(unicode_gaps_bidi_classes) {
    using B = uni::BidiClass;
    const std::pair<char32_t, B> cases[] = {
        {'\t', B::S},      {0x0B, B::S},      {0x1F, B::S},       {'\n', B::B},      {'\r', B::B},      {0x1C, B::B},
        {'#', B::ET},      {'$', B::ET},      {'%', B::ET},       {0xAD, B::BN},     {0xB2, B::EN},     {0xB9, B::EN},
        {0xA2, B::ET},     {0xB0, B::ET},     {0xB1, B::ET},      {0x06F0, B::EN},   {0x06F9, B::EN},   {0x060C, B::CS},
        {0x0609, B::ET},   {0x066A, B::ET},   {0x060E, B::ON},    {0x06DE, B::ON},   {0x06E9, B::ON},   {0x3000, B::WS},
        {0x3001, B::ON},   {0x3008, B::ON},   {0x3020, B::ON},    {0x3030, B::ON},   {0x303D, B::ON},   {0x30A0, B::ON},
        {0x30FB, B::ON},   {0xFB1D, B::R},    {0xFB4F, B::R},     {0xFB50, B::AL},   {0xFDFF, B::AL},   {0xFD3E, B::ON},
        {0xFD3F, B::ON},   {0xFE00, B::NSM},  {0xFE0F, B::NSM},   {0xFE50, B::CS},   {0xFE52, B::CS},   {0xFE55, B::CS},
        {0xFE70, B::AL},   {0xFEFE, B::AL},   {0xFEFF, B::BN},    {0xFF10, B::EN},   {0xFF19, B::EN},   {0xFF0B, B::ES},
        {0xFF0D, B::ES},   {0xFF03, B::ET},   {0xFF04, B::ET},    {0xFF05, B::ET},   {0xFFE0, B::ET},   {0xFFE5, B::ET},
        {0xFF0C, B::CS},   {0xFF0E, B::CS},   {0xFF0F, B::CS},    {0xFF1A, B::CS},   {0xFF01, B::ON},   {0xFF1B, B::ON},
        {0xFF3B, B::ON},   {0xFF5B, B::ON},   {0xFF65, B::ON},    {0x10800, B::R},   {0x10FFF, B::R},   {0x1E800, B::R},
        {0x1EE00, B::AL},  {0x1EEFF, B::AL},  {0x1EF00, B::R},    {0x20000, B::L},   {0xFF21, B::L},
    };
    for (const auto& c : cases) CHECK(uni::bidiClass(c.first) == c.second);
}

TEST(unicode_gaps_numbers_terminators_and_separators) {
    // W5: a currency sign before a number and a percent sign after it join the number (level 2 in
    // a right-to-left line, like the digits).
    CHECK(uni::resolveLevels(U"א $100", 1) == std::vector<int>({1, 1, 2, 2, 2, 2}));
    CHECK(uni::resolveLevels(U"א 100%", 1) == std::vector<int>({1, 1, 2, 2, 2, 2}));
    CHECK(uni::resolveLevels(U"א %%", 1) == std::vector<int>({1, 1, 1, 1}));   // no number: neutral
    // L1: a segment separator inside the line, and the spaces before it, go back to the paragraph
    // level; so does a paragraph separator.
    CHECK(uni::resolveLevels(U"ab \tcd", 1) == std::vector<int>({2, 2, 1, 1, 2, 2}));
    CHECK(uni::resolveLevels(U"a\nb", 1) == std::vector<int>({2, 1, 2}));
}

TEST(unicode_gaps_mirrored_pairs) {
    const std::pair<char32_t, char32_t> pairs[] = {
        {'(', ')'},         {'[', ']'},         {'{', '}'},         {'<', '>'},         {0xAB, 0xBB},
        {0x2039, 0x203A},   {0x2264, 0x2265},   {0x3008, 0x3009},   {0x300A, 0x300B},   {0x300C, 0x300D},
        {0x300E, 0x300F},   {0x3010, 0x3011},   {0xFF08, 0xFF09},   {0xFF3B, 0xFF3D},
    };
    for (const auto& p : pairs) {
        CHECK(uni::mirror(p.first) == p.second);
        CHECK(uni::mirror(p.second) == p.first);
    }
    for (char32_t c : {char32_t('a'), char32_t('/'), char32_t(0x0627), char32_t(0x2266)}) CHECK(uni::mirror(c) == c);
}

TEST(unicode_gaps_lam_alef_variants) {
    CHECK(uni::lamAlefLigature(0x0622, false) == 0xFEF5);   // alef with madda
    CHECK(uni::lamAlefLigature(0x0622, true) == 0xFEF6);
    CHECK(uni::lamAlefLigature(0x0623, false) == 0xFEF7);   // hamza above
    CHECK(uni::lamAlefLigature(0x0623, true) == 0xFEF8);
    CHECK(uni::lamAlefLigature(0x0625, false) == 0xFEF9);   // hamza below
    CHECK(uni::lamAlefLigature(0x0625, true) == 0xFEFA);
    CHECK(uni::lamAlefLigature(0x0628, false) == 0);
    // In a word: lam + alef with hamza above, alone, then after a joining letter.
    uni::Shaped s = uni::shapeArabic(U"لأ");
    CHECK(s.text == std::u32string({0xFEF7}));
    s = uni::shapeArabic(U"سلإ");
    CHECK(s.text == std::u32string({0xFEB3, 0xFEFA}));
    CHECK(s.source == std::vector<int>({0, 1}));
}

TEST(unicode_gaps_upper_case_greek_cyrillic_latin) {
    const std::pair<char32_t, char32_t> cases[] = {
        {0x017F, 'S'},       // long s
        {0x0138, 0x0138},    // kra: no upper case
        {0x0149, 0x0149},    // n preceded by apostrophe
        {0x0130, 0x0130},    // I with dot above is upper case already
        {0x0178, 0x0178},
        {0x03B1, 0x0391},    // alpha
        {0x03C9, 0x03A9},    // omega
        {0x03C2, 0x03A3},    // final sigma
        {0x03AC, 0x0386},    // alpha with tonos
        {0x03AD, 0x0388},    // epsilon with tonos
        {0x03AE, 0x0389},
        {0x03AF, 0x038A},
        {0x03CC, 0x038C},    // omicron with tonos
        {0x03CD, 0x038E},
        {0x03CE, 0x038F},
        {0x04C2, 0x04C1},    // zhe with breve
        {0x04CE, 0x04CD},
        {0x04C1, 0x04C1},    // upper case already
        {0x04CF, 0x04C0},    // palochka
    };
    for (const auto& c : cases) CHECK(uni::toUpper(c.first) == c.second);
    CHECK(uni::isLower(0x03C2) && uni::isLower(0x04CF) && !uni::isLower(0x0391) && !uni::isLower(0x0138));
    CHECK_EQ(uni::toUpper("\xCE\xBA\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1"),   // καλημέρα
             std::string("\xCE\x9A\xCE\x91\xCE\x9B\xCE\x97\xCE\x9C\xCE\x88\xCE\xA1\xCE\x91"));
}
