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

// The note after signing out another device (the devices page: the current one has no Sign out
// button) speaks of that device, never in the words of the tag that marks this one ("THIS
// DEVICE"): "This device is signed out" tells the player that they were signed out themselves.
TEST(i18n_devices_signed_out_note_is_about_another_device) {
    // "this device" in the cases a sentence may put it in (the tag is in the nominative).
    const std::map<std::string, std::vector<std::string>> thisDevice = {
        {"en", {"this device"}},
        {"fr", {"cet appareil"}},
        {"de", {"dieses Ger\xC3\xA4t", "diesem Ger\xC3\xA4t"}},
        {"es", {"este dispositivo"}},
        {"ru", {"\xD1\x8D\xD1\x82\xD0\xBE\xD0\xBC \xD1\x83\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD1\x81\xD1\x82\xD0\xB2\xD0\xB5",       // этом устройстве
                "\xD1\x8D\xD1\x82\xD0\xBE\xD0\xB3\xD0\xBE \xD1\x83\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD1\x81\xD1\x82\xD0\xB2\xD0\xB0"}},  // этого устройства
        {"uk", {"\xD1\x86\xD1\x8C\xD0\xBE\xD0\xBC\xD1\x83 \xD0\xBF\xD1\x80\xD0\xB8\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD1\x97",              // цьому пристрої
                "\xD1\x86\xD1\x8C\xD0\xBE\xD0\xB3\xD0\xBE \xD0\xBF\xD1\x80\xD0\xB8\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD1\x8E"}},             // цього пристрою
        {"ar", {"\xD9\x87\xD8\xB0\xD8\xA7 \xD8\xA7\xD9\x84\xD8\xAC\xD9\x87\xD8\xA7\xD8\xB2"}},                                             // هذا الجهاز
        {"ja", {"\xE3\x81\x93\xE3\x81\xAE\xE3\x83\x87\xE3\x83\x90\xE3\x82\xA4\xE3\x82\xB9"}},                                               // このデバイス
        {"zh-Hans", {"\xE6\x9C\xAC\xE8\xAE\xBE\xE5\xA4\x87", "\xE6\xAD\xA4\xE8\xAE\xBE\xE5\xA4\x87"}},                                     // 本设备, 此设备
        {"zh-Hant", {"\xE6\x9C\xAC\xE8\xA3\x9D\xE7\xBD\xAE", "\xE6\xAD\xA4\xE8\xA3\x9D\xE7\xBD\xAE"}},                                     // 本裝置, 此裝置
    };
    CHECK_EQ(int(thisDevice.size()), int(i18n::languages().size()));
    for (const i18n::Language& lang : i18n::languages()) {
        std::vector<std::pair<std::string, std::string>> entries;
        const std::string path = std::string("assets/i18n/") + lang.code + ".lang";
        if (!embedded::find(path.c_str())) continue;  // reported by i18n_lang_files_match_english
        CHECK(i18n::parse(embedded::text(path.c_str()), entries, nullptr));
        std::string note, tag;
        for (const auto& kv : entries) {
            if (kv.first == "online.devices.signed_out") note = uni::toUpper(kv.second);
            if (kv.first == "online.devices.this") tag = uni::toUpper(kv.second);
        }
        CHECK(!note.empty() && !tag.empty());
        std::vector<std::string> words{tag};
        auto it = thisDevice.find(lang.code);
        CHECK(it != thisDevice.end());
        if (it != thisDevice.end())
            for (const std::string& w : it->second)
                if (uni::toUpper(w) != tag) words.push_back(uni::toUpper(w));
        for (const std::string& w : words) {
            if (note.find(w) == std::string::npos) continue;
            std::fprintf(stderr, "  %s: online.devices.signed_out says \"%s\"\n", lang.code, w.c_str());
            CHECK(false);
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
        CHECK(has("coach.takebacks_left", "{0}"));   // the count, levels 4-6 (one form: a label)
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

// The three failures before the game starts (src/main.cpp: OpenGL 4.6, with its macOS wording, the
// renderer, the scene) are told in the player's language, followed by the log file's name
// (error.log_file).
TEST(i18n_startup_errors) {
    const std::string log = "C:\\Games\\Scacelith\\scacelith.log";
    for (const i18n::Language& lang : i18n::languages()) {
        CHECK(i18n::setLanguage(lang.code));
        for (const char* key : {"error.opengl", "error.opengl_macos", "error.renderer", "error.scene", "error.log_file"}) {
            CHECK(i18n::has(key));
            if (std::string(lang.code) != "en") CHECK(std::string(i18n::tr(key)) != i18n::english(key));
        }
        CHECK(i18n::trf("error.log_file", {log}).find(log) != std::string::npos);
    }
    i18n::setLanguage("en");
}

// Google sign-in by loopback redirect: every language has the game's texts and the page the browser
// shows, translated; the link page names the account ({0}) and the server ({1}); the texts that send
// the player to "Forgot password?" quote that link's own label.
TEST(i18n_sso_texts) {
    const char* keys[] = {
        "online.sso.wait", "online.sso.finishing", "online.sso.link_title", "online.sso.link_lead",
        "online.sso.link_button", "online.sso.link_mfa", "online.sso.not_mine", "online.sso.not_mine_note",
        "online.sso.page.title", "online.sso.page.done_heading", "online.sso.page.done",
        "online.sso.page.cancelled_heading", "online.sso.page.cancelled", "online.sso.page.foreign_heading",
        "online.sso.page.foreign", "online.err.sso_expired", "online.err.sso_failed", "online.err.sso_listen",
        "online.err.sso_origin", "online.err.browser", "online.err.sso_email_unverified",
        "online.err.sso_account_exists", "online.err.sso_already_linked", "online.err.account_disabled"};
    for (const i18n::Language& lang : i18n::languages()) {
        CHECK(i18n::setLanguage(lang.code));
        for (const char* key : keys) {
            CHECK(i18n::has(key));
            if (std::string(lang.code) != "en" && std::string(i18n::tr(key)) == i18n::english(key)) {
                std::fprintf(stderr, "  %s: %s is not translated\n", lang.code, key);
                CHECK(false);
            }
        }
        std::string lead = i18n::trf("online.sso.link_lead", {"Guillaume_G", "play.example"});
        CHECK(lead.find("Guillaume_G") != std::string::npos);
        CHECK(lead.find("play.example") != std::string::npos);
        const std::string forgot = i18n::tr("online.signin.forgot");
        for (const char* key : {"online.sso.not_mine_note", "online.err.sso_account_exists"}) {
            bool ok = std::string(i18n::tr(key)).find(forgot) != std::string::npos;
            if (!ok) std::fprintf(stderr, "  %s: %s should quote \"%s\"\n", lang.code, key, forgot.c_str());
            CHECK(ok);
        }
    }
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
    // The languages added in 1.1: {count, form} per rule.
    const int hr[][2] = {{1, 0}, {21, 0}, {11, 2}, {2, 1}, {24, 1}, {12, 2}, {5, 2}, {0, 2}};
    for (auto& c : hr) CHECK_EQ(i18n::pluralIndex("hr", c[0]), c[1]);
    const int pl[][2] = {{1, 0}, {2, 1}, {4, 1}, {22, 1}, {5, 2}, {12, 2}, {21, 2}, {112, 2}, {0, 2}};
    for (auto& c : pl) CHECK_EQ(i18n::pluralIndex("pl", c[0]), c[1]);
    const int cs[][2] = {{1, 0}, {2, 1}, {4, 1}, {5, 2}, {0, 2}, {21, 2}, {22, 2}};
    for (auto& c : cs) {
        CHECK_EQ(i18n::pluralIndex("cs", c[0]), c[1]);
        CHECK_EQ(i18n::pluralIndex("sk", c[0]), c[1]);
    }
    const int lt[][2] = {{1, 0}, {21, 0}, {101, 0}, {2, 1}, {9, 1}, {22, 1}, {0, 2}, {10, 2}, {11, 2}, {12, 2}, {19, 2}, {20, 2}};
    for (auto& c : lt) CHECK_EQ(i18n::pluralIndex("lt", c[0]), c[1]);
    const int lv[][2] = {{0, 0}, {10, 0}, {11, 0}, {111, 0}, {1, 1}, {21, 1}, {2, 2}, {9, 2}, {22, 2}};
    for (auto& c : lv) CHECK_EQ(i18n::pluralIndex("lv", c[0]), c[1]);
    const int ro[][2] = {{1, 0}, {0, 1}, {2, 1}, {19, 1}, {101, 1}, {119, 1}, {20, 2}, {100, 2}, {120, 2}};
    for (auto& c : ro) CHECK_EQ(i18n::pluralIndex("ro", c[0]), c[1]);
    const int sl[][2] = {{1, 0}, {101, 0}, {2, 1}, {102, 1}, {3, 2}, {4, 2}, {5, 3}, {0, 3}, {11, 3}};
    for (auto& c : sl) CHECK_EQ(i18n::pluralIndex("sl", c[0]), c[1]);
    for (const char* code : {"pt-BR", "hi"}) {
        CHECK_EQ(i18n::pluralIndex(code, 0), 0);
        CHECK_EQ(i18n::pluralIndex(code, 1), 0);
        CHECK_EQ(i18n::pluralIndex(code, 2), 1);
    }
    CHECK_EQ(i18n::pluralIndex("pt", 0), 1);
    CHECK_EQ(i18n::pluralIndex("pt", 1), 0);
    for (const char* code : {"ko", "id", "vi"}) {
        CHECK_EQ(i18n::pluralIndex(code, 1), 0);
        CHECK_EQ(i18n::pluralIndex(code, 7), 0);
        CHECK_EQ(i18n::pluralForms(code), 1);
    }
    CHECK_EQ(i18n::pluralForms("sl"), 4);
    CHECK_EQ(i18n::pluralForms("lv"), 3);
    CHECK_EQ(i18n::pluralForms("ro"), 3);
    CHECK_EQ(i18n::pluralForms("pt-BR"), 2);
    // Every language of the list has a rule (the default two forms are right for the others).
    for (const i18n::Language& l : i18n::languages()) CHECK(i18n::pluralForms(l.code) >= 1);
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
    CHECK_EQ(i18n::matchLocale("pt_BR"), std::string("pt-BR"));
    CHECK_EQ(i18n::matchLocale("pt-BR"), std::string("pt-BR"));
    CHECK_EQ(i18n::matchLocale("pt_PT.UTF-8"), std::string("pt"));
    CHECK_EQ(i18n::matchLocale("pt-AO"), std::string("pt"));
    CHECK_EQ(i18n::matchLocale("pt"), std::string("pt"));
    CHECK_EQ(i18n::matchLocale("in_ID"), std::string("id"));   // Indonesian's old code
    CHECK_EQ(i18n::matchLocale("id-ID"), std::string("id"));
    CHECK_EQ(i18n::matchLocale("ko_KR.UTF-8"), std::string("ko"));
    CHECK_EQ(i18n::matchLocale("hi-IN"), std::string("hi"));
    CHECK_EQ(i18n::matchLocale("el_GR"), std::string("el"));
    CHECK_EQ(i18n::matchLocale("sv_FI"), std::string("sv"));
    CHECK_EQ(i18n::matchLocale("nb_NO"), std::string("en"));   // not a game language
    CHECK_EQ(i18n::matchLocale("C"), std::string("en"));
    CHECK_EQ(i18n::matchLocale(""), std::string("en"));
}

// ---- Unicode ----------------------------------------------------------------------------------------------
TEST(unicode_utf8_roundtrip) {
    const char* s = "Fran\xC3\xA7" "ais \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80";
    std::u32string u = U(s);
    CHECK_EQ(int(u.size()), 18);
    CHECK_EQ(uni::encode(u), std::string(s));
    CHECK(U("\xC3")[0] == 0xFFFD);          // truncated sequence
    CHECK(U("\xC0\xAF")[0] == 0xFFFD);      // overlong
}

TEST(unicode_decode_at_steps_like_decode) {
    // Text wrapping walks a paragraph with decodeAt and the shaper decodes it with decode(): both
    // must read malformed bytes alike (C0 A0 is no space, a surrogate no character).
    std::string s = "ab\xC0\xA0" "cd";
    size_t i = 2;
    CHECK(uni::decodeAt(s, i) == 0xFFFD);
    CHECK_EQ(int(i), 3);
    const char* samples[] = {"Fran\xC3\xA7" "ais \xF0\x9F\x98\x80", "ab\xC0\xA0" "cd", "\xED\xA0\x80", "\xF4\x90\x80\x80",
                             "\xE0\x80\xAF", "\xF0\x80\x80\xAF", "\xC3", "x\xE2\x82", "\xC1\xBF\xF8\x88\x80\x80\x80"};
    uint32_t seed = 12345;
    std::vector<std::string> all(std::begin(samples), std::end(samples));
    for (int k = 0; k < 2000; ++k) {  // random bytes biased towards lead and continuation bytes
        std::string r;
        for (int n = 0; n < 12; ++n) {
            seed = seed * 1664525u + 1013904223u;
            unsigned b = seed >> 24;
            r += char((b & 1) ? (0x80 | (b >> 2)) : b);
        }
        all.push_back(r);
    }
    for (const std::string& t : all) {
        std::u32string stepped;
        for (size_t j = 0; j < t.size();) stepped += uni::decodeAt(t, j);
        CHECK(stepped == uni::decode(t));
    }
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
    // Romanian comma-below letters, Vietnamese horn and dot-below letters.
    CHECK(uni::toUpper(char32_t(0x0219)) == 0x0218);  // ș
    CHECK(uni::toUpper(char32_t(0x021B)) == 0x021A);  // ț
    CHECK(uni::toUpper(char32_t(0x01A1)) == 0x01A0);  // ơ
    CHECK(uni::toUpper(char32_t(0x01B0)) == 0x01AF);  // ư
    CHECK(uni::toUpper(char32_t(0x1EA1)) == 0x1EA0);  // ạ
    CHECK(uni::toUpper(char32_t(0x1EC5)) == 0x1EC4);  // ễ
    CHECK(uni::toUpper(char32_t(0x1EF9)) == 0x1EF8);  // ỹ
    CHECK(uni::toUpper(char32_t(0x1EA0)) == 0x1EA0);
    CHECK(uni::isLower(0x1EDD) && !uni::isLower(0x1EDC));  // ờ / Ờ
    CHECK_EQ(uni::toUpper("c\xE1\xBB\x9D vua"), std::string("C\xE1\xBB\x9C VUA"));   // cờ vua
    // Greek: a sentence's first letter keeps its tonos, all caps drop it (the diaeresis stays).
    CHECK(uni::toUpper(char32_t(0x03AD)) == 0x0388);   // έ -> Έ
    CHECK(uni::toCapital(0x03AD) == 0x0395);           // έ -> Ε
    CHECK(uni::toCapital(0x0388) == 0x0395);           // Έ -> Ε
    CHECK(uni::toCapital(0x03CA) == 0x03AA);           // ϊ -> Ϊ
    CHECK(uni::toCapital(0x0390) == 0x03AA);           // ΐ -> Ϊ
    CHECK(uni::toUpper(char32_t(0x03C2)) == 0x03A3);   // final sigma
    CHECK_EQ(uni::toUpper("\xCE\xBA\xCE\xAF\xCE\xBD\xCE\xB7\xCF\x83\xCE\xB7"),
             std::string("\xCE\x9A\xCE\x99\xCE\x9D\xCE\x97\xCE\xA3\xCE\x97"));   // κίνηση -> ΚΙΝΗΣΗ
    // Turkish dotted and dotless i.
    CHECK(uni::toUpper(char32_t('i'), true) == 0x0130);
    CHECK(uni::toUpper(char32_t('i')) == 'I');
    CHECK(uni::toUpper(char32_t(0x0131), true) == 'I');
    CHECK_EQ(uni::toUpper("bitir", "tr"), std::string("B\xC4\xB0T\xC4\xB0R"));   // BİTİR
    CHECK_EQ(uni::toUpper("bitir", "en"), std::string("BITIR"));
    CHECK_EQ(uni::toUpper("\xC4\xB1sl\xC4\xB1k", "tr"), std::string("ISLIK"));   // ıslık
}

// Hangul and Devanagari are their own scripts (their own fonts); Devanagari's non-spacing signs
// are marks, its spacing vowel signs are not.
TEST(unicode_hangul_and_devanagari_scripts) {
    CHECK(uni::isHangul(0xD55C) && uni::isHangul(0xAC00) && uni::isHangul(0xD7A3) && uni::isHangul(0x3131));
    CHECK(!uni::isHangul(0x4E00) && !uni::isHangul('a'));
    CHECK(uni::isDevanagari(0x0939) && uni::isDevanagari(0x094D) && uni::isDevanagari(uni::kDevanagariClusterFirst));
    CHECK(!uni::isDevanagari(0x0627) && !uni::isDevanagari(0xF000));
    CHECK(uni::isMark(0x0941) && uni::isMark(0x094D) && uni::isMark(0x0902) && uni::isMark(0x093C));
    CHECK(!uni::isMark(0x093F) && !uni::isMark(0x0940) && !uni::isMark(0x093E) && !uni::isMark(0x0903));
    CHECK(!uni::isMark(0x0939) && !uni::isMark(0x0964));
}

// Devanagari orthographic syllables: consonant clusters joined by virama with their signs and
// bindus, independent vowels with theirs; anything else belongs to no syllable.
TEST(unicode_devanagari_syllables) {
    auto syllables = [](const char* utf8) {
        std::u32string t = U(utf8);
        std::vector<std::string> out;
        for (const auto& s : uni::devanagariSyllables(t)) out.push_back(uni::encode(t.substr(size_t(s.first), size_t(s.second - s.first))));
        return out;
    };
    // हिन्दी = हि + न्दी
    CHECK(syllables("\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80") ==
          std::vector<std::string>({"\xE0\xA4\xB9\xE0\xA4\xBF", "\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80"}));
    // ऊँट = ऊँ + ट; a final virama stays with its consonant (क्)
    CHECK(syllables("\xE0\xA4\x8A\xE0\xA4\x81\xE0\xA4\x9F") ==
          std::vector<std::string>({"\xE0\xA4\x8A\xE0\xA4\x81", "\xE0\xA4\x9F"}));
    CHECK(syllables("\xE0\xA4\x95\xE0\xA5\x8D") == std::vector<std::string>({"\xE0\xA4\x95\xE0\xA5\x8D"}));
    // घोड़ा: the nukta stays with its consonant (ड़ा)
    CHECK(syllables("\xE0\xA4\x98\xE0\xA5\x8B\xE0\xA4\xA1\xE0\xA4\xBC\xE0\xA4\xBE") ==
          std::vector<std::string>({"\xE0\xA4\x98\xE0\xA5\x8B", "\xE0\xA4\xA1\xE0\xA4\xBC\xE0\xA4\xBE"}));
    // Latin, digits, spaces and the danda are outside syllables.
    const std::u32string mixed = U("e4 \xE0\xA4\x95\xE0\xA4\xBE\xE0\xA5\xA4");   // e4 का।
    const auto spans = uni::devanagariSyllables(mixed);
    REQUIRE(spans.size() == 1u);
    CHECK_EQ(spans[0].first, 3);
    CHECK_EQ(spans[0].second, 5);
}

// The fonts' precomposed syllables (assets/fonts/devanagari-clusters.txt, loaded by i18n) replace
// the syllables of a text; every other character and the sources stay as they were.
TEST(unicode_devanagari_precomposed_syllables) {
    i18n::tr("menu.play");   // loads English and the syllable table
    const std::u32string hi = U("\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80");   // हिन्दी
    const char32_t a = uni::devanagariCluster(hi.substr(0, 2));
    const char32_t b = uni::devanagariCluster(hi.substr(2));
    REQUIRE(a >= uni::kDevanagariClusterFirst && a <= uni::kDevanagariClusterLast);
    REQUIRE(b >= uni::kDevanagariClusterFirst && b <= uni::kDevanagariClusterLast);
    CHECK(a != b);
    const uni::Shaped sh = uni::shape(U("A \xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80!"));
    REQUIRE(sh.text.size() == 5u);
    CHECK(sh.text[0] == 'A' && sh.text[1] == ' ' && sh.text[2] == a && sh.text[3] == b && sh.text[4] == '!');
    CHECK(sh.source == std::vector<int>({0, 1, 2, 4, 8}));
    CHECK(uni::devanagariCluster(U("ab")) == 0);
    // The table's own format: hexadecimal codepoint, a space, the syllable; '#' comments.
    CHECK_EQ(uni::setDevanagariClusters("# test\nE001 \xE0\xA4\x95\xE0\xA4\xBF\nzz\n12 \xE0\xA4\x95\xE0\xA5\x81\n"), 1);
    CHECK(uni::devanagariCluster(U("\xE0\xA4\x95\xE0\xA4\xBF")) == 0xE001);
    CHECK(uni::devanagariCluster(U("\xE0\xA4\x95\xE0\xA5\x81")) == 0);   // outside the Private Use range
    CHECK_EQ(uni::setDevanagariClusters(embedded::text("assets/fonts/devanagari-clusters.txt")) > 1000, true);
    // No table: the text is left as it is.
    uni::setDevanagariClusters("");
    CHECK(uni::shape(hi).text == hi);
    uni::setDevanagariClusters(embedded::text("assets/fonts/devanagari-clusters.txt"));
}

// Every syllable of the Hindi texts (UI, coach subtitles, opening names) is drawn right: either a
// precomposed glyph of the fonts, or letters the fonts draw side by side (a consonant or vowel with
// spacing vowel signs only). tools/prepare_fonts.py must be run again when this fails.
TEST(devanagari_clusters_cover_the_hindi_texts) {
    i18n::tr("menu.play");
    auto plain = [](const std::u32string& syl) {
        for (size_t i = 1; i < syl.size(); ++i) {
            const char32_t c = syl[i];
            if (!(c == 0x093E || c == 0x0940 || (c >= 0x0949 && c <= 0x094C) || c == 0x0903 || c == 0x093B ||
                  c == 0x094E || c == 0x094F))
                return false;
        }
        return true;
    };
    std::vector<std::string> paths = {"assets/i18n/hi.lang", "assets/coach/openings/hi.lang"};
    for (const char* f : {"common", "events", "lesson", "challenge", "analysis", "appraisal", "review"})
        paths.push_back(std::string("assets/coach/speech/hi/") + f + ".lang");
    int texts = 0, missing = 0;
    for (const std::string& path : paths) {
        REQUIRE(embedded::find(path.c_str()) != nullptr);
        const std::string text = embedded::text(path.c_str());
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
            if (line.substr(0, eq).find(".spoken") != std::string::npos) continue;
            const std::u32string v = uni::decode(line.substr(eq + 1));
            ++texts;
            for (const auto& s : uni::devanagariSyllables(v)) {
                const std::u32string syl = v.substr(size_t(s.first), size_t(s.second - s.first));
                if (syl.size() < 2 || plain(syl) || uni::devanagariCluster(syl)) continue;
                if (++missing <= 10) std::fprintf(stderr, "  %s: syllable \"%s\" has no precomposed glyph\n", path.c_str(), uni::encode(syl).c_str());
            }
        }
    }
    CHECK(texts > 1000);
    CHECK_EQ(missing, 0);
}
