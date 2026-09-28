// Translations. Every language has its own UTF-8 file in assets/i18n/<code>.lang (embedded into
// the executable like every asset): one "key = value" per line, '#' starts a comment line, "\n" in
// a value is a line break. English (en.lang) is the reference: a key missing from the current
// language falls back to English, then to the key itself. Placeholders are {0}, {1}... (trf).
//
// Code uses keys, never English literals, for anything shown to the player:
//     ui::text(i18n::tr("menu.new_game"), ...);
//     ui::notify(i18n::trf("arbiter.touch_move", {squareName(sq)}));
//
// Plurals: a value whose forms are separated by '|' is chosen by trn() with the CLDR plural rule
// of the language, in this order: English/German/Spanish "one|other", French "one|other" (0 and 1
// are singular), Russian/Ukrainian "one|few|many", Arabic "zero|one|two|few|many|other",
// Japanese/Chinese a single form. pluralForms(code) gives the count a file must provide.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace i18n {

struct Language {
    const char* code;        // file name in assets/i18n/ and .ini value ("fr", "zh-Hant", ...)
    const char* nativeName;  // shown in the language selector ("Français", "日本語", ...)
    bool rtl;                // right-to-left script (Arabic)
};

// English, French, German, Spanish, Ukrainian, Arabic, Russian, Japanese, Traditional Chinese,
// Simplified Chinese (in that order; index 0 = English).
const std::vector<Language>& languages();
int languageIndex(const std::string& code);  // -1 when unknown

// Loads a language (English is always loaded as the fallback). Unknown code -> English.
bool setLanguage(const std::string& code);
const std::string& language();               // current code
bool rtl();                                  // current language is right-to-left
int generation();                            // incremented by every setLanguage() (cache keys)
// Supported language for an OS locale tag ("fr-FR", "zh_TW.UTF-8", "zh-Hant-HK", "pt-BR"...):
// zh-TW/HK/MO and zh-Hant -> "zh-Hant", other Chinese -> "zh-Hans", unsupported -> "en".
std::string matchLocale(const std::string& tag);

// Translation of 'key' in the current language (pointer stays valid until the next
// setLanguage()).
const char* tr(const char* key);
std::string tr(const std::string& key);
// English text of 'key' whatever the current language (logs, PGN comments).
const char* english(const char* key);
// tr() with {0}, {1}... replaced by args.
std::string trf(const char* key, std::initializer_list<std::string> args);
// Plural: the form of 'key' for the count n, {0} = n unless args are given.
std::string trn(const char* key, long long n, std::initializer_list<std::string> args = {});
// 'key' when it exists (current language or English), else 'fallback'.
std::string trOr(const std::string& key, const std::string& fallback);
bool has(const char* key);                   // key exists in the current language or English
// Replaces {0}, {1}... in any string.
std::string format(const std::string& pattern, const std::vector<std::string>& args);
// 's' wrapped in left-to-right marks when the UI is right-to-left, so a clock time ("10+5",
// "3:05"), a move ("Nf3+") or a Latin name keeps its order inside Arabic text; unchanged otherwise.
std::string ltr(const std::string& s);

// Plural forms of a language and the form index for a count (see above).
int pluralForms(const std::string& code);
int pluralIndex(const std::string& code, long long n);

// Every codepoint used by the current language's strings (sorted, unique; Arabic letters are
// given in their contextual presentation forms): the UI builds these glyphs up front.
std::vector<uint32_t> codepoints();

// Parses one .lang file (for tests and tools). Returns false on a malformed line.
bool parse(const std::string& text, std::vector<std::pair<std::string, std::string>>& out, std::string* error = nullptr);

}  // namespace i18n
