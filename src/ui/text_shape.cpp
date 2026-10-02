#include "text_shape.h"
#include "../i18n/unicode.h"
#include <algorithm>
#include <cmath>

namespace ui {
namespace text {
namespace {

// Characters that take part in the logic but are never drawn (joiners, direction marks, line
// breaks, soft hyphen, BOM, explicit bidi controls, and the other default ignorable code points:
// variation selectors, tags, fillers, format controls). No face has a glyph for them: drawn,
// they would show as '?' (an emoji picked with its variation selector: "♟?").
bool invisible(char32_t c) {
    return c == '\n' || c == '\r' || c == 0xAD || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2060 && c <= 0x206F) || c == 0xFEFF || c < 0x20 || c == 0x34F || c == 0x61C || c == 0x115F ||
           c == 0x1160 || c == 0x17B4 || c == 0x17B5 || (c >= 0x180B && c <= 0x180F) || c == 0x2028 || c == 0x2029 ||
           c == 0x3164 || (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFFA0 || (c >= 0x1BCA0 && c <= 0x1BCA3) ||
           (c >= 0x1D173 && c <= 0x1D17A) || (c >= 0xE0000 && c <= 0xE0FFF);
}

bool arabicLetter(char32_t c) { return uni::isArabic(c) && !uni::isMark(c); }

}  // namespace

Run shapeLine(const std::string& utf8, const std::function<int(uint32_t)>& faceFor, float tracking, int baseRtl) {
    Run run;
    std::u32string logical = uni::decode(utf8);
    run.sourceCount = int(logical.size());
    if (logical.empty()) {
        run.rtl = baseRtl == 1;
        return run;
    }
    uni::Shaped sh = uni::shapeArabic(logical);
    int base = baseRtl >= 0 ? (baseRtl & 1) : uni::paragraphLevel(sh.text, 0);
    run.rtl = base == 1;
    std::vector<int> levels = uni::resolveLevels(sh.text, base);
    std::vector<int> order = uni::visualOrder(levels);

    const float titleCap = font::metrics(font::FACE_TITLE).capHeight;
    const float titleSmall = font::metrics(font::FACE_TITLE).xHeight;
    float pen = 0.0f;
    int prevFace = -1, prevIndex = 0;
    char32_t prevCp = 0;
    bool first = true, anyMark = false;
    run.glyphs.reserve(order.size());
    for (int k : order) {
        char32_t cp = sh.text[size_t(k)];
        if (invisible(cp)) continue;
        bool isRtl = (levels[size_t(k)] & 1) != 0;
        if (isRtl) cp = uni::mirror(cp);
        bool mark = uni::isMark(cp);
        int want = faceFor ? faceFor(uint32_t(cp)) : int(font::FACE_TEXT);
        int used = want;
        const font::Glyph* g = font::glyph(want, uint32_t(cp), &used);
        float scale = font::fallbackScale(want, used);
        if (g && want == font::FACE_TITLE && used != font::FACE_TITLE && !uni::isArabic(cp) && !uni::isCjk(cp)) {
            // Cinzel has no Cyrillic: inscriptional capitals from the text face instead, lowercase as
            // small capitals, sized on Cinzel's capitals.
            const float cap = font::metrics(used).capHeight;
            if (uni::isLower(cp) && cp != 0xDF) {
                char32_t up = uni::toUpper(cp);
                int u2 = used;
                if (const font::Glyph* g2 = font::glyph(want, uint32_t(up), &u2)) {
                    g = g2;
                    cp = up;
                    used = u2;
                    scale = cap > 0.0f ? titleSmall / font::metrics(used).capHeight : 0.8f;
                }
            } else if (cap > 0.0f && titleCap > 0.0f) {
                scale = titleCap / cap;
            }
        }
        if (!g) {
            g = font::glyph(want, '?', &used);
            if (!g) continue;
            scale = 1.0f;
        }
        PlacedGlyph pg;
        pg.cp = uint32_t(cp);
        pg.face = used;
        pg.glyph = g;
        pg.source = sh.source[size_t(k)];
        pg.scale = scale;
        pg.rtl = isRtl;
        pg.mark = mark;
        if (mark) {
            pg.advance = 0.0f;
            pg.x = pen;  // positioned over its base below
            run.glyphs.push_back(pg);
            anyMark = true;
            continue;
        }
        bool arabicPair = arabicLetter(cp) && arabicLetter(prevCp);
        if (!first && !arabicPair) pen += tracking * uni::trackingScale(prevCp, cp);
        if (used == prevFace && !arabicPair) pen += font::kerning(used, prevIndex, g->index) * scale;
        pg.x = pen;
        pg.advance = g->advance * scale;
        run.glyphs.push_back(pg);
        pen += pg.advance;
        prevIndex = g->index;
        prevFace = used;
        prevCp = cp;
        first = false;
    }
    // Combining marks: centred over the ink of their base (the closest preceding character in
    // logical order), wherever the font draws them relative to its own origin. before[s] = the
    // first glyph (visual order) of the greatest non-mark source below s, -1 when none.
    std::vector<int> before;
    if (anyMark) {
        before.assign(size_t(run.sourceCount), -1);
        for (size_t i = 0; i < run.glyphs.size(); ++i) {
            int& at = before[size_t(run.glyphs[i].source)];
            if (!run.glyphs[i].mark && at < 0) at = int(i);
        }
        int last = -1;
        for (int& b : before) {
            int at = b;
            b = last;
            if (at >= 0) last = at;
        }
    }
    for (PlacedGlyph& m : run.glyphs) {
        if (!m.mark) continue;
        int b = before[size_t(m.source)];
        if (b < 0) continue;
        const PlacedGlyph* base = &run.glyphs[size_t(b)];
        float bc = base->glyph->hasQuad || base->glyph->x1 > base->glyph->x0
                       ? base->x + 0.5f * (base->glyph->x0 + base->glyph->x1) * base->scale
                       : base->x + 0.5f * base->advance;
        float mc = 0.5f * (m.glyph->x0 + m.glyph->x1) * m.scale;
        m.x = bc - mc;
    }
    run.advance = std::max(0.0f, pen);
    return run;
}

Run shapeLine(const std::string& utf8, int face, float tracking, int baseRtl) {
    return shapeLine(utf8, [face](uint32_t) { return face; }, tracking, baseRtl);
}

Run shapeHandwriting(const std::string& utf8, int handStyle, float tracking, int baseRtl) {
    return shapeLine(utf8, [handStyle](uint32_t cp) { return font::handwritingFace(handStyle, cp); }, tracking, baseRtl);
}

float caretX(const Run& run, int index) {
    const PlacedGlyph* at = nullptr;
    const PlacedGlyph* before = nullptr;
    for (const PlacedGlyph& g : run.glyphs) {
        if (g.mark) continue;
        if (g.source == index && !at) at = &g;
        if (g.source < index && (!before || g.source > before->source)) before = &g;
    }
    if (at) return at->rtl ? at->x + at->advance : at->x;
    if (before) return before->rtl ? before->x : before->x + before->advance;
    return run.rtl ? run.advance : 0.0f;
}

int caretIndex(const Run& run, float x) {
    int best = 0;
    float bestD = 1e30f;
    for (int i = 0; i <= run.sourceCount; ++i) {
        float d = std::fabs(caretX(run, i) - x);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

}  // namespace text
}  // namespace ui
