// The coach's speech catalog: hand-written sentence templates in the 10 UI languages, rendered
// twice from one Line (coach/script.h): the spoken text the TTS reads and the written text of the
// subtitles. No language model: every sentence is a template with typed placeholders, and a
// message has several phrasings (variants) so that the coach does not repeat itself word for word.
//
// Files (embedded assets, same "key = value" syntax as assets/i18n, parsed by i18n::parse):
//   assets/coach/speech/<code>/<topic>.lang   common (pieces and their grammatical forms, squares,
//                                             moves and numbers spoken), events, lesson, review,
//                                             appraisal; every file of a language shares one key space
//   assets/coach/openings/<code>.lang         opening names and sentences (coach/openings.h)
// English is the reference; a key missing in a language falls back to English.
//
// Template syntax:
//   key, key.2, key.3 ...   variants: alternative phrasings of one message
//   key.spoken              what the voice says instead of 'key' (respellings, digits in words);
//                           written renderings never use it
//   respell.<word>          (common.lang) what the voice says instead of a word it misreads, in
//                           every spoken line of the language: whole words, any case ("mat" ->
//                           "matte"); respell.<word>.pause only before a pause (punctuation or the
//                           end of the line: "huit" -> "huite", but "huit coups" unchanged)
//   {name}, {name:form}     placeholder filled from the Line's argument 'name' (a form selects a
//                           grammatical form, see common.lang)
//   {@}, {@2} ...           zero-width anchors of static lines (removed from the text): the
//                           director times a gesture on the word that follows
// Renderings: written = squares "e4", figurine SAN "♘f3", digits, and in Arabic the Latin
// fragments wrapped in left-to-right marks; spoken = squares, moves and numbers in words.
//
// Thread safety: load() once before any use (not concurrently with rendering); render() may then
// be called from any thread (the variant history is locked). Everything else is read-only.
#pragma once
#include "script.h"
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace coach {

// Language the coach speaks for a UI language: the UI language when the voice has it (en fr de es
// ru uk ar ja), else English (Supertonic 3 has no Chinese: Chinese UIs get English speech and
// Chinese subtitles).
std::string speechLanguage(const std::string& uiLanguage);
// True when the coach's voice speaks this language (the voice's own list, tts::languageSupported).
bool speechSupported(const std::string& lang);

class Catalog {
public:
    // Byte offset, in Rendered::text, of the first character of a placeholder's rendering or of a
    // {@} marker: the director estimates when that word is heard (coach/pacing.h).
    struct Anchor {
        std::string name;   // placeholder name ("sq", "your") or marker name ("@", "@2")
        int offset = 0;
    };
    struct Rendered {
        std::string text;
        std::vector<Anchor> anchors;   // in text order (a placeholder used twice appears twice)
        int variant = 1;               // the phrasing used: 1 = 'key', 2 = 'key.2' ...
        // Offset of the first anchor with this name, -1 when the line has none.
        int anchor(const std::string& name) const;
    };

    // The catalog every game system shares, loaded from the embedded assets on first use.
    static Catalog& shared();

    // Loads every language found under assets/coach/speech/ and assets/coach/openings/ (embedded).
    // False when English is missing. Problems (parse errors, duplicate keys) are logged and kept
    // in problems().
    bool load();
    // Adds one file's text to a language (load() and the tests). 'topic' names the file ("events").
    bool addFile(const std::string& lang, const std::string& topic, const std::string& text);
    void clear();

    // Text of a line. spoken = the TTS text in speechLanguage(lang); otherwise the subtitle text in
    // 'lang'. The same seed picks the same variant in both renderings; consecutive seeds avoid
    // repeating the last pick of the key (the last two when it has three variants or more).
    Rendered render(const Line& line, const std::string& lang, bool spoken, uint32_t variantSeed) const;
    // Same with an explicit variant (1-based; clamped to the English count): pre-synthesis, tests.
    Rendered renderVariant(const Line& line, const std::string& lang, bool spoken, int variant) const;
    // The variant render() uses for this key and seed (remembered, so both renderings agree).
    int pickVariant(const std::string& key, uint32_t variantSeed) const;
    void resetHistory() const;

    bool has(const std::string& key) const;                              // in English
    bool has(const std::string& lang, const std::string& key) const;     // in that language itself
    int variants(const std::string& key) const;                          // English count, 0 = no key
    // Raw template of a key in a language (no fallback), nullptr when absent.
    const std::string* find(const std::string& lang, const std::string& key) const;

    std::vector<std::string> languages() const;                         // loaded codes, sorted
    std::vector<std::string> topics(const std::string& lang) const;      // "common", "events" ...
    // Keys of one topic file of a language, in file order (tests: parity with English).
    std::vector<std::string> keys(const std::string& lang, const std::string& topic) const;
    const std::vector<std::string>& problems() const { return problems_; }

    // Every codepoint the written lines of a language can show (subtitle glyph prewarm).
    std::vector<uint32_t> codepoints(const std::string& lang) const;

    // Opening names the catalog cannot resolve from the opening files alone ("line:<component>"
    // references, OpeningTexts::arg composes them). Returns "" when it cannot say it.
    using OpeningResolver = std::function<std::string(const std::string& ref, const std::string& form,
                                                      const std::string& lang, bool spoken)>;
    void setOpeningResolver(OpeningResolver r) { openingResolver_ = std::move(r); }

private:
    struct Entry {
        std::string value;
        std::string topic;
    };
    struct Respelling {
        std::u32string word;     // as written in the key
        std::u32string spoken;
        bool pauseOnly = false;
    };
    struct Language {
        std::unordered_map<std::string, Entry> map;
        std::map<std::string, std::vector<std::string>> topicKeys;   // topic -> keys in file order
        std::vector<Respelling> respellings;                         // the respell.* keys
    };
    std::map<std::string, Language> langs_;
    std::vector<std::string> problems_;
    OpeningResolver openingResolver_;

    struct History {
        std::vector<int> recent;                             // last picks, newest last
        std::vector<std::pair<uint32_t, int>> bySeed;        // recent (seed, pick) pairs
    };
    mutable std::mutex historyMutex_;
    mutable std::unordered_map<std::string, History> history_;

    // Rendering (catalog.cpp).
    struct Ctx;
    struct Local;
    const Language* language(const std::string& code) const;
    const std::string* lookup(const Ctx& c, const std::string& key, std::string* fromLang = nullptr) const;
    std::string expand(Ctx& c, const std::string& tmpl, const std::vector<Local>* locals, bool top) const;
    std::string renderArg(Ctx& c, const Arg& a, const std::string& form) const;
    std::string renderPiece(Ctx& c, const Arg& a, const std::string& form) const;
    std::string renderSquare(Ctx& c, chess::Square sq) const;
    std::string renderMove(Ctx& c, const std::string& san, const std::string& form, chess::Square* prevTo) const;
    std::string renderMoves(Ctx& c, const std::string& line, const std::string& form) const;
    std::string renderNumber(Ctx& c, int n, const std::string& form) const;
    std::string numberWords(Ctx& c, int n, const std::string& form) const;
    std::string renderEval(Ctx& c, const Arg& a) const;
    std::string renderOpening(Ctx& c, const std::string& ref, const std::string& form) const;
    std::string renderText(Ctx& c, const std::string& keyOrText, const std::string& form) const;
    std::string pattern(Ctx& c, const std::string& key, const std::vector<Local>& locals) const;
    static void respell(const Language& L, Rendered& r);
    Rendered renderWith(const Line& line, const std::string& lang, bool spoken, int variant, uint32_t seed) const;
};

// Piece letters of written SAN with letters ({move:letters}, "Nf3"), in the language's order
// K Q R B N from common.lang "move.letters" ("K Q R B N" in English).
std::string sanWithLetters(const std::string& san, const std::string& letters);
// Written SAN with figurines: "Nxe5+" -> "♘xe5+", "e8=Q" -> "e8=♕".
std::string figurineSan(const std::string& san);

}  // namespace coach
