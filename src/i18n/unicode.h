// Engine-free Unicode text logic used by the UI text shaper (src/ui/text_shape.cpp) and the unit
// tests: UTF-8 <-> UTF-32, Arabic contextual shaping to the Unicode presentation forms, Devanagari
// syllables to the fonts' precomposed glyphs, a simplified Unicode bidirectional algorithm for one
// line, bracket mirroring, line break opportunities (spaces, CJK with basic kinsoku rules) and
// simple upper-casing.
//
// Everything works on codepoints; fonts and glyphs are the UI's business.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace uni {

// ---- UTF-8 ------------------------------------------------------------------------------------------
// Invalid sequences decode to U+FFFD, one per bad byte.
std::u32string decode(const std::string& utf8);
// The codepoint at utf8[i] (i < size), decoded as decode() does; i moves past the bytes it used.
char32_t decodeAt(const std::string& utf8, size_t& i);
std::string encode(const std::u32string& text);
void append(std::string& out, char32_t cp);

// ---- Scripts -----------------------------------------------------------------------------------------
bool isArabic(char32_t cp);         // Arabic blocks, including the presentation forms
bool isRtl(char32_t cp);            // strong right-to-left (Arabic, Hebrew, ...)
bool isHan(char32_t cp);            // CJK ideographs
bool isKana(char32_t cp);           // hiragana, katakana (incl. half-width)
bool isCjkPunct(char32_t cp);       // CJK symbols and punctuation, full-width forms
bool isCjk(char32_t cp);            // any of the three above (no spaces between words)
bool isHangul(char32_t cp);         // Hangul syllables and jamo
bool isDevanagari(char32_t cp);     // Devanagari block and the fonts' precomposed syllables (below)
bool isMark(char32_t cp);           // combining mark (zero advance, drawn on its base)
bool isSpace(char32_t cp);
bool containsRtl(const std::u32string& text);

// ---- Arabic joining -----------------------------------------------------------------------------------
// Joining types of the Unicode ArabicShaping data: U non-joining, R right-joining (joins the
// previous letter only), D dual-joining, C join-causing (tatweel, ZWJ), T transparent (marks).
enum class Joining { U, R, D, C, T };
Joining joiningType(char32_t cp);

// Contextual form of a letter: isolated, final (joined to the previous letter), initial (joined to
// the next one), medial (both).
enum class Form { Isolated = 0, Final = 1, Initial = 2, Medial = 3 };
// Form of every character of a logical string (transparent characters and non-letters: Isolated).
std::vector<Form> joiningForms(const std::u32string& logical);
// Presentation-form codepoint of a letter in a form, 0 when the letter has none.
char32_t presentationForm(char32_t cp, Form form);
// Lam-alef ligature (U+FEF5..U+FEFC) for lam + this alef variant; 0 when 'alef' is not one.
char32_t lamAlefLigature(char32_t alef, bool joinedToPrevious);

struct Shaped {
    std::u32string text;       // presentation forms
    std::vector<int> source;   // text[i] comes from logical character source[i]
};
// Maps Arabic letters to their contextual presentation forms and forms the mandatory lam-alef
// ligatures (the alef has no output of its own; the ligature's source is the lam). Text without
// Arabic is returned unchanged.
Shaped shapeArabic(const std::u32string& logical);

// ---- Devanagari syllables -----------------------------------------------------------------------------
// stb_truetype has no OpenType shaping either: every orthographic syllable that the game's
// Devanagari fonts draw otherwise than its letters side by side (reordered short i, conjuncts,
// marks placed on their consonant, reph) is a precomposed glyph at a Private Use codepoint of
// U+E000..U+EFFF, the same in every Devanagari face. tools/prepare_fonts.py bakes them and lists
// them in assets/fonts/devanagari-clusters.txt, which i18n loads.
constexpr char32_t kDevanagariClusterFirst = 0xE000, kDevanagariClusterLast = 0xEFFF;
// The syllables of a text, [begin, end) each, in order (consonant clusters joined by virama with
// their nukta, vowel signs and bindus; an independent vowel with its signs). Other characters
// belong to none. Must match deva_syllables in tools/prepare_fonts.py.
std::vector<std::pair<int, int>> devanagariSyllables(const std::u32string& text);
// Loads the syllable table (lines "<hex codepoint> <syllable>", '#' comments) for
// shapeDevanagari; replaces the previous one. Returns the number of syllables.
int setDevanagariClusters(const std::string& fileText);
// Precomposed codepoint of a syllable, 0 when the fonts draw it from its letters.
char32_t devanagariCluster(const std::u32string& syllable);
// Replaces every syllable that has a precomposed glyph by its codepoint (its source: the
// syllable's first character). Text without Devanagari is returned unchanged.
Shaped shapeDevanagari(const Shaped& in);
// Everything the fonts need before drawing: shapeArabic, then shapeDevanagari.
Shaped shape(const std::u32string& logical);

// ---- Bidirectional text ---------------------------------------------------------------------------------
// Simplified Unicode Bidirectional Algorithm (UAX #9) for a single line without explicit
// embeddings or isolates: weak types (W1-W7), neutrals (N1-N2), implicit levels (I1-I2), trailing
// whitespace (L1) and reordering (L2).
enum class BidiClass { L, R, AL, EN, ES, ET, AN, CS, NSM, BN, B, S, WS, ON };
BidiClass bidiClass(char32_t cp);
// 0 = left-to-right, 1 = right-to-left, from the first strong character (P2/P3); 'fallback' when
// there is none.
int paragraphLevel(const std::u32string& text, int fallback = 0);
// Embedding level of every character (even = LTR, odd = RTL).
std::vector<int> resolveLevels(const std::u32string& text, int baseLevel);
// Visual (left-to-right drawing) order: result[k] = logical index drawn at position k.
std::vector<int> visualOrder(const std::vector<int>& levels);
// Mirrored glyph for characters drawn in a right-to-left run: ( -> ), « -> », ... (else cp).
char32_t mirror(char32_t cp);

// ---- Line breaking ------------------------------------------------------------------------------------
// True when a line may break between 'before' and 'after' without a space (CJK text), honouring the
// basic Japanese / Chinese line-start and line-end prohibitions (no break before 。、」…, none
// after 「（…).
bool breakBetween(char32_t before, char32_t after);

// Share of the letter-spacing to put between two neighbours. The UI tracks its small capitals
// widely; full-width CJK glyphs already carry their spacing and fall apart into single characters
// with the full amount, so pairs of them get a quarter.
float trackingScale(char32_t before, char32_t after);

// ---- Case ------------------------------------------------------------------------------------------------
// Simple upper-case mapping for Latin (Latin-1, Extended-A, the Extended-B letters of Romanian and
// Vietnamese, Latin Extended Additional), Greek and Cyrillic. toUpper keeps accents (a sentence's
// first letter: "Ένα"); toCapital is the all-caps form: Greek capitals drop the tonos ("ΕΝΑ"), and
// in Turkish (turkic) i becomes İ.
char32_t toUpper(char32_t cp, bool turkic = false);
char32_t toCapital(char32_t cp, bool turkic = false);
bool isLower(char32_t cp);
// All caps (toCapital; ß -> SS). lang: the text's language code ("tr": dotted capital İ).
std::string toUpper(const std::string& utf8, const std::string& lang = std::string());

}  // namespace uni
