#include "unicode.h"
#include <algorithm>

namespace uni {

// ---- UTF-8 ------------------------------------------------------------------------------------------
std::u32string decode(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        auto cont = [&](size_t k) -> int {
            if (i + k >= s.size()) return -1;
            unsigned char b = static_cast<unsigned char>(s[i + k]);
            return (b & 0xC0) == 0x80 ? (b & 0x3F) : -1;
        };
        char32_t cp = 0xFFFD;
        size_t n = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            int b1 = cont(1);
            if (b1 >= 0 && c >= 0xC2) cp = (char32_t(c & 0x1F) << 6) | char32_t(b1), n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            int b1 = cont(1), b2 = cont(2);
            if (b1 >= 0 && b2 >= 0) {
                char32_t v = (char32_t(c & 0x0F) << 12) | (char32_t(b1) << 6) | char32_t(b2);
                if (v >= 0x800 && (v < 0xD800 || v > 0xDFFF)) cp = v, n = 3;
            }
        } else if ((c & 0xF8) == 0xF0) {
            int b1 = cont(1), b2 = cont(2), b3 = cont(3);
            if (b1 >= 0 && b2 >= 0 && b3 >= 0) {
                char32_t v = (char32_t(c & 0x07) << 18) | (char32_t(b1) << 12) | (char32_t(b2) << 6) | char32_t(b3);
                if (v >= 0x10000 && v <= 0x10FFFF) cp = v, n = 4;
            }
        }
        out += cp;
        i += n;
    }
    return out;
}

void append(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

std::string encode(const std::u32string& t) {
    std::string out;
    out.reserve(t.size());
    for (char32_t c : t) append(out, c);
    return out;
}

size_t length(const std::string& s) {
    size_t n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

// ---- Scripts ----------------------------------------------------------------------------------------
bool isArabic(char32_t c) {
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) || (c >= 0x08A0 && c <= 0x08FF) ||
           (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}
bool isRtl(char32_t c) {
    BidiClass b = bidiClass(c);
    return b == BidiClass::R || b == BidiClass::AL;
}
bool isHan(char32_t c) {
    return (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0x20000 && c <= 0x3134F) || c == 0x3005 || c == 0x3007;
}
bool isKana(char32_t c) {
    return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF66 && c <= 0xFF9F);
}
bool isCjkPunct(char32_t c) {
    return (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFF65) || (c >= 0xFFE0 && c <= 0xFFEF);
}
bool isCjk(char32_t c) { return isHan(c) || isKana(c) || (isCjkPunct(c) && c != 0x3000); }
bool isMark(char32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) || (c >= 0x0610 && c <= 0x061A) ||
           (c >= 0x064B && c <= 0x065F) || c == 0x0670 || (c >= 0x06D6 && c <= 0x06DC) || (c >= 0x06DF && c <= 0x06E4) ||
           (c >= 0x06E7 && c <= 0x06E8) || (c >= 0x06EA && c <= 0x06ED) || (c >= 0x08D3 && c <= 0x08FF && c != 0x08E2) ||
           (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
           (c >= 0x3099 && c <= 0x309A) || (c >= 0xFE20 && c <= 0xFE2F);
}
bool isSpace(char32_t c) {
    return c == ' ' || c == '\t' || c == 0xA0 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000;
}
bool containsRtl(const std::u32string& t) {
    for (char32_t c : t)
        if (isRtl(c)) return true;
    return false;
}

// ---- Arabic joining -------------------------------------------------------------------------------------
namespace {
// Letter -> presentation forms {isolated, final, initial, medial} (Unicode decompositions of
// U+FB50..U+FDFF and U+FE70..U+FEFF).
struct ArabicForms { char32_t base, isol, fina, init, medi; };
const ArabicForms kForms[] = {
    {0x0621, 0xFE80, 0, 0, 0},           {0x0622, 0xFE81, 0xFE82, 0, 0},      {0x0623, 0xFE83, 0xFE84, 0, 0},
    {0x0624, 0xFE85, 0xFE86, 0, 0},      {0x0625, 0xFE87, 0xFE88, 0, 0},      {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},
    {0x0627, 0xFE8D, 0xFE8E, 0, 0},      {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92}, {0x0629, 0xFE93, 0xFE94, 0, 0},
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98}, {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, {0x062F, 0xFEA9, 0xFEAA, 0, 0},
    {0x0630, 0xFEAB, 0xFEAC, 0, 0},      {0x0631, 0xFEAD, 0xFEAE, 0, 0},      {0x0632, 0xFEAF, 0xFEB0, 0, 0},
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC},
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8},
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC}, {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0}, {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4},
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8}, {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}, {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0},
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}, {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC},
    {0x0648, 0xFEED, 0xFEEE, 0, 0},      {0x0649, 0xFEEF, 0xFEF0, 0xFBE8, 0xFBE9}, {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4},
    {0x0671, 0xFB50, 0xFB51, 0, 0},      {0x0679, 0xFB66, 0xFB67, 0xFB68, 0xFB69}, {0x067A, 0xFB5E, 0xFB5F, 0xFB60, 0xFB61},
    {0x067B, 0xFB52, 0xFB53, 0xFB54, 0xFB55}, {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59}, {0x067F, 0xFB62, 0xFB63, 0xFB64, 0xFB65},
    {0x0680, 0xFB5A, 0xFB5B, 0xFB5C, 0xFB5D}, {0x0683, 0xFB76, 0xFB77, 0xFB78, 0xFB79}, {0x0684, 0xFB72, 0xFB73, 0xFB74, 0xFB75},
    {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D}, {0x0687, 0xFB7E, 0xFB7F, 0xFB80, 0xFB81}, {0x0688, 0xFB88, 0xFB89, 0, 0},
    {0x068C, 0xFB84, 0xFB85, 0, 0},      {0x068D, 0xFB82, 0xFB83, 0, 0},      {0x068E, 0xFB86, 0xFB87, 0, 0},
    {0x0691, 0xFB8C, 0xFB8D, 0, 0},      {0x0698, 0xFB8A, 0xFB8B, 0, 0},      {0x06A4, 0xFB6A, 0xFB6B, 0xFB6C, 0xFB6D},
    {0x06A6, 0xFB6E, 0xFB6F, 0xFB70, 0xFB71}, {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91}, {0x06AD, 0xFBD3, 0xFBD4, 0xFBD5, 0xFBD6},
    {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95}, {0x06B1, 0xFB9A, 0xFB9B, 0xFB9C, 0xFB9D}, {0x06B3, 0xFB96, 0xFB97, 0xFB98, 0xFB99},
    {0x06BA, 0xFB9E, 0xFB9F, 0, 0},      {0x06BB, 0xFBA0, 0xFBA1, 0xFBA2, 0xFBA3}, {0x06BE, 0xFBAA, 0xFBAB, 0xFBAC, 0xFBAD},
    {0x06C0, 0xFBA4, 0xFBA5, 0, 0},      {0x06C1, 0xFBA6, 0xFBA7, 0xFBA8, 0xFBA9}, {0x06C5, 0xFBE0, 0xFBE1, 0, 0},
    {0x06C6, 0xFBD9, 0xFBDA, 0, 0},      {0x06C7, 0xFBD7, 0xFBD8, 0, 0},      {0x06C8, 0xFBDB, 0xFBDC, 0, 0},
    {0x06C9, 0xFBE2, 0xFBE3, 0, 0},      {0x06CB, 0xFBDE, 0xFBDF, 0, 0},      {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF},
    {0x06D0, 0xFBE4, 0xFBE5, 0xFBE6, 0xFBE7}, {0x06D2, 0xFBAE, 0xFBAF, 0, 0},      {0x06D3, 0xFBB0, 0xFBB1, 0, 0},
};

const ArabicForms* findForms(char32_t cp) {
    if (cp < 0x0621 || cp > 0x06D3) return nullptr;
    const ArabicForms* b = std::begin(kForms);
    const ArabicForms* e = std::end(kForms);
    const ArabicForms* it = std::lower_bound(b, e, cp, [](const ArabicForms& f, char32_t c) { return f.base < c; });
    return it != e && it->base == cp ? it : nullptr;
}

bool joinsNextCapable(Joining j) { return j == Joining::D || j == Joining::C; }
bool joinsPrevCapable(Joining j) { return j == Joining::D || j == Joining::R || j == Joining::C; }
bool isAlefVariant(char32_t c) { return c == 0x0622 || c == 0x0623 || c == 0x0625 || c == 0x0627; }
}  // namespace

Joining joiningType(char32_t cp) {
    if (cp == 0x0640 || cp == 0x200D) return Joining::C;  // tatweel, zero width joiner
    if (isMark(cp)) return Joining::T;
    if (const ArabicForms* f = findForms(cp)) return f->init ? Joining::D : f->fina ? Joining::R : Joining::U;
    return Joining::U;
}

std::vector<Form> joiningForms(const std::u32string& t) {
    const int n = int(t.size());
    std::vector<Joining> jt(size_t(n), Joining::U);
    for (int i = 0; i < n; ++i) jt[size_t(i)] = joiningType(t[size_t(i)]);
    std::vector<Form> forms(size_t(n), Form::Isolated);
    for (int i = 0; i < n; ++i) {
        Joining j = jt[size_t(i)];
        if (j == Joining::T || j == Joining::U) continue;
        int p = i - 1, q = i + 1;
        while (p >= 0 && jt[size_t(p)] == Joining::T) --p;
        while (q < n && jt[size_t(q)] == Joining::T) ++q;
        bool prev = p >= 0 && joinsNextCapable(jt[size_t(p)]) && joinsPrevCapable(j);
        bool next = q < n && joinsPrevCapable(jt[size_t(q)]) && joinsNextCapable(j);
        forms[size_t(i)] = prev && next ? Form::Medial : prev ? Form::Final : next ? Form::Initial : Form::Isolated;
    }
    return forms;
}

char32_t presentationForm(char32_t cp, Form form) {
    const ArabicForms* f = findForms(cp);
    if (!f) return 0;
    switch (form) {
        case Form::Final: return f->fina;
        case Form::Initial: return f->init;
        case Form::Medial: return f->medi;
        default: return f->isol;
    }
}

char32_t lamAlefLigature(char32_t alef, bool joined) {
    switch (alef) {
        case 0x0622: return joined ? 0xFEF6 : 0xFEF5;
        case 0x0623: return joined ? 0xFEF8 : 0xFEF7;
        case 0x0625: return joined ? 0xFEFA : 0xFEF9;
        case 0x0627: return joined ? 0xFEFC : 0xFEFB;
        default: return 0;
    }
}

Shaped shapeArabic(const std::u32string& t) {
    Shaped out;
    const int n = int(t.size());
    bool any = false;
    for (char32_t c : t)
        if (findForms(c)) any = true;
    if (!any) {
        out.text = t;
        out.source.resize(size_t(n));
        for (int i = 0; i < n; ++i) out.source[size_t(i)] = i;
        return out;
    }
    std::vector<Form> forms = joiningForms(t);
    std::vector<bool> skip(size_t(n), false);
    out.text.reserve(size_t(n));
    out.source.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        if (skip[size_t(i)]) continue;
        char32_t c = t[size_t(i)];
        Form f = forms[size_t(i)];
        if (c == 0x0644 && (f == Form::Initial || f == Form::Medial)) {
            int q = i + 1;
            while (q < n && joiningType(t[size_t(q)]) == Joining::T) ++q;
            if (q < n && isAlefVariant(t[size_t(q)])) {
                out.text += lamAlefLigature(t[size_t(q)], f == Form::Medial);
                out.source.push_back(i);
                skip[size_t(q)] = true;  // marks between lam and alef stay, after the ligature
                continue;
            }
        }
        char32_t pf = presentationForm(c, f);
        if (!pf && f != Form::Isolated) pf = presentationForm(c, Form::Isolated);
        out.text += pf ? pf : c;
        out.source.push_back(i);
    }
    return out;
}

// ---- Bidi -----------------------------------------------------------------------------------------------
BidiClass bidiClass(char32_t c) {
    using B = BidiClass;
    if (c < 0x80) {
        if (c >= '0' && c <= '9') return B::EN;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return B::L;
        switch (c) {
            case ' ': return B::WS;
            case '\t': case 0x0B: case 0x1F: return B::S;
            case '\n': case '\r': case 0x1C: case 0x1D: case 0x1E: return B::B;
            case '+': case '-': return B::ES;
            case '#': case '$': case '%': return B::ET;
            case ',': case '.': case '/': case ':': return B::CS;
            default: return c < 0x20 || c == 0x7F ? B::BN : B::ON;
        }
    }
    if (c == 0xA0) return B::CS;
    if (c == 0xAD) return B::BN;
    if (c == 0xB2 || c == 0xB3 || c == 0xB9) return B::EN;
    if ((c >= 0xA2 && c <= 0xA5) || c == 0xB0 || c == 0xB1) return B::ET;
    if (c == 0xAA || c == 0xB5 || c == 0xBA || (c >= 0xC0 && c <= 0x2B8 && c != 0xD7 && c != 0xF7)) return B::L;
    if (c < 0x300) return B::ON;
    if (isMark(c)) return B::NSM;
    if (c < 0x590) return B::L;  // Greek, Cyrillic, Armenian
    if (c >= 0x590 && c <= 0x5FF) return B::R;
    if (c >= 0x600 && c <= 0x6FF) {
        if (c >= 0x660 && c <= 0x669) return B::AN;
        if (c == 0x66B || c == 0x66C || (c >= 0x600 && c <= 0x605) || c == 0x6DD) return B::AN;
        if (c >= 0x6F0 && c <= 0x6F9) return B::EN;
        if (c == 0x60C) return B::CS;
        if (c == 0x609 || c == 0x60A || c == 0x66A) return B::ET;
        if (c == 0x60E || c == 0x60F || c == 0x6DE || c == 0x6E9) return B::ON;
        return B::AL;
    }
    if ((c >= 0x700 && c <= 0x7BF) || (c >= 0x860 && c <= 0x8FF) || (c >= 0x750 && c <= 0x77F)) return B::AL;
    if (c >= 0x7C0 && c <= 0x85F) return B::R;
    if (c >= 0x2000 && c <= 0x200A) return B::WS;
    if (c >= 0x200B && c <= 0x200D) return B::BN;
    if (c == 0x200E) return B::L;
    if (c == 0x200F) return B::R;
    if (c == 0x2028) return B::WS;
    if (c == 0x2029) return B::B;
    if (c >= 0x202A && c <= 0x202E) return B::BN;  // explicit embeddings: ignored
    if (c == 0x202F) return B::CS;
    if (c >= 0x2030 && c <= 0x2034) return B::ET;
    if (c == 0x2044) return B::CS;
    if (c == 0x205F) return B::WS;
    if (c >= 0x2060 && c <= 0x206F) return B::BN;
    if (c == 0x2070 || (c >= 0x2074 && c <= 0x2079) || (c >= 0x2080 && c <= 0x2089)) return B::EN;
    if (c == 0x207A || c == 0x207B || c == 0x208A || c == 0x208B || c == 0x2212) return B::ES;
    if (c >= 0x20A0 && c <= 0x20CF) return B::ET;
    if (c >= 0x2000 && c <= 0x2BFF) return B::ON;  // punctuation, symbols, arrows, math, box drawing
    if (c == 0x3000) return B::WS;
    if (c >= 0x3001 && c <= 0x3004) return B::ON;
    if (c >= 0x3008 && c <= 0x3020) return B::ON;
    if (c == 0x3030 || c == 0x303D || c == 0x30A0 || c == 0x30FB) return B::ON;
    if (c >= 0xFB1D && c <= 0xFB4F) return B::R;
    if (c >= 0xFB50 && c <= 0xFDFF) return c == 0xFD3E || c == 0xFD3F ? B::ON : B::AL;
    if (c >= 0xFE00 && c <= 0xFE0F) return B::NSM;
    if (c == 0xFE50 || c == 0xFE52 || c == 0xFE55) return B::CS;
    if (c >= 0xFE70 && c <= 0xFEFE) return B::AL;
    if (c == 0xFEFF) return B::BN;
    if (c >= 0xFF10 && c <= 0xFF19) return B::EN;
    if (c == 0xFF0B || c == 0xFF0D) return B::ES;
    if (c == 0xFF03 || c == 0xFF04 || c == 0xFF05 || c == 0xFFE0 || c == 0xFFE1 || c == 0xFFE5 || c == 0xFFE6) return B::ET;
    if (c == 0xFF0C || c == 0xFF0E || c == 0xFF0F || c == 0xFF1A) return B::CS;
    if ((c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) ||
        (c >= 0xFF5B && c <= 0xFF65))
        return B::ON;
    if (c >= 0x10800 && c <= 0x10FFF) return B::R;
    if (c >= 0x1E800 && c <= 0x1EFFF) return (c >= 0x1EE00 && c <= 0x1EEFF) ? B::AL : B::R;
    return B::L;
}

int paragraphLevel(const std::u32string& t, int fallback) {
    for (char32_t c : t) {
        BidiClass b = bidiClass(c);
        if (b == BidiClass::L) return 0;
        if (b == BidiClass::R || b == BidiClass::AL) return 1;
    }
    return fallback;
}

std::vector<int> resolveLevels(const std::u32string& t, int base) {
    using B = BidiClass;
    const int n = int(t.size());
    base &= 1;
    const B sor = base ? B::R : B::L;
    std::vector<B> ty(t.size(), B::ON);
    for (int i = 0; i < n; ++i) ty[size_t(i)] = bidiClass(t[size_t(i)]);
    // X9 (simplified): boundary neutrals take the class of the previous character so they never
    // split a run; at the start they are neutral.
    for (int i = 0; i < n; ++i)
        if (ty[size_t(i)] == B::BN) ty[size_t(i)] = i > 0 ? ty[size_t(i - 1)] : B::ON;
    // W1: non-spacing marks take the class of the previous character.
    for (int i = 0; i < n; ++i)
        if (ty[size_t(i)] == B::NSM) ty[size_t(i)] = i > 0 ? ty[size_t(i - 1)] : sor;
    // W2: European digits after Arabic letters are Arabic numbers. W3: AL -> R.
    B lastStrong = sor;
    for (int i = 0; i < n; ++i) {
        B& c = ty[size_t(i)];
        if (c == B::L || c == B::R || c == B::AL) lastStrong = c;
        else if (c == B::EN && lastStrong == B::AL) c = B::AN;
    }
    for (auto& c : ty)
        if (c == B::AL) c = B::R;
    // W4: a single separator between two numbers of the same kind joins them.
    for (int i = 1; i + 1 < n; ++i) {
        B p = ty[size_t(i - 1)], c = ty[size_t(i)], q = ty[size_t(i + 1)];
        if (c == B::ES && p == B::EN && q == B::EN) ty[size_t(i)] = B::EN;
        else if (c == B::CS && p == B::EN && q == B::EN) ty[size_t(i)] = B::EN;
        else if (c == B::CS && p == B::AN && q == B::AN) ty[size_t(i)] = B::AN;
    }
    // W5: terminators next to European numbers become European numbers.
    for (int i = 0; i < n; ++i) {
        if (ty[size_t(i)] != B::ET) continue;
        int j = i;
        while (j < n && ty[size_t(j)] == B::ET) ++j;
        bool adj = (i > 0 && ty[size_t(i - 1)] == B::EN) || (j < n && ty[size_t(j)] == B::EN);
        if (adj)
            for (int k = i; k < j; ++k) ty[size_t(k)] = B::EN;
        i = j - 1;
    }
    // W6: remaining separators and terminators are neutral.
    for (auto& c : ty)
        if (c == B::ES || c == B::ET || c == B::CS) c = B::ON;
    // W7: European numbers after left-to-right text are left-to-right.
    lastStrong = sor;
    for (int i = 0; i < n; ++i) {
        B& c = ty[size_t(i)];
        if (c == B::L || c == B::R) lastStrong = c;
        else if (c == B::EN && lastStrong == B::L) c = B::L;
    }
    // N1/N2: neutral sequences take the direction of their surroundings when both sides agree
    // (numbers count as right-to-left), else the embedding direction.
    auto isNeutral = [](B c) { return c == B::ON || c == B::WS || c == B::S || c == B::B || c == B::BN; };
    auto strongDir = [](B c) { return c == B::L ? B::L : B::R; };  // R, EN, AN -> R
    for (int i = 0; i < n; ++i) {
        if (!isNeutral(ty[size_t(i)])) continue;
        int j = i;
        while (j < n && isNeutral(ty[size_t(j)])) ++j;
        B before = i > 0 ? strongDir(ty[size_t(i - 1)]) : sor;
        B after = j < n ? strongDir(ty[size_t(j)]) : sor;
        B res = before == after ? before : sor;
        for (int k = i; k < j; ++k) ty[size_t(k)] = res;
        i = j - 1;
    }
    // I1/I2: implicit levels.
    std::vector<int> lv(size_t(n), base);
    for (int i = 0; i < n; ++i) {
        B c = ty[size_t(i)];
        if (base == 0) {
            if (c == B::R) lv[size_t(i)] = 1;
            else if (c == B::AN || c == B::EN) lv[size_t(i)] = 2;
        } else {
            if (c == B::L || c == B::AN || c == B::EN) lv[size_t(i)] = 2;
        }
    }
    // L1: separators and trailing whitespace go back to the paragraph level.
    for (int i = n - 1; i >= 0; --i) {
        B orig = bidiClass(t[size_t(i)]);
        if (orig == B::WS || orig == B::S || orig == B::B || orig == B::BN) lv[size_t(i)] = base;
        else break;
    }
    for (int i = 0; i < n; ++i) {
        B orig = bidiClass(t[size_t(i)]);
        if (orig == B::S || orig == B::B) {
            lv[size_t(i)] = base;
            for (int k = i - 1; k >= 0; --k) {
                B o = bidiClass(t[size_t(k)]);
                if (o == B::WS || o == B::BN) lv[size_t(k)] = base;
                else break;
            }
        }
    }
    return lv;
}

std::vector<int> visualOrder(const std::vector<int>& levels) {
    const int n = int(levels.size());
    std::vector<int> order(levels.size(), 0);
    for (int i = 0; i < n; ++i) order[size_t(i)] = i;
    if (n == 0) return order;
    int maxLevel = 0, minOdd = 1 << 20;
    for (int l : levels) {
        maxLevel = std::max(maxLevel, l);
        if (l & 1) minOdd = std::min(minOdd, l);
    }
    if (minOdd > maxLevel) return order;
    // L2: from the highest level down to the lowest odd level, reverse every run at or above it.
    std::vector<int> lv = levels;
    for (int level = maxLevel; level >= minOdd; --level) {
        for (int i = 0; i < n; ++i) {
            if (lv[size_t(i)] < level) continue;
            int j = i;
            while (j < n && lv[size_t(j)] >= level) ++j;
            std::reverse(order.begin() + i, order.begin() + j);
            std::reverse(lv.begin() + i, lv.begin() + j);
            i = j - 1;
        }
    }
    return order;
}

char32_t mirror(char32_t c) {
    switch (c) {
        case '(': return ')'; case ')': return '(';
        case '[': return ']'; case ']': return '[';
        case '{': return '}'; case '}': return '{';
        case '<': return '>'; case '>': return '<';
        case 0xAB: return 0xBB; case 0xBB: return 0xAB;
        case 0x2039: return 0x203A; case 0x203A: return 0x2039;
        case 0x2264: return 0x2265; case 0x2265: return 0x2264;
        case 0x3008: return 0x3009; case 0x3009: return 0x3008;
        case 0x300A: return 0x300B; case 0x300B: return 0x300A;
        case 0x300C: return 0x300D; case 0x300D: return 0x300C;
        case 0x300E: return 0x300F; case 0x300F: return 0x300E;
        case 0x3010: return 0x3011; case 0x3011: return 0x3010;
        case 0xFF08: return 0xFF09; case 0xFF09: return 0xFF08;
        case 0xFF3B: return 0xFF3D; case 0xFF3D: return 0xFF3B;
        default: return c;
    }
}

// ---- Line breaking ---------------------------------------------------------------------------------------
namespace {
bool noLineStart(char32_t c) {
    static const char32_t k[] = {
        ',', '.', ':', ';', '!', '?', ')', ']', '}', '%', 0x2019, 0x201D, 0xBB, 0x203A, 0x2026, 0x2025, 0xB0,
        0x3001, 0x3002, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF01, 0xFF09, 0x300D, 0x300F, 0x3011, 0x3015,
        0x3009, 0x300B, 0x3017, 0x3019, 0x301B, 0xFF5D, 0xFF3D, 0xFF05, 0x30FC, 0x3005, 0x309D, 0x309E, 0x30FD,
        0x30FE, 0x30FB, 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x308E, 0x3095,
        0x3096, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3, 0x30E3, 0x30E5, 0x30E7, 0x30EE, 0x30F5, 0x30F6,
        0x2103, 0xFF64, 0xFF61, 0x301C, 0x2014};
    for (char32_t x : k)
        if (x == c) return true;
    return false;
}
bool noLineEnd(char32_t c) {
    static const char32_t k[] = {'(', '[', '{', 0x2018, 0x201C, 0xAB, 0x2039, 0xFF08, 0x300C, 0x300E, 0x3010,
                                 0x3014, 0x3008, 0x300A, 0x3016, 0x3018, 0x301A, 0xFF5B, 0xFF3B, 0xFF04, 0xFFE5};
    for (char32_t x : k)
        if (x == c) return true;
    return false;
}
}  // namespace

bool breakBetween(char32_t a, char32_t b) {
    if (isSpace(a) || isSpace(b)) return false;
    if (!isCjk(a) && !isCjk(b)) return false;
    if (noLineStart(b) || noLineEnd(a)) return false;
    return true;
}

float trackingScale(char32_t a, char32_t b) { return isCjk(a) && isCjk(b) ? 0.25f : 1.0f; }

// ---- Case ---------------------------------------------------------------------------------------------
char32_t toUpper(char32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x131) return 'I';
        if (c == 0x17F) return 'S';
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        if (c == 0x138 || c == 0x149 || c == 0x130 || c == 0x178) return c;
        return (c & 1) ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9) return c == 0x3C2 ? 0x3A3 : c - 32;
    if (c == 0x3AC) return 0x386;
    if (c >= 0x3AD && c <= 0x3AF) return c - 37;
    if (c == 0x3CC) return 0x38C;
    if (c == 0x3CD || c == 0x3CE) return c - 63;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return (c & 1) ? c - 1 : c;
    if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c : c - 1;
    if (c == 0x4CF) return 0x4C0;
    return c;
}

bool isLower(char32_t c) { return c == 0xDF || toUpper(c) != c; }

std::string toUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : decode(s)) {
        if (c == 0xDF) out += "SS";
        else append(out, toUpper(c));
    }
    return out;
}

}  // namespace uni
