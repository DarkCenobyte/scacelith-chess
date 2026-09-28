// Scoresheet layout tests (engine-free part of the scoresheet): printed form, move -> cell mapping,
// localized notation, handwriting placement, pen path, pad placement and page-flip geometry.
#include "test.h"
#include "game/layout.h"
#include "game/scoresheet_layout.h"
#include <cmath>
#include <cstdio>

using namespace game::sheet;
using m::vec2;
using m::vec3;

namespace {

bool inside(const Rect& outer, const Rect& inner, float eps = 1e-4f) {
    return inner.x0 >= outer.x0 - eps && inner.y0 >= outer.y0 - eps && inner.x1 <= outer.x1 + eps && inner.y1 <= outer.y1 + eps;
}
// Shared edges (up to rounding) do not count as overlap.
bool overlap(const Rect& a, const Rect& b, float eps = 1e-3f) {
    return a.x0 < b.x1 - eps && b.x0 < a.x1 - eps && a.y0 < b.y1 - eps && b.y0 < a.y1 - eps;
}

// A synthetic run: 'n' glyphs of 0.5 em advance, ink box 0.05..0.45 em wide, cap height tall.
Run syntheticRun(const std::string& text, bool rtl = false) {
    Run r;
    r.rtl = rtl;
    r.capHeight = 0.65f;
    r.xHeight = 0.45f;
    float pen = 0.0f;
    for (size_t i = 0; i < text.size(); ++i) {
        RunGlyph g;
        g.cp = uint32_t(uint8_t(text[i]));
        g.penX = pen;
        if (text[i] != ' ') {
            bool tall = (text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= '0' && text[i] <= '9') || text[i] == 'i';
            g.x0 = 0.05f;
            g.x1 = 0.45f;
            g.y0 = tall ? -0.68f : -0.45f;
            g.y1 = 0.0f;
        }
        g.source = rtl ? int(text.size() - 1 - i) : int(i);
        r.glyphs.push_back(g);
        pen += 0.5f;
    }
    r.advance = pen;
    return r;
}

}  // namespace

TEST(scoresheet_cells_mapping) {
    CHECK_EQ(moveNumber(0), 1);
    CHECK_EQ(moveNumber(1), 1);
    CHECK_EQ(moveNumber(79), 40);
    CHECK_EQ(moveNumber(80), 41);
    CHECK_EQ(pageOfPly(79), 0);
    CHECK_EQ(pageOfPly(80), 1);
    CHECK_EQ(pageOfPly(159), 1);
    CHECK_EQ(pageOfPly(160), 2);
    Cell c = cellOf(0);
    CHECK(c.page == 0 && c.block == 0 && c.row == 0 && !c.black);
    c = cellOf(39);  // 20... (Black)
    CHECK(c.page == 0 && c.block == 0 && c.row == 19 && c.black);
    c = cellOf(40);  // 21.
    CHECK(c.page == 0 && c.block == 1 && c.row == 0 && !c.black);
    c = cellOf(81);  // 41... on page 2
    CHECK(c.page == 1 && c.block == 0 && c.row == 0 && c.black);

    // Every cell of a page is on the page, cells never overlap, White is left of Black.
    Rect page{0, 0, PAGE_W, PAGE_H};
    std::vector<Rect> cells;
    for (int ply = 0; ply < 2 * MOVES_PER_PAGE; ++ply) {
        Rect r = cellRect(cellOf(ply));
        CHECK(inside(page, r));
        CHECK(r.w() > 20.0f && r.h() > 5.5f);
        WriteBox b = moveBox(ply);
        CHECK(b.x0 > r.x0 && b.x1 < r.x1 && b.baseline > r.y0 && b.baseline < r.y1);
        CHECK(b.capHeight >= 4.0f && b.capHeight <= 5.0f);
        cells.push_back(r);
    }
    for (size_t i = 0; i < cells.size(); ++i)
        for (size_t j = i + 1; j < cells.size(); ++j) CHECK(!overlap(cells[i], cells[j]));
    CHECK(cellRect(cellOf(0)).x1 <= cellRect(cellOf(1)).x0 + 1e-4f);
    // The same row of the next page is at the same place.
    Rect a = cellRect(cellOf(10)), b = cellRect(cellOf(10 + 2 * MOVES_PER_PAGE));
    CHECK(std::fabs(a.x0 - b.x0) < 1e-5f && std::fabs(a.y0 - b.y0) < 1e-5f);
}

TEST(scoresheet_form_and_fields) {
    Rect page{0, 0, PAGE_W, PAGE_H};
    Form f = printedForm(1);
    CHECK(f.rules.size() > 50);
    for (const Rect& r : f.rules) CHECK(inside(page, r));
    int numbers = 0;
    bool has41 = false, has80 = false, has1 = false;
    for (const FormText& t : f.texts) {
        CHECK(!t.text.empty());
        CHECK(t.x > 0.0f && t.x < PAGE_W && t.baseline > HINGE_Y && t.baseline < PAGE_H);
        if (t.text == "41") has41 = true;
        if (t.text == "80") has80 = true;
        if (t.text == "1") has1 = true;
        if (!t.text.empty() && t.text[0] >= '0' && t.text[0] <= '9') ++numbers;
    }
    CHECK_EQ(numbers, MOVES_PER_PAGE);
    CHECK(has41 && has80 && !has1);
    // English labels come from assets/i18n/en.lang.
    bool event = false;
    for (const FormText& t : printedForm(0).texts) event = event || t.text == "Event";
    CHECK(event);
    // Field boxes are on the page, above the table (header) or below it (result), never in the
    // printed area of the move cells.
    for (int i = 0; i < int(Field::Count); ++i) {
        WriteBox b = fieldBox(Field(i));
        CHECK(b.x1 - b.x0 > 12.0f);
        CHECK(b.x0 > 0.0f && b.x1 < PAGE_W);
        CHECK(b.baseline - b.capHeight > HINGE_Y);
        Rect ink{b.x0, b.baseline - b.capHeight, b.x1, b.baseline};
        for (int ply = 0; ply < MOVES_PER_PAGE * 2; ply += 7) CHECK(!overlap(ink, cellRect(cellOf(ply))));
    }
    CHECK(isHeaderField(Field::BlackElo) && !isHeaderField(Field::Page) && !isHeaderField(Field::Result));
}

TEST(scoresheet_localized_san) {
    PieceLetters fr;
    fr.king = "R";
    fr.queen = "D";
    fr.rook = "T";
    fr.bishop = "F";
    fr.knight = "C";
    CHECK_EQ(localizeSan("Nf3", fr), std::string("Cf3"));
    CHECK_EQ(localizeSan("Kxe2+", fr), std::string("Rxe2+"));
    CHECK_EQ(localizeSan("Rae1", fr), std::string("Tae1"));
    CHECK_EQ(localizeSan("Bb5#", fr), std::string("Fb5#"));
    CHECK_EQ(localizeSan("exd8=Q+", fr), std::string("exd8=D+"));
    CHECK_EQ(localizeSan("b8=N", fr), std::string("b8=C"));
    CHECK_EQ(localizeSan("O-O-O", fr), std::string("O-O-O"));
    CHECK_EQ(localizeSan("e4", fr), std::string("e4"));
    CHECK_EQ(localizeSan("Qb6", PieceLetters()), std::string("Qb6"));
    PieceLetters ru;
    ru.king = "\xD0\x9A\xD1\x80";  // Кр
    ru.knight = "\xD0\x9A";        // К
    CHECK_EQ(localizeSan("Nf3", ru), std::string("\xD0\x9A") + "f3");
    CHECK_EQ(localizeSan("Kg1", ru), std::string("\xD0\x9A\xD1\x80") + "g1");
}

TEST(scoresheet_handwriting_placement) {
    WriteBox box = moveBox(0);
    Run run = syntheticRun("Nf3");
    for (uint32_t seed = 1; seed < 40; ++seed) {
        std::vector<GlyphInk> g = placeHandwriting(run, box, seed);
        CHECK_EQ(g.size(), size_t(3));
        Rect ink = inkBounds(g);
        Rect cell = cellRect(cellOf(0));
        // Natural jitter, but inside the cell (a little tolerance for the ascenders).
        CHECK(ink.x0 >= box.x0 - 0.5f && ink.x1 <= box.x1 + 1.0f);
        CHECK(ink.y1 <= cell.y1 + 0.3f && ink.y0 >= cell.y0 - 0.5f);
        // Writing order = left to right, glyphs slightly different from each other.
        CHECK(g[0].origin.x < g[1].origin.x && g[1].origin.x < g[2].origin.x);
        float cap = -g[0].y0 * length(g[0].ay);
        CHECK(cap > 3.4f && cap < 5.0f);
    }
    // Deterministic per seed, different between seeds.
    std::vector<GlyphInk> a = placeHandwriting(run, box, 7), b = placeHandwriting(run, box, 7), c = placeHandwriting(run, box, 8);
    CHECK(a[1].origin == b[1].origin && !(a[1].origin == c[1].origin));
    // Too long for the cell: squeezed into it.
    Run longRun = syntheticRun("Qxf7+Qxf7+Qxf7+");
    Rect ink = inkBounds(placeHandwriting(longRun, box, 3));
    CHECK(ink.x1 <= box.x1 + 1.0f);
    // Right-to-left run: right-aligned, written from the right.
    Run rtl = syntheticRun("abc de", true);
    WriteBox field = fieldBox(Field::WhiteName);
    std::vector<GlyphInk> r = placeHandwriting(rtl, field, 5);
    CHECK_EQ(r.size(), size_t(5));
    CHECK(r[0].origin.x > r[1].origin.x && r[3].origin.x > r[4].origin.x);
    CHECK(inkBounds(r).x1 > field.x1 - 3.0f);
    CHECK(r[0].rtl);
    // The space ends a word (in writing order).
    int wordEnds = 0;
    for (const GlyphInk& gi : r) wordEnds += gi.wordEnd ? 1 : 0;
    CHECK_EQ(wordEnds, 1);
}

TEST(scoresheet_pen_path) {
    WriteBox box = fieldBox(Field::WhiteName);
    Run run = syntheticRun("Hi Stockfish");
    std::vector<GlyphInk> glyphs = placeHandwriting(run, box, 11);
    CHECK_EQ(glyphs.size(), size_t(11));
    PenPath p = buildPenPath(glyphs, 11);
    CHECK(p.keys.size() > 50);
    CHECK(!p.keys.front().down && p.keys.front().lift > 0.0f);
    CHECK(!p.keys.back().down && p.keys.back().lift > 0.0f);
    for (size_t k = 1; k < p.keys.size(); ++k) CHECK(p.keys[k].t > p.keys[k - 1].t);
    CHECK(std::fabs(p.duration - p.keys.back().t) < 1e-5f);
    // About 0.12-0.24 s per Latin glyph, lifts included.
    float perGlyph = p.duration / float(glyphs.size());
    std::fprintf(stderr, "  pen path: %zu keys, %.2f s for %zu glyphs (%.3f s/glyph)\n", p.keys.size(), p.duration,
                 glyphs.size(), perGlyph);
    CHECK(perGlyph > 0.12f && perGlyph < 0.3f);
    // Down keys lie in the ink boxes of their glyph; each glyph is inked by at least one band.
    std::vector<int> inked(glyphs.size(), 0);
    for (const InkBand& b : p.bands) {
        CHECK(b.k0 <= b.k1);
        CHECK(b.glyph >= 0 && b.glyph < int(glyphs.size()));
        inked[size_t(b.glyph)]++;
        const GlyphInk& g = glyphs[size_t(b.glyph)];
        Rect gr = inkBounds({g});
        for (int k = b.k0; k <= b.k1; ++k) {
            const PathKey& key = p.keys[size_t(k)];
            if (key.lift > 0.0f) continue;
            CHECK(gr.contains(key.x, key.y, 0.3f));
        }
    }
    for (int n : inked) CHECK(n >= 1);
    // 'i' has a separate dot band; the space between the words is a longer lift.
    int iBands = 0;
    for (const InkBand& b : p.bands) iBands += b.glyph == 1 ? 1 : 0;
    CHECK_EQ(iBands, 2);
    float maxLift = 0.0f;
    for (const PathKey& k : p.keys)
        if (k.t > 0.1f && k.t < p.duration - 0.1f) maxLift = std::max(maxLift, k.lift);
    CHECK(maxLift >= 1.5f);
    // Reveal: points on the stroke appear when the tip passes, never before the band starts.
    for (const InkBand& b : p.bands) {
        const PathKey& mid = p.keys[size_t((b.k0 + b.k1) / 2)];
        float tr = revealTime(p, b, mid.x, mid.y);
        CHECK(tr >= p.keys[size_t(b.k0)].t - 1e-4f);
        CHECK(tr <= mid.t + 1e-3f);
    }
    // Stroke durations for the writing sound.
    float d = strokeDurationAt(p, p.keys[2].t);
    CHECK(d > 0.05f && d < 0.4f);
    CHECK_EQ(strokeDurationAt(p, 0.0f), 0.0f);
    // RTL writing goes from right to left.
    Run rtl = syntheticRun("abcd", true);
    std::vector<GlyphInk> rg = placeHandwriting(rtl, box, 2);
    PenPath rp = buildPenPath(rg, 2);
    CHECK(rp.keys[1].x > rp.keys[rp.keys.size() - 2].x);
    // Concatenation keeps times increasing.
    PenPath all = p;
    appendPath(all, rp, 0.3f, int(glyphs.size()));
    for (size_t k = 1; k < all.keys.size(); ++k) CHECK(all.keys[k].t > all.keys[k - 1].t);
    CHECK(std::fabs(all.duration - (p.duration + 0.3f + rp.duration)) < 1e-4f);
    CHECK(all.bands.back().glyph >= int(glyphs.size()));
    // CJK characters take longer (three strokes).
    Run cjk;
    cjk.capHeight = 0.65f;
    cjk.xHeight = 0.45f;
    for (int i = 0; i < 3; ++i) {
        RunGlyph g;
        g.cp = 0x674E + uint32_t(i);
        g.penX = float(i);
        g.x0 = 0.05f;
        g.x1 = 0.95f;
        g.y0 = -0.8f;
        g.y1 = 0.1f;
        g.source = i;
        cjk.glyphs.push_back(g);
    }
    cjk.advance = 3.0f;
    PenPath cp = buildPenPath(placeHandwriting(cjk, box, 3), 3);
    CHECK(cp.duration / 3.0f > 0.3f && cp.duration / 3.0f < 0.5f);
    CHECK(isCjk(0x674E) && !isCjk('A') && hasMarkAbove(0xE9) && !hasMarkAbove(0xE7) && hasMarkAbove(0x439));
}

TEST(scoresheet_pad_placement) {
    const float tableX = 0.5f * layout::TABLE_WIDTH, tableZ = 0.5f * layout::TABLE_DEPTH;
    const float boardHalf = 0.5f * layout::BOARD_SIZE;
    for (int clock = 0; clock < 2; ++clock)
        for (int owner = 0; owner < 2; ++owner) {
            PadFrame f = padFrame(owner, clock == 1);
            // Pad on the side without the clock, in front of its owner.
            CHECK(f.center.x * (clock == 1 ? 1.0f : -1.0f) < 0.0f);
            CHECK(f.center.z * (owner == 0 ? 1.0f : -1.0f) > 0.0f);
            // Upright for its owner: page down points towards the owner, right-handed frame.
            CHECK(f.down.z * (owner == 0 ? 1.0f : -1.0f) > 0.99f);
            // (right, up, down) is the pad's (X, Y, Z): orthonormal and not mirrored.
            CHECK(dot(cross(f.right, f.up), f.down) > 0.999f);
            // Corners on the table, clear of the board.
            for (float cx : {0.0f, PAGE_W})
                for (float cy : {0.0f, PAGE_H}) {
                    vec3 w = f.padToWorld(pageToPad(cx, cy, 0.0f));
                    CHECK(std::fabs(w.x) < tableX - 0.02f);
                    CHECK(std::fabs(w.z) < tableZ);
                    CHECK(std::fabs(w.x) > boardHalf + 0.08f);
                }
            // Outer edge = away from the board.
            vec3 outer = f.padToWorld(pageToPad(outerEdgeX(f), PAGE_H * 0.5f, 0.0f));
            vec3 inner = f.padToWorld(pageToPad(PAGE_W - outerEdgeX(f), PAGE_H * 0.5f, 0.0f));
            CHECK(std::fabs(outer.x) > std::fabs(inner.x));
            // Fully turned pages stay in their owner's half of the table.
            for (float cx : {0.0f, PAGE_W}) {
                vec3 p = f.padToWorld(flipPoint(cx, PAGE_H, 1.0f, {6, f.outerSign}));
                CHECK(p.z * (owner == 0 ? 1.0f : -1.0f) > 0.002f);
                CHECK(std::fabs(p.x) > boardHalf + 0.05f);
            }
            // Pen at rest on the table beside the outer edge, lying flat, its tip towards the board side.
            m::mat4 pen = penRestTransform(f);
            vec3 tip = pen.translation(), back = transformPoint(pen, vec3(0, layout::PEN_LENGTH, 0));
            CHECK(std::fabs(tip.x) < tableX - 0.01f && std::fabs(back.x) < tableX - 0.01f);
            CHECK(std::fabs(tip.z) < tableZ && std::fabs(back.z) < tableZ);
            CHECK(std::fabs(tip.x) > std::fabs(outer.x));
            CHECK(tip.y > layout::TABLE_TOP_Y && tip.y < layout::TABLE_TOP_Y + 0.006f);
            CHECK(std::fabs(back.y - tip.y) < 0.002f);
            CHECK(std::fabs(tip.z) < std::fabs(back.z));
        }
}

TEST(scoresheet_page_flip_geometry) {
    FlipParams fp{0, 1.0f};
    // s = 0: flat on the pad.
    for (float x : {0.0f, 50.0f, PAGE_W})
        for (float y : {HINGE_Y, 60.0f, PAGE_H}) {
            vec3 p = flipPoint(x, y, 0.0f, fp), q = pageToPad(x, y, PAD_TOP);
            CHECK(length(p - q) < 1e-5f);
        }
    // s = 1: lying face down beyond the top edge, on the table.
    vec3 far = flipPoint(PAGE_W, PAGE_H, 1.0f, fp);
    CHECK(far.y > 0.0f && far.y < 0.001f);
    CHECK(far.z < -0.5f * PAGE_H * 0.001f - 0.18f);
    // Inextensible along the page, no penetration of the table or the pad, for every s.
    std::vector<float> xs = {0.0f, 37.0f, 74.0f, 111.0f, PAGE_W}, ys;
    for (float y = HINGE_Y; y <= PAGE_H + 1e-3f; y += 0.5f) ys.push_back(y);
    const float padZ0 = -0.5f * PAGE_H * 0.001f, padZ1 = 0.5f * PAGE_H * 0.001f;
    float maxStretch = 0.0f, minTable = 1.0f, minPad = 1.0f;
    for (int is = 0; is <= 40; ++is) {
        float s = float(is) / 40.0f;
        std::vector<vec3> g;
        flipGrid(xs, ys, s, fp, g);
        for (size_t i = 0; i < xs.size(); ++i) {
            float len = 0.0f;
            for (size_t j = 1; j < ys.size(); ++j) len += length(g[j * xs.size() + i] - g[(j - 1) * xs.size() + i]);
            maxStretch = std::max(maxStretch, std::fabs(len * 1000.0f - FLIP_LENGTH) / FLIP_LENGTH);
            for (size_t j = 0; j < ys.size(); ++j) {
                vec3 p = g[j * xs.size() + i];
                minTable = std::min(minTable, p.y);
                if (p.z > padZ0 + 0.0005f && p.z < padZ1) minPad = std::min(minPad, p.y - PAD_TOP * 0.001f);
            }
        }
        // Grid and single-point evaluation agree.
        vec3 a = g.back(), b = flipPoint(PAGE_W, PAGE_H, s, fp);
        CHECK(length(a - b) < 1e-4f);
    }
    std::fprintf(stderr, "  page flip: max stretch %.3f %%, min height above table %.2f mm, above pad %.2f mm\n",
                 maxStretch * 100.0f, minTable * 1000.0f, minPad * 1000.0f);
    CHECK(maxStretch < 0.003f);
    CHECK(minTable > 0.0f);
    CHECK(minPad > -0.0003f);
    // The pinched corner (outer bottom) leads: it is higher than the other bottom corner mid-turn,
    // and its path is continuous.
    vec3 lead = flipPoint(PAGE_W, PAGE_H, 0.3f, fp), lag = flipPoint(0.0f, PAGE_H, 0.3f, fp);
    CHECK(lead.y > lag.y);
    vec3 prev = flipPoint(PAGE_W, PAGE_H, 0.0f, fp);
    for (int is = 1; is <= 200; ++is) {
        vec3 p = flipPoint(PAGE_W, PAGE_H, float(is) / 200.0f, fp);
        CHECK(length(p - prev) < 0.012f);
        prev = p;
    }
    // A page turned over others lies on top of them.
    vec3 k0 = flipPoint(70.0f, 150.0f, 1.0f, {0, 1.0f}), k3 = flipPoint(70.0f, 150.0f, 1.0f, {3, 1.0f});
    CHECK(k3.y > k0.y + 0.0002f);
}
