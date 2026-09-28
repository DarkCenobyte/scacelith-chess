#include "i18n.h"
#include "../core/embedded.h"
#include "../core/log.h"
#include <unordered_map>

namespace i18n {
namespace {

struct Table {
    std::string code;
    std::unordered_map<std::string, std::string> map;
};

Table g_english, g_current;
bool g_loaded = false;

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
    if (g_current.code.empty()) g_current = g_english;
}

}  // namespace

const std::vector<Language>& languages() {
    static const std::vector<Language> list = {
        {"en", "English", false},        {"fr", "Fran\xC3\xA7" "ais", false},
        {"de", "Deutsch", false},        {"es", "Espa\xC3\xB1ol", false},
        {"uk", "\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0", false},
        {"ar", "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9", true},
        {"ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", false},
        {"ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", false},
        {"zh-Hant", "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87", false},
        {"zh-Hans", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87", false},
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
    int idx = languageIndex(code);
    if (idx <= 0) {
        g_current = g_english;
        g_current.code = "en";
        return idx == 0;
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

const char* tr(const char* key) {
    ensureEnglish();
    auto it = g_current.map.find(key);
    if (it != g_current.map.end()) return it->second.c_str();
    auto en = g_english.map.find(key);
    if (en != g_english.map.end()) return en->second.c_str();
    return key;
}

std::string tr(const std::string& key) { return tr(key.c_str()); }

std::string trf(const char* key, std::initializer_list<std::string> args) {
    std::string s = tr(key);
    std::vector<std::string> a(args);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{') {
            size_t j = s.find('}', i);
            if (j != std::string::npos && j > i + 1) {
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
