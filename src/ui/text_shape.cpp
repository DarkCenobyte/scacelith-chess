#include "text_shape.h"
#include <algorithm>

namespace ui {
namespace text {

// Minimal left-to-right implementation (no Arabic joining, no bidi yet).
Run shapeLine(const std::string& utf8, const std::function<int(uint32_t)>& faceFor, float tracking, int baseRtl) {
    Run run;
    run.rtl = baseRtl == 1;
    float pen = 0.0f;
    int prevIndex = 0, prevFace = -1;
    int src = 0;
    for (size_t i = 0; i < utf8.size();) {
        uint32_t cp = font::decodeUtf8(utf8, i);
        int want = faceFor ? faceFor(cp) : int(font::FACE_TEXT);
        int used = want;
        const font::Glyph* g = font::glyph(want, cp, &used);
        if (g) {
            if (used == prevFace) pen += font::kerning(used, prevIndex, g->index);
            PlacedGlyph pg;
            pg.cp = cp;
            pg.face = used;
            pg.glyph = g;
            pg.x = pen;
            pg.source = src;
            run.glyphs.push_back(pg);
            pen += g->advance + tracking;
            prevIndex = g->index;
            prevFace = used;
        }
        ++src;
    }
    run.advance = std::max(0.0f, pen - (run.glyphs.empty() ? 0.0f : tracking));
    run.sourceCount = src;
    return run;
}

Run shapeLine(const std::string& utf8, int face, float tracking, int baseRtl) {
    return shapeLine(utf8, [face](uint32_t) { return face; }, tracking, baseRtl);
}

}  // namespace text
}  // namespace ui
