#include "catalog.h"
#include "../core/embedded.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include "../tts/tts.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>

namespace coach {
namespace {

const char* const kLrm = "\xE2\x80\x8E";   // U+200E LEFT-TO-RIGHT MARK (as i18n::ltr)
constexpr int kMaxDepth = 6;               // nested Text arguments / patterns

const char* pieceName(chess::PieceType t) {
    switch (t) {
    case chess::Pawn: return "pawn";
    case chess::Knight: return "knight";
    case chess::Bishop: return "bishop";
    case chess::Rook: return "rook";
    case chess::Queen: return "queen";
    case chess::King: return "king";
    default: return "";
    }
}

chess::PieceType pieceFromLetter(char c) {
    switch (c) {
    case 'N': return chess::Knight;
    case 'B': return chess::Bishop;
    case 'R': return chess::Rook;
    case 'Q': return chess::Queen;
    case 'K': return chess::King;
    default: return chess::NoPiece;
    }
}

const char* figurine(chess::PieceType t) {
    switch (t) {
    case chess::King: return "\xE2\x99\x94";     // U+2654
    case chess::Queen: return "\xE2\x99\x95";    // U+2655
    case chess::Rook: return "\xE2\x99\x96";     // U+2656
    case chess::Bishop: return "\xE2\x99\x97";   // U+2657
    case chess::Knight: return "\xE2\x99\x98";   // U+2658
    default: return "";
    }
}

uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

uint32_t mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

bool endsWith(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

// SAN split into its parts (no board: the text alone says what is needed to speak it).
struct San {
    bool valid = false;
    bool castleShort = false, castleLong = false;
    chess::PieceType piece = chess::Pawn;
    std::string from;                      // disambiguation ("b", "1", "b1"); pawn captures: the file
    bool capture = false;
    chess::Square to = chess::NoSquare;
    chess::PieceType promotion = chess::NoPiece;
    bool check = false, mate = false;
};

San parseSan(std::string s) {
    San m;
    while (!s.empty() && (s.back() == '+' || s.back() == '#' || s.back() == '!' || s.back() == '?')) {
        if (s.back() == '+') m.check = true;
        if (s.back() == '#') m.mate = true;
        s.pop_back();
    }
    if (s == "O-O" || s == "0-0") { m.valid = m.castleShort = true; return m; }
    if (s == "O-O-O" || s == "0-0-0") { m.valid = m.castleLong = true; return m; }
    if (s.empty()) return m;
    size_t i = 0;
    if (pieceFromLetter(s[0]) != chess::NoPiece) m.piece = pieceFromLetter(s[i++]);
    // Promotion at the end: "=Q", "Q" or "(Q)".
    if (!s.empty() && s.back() == ')') s.pop_back();
    if (s.size() > i + 2 && pieceFromLetter(s.back()) != chess::NoPiece) {
        m.promotion = pieceFromLetter(s.back());
        s.pop_back();
        if (!s.empty() && (s.back() == '=' || s.back() == '(')) s.pop_back();
    }
    if (s.size() < i + 2) return m;
    m.to = chess::parseSquare(s.substr(s.size() - 2));
    if (m.to == chess::NoSquare) return m;
    std::string mid = s.substr(i, s.size() - 2 - i);
    if (!mid.empty() && (mid.back() == 'x' || mid.back() == ':')) { m.capture = true; mid.pop_back(); }
    if (!mid.empty() && mid.back() == '-') mid.pop_back();   // long algebraic "Ng1-f3"
    for (char c : mid)
        if (!((c >= 'a' && c <= 'h') || (c >= '1' && c <= '8'))) return m;
    m.from = mid;
    m.valid = true;
    return m;
}

}  // namespace

// ==== Languages ==================================================================================
bool speechSupported(const std::string& lang) { return tts::languageSupported(lang); }

std::string speechLanguage(const std::string& uiLanguage) {
    return speechSupported(uiLanguage) ? uiLanguage : std::string("en");
}

// ==== Written SAN ================================================================================
std::string figurineSan(const std::string& san) {
    std::string out;
    out.reserve(san.size() + 8);
    for (size_t i = 0; i < san.size(); ++i) {
        char c = san[i];
        chess::PieceType t = pieceFromLetter(c);
        // A piece letter starts the move or follows '=' / a rank digit (promotion without '=').
        bool promo = i > 0 && (san[i - 1] == '=' || (san[i - 1] >= '1' && san[i - 1] <= '8'));
        if (t != chess::NoPiece && (i == 0 || promo)) {
            if (promo && san[i - 1] != '=') out += '=';
            out += figurine(t);
        } else {
            out += c;
        }
    }
    return out;
}

std::string sanWithLetters(const std::string& san, const std::string& letters) {
    // letters: "K Q R B N" in that order (space separated, a letter may be several characters).
    std::vector<std::string> l;
    size_t p = 0;
    while (p < letters.size()) {
        size_t q = letters.find(' ', p);
        if (q == std::string::npos) q = letters.size();
        if (q > p) l.push_back(letters.substr(p, q - p));
        p = q + 1;
    }
    if (l.size() != 5) return san;
    auto letterOf = [&](chess::PieceType t) -> const std::string& {
        static const std::string empty;
        switch (t) {
        case chess::King: return l[0];
        case chess::Queen: return l[1];
        case chess::Rook: return l[2];
        case chess::Bishop: return l[3];
        case chess::Knight: return l[4];
        default: return empty;
        }
    };
    std::string out;
    for (size_t i = 0; i < san.size(); ++i) {
        char c = san[i];
        chess::PieceType t = pieceFromLetter(c);
        bool promo = i > 0 && (san[i - 1] == '=' || (san[i - 1] >= '1' && san[i - 1] <= '8'));
        if (t != chess::NoPiece && (i == 0 || promo)) {
            if (promo && san[i - 1] != '=') out += '=';
            out += letterOf(t);
        } else {
            out += c;
        }
    }
    return out;
}

// ==== Loading ====================================================================================
Catalog& Catalog::shared() {
    static Catalog catalog;
    static std::once_flag once;
    std::call_once(once, [] { catalog.load(); });
    return catalog;
}

void Catalog::clear() {
    langs_.clear();
    problems_.clear();
    resetHistory();
}

bool Catalog::addFile(const std::string& lang, const std::string& topic, const std::string& text) {
    std::vector<std::pair<std::string, std::string>> entries;
    std::string err;
    bool ok = i18n::parse(text, entries, &err);
    if (!ok) {
        problems_.push_back(lang + "/" + topic + ": " + err);
        LOGW("coach catalog: %s/%s: %s", lang.c_str(), topic.c_str(), err.c_str());
    }
    Language& L = langs_[lang];
    auto& order = L.topicKeys[topic];
    for (auto& e : entries) {
        auto it = L.map.find(e.first);
        if (it != L.map.end()) {
            std::string msg = lang + "/" + topic + ": duplicate key " + e.first + " (first in " + it->second.topic + ")";
            problems_.push_back(msg);
            LOGW("coach catalog: %s", msg.c_str());
            ok = false;
            continue;
        }
        L.map.emplace(e.first, Entry{e.second, topic});
        order.push_back(e.first);
        if (startsWith(e.first, "respell.")) {
            Respelling r;
            std::string word = e.first.substr(8);
            r.pauseOnly = endsWith(word, ".pause");
            if (r.pauseOnly) word.resize(word.size() - 6);
            r.word = uni::decode(word);
            r.spoken = uni::decode(e.second);
            if (!r.word.empty()) L.respellings.push_back(std::move(r));
        }
    }
    return ok;
}

bool Catalog::load() {
    clear();
    size_t count = 0;
    const embedded::File* files = embedded::all(&count);
    const std::string speech = "assets/coach/speech/", openings = "assets/coach/openings/";
    int loaded = 0;
    for (size_t i = 0; i < count; ++i) {
        std::string path = files[i].path;
        if (!endsWith(path, ".lang")) continue;
        std::string lang, topic;
        if (startsWith(path, speech.c_str())) {
            std::string rest = path.substr(speech.size());   // <code>/<topic>.lang
            size_t slash = rest.find('/');
            if (slash == std::string::npos || rest.find('/', slash + 1) != std::string::npos) continue;
            lang = rest.substr(0, slash);
            topic = rest.substr(slash + 1, rest.size() - slash - 1 - 5);
        } else if (startsWith(path, openings.c_str())) {
            std::string rest = path.substr(openings.size());  // <code>.lang
            if (rest.find('/') != std::string::npos) continue;
            lang = rest.substr(0, rest.size() - 5);
            topic = "openings";
        } else {
            continue;
        }
        addFile(lang, topic, embedded::text(path.c_str()));
        ++loaded;
    }
    if (!langs_.count("en")) {
        LOGE("coach catalog: no English speech files embedded");
        return false;
    }
    LOGI("coach catalog: %d files, %d languages", loaded, int(langs_.size()));
    return true;
}

// ==== Queries ====================================================================================
const Catalog::Language* Catalog::language(const std::string& code) const {
    auto it = langs_.find(code);
    return it == langs_.end() ? nullptr : &it->second;
}

const std::string* Catalog::find(const std::string& lang, const std::string& key) const {
    const Language* L = language(lang);
    if (!L) return nullptr;
    auto it = L->map.find(key);
    return it == L->map.end() ? nullptr : &it->second.value;
}

bool Catalog::has(const std::string& key) const { return find("en", key) != nullptr; }
bool Catalog::has(const std::string& lang, const std::string& key) const { return find(lang, key) != nullptr; }

int Catalog::variants(const std::string& key) const {
    if (!has(key)) return 0;
    int n = 1;
    while (has(key + "." + std::to_string(n + 1))) ++n;
    return n;
}

std::vector<std::string> Catalog::languages() const {
    std::vector<std::string> out;
    for (auto& l : langs_) out.push_back(l.first);
    return out;
}

std::vector<std::string> Catalog::topics(const std::string& lang) const {
    std::vector<std::string> out;
    if (const Language* L = language(lang))
        for (auto& t : L->topicKeys) out.push_back(t.first);
    return out;
}

std::vector<std::string> Catalog::keys(const std::string& lang, const std::string& topic) const {
    const Language* L = language(lang);
    if (!L) return {};
    auto it = L->topicKeys.find(topic);
    return it == L->topicKeys.end() ? std::vector<std::string>() : it->second;
}

std::vector<uint32_t> Catalog::codepoints(const std::string& lang) const {
    std::set<uint32_t> cps;
    auto addText = [&](const std::string& s) {
        for (char32_t c : uni::decode(s))
            if (c >= 0x20) cps.insert(uint32_t(c));
    };
    if (const Language* L = language(lang))
        for (auto& e : L->map)
            if (!endsWith(e.first, ".spoken")) addText(e.second.value);
    addText("0123456789+-=#x.,:()\xE2\x88\x92");   // written numbers, evaluations, SAN (U+2212 minus)
    for (int t = chess::Knight; t <= chess::King; ++t) addText(figurine(chess::PieceType(t)));
    for (char c = 'a'; c <= 'h'; ++c) cps.insert(uint32_t(c));
    return std::vector<uint32_t>(cps.begin(), cps.end());
}

// ==== Variants ===================================================================================
int Catalog::pickVariant(const std::string& key, uint32_t seed) const {
    int n = variants(key);
    if (n <= 1) return 1;
    std::lock_guard<std::mutex> lock(historyMutex_);
    History& h = history_[key];
    for (auto& p : h.bySeed)
        if (p.first == seed) return p.second;
    // Keys with three phrasings or more skip the last two picks, two phrasings alternate ("well
    // done" three times in five moves is what players notice).
    size_t avoid = n >= 3 ? 2 : 1;
    std::vector<int> candidates;
    for (int v = 1; v <= n; ++v) {
        bool recent = false;
        for (size_t k = 0; k < h.recent.size() && k < avoid; ++k)
            if (h.recent[h.recent.size() - 1 - k] == v) recent = true;
        if (!recent) candidates.push_back(v);
    }
    if (candidates.empty())
        for (int v = 1; v <= n; ++v) candidates.push_back(v);
    int pick = candidates[mix(fnv1a(key) ^ mix(seed + 0x9E3779B9u)) % candidates.size()];
    h.recent.push_back(pick);
    if (h.recent.size() > 2) h.recent.erase(h.recent.begin());
    h.bySeed.emplace_back(seed, pick);
    if (h.bySeed.size() > 8) h.bySeed.erase(h.bySeed.begin());
    return pick;
}

void Catalog::resetHistory() const {
    std::lock_guard<std::mutex> lock(historyMutex_);
    history_.clear();
}

// ==== Rendering ==================================================================================
struct Catalog::Local {
    std::string name;
    std::string value;
    bool isNumber = false;   // 'number' is rendered with the placeholder's form ({n:f})
    int number = 0;
};

struct Catalog::Ctx {
    const Language* L = nullptr;    // the rendering language
    const Language* en = nullptr;   // fallback
    std::string lang;               // code of L
    bool spoken = false;
    bool ltrWrap = false;           // written Arabic: Latin fragments in left-to-right marks
    const Line* line = nullptr;
    uint32_t seed = 0;
    int depth = 0;
    Rendered* out = nullptr;        // top level: anchors are recorded here
};

// Template of a key: in speech the ".spoken" override first; the language, then English.
const std::string* Catalog::lookup(const Ctx& c, const std::string& key, std::string* fromLang) const {
    auto get = [](const Language* L, const std::string& k) -> const std::string* {
        if (!L) return nullptr;
        auto it = L->map.find(k);
        return it == L->map.end() ? nullptr : &it->second.value;
    };
    const std::string* v = nullptr;
    if (c.spoken) v = get(c.L, key + ".spoken");
    if (!v) v = get(c.L, key);
    if (v) {
        if (fromLang) *fromLang = c.lang;
        return v;
    }
    if (c.en && c.en != c.L) {
        if (c.spoken) v = get(c.en, key + ".spoken");
        if (!v) v = get(c.en, key);
        if (v && fromLang) *fromLang = "en";
    }
    return v;
}

std::string Catalog::pattern(Ctx& c, const std::string& key, const std::vector<Local>& locals) const {
    const std::string* t = lookup(c, key);
    if (!t) {
        LOGW("coach catalog: missing %s (%s)", key.c_str(), c.lang.c_str());
        return std::string();
    }
    return expand(c, *t, &locals, false);
}

namespace {
// True when text appended to 'out' starts a sentence (capitalise a placeholder's first letter).
bool atSentenceStart(const std::string& out) {
    std::u32string t = uni::decode(out);
    size_t i = t.size();
    while (i > 0 && (t[i - 1] == ' ' || t[i - 1] == 0x200E || t[i - 1] == 0x00A1 || t[i - 1] == 0x00BF ||
                     t[i - 1] == '"' || t[i - 1] == 0x00AB || t[i - 1] == 0x201C))
        --i;
    if (i == 0) return true;
    char32_t p = t[i - 1];
    return p == '.' || p == '!' || p == '?' || p == '\n' || p == 0x2026;
}

std::string capitalizeFirst(const std::string& s) {
    std::u32string t = uni::decode(s);
    for (char32_t& ch : t) {
        if (ch == ' ' || ch == 0x200E) continue;
        ch = uni::toUpper(ch);
        break;
    }
    return uni::encode(t);
}
}  // namespace

std::string Catalog::expand(Ctx& c, const std::string& tmpl, const std::vector<Local>* locals, bool top) const {
    std::string out;
    if (c.depth > kMaxDepth) return out;
    ++c.depth;
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] != '{') { out += tmpl[i++]; continue; }
        size_t close = tmpl.find('}', i + 1);
        if (close == std::string::npos) { out += tmpl.substr(i); break; }
        std::string token = tmpl.substr(i + 1, close - i - 1);
        bool ok = !token.empty();
        for (char ch : token)
            if (!(std::isalnum((unsigned char)ch) || ch == '_' || ch == '@' || ch == ':')) ok = false;
        if (!ok) { out += tmpl[i++]; continue; }
        i = close + 1;
        std::string name = token, form;
        size_t colon = token.find(':');
        if (colon != std::string::npos) { name = token.substr(0, colon); form = token.substr(colon + 1); }
        // Zero-width anchor of a static line.
        if (name[0] == '@') {
            if (top && c.out) c.out->anchors.push_back({name, int(out.size())});
            continue;
        }
        if (locals) {
            const Local* hit = nullptr;
            for (const Local& l : *locals)
                if (l.name == name) hit = &l;
            if (hit) {
                out += hit->isNumber ? (c.spoken ? numberWords(c, hit->number, form)
                                                 : std::to_string(hit->number))
                                     : hit->value;
                continue;
            }
        }
        const Arg* a = c.line ? c.line->arg(name) : nullptr;
        if (!a) {
            LOGW("coach catalog: no argument {%s} for %s", name.c_str(), c.line ? c.line->key.c_str() : "?");
            continue;
        }
        std::string value = renderArg(c, *a, form);
        bool capitalise = a->kind == Arg::Kind::Piece || a->kind == Arg::Kind::Text || a->kind == Arg::Kind::Opening;
        // Only at the top level: a nested text is inserted inside the parent's sentence.
        if (capitalise && top && atSentenceStart(out)) value = capitalizeFirst(value);
        if (top && c.out) c.out->anchors.push_back({name, int(out.size())});
        out += value;
    }
    --c.depth;
    return out;
}

std::string Catalog::renderArg(Ctx& c, const Arg& a, const std::string& form) const {
    switch (a.kind) {
    case Arg::Kind::Piece: return renderPiece(c, a, form);
    case Arg::Kind::Square: return renderSquare(c, a.square);
    case Arg::Kind::Move: return renderMove(c, a.san, form, nullptr);
    case Arg::Kind::Moves: return renderMoves(c, a.san);
    case Arg::Kind::Number: return renderNumber(c, a.number, form);
    case Arg::Kind::Eval: return renderEval(c, a);
    case Arg::Kind::Opening: return renderOpening(c, a.text, form);
    case Arg::Kind::Text: return renderText(c, a.text, form);
    }
    return std::string();
}

// Piece with its owner: phrase.<owner>.<type>.<form> (a whole phrase, for languages whose
// possessive changes the noun), else owner.<owner>.<form>[.<gender>] around piece.<type>.<form>.
std::string Catalog::renderPiece(Ctx& c, const Arg& a, const std::string& form) const {
    std::string type = pieceName(a.piece);
    if (type.empty()) return std::string();
    std::string owner = a.bySide ? (a.color == chess::White ? "white" : "black") : a.own ? "your" : "my";
    std::string f = form.empty() ? "nom" : form;
    Ctx local = c;
    local.en = c.L;   // grammar never mixes languages: fall back to the nominative of this language
    for (const std::string& ff : {f, std::string("nom")}) {
        if (const std::string* p = lookup(local, "phrase." + owner + "." + type + "." + ff))
            return expand(c, *p, nullptr, false);
        const std::string* noun = lookup(local, "piece." + type + "." + ff);
        if (!noun) continue;
        std::string n = expand(c, *noun, nullptr, false);
        const std::string* gender = lookup(local, "piece." + type + ".gender");
        const std::string* pat = nullptr;
        if (gender) pat = lookup(local, "owner." + owner + "." + ff + "." + *gender);
        if (!pat) pat = lookup(local, "owner." + owner + "." + ff);
        if (!pat) continue;
        std::vector<Local> locals{{"piece", n}};
        return expand(c, *pat, &locals, false);
    }
    // Last resort: English (logged; the catalog tests keep every language complete).
    LOGW("coach catalog: no form %s of %s %s in %s", f.c_str(), owner.c_str(), type.c_str(), c.lang.c_str());
    if (c.en && c.en != c.L) {
        Ctx e = c;
        e.L = c.en;
        e.lang = "en";
        return renderPiece(e, a, "nom");
    }
    return type;
}

std::string Catalog::renderSquare(Ctx& c, chess::Square sq) const {
    if (sq < 0 || sq > 63) return std::string();
    std::string name = chess::squareName(sq);
    if (!c.spoken) return c.ltrWrap ? kLrm + name + kLrm : name;
    std::vector<Local> locals{{"file", pattern(c, std::string("file.") + name[0], {})},
                              {"rank", pattern(c, std::string("rank.") + name[1], {})}};
    return pattern(c, "square", locals);
}

std::string Catalog::renderMove(Ctx& c, const std::string& san, const std::string& form, chess::Square* prevTo) const {
    if (!c.spoken) {
        std::string w;
        if (form == "letters") {
            const std::string* letters = lookup(c, "move.letters");
            w = sanWithLetters(san, letters ? *letters : std::string("K Q R B N"));
        } else {
            w = figurineSan(san);
        }
        return c.ltrWrap ? kLrm + w + kLrm : w;
    }
    San m = parseSan(san);
    if (!m.valid) {
        LOGW("coach catalog: cannot speak the move '%s'", san.c_str());
        return std::string();
    }
    std::string base;
    auto noun = [&](chess::PieceType t) { return pattern(c, std::string("piece.") + pieceName(t) + ".nom", {}); };
    if (m.castleShort || m.castleLong) {
        base = pattern(c, m.castleShort ? "move.castle_short" : "move.castle_long", {});
    } else {
        bool recapture = m.capture && prevTo && *prevTo == m.to;
        std::string to = renderSquare(c, m.to);
        if (recapture) {
            base = pattern(c, "move.recapture", {{"piece", noun(m.piece)}});
        } else if (m.piece == chess::Pawn) {
            if (m.capture && !m.from.empty())
                base = pattern(c, "move.pawn_capture", {{"file", pattern(c, "file." + m.from.substr(0, 1), {})}, {"to", to}});
            else
                base = pattern(c, "move.pawn", {{"to", to}});
        } else {
            std::string from;
            if (m.from.size() == 2) from = renderSquare(c, chess::parseSquare(m.from));
            else if (!m.from.empty()) from = pattern(c, (m.from[0] >= 'a' ? "file." : "rank.") + m.from, {});
            std::string key = from.empty() ? (m.capture ? "move.piece_capture" : "move.piece")
                                           : (m.capture ? "move.piece_from_capture" : "move.piece_from");
            base = pattern(c, key, {{"piece", noun(m.piece)}, {"from", from}, {"to", to}});
        }
        if (m.promotion != chess::NoPiece)
            base = pattern(c, "move.promotion", {{"move", base}, {"piece", noun(m.promotion)}});
    }
    if (m.mate) base = pattern(c, "move.mate", {{"move", base}});
    else if (m.check) base = pattern(c, "move.check", {{"move", base}});
    if (prevTo) *prevTo = m.to;
    return base;
}

std::string Catalog::renderMoves(Ctx& c, const std::string& line) const {
    std::vector<std::string> tokens;
    size_t p = 0;
    while (p < line.size()) {
        size_t q = line.find(' ', p);
        if (q == std::string::npos) q = line.size();
        std::string t = line.substr(p, q - p);
        p = q + 1;
        // Move numbers ("12.", "12...", "...") are not moves.
        while (!t.empty() && (std::isdigit((unsigned char)t[0]) || t[0] == '.')) {
            size_t k = 0;
            while (k < t.size() && std::isdigit((unsigned char)t[k])) ++k;
            if (k < t.size() && t[k] == '.') {
                while (k < t.size() && t[k] == '.') ++k;
                t = t.substr(k);
            } else {
                break;
            }
        }
        if (!t.empty()) tokens.push_back(t);
    }
    if (!c.spoken) {
        std::string w;
        for (auto& t : tokens) w += (w.empty() ? "" : " ") + figurineSan(t);
        return c.ltrWrap && !w.empty() ? kLrm + w + kLrm : w;
    }
    std::string acc;
    chess::Square prev = chess::NoSquare;
    for (auto& t : tokens) {
        std::string m = renderMove(c, t, "", &prev);
        acc = acc.empty() ? m : pattern(c, "moves.join", {{"a", acc}, {"b", m}});
    }
    return acc;
}

std::string Catalog::numberWords(Ctx& c, int n, const std::string& form) const {
    auto words = [&](const std::string& key) -> const std::string* { return lookup(c, key); };
    if (n < 0) {
        std::vector<Local> l{{"n", numberWords(c, -n, form)}};
        return pattern(c, "number.minus", l);
    }
    std::string k = "number." + std::to_string(n);
    if (!form.empty())
        if (const std::string* v = words(k + "." + form)) return *v;
    if (const std::string* v = words(k)) return *v;
    if (n < 100) {
        int tens = n / 10 * 10, units = n % 10;
        if (words("number." + std::to_string(tens)) && words("number." + std::to_string(units)) && words("number.tens"))
            return pattern(c, "number.tens", {{"tens", numberWords(c, tens, "")}, {"units", numberWords(c, units, form)}});
    } else if (n < 1000) {
        int h = n / 100, rest = n % 100;
        std::string hundreds;
        if (const std::string* v = words("number." + std::to_string(h * 100))) hundreds = *v;
        else if (words("number.hundreds")) hundreds = pattern(c, "number.hundreds", {{"n", numberWords(c, h, "")}});
        if (!hundreds.empty()) {
            if (rest == 0) return hundreds;
            if (words("number.hundreds_rest"))
                return pattern(c, "number.hundreds_rest", {{"hundreds", hundreds}, {"rest", numberWords(c, rest, form)}});
        }
    }
    return std::to_string(n);
}

// A number: digits (written) or words (spoken); with a form naming a count noun ({n:point} ->
// count.point = "{n} point|{n} points") the noun in the plural form the language's rule picks.
std::string Catalog::renderNumber(Ctx& c, int n, const std::string& form) const {
    if (!form.empty()) {
        std::string fromLang;
        if (const std::string* plural = lookup(c, "count." + form, &fromLang)) {
            std::vector<std::string> forms;
            size_t p = 0;
            while (true) {
                size_t q = plural->find('|', p);
                forms.push_back(plural->substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
            size_t idx = size_t(std::max(0, i18n::pluralIndex(fromLang, n)));
            if (idx >= forms.size()) idx = forms.size() - 1;
            Local num;
            num.name = "n";
            num.isNumber = true;
            num.number = n;
            std::vector<Local> locals{num};
            std::string s = expand(c, forms[idx], &locals, false);
            return c.ltrWrap ? kLrm + s + kLrm : s;
        }
    }
    if (c.spoken) return numberWords(c, n, form);
    std::string s = std::to_string(n);
    return c.ltrWrap ? kLrm + s + kLrm : s;
}

// Evaluation from the listener's point of view: "+1.3" / "plus one point three", "mate in 4".
std::string Catalog::renderEval(Ctx& c, const Arg& a) const {
    std::string s;
    if (a.mate != 0) {
        Local num;
        num.name = "n";
        num.isNumber = true;
        num.number = a.mate > 0 ? a.mate : -a.mate;
        s = pattern(c, a.mate > 0 ? "eval.mate_win" : "eval.mate_loss", {num});
    } else {
        int tenths = (std::abs(a.number) + 5) / 10;   // pawns to one decimal
        if (tenths == 0) {
            s = pattern(c, "eval.even", {});
        } else {
            std::string x;
            int whole = tenths / 10, frac = tenths % 10;
            if (c.spoken)
                x = pattern(c, "number.decimal", {{"int", numberWords(c, whole, "")}, {"frac", numberWords(c, frac, "")}});
            else
                x = pattern(c, "number.decimal", {{"int", std::to_string(whole)}, {"frac", std::to_string(frac)}});
            s = pattern(c, a.number > 0 ? "eval.up" : "eval.down", {{"x", x}});
        }
    }
    return c.ltrWrap && !c.spoken && !s.empty() ? kLrm + s + kLrm : s;
}

// Opening names (the openings files): "family:<id>" -> opening.family.<id>[.<form>],
// "variation:<id>" -> opening.variation.<id>[.<form>]; "line:<component>" needs the composing
// resolver (OpeningTexts).
std::string Catalog::renderOpening(Ctx& c, const std::string& ref, const std::string& form) const {
    if (openingResolver_) {
        std::string r = openingResolver_(ref, form, c.lang, c.spoken);
        if (!r.empty()) return r;
    }
    std::string base;
    if (startsWith(ref, "family:")) base = "opening.family." + ref.substr(7);
    else if (startsWith(ref, "variation:")) base = "opening.variation." + ref.substr(10);
    else if (startsWith(ref, "line:")) {
        // Without the resolver only English (and the Chinese subtitles, which keep the English
        // words) can say a lichess component as it is.
        if (c.lang == "en" || startsWith(c.lang, "zh")) return ref.substr(5);
        return std::string();
    } else {
        base = "opening.family." + ref;   // a bare id
    }
    std::string key = (form.empty() || form == "nom") ? base : base + "." + form;
    const std::string* t = lookup(c, key);
    if (!t) t = lookup(c, base);
    if (!t) {
        LOGW("coach catalog: unknown opening %s (%s)", ref.c_str(), c.lang.c_str());
        return std::string();
    }
    return expand(c, *t, nullptr, false);
}

std::string Catalog::renderText(Ctx& c, const std::string& keyOrText, const std::string& form) const {
    std::string key = keyOrText;
    if (!form.empty() && (has(c.lang, key + "." + form) || has(key + "." + form))) key += "." + form;
    if (!has(c.lang, key) && !has(key)) return keyOrText;   // literal text
    int v = pickVariant(key, c.seed);
    std::string k = v == 1 ? key : key + "." + std::to_string(v);
    const std::string* t = lookup(c, k);
    if (!t) t = lookup(c, key);
    return t ? expand(c, *t, nullptr, false) : std::string();
}

namespace {
// A letter or digit of a word ("dix-huit", "l’échec" and "mat." end their words at - ’ and .).
bool isWordChar(char32_t c) {
    if (c < 0x80) return std::isalnum(int(c)) != 0;
    return c >= 0xC0 && c != 0xD7 && c != 0xF7 && !(c >= 0x2000 && c <= 0x206F) && !(c >= 0x3000 && c <= 0x303F);
}
}  // namespace

// The language's respell.* words in a spoken line, whole words in any case; the anchors follow.
void Catalog::respell(const Language& L, Rendered& r) {
    if (L.respellings.empty()) return;
    const std::u32string t = uni::decode(r.text);
    std::u32string out;
    std::vector<std::pair<int, int>> shifts;   // (byte offset in r.text of a respelled word, size change)
    int byteAt = 0;                            // byte offset in r.text of t[i]
    auto bytes = [](const std::u32string& s, size_t from, size_t to) {
        int n = 0;
        for (size_t k = from; k < to; ++k) n += s[k] < 0x80 ? 1 : s[k] < 0x800 ? 2 : s[k] < 0x10000 ? 3 : 4;
        return n;
    };
    for (size_t i = 0; i < t.size();) {
        if (!isWordChar(t[i])) {
            out.push_back(t[i]);
            byteAt += bytes(t, i, i + 1);
            ++i;
            continue;
        }
        size_t end = i;
        while (end < t.size() && isWordChar(t[end])) ++end;
        const Respelling* hit = nullptr;
        for (const Respelling& rs : L.respellings) {
            if (rs.word.size() != end - i) continue;
            bool same = true;
            for (size_t k = 0; k < rs.word.size() && same; ++k) same = uni::toUpper(rs.word[k]) == uni::toUpper(t[i + k]);
            if (!same) continue;
            if (rs.pauseOnly) {
                size_t n = end;
                while (n < t.size() && uni::isSpace(t[n])) ++n;
                if (n < t.size() && isWordChar(t[n])) continue;   // a word follows: no pause
            }
            hit = &rs;
            break;
        }
        const int wordBytes = bytes(t, i, end);
        if (hit && !hit->spoken.empty()) {
            std::u32string s = hit->spoken;
            if (t[i] != hit->word[0] && t[i] == uni::toUpper(hit->word[0])) s[0] = uni::toUpper(s[0]);   // "Mat !"
            out += s;
            shifts.emplace_back(byteAt, bytes(s, 0, s.size()) - wordBytes);
        } else {
            out.append(t, i, end - i);
        }
        byteAt += wordBytes;
        i = end;
    }
    if (shifts.empty()) return;
    r.text = uni::encode(out);
    // An anchor is where a placeholder's rendering starts: it moves by the respellings before it.
    for (Anchor& a : r.anchors) {
        int d = 0;
        for (auto& s : shifts)
            if (s.first < a.offset) d += s.second;
        a.offset += d;
    }
}

int Catalog::Rendered::anchor(const std::string& name) const {
    for (auto& a : anchors)
        if (a.name == name) return a.offset;
    return -1;
}

Catalog::Rendered Catalog::renderWith(const Line& line, const std::string& lang, bool spoken, int variant,
                                      uint32_t seed) const {
    Rendered r;
    std::string eff = spoken ? speechLanguage(lang) : lang;
    if (!language(eff)) eff = "en";
    Ctx c;
    c.L = language(eff);
    c.en = language("en");
    c.lang = eff;
    c.spoken = spoken;
    c.ltrWrap = !spoken && eff == "ar";
    c.line = &line;
    c.seed = seed;
    c.out = &r;
    int n = variants(line.key);
    if (n == 0) {
        LOGW("coach catalog: unknown key %s", line.key.c_str());
        return r;
    }
    r.variant = std::max(1, std::min(variant, n));
    std::string key = r.variant == 1 ? line.key : line.key + "." + std::to_string(r.variant);
    const std::string* t = lookup(c, key);
    if (!t) {
        r.variant = 1;
        t = lookup(c, line.key);
    }
    if (t) r.text = expand(c, *t, nullptr, true);
    if (spoken) respell(*c.L, r);
    return r;
}

Catalog::Rendered Catalog::render(const Line& line, const std::string& lang, bool spoken, uint32_t variantSeed) const {
    return renderWith(line, lang, spoken, pickVariant(line.key, variantSeed), variantSeed);
}

Catalog::Rendered Catalog::renderVariant(const Line& line, const std::string& lang, bool spoken, int variant) const {
    return renderWith(line, lang, spoken, variant, uint32_t(variant));
}

}  // namespace coach
