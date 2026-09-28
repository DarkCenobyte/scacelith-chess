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
    FACE_COUNT
};

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

// Resolves a codepoint with fallbacks (Title -> Text -> Symbol, Italic -> Text -> Symbol).
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
