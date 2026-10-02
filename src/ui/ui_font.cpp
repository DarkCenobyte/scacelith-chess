// SDF font atlas. Each glyph is rasterised by stb_truetype at 4x (2x for the CJK faces, see
// kFaces) the atlas resolution, converted to an exact Euclidean distance field
// (Felzenszwalb-Huttenlocher) and box-filtered down. The high resolution raster uses the non-zero
// winding rule, so variable fonts with overlapping contours (Cinzel) produce clean fields.
#include "ui_font.h"
#include "../core/embedded.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include "../platform/platform.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ui {
namespace font {
namespace {

constexpr int kAtlasW = 4096, kAtlasH = 4096;
constexpr int kAtlasLevels = 5;
constexpr int kGutter = 2;  // empty texels between packed glyphs
// Share of the atlas height above which a language switch clears it before building the new
// language's glyphs.
constexpr float kResetOnLanguageSwitch = 0.55f;

struct FaceDesc {
    const char* path;
    float emPx;          // atlas pixels per em
    int spread;          // distance range on each side of the outline, atlas texels
    int oversample;      // rasterisation resolution / atlas resolution
    float fallbackScale; // size factor when the face stands in for another UI face
};
// The CJK faces use a lower oversampling: their thousands of possible glyphs are built on demand
// and 2x is plenty for dense strokes at 56 px/em.
const FaceDesc kFaces[FACE_COUNT] = {
    {"assets/fonts/EBGaramond12-Regular.ttf", 48.0f, 7, 4, 1.0f},
    {"assets/fonts/EBGaramond12-Italic.ttf", 48.0f, 7, 4, 1.0f},
    {"assets/fonts/Cinzel.ttf", 72.0f, 9, 4, 1.0f},
    {"assets/fonts/FreeSerif-Chess.ttf", 112.0f, 12, 4, 1.0f},
    {"assets/fonts/hand/Caveat.ttf", 56.0f, 7, 4, 1.0f},
    {"assets/fonts/hand/MarckScript-Regular.ttf", 56.0f, 7, 4, 1.0f},
    {"assets/fonts/hand/BadScript-Regular.ttf", 56.0f, 7, 4, 1.0f},
    {"assets/fonts/hand/ArefRuqaa-Hand.ttf", 56.0f, 7, 4, 1.05f},
    {"assets/fonts/hand/KleeOne-Hand.ttf", 56.0f, 7, 2, 0.92f},
    {"assets/fonts/hand/LXGWWenKai-Hand.ttf", 56.0f, 7, 2, 0.92f},
    {"assets/fonts/hand/LXGWWenKaiTC-Hand.ttf", 56.0f, 7, 2, 0.92f},
    {"assets/fonts/AmiriUI-Regular.ttf", 56.0f, 7, 4, 1.12f},
};

struct Slot {
    Glyph g;
    bool ink = false;          // has a distance field (hasQuad once packed)
    int gen = -1;              // atlas generation it is packed in
    int w = 0, h = 0;          // distance field size, atlas texels
    std::vector<uint8_t> px;   // kept so an atlas clear only needs a copy
};

struct FaceData {
    bool ok = false;
    stbtt_fontinfo info{};
    float scale = 1.0f;   // font units -> atlas pixels
    float unitEm = 1.0f;  // font units -> em
    Metrics m;
    std::unordered_map<uint32_t, Slot> glyphs;  // index 0 = known missing
    std::unordered_map<uint64_t, float> kern;
};

struct Shelf { int y, h, x; };

struct Atlas {
    GLuint tex = 0;
    std::vector<uint8_t> pixels;
    std::vector<Shelf> shelves;
    int nextY = 0;
    int dirtyY0 = kAtlasH, dirtyY1 = 0;
    int gen = 0;
    bool resetPending = false;
};

FaceData g_faces[FACE_COUNT];
Atlas g_atlas;
bool g_ready = false;
int g_langGen = -1;
int g_hanOrder[3] = {FACE_HAND_SC, FACE_HAND_TC, FACE_HAND_JA};
int g_punctOrder[3] = {FACE_HAND_SC, FACE_HAND_TC, FACE_HAND_JA};

// ---- Distance transform -------------------------------------------------------------------------
struct Scratch {
    std::vector<uint8_t> cov;
    std::vector<float> in, out;  // squared distances
    std::vector<double> f, d, z;
    std::vector<int> v;
};

const double kInf = 1e20;

void edt1d(Scratch& s, int n) {
    double* f = s.f.data();
    double* d = s.d.data();
    double* z = s.z.data();
    int* v = s.v.data();
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        double sv = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        while (sv <= z[k]) {
            --k;
            sv = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = sv;
        z[k + 1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) ++k;
        double dq = double(q - v[k]);
        d[q] = dq * dq + f[v[k]];
    }
}

// grid: 0 at feature pixels, kInf elsewhere (in place -> squared distance to nearest feature).
void edt2d(Scratch& s, std::vector<float>& grid, int w, int h) {
    int n = std::max(w, h);
    s.f.resize(size_t(n));
    s.d.resize(size_t(n));
    s.z.resize(size_t(n) + 1);
    s.v.resize(size_t(n));
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) s.f[size_t(y)] = grid[size_t(y) * size_t(w) + size_t(x)];
        edt1d(s, h);
        for (int y = 0; y < h; ++y) grid[size_t(y) * size_t(w) + size_t(x)] = float(s.d[size_t(y)]);
    }
    for (int y = 0; y < h; ++y) {
        float* row = &grid[size_t(y) * size_t(w)];
        for (int x = 0; x < w; ++x) s.f[size_t(x)] = row[x];
        edt1d(s, w);
        for (int x = 0; x < w; ++x) row[x] = float(s.d[size_t(x)]);
    }
}

// Coverage at O x the output resolution (sc.cov, W x H) -> exact inside / outside distances,
// box-filtered down to w x h bytes: 0.5 + d / (2 * spread), d in output texels (inside > 0).
void coverageToSdf(Scratch& sc, int W, int H, int w, int h, int O, int spread, uint8_t* dst) {
    size_t N = size_t(W) * size_t(H);
    sc.in.resize(N);
    sc.out.resize(N);
    for (size_t i = 0; i < N; ++i) {
        bool inside = sc.cov[i] >= 128;
        sc.out[i] = inside ? 0.0f : float(kInf);  // distance to the nearest inside pixel
        sc.in[i] = inside ? float(kInf) : 0.0f;   // distance to the nearest outside pixel
    }
    edt2d(sc, sc.out, W, H);
    edt2d(sc, sc.in, W, H);
    const float invO = 1.0f / float(O);
    const float norm = 1.0f / (2.0f * float(spread));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float sum = 0.0f;
            for (int sy = 0; sy < O; ++sy) {
                size_t row = size_t(y * O + sy) * size_t(W);
                for (int sx = 0; sx < O; ++sx) {
                    size_t i = row + size_t(x * O + sx);
                    // Refine boundary pixels with the anti-aliased coverage.
                    float d;
                    if (sc.cov[i] >= 128) d = std::sqrt(sc.in[i]) - 0.5f;
                    else d = -(std::sqrt(sc.out[i]) - 0.5f);
                    if (std::fabs(d) <= 1.0f) d = float(sc.cov[i]) / 255.0f - 0.5f;
                    sum += d;
                }
            }
            float v = 0.5f + sum * invO * invO * invO * norm;  // average, then hi-res px -> output px
            dst[size_t(y) * size_t(w) + size_t(x)] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }
    }
}

struct SdfGlyph {
    uint32_t cp = 0;
    Glyph g;
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

// Builds the glyph metrics and (for visible glyphs) its distance field bitmap.
void buildGlyph(const FaceData& fd, const FaceDesc& desc, uint32_t cp, int gi, SdfGlyph& out, Scratch& sc) {
    out.cp = cp;
    out.g = Glyph();
    out.g.index = gi;
    out.px.clear();
    out.w = out.h = 0;
    if (!gi) return;
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&fd.info, gi, &adv, &lsb);
    out.g.advance = float(adv) * fd.unitEm;
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&fd.info, gi, fd.scale, fd.scale, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) return;
    const int O = desc.oversample;
    int pad = desc.spread + 1;
    int bx0 = x0 - pad, by0 = y0 - pad;
    int w = x1 - x0 + 2 * pad, h = y1 - y0 + 2 * pad;
    int W = w * O, H = h * O;
    float hs = fd.scale * float(O);
    int hx0, hy0, hx1, hy1;
    stbtt_GetGlyphBitmapBox(&fd.info, gi, hs, hs, &hx0, &hy0, &hx1, &hy1);
    int ox = hx0 - bx0 * O, oy = hy0 - by0 * O;
    int rw = hx1 - hx0, rh = hy1 - hy0;
    ox = std::max(ox, 0);
    oy = std::max(oy, 0);
    rw = std::min(rw, W - ox);
    rh = std::min(rh, H - oy);
    sc.cov.assign(size_t(W) * size_t(H), 0);
    if (rw > 0 && rh > 0)
        stbtt_MakeGlyphBitmap(&fd.info, &sc.cov[size_t(oy) * size_t(W) + size_t(ox)], rw, rh, W, hs, hs, gi);

    out.w = w;
    out.h = h;
    out.px.resize(size_t(w) * size_t(h));
    coverageToSdf(sc, W, H, w, h, O, desc.spread, out.px.data());
    float inv = 1.0f / desc.emPx;
    out.g.hasQuad = true;
    out.g.x0 = float(bx0) * inv;
    out.g.y0 = float(by0) * inv;
    out.g.x1 = float(bx0 + w) * inv;
    out.g.y1 = float(by0 + h) * inv;
    out.g.pxRange = 2.0f * float(desc.spread);
}

bool pack(int w, int h, int& ox, int& oy) {
    int pw = w + kGutter, ph = h + kGutter;
    Shelf* best = nullptr;
    for (Shelf& s : g_atlas.shelves) {
        if (s.h >= ph && s.h <= ph + ph / 3 + 4 && s.x + pw <= kAtlasW) {
            if (!best || s.h < best->h) best = &s;
        }
    }
    if (!best) {
        if (g_atlas.nextY + ph > kAtlasH) return false;
        g_atlas.shelves.push_back({g_atlas.nextY, ph, kGutter});
        g_atlas.nextY += ph;
        best = &g_atlas.shelves.back();
        if (best->x + pw > kAtlasW) return false;
    }
    ox = best->x;
    oy = best->y + kGutter / 2;
    best->x += pw;
    return true;
}

// Copies a slot's distance field into the atlas. A full atlas schedules a clear for the next
// frame; the glyph stays invisible until then.
void place(Slot& s, uint32_t cp) {
    if (!s.ink) return;
    int ox = 0, oy = 0;
    if (!pack(s.w, s.h, ox, oy)) {
        if (!g_atlas.resetPending) LOGI("ui: font atlas full (glyph U+%04X), clearing it next frame", unsigned(cp));
        g_atlas.resetPending = true;
        s.g.hasQuad = false;
        s.gen = -1;
        return;
    }
    for (int y = 0; y < s.h; ++y)
        std::copy_n(&s.px[size_t(y) * size_t(s.w)], size_t(s.w), &g_atlas.pixels[size_t(oy + y) * kAtlasW + size_t(ox)]);
    g_atlas.dirtyY0 = std::min(g_atlas.dirtyY0, oy);
    g_atlas.dirtyY1 = std::max(g_atlas.dirtyY1, oy + s.h);
    s.g.u0 = float(ox) / kAtlasW;
    s.g.v0 = float(oy) / kAtlasH;
    s.g.u1 = float(ox + s.w) / kAtlasW;
    s.g.v1 = float(oy + s.h) / kAtlasH;
    s.g.hasQuad = true;
    s.gen = g_atlas.gen;
}

void insert(int face, SdfGlyph& sg) {
    Slot& s = g_faces[face].glyphs[sg.cp];
    s.g = sg.g;
    s.ink = sg.g.hasQuad;
    s.w = sg.w;
    s.h = sg.h;
    s.px = std::move(sg.px);
    s.g.hasQuad = false;
    s.gen = -1;
    place(s, sg.cp);
}

const Glyph* lookupOrBuild(int face, uint32_t cp) {
    FaceData& fd = g_faces[face];
    if (!fd.ok) return nullptr;
    auto it = fd.glyphs.find(cp);
    if (it == fd.glyphs.end()) {
        static Scratch sc;
        SdfGlyph sg;
        int gi = stbtt_FindGlyphIndex(&fd.info, int(cp));
        buildGlyph(fd, kFaces[face], cp, gi, sg, sc);
        insert(face, sg);
        it = fd.glyphs.find(cp);
    } else if (it->second.ink && it->second.gen != g_atlas.gen && !g_atlas.resetPending) {
        place(it->second, cp);  // packed before the last atlas clear
    }
    return it->second.g.index ? &it->second.g : nullptr;
}

bool faceHas(int face, uint32_t cp) {
    const FaceData& fd = g_faces[face];
    if (!fd.ok) return false;
    auto it = fd.glyphs.find(cp);
    if (it != fd.glyphs.end()) return it->second.g.index != 0;
    return stbtt_FindGlyphIndex(&fd.info, int(cp)) != 0;
}

void addRange(std::vector<uint32_t>& v, uint32_t a, uint32_t b) {
    for (uint32_t c = a; c <= b; ++c) v.push_back(c);
}

// Han / CJK punctuation face order for the current language.
void updateLanguageOrder() {
    const std::string& lang = i18n::language();
    if (lang == "ja") {
        int h[3] = {FACE_HAND_JA, FACE_HAND_SC, FACE_HAND_TC};
        std::copy(h, h + 3, g_hanOrder);
        std::copy(h, h + 3, g_punctOrder);
    } else if (lang == "zh-Hant") {
        int h[3] = {FACE_HAND_TC, FACE_HAND_SC, FACE_HAND_JA};
        std::copy(h, h + 3, g_hanOrder);
        std::copy(h, h + 3, g_punctOrder);
    } else {
        int h[3] = {FACE_HAND_SC, FACE_HAND_TC, FACE_HAND_JA};
        std::copy(h, h + 3, g_hanOrder);
        std::copy(h, h + 3, g_punctOrder);
    }
}

// Candidate faces for a codepoint, best first. Returns the count.
int fallbackChain(int face, uint32_t cp, int* out) {
    int n = 0;
    auto add = [&](int f) {
        for (int i = 0; i < n; ++i)
            if (out[i] == f) return;
        out[n++] = f;
    };
    add(face);
    bool hand = isHandwritingFace(face);
    if (uni::isArabic(cp)) {
        if (hand) add(FACE_HAND_ARABIC);
        add(FACE_ARABIC);
        add(FACE_HAND_ARABIC);
    } else if (uni::isKana(cp)) {
        add(FACE_HAND_JA);
        for (int f : g_hanOrder) add(f);
    } else if (uni::isHan(cp)) {
        for (int f : g_hanOrder) add(f);
    } else if (uni::isCjkPunct(cp)) {
        for (int f : g_punctOrder) add(f);
    }
    if (hand) {
        add(FACE_HAND_CAVEAT);
        add(FACE_HAND_MARCK);
        add(FACE_HAND_BADSCRIPT);
    }
    add(FACE_TEXT);
    add(FACE_SYMBOL);
    for (int f = FACE_HAND_CAVEAT; f <= FACE_HAND_TC; ++f) add(f);
    add(FACE_ARABIC);
    return n;
}

void resetAtlas() {
    std::fill(g_atlas.pixels.begin(), g_atlas.pixels.end(), uint8_t(0));
    g_atlas.shelves.clear();
    g_atlas.nextY = kGutter;
    g_atlas.gen++;
    g_atlas.resetPending = false;
    g_atlas.dirtyY0 = 0;
    g_atlas.dirtyY1 = kAtlasH;
    for (auto& fd : g_faces)
        for (auto& kv : fd.glyphs) {
            kv.second.g.hasQuad = false;
            kv.second.gen = -1;
        }
    LOGI("ui: font atlas cleared (generation %d)", g_atlas.gen);
}

// Glyphs of the current language's strings in the UI faces.
void prewarmLanguage() {
    std::vector<std::pair<int, uint32_t>> want;
    const int uiFaces[3] = {FACE_TEXT, FACE_ITALIC, FACE_TITLE};
    std::unordered_set<uint64_t> seen;
    for (uint32_t cp : i18n::codepoints()) {
        for (int f : uiFaces) {
            int used = resolveFace(f, cp);
            if (used < 0) continue;
            uint64_t key = (uint64_t(uint32_t(used)) << 32) | cp;
            if (seen.insert(key).second) want.push_back({used, cp});
            // Title-face fallbacks draw lowercase Cyrillic / Greek as small capitals.
            if (f == FACE_TITLE && used != FACE_TITLE && uni::isLower(cp)) {
                uint32_t up = uint32_t(uni::toUpper(char32_t(cp)));
                int u2 = resolveFace(FACE_TITLE, up);
                uint64_t k2 = (uint64_t(uint32_t(u2)) << 32) | up;
                if (u2 >= 0 && seen.insert(k2).second) want.push_back({u2, up});
            }
        }
    }
    prewarm(want);
}

}  // namespace

uint32_t decodeUtf8(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k) -> int {
        if (i + k >= s.size()) return -1;
        unsigned char b = static_cast<unsigned char>(s[i + k]);
        return (b & 0xC0) == 0x80 ? (b & 0x3F) : -1;
    };
    if (c < 0x80) { i += 1; return c; }
    if ((c & 0xE0) == 0xC0) {
        int b1 = cont(1);
        if (b1 >= 0) { i += 2; return (uint32_t(c & 0x1F) << 6) | uint32_t(b1); }
    } else if ((c & 0xF0) == 0xE0) {
        int b1 = cont(1), b2 = cont(2);
        if (b1 >= 0 && b2 >= 0) { i += 3; return (uint32_t(c & 0x0F) << 12) | (uint32_t(b1) << 6) | uint32_t(b2); }
    } else if ((c & 0xF8) == 0xF0) {
        int b1 = cont(1), b2 = cont(2), b3 = cont(3);
        if (b1 >= 0 && b2 >= 0 && b3 >= 0) {
            i += 4;
            return (uint32_t(c & 0x07) << 18) | (uint32_t(b1) << 12) | (uint32_t(b2) << 6) | uint32_t(b3);
        }
    }
    i += 1;
    return 0xFFFD;
}

bool init() {
    if (g_ready) return true;
    double t0 = plat::time();
    g_atlas.pixels.assign(size_t(kAtlasW) * kAtlasH, 0);
    g_atlas.shelves.clear();
    g_atlas.nextY = kGutter;

    std::vector<uint32_t> sets[FACE_COUNT];
    {
        std::vector<uint32_t> text;
        addRange(text, 0x20, 0x7E);
        addRange(text, 0xA0, 0xFF);
        const uint32_t extra[] = {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2039,
                                  0x203A, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212, 0x2767, 0x2009, 0x202F};
        text.insert(text.end(), std::begin(extra), std::end(extra));
        sets[FACE_TEXT] = text;
        sets[FACE_ITALIC] = text;
        addRange(sets[FACE_TITLE], 0x20, 0x7E);
        const uint32_t titleExtra[] = {0xB7, 0xBD, 0xC9, 0x2013, 0x2014, 0x2019, 0x2039, 0x203A};
        sets[FACE_TITLE].insert(sets[FACE_TITLE].end(), std::begin(titleExtra), std::end(titleExtra));
        addRange(sets[FACE_SYMBOL], 0x2654, 0x265F);
    }

    for (int f = 0; f < FACE_COUNT; ++f) {
        FaceData& fd = g_faces[f];
        fd = FaceData();
        const embedded::File* file = embedded::find(kFaces[f].path);
        if (!file) {
            if (f > FACE_SYMBOL) LOGW("ui: font %s not embedded", kFaces[f].path);
            else LOGE("ui: font %s not embedded", kFaces[f].path);
            continue;
        }
        int off = stbtt_GetFontOffsetForIndex(file->data, 0);
        if (off < 0 || !stbtt_InitFont(&fd.info, file->data, off)) {
            LOGE("ui: cannot parse font %s", kFaces[f].path);
            continue;
        }
        fd.ok = true;
        fd.scale = stbtt_ScaleForMappingEmToPixels(&fd.info, kFaces[f].emPx);
        fd.unitEm = stbtt_ScaleForMappingEmToPixels(&fd.info, 1.0f);
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&fd.info, &asc, &desc, &gap);
        fd.m.ascent = float(asc) * fd.unitEm;
        fd.m.descent = float(-desc) * fd.unitEm;
        fd.m.lineGap = float(gap) * fd.unitEm;
        int bx0, by0, bx1, by1;
        if (stbtt_GetCodepointBox(&fd.info, 'H', &bx0, &by0, &bx1, &by1)) fd.m.capHeight = float(by1) * fd.unitEm;
        if (stbtt_GetCodepointBox(&fd.info, 'x', &bx0, &by0, &bx1, &by1)) fd.m.xHeight = float(by1) * fd.unitEm;
        if (f == FACE_SYMBOL) {
            if (stbtt_GetCodepointBox(&fd.info, 0x265B, &bx0, &by0, &bx1, &by1)) fd.m.capHeight = float(by1) * fd.unitEm;
            fd.m.xHeight = fd.m.capHeight;
        }
    }

    glCreateTextures(GL_TEXTURE_2D, 1, &g_atlas.tex);
    glTextureStorage2D(g_atlas.tex, kAtlasLevels, GL_R8, kAtlasW, kAtlasH);
    glTextureParameteri(g_atlas.tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(g_atlas.tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(g_atlas.tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(g_atlas.tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(g_atlas.tex, GL_TEXTURE_MAX_LEVEL, kAtlasLevels - 1);
    g_ready = true;

    // Base sets and the current language's strings, built in parallel.
    std::vector<std::pair<int, uint32_t>> base;
    for (int f = 0; f < FACE_COUNT; ++f)
        for (uint32_t cp : sets[f]) base.push_back({f, cp});
    prewarm(base);
    updateLanguageOrder();
    g_langGen = i18n::generation();
    prewarmLanguage();
    g_atlas.dirtyY0 = 0;
    g_atlas.dirtyY1 = kAtlasH;
    flushUploads();
    LOGI("ui: font atlas built (%d/%d rows used, %.0f ms)", g_atlas.nextY, kAtlasH, (plat::time() - t0) * 1000.0);
    return g_faces[FACE_TEXT].ok;
}

void shutdown() {
    if (g_atlas.tex) glDeleteTextures(1, &g_atlas.tex);
    g_atlas = Atlas();
    for (auto& f : g_faces) f = FaceData();
    g_ready = false;
    g_langGen = -1;
}

bool ready() { return g_ready; }

void beginFrame() {
    if (!g_ready) return;
    if (g_atlas.resetPending) resetAtlas();
    if (g_langGen != i18n::generation()) {
        g_langGen = i18n::generation();
        updateLanguageOrder();
        if (float(g_atlas.nextY) > kResetOnLanguageSwitch * float(kAtlasH)) resetAtlas();
        double t0 = plat::time();
        prewarmLanguage();
        LOGI("ui: glyphs of language %s ready (%.0f ms, %d/%d atlas rows)", i18n::language().c_str(),
             (plat::time() - t0) * 1000.0, g_atlas.nextY, kAtlasH);
    }
}

void prewarm(const std::vector<std::pair<int, uint32_t>>& list) {
    if (!g_ready) return;
    struct Job { int face; uint32_t cp; int gi; };
    std::vector<Job> jobs;
    for (auto& fc : list) {
        int f = fc.first;
        if (f < 0 || f >= FACE_COUNT || !g_faces[f].ok) continue;
        auto it = g_faces[f].glyphs.find(fc.second);
        if (it != g_faces[f].glyphs.end()) {
            if (it->second.ink && it->second.gen != g_atlas.gen && !g_atlas.resetPending) place(it->second, fc.second);
            continue;
        }
        jobs.push_back({f, fc.second, stbtt_FindGlyphIndex(&g_faces[f].info, int(fc.second))});
    }
    if (jobs.empty()) return;
    std::vector<SdfGlyph> results(jobs.size());
    unsigned hw = std::thread::hardware_concurrency();
    int threads = int(std::clamp(hw == 0 ? 2u : hw, 1u, 4u));
    threads = std::min(threads, int(jobs.size()));
    auto work = [&](int t) {
        Scratch sc;
        for (size_t i = size_t(t); i < jobs.size(); i += size_t(threads))
            buildGlyph(g_faces[jobs[i].face], kFaces[jobs[i].face], jobs[i].cp, jobs[i].gi, results[i], sc);
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(work, t);
    work(0);
    for (auto& th : pool) th.join();
    // Pack tallest first for a tighter shelf layout.
    std::vector<size_t> order(results.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return results[a].h > results[b].h; });
    for (size_t i : order) insert(jobs[i].face, results[i]);
}

void flushUploads() {
    if (!g_atlas.tex || g_atlas.dirtyY1 <= g_atlas.dirtyY0) return;
    int y0 = g_atlas.dirtyY0, y1 = g_atlas.dirtyY1;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTextureSubImage2D(g_atlas.tex, 0, 0, y0, kAtlasW, y1 - y0, GL_RED, GL_UNSIGNED_BYTE,
                        &g_atlas.pixels[size_t(y0) * kAtlasW]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glGenerateTextureMipmap(g_atlas.tex);
    g_atlas.dirtyY0 = kAtlasH;
    g_atlas.dirtyY1 = 0;
}

const Glyph* glyph(int face, uint32_t cp, int* usedFace) {
    if (face < 0 || face >= FACE_COUNT) face = FACE_TEXT;
    int chain[FACE_COUNT + 4];
    int n = fallbackChain(face, cp, chain);
    for (int i = 0; i < n; ++i) {
        if (const Glyph* g = lookupOrBuild(chain[i], cp)) {
            if (usedFace) *usedFace = chain[i];
            return g;
        }
    }
    return nullptr;
}

int resolveFace(int face, uint32_t cp) {
    if (face < 0 || face >= FACE_COUNT) face = FACE_TEXT;
    int chain[FACE_COUNT + 4];
    int n = fallbackChain(face, cp, chain);
    for (int i = 0; i < n; ++i)
        if (faceHas(chain[i], cp)) return chain[i];
    return -1;
}

float fallbackScale(int requested, int used) {
    if (requested == used || used < 0 || used >= FACE_COUNT || isHandwritingFace(requested)) return 1.0f;
    return kFaces[used].fallbackScale;
}

bool isHandwritingFace(int face) { return face >= FACE_HAND_CAVEAT && face <= FACE_HAND_TC; }

const char* handStyleName(int style) {
    switch (style) {
    case HAND_MARCK: return "Marck Script";
    case HAND_BADSCRIPT: return "Bad Script";
    default: return "Caveat";
    }
}

int handwritingFace(int style, uint32_t cp) {
    if (g_langGen != i18n::generation()) updateLanguageOrder();
    int latin = FACE_HAND_CAVEAT + std::clamp(style, 0, HAND_STYLE_COUNT - 1);
    int order[10];
    int n = 0;
    if (uni::isArabic(cp)) {
        order[n++] = FACE_HAND_ARABIC;
    } else if (uni::isKana(cp)) {
        order[n++] = FACE_HAND_JA;
    } else if (uni::isHan(cp)) {
        for (int f : g_hanOrder) order[n++] = f;
    } else if (uni::isCjkPunct(cp)) {
        for (int f : g_punctOrder) order[n++] = f;
    }
    order[n++] = latin;
    order[n++] = FACE_HAND_CAVEAT;
    order[n++] = FACE_HAND_JA;
    order[n++] = FACE_HAND_SC;
    for (int i = 0; i < n; ++i)
        if (faceHas(order[i], cp)) return order[i];
    return faceHas(FACE_TEXT, cp) ? int(FACE_TEXT) : latin;
}

float kerning(int face, int a, int b) {
    FaceData& fd = g_faces[face];
    if (!fd.ok || !a || !b) return 0.0f;
    uint64_t key = (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
    auto it = fd.kern.find(key);
    if (it != fd.kern.end()) return it->second;
    float k = float(stbtt_GetGlyphKernAdvance(&fd.info, a, b)) * fd.unitEm;
    fd.kern.emplace(key, k);
    return k;
}

const Metrics& metrics(int face) {
    if (face < 0 || face >= FACE_COUNT) face = FACE_TEXT;
    return g_faces[face].m;
}

GLuint atlasTexture() { return g_atlas.tex; }
int atlasGeneration() { return g_atlas.gen; }

// ---- Standalone line distance fields (3D markings) ---------------------------------------------

bool renderLineSdf(int face, const std::string& utf8, float capPx, int spread, float tracking, int w, int h,
                   std::vector<uint8_t>& out, float* inkWidthPx) {
    out.clear();
    if (face < 0 || face >= FACE_COUNT || w <= 0 || h <= 0 || capPx <= 0.0f || spread <= 0) return false;
    // The face as loaded by init(), else parsed here: no GL and no atlas are needed, so a world can
    // bake its markings before (or without) the UI.
    stbtt_fontinfo info;
    if (g_faces[face].ok) {
        info = g_faces[face].info;
    } else {
        const embedded::File* file = embedded::find(kFaces[face].path);
        int off = file ? stbtt_GetFontOffsetForIndex(file->data, 0) : -1;
        if (off < 0 || !stbtt_InitFont(&info, file->data, off)) {
            LOGW("ui: renderLineSdf: font %s unavailable", kFaces[face].path);
            return false;
        }
    }
    int bx0, by0, bx1, by1;
    if (!stbtt_GetCodepointBox(&info, 'H', &bx0, &by0, &bx1, &by1) || by1 <= 0) return false;
    constexpr int O = 4;  // rasterisation oversampling, as the atlas (non-CJK faces)
    const int W = w * O, H = h * O;
    const float s = capPx * float(O) / float(by1);          // font units -> high resolution pixels
    const float emHi = s / stbtt_ScaleForMappingEmToPixels(&info, 1.0f);  // high resolution pixels per em

    // Pen layout (left to right, face kerning, tracking in em), then the ink extent.
    struct Placed { int gi; float pen; int x0, y0, x1, y1; };
    std::vector<Placed> glyphs;
    float pen = 0.0f;
    int prev = 0;
    int inkX0 = 1 << 30, inkX1 = -(1 << 30);
    for (size_t i = 0; i < utf8.size();) {
        uint32_t cp = decodeUtf8(utf8, i);
        int gi = stbtt_FindGlyphIndex(&info, int(cp));
        if (prev && gi) pen += float(stbtt_GetGlyphKernAdvance(&info, prev, gi)) * s;
        Placed p{gi, pen, 0, 0, 0, 0};
        float shift = pen - std::floor(pen);
        stbtt_GetGlyphBitmapBoxSubpixel(&info, gi, s, s, shift, 0.0f, &p.x0, &p.y0, &p.x1, &p.y1);
        if (gi && p.x1 > p.x0 && p.y1 > p.y0) {
            inkX0 = std::min(inkX0, int(std::floor(pen)) + p.x0);
            inkX1 = std::max(inkX1, int(std::floor(pen)) + p.x1);
            glyphs.push_back(p);
        }
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
        pen += float(adv) * s + tracking * emHi;
        prev = gi;
    }
    if (glyphs.empty()) return false;
    // Ink centred horizontally; the cap band (baseline to cap line) centred vertically.
    int offX = (W - (inkX1 - inkX0)) / 2 - inkX0;
    int baseY = int(std::lround(0.5f * float(H) + 0.5f * capPx * float(O)));
    const int margin = spread * O;
    if (inkX0 + offX < margin || inkX1 + offX > W - margin) {
        LOGW("ui: renderLineSdf: \"%s\" does not fit %dx%d texels at cap %.0f", utf8.c_str(), w, h, double(capPx));
        return false;
    }

    // Union of the glyph coverages (max), at O x the output resolution.
    Scratch sc;
    sc.cov.assign(size_t(W) * size_t(H), 0);
    std::vector<uint8_t> tmp;
    for (const Placed& p : glyphs) {
        int gw = p.x1 - p.x0, gh = p.y1 - p.y0;
        tmp.assign(size_t(gw) * size_t(gh), 0);
        float shift = p.pen - std::floor(p.pen);
        stbtt_MakeGlyphBitmapSubpixel(&info, tmp.data(), gw, gh, gw, s, s, shift, 0.0f, p.gi);
        int ox = int(std::floor(p.pen)) + p.x0 + offX, oy = baseY + p.y0;
        for (int y = 0; y < gh; ++y) {
            int ty = oy + y;
            if (ty < 0 || ty >= H) continue;
            for (int x = 0; x < gw; ++x) {
                int tx = ox + x;
                if (tx < 0 || tx >= W) continue;
                uint8_t& c = sc.cov[size_t(ty) * size_t(W) + size_t(tx)];
                c = std::max(c, tmp[size_t(y) * size_t(gw) + size_t(x)]);
            }
        }
    }

    // Exact inside / outside distances, box-filtered to the output resolution (as buildGlyph).
    out.resize(size_t(w) * size_t(h));
    coverageToSdf(sc, W, H, w, h, O, spread, out.data());
    if (inkWidthPx) *inkWidthPx = float(inkX1 - inkX0) / float(O);
    return true;
}

}  // namespace font
}  // namespace ui
