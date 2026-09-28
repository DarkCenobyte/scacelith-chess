// SDF font atlas. Each glyph is rasterised by stb_truetype at 4x the atlas resolution, converted
// to an exact Euclidean distance field (Felzenszwalb-Huttenlocher) and box-filtered down. The
// high resolution raster uses the non-zero winding rule, so variable fonts with overlapping
// contours (Cinzel) produce clean fields.
#include "ui_font.h"
#include "../core/embedded.h"
#include "../core/log.h"
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
#include <vector>

namespace ui {
namespace font {
namespace {

constexpr int kAtlasW = 2048, kAtlasH = 2048;
constexpr int kAtlasLevels = 4;
constexpr int kOversample = 4;
constexpr int kGutter = 2;  // empty texels between packed glyphs

struct FaceDesc {
    const char* path;
    float emPx;   // atlas pixels per em
    int spread;   // distance range on each side of the outline, atlas texels
};
const FaceDesc kFaces[FACE_COUNT] = {
    {"assets/fonts/EBGaramond12-Regular.ttf", 48.0f, 7},
    {"assets/fonts/EBGaramond12-Italic.ttf", 48.0f, 7},
    {"assets/fonts/Cinzel.ttf", 72.0f, 9},
    {"assets/fonts/FreeSerif-Chess.ttf", 112.0f, 12},
};

struct FaceData {
    bool ok = false;
    stbtt_fontinfo info{};
    float scale = 1.0f;   // font units -> atlas pixels
    float unitEm = 1.0f;  // font units -> em
    Metrics m;
    std::unordered_map<uint32_t, Glyph> glyphs;  // index 0 = known missing
    std::unordered_map<uint64_t, float> kern;
};

struct Shelf { int y, h, x; };

struct Atlas {
    GLuint tex = 0;
    std::vector<uint8_t> pixels;
    std::vector<Shelf> shelves;
    int nextY = 0;
    int dirtyY0 = kAtlasH, dirtyY1 = 0;
    bool full = false;
};

FaceData g_faces[FACE_COUNT];
Atlas g_atlas;
bool g_ready = false;

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
    const int O = kOversample;
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

    out.w = w;
    out.h = h;
    out.px.resize(size_t(w) * size_t(h));
    const float invO = 1.0f / float(O);
    const float norm = 1.0f / (2.0f * float(desc.spread));
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
            float sd = sum * invO * invO * invO;  // average, then hi-res px -> atlas px
            float v = 0.5f + sd * norm;
            out.px[size_t(y) * size_t(w) + size_t(x)] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }
    }
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

void insert(int face, SdfGlyph& sg) {
    FaceData& fd = g_faces[face];
    if (sg.g.hasQuad) {
        int ox = 0, oy = 0;
        if (!pack(sg.w, sg.h, ox, oy)) {
            if (!g_atlas.full) LOGW("ui: font atlas full, glyph U+%04X dropped", unsigned(sg.cp));
            g_atlas.full = true;
            sg.g.hasQuad = false;
        } else {
            for (int y = 0; y < sg.h; ++y)
                std::copy_n(&sg.px[size_t(y) * size_t(sg.w)], size_t(sg.w),
                            &g_atlas.pixels[size_t(oy + y) * kAtlasW + size_t(ox)]);
            g_atlas.dirtyY0 = std::min(g_atlas.dirtyY0, oy);
            g_atlas.dirtyY1 = std::max(g_atlas.dirtyY1, oy + sg.h);
            sg.g.u0 = float(ox) / kAtlasW;
            sg.g.v0 = float(oy) / kAtlasH;
            sg.g.u1 = float(ox + sg.w) / kAtlasW;
            sg.g.v1 = float(oy + sg.h) / kAtlasH;
        }
    }
    fd.glyphs[sg.cp] = sg.g;
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
    }
    return it->second.index ? &it->second : nullptr;
}

void addRange(std::vector<uint32_t>& v, uint32_t a, uint32_t b) {
    for (uint32_t c = a; c <= b; ++c) v.push_back(c);
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
            LOGE("ui: font %s not embedded", kFaces[f].path);
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

    // Build the distance fields on a few worker threads, then pack sequentially.
    struct Job { int face; uint32_t cp; int gi; };
    std::vector<Job> jobs;
    for (int f = 0; f < FACE_COUNT; ++f) {
        if (!g_faces[f].ok) continue;
        for (uint32_t cp : sets[f]) jobs.push_back({f, cp, stbtt_FindGlyphIndex(&g_faces[f].info, int(cp))});
    }
    std::vector<SdfGlyph> results(jobs.size());
    unsigned hw = std::thread::hardware_concurrency();
    int threads = int(std::clamp(hw == 0 ? 2u : hw, 1u, 4u));
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

    glCreateTextures(GL_TEXTURE_2D, 1, &g_atlas.tex);
    glTextureStorage2D(g_atlas.tex, kAtlasLevels, GL_R8, kAtlasW, kAtlasH);
    glTextureParameteri(g_atlas.tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(g_atlas.tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(g_atlas.tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(g_atlas.tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(g_atlas.tex, GL_TEXTURE_MAX_LEVEL, kAtlasLevels - 1);
    g_atlas.dirtyY0 = 0;
    g_atlas.dirtyY1 = kAtlasH;
    flushUploads();
    g_ready = true;
    LOGI("ui: font atlas built (%d glyphs, %d/%d rows used, %.0f ms)", int(results.size()), g_atlas.nextY, kAtlasH,
         (plat::time() - t0) * 1000.0);
    return g_faces[FACE_TEXT].ok;
}

void shutdown() {
    if (g_atlas.tex) glDeleteTextures(1, &g_atlas.tex);
    g_atlas = Atlas();
    for (auto& f : g_faces) f = FaceData();
    g_ready = false;
}

bool ready() { return g_ready; }

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
    int chain[3] = {face, FACE_TEXT, FACE_SYMBOL};
    if (face == FACE_SYMBOL) chain[1] = FACE_TEXT, chain[2] = FACE_TEXT;
    for (int f : chain) {
        if (const Glyph* g = lookupOrBuild(f, cp)) {
            if (usedFace) *usedFace = f;
            return g;
        }
    }
    return nullptr;
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

}  // namespace font
}  // namespace ui
