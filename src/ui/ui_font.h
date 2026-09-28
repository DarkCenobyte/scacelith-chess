// UI fonts: embedded TTF faces rasterised into a single-channel signed distance field atlas at
// ui::init(). Glyphs outside the pre-built set are added lazily. Internal to src/ui.
#pragma once
#include "../gl/gl46.h"
#include <cstdint>
#include <string>

namespace ui {
namespace font {

enum Face : int {
    FACE_TEXT = 0,    // EB Garamond 12 Regular: body text
    FACE_ITALIC = 1,  // EB Garamond 12 Italic: captions, descriptions
    FACE_TITLE = 2,   // Cinzel: inscriptional capitals for titles and menu entries
    FACE_SYMBOL = 3,  // FreeSerif subset: chess figures (U+2654..U+265F)
    // Handwriting (scoresheets; assets/fonts/hand/). Loaded lazily: no glyph is baked until used.
    FACE_HAND_CAVEAT = 4,     // Caveat: casual Latin + Cyrillic hand
    FACE_HAND_MARCK = 5,      // Marck Script: cursive Latin + Cyrillic
    FACE_HAND_BADSCRIPT = 6,  // Bad Script: slanted Latin + Cyrillic
    FACE_HAND_ARABIC = 7,     // Arabic handwriting (Ruqaa)
    FACE_HAND_JA = 8,         // Japanese handwriting (kana + kanji)
    FACE_HAND_SC = 9,         // Simplified Chinese handwriting (Kai)
    FACE_HAND_TC = 10,        // Traditional Chinese handwriting (Kai)
    FACE_COUNT
};

// A player's handwriting: the Latin/Cyrillic style is chosen per player; every other script is
// written with the handwriting face of that script (Arabic, kana/kanji, hanzi). Two players can
// have different styles on the same scoresheet.
enum HandStyle : int { HAND_CAVEAT = 0, HAND_MARCK = 1, HAND_BADSCRIPT = 2, HAND_STYLE_COUNT };
const char* handStyleName(int style);   // "Caveat", "Marck Script", ...
// Face that writes 'cp' in the given hand: the style's face when it has the glyph, then the
// script faces, then the UI text face. Never returns a face without the glyph unless none has it.
int handwritingFace(int style, uint32_t cp);
bool isHandwritingFace(int face);

// Glyph quad and metrics. Geometry is in em units relative to the pen position on the baseline
// (y grows downwards); multiply by the font size in pixels.
struct Glyph {
    int index = 0;           // glyph index in its face (0 = missing)
    float advance = 0.0f;    // em
    bool hasQuad = false;    // false for blanks (space)
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // em, quad bounds (includes the SDF padding)
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // atlas texture coordinates
    float pxRange = 0.0f;    // full distance range encoded in the atlas, in atlas texels
};

struct Metrics {
    float ascent = 0.8f, descent = 0.2f, lineGap = 0.0f;  // em (descent positive)
    float capHeight = 0.65f, xHeight = 0.4f;             // em
};

bool init();
void shutdown();
bool ready();

// Resolves a codepoint with fallbacks (Title -> Text -> Symbol, Italic -> Text -> Symbol; a
// handwriting face -> the other handwriting faces -> Text).
// Returns nullptr when no face has it. usedFace receives the face that provided the glyph.
const Glyph* glyph(int face, uint32_t codepoint, int* usedFace = nullptr);
// Kerning between two glyph indices of the same face, in em.
float kerning(int face, int glyphA, int glyphB);
const Metrics& metrics(int face);

GLuint atlasTexture();
// Call once per frame before drawing: uploads glyphs added since the last call.
void flushUploads();

// UTF-8 decoding helper: returns the codepoint at s[i] and advances i (invalid bytes -> U+FFFD).
uint32_t decodeUtf8(const std::string& s, size_t& i);

}  // namespace font
}  // namespace ui
