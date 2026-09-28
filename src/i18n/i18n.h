// Translations. Every language has its own UTF-8 file in assets/i18n/<code>.lang (embedded into
// the executable like every asset): one "key = value" per line, '#' starts a comment line, "\n" in
// a value is a line break. English (en.lang) is the reference: a key missing from the current
// language falls back to English, then to the key itself. Placeholders are {0}, {1}... (trf).
//
// Code uses keys, never English literals, for anything shown to the player:
//     ui::text(i18n::tr("menu.new_game"), ...);
//     ui::notify(i18n::trf("arbiter.touch_move", {squareName(sq)}));
#pragma once
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

// Translation of 'key' in the current language (pointer stays valid until the next
// setLanguage()).
const char* tr(const char* key);
std::string tr(const std::string& key);
// tr() with {0}, {1}... replaced by args.
std::string trf(const char* key, std::initializer_list<std::string> args);
bool has(const char* key);                   // key exists in the current language or English

// Parses one .lang file (for tests and tools). Returns false on a malformed line.
bool parse(const std::string& text, std::vector<std::pair<std::string, std::string>>& out, std::string* error = nullptr);

}  // namespace i18n
