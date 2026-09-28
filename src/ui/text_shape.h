// Text shaping for the UI and the scoresheets: UTF-8 -> positioned glyphs of one line, in visual
// (left-to-right drawing) order. Handles per-character face fallback, kerning, Arabic contextual
// forms (joining, lam-alef ligatures, transparent marks) and bidirectional reordering
// (right-to-left runs, numbers inside them, mirrored brackets). The pure codepoint logic lives in
// src/i18n/unicode.h (unit-tested).
//
// Glyph pointers stay valid until ui::shutdown(); their atlas coordinates are valid until
// font::atlasGeneration() changes (shape again when it does if you keep a Run across frames).
#pragma once
#include "ui_font.h"
#include <functional>
#include <string>
#include <vector>

namespace ui {
namespace text {

struct PlacedGlyph {
    uint32_t cp = 0;                 // codepoint drawn (Arabic: the contextual presentation form;
                                     // mirrored bracket in right-to-left runs)
    int face = 0;                    // face that provides the glyph
    const font::Glyph* glyph = nullptr;  // never null in a Run (missing glyphs are dropped)
    float x = 0.0f;                  // pen position of the glyph origin, em, from the run's left edge
    int source = 0;                  // index of the source character (logical order, 0-based):
                                     // writing order for progressive reveal
    // Extensions (defaults keep the original contract):
    float scale = 1.0f;              // glyph size relative to the font size (fallback faces sized
                                     // to match, synthesised small capitals): multiply the quad
    float advance = 0.0f;            // em, including scale (0 for combining marks)
    bool rtl = false;                // drawn inside a right-to-left run
    bool mark = false;               // combining mark positioned over its base (zero advance)
};

struct Run {
    std::vector<PlacedGlyph> glyphs; // visual order
    float advance = 0.0f;            // total width, em
    bool rtl = false;                // the line's base direction is right-to-left
    int sourceCount = 0;             // number of source characters
};

// faceFor(cp) picks the face of each character (after contextual shaping); tracking = extra
// letter spacing in em (never applied inside Arabic words, which are joined). baseRtl: -1 = from
// the first strong character, 0 = LTR, 1 = RTL.
Run shapeLine(const std::string& utf8, const std::function<int(uint32_t)>& faceFor, float tracking = 0.0f,
              int baseRtl = -1);
// Convenience: every character in one face (with the face's fallback chain). A UI face drawing
// Cyrillic or Greek through the Cinzel fallback gets synthesised small capitals.
Run shapeLine(const std::string& utf8, int face, float tracking = 0.0f, int baseRtl = -1);
// Handwriting of a player (font::HandStyle): every character in font::handwritingFace(style, cp).
Run shapeHandwriting(const std::string& utf8, int handStyle, float tracking = 0.0f, int baseRtl = -1);

// Caret position (em from the run's left edge) before the logical character 'index'
// (0..sourceCount), for text fields.
float caretX(const Run& run, int index);
// Logical caret index closest to x (em), for mouse placement.
int caretIndex(const Run& run, float x);

}  // namespace text
}  // namespace ui
