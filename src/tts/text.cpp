#include "text.h"
#include "i18n/unicode.h"
#include <algorithm>

namespace tts {
namespace text {

// Generated tables (nfkd_table.cpp, tools/tts_nfkd_table.py).
extern const size_t kNfkdCount;
extern const uint32_t kNfkdCodepoints[];
extern const uint16_t kNfkdOffsets[];
extern const uint32_t kNfkdPool[];
extern const size_t kCccCount;
extern const uint32_t kCccCodepoints[];
extern const uint8_t kCccClasses[];

namespace {

int combiningClass(char32_t c) {
    const uint32_t* end = kCccCodepoints + kCccCount;
    const uint32_t* it = std::lower_bound(kCccCodepoints, end, uint32_t(c));
    return it != end && *it == uint32_t(c) ? kCccClasses[it - kCccCodepoints] : 0;
}

// Python's str.isspace() (what \s and strip() use for str patterns).
bool isSpace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

// The emoji class of the official regex.
bool isEmoji(char32_t c) {
    return (c >= 0x1F600 && c <= 0x1F64F) || (c >= 0x1F300 && c <= 0x1F5FF) || (c >= 0x1F680 && c <= 0x1F6FF) ||
           (c >= 0x1F700 && c <= 0x1F77F) || (c >= 0x1F780 && c <= 0x1F7FF) || (c >= 0x1F800 && c <= 0x1F8FF) ||
           (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x1FA00 && c <= 0x1FA6F) || (c >= 0x1FA70 && c <= 0x1FAFF) ||
           (c >= 0x2600 && c <= 0x26FF) || (c >= 0x2700 && c <= 0x27BF) || (c >= 0x1F1E6 && c <= 0x1F1FF);
}

// str.replace: every non-overlapping occurrence, left to right.
void replaceAll(std::u32string& s, const std::u32string& from, const std::u32string& to) {
    if (from.empty()) return;
    std::u32string out;
    size_t pos = 0;
    bool any = false;
    for (;;) {
        size_t f = s.find(from, pos);
        if (f == std::u32string::npos) break;
        any = true;
        out.append(s, pos, f - pos);
        out += to;
        pos = f + from.size();
    }
    if (!any) return;
    out.append(s, pos, std::u32string::npos);
    s.swap(out);
}

std::u32string u32(const char* ascii) {
    std::u32string r;
    for (; *ascii; ++ascii) r.push_back(char32_t(static_cast<unsigned char>(*ascii)));
    return r;
}

const char* const kLanguages[] = {"en", "ko", "ja", "ar", "bg", "cs", "da", "de", "el", "es", "et",
                                  "fi", "fr", "hi", "hr", "hu", "id", "it", "lt", "lv", "nl", "pl",
                                  "pt", "ro", "ru", "sk", "sl", "sv", "tr", "uk", "vi", "na"};

}  // namespace

std::u32string nfkd(const std::u32string& s) {
    std::u32string out;
    out.reserve(s.size() + 8);
    const uint32_t* end = kNfkdCodepoints + kNfkdCount;
    for (char32_t c : s) {
        if (c >= 0xAC00 && c <= 0xD7A3) {   // Hangul syllable: L V [T]
            uint32_t si = c - 0xAC00;
            out.push_back(char32_t(0x1100 + si / 588));
            out.push_back(char32_t(0x1161 + (si % 588) / 28));
            if (si % 28) out.push_back(char32_t(0x11A7 + si % 28));
            continue;
        }
        const uint32_t* it = std::lower_bound(kNfkdCodepoints, end, uint32_t(c));
        if (it != end && *it == uint32_t(c)) {
            size_t i = size_t(it - kNfkdCodepoints);
            for (uint32_t k = kNfkdOffsets[i]; k < kNfkdOffsets[i + 1]; ++k) out.push_back(char32_t(kNfkdPool[k]));
        } else {
            out.push_back(c);
        }
    }
    // Canonical ordering: stable sort of every run of non-starters by combining class.
    for (size_t i = 1; i < out.size(); ++i) {
        int cc = combiningClass(out[i]);
        if (cc == 0) continue;
        size_t j = i;
        while (j > 0) {
            int prev = combiningClass(out[j - 1]);
            if (prev == 0 || prev <= cc) break;
            std::swap(out[j - 1], out[j]);
            --j;
        }
    }
    return out;
}

bool modelLanguage(const std::string& lang) {
    for (const char* l : kLanguages)
        if (lang == l) return true;
    return false;
}

std::u32string preprocess(const std::string& utf8, const std::string& lang) {
    std::u32string t = nfkd(uni::decode(utf8));
    t.erase(std::remove_if(t.begin(), t.end(), isEmoji), t.end());
    // Single characters replaced (the official dictionary; none of the outputs is itself a key).
    std::u32string r;
    r.reserve(t.size());
    for (char32_t c : t) {
        switch (c) {
        case 0x2013: case 0x2011: case 0x2014: r.push_back('-'); break;
        case '_': case '[': case ']': case '|': case '/': case '#': case 0x2192: case 0x2190: r.push_back(' '); break;
        case 0x201C: case 0x201D: r.push_back('"'); break;
        case 0x2018: case 0x2019: case 0x00B4: case '`': r.push_back('\''); break;
        case 0x2665: case 0x2606: case 0x2661: case 0x00A9: case '\\': break;   // removed
        default: r.push_back(c);
        }
    }
    t.swap(r);
    replaceAll(t, u32("@"), u32(" at "));
    replaceAll(t, u32("e.g.,"), u32("for example, "));
    replaceAll(t, u32("i.e.,"), u32("that is, "));
    for (const char* p : {",", ".", "!", "?", ";", ":", "'"}) replaceAll(t, u32(" ") + u32(p), u32(p));
    for (const char* q : {"\"", "'", "`"}) {
        std::u32string two = u32(q) + u32(q);
        while (t.find(two) != std::u32string::npos) replaceAll(t, two, u32(q));
    }
    // \s+ -> " ", then strip().
    r.clear();
    for (size_t i = 0; i < t.size();) {
        if (isSpace(t[i])) {
            while (i < t.size() && isSpace(t[i])) ++i;
            r.push_back(' ');
        } else {
            r.push_back(t[i++]);
        }
    }
    size_t b = 0, e = r.size();
    while (b < e && r[b] == ' ') ++b;
    while (e > b && r[e - 1] == ' ') --e;
    t = r.substr(b, e - b);
    static const std::u32string kEnders = U".!?;:,'\")]}…。」』】〉》›»";
    if (t.empty() || kEnders.find(t.back()) == std::u32string::npos) t.push_back('.');
    std::u32string tag = u32(lang.c_str());
    return U"<" + tag + U">" + t + U"</" + tag + U">";
}

std::vector<int64_t> indices(const std::u32string& s, const int32_t* indexer, int* dropped) {
    std::vector<int64_t> ids;
    ids.reserve(s.size());
    int bad = 0;
    for (char32_t c : s) {
        int32_t id = c < 0x10000 ? indexer[c] : -1;
        if (id < 0) ++bad;
        else ids.push_back(id);
    }
    if (dropped) *dropped = bad;
    return ids;
}

size_t chunkLength(const std::string& lang) { return lang == "ko" || lang == "ja" ? 120 : 300; }

namespace {

bool isWordChar(char32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

// The abbreviations of the official sentence splitter (no break after them).
bool endsWithAbbreviation(const std::u32string& s, size_t dotPos) {
    static const char* const kAbbr[] = {"Mr.", "Mrs.", "Ms.", "Dr.", "Prof.", "Sr.", "Jr.", "Ph.D.", "etc.", "e.g.",
                                        "i.e.", "vs.", "Inc.", "Ltd.", "Co.", "Corp.", "St.", "Ave.", "Blvd."};
    for (const char* a : kAbbr) {
        std::u32string w = u32(a);
        if (dotPos + 1 >= w.size() && s.compare(dotPos + 1 - w.size(), w.size(), w) == 0) return true;
    }
    // \b[A-Z]\. (an initial)
    if (dotPos >= 1 && s[dotPos - 1] >= 'A' && s[dotPos - 1] <= 'Z' && (dotPos < 2 || !isWordChar(s[dotPos - 2])))
        return true;
    return false;
}

std::u32string trimmed(const std::u32string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::vector<std::u32string> sentences(const std::u32string& p) {
    std::vector<std::u32string> out;
    size_t start = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        char32_t c = p[i];
        bool cjk = c == 0x3002 || c == 0xFF01 || c == 0xFF1F || c == 0xFF61;   // 。！？｡
        bool western = c == '.' || c == '!' || c == '?' || c == 0x061F || c == 0x061B || c == 0x06D4;
        if (!cjk && !western) continue;
        size_t j = i + 1;
        // Closing quotes and brackets stay with their sentence.
        while (j < p.size() && (p[j] == 0x300D || p[j] == 0x300F || p[j] == ')' || p[j] == '"' || p[j] == 0xBB)) ++j;
        if (j < p.size() && !isSpace(p[j]) && !cjk) continue;
        if (c == '.' && endsWithAbbreviation(p, i)) continue;
        if (j >= p.size()) break;
        std::u32string s = trimmed(p.substr(start, j - start));
        if (!s.empty()) out.push_back(s);
        while (j < p.size() && isSpace(p[j])) ++j;
        start = j;
        i = j - 1;
    }
    std::u32string s = trimmed(p.substr(start));
    if (!s.empty()) out.push_back(s);
    return out;
}

// Cuts an over-long sentence: at commas, then spaces, then anywhere.
void cutLong(const std::u32string& s, size_t maxLen, std::vector<std::u32string>& out) {
    if (s.size() <= maxLen) {
        out.push_back(s);
        return;
    }
    size_t cut = std::u32string::npos;
    for (size_t i = maxLen; i-- > maxLen / 3;) {
        char32_t c = s[i];
        if (c == ',' || c == 0x060C || c == 0x3001 || c == 0xFF0C) {
            cut = i + 1;
            break;
        }
    }
    if (cut == std::u32string::npos)
        for (size_t i = maxLen; i-- > maxLen / 3;)
            if (isSpace(s[i])) {
                cut = i;
                break;
            }
    if (cut == std::u32string::npos) cut = maxLen;
    std::u32string head = trimmed(s.substr(0, cut)), tail = trimmed(s.substr(cut));
    if (!head.empty()) out.push_back(head);
    if (!tail.empty()) cutLong(tail, maxLen, out);
}

}  // namespace

std::vector<std::string> chunk(const std::string& utf8, size_t maxLen) {
    if (maxLen < 8) maxLen = 8;
    std::u32string all = uni::decode(utf8);
    // Paragraphs: \n\s*\n+
    std::vector<std::u32string> paragraphs;
    size_t start = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] != '\n') continue;
        size_t j = i + 1;
        while (j < all.size() && isSpace(all[j]) && all[j] != '\n') ++j;
        if (j < all.size() && all[j] == '\n') {
            paragraphs.push_back(all.substr(start, i - start));
            while (j < all.size() && isSpace(all[j])) ++j;
            start = j;
            i = j - 1;
        }
    }
    paragraphs.push_back(all.substr(start));
    std::vector<std::string> chunks;
    for (const std::u32string& para : paragraphs) {
        std::u32string p = trimmed(para);
        if (p.empty()) continue;
        std::vector<std::u32string> pieces;
        for (const std::u32string& s : sentences(p)) cutLong(s, maxLen, pieces);
        std::u32string cur;
        for (const std::u32string& s : pieces) {
            if (!cur.empty() && cur.size() + 1 + s.size() > maxLen) {
                chunks.push_back(uni::encode(cur));
                cur.clear();
            }
            // Sentences of scripts written without spaces are joined without one.
            if (!cur.empty() && !(cur.back() == 0x3002 || cur.back() == 0xFF01 || cur.back() == 0xFF1F ||
                                  cur.back() == 0xFF61 || cur.back() == 0x300D || cur.back() == 0x300F))
                cur.push_back(' ');
            cur += s;
        }
        if (!cur.empty()) chunks.push_back(uni::encode(cur));
    }
    return chunks;
}

}  // namespace text
}  // namespace tts
