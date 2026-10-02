// PGN reader and writer (see pgn.h). The reader is a small lexer (tokens with their line and column)
// under a parser that either replays the moves (read) or only counts them (scan); both share the
// game boundaries, the caps and the error recovery, so a listing and a full read always agree on
// where each game of a file is.
#include "chess/pgn.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace chess {
namespace pgn {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!isDigit(c)) return false;
    return true;
}
std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// ---- Text encoding ------------------------------------------------------------------------------
// Files from older programs (ChessBase among them) are often Windows-1252 rather than UTF-8. A
// string that is not valid UTF-8 is read as Windows-1252, so names keep their accents and the UI
// never receives broken UTF-8.
bool validUtf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + size_t(len) > n) return false;
        uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < len; ++k) {
            unsigned char d = (unsigned char)s[i + size_t(k)];
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        static const uint32_t minCp[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < minCp[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += size_t(len);
    }
    return true;
}
void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}
std::string toUtf8(const std::string& s) {
    if (validUtf8(s)) return s;
    // Windows-1252 0x80..0x9F (0 = undefined there: U+FFFD).
    static const uint16_t k80[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                     0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                     0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};
    std::string out;
    for (char ch : s) {
        unsigned char c = (unsigned char)ch;
        if (c < 0x80) out += ch;
        else if (c < 0xA0) appendUtf8(out, k80[c - 0x80] ? k80[c - 0x80] : 0xFFFD);
        else appendUtf8(out, c);
    }
    return out;
}
// Cut at 'max' bytes without splitting a UTF-8 sequence.
void cutUtf8(std::string& s, size_t max) {
    if (s.size() <= max) return;
    size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
    s.resize(n);
}

// ---- Lexer --------------------------------------------------------------------------------------
enum class Tok { End, TagPair, Comment, Open, Close, Nag, Symbol, Star, Bad };

struct Token {
    Tok kind = Tok::End;
    std::string text;           // symbol, comment text, tag name
    std::string value;          // tag value
    int nag = 0;
    bool tagLike = false;       // Bad: a broken tag pair (it belongs to the tag section)
    size_t begin = 0, end = 0;
    int line = 1, column = 1;
};

class Lexer {
public:
    Lexer(const std::string& s, const Limits& lim, Origin o) : s_(s), lim_(lim), line_(o.line), col_(o.column) {
        if (s_.size() >= 3 && (unsigned char)s_[0] == 0xEF && (unsigned char)s_[1] == 0xBB && (unsigned char)s_[2] == 0xBF)
            p_ = 3;  // UTF-8 byte order mark
    }

    Token next() {
        for (;;) {
            while (p_ < s_.size() && isSpace(s_[p_])) advance();
            Token t;
            t.begin = p_;
            t.line = line_;
            t.column = col_;
            if (p_ >= s_.size()) {
                t.kind = Tok::End;
                t.end = p_;
                return t;
            }
            const char c = s_[p_];
            if (c == '%' && col_ == 1) {  // escape line (export comments of other programs)
                skipLine();
                continue;
            }
            if (c == '<') {  // reserved for future expansion: skipped up to its '>' on the same line
                size_t q = p_ + 1;
                while (q < s_.size() && s_[q] != '>' && !lineBreak(q)) ++q;
                if (q < s_.size() && s_[q] == '>') {
                    while (p_ <= q) advance();
                    continue;
                }
                // Never further: a stray '<' must not swallow the games after it.
                advance();
                bad(t, "'<' without '>' on its line");
                t.end = p_;
                return t;
            }
            if (c == '.') {  // periods of move numbers
                advance();
                continue;
            }
            switch (c) {
            case '[': tagPair(t); break;
            case '{': braceComment(t); break;
            case ';': lineComment(t); break;
            case '(': advance(); t.kind = Tok::Open; break;
            case ')': advance(); t.kind = Tok::Close; break;
            case '*': advance(); t.kind = Tok::Star; break;
            case '$': nag(t); break;
            case '!':
            case '?': glyph(t); break;
            default:
                if (c == '+' || c == '=' || (c == '-' && peek(1) != '-')) {
                    // Text evaluations of some exports ("+-", "=", "-/+"): ignored.
                    while (p_ < s_.size() && std::strchr("+-=/", s_[p_])) advance();
                    continue;
                }
                if (isAlpha(c) || isDigit(c) || c == '-' || (unsigned char)c >= 0x80) {
                    symbol(t);
                    if (t.kind == Tok::End) continue;  // a lone "e.p."
                } else {
                    const size_t from = p_;
                    advanceChar();
                    if ((unsigned char)c < 0x20 || c == 0x7F) bad(t, "unexpected control character");
                    else bad(t, "unexpected character '" + s_.substr(from, p_ - from) + "'");
                }
            }
            t.end = p_;
            return t;
        }
    }

private:
    const std::string& s_;
    const Limits& lim_;
    size_t p_ = 0;
    int line_ = 1, col_ = 1;

    char peek(size_t k) const { return p_ + k < s_.size() ? s_[p_ + k] : '\0'; }
    // A line ends at '\n', and at a '\r' not followed by '\n' (files with CR line ends).
    bool lineBreak(size_t q) const { return s_[q] == '\n' || (s_[q] == '\r' && (q + 1 >= s_.size() || s_[q + 1] != '\n')); }
    void advance() {
        const bool newLine = lineBreak(p_);
        const unsigned char c = (unsigned char)s_[p_++];
        if (newLine) {
            ++line_;
            col_ = 1;
        } else if ((c & 0xC0) != 0x80) {
            ++col_;
        }
    }
    void advanceChar() {  // one whole UTF-8 sequence
        advance();
        while (p_ < s_.size() && ((unsigned char)s_[p_] & 0xC0) == 0x80) advance();
    }
    void skipLine() {
        while (p_ < s_.size() && !lineBreak(p_)) advance();
    }
    void skipSpacesInLine() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t')) advance();
    }
    static void bad(Token& t, const std::string& why) {
        t.kind = Tok::Bad;
        t.text = why;
    }
    // At the start of a line: does it open a tag pair ('[', a name, a quote)? A brace comment
    // running into such a line was never closed.
    bool tagLineAhead() const {
        size_t q = p_;
        while (q < s_.size() && (s_[q] == ' ' || s_[q] == '\t')) ++q;
        if (q >= s_.size() || s_[q] != '[') return false;
        ++q;
        size_t n = 0;
        while (q < s_.size() && (isAlpha(s_[q]) || isDigit(s_[q]) || s_[q] == '_')) ++q, ++n;
        while (q < s_.size() && (s_[q] == ' ' || s_[q] == '\t')) ++q;
        return n > 0 && q < s_.size() && s_[q] == '"';
    }
    // After a quote at q: only spaces then ']' on this line (the quote closes the value).
    bool closesTag(size_t q) const {
        ++q;
        while (q < s_.size() && (s_[q] == ' ' || s_[q] == '\t')) ++q;
        return q < s_.size() && s_[q] == ']';
    }

    void tagPair(Token& t) {
        advance();  // '['
        skipSpacesInLine();
        std::string name;
        while (p_ < s_.size() && (isAlpha(s_[p_]) || isDigit(s_[p_]) || s_[p_] == '_')) {
            name += s_[p_];
            advance();
        }
        t.tagLike = true;
        if (name.empty()) {
            skipLine();
            return bad(t, "tag name expected after '['");
        }
        if (name.size() > lim_.maxTagName) {
            skipLine();
            return bad(t, "tag name too long");
        }
        skipSpacesInLine();
        if (peek(0) != '"') {
            skipLine();
            return bad(t, "quoted value expected in tag " + name);
        }
        advance();
        // An unescaped quote inside the value (some writers) when a later quote on the line closes
        // the tag: the last quote of the line followed by ']' is found once, so that a line full of
        // quotes costs one pass, not one per quote.
        size_t lastClose = std::string::npos;
        for (size_t q = p_; q < s_.size() && !lineBreak(q); ++q)
            if (s_[q] == '"' && closesTag(q)) lastClose = q;
        std::string value;
        bool tooLong = false;
        for (;;) {
            if (p_ >= s_.size() || lineBreak(p_)) return bad(t, "unterminated value of tag " + name);
            char c = s_[p_];
            if (c == '\\' && (peek(1) == '"' || peek(1) == '\\')) {
                advance();
                c = s_[p_];
            } else if (c == '"') {
                const bool closing = lastClose == std::string::npos || lastClose <= p_ || closesTag(p_);
                if (closing) {
                    advance();
                    break;
                }
            }
            if (value.size() < lim_.maxTagValue) value += c;
            else tooLong = true;
            advance();
        }
        skipSpacesInLine();
        if (peek(0) != ']') {
            skipLine();
            return bad(t, "']' expected after the value of tag " + name);
        }
        advance();
        if (tooLong) return bad(t, "value of tag " + name + " too long");
        t.kind = Tok::TagPair;
        t.text = name;
        t.value = toUtf8(value);
    }

    void braceComment(Token& t) {
        advance();  // '{'
        std::string text;
        for (;;) {
            if (p_ >= s_.size()) return bad(t, "unterminated comment");
            const char c = s_[p_];
            if (c == '}') {
                advance();
                break;
            }
            const bool newLine = lineBreak(p_);
            advance();
            if (newLine && tagLineAhead()) return bad(t, "unterminated comment");
            if (text.size() < lim_.maxComment) text += c;
        }
        t.kind = Tok::Comment;
        t.text = std::move(text);
    }

    void lineComment(Token& t) {
        advance();  // ';'
        std::string text;
        while (p_ < s_.size() && !lineBreak(p_)) {
            if (text.size() < lim_.maxComment) text += s_[p_];
            advance();
        }
        t.kind = Tok::Comment;
        t.text = std::move(text);
    }

    void nag(Token& t) {
        advance();  // '$'
        int n = 0, digits = 0;
        while (p_ < s_.size() && isDigit(s_[p_])) {
            if (digits < 4) n = n * 10 + (s_[p_] - '0');
            ++digits;
            advance();
        }
        if (digits == 0 || n > 255) return bad(t, "malformed NAG");
        t.kind = Tok::Nag;
        t.nag = n;
    }

    // Suffix annotations: ! ? !! ?? !? ?! = $1..$6 (any other run is ignored: NAG 0).
    void glyph(Token& t) {
        std::string g;
        while (p_ < s_.size() && (s_[p_] == '!' || s_[p_] == '?')) {
            g += s_[p_];
            advance();
        }
        static const char* glyphs[] = {"!", "?", "!!", "??", "!?", "?!"};
        t.kind = Tok::Nag;
        t.nag = 0;
        for (int i = 0; i < 6; ++i)
            if (g == glyphs[i]) t.nag = i + 1;
    }

    void symbol(Token& t) {
        std::string sym;
        bool tooLong = false;
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (s_.compare(p_, 4, "e.p.") == 0) {  // "exd6 e.p." / "exd6e.p.": dropped
                for (int k = 0; k < 4; ++k) advance();
                continue;
            }
            const bool ok = isAlpha(c) || isDigit(c) || (unsigned char)c >= 0x80 || std::strchr("_+#=:-/", c);
            if (!ok) break;
            if (sym.size() < 40) sym += c;
            else tooLong = true;
            advance();
        }
        if (sym.empty()) {
            t.kind = Tok::End;  // caller skips
            return;
        }
        if (tooLong) return bad(t, "token too long");
        t.kind = Tok::Symbol;
        t.text = std::move(sym);
    }
};

// ---- Parser --------------------------------------------------------------------------------------
// Figurine SAN ("♘f3") to letters; the pawn figurines disappear.
std::string sanLetters(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x99) {
            const unsigned char d = (unsigned char)s[i + 2];
            if (d >= 0x94 && d <= 0x9F) {
                static const char letters[] = "KQRBN\0KQRBN\0";
                const char l = letters[d - 0x94];
                if (l) out += l;
                i += 2;
                continue;
            }
        }
        out += char(c);
    }
    return out;
}

// A Variant tag value naming Chess960 (which needs its start position in a FEN tag).
bool chess960Variant(const std::string& value) {
    const std::string v = lower(trim(value));
    return v == "chess960" || v == "chess 960" || v == "fischerandom" || v == "fischer random" || v == "960";
}

bool hasAsciiAlnum(const std::string& s) {
    for (char c : s)
        if (isAlpha(c) || isDigit(c)) return true;
    return false;
}

// One game of the input, parsed or scanned.
struct Parsed {
    Record record;
    int plies = 0;
    std::string movetextResult;
    Error error;
    size_t offset = 0, end = 0;
    int line = 1, column = 1;
};

struct TagPos {
    int line, column;
};

class Parser {
public:
    Parser(const std::string& text, const Limits& lim, Origin origin, bool validate)
        : lim_(lim), lex_(text, lim, origin), validate_(validate) {}

    template <class Out> void run(Result<Out>& out, Out (*convert)(Parsed&)) {
        Token t = lex_.next();
        while (t.kind != Tok::End) {
            if (int(out.games.size()) >= lim_.maxGames) {
                out.truncated = true;
                return;
            }
            Parsed g;
            if (game(t, g)) out.games.push_back(convert(g));
        }
    }

private:
    const Limits& lim_;
    Lexer lex_;
    bool validate_;

    // Parses the game starting at token t; t is left on the first token after it. False when the
    // tokens held no game at all (nothing but stray comments).
    bool game(Token& t, Parsed& g) {
        g.offset = t.begin;
        g.end = t.end;
        g.line = t.line;
        g.column = t.column;
        std::vector<TagPos> tagPos;
        bool started = false, skipping = false, setUp = false, any = false;
        int depth = 0, tagCount = 0;
        Position pos;
        Token last = t;
        auto fail = [&](int line, int column, const std::string& why) {
            if (g.error.empty()) g.error = Error{line, column, why};
            skipping = true;
        };
        auto failAt = [&](const Token& at, const std::string& why) { fail(at.line, at.column, why); };
        auto beginMoves = [&]() {
            started = true;
            if (setUp) return;
            setUp = true;
            std::string why;
            int at = -1;
            if (!startPosition(g.record, pos, why, at)) {
                if (at >= 0) fail(tagPos[size_t(at)].line, tagPos[size_t(at)].column, why);
                else failAt(t, why);
            }
        };
        auto finishResult = [&](const std::string& r) {
            if (depth > 0 && !skipping) failAt(t, "unterminated variation before the result");
            g.movetextResult = r;
            g.end = t.end;
            t = lex_.next();
        };
        for (;;) {
            switch (t.kind) {
            case Tok::End:
                if (any) beginMoves();  // a game of tags only: its FEN and Variant are checked too
                if (depth > 0 && !skipping) failAt(last, "unterminated variation");
                return finish(g, any);
            case Tok::TagPair:
                if (started) return finish(g, any);  // the next game (this one had no result)
                any = true;
                if (tagCount >= lim_.maxTags) {
                    failAt(t, "too many tags");
                } else {
                    ++tagCount;
                    if (keepTag(g.record, t.text)) {
                        g.record.tags.push_back(Tag{t.text, t.value});
                        tagPos.push_back(TagPos{t.line, t.column});
                    }
                }
                break;
            case Tok::Bad:
                if (t.tagLike && started) return finish(g, any);  // a broken tag opens the next game
                any = true;
                if (!skipping) failAt(t, t.text);
                break;
            case Tok::Comment:
                if (!skipping && depth == 0) comment(g, t);
                break;
            case Tok::Open:
                any = true;
                beginMoves();
                if (++depth > lim_.maxDepth && !skipping) failAt(t, "variations nested too deeply");
                break;
            case Tok::Close:
                any = true;
                beginMoves();
                if (depth == 0) {
                    if (!skipping) failAt(t, "')' without a variation");
                } else {
                    --depth;
                }
                break;
            case Tok::Nag:
                any = true;
                beginMoves();
                if (!skipping && depth == 0 && t.nag > 0 && !g.record.plies.empty()) {
                    std::vector<int>& nags = g.record.plies.back().nags;
                    if (nags.size() < 8) nags.push_back(t.nag);
                }
                break;
            case Tok::Star:
                any = true;
                beginMoves();  // "*" alone is a game too (a study chapter, a position): its FEN counts
                finishResult("*");
                return finish(g, true);
            case Tok::Symbol: {
                any = true;
                beginMoves();
                const std::string r = normalizeResult(t.text);
                if (!r.empty()) {
                    finishResult(r);
                    return finish(g, true);
                }
                if (skipping || depth > 0 || allDigits(t.text)) break;  // move numbers
                if (t.text == "--" || t.text == "Z0") {
                    failAt(t, "null moves are not supported");
                    break;
                }
                if (!hasAsciiAlnum(t.text)) break;  // an annotation glyph ("±", "∞")
                if (g.plies >= lim_.maxPlies) {
                    failAt(t, "too many moves (more than " + std::to_string(lim_.maxPlies) + ")");
                    break;
                }
                if (validate_) {
                    const Move m = pos.parseSAN(sanLetters(t.text));
                    if (!m.valid()) {
                        failAt(t, "illegal move '" + toUtf8(t.text) + "'");
                        break;
                    }
                    Ply p;
                    p.move = pos.findLegal(m.from, m.to, m.promotion);
                    p.san = pos.toSAN(p.move);
                    pos.makeMove(p.move);
                    g.record.plies.push_back(std::move(p));
                }
                ++g.plies;
                break;
            }
            }
            if (t.kind != Tok::Comment || any) g.end = t.end;
            last = t;
            t = lex_.next();
        }
    }

    // Whether a tag goes in the record: always when reading; in a scan, the Summary's filter.
    bool keepTag(const Record& r, const std::string& name) const {
        if (validate_ || !lim_.summaryTag) return true;
        if (r.findTag(name)) return false;
        return name == "Variant" || name == "SetUp" || name == "FEN" || name == "Result" || lim_.summaryTag(name);
    }

    bool finish(Parsed& g, bool any) {
        Record& r = g.record;
        const std::string tagResult = normalizeResult(r.tag("Result"));
        r.result = !g.movetextResult.empty() ? g.movetextResult : !tagResult.empty() ? tagResult : "*";
        return any;
    }

    // Start position from the tags. 'at' = index of the tag to blame, -1 = none.
    static bool startPosition(Record& r, Position& pos, std::string& why, int& at) {
        auto index = [&](const char* name) {
            for (size_t i = 0; i < r.tags.size(); ++i)
                if (r.tags[i].name == name) return int(i);
            return -1;
        };
        const int vi = index("Variant"), fi = index("FEN"), si = index("SetUp");
        bool chess960 = false;
        if (vi >= 0) {
            const std::string v = lower(trim(r.tags[size_t(vi)].value));
            if (chess960Variant(v)) {
                chess960 = true;
            } else if (!v.empty() && v != "standard" && v != "chess" && v != "normal" && v != "from position") {
                why = "variant '" + r.tags[size_t(vi)].value + "' is not supported";
                at = vi;
                return false;
            }
        }
        const bool useFen = fi >= 0 && !(si >= 0 && trim(r.tags[size_t(si)].value) == "0");
        if (!useFen) {
            if (chess960) {
                why = "Chess960 game without a FEN tag";
                at = vi;
                return false;
            }
            pos.setStart();
            return true;
        }
        const std::string fen = trim(r.tags[size_t(fi)].value);
        Position p;
        if (!p.setFEN(fen)) {
            why = chess960 ? "Chess960 castling rights are not supported" : "invalid FEN";
            at = fi;
            return false;
        }
        if (chess960) {
            // The castling rights asked for must all survive (king and rooks on the standard squares).
            uint8_t asked = 0;
            size_t sp = fen.find(' ');
            sp = sp == std::string::npos ? sp : fen.find(' ', sp + 1);
            if (sp != std::string::npos) {
                const size_t e = fen.find(' ', sp + 1);
                for (char c : fen.substr(sp + 1, e == std::string::npos ? std::string::npos : e - sp - 1)) {
                    if (c == 'K') asked |= WhiteKingSide;
                    if (c == 'Q') asked |= WhiteQueenSide;
                    if (c == 'k') asked |= BlackKingSide;
                    if (c == 'q') asked |= BlackQueenSide;
                }
            }
            if (asked != p.castling()) {
                why = "Chess960 castling from this setup is not supported";
                at = fi;
                return false;
            }
        }
        pos = p;
        const Position standard;
        if (!(p.samePosition(standard) && p.halfmoveClock() == 0 && p.fullmoveNumber() == 1)) r.fen = p.fen();
        return true;
    }

    // Comment text after a move (or before the first one): [%clk] and [%emt] taken out.
    void comment(Parsed& g, const Token& t) {
        if (!validate_) return;
        Ply* ply = g.record.plies.empty() ? nullptr : &g.record.plies.back();
        std::string text = toUtf8(t.text), kept;
        size_t i = 0;
        while (i < text.size()) {
            const size_t open = text.find("[%", i);
            if (open == std::string::npos) {
                kept += text.substr(i);
                break;
            }
            kept += text.substr(i, open - i);
            const size_t close = text.find(']', open);
            if (close == std::string::npos) {
                kept += text.substr(open);
                break;
            }
            size_t q = open + 2;
            std::string name;
            while (q < close && isAlpha(text[q])) name += text[q++];
            int64_t ms = -1;
            const bool timed = (name == "clk" || name == "emt") && ply && parseTime(trim(text.substr(q, close - q)), ms);
            if (timed) (name == "clk" ? ply->clockMs : ply->elapsedMs) = ms;
            else kept += text.substr(open, close + 1 - open);
            i = close + 1;
        }
        // One line: runs of white space become one space.
        std::string clean;
        for (char c : kept) {
            if (isSpace(c)) {
                if (!clean.empty() && clean.back() != ' ') clean += ' ';
            } else {
                clean += c;
            }
        }
        clean = trim(clean);
        if (clean.empty()) return;
        std::string& dst = ply ? ply->comment : g.record.comment;
        if (!dst.empty()) dst += ' ';
        dst += clean;
        cutUtf8(dst, lim_.maxComment);
    }
};

ParsedGame toGame(Parsed& p) {
    ParsedGame g;
    g.record = std::move(p.record);
    g.error = p.error;
    g.offset = p.offset;
    g.length = p.end - p.offset;
    g.line = p.line;
    g.column = p.column;
    return g;
}

Summary toSummary(Parsed& p) {
    Summary s;
    s.tags = std::move(p.record.tags);
    s.plies = p.plies;
    s.result = p.record.result;
    s.error = p.error;
    s.offset = p.offset;
    s.length = p.end - p.offset;
    s.line = p.line;
    s.column = p.column;
    return s;
}

template <class Out> Result<Out> parseInput(const std::string& text, const Limits& limits, Origin origin, bool validate,
                                            Out (*convert)(Parsed&)) {
    Result<Out> out;
    if (text.size() > limits.maxBytes) {
        out.error = "the file is too large (more than " + std::to_string(limits.maxBytes >> 20) + " MB)";
        return out;
    }
    if (text.size() >= 2 && (((unsigned char)text[0] == 0xFF && (unsigned char)text[1] == 0xFE) ||
                             ((unsigned char)text[0] == 0xFE && (unsigned char)text[1] == 0xFF))) {
        out.error = "UTF-16 text is not supported (save the file as UTF-8)";
        return out;
    }
    Parser parser(text, limits, origin, validate);
    parser.run(out, convert);
    return out;
}

}  // namespace

// ---- Public: records -----------------------------------------------------------------------------

const std::string* Record::findTag(const std::string& name) const {
    for (const Tag& t : tags)
        if (t.name == name) return &t.value;
    return nullptr;
}

std::string Record::tag(const std::string& name, const std::string& fallback) const {
    const std::string* v = findTag(name);
    return v ? *v : fallback;
}

void Record::setTag(const std::string& name, const std::string& value) {
    for (Tag& t : tags)
        if (t.name == name) {
            t.value = value;
            return;
        }
    tags.push_back(Tag{name, value});
}

void Record::eraseTag(const std::string& name) {
    tags.erase(std::remove_if(tags.begin(), tags.end(), [&](const Tag& t) { return t.name == name; }), tags.end());
}

Position Record::startPosition() const {
    Position p;
    if (!fen.empty() && !p.setFEN(fen)) p.setStart();
    return p;
}

std::vector<Position> Record::positions() const {
    std::vector<Position> out;
    out.reserve(plies.size() + 1);
    out.push_back(startPosition());
    for (const Ply& p : plies) {
        const Move m = out.back().findLegal(p.move.from, p.move.to, p.move.promotion);
        if (!m.valid()) break;  // a record built by hand with a wrong move: positions stop there
        Position next = out.back();
        next.makeMove(m);
        out.push_back(next);
    }
    return out;
}

bool Record::hasClocks() const {
    for (const Ply& p : plies)
        if (p.clockMs >= 0) return true;
    return false;
}

bool Record::hasElapsed() const {
    for (const Ply& p : plies)
        if (p.elapsedMs >= 0) return true;
    return false;
}

Record Record::fromGame(const chess::Game& game) {
    Record r;
    const Position& start = game.startPosition();
    const Position standard;
    if (!(start.samePosition(standard) && start.halfmoveClock() == 0 && start.fullmoveNumber() == 1)) r.fen = start.fen();
    for (size_t i = 0; i < game.moves().size(); ++i) {
        Ply p;
        p.move = game.moves()[i];
        p.san = game.sanMoves()[i];
        r.plies.push_back(std::move(p));
    }
    r.result = game.resultString();
    return r;
}

bool Record::toGame(chess::Game& game) const {
    if (fen.empty()) game.reset();
    else if (!game.resetFromFEN(fen)) return false;
    for (const Ply& p : plies)
        if (!game.play(p.move)) return false;
    return true;
}

std::string Error::text() const {
    if (message.empty()) return std::string();
    if (line <= 0) return message;
    return "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message;
}

std::string Summary::tag(const std::string& name, const std::string& fallback) const {
    for (const Tag& t : tags)
        if (t.name == name) return t.value;
    return fallback;
}

// ---- Public: reading -----------------------------------------------------------------------------

Result<ParsedGame> read(const std::string& text, const Limits& limits, Origin origin) {
    return parseInput<ParsedGame>(text, limits, origin, true, &toGame);
}

Result<Summary> scan(const std::string& text, const Limits& limits, Origin origin) {
    return parseInput<Summary>(text, limits, origin, false, &toSummary);
}

// ---- Public: writing -----------------------------------------------------------------------------

namespace {
const char* const kRanked[] = {"Event",   "Site",      "Date",     "Round",    "White",    "Black",    "Result",
                               "TimeControl", "Termination", "ECO", "Opening", "Variation", "WhiteElo", "BlackElo"};
// Computed by the writer: never copied from the record.
bool computedTag(const std::string& n) { return n == "Result" || n == "PlyCount" || n == "SetUp" || n == "FEN"; }

std::string tagValue(const std::string& v) {
    std::string out;
    for (char c : toUtf8(v)) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        else if ((unsigned char)c < 0x20) continue;
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}
bool validTagName(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n)
        if (!isAlpha(c) && !isDigit(c) && c != '_') return false;
    return true;
}
std::string commentText(const std::string& s) {
    std::string out;
    for (char c : toUtf8(s)) {
        if (c == '{') c = '(';
        else if (c == '}') c = ')';
        else if (isSpace(c)) c = ' ';
        else if ((unsigned char)c < 0x20) continue;
        out += c;
    }
    return trim(out);
}

// Movetext lines of at most 80 columns.
struct Wrapper {
    std::string out, line;
    void emit(const std::string& tok) {
        if (!line.empty() && line.size() + 1 + tok.size() > 80) {
            out += line + '\n';
            line.clear();
        }
        if (!line.empty()) line += ' ';
        line += tok;
    }
    // A comment, breakable between its words (each [%command] stays whole).
    void comment(const std::vector<std::string>& commands, const std::string& text) {
        std::vector<std::string> words = commands;
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && text[i] == ' ') ++i;
            size_t j = text.find(' ', i);
            if (j == std::string::npos) j = text.size();
            if (j > i) words.push_back(text.substr(i, j - i));
            i = j;
        }
        if (words.empty()) return;
        words.front() = "{" + words.front();
        words.back() += "}";
        for (const std::string& w : words) emit(w);
    }
};
}  // namespace

int tagRank(const std::string& name) {
    for (int i = 0; i < int(sizeof(kRanked) / sizeof(kRanked[0])); ++i)
        if (name == kRanked[i]) return i;
    if (name == "PlyCount") return 14;
    if (name == "SetUp") return 15;
    if (name == "FEN") return 16;
    return 100;
}

std::string write(const Record& r) {
    std::string out;
    auto tag = [&out](const std::string& name, const std::string& value) {
        out += "[" + name + " \"" + tagValue(value) + "\"]\n";
    };
    static const char* const defaults[7] = {"?", "?", "????.??.??", "?", "?", "?", ""};
    for (int i = 0; i < 7; ++i) {
        const std::string name = kRanked[i];
        if (name == "Result") {
            const std::string res = normalizeResult(r.result);
            tag(name, res.empty() ? "*" : res);
            continue;
        }
        const std::string* v = r.findTag(name);
        tag(name, v && !v->empty() ? *v : defaults[i]);
    }
    for (size_t i = 7; i < sizeof(kRanked) / sizeof(kRanked[0]); ++i)
        if (const std::string* v = r.findTag(kRanked[i])) tag(kRanked[i], *v);
    tag("PlyCount", std::to_string(r.plies.size()));
    const Position start = r.startPosition();
    const Position standard;
    const bool custom = !(start.samePosition(standard) && start.halfmoveClock() == 0 && start.fullmoveNumber() == 1);
    // A Chess960 game from the standard setup (position 518) keeps its FEN: the reader requires it.
    const std::string* variant = r.findTag("Variant");
    if (custom || (variant && chess960Variant(*variant))) {
        tag("SetUp", "1");
        tag("FEN", start.fen());
    }
    for (const Tag& t : r.tags)
        if (tagRank(t.name) == 100 && !computedTag(t.name) && validTagName(t.name)) tag(t.name, t.value);
    out += '\n';

    // Movetext.
    Wrapper w;
    int moveNo = start.fullmoveNumber();
    Color side = start.sideToMove();
    bool afterComment = false;
    const std::string pre = commentText(r.comment);
    if (!pre.empty()) {
        w.comment({}, pre);
        afterComment = true;
    }
    std::vector<Position> positions;
    for (size_t i = 0; i < r.plies.size(); ++i) {
        const Ply& p = r.plies[i];
        if (side == White) w.emit(std::to_string(moveNo) + ".");
        else if (i == 0 || afterComment) w.emit(std::to_string(moveNo) + "...");
        std::string san = p.san;
        if (san.empty()) {  // a record built without SAN: from the positions
            if (positions.empty()) positions = r.positions();
            san = i < positions.size() ? positions[i].toSAN(p.move) : std::string();
            if (san.empty()) break;
        }
        w.emit(san);
        for (int n : p.nags)
            if (n > 0 && n <= 255) w.emit("$" + std::to_string(n));
        std::vector<std::string> commands;
        if (p.clockMs >= 0) commands.push_back("[%clk " + formatTime(p.clockMs) + "]");
        if (p.elapsedMs >= 0) commands.push_back("[%emt " + formatTime(p.elapsedMs) + "]");
        const std::string text = commentText(p.comment);
        afterComment = !commands.empty() || !text.empty();
        if (afterComment) w.comment(commands, text);
        if (side == Black) ++moveNo;
        side = opposite(side);
    }
    const std::string res = normalizeResult(r.result);
    w.emit(res.empty() ? "*" : res);
    out += w.out + w.line + '\n';
    return out;
}

std::string writeAll(const std::vector<Record>& records) {
    std::string out;
    for (size_t i = 0; i < records.size(); ++i) {
        if (i) out += '\n';
        out += write(records[i]);
    }
    return out;
}

// ---- Public: helpers -----------------------------------------------------------------------------

std::string formatTime(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t h = ms / 3600000, m = ms / 60000 % 60, s = ms / 1000 % 60, f = ms % 1000;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%lld:%02d:%02d", (long long)h, int(m), int(s));
    std::string out = buf;
    if (f) {
        std::snprintf(buf, sizeof buf, ".%03d", int(f));
        std::string frac = buf;
        while (frac.back() == '0') frac.pop_back();
        out += frac;
    }
    return out;
}

bool parseTime(const std::string& s, int64_t& ms) {
    std::vector<std::string> parts;
    size_t i = 0;
    for (;;) {
        const size_t c = s.find(':', i);
        parts.push_back(s.substr(i, c == std::string::npos ? std::string::npos : c - i));
        if (c == std::string::npos) break;
        i = c + 1;
    }
    if (parts.empty() || parts.size() > 3) return false;
    std::string last = parts.back(), frac;
    const size_t dot = last.find('.');
    if (dot != std::string::npos) {
        frac = last.substr(dot + 1);
        last.resize(dot);
        if (!frac.empty() && !allDigits(frac)) return false;
    }
    parts.back() = last;
    int64_t total = 0;
    for (size_t k = 0; k < parts.size(); ++k) {
        const std::string& p = parts[k];
        if (!allDigits(p) || p.size() > 6) return false;
        const int64_t v = std::stoll(p);
        if (k > 0 && v >= 60) return false;  // minutes and seconds after the first field
        total = total * 60 + v;
    }
    if (total > int64_t(1000) * 3600) return false;
    int64_t f = 0;
    for (size_t k = 0; k < 3; ++k) f = f * 10 + (k < frac.size() ? frac[k] - '0' : 0);
    ms = total * 1000 + f;
    return true;
}

bool parseTimeControl(const std::string& tcIn, int64_t& baseMs, int64_t& incrementMs) {
    const std::string tc = trim(tcIn);
    const size_t plus = tc.find('+');
    const std::string base = tc.substr(0, plus);
    const std::string inc = plus == std::string::npos ? std::string("0") : tc.substr(plus + 1);
    if (!allDigits(base) || !allDigits(inc) || base.size() > 7 || inc.size() > 6) return false;
    baseMs = std::stoll(base) * 1000;
    incrementMs = std::stoll(inc) * 1000;
    return baseMs > 0 || incrementMs > 0;
}

std::string normalizeResult(const std::string& s) {
    if (s == "1-0" || s == "0-1" || s == "1/2-1/2" || s == "*") return s;
    if (s == "\xC2\xBD-\xC2\xBD" || s == "0.5-0.5" || s == "1/2") return "1/2-1/2";
    return std::string();
}

const char* terminationValue(GameStatus status, GameEndReason reason) { return terminationTag(status, reason); }

}  // namespace pgn
}  // namespace chess
