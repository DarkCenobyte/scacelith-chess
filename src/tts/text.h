// Text front end of the TTS: the official Supertonic 3 normalisation (py/helper.py,
// UnicodeProcessor._preprocess_text, reproduced character for character), the character indexer
// and the chunker that cuts long text into sentence-aligned pieces.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tts {
namespace text {

// Unicode NFKD (full compatibility decomposition, Hangul included, canonical reordering).
std::u32string nfkd(const std::u32string& s);

// Language tags the model knows (Supertonic 3: 31 languages plus "na").
bool modelLanguage(const std::string& lang);

// The official normalisation, including its wrapper: "<lang>text</lang>". Input is UTF-8.
std::u32string preprocess(const std::string& utf8, const std::string& lang);

// Model ids of a preprocessed text through the 65536-entry indexer. Code points the indexer does
// not know (or outside the BMP) are dropped and counted in 'dropped'.
std::vector<int64_t> indices(const std::u32string& s, const int32_t* indexer, int* dropped = nullptr);

// Splits UTF-8 text into chunks of at most maxLen code points: paragraphs on blank lines,
// sentences after . ! ? (followed by white space, not after common abbreviations) and after the
// terminators of other scripts (。！？ without a space, ؟ ؛ ۔), packed greedily. A sentence longer
// than maxLen is cut at commas (, ، 、), then at spaces, then anywhere.
std::vector<std::string> chunk(const std::string& utf8, size_t maxLen);

// Chunk length of the official helper: 120 for ko and ja, 300 otherwise.
size_t chunkLength(const std::string& lang);

}  // namespace text
}  // namespace tts
