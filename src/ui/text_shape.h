// Text shaping for the UI and the scoresheets: UTF-8 -> positioned glyphs of one line, in visual
// (left-to-right drawing) order. Handles per-character face fallback, kerning, Arabic contextual
// forms (joining) and bidirectional reordering (right-to-left runs, numbers inside them).
#pragma once
#include "ui_font.h"
#include <functional>
#include <string>
#include <vector>

namespace ui {
namespace text {

struct PlacedGlyph {
    uint32_t cp = 0;                 // codepoint drawn (Arabic: the contextual presentation form)
    int face = 0;                    // face that provides the glyph
    const font::Glyph* glyph = nullptr;  // never null in a Run (missing glyphs are dropped)
    float x = 0.0f;                  // pen position of the glyph origin, em, from the run's left edge
    int source = 0;                  // index of the source character (logical order, 0-based):
                                     // writing order for progressive reveal
};

struct Run {
    std::vector<PlacedGlyph> glyphs; // visual order
    float advance = 0.0f;            // total width, em
    bool rtl = false;                // the line's base direction is right-to-left
    int sourceCount = 0;             // number of source characters
};

// faceFor(cp) picks the face of each character (after contextual shaping); tracking = extra
// letter spacing in em. baseRtl: -1 = from the first strong character, 0 = LTR, 1 = RTL.
Run shapeLine(const std::string& utf8, const std::function<int(uint32_t)>& faceFor, float tracking = 0.0f,
              int baseRtl = -1);
// Convenience: every character in one face (with the face's fallback chain).
Run shapeLine(const std::string& utf8, int face, float tracking = 0.0f, int baseRtl = -1);

}  // namespace text
}  // namespace ui
