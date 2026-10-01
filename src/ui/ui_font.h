// UI fonts: embedded TTF faces rasterised into a single-channel signed distance field atlas at
// ui::init(). Glyphs outside the pre-built set are added lazily. Internal to src/ui.
//
// Scripts: EB Garamond / Cinzel cover Latin (Garamond also Cyrillic and Greek); Arabic comes from
// Amiri (UI) or Aref Ruqaa (handwriting) through the presentation forms produced by the shaper;
// kana, hanzi and kanji from Klee One (Japanese) and LXGW WenKai (Simplified / Traditional
// Chinese), in the order preferred by the current language (Han unification: the same codepoint
// is drawn with Japanese, Simplified or Traditional glyph shapes).
//
// Atlas: one 4096^2 R8 texture shared by every face. Each glyph's distance field is kept in memory
// once built; when the atlas fills up (a session that switches between several CJK languages) it
// is cleared at the start of the next frame and glyphs are packed again as they are used.
// atlasGeneration() changes on every clear: code that caches Glyph texture coordinates across
// frames must look its glyphs up again (text::shapeLine) when it changes.
#pragma once
#include "../gl/gl46.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
    FACE_HAND_JA = 8,         // Japanese handwriting (kana + kanji); also the Japanese UI face
    FACE_HAND_SC = 9,         // Simplified Chinese handwriting (Kai); also the Simplified UI face
    FACE_HAND_TC = 10,        // Traditional Chinese handwriting (Kai); also the Traditional UI face
    FACE_ARABIC = 11,         // Amiri: Arabic UI text (fallback of the UI faces for Arabic)
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
    bool hasQuad = false;    // false for blanks (space) and while not packed in the atlas
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
// Call once per frame before any text is laid out (ui::beginFrame does): clears a full atlas and
// builds the glyphs of a newly selected language (i18n) in parallel.
void beginFrame();

// Resolves a codepoint with fallbacks: the face itself, then by script (Arabic -> Amiri / Ruqaa,
// kana and Han -> Klee / WenKai in the language's order), then Text -> Symbol; a handwriting face
// tries the other handwriting faces first. Returns nullptr when no face has it. usedFace receives
// the face that provided the glyph.
const Glyph* glyph(int face, uint32_t codepoint, int* usedFace = nullptr);
// Face that glyph() would use, without building the glyph (-1 when none has it).
int resolveFace(int face, uint32_t codepoint);
// Size factor for a glyph drawn by a fallback face so scripts look balanced next to the
// requested UI face (1 for the face itself and for handwriting).
float fallbackScale(int requestedFace, int usedFace);
// Kerning between two glyph indices of the same face, in em.
float kerning(int face, int glyphA, int glyphB);
const Metrics& metrics(int face);

GLuint atlasTexture();
int atlasGeneration();
// Builds the given (face, codepoint) glyphs now, on worker threads (e.g. a language's UI strings).
void prewarm(const std::vector<std::pair<int, uint32_t>>& glyphs);
// Call once per frame before drawing: uploads glyphs added since the last call.
void flushUploads();

// UTF-8 decoding helper: returns the codepoint at s[i] and advances i (invalid bytes -> U+FFFD).
uint32_t decodeUtf8(const std::string& s, size_t& i);

// Renders one line of text in 'face' into a standalone single-channel distance field, independent
// of the atlas (for markings baked into 3D material textures). No shaping: left to right, with the
// face's kerning plus 'tracking' (em) between letters. out = w*h bytes, row 0 = top, value
// 0.5 + d / (2 * spread) with d the signed distance in texels (inside > 0). The ink is centred
// horizontally and the cap band (baseline to cap line, capPx texels high) vertically. Needs no GL
// context and works before init(). Returns false when the face is missing or the ink plus the
// spread does not fit; inkWidthPx receives the ink width in texels.
bool renderLineSdf(int face, const std::string& utf8, float capPx, int spread, float tracking, int w, int h,
                   std::vector<uint8_t>& out, float* inkWidthPx = nullptr);

}  // namespace font
}  // namespace ui
