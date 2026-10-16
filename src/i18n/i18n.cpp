#include "i18n.h"
#include "unicode.h"
#include "../core/embedded.h"
#include "../core/log.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace i18n {
namespace {

struct Table {
    std::string code;
    std::unordered_map<std::string, std::string> map;
};

Table g_english, g_current;
bool g_loaded = false;
int g_generation = 0;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string unescape(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char c = v[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            out += v[i];
        }
    }
    return out;
}

bool loadTable(const std::string& code, Table& t) {
    t.code = code;
    t.map.clear();
    std::string path = "assets/i18n/" + code + ".lang";
    if (!embedded::find(path.c_str())) {
        LOGW("i18n: %s is not embedded", path.c_str());
        return false;
    }
    std::string text = embedded::text(path.c_str());
    std::vector<std::pair<std::string, std::string>> entries;
    std::string err;
    if (!parse(text, entries, &err)) LOGW("i18n: %s: %s", path.c_str(), err.c_str());
    for (auto& e : entries) t.map[e.first] = e.second;
    return true;
}

void ensureEnglish() {
    if (g_loaded) return;
    g_loaded = true;
    loadTable("en", g_english);
    // The Devanagari syllables the fonts draw precomposed (uni::shapeDevanagari).
    const char* clusters = "assets/fonts/devanagari-clusters.txt";
    if (embedded::find(clusters)) uni::setDevanagariClusters(embedded::text(clusters));
    else LOGW("i18n: %s is not embedded", clusters);
    if (g_current.code.empty()) g_current = g_english;
}

}  // namespace

// The native names live here, not in the .lang files: the selector shows every language in its
// own name whatever the current one. English first (the reference), then by native name: Latin
// script (alphabetical), Greek, Cyrillic, Arabic, Devanagari, Hangul, Japanese, Chinese. The fourth
// field false: not adapted yet (Language::adapted).
const std::vector<Language>& languages() {
    static const std::vector<Language> list = {
        {"en", "English", false},
        {"id", "Bahasa Indonesia", false, false},
        {"cs", "\xC4\x8C" "e\xC5\xA1tina", false, false},                                   // Čeština
        {"da", "Dansk", false},
        {"de", "Deutsch", false},
        {"et", "Eesti", false, false},
        {"es", "Espa\xC3\xB1ol", false},
        {"fr", "Fran\xC3\xA7" "ais", false},
        {"hr", "Hrvatski", false, false},
        {"it", "Italiano", false},
        {"lv", "Latvie\xC5\xA1u", false, false},                                             // Latviešu
        {"lt", "Lietuvi\xC5\xB3", false, false},                                             // Lietuvių
        {"hu", "Magyar", false, false},
        {"nl", "Nederlands", false, false},
        {"pl", "Polski", false, false},
        {"pt-BR", "Portugu\xC3\xAAs (Brasil)", false},
        {"pt", "Portugu\xC3\xAAs (Portugal)", false},
        {"ro", "Rom\xC3\xA2n\xC4\x83", false, false},                                        // Română
        {"sk", "Sloven\xC4\x8Dina", false, false},                                           // Slovenčina
        {"sl", "Sloven\xC5\xA1\xC4\x8Dina", false, false},                                   // Slovenščina
        {"fi", "Suomi", false, false},
        {"sv", "Svenska", false, false},
        {"vi", "Ti\xE1\xBA\xBFng Vi\xE1\xBB\x87t", false, false},                            // Tiếng Việt
        {"tr", "T\xC3\xBCrk\xC3\xA7" "e", false, false},                                     // Türkçe
        {"el", "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC", false, false},  // Ελληνικά
        {"bg", "\xD0\x91\xD1\x8A\xD0\xBB\xD0\xB3\xD0\xB0\xD1\x80\xD1\x81\xD0\xBA\xD0\xB8", false},  // Български
        {"ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", false},  // Русский
        {"uk", "\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0", false},  // Українська
        {"ar", "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9", true},  // العربية
        {"hi", "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80", false},  // हिन्दी
        {"ko", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", false},                      // 한국어
        {"ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", false},                      // 日本語
        {"zh-Hans", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87", false},  // 简体中文
        {"zh-Hant", "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87", false},  // 繁體中文
    };
    return list;
}

int languageIndex(const std::string& code) {
    const auto& l = languages();
    for (size_t i = 0; i < l.size(); ++i)
        if (code == l[i].code) return int(i);
    return -1;
}

bool setLanguage(const std::string& code) {
    ensureEnglish();
    ++g_generation;
    int idx = languageIndex(code);
    if (idx < 0 || code == "en") {
        g_current = g_english;
        g_current.code = "en";
        return idx >= 0;
    }
    Table t;
    if (!loadTable(code, t)) {
        g_current = g_english;
        g_current.code = "en";
        return false;
    }
    g_current = std::move(t);
    LOGI("i18n: language %s (%d strings)", code.c_str(), int(g_current.map.size()));
    return true;
}

const std::string& language() {
    ensureEnglish();
    return g_current.code;
}

bool rtl() {
    int idx = languageIndex(language());
    return idx >= 0 && languages()[size_t(idx)].rtl;
}

int generation() { return g_generation; }

std::string matchLocale(const std::string& tag) {
    // Normalise "zh_TW.UTF-8@x" -> subtags {"zh", "tw"}.
    std::string t;
    for (char c : tag) {
        if (c == '.' || c == '@') break;
        t += c == '_' ? '-' : char(std::tolower(static_cast<unsigned char>(c)));
    }
    std::vector<std::string> sub;
    size_t p = 0;
    while (p <= t.size()) {
        size_t q = t.find('-', p);
        sub.push_back(t.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    const std::string& lang = sub.empty() ? t : sub[0];
    if (lang == "pt") {
        for (size_t i = 1; i < sub.size(); ++i)
            if (sub[i] == "br") return "pt-BR";
        return "pt";
    }
    if (lang == "in") return "id";   // the old ISO 639 code of Indonesian (Java, older Android)
    if (lang == "zh") {
        for (size_t i = 1; i < sub.size(); ++i) {
            if (sub[i] == "hant" || sub[i] == "tw" || sub[i] == "hk" || sub[i] == "mo") return "zh-Hant";
            if (sub[i] == "hans") return "zh-Hans";
        }
        return "zh-Hans";
    }
    for (const Language& l : languages())
        if (lang == l.code) return l.code;
    return "en";
}

const char* tr(const char* key) {
    ensureEnglish();
    auto it = g_current.map.find(key);
    if (it != g_current.map.end()) return it->second.c_str();
    auto en = g_english.map.find(key);
    if (en != g_english.map.end()) return en->second.c_str();
    return key;
}

std::string tr(const std::string& key) { return tr(key.c_str()); }

const char* english(const char* key) {
    ensureEnglish();
    auto en = g_english.map.find(key);
    return en != g_english.map.end() ? en->second.c_str() : key;
}

std::string trf(const char* key, std::initializer_list<std::string> args) {
    return format(tr(key), std::vector<std::string>(args));
}

// CLDR plural rules for integer counts (the forms a decimal count would need are never used).
int pluralForms(const std::string& code) {
    if (code == "ar") return 6;
    if (code == "sl") return 4;
    if (code == "ru" || code == "uk" || code == "pl" || code == "cs" || code == "sk" || code == "hr" ||
        code == "lt" || code == "lv" || code == "ro")
        return 3;
    if (code == "ja" || code == "zh-Hans" || code == "zh-Hant" || code == "ko" || code == "id" || code == "vi")
        return 1;
    return 2;
}

int pluralIndex(const std::string& code, long long n) {
    long long a = n < 0 ? -n : n;
    long long m10 = a % 10, m100 = a % 100;
    if (code == "ru" || code == "uk" || code == "hr") {   // one|few|many (Croatian: one|few|other)
        if (m10 == 1 && m100 != 11) return 0;
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return 1;
        return 2;
    }
    if (code == "pl") {   // one|few|many
        if (a == 1) return 0;
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return 1;
        return 2;
    }
    if (code == "cs" || code == "sk") return a == 1 ? 0 : a >= 2 && a <= 4 ? 1 : 2;   // one|few|other
    if (code == "lt") {   // one|few|other
        if (m100 >= 11 && m100 <= 19) return 2;
        return m10 == 1 ? 0 : m10 >= 2 ? 1 : 2;
    }
    if (code == "lv") {   // zero|one|other
        if (m10 == 0 || (m100 >= 11 && m100 <= 19)) return 0;
        return m10 == 1 ? 1 : 2;
    }
    if (code == "ro") return a == 1 ? 0 : a == 0 || (m100 >= 1 && m100 <= 19) ? 1 : 2;   // one|few|other
    if (code == "sl") return m100 == 1 ? 0 : m100 == 2 ? 1 : m100 == 3 || m100 == 4 ? 2 : 3;   // one|two|few|other
    if (code == "ar") {
        if (a == 0) return 0;
        if (a == 1) return 1;
        if (a == 2) return 2;
        if (m100 >= 3 && m100 <= 10) return 3;
        if (m100 >= 11) return 4;
        return 5;
    }
    if (pluralForms(code) == 1) return 0;
    if (code == "fr" || code == "pt-BR" || code == "hi") return a <= 1 ? 0 : 1;   // 0 and 1 are singular
    return a == 1 ? 0 : 1;
}

std::string trn(const char* key, long long n, std::initializer_list<std::string> args) {
    ensureEnglish();
    // The forms come from the table that provides the key (a missing translation falls back to
    // English with the English rule).
    auto it = g_current.map.find(key);
    const std::string* value = nullptr;
    std::string code = g_current.code;
    if (it != g_current.map.end()) {
        value = &it->second;
    } else {
        auto en = g_english.map.find(key);
        if (en == g_english.map.end()) return key;
        value = &en->second;
        code = "en";
    }
    std::vector<std::string> forms;
    size_t p = 0;
    while (true) {
        size_t q = value->find('|', p);
        forms.push_back(value->substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    size_t idx = std::min(size_t(pluralIndex(code, n)), forms.size() - 1);
    std::vector<std::string> a(args);
    if (a.empty()) a.push_back(std::to_string(n));
    return format(forms[idx], a);
}

std::string trOr(const std::string& key, const std::string& fallback) {
    return has(key.c_str()) ? std::string(tr(key.c_str())) : fallback;
}

std::vector<uint32_t> codepoints() {
    ensureEnglish();
    std::vector<uint32_t> out;
    for (auto& kv : g_current.map) {
        uni::Shaped sh = uni::shape(uni::decode(kv.second));
        for (char32_t c : sh.text)
            if (c > ' ' && c != '|') out.push_back(uint32_t(c));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string format(const std::string& s, const std::vector<std::string>& a) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{') {
            size_t j = s.find('}', i);
            if (j != std::string::npos && j > i + 1 && j < i + 5) {
                bool digits = true;
                for (size_t k = i + 1; k < j; ++k) digits = digits && s[k] >= '0' && s[k] <= '9';
                if (digits) {
                    size_t n = size_t(std::stoi(s.substr(i + 1, j - i - 1)));
                    if (n < a.size()) out += a[n];
                    i = j;
                    continue;
                }
            }
        }
        out += s[i];
    }
    return out;
}

std::string ltr(const std::string& s) {
    if (!rtl() || s.empty()) return s;
    return "\xE2\x80\x8E" + s + "\xE2\x80\x8E";  // U+200E LEFT-TO-RIGHT MARK
}

bool has(const char* key) {
    ensureEnglish();
    return g_current.map.count(key) || g_english.map.count(key);
}

bool parse(const std::string& text, std::vector<std::pair<std::string, std::string>>& out, std::string* error) {
    bool ok = true;
    size_t pos = 0;
    int lineNo = 0;
    // Skip a UTF-8 byte order mark.
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        ++lineNo;
        pos = nl + 1;
        if (line.empty() || line[0] == '#') {
            if (nl == text.size()) break;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (error && ok) *error = "line " + std::to_string(lineNo) + ": missing '='";
            ok = false;
        } else {
            std::string k = trim(line.substr(0, eq));
            std::string v = unescape(trim(line.substr(eq + 1)));
            if (k.empty()) {
                if (error && ok) *error = "line " + std::to_string(lineNo) + ": empty key";
                ok = false;
            } else {
                out.emplace_back(k, v);
            }
        }
        if (nl == text.size()) break;
    }
    return ok;
}

}  // namespace i18n
