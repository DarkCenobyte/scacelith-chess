#include "scoresheet_layout.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include <algorithm>
#include <cmath>

using namespace m;

namespace game {
namespace sheet {

namespace {

// Move table (mm).
constexpr float TABLE_Y0 = 53.0f;     // top border
constexpr float HEAD_H = 5.0f;        // column titles
constexpr float ROW_H = 6.4f;
constexpr float BLOCK_X0 = 8.0f, BLOCK_GAP = 4.0f;
constexpr float NO_W = 8.5f, MOVE_W = 27.75f;
constexpr float BLOCK_W = NO_W + 2.0f * MOVE_W;  // 64 mm
constexpr float ROWS_Y0 = TABLE_Y0 + HEAD_H;
constexpr float TABLE_Y1 = ROWS_Y0 + ROWS * ROW_H;

// Printed line weights (mm).
constexpr float RULE_BOLD = 0.30f, RULE_MID = 0.20f, RULE_THIN = 0.12f;

struct FieldSpec {
    const char* labelKey;
    float labelX, baseline, lineX0, lineX1, cap;
};
const FieldSpec kFields[int(Field::Count)] = {
    {"scoresheet.event", 8.0f, 26.0f, 21.0f, 95.0f, 3.7f},
    {"scoresheet.date", 98.0f, 26.0f, 109.0f, 140.0f, 3.4f},
    {"scoresheet.round", 8.0f, 33.5f, 21.0f, 48.0f, 3.6f},
    {"scoresheet.board", 52.0f, 33.5f, 65.0f, 95.0f, 3.6f},
    {"scoresheet.white", 8.0f, 41.0f, 21.0f, 113.0f, 3.8f},
    {"scoresheet.elo", 116.0f, 41.0f, 124.0f, 140.0f, 3.4f},
    {"scoresheet.black", 8.0f, 48.5f, 21.0f, 113.0f, 3.8f},
    {"scoresheet.elo", 116.0f, 48.5f, 124.0f, 140.0f, 3.4f},
    {"scoresheet.page", 111.0f, 15.5f, 121.0f, 140.0f, 3.4f},
    {"scoresheet.result", 8.0f, 193.5f, 23.0f, 62.0f, 3.9f},
    {nullptr, 98.0f, 33.5f, 98.0f, 140.0f, 3.2f},    // Note: the free end of the Round / Board row
    {nullptr, 96.0f, 193.5f, 96.0f, 140.0f, 2.5f},   // Reference: beside the result, small
};
constexpr float LINE_BELOW_BASELINE = 0.7f;  // the writing line sits just under the label baseline

float blockX0(int block) { return BLOCK_X0 + float(block) * (BLOCK_W + BLOCK_GAP); }

}  // namespace

// ---- Form ---------------------------------------------------------------------------------------
Form printedForm(int page) {
    Form f;
    auto hline = [&](float x0, float x1, float y, float t) { f.rules.push_back({x0, y - 0.5f * t, x1, y + 0.5f * t}); };
    auto vline = [&](float x, float y0, float y1, float t) { f.rules.push_back({x - 0.5f * t, y0, x + 0.5f * t, y1}); };
    auto box = [&](float x0, float y0, float x1, float y1, float t) {
        hline(x0 - 0.5f * t, x1 + 0.5f * t, y0, t);
        hline(x0 - 0.5f * t, x1 + 0.5f * t, y1, t);
        vline(x0, y0, y1, t);
        vline(x1, y0, y1, t);
    };
    auto text = [&](const std::string& s, int face, float x, float baseline, float cap, int align, float maxWidth = 0.0f) {
        FormText t;
        t.text = s;
        t.face = face;
        t.x = x;
        t.baseline = baseline;
        t.capHeight = cap;
        t.align = align;
        t.maxWidth = maxWidth;
        f.texts.push_back(t);
    };

    // Title: a knight figure and the form's name, double rule underneath.
    std::string knight;
    uni::append(knight, 0x265E);
    text(knight, PRINT_SYMBOL, BLOCK_X0, 16.1f, 4.4f, 0);
    // Labels fit their room in every language: the title ends before "Page", a field label before
    // its writing line, column titles inside their column.
    const float titleX = BLOCK_X0 + 6.3f;
    text(i18n::tr("scoresheet.title"), PRINT_TITLE, titleX, 15.5f, 3.3f, 0,
         kFields[int(Field::Page)].labelX - titleX - 3.0f);
    hline(BLOCK_X0, PAGE_W - BLOCK_X0, 18.7f, 0.35f);
    hline(BLOCK_X0, PAGE_W - BLOCK_X0, 19.45f, RULE_THIN);

    // Header fields: printed label and the line to write on.
    for (int i = 0; i < int(Field::Count); ++i) {
        const FieldSpec& s = kFields[i];
        if (!s.labelKey) continue;  // unlabeled additions: no printed line either
        text(i18n::tr(s.labelKey), PRINT_TEXT, s.labelX, s.baseline, 2.05f, 0, s.lineX0 - s.labelX - 1.0f);
        hline(s.lineX0, s.lineX1, s.baseline + LINE_BELOW_BASELINE, RULE_THIN);
    }

    // Move table: two blocks of 20 rows, number | White | Black.
    for (int b = 0; b < 2; ++b) {
        float x0 = blockX0(b), x1 = x0 + BLOCK_W;
        box(x0, TABLE_Y0, x1, TABLE_Y1, RULE_BOLD);
        hline(x0, x1, ROWS_Y0, 0.25f);
        vline(x0 + NO_W, TABLE_Y0, TABLE_Y1, 0.15f);
        vline(x0 + NO_W + MOVE_W, TABLE_Y0, TABLE_Y1, 0.15f);
        for (int r = 1; r < ROWS; ++r) hline(x0, x1, ROWS_Y0 + float(r) * ROW_H, r % 5 == 0 ? RULE_MID : RULE_THIN);
        float hb = TABLE_Y0 + HEAD_H - 1.45f;
        text(i18n::tr("scoresheet.move_no"), PRINT_ITALIC, x0 + 0.5f * NO_W, hb, 1.6f, 1, NO_W - 1.2f);
        text(i18n::tr("scoresheet.white"), PRINT_TEXT, x0 + NO_W + 0.5f * MOVE_W, hb, 2.0f, 1, MOVE_W - 2.0f);
        text(i18n::tr("scoresheet.black"), PRINT_TEXT, x0 + NO_W + 1.5f * MOVE_W, hb, 2.0f, 1, MOVE_W - 2.0f);
        for (int r = 0; r < ROWS; ++r) {
            int n = page * MOVES_PER_PAGE + b * ROWS + r + 1;
            text(std::to_string(n), PRINT_TEXT, x0 + 0.5f * NO_W, ROWS_Y0 + float(r + 1) * ROW_H - 2.05f, 2.05f, 1);
        }
    }

    // Signature boxes.
    const float sy0 = 197.0f, sy1 = 207.5f;
    box(blockX0(0), sy0, blockX0(0) + BLOCK_W, sy1, RULE_MID);
    box(blockX0(1), sy0, blockX0(1) + BLOCK_W, sy1, RULE_MID);
    text(i18n::tr("scoresheet.sign_white"), PRINT_ITALIC, blockX0(0) + 1.5f, sy0 + 2.9f, 1.6f, 0, BLOCK_W - 3.0f);
    text(i18n::tr("scoresheet.sign_black"), PRINT_ITALIC, blockX0(1) + 1.5f, sy0 + 2.9f, 1.6f, 0, BLOCK_W - 3.0f);
    return f;
}

// ---- Fields and cells -------------------------------------------------------------------------
const char* fieldName(Field f) {
    static const char* names[int(Field::Count)] = {"event", "date", "round", "board", "white", "white_elo",
                                                   "black", "black_elo", "page", "result", "note", "reference"};
    int i = int(f);
    return i >= 0 && i < int(Field::Count) ? names[i] : "?";
}

bool isHeaderField(Field f) { return int(f) <= int(Field::BlackElo) || f == Field::Note || f == Field::Reference; }

WriteBox fieldBox(Field f) {
    const FieldSpec& s = kFields[std::clamp(int(f), 0, int(Field::Count) - 1)];
    WriteBox b;
    b.x0 = s.lineX0 + 1.2f;
    b.x1 = s.lineX1 - 0.5f;
    b.baseline = s.baseline + LINE_BELOW_BASELINE - 0.55f;
    b.capHeight = s.cap;
    return b;
}

Cell cellOf(int ply) {
    Cell c;
    int n = moveNumber(std::max(ply, 0)) - 1;
    c.page = n / MOVES_PER_PAGE;
    int i = n % MOVES_PER_PAGE;
    c.block = i / ROWS;
    c.row = i % ROWS;
    c.black = (ply & 1) != 0;
    return c;
}

Rect cellRect(const Cell& c) {
    Rect r;
    r.x0 = blockX0(c.block) + NO_W + (c.black ? MOVE_W : 0.0f);
    r.x1 = r.x0 + MOVE_W;
    r.y0 = ROWS_Y0 + float(c.row) * ROW_H;
    r.y1 = r.y0 + ROW_H;
    return r;
}

Rect numberCellRect(int block, int row) {
    Rect r;
    r.x0 = blockX0(block);
    r.x1 = r.x0 + NO_W;
    r.y0 = ROWS_Y0 + float(row) * ROW_H;
    r.y1 = r.y0 + ROW_H;
    return r;
}

WriteBox moveBox(int ply) {
    Rect r = cellRect(cellOf(ply));
    WriteBox b;
    b.x0 = r.x0 + 1.4f;
    b.x1 = r.x1 - 0.8f;
    b.baseline = r.y1 - 1.3f;
    b.capHeight = 4.1f;
    return b;
}

std::string localizeSan(const std::string& san, const PieceLetters& L) {
    auto letter = [&](char c) -> const std::string* {
        switch (c) {
        case 'K': return &L.king;
        case 'Q': return &L.queen;
        case 'R': return &L.rook;
        case 'B': return &L.bishop;
        case 'N': return &L.knight;
        default: return nullptr;
        }
    };
    std::string out;
    out.reserve(san.size() + 4);
    for (size_t i = 0; i < san.size(); ++i) {
        char c = san[i];
        bool pieceSlot = i == 0 || (i > 0 && san[i - 1] == '=');
        const std::string* rep = pieceSlot ? letter(c) : nullptr;
        if (rep) out += *rep;
        else out += c;
    }
    return out;
}

// ---- Handwriting placement ------------------------------------------------------------------------
namespace {
float rnd(Rng& r, float a, float b) { return r.range(a, b); }
vec2 rot2(vec2 v, float a) {
    float c = std::cos(a), s = std::sin(a);
    return vec2(v.x * c - v.y * s, v.x * s + v.y * c);
}
}  // namespace

std::vector<GlyphInk> placeHandwriting(const Run& run, const WriteBox& box, uint32_t seed) {
    std::vector<GlyphInk> out;
    if (run.glyphs.empty()) return out;
    Rng rng(uint64_t(seed) * 2654435761u + 17u);
    const float emMm = box.capHeight / std::max(run.capHeight, 0.2f);
    // Entry-level character of the hand.
    float scale = 1.0f + rnd(rng, -0.05f, 0.05f);
    float slope = rnd(rng, -1.6f, 1.0f) * DEG;          // baseline angle (y down: > 0 descends)
    float slant = rnd(rng, -2.0f, 5.0f) * DEG;          // lean of the letters (> 0 leans right)
    float inset = rnd(rng, 0.0f, 0.9f);
    float baseOff = rnd(rng, -0.35f, 0.25f);
    float natural = run.advance * emMm * scale;
    float avail = std::max(1.0f, box.x1 - box.x0 - inset);
    float squeeze = natural > avail ? std::max(0.55f, avail / natural) : 1.0f;
    float advScale = scale * squeeze;
    float sizeScale = scale * std::sqrt(squeeze);
    float width = run.advance * emMm * advScale;
    float startX = run.rtl ? box.x1 - inset - width : box.x0 + inset;
    float lineAngle = run.rtl ? -slope : slope;

    // Writing order.
    std::vector<int> order(run.glyphs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return run.glyphs[size_t(a)].source < run.glyphs[size_t(b)].source; });

    float drift = 0.0f;
    for (int idx : order) {
        const RunGlyph& g = run.glyphs[size_t(idx)];
        bool blank = !(g.x1 > g.x0 && g.y1 > g.y0);
        if (blank) {
            if (!out.empty()) out.back().wordEnd = true;
            continue;
        }
        drift = clamp(drift + rnd(rng, -0.07f, 0.07f), -0.35f, 0.35f);
        float cx = (g.penX + 0.5f * (g.x0 + g.x1)) * emMm * advScale;  // from the run's left edge
        float along = run.rtl ? width - cx : cx;                        // writing distance
        GlyphInk gi;
        gi.cp = g.cp;
        gi.runIndex = idx;
        gi.rtl = run.rtl;
        gi.x0 = g.x0 * emMm;
        gi.x1 = g.x1 * emMm;
        gi.y0 = g.y0 * emMm;
        gi.y1 = g.y1 * emMm;
        gi.xHeight = run.xHeight * emMm;
        gi.capHeight = run.capHeight * emMm;
        float gs = sizeScale * (1.0f + rnd(rng, -0.05f, 0.05f));
        float rot = lineAngle + rnd(rng, -2.5f, 2.5f) * DEG;
        float sl = slant + rnd(rng, -1.5f, 1.5f) * DEG;
        gi.ax = rot2(vec2(gs, 0.0f), rot);
        gi.ay = rot2(vec2(-std::tan(sl) * gs, gs), rot);
        gi.origin.x = startX + g.penX * emMm * advScale + rnd(rng, -0.12f, 0.12f);
        gi.origin.y = box.baseline + baseOff + std::tan(slope) * along + drift + rnd(rng, -0.15f, 0.15f);
        out.push_back(gi);
    }
    return out;
}

Rect inkBounds(const std::vector<GlyphInk>& glyphs) {
    Rect r{1e9f, 1e9f, -1e9f, -1e9f};
    for (const GlyphInk& g : glyphs) {
        const vec2 c[4] = {g.toPage(g.x0, g.y0), g.toPage(g.x1, g.y0), g.toPage(g.x0, g.y1), g.toPage(g.x1, g.y1)};
        for (vec2 p : c) {
            r.x0 = std::min(r.x0, p.x);
            r.y0 = std::min(r.y0, p.y);
            r.x1 = std::max(r.x1, p.x);
            r.y1 = std::max(r.y1, p.y);
        }
    }
    return r;
}

// ---- Pen path ---------------------------------------------------------------------------------------
bool isCjk(uint32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x20000 && cp <= 0x3134F) ||
           (cp >= 0xFF66 && cp <= 0xFF9F);
}

bool hasMarkAbove(uint32_t cp) {
    if (cp == 'i' || cp == 'j') return true;
    if (cp >= 0xC0 && cp <= 0xFF) {
        // Latin-1 letters with a diacritic above (not ç Ç ð Ð ø Ø æ Æ ß þ Þ × ÷).
        switch (cp) {
        case 0xC6: case 0xC7: case 0xD0: case 0xD7: case 0xD8: case 0xDE: case 0xDF:
        case 0xE6: case 0xE7: case 0xF0: case 0xF7: case 0xF8: case 0xFE: return false;
        default: return true;
        }
    }
    if (cp >= 0x100 && cp <= 0x17F) {
        // Latin Extended-A: most letters carry a mark above; the ones below (cedilla, ogonek) and
        // the plain ones are excluded.
        switch (cp) {
        case 0x104: case 0x105: case 0x118: case 0x119: case 0x122: case 0x123: case 0x12E: case 0x12F:
        case 0x131: case 0x136: case 0x137: case 0x138: case 0x13B: case 0x13C: case 0x141: case 0x142:
        case 0x145: case 0x146: case 0x14A: case 0x14B: case 0x152: case 0x153: case 0x156: case 0x157:
        case 0x15E: case 0x15F: case 0x162: case 0x163: case 0x166: case 0x167: case 0x172: case 0x173: return false;
        default: return true;
        }
    }
    switch (cp) {  // Cyrillic й Й ё Ё ї Ї і І ў Ў
    case 0x419: case 0x439: case 0x401: case 0x451: case 0x407: case 0x457: case 0x406: case 0x456:
    case 0x40E: case 0x45E: return true;
    default: return false;
    }
}

namespace {

bool isUpperWithMark(uint32_t cp) {
    if (cp >= 0xC0 && cp <= 0xDE) return true;
    if (cp >= 0x100 && cp <= 0x17F) return (cp & 1u) == 0u;
    return cp == 0x419 || cp == 0x401 || cp == 0x407 || cp == 0x406 || cp == 0x40E;
}

// One pen-down stroke in glyph-local mm.
struct Stroke {
    std::vector<vec2> pts;
    float duration = 0.1f;
    int band = 0;
};
struct Plan {
    std::vector<Stroke> strokes;
    std::vector<std::pair<float, float>> bands;  // local y ranges
};

constexpr float SAMPLE_HZ = 120.0f;
constexpr float BIG = 1e4f;

int samplesFor(float duration) { return std::max(3, int(std::ceil(duration * SAMPLE_HZ))); }

// Quick oscillating sweep through a box (top first), n down/up cycles, in the writing direction.
Stroke sweep(float bx0, float by0, float bx1, float by1, float dir, float duration, int band, float loop = 0.10f) {
    float w = bx1 - bx0, h = by1 - by0;
    float ix = std::min(0.12f * w, 0.25f), iy = std::min(0.08f * h, 0.2f);
    bx0 += ix;
    bx1 -= ix;
    by0 += iy;
    by1 -= iy;
    w = std::max(bx1 - bx0, 0.05f);
    h = std::max(by1 - by0, 0.05f);
    int n = std::clamp(int(std::lround(w / (0.45f * h))), 1, 4);
    Stroke s;
    s.duration = duration;
    s.band = band;
    int N = samplesFor(duration);
    float xs = dir > 0 ? bx0 : bx1;
    for (int k = 0; k <= N; ++k) {
        float tau = float(k) / float(N);
        float ph = TAU * float(n) * tau;
        float y = by0 + h * (0.5f - 0.5f * std::cos(ph));
        float x = xs + dir * (w * tau + loop * (w / float(n)) * std::sin(ph));
        s.pts.push_back(vec2(x, y));
    }
    return s;
}

Stroke line(vec2 a, vec2 b, float duration, int band) {
    Stroke s;
    s.duration = duration;
    s.band = band;
    int N = samplesFor(duration);
    for (int k = 0; k <= N; ++k) {
        float t = float(k) / float(N);
        float e = t * t * (3.0f - 2.0f * t);  // accelerate / decelerate
        s.pts.push_back(lerp(a, b, e));
    }
    return s;
}

Stroke tap(vec2 c, float size, int band) {
    Stroke s;
    s.duration = 0.045f;
    s.band = band;
    s.pts = {c + vec2(-0.35f, 0.25f) * size, c, c + vec2(0.3f, -0.3f) * size};
    return s;
}

Plan planGlyph(const GlyphInk& g, float pageScale) {
    Plan p;
    const float x0 = g.x0, y0 = g.y0, x1 = g.x1, y1 = g.y1;
    const float w = x1 - x0, h = y1 - y0;
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
    const float dir = g.rtl ? -1.0f : 1.0f;
    const float wp = w * pageScale, hp = h * pageScale;
    const float tGlyph = clamp(0.05f + 0.024f * wp + 0.01f * hp, 0.09f, 0.21f);
    const uint32_t cp = g.cp;
    auto horiz = [&](float y, float dur, int band) {
        vec2 a(dir > 0 ? x0 + 0.1f * w : x1 - 0.1f * w, y), b(dir > 0 ? x1 - 0.1f * w : x0 + 0.1f * w, y);
        return line(a, b, dur, band);
    };
    auto whole = [&]() { p.bands.push_back({-BIG, BIG}); };

    if (cp == '.' || cp == ',' || cp == 0x3002 || cp == 0x060C) {
        whole();
        p.strokes.push_back(tap(vec2(cx, cy), 0.3f, 0));
    } else if (cp == ':' || cp == ';') {
        p.bands.push_back({-BIG, cy});
        p.bands.push_back({cy, BIG});
        p.strokes.push_back(tap(vec2(cx, y0 + 0.25f * h), 0.25f, 0));
        p.strokes.push_back(tap(vec2(cx, y1 - 0.25f * h), 0.25f, 1));
    } else if (cp == '-' || cp == 0x2013 || cp == 0x2014 || cp == '_' || cp == 0x2212) {
        whole();
        p.strokes.push_back(horiz(cy, 0.05f + 0.012f * wp, 0));
    } else if (cp == '=') {
        p.bands.push_back({-BIG, cy});
        p.bands.push_back({cy, BIG});
        p.strokes.push_back(horiz(y0 + 0.2f * h, 0.05f + 0.01f * wp, 0));
        p.strokes.push_back(horiz(y1 - 0.2f * h, 0.05f + 0.01f * wp, 1));
    } else if (cp == '+') {
        whole();
        p.strokes.push_back(horiz(cy, 0.06f, 0));
        p.strokes.push_back(line(vec2(cx, y0 + 0.08f * h), vec2(cx, y1 - 0.08f * h), 0.06f, 0));
    } else if (cp == '#') {
        whole();
        p.strokes.push_back(line(vec2(x0 + 0.35f * w, y0 + 0.05f * h), vec2(x0 + 0.25f * w, y1 - 0.05f * h), 0.05f, 0));
        p.strokes.push_back(line(vec2(x0 + 0.75f * w, y0 + 0.05f * h), vec2(x0 + 0.65f * w, y1 - 0.05f * h), 0.05f, 0));
        p.strokes.push_back(horiz(y0 + 0.35f * h, 0.05f, 0));
        p.strokes.push_back(horiz(y0 + 0.68f * h, 0.05f, 0));
    } else if (cp == '/') {
        whole();
        p.strokes.push_back(line(vec2(x1 - 0.1f * w, y0 + 0.05f * h), vec2(x0 + 0.1f * w, y1 - 0.05f * h), 0.07f, 0));
    } else if (isCjk(cp)) {
        // Three horizontal bands written top to bottom, each with a short zig-zag stroke.
        float b1 = y0 + h / 3.0f, b2 = y0 + 2.0f * h / 3.0f;
        p.bands.push_back({-BIG, b1});
        p.bands.push_back({b1, b2});
        p.bands.push_back({b2, BIG});
        p.strokes.push_back(sweep(x0, y0, x1, b1, dir, 0.10f, 0, 0.05f));
        p.strokes.push_back(sweep(x0, b1, x1, b2, dir, 0.10f, 1, 0.05f));
        p.strokes.push_back(sweep(x0, b2, x1, y1, dir, 0.10f, 2, 0.05f));
    } else if (hasMarkAbove(cp)) {
        float split = -(isUpperWithMark(cp) ? g.capHeight * 1.04f : g.xHeight * 1.1f);
        split = clamp(split, y0 + 0.2f * h, y1 - 0.2f * h);
        p.bands.push_back({split, BIG});
        p.bands.push_back({-BIG, split});
        p.strokes.push_back(sweep(x0, split, x1, y1, dir, tGlyph, 0));
        p.strokes.push_back(tap(vec2(cx + 0.1f * w, 0.5f * (y0 + split)), std::min(0.35f, 0.5f * (split - y0)), 1));
    } else {
        whole();
        p.strokes.push_back(sweep(x0, y0, x1, y1, dir, tGlyph, 0));
    }
    return p;
}

}  // namespace

PenPath buildPenPath(const std::vector<GlyphInk>& glyphs, uint32_t seed) {
    PenPath P;
    Rng rng(uint64_t(seed) * 0x9E3779B97F4A7C15ull + 5u);
    float t = 0.0f;
    bool down = false;
    vec2 cur;
    auto push = [&](vec2 p, float lift, bool dn) { P.keys.push_back({p.x, p.y, lift, t, dn}); };

    for (size_t gi = 0; gi < glyphs.size(); ++gi) {
        const GlyphInk& g = glyphs[gi];
        float pageScale = length(g.ax);
        Plan plan = planGlyph(g, pageScale);
        std::vector<int> bandK0(plan.bands.size(), -1), bandK1(plan.bands.size(), -1);
        for (size_t si = 0; si < plan.strokes.size(); ++si) {
            const Stroke& st = plan.strokes[si];
            vec2 start = g.toPage(st.pts.front().x, st.pts.front().y);
            if (P.keys.empty()) {
                push(start, 2.0f, false);
                t += 0.05f;
            } else {
                float dist = length(start - cur);
                bool joined = down && si == 0 && gi > 0 && glyphs[gi - 1].joinNext && dist < 1.2f;
                if (joined) {
                    t += 0.015f + 0.004f * dist;  // continue the cursive line to the next letter
                } else {
                    if (down) P.keys.back().down = false;
                    bool word = si == 0 && gi > 0 && glyphs[gi - 1].wordEnd;
                    float lift = word ? 1.8f : 1.0f;
                    float travel = (word ? 0.07f : 0.035f) + (word ? 0.006f : 0.005f) * dist;
                    travel *= rng.range(0.9f, 1.12f);
                    t += 0.45f * travel;
                    push(lerp(cur, start, 0.5f), lift, false);
                    t += 0.55f * travel;
                }
            }
            int k0 = int(P.keys.size());
            int N = int(st.pts.size()) - 1;
            for (int k = 0; k <= N; ++k) {
                float tk = t + st.duration * float(k) / float(std::max(N, 1));
                vec2 p = g.toPage(st.pts[size_t(k)].x, st.pts[size_t(k)].y);
                P.keys.push_back({p.x, p.y, 0.0f, tk, true});
            }
            t += st.duration;
            cur = g.toPage(st.pts.back().x, st.pts.back().y);
            down = true;
            int k1 = int(P.keys.size()) - 1;
            int b = st.band;
            if (bandK0[size_t(b)] < 0) bandK0[size_t(b)] = k0;
            bandK1[size_t(b)] = k1;
        }
        for (size_t b = 0; b < plan.bands.size(); ++b) {
            if (bandK0[b] < 0) continue;
            InkBand ib;
            ib.glyph = int(gi);
            ib.ly0 = plan.bands[b].first;
            ib.ly1 = plan.bands[b].second;
            ib.k0 = bandK0[b];
            ib.k1 = bandK1[b];
            P.bands.push_back(ib);
        }
    }
    if (!P.keys.empty()) {
        P.keys.back().down = false;
        t += 0.06f;
        push(cur, 2.5f, false);
    }
    P.duration = t;
    return P;
}

void appendPath(PenPath& a, const PenPath& b, float gap, int glyphOffset) {
    if (b.keys.empty()) return;
    float t0 = a.keys.empty() ? 0.0f : a.duration + gap;
    int k0 = int(a.keys.size());
    for (PathKey k : b.keys) {
        k.t += t0;
        a.keys.push_back(k);
    }
    for (InkBand ib : b.bands) {
        ib.k0 += k0;
        ib.k1 += k0;
        ib.glyph += glyphOffset;
        a.bands.push_back(ib);
    }
    a.duration = t0 + b.duration;
}

float revealTime(const PenPath& path, const InkBand& band, float x, float y) {
    float best = -1.0f;
    vec2 p(x, y);
    for (int k = band.k0; k < band.k1 && k + 1 < int(path.keys.size()); ++k) {
        const PathKey& A = path.keys[size_t(k)];
        const PathKey& B = path.keys[size_t(k) + 1];
        if (!A.down) continue;
        vec2 a(A.x, A.y), b(B.x, B.y), ab = b - a;
        float l2 = dot(ab, ab);
        float u = l2 > 1e-12f ? clamp(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
        float d = length(p - (a + ab * u));
        if (d > REVEAL_RADIUS) continue;
        float tt = A.t + (B.t - A.t) * u + d / REVEAL_SPEED;
        if (best < 0.0f || tt < best) best = tt;
    }
    return best;
}

float strokeDurationAt(const PenPath& path, float t) {
    const auto& K = path.keys;
    for (size_t k = 0; k + 1 < K.size(); ++k) {
        if (t < K[k].t || t >= K[k + 1].t) continue;
        if (!K[k].down) return 0.0f;
        size_t e = k + 1;
        while (e + 1 < K.size() && K[e].down) ++e;
        return K[e].t - t;
    }
    return 0.0f;
}

// ---- Writing sound -------------------------------------------------------------------------------
PenStrokeSound penStrokeSound(const PenPath* path, float downTime, float late, vec3 tip, vec3 writerEyes, vec3 listener) {
    PenStrokeSound s;
    s.position = tip;
    if (path && downTime >= 0.0f) {
        // Looked up just after the pen-down key: an update that ends exactly on the key (fixed 60 Hz
        // steps: every entry starts on a frame) may give a downTime that falls short of it.
        constexpr float kAfterKey = 1e-3f;
        const float left = strokeDurationAt(*path, downTime + kAfterKey);
        if (left > 0.0f) s.seconds = std::max(0.0f, left + kAfterKey - std::max(0.0f, late));
    }
    s.writersOwn = length(listener - writerEyes) < FIRST_PERSON_RADIUS;
    const vec3 d = tip - listener;
    const float dist = length(d);
    if (s.writersOwn && dist > WRITER_EAR_DISTANCE) s.position = listener + d * (WRITER_EAR_DISTANCE / dist);
    return s;
}

// ---- Pad placement ---------------------------------------------------------------------------------
PadFrame padFrame(int owner, bool clockOnPositiveX) {
    PadFrame f;
    f.owner = owner;
    f.clockOnPositiveX = clockOnPositiveX;
    float sx = clockOnPositiveX ? -1.0f : 1.0f;
    float sz = owner == 0 ? 1.0f : -1.0f;
    float th = PAD_YAW_DEG * DEG;
    // The page's down direction turns outwards (away from the board): the top of the page leans
    // towards the board, as a writer tilts a sheet for the hand on that side.
    f.down = normalize(vec3(sx * std::sin(th), 0.0f, sz * std::cos(th)));
    f.up = vec3(0, 1, 0);
    f.right = cross(f.up, f.down);
    f.center = vec3(sx * layout::SCORESHEET_X, layout::TABLE_TOP_Y, sz * layout::SCORESHEET_Z);
    f.outerSign = f.right.x * sx > 0.0f ? 1.0f : -1.0f;
    return f;
}

m::mat4 penRestTransform(const PadFrame& f) {
    // The pen lies on the front of its barrel (r 4.05 mm, 20 mm from the tip) and on its cap
    // (r 4.5 mm, 115 mm from the tip), clip up, tip towards the top of the pad.
    const float yA = 0.020f, rA = 0.00405f, yB = 0.115f, rB = 0.0045f;
    float a = std::atan((rB - rA) / (yB - yA));
    vec3 axisL(0.0f, std::sin(a), std::cos(a));          // pad-local, towards the back end
    vec3 clipL(0.0f, std::cos(a), -std::sin(a));
    vec3 xL = cross(axisL, clipL);
    float hTip = rA / std::cos(a) - yA * std::sin(a);
    float zCenter = 0.012f;
    vec3 tipL(f.outerSign * (0.5f * layout::SCORESHEET_WIDTH + 0.021f), hTip, zCenter - 0.5f * layout::PEN_LENGTH);
    auto toW = [&](vec3 d) { return f.right * d.x + f.up * d.y + f.down * d.z; };
    return mat4(vec4(toW(xL), 0), vec4(toW(axisL), 0), vec4(toW(clipL), 0), vec4(f.padToWorld(tipL), 1));
}

m::mat4 penWritingTransform(const PadFrame& f, m::vec3 tipWorld) {
    vec3 axisL = normalize(vec3(f.outerSign * 0.42f, 0.80f, 0.46f));
    auto toW = [&](vec3 d) { return f.right * d.x + f.up * d.y + f.down * d.z; };
    vec3 Y = normalize(toW(axisL));
    vec3 Z = normalize(cross(Y, f.right * f.outerSign));
    if (Z.y < 0.0f) Z = -Z;
    vec3 X = cross(Y, Z);
    return mat4(vec4(X, 0), vec4(Y, 0), vec4(Z, 0), vec4(tipWorld, 1));
}

// ---- Page flip -----------------------------------------------------------------------------------------
namespace {

constexpr float BETA0 = 0.80f;    // curl of the whole page while it turns (rad at the free edge)
constexpr float LAMBDA0 = 0.35f;  // extra curl of the pinched side (the corner leads)

// Final pose: from the hinge the page rises, wraps over the binding lip, bends down the spine and
// lies on the table beyond the top edge (arc lengths in mm).
struct Drape {
    float ru, L0, L1, rd, L2, L3, L4;
};
Drape drape(int k) {
    Drape d;
    float kk = float(std::max(k, 0));
    d.ru = BINDING_T + 0.2f + kk * SHEET_T;
    d.L0 = 0.5f * PI * d.ru;
    d.L1 = std::max(0.0f, HINGE_Y - d.ru + BINDING_T + kk * SHEET_T);
    d.rd = 1.4f + kk * SHEET_T;
    d.L2 = 0.5f * PI * d.rd;
    float yTop = PAD_TOP + d.ru;
    float yRest = 0.3f + kk * 0.12f;
    d.L3 = std::max(0.0f, (yTop - d.rd) - (yRest + d.rd));
    d.L4 = 0.5f * PI * d.rd;
    return d;
}

float alphaFinal(const Drape& d, float v) {
    if (v < d.L0) return 0.5f * PI + 0.5f * PI * (v / d.L0);
    v -= d.L0;
    if (v < d.L1) return PI;
    v -= d.L1;
    if (v < d.L2) return PI + 0.5f * PI * (v / d.L2);
    v -= d.L2;
    if (v < d.L3) return 1.5f * PI;
    v -= d.L3;
    if (v < d.L4) return 1.5f * PI - 0.5f * PI * (v / d.L4);
    return PI;
}

struct FlipState {
    float phi, curl, w;
    Drape d;
};
FlipState flipState(float s, int k) {
    s = clamp(s, 0.0f, 1.0f);
    float e = s * s * (3.0f - 2.0f * s);
    FlipState st;
    st.phi = PI * e;
    st.curl = std::sin(PI * e);
    st.w = smoothstep(0.72f, 1.0f, s);
    st.d = drape(k);
    return st;
}

float alphaAt(const FlipState& st, float v, float lead) {
    float g = std::pow(clamp(v / FLIP_LENGTH, 0.0f, 1.0f), 1.5f);
    float turn = st.phi + (BETA0 + LAMBDA0 * lead) * st.curl * g;
    if (st.w <= 0.0f) return turn;
    return turn + (alphaFinal(st.d, v) - turn) * st.w;
}

// Integrates one column (the page is inextensible along it): positions (z, y in mm, hinge-relative)
// at the increasing distances vs[] from the hinge.
void integrateColumn(const FlipState& st, float lead, const float* vs, size_t n, vec2* out) {
    float v = 0.0f;
    vec2 p(0.0f, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        float target = std::max(vs[i], v);
        while (v < target - 1e-6f) {
            float hmax = v < 16.0f ? 0.25f : 1.0f;
            float h = std::min(hmax, target - v);
            float a = alphaAt(st, v + 0.5f * h, lead);
            p += vec2(std::cos(a), std::sin(a)) * h;
            v += h;
        }
        out[i] = p;
    }
}

float leadOf(float xMm, const FlipParams& fp) {
    float u = clamp(xMm / PAGE_W, 0.0f, 1.0f);
    return fp.outerSign > 0.0f ? u : 1.0f - u;
}

vec3 toPad(float xMm, vec2 zy) {
    float zHinge = HINGE_Y - 0.5f * PAGE_H;
    return vec3((xMm - 0.5f * PAGE_W) * 0.001f, (PAD_TOP + zy.y) * 0.001f, (zHinge + zy.x) * 0.001f);
}

}  // namespace

m::vec3 flipPoint(float xMm, float yMm, float s, const FlipParams& p) {
    if (yMm < HINGE_Y) return toPad(xMm, vec2(yMm - HINGE_Y, 0.0f));  // glued under the tape
    FlipState st = flipState(s, p.turnedBelow);
    float v = yMm - HINGE_Y;
    vec2 zy;
    integrateColumn(st, leadOf(xMm, p), &v, 1, &zy);
    return toPad(xMm, zy);
}

void flipGrid(const std::vector<float>& xs, const std::vector<float>& ys, float s, const FlipParams& p,
              std::vector<m::vec3>& out) {
    FlipState st = flipState(s, p.turnedBelow);
    std::vector<float> vs(ys.size());
    for (size_t j = 0; j < ys.size(); ++j) vs[j] = std::max(0.0f, ys[j] - HINGE_Y);
    std::vector<vec2> col(ys.size());
    out.resize(xs.size() * ys.size());
    for (size_t i = 0; i < xs.size(); ++i) {
        integrateColumn(st, leadOf(xs[i], p), vs.data(), vs.size(), col.data());
        for (size_t j = 0; j < ys.size(); ++j)
            out[j * xs.size() + i] = toPad(xs[i], ys[j] < HINGE_Y ? vec2(ys[j] - HINGE_Y, 0.0f) : col[j]);
    }
}

}  // namespace sheet
}  // namespace game
