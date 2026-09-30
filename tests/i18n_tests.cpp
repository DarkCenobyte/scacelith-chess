// Languages: .lang files, plurals, locale matching, and the Unicode logic of the text shaper
// (Arabic joining, bidi reordering, line breaks, case).
#include "test.h"
#include "core/embedded.h"
#include "i18n/i18n.h"
#include "i18n/unicode.h"
#include <algorithm>
#include <map>
#include <set>

namespace {
std::u32string U(const char* utf8) { return uni::decode(utf8); }

std::vector<int> order(const char* utf8, int base) {
    std::u32string t = U(utf8);
    return uni::visualOrder(uni::resolveLevels(t, base));
}

std::set<std::string> placeholders(const std::string& v) {
    std::set<std::string> out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '{') continue;
        size_t j = v.find('}', i);
        if (j == std::string::npos) break;
        out.insert(v.substr(i, j - i + 1));
        i = j;
    }
    return out;
}

size_t countForms(const std::string& v) { return size_t(std::count(v.begin(), v.end(), '|')) + 1; }
}  // namespace

// ---- Translation files ----------------------------------------------------------------------------------
TEST(i18n_lang_files_match_english) {
    std::vector<std::pair<std::string, std::string>> en;
    std::string err;
    CHECK(i18n::parse(embedded::text("assets/i18n/en.lang"), en, &err));
    if (!err.empty()) std::fprintf(stderr, "  en.lang: %s\n", err.c_str());
    CHECK(en.size() > 100);
    std::map<std::string, std::string> ref;
    for (auto& kv : en) {
        if (ref.count(kv.first)) std::fprintf(stderr, "  en.lang: duplicate key %s\n", kv.first.c_str());
        CHECK(!ref.count(kv.first));
        ref[kv.first] = kv.second;
    }
    CHECK_EQ(int(i18n::languages().size()), 10);
    for (const i18n::Language& lang : i18n::languages()) {
        std::string path = std::string("assets/i18n/") + lang.code + ".lang";
        CHECK(embedded::find(path.c_str()) != nullptr);
        if (!embedded::find(path.c_str())) {
            std::fprintf(stderr, "  missing %s\n", path.c_str());
            continue;
        }
        std::vector<std::pair<std::string, std::string>> entries;
        std::string e;
        bool ok = i18n::parse(embedded::text(path.c_str()), entries, &e);
        if (!ok) std::fprintf(stderr, "  %s: %s\n", path.c_str(), e.c_str());
        CHECK(ok);
        std::map<std::string, std::string> got;
        for (auto& kv : entries) {
            if (got.count(kv.first)) std::fprintf(stderr, "  %s: duplicate key %s\n", lang.code, kv.first.c_str());
            CHECK(!got.count(kv.first));
            got[kv.first] = kv.second;
            if (!ref.count(kv.first)) std::fprintf(stderr, "  %s: extra key %s\n", lang.code, kv.first.c_str());
            CHECK(ref.count(kv.first) == 1);
        }
        for (auto& kv : ref) {
            auto it = got.find(kv.first);
            if (it == got.end()) {
                std::fprintf(stderr, "  %s: missing key %s\n", lang.code, kv.first.c_str());
                CHECK(false);
                continue;
            }
            if (placeholders(it->second) != placeholders(kv.second)) {
                std::fprintf(stderr, "  %s: placeholders differ for %s\n", lang.code, kv.first.c_str());
                CHECK(false);
            }
            CHECK(!it->second.empty());
            // Plural keys (English value with '|') carry the language's number of forms.
            bool plural = kv.second.find('|') != std::string::npos;
            size_t want = plural ? size_t(i18n::pluralForms(lang.code)) : 1;
            if (countForms(it->second) != want) {
                std::fprintf(stderr, "  %s: %s has %d form(s), %d expected\n", lang.code, kv.first.c_str(),
                             int(countForms(it->second)), int(want));
                CHECK(false);
            }
        }
    }
}

// Every language names the five pieces of its scoresheet notation with distinct letters that cannot be
// read as a file (a-h), a capture (x) or castling (O), and writes the date with day, month and year once each.
TEST(i18n_scoresheet_pieces_and_date) {
    for (const i18n::Language& lang : i18n::languages()) {
        std::string path = std::string("assets/i18n/") + lang.code + ".lang";
        if (!embedded::find(path.c_str())) continue;  // reported by i18n_lang_files_match_english
        std::vector<std::pair<std::string, std::string>> entries;
        CHECK(i18n::parse(embedded::text(path.c_str()), entries, nullptr));
        std::map<std::string, std::string> got(entries.begin(), entries.end());
        std::vector<std::string> letters;
        std::string cur;
        for (char c : got["scoresheet.pieces"] + " ") {
            if (c == ' ') {
                if (!cur.empty()) letters.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (letters.size() != 5) std::fprintf(stderr, "  %s: scoresheet.pieces needs 5 letters\n", lang.code);
        CHECK_EQ(int(letters.size()), 5);
        CHECK_EQ(int(std::set<std::string>(letters.begin(), letters.end()).size()), int(letters.size()));
        for (const std::string& l : letters) {
            bool clash = l.size() == 1 && std::string("abcdefghxO").find(l[0]) != std::string::npos;
            if (clash) std::fprintf(stderr, "  %s: piece letter %s reads as a square or move\n", lang.code, l.c_str());
            CHECK(!clash);
        }
        const std::string& date = got["scoresheet.date_format"];
        for (const char* p : {"{0}", "{1}", "{2}"}) {
            size_t first = date.find(p);
            CHECK(first != std::string::npos);
            CHECK(date.find(p, first + 1) == std::string::npos);
        }
    }
}

// Coach mode: every level has its texts, the voice credit names Supertonic 3 (never translated)
// and its licence, the openings credit its source and licence, and the keys that skip the coach
// and accept its takeback differ.
TEST(i18n_coach_texts) {
    for (const i18n::Language& lang : i18n::languages()) {
        std::string path = std::string("assets/i18n/") + lang.code + ".lang";
        if (!embedded::find(path.c_str())) continue;  // reported by i18n_lang_files_match_english
        std::vector<std::pair<std::string, std::string>> entries;
        CHECK(i18n::parse(embedded::text(path.c_str()), entries, nullptr));
        std::map<std::string, std::string> got(entries.begin(), entries.end());
        auto has = [&](const std::string& key, const char* part) {
            auto it = got.find(key);
            bool ok = it != got.end() && it->second.find(part) != std::string::npos;
            if (!ok) std::fprintf(stderr, "  %s: %s should contain \"%s\"\n", lang.code, key.c_str(), part);
            return ok;
        };
        for (int level = 0; level <= 6; ++level)
            for (const char* field : {"name", "desc", "detail"}) {
                std::string key = "coach.level." + std::to_string(level) + "." + field;
                CHECK(got.count(key) == 1 && !got[key].empty());
            }
        CHECK(has("menu.coach", ""));
        CHECK(has("menu.tts_credit", "{0}"));  // "Supertonic 3" is passed in, untranslated
        CHECK(has("credits.voice", "Supertonic 3"));
        CHECK(has("credits.voice.licence", "OpenRAIL-M"));
        CHECK(has("credits.voice.licence", "Supertone"));
        CHECK(has("credits.openings", "lichess"));
        CHECK(has("credits.openings", "CC0"));
        CHECK(got["controls.coach_skip.keys"] != got["controls.coach_takeback.keys"]);
        // No "natural language" option: the coach speaks from its own sentences only.
        for (auto& kv : entries) CHECK(kv.first.find("natural") == std::string::npos);
    }
    CHECK(i18n::setLanguage("de"));
    std::string credit = i18n::trf("menu.tts_credit", {"Supertonic 3"});
    CHECK(credit.find("Supertonic 3") != std::string::npos);
    CHECK(credit.find("{0}") == std::string::npos);
    i18n::setLanguage("en");
}

TEST(i18n_tr_fallback_and_format) {
    CHECK(i18n::setLanguage("fr"));
    CHECK_EQ(i18n::language(), std::string("fr"));
    CHECK_EQ(std::string(i18n::tr("no.such.key")), std::string("no.such.key"));
    CHECK_EQ(i18n::format("{1} {0}", {"a", "b"}), std::string("b a"));
    CHECK(i18n::setLanguage("ar"));
    CHECK(i18n::rtl());
    CHECK(!i18n::setLanguage("xx"));
    CHECK_EQ(i18n::language(), std::string("en"));
    CHECK(!i18n::rtl());
    CHECK_EQ(i18n::trn("gameover.win_moves", 1), std::string("Well played \xE2\x80\x94 you win in 1 move."));
    CHECK_EQ(i18n::trn("gameover.win_moves", 34), std::string("Well played \xE2\x80\x94 you win in 34 moves."));
    i18n::setLanguage("en");
}

TEST(i18n_plural_rules) {
    CHECK_EQ(i18n::pluralIndex("en", 1), 0);
    CHECK_EQ(i18n::pluralIndex("en", 0), 1);
    CHECK_EQ(i18n::pluralIndex("fr", 0), 0);
    CHECK_EQ(i18n::pluralIndex("fr", 1), 0);
    CHECK_EQ(i18n::pluralIndex("fr", 2), 1);
    const int ru[][2] = {{1, 0}, {2, 1}, {4, 1}, {5, 2}, {11, 2}, {12, 2}, {21, 0}, {22, 1}, {25, 2}, {111, 2}, {0, 2}};
    for (auto& c : ru) {
        CHECK_EQ(i18n::pluralIndex("ru", c[0]), c[1]);
        CHECK_EQ(i18n::pluralIndex("uk", c[0]), c[1]);
    }
    const int ar[][2] = {{0, 0}, {1, 1}, {2, 2}, {3, 3}, {10, 3}, {11, 4}, {99, 4}, {100, 5}, {102, 5}, {103, 3}};
    for (auto& c : ar) CHECK_EQ(i18n::pluralIndex("ar", c[0]), c[1]);
    CHECK_EQ(i18n::pluralIndex("ja", 5), 0);
    CHECK_EQ(i18n::pluralForms("ar"), 6);
    CHECK_EQ(i18n::pluralForms("zh-Hant"), 1);
}

// The toast of the points given back after a cheater's ban: a refund of 1 point is common (a
// draw, a loss at K 10), and every language words the count by its own plural rule.
TEST(i18n_rating_restored_plural) {
    auto has = [](const std::string& text, const char* part) {
        bool ok = text.find(part) != std::string::npos;
        if (!ok) std::fprintf(stderr, "  %s: \"%s\" not in \"%s\"\n", i18n::language().c_str(), part, text.c_str());
        return ok;
    };
    const char* key = "online.notice.rating_restored";
    i18n::setLanguage("en");
    CHECK_EQ(i18n::trn(key, 1), std::string("Your rating was restored by 1 point: an opponent was banned for cheating."));
    CHECK_EQ(i18n::trn(key, 12), std::string("Your rating was restored by 12 points: an opponent was banned for cheating."));
    i18n::setLanguage("de");
    CHECK(has(i18n::trn(key, 1), "um 1 Punkt ") && has(i18n::trn(key, 3), "um 3 Punkte "));
    i18n::setLanguage("fr");
    CHECK(has(i18n::trn(key, 1), "de 1 point :") && has(i18n::trn(key, 2), "de 2 points :"));
    i18n::setLanguage("es");
    CHECK(has(i18n::trn(key, 1), "en 1 punto:") && has(i18n::trn(key, 7), "en 7 puntos:"));
    i18n::setLanguage("ru");
    CHECK(has(i18n::trn(key, 1), " 1 пункт:") && has(i18n::trn(key, 21), " 21 пункт:") && has(i18n::trn(key, 3), " 3 пункта:"));
    CHECK(has(i18n::trn(key, 5), " 5 пунктов:") && has(i18n::trn(key, 11), " 11 пунктов:"));
    i18n::setLanguage("uk");
    CHECK(has(i18n::trn(key, 1), " 1 пункт:") && has(i18n::trn(key, 21), " 21 пункт:") && has(i18n::trn(key, 3), " 3 пункти:"));
    CHECK(has(i18n::trn(key, 5), " 5 пунктів:") && has(i18n::trn(key, 11), " 11 пунктів:"));
    i18n::setLanguage("ar");
    CHECK(has(i18n::trn(key, 1), "نقطة واحدة") && has(i18n::trn(key, 2), "نقطتين"));
    CHECK(has(i18n::trn(key, 4), "4 نقاط") && has(i18n::trn(key, 15), "15 نقطة") && has(i18n::trn(key, 100), "100 نقطة"));
    i18n::setLanguage("ja");
    CHECK(has(i18n::trn(key, 1), "1ポイント"));
    i18n::setLanguage("en");
}

TEST(i18n_match_locale) {
    CHECK_EQ(i18n::matchLocale("fr_FR.UTF-8"), std::string("fr"));
    CHECK_EQ(i18n::matchLocale("de-AT"), std::string("de"));
    CHECK_EQ(i18n::matchLocale("uk_UA"), std::string("uk"));
    CHECK_EQ(i18n::matchLocale("ar-EG"), std::string("ar"));
    CHECK_EQ(i18n::matchLocale("ja_JP.eucJP"), std::string("ja"));
    CHECK_EQ(i18n::matchLocale("zh_TW.UTF-8"), std::string("zh-Hant"));
    CHECK_EQ(i18n::matchLocale("zh-HK"), std::string("zh-Hant"));
    CHECK_EQ(i18n::matchLocale("zh-Hant-MO"), std::string("zh-Hant"));
    CHECK_EQ(i18n::matchLocale("zh_CN"), std::string("zh-Hans"));
    CHECK_EQ(i18n::matchLocale("zh-SG"), std::string("zh-Hans"));
    CHECK_EQ(i18n::matchLocale("zh"), std::string("zh-Hans"));
    CHECK_EQ(i18n::matchLocale("pt_BR"), std::string("en"));
    CHECK_EQ(i18n::matchLocale("C"), std::string("en"));
    CHECK_EQ(i18n::matchLocale(""), std::string("en"));
}

// ---- Unicode ----------------------------------------------------------------------------------------------
TEST(unicode_utf8_roundtrip) {
    const char* s = "Fran\xC3\xA7" "ais \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80";
    std::u32string u = U(s);
    CHECK_EQ(int(u.size()), 18);
    CHECK_EQ(uni::encode(u), std::string(s));
    CHECK_EQ(int(uni::length(s)), 18);
    CHECK(U("\xC3")[0] == 0xFFFD);          // truncated sequence
    CHECK(U("\xC0\xAF")[0] == 0xFFFD);      // overlong
}

TEST(unicode_arabic_joining_forms) {
    using F = uni::Form;
    // beh yeh teh: initial, medial, final
    auto f = uni::joiningForms(U("\xD8\xA8\xD9\x8A\xD8\xAA"));
    CHECK(f[0] == F::Initial && f[1] == F::Medial && f[2] == F::Final);
    // dal alef reh: right-joining letters never join the next one
    f = uni::joiningForms(U("\xD8\xAF\xD8\xA7\xD8\xB1"));
    CHECK(f[0] == F::Isolated && f[1] == F::Isolated && f[2] == F::Isolated);
    // Muhammad: meem hah meem dal
    f = uni::joiningForms(U("\xD9\x85\xD8\xAD\xD9\x85\xD8\xAF"));
    CHECK(f[0] == F::Initial && f[1] == F::Medial && f[2] == F::Medial && f[3] == F::Final);
    // A fatha between beh and teh is transparent.
    f = uni::joiningForms(U("\xD8\xA8\xD9\x8E\xD8\xAA"));
    CHECK(f[0] == F::Initial && f[2] == F::Final);
    // Tatweel causes joining: tatweel + beh -> final beh; beh + ZWJ -> initial.
    f = uni::joiningForms(U("\xD9\x80\xD8\xA8"));
    CHECK(f[1] == F::Final);
    f = uni::joiningForms(U("\xD8\xA8\xE2\x80\x8D"));
    CHECK(f[0] == F::Initial);
    // A space or a Latin letter breaks the joining.
    f = uni::joiningForms(U("\xD8\xA8 \xD8\xA8"));
    CHECK(f[0] == F::Isolated && f[2] == F::Isolated);
    CHECK(uni::presentationForm(0x0628, F::Initial) == 0xFE91);
    CHECK(uni::presentationForm(0x0627, F::Initial) == 0);  // alef has no initial form
    CHECK(uni::joiningType(0x0627) == uni::Joining::R);
    CHECK(uni::joiningType(0x0628) == uni::Joining::D);
    CHECK(uni::joiningType(0x0621) == uni::Joining::U);
    CHECK(uni::joiningType(0x064E) == uni::Joining::T);
}

TEST(unicode_arabic_shaping_lam_alef) {
    // "la": one isolated ligature from the lam.
    uni::Shaped s = uni::shapeArabic(U("\xD9\x84\xD8\xA7"));
    CHECK_EQ(int(s.text.size()), 1);
    CHECK(s.text[0] == 0xFEFB);
    CHECK_EQ(s.source[0], 0);
    // salam: seen (initial), lam-alef (final ligature), meem (isolated)
    s = uni::shapeArabic(U("\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85"));
    CHECK_EQ(int(s.text.size()), 3);
    CHECK(s.text[0] == 0xFEB3);
    CHECK(s.text[1] == 0xFEFC);
    CHECK(s.text[2] == 0xFEE1);
    CHECK_EQ(s.source[1], 1);
    CHECK_EQ(s.source[2], 3);
    // Allah: alef lam lam heh -> alef isolated, lam initial, lam medial, heh final (no ligature)
    s = uni::shapeArabic(U("\xD8\xA7\xD9\x84\xD9\x84\xD9\x87"));
    CHECK(s.text == std::u32string({0xFE8D, 0xFEDF, 0xFEE0, 0xFEEA}));
    // Latin text is untouched.
    s = uni::shapeArabic(U("abc"));
    CHECK(s.text == U("abc"));
}

TEST(unicode_bidi_levels_and_order) {
    CHECK_EQ(uni::paragraphLevel(U("abc")), 0);
    CHECK_EQ(uni::paragraphLevel(U("123 \xD8\xA8")), 1);
    CHECK_EQ(uni::paragraphLevel(U("123"), 1), 1);
    // Pure Latin: identity.
    CHECK(order("abc", 0) == std::vector<int>({0, 1, 2}));
    // Pure Arabic: reversed.
    CHECK(order("\xD8\xA7\xD8\xA8\xD8\xAC", 1) == std::vector<int>({2, 1, 0}));
    // Arabic with a number: the digits stay left-to-right inside the right-to-left line.
    CHECK(order("\xD8\xA7\xD8\xA8 123 \xD8\xAC\xD8\xAF", 1) == std::vector<int>({8, 7, 6, 3, 4, 5, 2, 1, 0}));
    // Arabic word inside English.
    CHECK(order("a \xD8\xA7\xD8\xA8\xD8\xAC b", 0) == std::vector<int>({0, 1, 4, 3, 2, 5, 6}));
    // Latin word inside Arabic: kept left-to-right, the whole line reversed around it.
    CHECK(order("\xD8\xA7 Elo \xD8\xA8", 1) == std::vector<int>({6, 5, 2, 3, 4, 1, 0}));
    // Neutrals between different directions take the paragraph direction.
    std::vector<int> lv = uni::resolveLevels(U("a \xD8\xA8"), 0);
    CHECK(lv == std::vector<int>({0, 0, 1}));
    // Trailing spaces go back to the paragraph level (L1).
    lv = uni::resolveLevels(U("\xD8\xA8  "), 0);
    CHECK(lv == std::vector<int>({1, 0, 0}));
    // Brackets in a right-to-left run are mirrored when drawn.
    lv = uni::resolveLevels(U("(\xD8\xA8)"), 1);
    CHECK(lv == std::vector<int>({1, 1, 1}));
    CHECK(uni::mirror('(') == ')');
    CHECK(uni::mirror(0xAB) == 0xBB);
    // European digits after Arabic letters are Arabic numbers (W2) but still left-to-right.
    lv = uni::resolveLevels(U("\xD8\xA8 12"), 0);
    CHECK(lv == std::vector<int>({1, 1, 2, 2}));
    // "10+5" after Arabic letters: Arabic numbers, the plus sign is not a number separator for
    // them, so the UBA shows "5+10" (as every conforming renderer does)...
    CHECK(order("\xD8\xA8 10+5", 1) == std::vector<int>({5, 4, 2, 3, 1, 0}));
    // ...unless a left-to-right mark isolates it (i18n::ltr()).
    CHECK(order("\xD8\xA8 \xE2\x80\x8E" "10+5", 1) == std::vector<int>({2, 3, 4, 5, 6, 1, 0}));
    // Numbers in an Arabic word order of multi-level runs.
    CHECK(uni::visualOrder({0, 1, 1, 2, 2, 1, 0}) == std::vector<int>({0, 5, 3, 4, 2, 1, 6}));
}

TEST(unicode_line_breaks) {
    CHECK(uni::breakBetween(0x65E5, 0x672C));    // 日本
    CHECK(uni::breakBetween(0x3042, 0x3044));    // あい
    CHECK(!uni::breakBetween(0x3042, 0x3002));   // no break before 。
    CHECK(!uni::breakBetween(0x300C, 0x3042));   // no break after 「
    CHECK(!uni::breakBetween(0x3042, 0x3063));   // no break before small tsu
    CHECK(!uni::breakBetween(0x30AB, 0x30FC));   // no break before the long vowel mark
    CHECK(!uni::breakBetween('a', 'b'));
    CHECK(!uni::breakBetween(0x4E2D, ' '));
    CHECK(uni::breakBetween('a', 0x4E2D));
    CHECK(uni::trackingScale(0x8A2D, 0x5B9A) < 0.5f);   // 設定: tight
    CHECK(uni::trackingScale('A', 0x5B9A) == 1.0f);     // mixed scripts keep the full spacing
    CHECK(uni::trackingScale('A', 'B') == 1.0f);
}

TEST(unicode_upper_case) {
    CHECK_EQ(uni::toUpper("stra\xC3\x9F" "e"), std::string("STRASSE"));
    CHECK_EQ(uni::toUpper("\xC3\xA9lan"), std::string("\xC3\x89LAN"));
    CHECK_EQ(uni::toUpper("\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"),
             std::string("\xD0\x9F\xD0\xA0\xD0\x98\xD0\x92\xD0\x95\xD0\xA2"));  // привет
    CHECK(uni::toUpper(char32_t(0x0491)) == 0x0490);  // ґ
    CHECK(uni::toUpper(char32_t(0x0457)) == 0x0407);  // ї
    CHECK(uni::toUpper(char32_t(0x0142)) == 0x0141);  // ł
    CHECK(uni::toUpper(char32_t(0x0153)) == 0x0152);  // œ
    CHECK(uni::toUpper(char32_t(0x3042)) == 0x3042);  // no case
    CHECK(uni::isLower('a') && !uni::isLower('A') && !uni::isLower(0x0628));
}
