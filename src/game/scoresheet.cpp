// One player's scoresheet: page textures (printed form + handwriting as distance fields), the
// entry being written with its reveal times, page turns, draw submission. See scoresheet.h.
//
// Page textures: a 2-layer RGBA8 array, 1024 x 1456 texels per A5 page (6.9 texels/mm); layer
// page % 2 holds the page on top and the one under it. The channels are ink distance fields
// (r = handwriting, g = print) and the pen pressure (b); shaders/materials/paper.glsl turns them
// into paper, ink and print, crisp at any distance (custom mips preserve ink coverage). A layer is
// re-rendered only when its page changes; finished entries are added to it incrementally.
// The entry being written is rendered once into its own RGBA16F texture (1024 x 384 texels over
// the full page width) together with the time at which the pen tip reaches each texel, so the
// reveal costs nothing per frame: the paper shader shows the ink whose time has come.
#include "scoresheet.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../render/gpu.h"
#include "../render/materials/material_library.h"
#include "../render/mesh.h"
#include "../render/shader.h"
#include "../scene/scoresheet_model.h"
#include "../ui/text_shape.h"
#include "../ui/ui_font.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>

using namespace m;
namespace font = ui::font;

namespace game {
using namespace sheet;

namespace {

constexpr int PAGE_TEX_W = 1024, PAGE_TEX_H = 1456;
constexpr float DENSITY = float(PAGE_TEX_W) / PAGE_W;   // texels per mm
constexpr float SPREAD = 4.0f;                          // distance range, texels each side
constexpr int ENTRY_TEX_W = 1024, ENTRY_TEX_H = 384;    // 148 x 55 mm
constexpr float SURFACE_MM = PAD_TOP + 0.04f;           // upper face of the top page

const char* PAGE_VS = "shaders/materials/bake/scoresheet_page.vert";
const char* PAGE_FS = "shaders/materials/bake/scoresheet_page.frag";
const char* MIPS_CS = "shaders/materials/bake/scoresheet_mips.comp";

// A glyph ready to be drawn into a page field: its placement, atlas quad and distance scale.
struct InkGlyph {
    GlyphInk ink;
    vec2 q0, q1;          // quad bounds, glyph-local mm (SDF padding included)
    vec2 uv0, uv1;        // atlas
    float distScale = 1;  // page mm per unit of atlas value
    float dilation = 0;   // mm
    float pressure = 1;
};

struct Entry {
    int page = 0;
    std::vector<InkGlyph> glyphs;
    PenPath path;
    float top = 0.0f;     // page mm of the entry texture's first row
};

struct InkVertex {
    vec4 posUv, p0, p1;
};

// Geometry shared by every scoresheet (pad space / pen frame).
struct Shared {
    int users = 0;
    bool built = false;
    Mesh board, stackTop, stackEdges, tape, flatPage;
    vec4 tapeInst;
    bool penBuilt = false;
    Mesh penMetal, penBody;
};
Shared g_shared;

void buildShared() {
    if (g_shared.built) return;
    Model pad = buildScoresheetPad();
    for (ModelPart& p : pad.parts) {
        Mesh* dst = p.name == "board" ? &g_shared.board
                    : p.name == "stack_top" ? &g_shared.stackTop
                    : p.name == "stack_edges" ? &g_shared.stackEdges
                    : &g_shared.tape;
        if (dst == &g_shared.tape) g_shared.tapeInst = p.inst[0];
        dst->upload(p.mesh, p.name.c_str());
    }
    std::vector<float> xs = {0.0f, PAGE_W}, ys = {0.0f, PAGE_H};
    std::vector<vec3> grid;
    flipGrid(xs, ys, 0.0f, FlipParams{}, grid);
    MeshData flat;
    buildPageMesh(xs, ys, grid, flat);
    g_shared.flatPage.upload(flat, "scoresheet_page");
    g_shared.built = true;
}

void buildPen() {
    if (g_shared.penBuilt) return;
    Model pen = buildBallpointPen();
    for (ModelPart& p : pen.parts) (p.material == MaterialId::PenBody ? g_shared.penBody : g_shared.penMetal).upload(p.mesh, p.name.c_str());
    g_shared.penBuilt = true;
}

void destroyShared() {
    for (Mesh* m : {&g_shared.board, &g_shared.stackTop, &g_shared.stackEdges, &g_shared.tape, &g_shared.flatPage,
                    &g_shared.penMetal, &g_shared.penBody})
        m->destroy();
    g_shared.built = g_shared.penBuilt = false;
}

// Mesh with a vertex buffer updated in place (the page being turned).
void uploadDynamic(Mesh& mesh, const MeshData& d, const AABB& bounds) {
    mesh.destroy();
    glCreateBuffers(1, &mesh.vbo);
    glNamedBufferStorage(mesh.vbo, GLsizeiptr(d.vertices.size() * sizeof(Vertex)), d.vertices.data(), GL_DYNAMIC_STORAGE_BIT);
    glCreateBuffers(1, &mesh.ibo);
    glNamedBufferStorage(mesh.ibo, GLsizeiptr(d.indices.size() * sizeof(uint32_t)), d.indices.data(), 0);
    glCreateVertexArrays(1, &mesh.vao);
    glVertexArrayVertexBuffer(mesh.vao, 0, mesh.vbo, 0, sizeof(Vertex));
    glVertexArrayElementBuffer(mesh.vao, mesh.ibo);
    for (GLuint a = 0; a < 4; ++a) {
        glEnableVertexArrayAttrib(mesh.vao, a);
        glVertexArrayAttribBinding(mesh.vao, a, 0);
    }
    glVertexArrayAttribFormat(mesh.vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, pos));
    glVertexArrayAttribFormat(mesh.vao, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, normal));
    glVertexArrayAttribFormat(mesh.vao, 2, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, tangent));
    glVertexArrayAttribFormat(mesh.vao, 3, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    mesh.indexCount = uint32_t(d.indices.size());
    mesh.vertexCount = uint32_t(d.vertices.size());
    mesh.bounds = bounds;
    mesh.name = "scoresheet_turning_page";
}

int atlasWidth() {
    static int w = 0;
    if (!w && font::atlasTexture()) glGetTextureLevelParameteriv(font::atlasTexture(), 0, GL_TEXTURE_WIDTH, &w);
    return w > 0 ? w : 4096;
}

// Atlas texels per em of a glyph (the quad spans (u1 - u0) * atlas width texels).
float emPx(const font::Glyph& g) {
    float w = g.x1 - g.x0;
    return w > 1e-6f ? (g.u1 - g.u0) * float(atlasWidth()) / w : 56.0f;
}

uint32_t mix32(uint32_t a, uint32_t b) { return hash32(a * 0x9E3779B9u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2))); }
float unit(uint32_t h) { return float(h & 0xFFFFFF) / 16777216.0f; }

// Distance-field thickness correction of the handwriting faces towards a ballpoint line
// (~0.35 mm): mm added to the glyph outline at the move size.
float styleDilation(int style) {
    // Brings each face to the line of a medium ballpoint (about 0.45 mm): thinner lines fell
    // below a pixel at the player's reading distance and the writing turned pale grey.
    switch (style) {
        case font::HAND_MARCK: return 0.06f;
        case font::HAND_BADSCRIPT: return 0.04f;
        default: return 0.01f;
    }
}

// Saves / restores the GL state the page rendering touches.
struct GlStateGuard {
    GLint fbo = 0, viewport[4] = {}, program = 0, vao = 0, blendSrc = 0, blendDst = 0, blendEq = 0, depthFunc = 0;
    GLboolean blend = 0, depth = 0, cull = 0, scissor = 0, depthMask = 0;
    GlStateGuard() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrc);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDst);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEq);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        blend = glIsEnabled(GL_BLEND);
        depth = glIsEnabled(GL_DEPTH_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
    }
    ~GlStateGuard() {
        glBindFramebuffer(GL_FRAMEBUFFER, GLuint(fbo));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glUseProgram(GLuint(program));
        glBindVertexArray(GLuint(vao));
        glBlendFunc(GLenum(blendSrc), GLenum(blendDst));
        glBlendEquation(GLenum(blendEq));
        glDepthFunc(GLenum(depthFunc));
        glDepthMask(depthMask);
        auto set = [](GLenum cap, GLboolean on) { on ? glEnable(cap) : glDisable(cap); };
        set(GL_BLEND, blend);
        set(GL_DEPTH_TEST, depth);
        set(GL_CULL_FACE, cull);
        set(GL_SCISSOR_TEST, scissor);
    }
};

void pushQuad(std::vector<InkVertex>& out, const vec2 p[4], const vec2 uv[4], vec4 p0, vec4 p1) {
    // p[0..3] = corners (q0.x,q0.y) (q1.x,q0.y) (q0.x,q1.y) (q1.x,q1.y): two triangles.
    for (int k : {0, 1, 2, 1, 3, 2}) out.push_back({vec4(p[k].x, p[k].y, uv[k].x, uv[k].y), p0, p1});
}

// The glyph quad of 'g' restricted to glyph-local rows [ly0, ly1].
void glyphQuad(std::vector<InkVertex>& out, const InkGlyph& g, float ly0, float ly1, vec4 p1) {
    float a = std::max(ly0, g.q0.y), b = std::min(ly1, g.q1.y);
    if (b <= a) return;
    float ta = (a - g.q0.y) / (g.q1.y - g.q0.y), tb = (b - g.q0.y) / (g.q1.y - g.q0.y);
    float va = g.uv0.y + (g.uv1.y - g.uv0.y) * ta, vb = g.uv0.y + (g.uv1.y - g.uv0.y) * tb;
    vec2 p[4] = {g.ink.toPage(g.q0.x, a), g.ink.toPage(g.q1.x, a), g.ink.toPage(g.q0.x, b), g.ink.toPage(g.q1.x, b)};
    vec2 uv[4] = {vec2(g.uv0.x, va), vec2(g.uv1.x, va), vec2(g.uv0.x, vb), vec2(g.uv1.x, vb)};
    pushQuad(out, p, uv, vec4(0.0f, g.distScale, g.dilation, g.pressure), p1);
}

void printedFormVertices(int page, std::vector<InkVertex>& out) {
    Form f = printedForm(page);
    const float margin = (SPREAD + 1.0f) / DENSITY;
    for (const Rect& r : f.rules) {
        vec2 c(r.cx(), r.cy()), h(0.5f * r.w(), 0.5f * r.h());
        vec2 e = h + vec2(margin);
        vec2 p[4] = {c + vec2(-e.x, -e.y), c + vec2(e.x, -e.y), c + vec2(-e.x, e.y), c + vec2(e.x, e.y)};
        vec2 uv[4] = {vec2(-e.x, -e.y), vec2(e.x, -e.y), vec2(-e.x, e.y), vec2(e.x, e.y)};
        pushQuad(out, p, uv, vec4(2.0f, 0.0f, 0.0f, 0.0f), vec4(h.x, h.y, 0.0f, 0.0f));
    }
    for (const FormText& t : f.texts) {
        ui::text::Run run = ui::text::shapeLine(t.text, t.face);
        float sz = t.capHeight / std::max(font::metrics(t.face).capHeight, 0.2f);  // mm per em
        float w = run.advance * sz;
        if (t.maxWidth > 0.0f && w > t.maxWidth) {
            sz *= t.maxWidth / w;
            w = t.maxWidth;
        }
        float x0 = t.align == 1 ? t.x - 0.5f * w : (t.align == 2 ? t.x - w : t.x);
        for (const ui::text::PlacedGlyph& pg : run.glyphs) {
            const font::Glyph& G = *pg.glyph;
            if (!G.hasQuad) continue;
            float gx0 = x0 + (pg.x + G.x0) * sz, gx1 = x0 + (pg.x + G.x1) * sz;
            float gy0 = t.baseline + G.y0 * sz, gy1 = t.baseline + G.y1 * sz;
            vec2 p[4] = {vec2(gx0, gy0), vec2(gx1, gy0), vec2(gx0, gy1), vec2(gx1, gy1)};
            vec2 uv[4] = {vec2(G.u0, G.v0), vec2(G.u1, G.v0), vec2(G.u0, G.v1), vec2(G.u1, G.v1)};
            float distScale = G.pxRange * sz / emPx(G);
            pushQuad(out, p, uv, vec4(1.0f, distScale, 0.02f, 0.0f), vec4(0.0f));
        }
    }
}

}  // namespace

struct Scoresheet::Impl {
    bool gl = false;
    gpu::Texture pageTex, entryTex;
    GLuint pageFbo[2] = {0, 0}, entryFbo = 0, entryDepth = 0;
    GLuint vao = 0;
    gpu::Buffer vbo, keys;
    Material paper;
    int layerPage[2] = {-1, -1};
    std::vector<InkGlyph> layerPending[2];
    std::string language;
    // Finished ink per page, and the fields written (or queued) per page.
    std::vector<std::vector<InkGlyph>> pageInk;
    std::vector<std::array<bool, size_t(Field::Count)>> fieldTaken;
    std::deque<Entry> entries;   // unfinished, oldest first (the front is the one being written)
    bool entryReady = false;     // the entry texture holds entries.front()
    float now = -1.0f;           // path time of the entry being written
    uint32_t entrySerial = 0;
    // Page turn.
    float turnS = -1.0f;
    bool flipDirty = false;
    std::vector<float> gx, gy;
    std::vector<vec3> grid;
    MeshData flipData;
    Mesh flipMesh;
    std::vector<Mesh> turned;    // pages lying face down beyond the top edge

    std::vector<InkGlyph>& inkOf(int page) {
        if (int(pageInk.size()) <= page) pageInk.resize(size_t(page) + 1);
        return pageInk[size_t(page)];
    }
    bool& taken(int page, Field f) {
        if (int(fieldTaken.size()) <= page) {
            std::array<bool, size_t(Field::Count)> none{};
            fieldTaken.resize(size_t(page) + 1, none);
        }
        return fieldTaken[size_t(page)][size_t(f)];
    }
    void addInk(int page, const std::vector<InkGlyph>& glyphs) {
        std::vector<InkGlyph>& dst = inkOf(page);
        dst.insert(dst.end(), glyphs.begin(), glyphs.end());
        int L = page % 2;
        if (layerPage[L] == page) layerPending[L].insert(layerPending[L].end(), glyphs.begin(), glyphs.end());
    }

    void drawVertices(const std::vector<InkVertex>& v) {
        if (v.empty()) return;
        gpu::ensureBuffer(vbo, v.size() * sizeof(InkVertex));
        glNamedBufferSubData(vbo.id, 0, GLsizeiptr(v.size() * sizeof(InkVertex)), v.data());
        glVertexArrayVertexBuffer(vao, 0, vbo.id, 0, sizeof(InkVertex));
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size()));
    }

    void buildMips(const gpu::Texture& t, int layer, bool entry) {
        const ShaderProgram& p =
            entry ? shaders::compute(MIPS_CS, {"ENTRY"}) : shaders::compute(MIPS_CS);
        if (!p.valid()) return;
        p.use();
        for (int lvl = 1; lvl < t.levels; ++lvl) {
            int dw = std::max(1, t.width >> lvl), dh = std::max(1, t.height >> lvl);
            float ks = std::min(float(1 << (lvl - 1)), 2.0f * SPREAD) / (2.0f * SPREAD);
            float kd = std::min(float(1 << lvl), 2.0f * SPREAD) / (2.0f * SPREAD);
            glBindImageTexture(0, t.id, lvl - 1, GL_FALSE, layer, GL_READ_ONLY, t.format);
            glBindImageTexture(1, t.id, lvl, GL_FALSE, layer, GL_WRITE_ONLY, t.format);
            p.set("uKSrc", ks);
            p.set("uKDst", kd);
            glProgramUniform2i(p.id, p.loc("uDstSize"), dw, dh);
            gpu::dispatch2D(dw, dh, 8, 8);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    // Draws the printed form and all the ink of 'page' (full) or only the pending ink into a layer.
    void renderLayer(int layer, int page, bool full) {
        std::vector<InkVertex> v;
        if (full) printedFormVertices(page, v);
        const std::vector<InkGlyph>& ink = full ? inkOf(page) : layerPending[layer];
        for (const InkGlyph& g : ink) glyphQuad(v, g, -1e9f, 1e9f, vec4(0.0f));
        layerPending[layer].clear();
        layerPage[layer] = page;
        if (v.empty() && !full) return;
        ProgramDesc d;
        d.vs = PAGE_VS;
        d.fs = PAGE_FS;
        const ShaderProgram& p = shaders::get(d);
        if (!p.valid()) return;
        {
            GlStateGuard guard;
            glBindFramebuffer(GL_FRAMEBUFFER, pageFbo[layer]);
            glViewport(0, 0, PAGE_TEX_W, PAGE_TEX_H);
            if (full) {
                const float zero[4] = {0, 0, 0, 0};
                glClearNamedFramebufferfv(pageFbo[layer], GL_COLOR, 0, zero);
            }
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendEquation(GL_MAX);
            glBlendFunc(GL_ONE, GL_ONE);
            p.use();
            p.set("uOrigin", 0.0f, 0.0f);
            p.set("uDensity", DENSITY);
            p.set("uSize", float(PAGE_TEX_W), float(PAGE_TEX_H));
            p.set("uSpread", SPREAD);
            glBindTextureUnit(0, font::atlasTexture());
            drawVertices(v);
            glBlendEquation(GL_FUNC_ADD);
        }
        buildMips(pageTex, layer, false);
    }

    void renderEntry(const Entry& e) {
        std::vector<vec4> k(e.path.keys.size());
        for (size_t i = 0; i < k.size(); ++i) {
            const PathKey& pk = e.path.keys[i];
            k[i] = vec4(pk.x, pk.y, pk.t, pk.down ? 1.0f : 0.0f);
        }
        std::vector<InkVertex> v;
        for (const InkBand& b : e.path.bands) {
            if (b.glyph < 0 || b.glyph >= int(e.glyphs.size()) || k.empty()) continue;
            float tEnd = e.path.keys[size_t(std::min(b.k1, int(k.size()) - 1))].t;
            glyphQuad(v, e.glyphs[size_t(b.glyph)], b.ly0, b.ly1, vec4(float(b.k0), float(b.k1), tEnd, 0.0f));
        }
        ProgramDesc d;
        d.vs = PAGE_VS;
        d.fs = PAGE_FS;
        d.defines = {"ENTRY"};
        const ShaderProgram& p = shaders::get(d);
        if (!p.valid()) return;
        {
            GlStateGuard guard;
            glBindFramebuffer(GL_FRAMEBUFFER, entryFbo);
            glViewport(0, 0, ENTRY_TEX_W, ENTRY_TEX_H);
            const float zero[4] = {0, 0, 0, 0};
            glClearNamedFramebufferfv(entryFbo, GL_COLOR, 0, zero);
            glClearNamedFramebufferfv(entryFbo, GL_DEPTH, 0, zero);
            glDisable(GL_BLEND);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_GREATER);
            glDepthMask(GL_TRUE);
            if (!k.empty()) {
                gpu::ensureBuffer(keys, k.size() * sizeof(vec4));
                glNamedBufferSubData(keys.id, 0, GLsizeiptr(k.size() * sizeof(vec4)), k.data());
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, keys.id);
            }
            p.use();
            p.set("uOrigin", 0.0f, e.top);
            p.set("uDensity", DENSITY);
            p.set("uSize", float(ENTRY_TEX_W), float(ENTRY_TEX_H));
            p.set("uSpread", SPREAD);
            p.set("uRevealSpeed", REVEAL_SPEED);
            p.set("uRevealRadius", REVEAL_RADIUS);
            glBindTextureUnit(0, font::atlasTexture());
            drawVertices(v);
        }
        buildMips(entryTex, 0, true);
    }
};

Scoresheet::Scoresheet() : impl_(new Impl()) {}
Scoresheet::~Scoresheet() { shutdown(); }

bool Scoresheet::init(const Config& c) {
    shutdown();
    cfg_ = c;
    frame_ = padFrame(cfg_.owner, cfg_.clockOnPositiveX);
    if (!font::ready()) font::init();
    Impl& I = *impl_;
    I.pageTex = gpu::createTexture2DArray(PAGE_TEX_W, PAGE_TEX_H, 2, GL_RGBA8, 0);
    gpu::setAnisotropy(I.pageTex, 16.0f);
    I.entryTex = gpu::createTexture2D(ENTRY_TEX_W, ENTRY_TEX_H, GL_RGBA16F, 0);
    gpu::setAnisotropy(I.entryTex, 16.0f);
    for (int L = 0; L < 2; ++L) {
        glCreateFramebuffers(1, &I.pageFbo[L]);
        glNamedFramebufferTextureLayer(I.pageFbo[L], GL_COLOR_ATTACHMENT0, I.pageTex.id, 0, L);
    }
    glCreateRenderbuffers(1, &I.entryDepth);
    glNamedRenderbufferStorage(I.entryDepth, GL_DEPTH_COMPONENT32F, ENTRY_TEX_W, ENTRY_TEX_H);
    glCreateFramebuffers(1, &I.entryFbo);
    glNamedFramebufferTexture(I.entryFbo, GL_COLOR_ATTACHMENT0, I.entryTex.id, 0);
    glNamedFramebufferRenderbuffer(I.entryFbo, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, I.entryDepth);
    if (glCheckNamedFramebufferStatus(I.pageFbo[0], GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glCheckNamedFramebufferStatus(I.entryFbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        LOGW("scoresheet: incomplete page framebuffer");
    glCreateVertexArrays(1, &I.vao);
    for (GLuint a = 0; a < 3; ++a) {
        glEnableVertexArrayAttrib(I.vao, a);
        glVertexArrayAttribFormat(I.vao, a, 4, GL_FLOAT, GL_FALSE, GLuint(a * sizeof(vec4)));
        glVertexArrayAttribBinding(I.vao, a, 0);
    }
    I.paper = materials::get(MaterialId::ScoresheetPaper);
    I.paper.name = cfg_.owner == 0 ? "ScoresheetPaperWhite" : "ScoresheetPaperBlack";
    I.paper.params[1] = vec4(cfg_.inkColor, I.paper.params[1].w);
    I.paper.params[3] = vec4(float(PAGE_TEX_W), float(PAGE_TEX_H), SPREAD, 1.0f);
    I.paper.params[4] = vec4(float(ENTRY_TEX_W), float(ENTRY_TEX_H), PAGE_W, PAGE_H);
    I.paper.textures[0] = I.pageTex.id;
    I.paper.textureTargets[0] = GL_TEXTURE_2D_ARRAY;
    I.paper.textures[1] = I.entryTex.id;
    I.paper.textureTargets[1] = GL_TEXTURE_2D;
    pageGridSamples(I.gx, I.gy);
    // Turning page: a dynamic mesh (updated in place), bounds = everywhere the page can go.
    flipGrid(I.gx, I.gy, 0.0f, FlipParams{0, frame_.outerSign}, I.grid);
    buildPageMesh(I.gx, I.gy, I.grid, I.flipData);
    AABB b;
    b.add(vec3(-0.5f * PAGE_W, 0.0f, -0.5f * PAGE_H - FLIP_LENGTH - 10.0f) * 0.001f);
    b.add(vec3(0.5f * PAGE_W, FLIP_LENGTH + 10.0f, 0.5f * PAGE_H) * 0.001f);
    uploadDynamic(I.flipMesh, I.flipData, b);
    buildShared();
    ++g_shared.users;
    I.gl = true;
    reset();
    return true;
}

void Scoresheet::shutdown() {
    Impl& I = *impl_;
    if (!I.gl) return;
    I.pageTex.destroy();
    I.entryTex.destroy();
    glDeleteFramebuffers(2, I.pageFbo);
    glDeleteFramebuffers(1, &I.entryFbo);
    glDeleteRenderbuffers(1, &I.entryDepth);
    glDeleteVertexArrays(1, &I.vao);
    I.vbo.destroy();
    I.keys.destroy();
    I.flipMesh.destroy();
    for (Mesh& m : I.turned) m.destroy();
    I.turned.clear();
    I.pageFbo[0] = I.pageFbo[1] = I.entryFbo = I.entryDepth = I.vao = 0;
    I.gl = false;
    if (--g_shared.users <= 0) {
        g_shared.users = 0;
        destroyShared();
    }
}

void Scoresheet::releaseShared() {
    if (g_shared.users <= 0) destroyShared();
}

void Scoresheet::reset() {
    Impl& I = *impl_;
    page_ = 0;
    pendingTurns_ = 0;
    I.pageInk.clear();
    I.fieldTaken.clear();
    I.entries.clear();
    I.entryReady = false;
    I.now = -1.0f;
    I.turnS = -1.0f;
    for (Mesh& m : I.turned) m.destroy();
    I.turned.clear();
    I.layerPage[0] = I.layerPage[1] = -1;
    I.layerPending[0].clear();
    I.layerPending[1].clear();
}

void Scoresheet::setClockSide(bool clockOnPositiveX) {
    if (clockOnPositiveX == cfg_.clockOnPositiveX) return;
    cfg_.clockOnPositiveX = clockOnPositiveX;
    frame_ = padFrame(cfg_.owner, clockOnPositiveX);
    // The pinched corner changes side: turned pages are rebuilt for it.
    Impl& I = *impl_;
    for (size_t k = 0; k < I.turned.size(); ++k) {
        flipGrid(I.gx, I.gy, 1.0f, FlipParams{int(k), frame_.outerSign}, I.grid);
        MeshData d;
        buildPageMesh(I.gx, I.gy, I.grid, d);
        I.turned[k].upload(d, "scoresheet_turned_page");
    }
    I.flipDirty = true;
}

void Scoresheet::setHandStyle(int style) { cfg_.handStyle = style; }
void Scoresheet::setLetters(const PieceLetters& l) { cfg_.letters = l; }
void Scoresheet::setInkColor(vec3 c) {
    cfg_.inkColor = c;
    impl_->paper.params[1] = vec4(c, impl_->paper.params[1].w);
}

// ---- Writing ------------------------------------------------------------------------------------
namespace {

// Shapes 'text' in the hand 'style' and places it naturally in 'box'.
std::vector<InkGlyph> handwrite(const std::string& text, const WriteBox& box, int style, uint32_t seed) {
    std::vector<InkGlyph> out;
    if (text.empty()) return out;
    ui::text::Run shaped = ui::text::shapeLine(text, [style](uint32_t cp) { return font::handwritingFace(style, cp); });
    if (shaped.glyphs.empty()) return out;
    const font::Metrics& mt = font::metrics(font::handwritingFace(style, 'A'));
    Run run;
    run.rtl = shaped.rtl;
    run.advance = shaped.advance;
    run.capHeight = mt.capHeight;
    run.xHeight = mt.xHeight;
    for (const ui::text::PlacedGlyph& pg : shaped.glyphs) {
        RunGlyph g;
        g.cp = pg.cp;
        g.penX = pg.x;
        g.source = pg.source;
        const font::Glyph& G = *pg.glyph;
        if (G.hasQuad) {
            // Ink box = the quad without its distance-field padding.
            float pad = (0.5f * G.pxRange + 1.0f) / emPx(G);
            g.x0 = G.x0 + pad;
            g.y0 = G.y0 + pad;
            g.x1 = std::max(g.x0 + 1e-3f, G.x1 - pad);
            g.y1 = std::max(g.y0 + 1e-3f, G.y1 - pad);
        }
        run.glyphs.push_back(g);
    }
    std::vector<GlyphInk> placed = placeHandwriting(run, box, seed);
    const float emMm = box.capHeight / std::max(run.capHeight, 0.2f);
    const float dil = styleDilation(style);
    uint32_t h = seed;
    for (GlyphInk& gi : placed) {
        const ui::text::PlacedGlyph& pg = shaped.glyphs[size_t(gi.runIndex)];
        const font::Glyph& G = *pg.glyph;
        // Cursive faces join their letters: the pen stays down between adjacent letters of a word.
        gi.joinNext = style == font::HAND_MARCK && !gi.wordEnd;
        InkGlyph ig;
        ig.ink = gi;
        ig.q0 = vec2(G.x0, G.y0) * emMm;
        ig.q1 = vec2(G.x1, G.y1) * emMm;
        ig.uv0 = vec2(G.u0, G.v0);
        ig.uv1 = vec2(G.u1, G.v1);
        float gs = length(gi.ax);
        ig.distScale = G.pxRange * emMm * gs / emPx(G);
        ig.dilation = dil;
        h = mix32(h, gi.cp);
        ig.pressure = 0.72f + 0.28f * unit(h);
        out.push_back(ig);
    }
    return out;
}

std::vector<GlyphInk> inks(const std::vector<InkGlyph>& g) {
    std::vector<GlyphInk> out;
    out.reserve(g.size());
    for (const InkGlyph& x : g) out.push_back(x.ink);
    return out;
}

}  // namespace

// Builds an entry from (field box, text) parts written in order; returns false when empty.
static bool composeEntry(Entry& e, const std::vector<std::pair<WriteBox, std::string>>& parts, int style, uint32_t seed) {
    PathKey last;
    bool any = false;
    for (size_t i = 0; i < parts.size(); ++i) {
        uint32_t s = mix32(seed, uint32_t(i) + 1u);
        std::vector<InkGlyph> g = handwrite(parts[i].second, parts[i].first, style, s);
        if (g.empty()) continue;
        PenPath p = buildPenPath(inks(g), s);
        if (p.keys.empty()) continue;
        float gap = 0.0f;
        if (any) {
            float d = length(vec2(p.keys.front().x - last.x, p.keys.front().y - last.y));
            gap = 0.12f + 0.004f * d;  // pen carried in the air to the next field
        }
        appendPath(e.path, p, gap, int(e.glyphs.size()));
        e.glyphs.insert(e.glyphs.end(), g.begin(), g.end());
        last = e.path.keys.back();
        any = true;
    }
    if (!any) return false;
    Rect r = inkBounds(inks(e.glyphs));
    e.top = std::max(0.0f, r.y0 - 3.0f);
    return true;
}

static std::vector<anim::PenKey> worldPath(const PadFrame& f, const PenPath& p) {
    std::vector<anim::PenKey> out;
    out.reserve(p.keys.size());
    for (const PathKey& k : p.keys) {
        anim::PenKey pk;
        pk.t = k.t;
        pk.tip = f.padToWorld(pageToPad(k.x, k.y, SURFACE_MM + k.lift));
        pk.down = k.down;
        out.push_back(pk);
    }
    return out;
}

static std::string pageLabel(int page) { return std::to_string(page + 1); }

std::vector<anim::PenKey> Scoresheet::beginHeader(const Header& h) {
    Impl& I = *impl_;
    Entry e;
    e.page = 0;
    std::vector<std::pair<WriteBox, std::string>> parts = {
        {fieldBox(Field::Event), h.event},         {fieldBox(Field::Date), h.date},
        {fieldBox(Field::Round), h.round},         {fieldBox(Field::Board), h.board},
        {fieldBox(Field::WhiteName), h.white},     {fieldBox(Field::WhiteElo), h.whiteElo},
        {fieldBox(Field::BlackName), h.black},     {fieldBox(Field::BlackElo), h.blackElo},
    };
    if (!h.note.empty()) parts.push_back({fieldBox(Field::Note), h.note});
    if (!h.reference.empty()) parts.push_back({fieldBox(Field::Reference), h.reference});
    if (!I.taken(0, Field::Page)) parts.push_back({fieldBox(Field::Page), pageLabel(0)});
    if (!composeEntry(e, parts, cfg_.handStyle, mix32(cfg_.seed, 0x4EADu))) return {};
    for (int f = 0; f <= int(Field::Page); ++f) I.taken(0, Field(f)) = true;
    I.taken(0, Field::Note) = I.taken(0, Field::Reference) = true;
    I.entries.push_back(std::move(e));
    return worldPath(frame_, I.entries.back().path);
}

std::vector<anim::PenKey> Scoresheet::beginMove(int ply, const std::string& san) {
    Impl& I = *impl_;
    int page = pageOfPly(ply);
    // Nobody turned the page: it turns at once.
    while (page_ + pendingTurns_ < page) {
        ++pendingTurns_;
        if (pendingTurns_ == 1) finishPageTurn();
    }
    Entry e;
    e.page = page;
    std::vector<std::pair<WriteBox, std::string>> parts;
    bool pageNo = page > 0 && !I.taken(page, Field::Page);
    if (pageNo) parts.push_back({fieldBox(Field::Page), pageLabel(page)});
    parts.push_back({moveBox(ply), localizeSan(san, cfg_.letters)});
    if (!composeEntry(e, parts, cfg_.handStyle, mix32(cfg_.seed, 0x10000u + uint32_t(ply)))) return {};
    if (pageNo) I.taken(page, Field::Page) = true;
    I.entries.push_back(std::move(e));
    return worldPath(frame_, I.entries.back().path);
}

std::vector<anim::PenKey> Scoresheet::beginField(Field f, const std::string& text) {
    Impl& I = *impl_;
    int page = isHeaderField(f) ? 0 : page_ + pendingTurns_;
    Entry e;
    e.page = page;
    if (!composeEntry(e, {{fieldBox(f), text}}, cfg_.handStyle, mix32(cfg_.seed, 0x20000u + uint32_t(f) * 131u + uint32_t(page))))
        return {};
    I.taken(page, f) = true;
    I.entries.push_back(std::move(e));
    return worldPath(frame_, I.entries.back().path);
}

std::vector<anim::PenKey> Scoresheet::beginResult(const std::string& result) { return beginField(Field::Result, result); }

void Scoresheet::setWritingTime(float t) {
    if (t < 0.0f) return;
    impl_->now = t;
}

void Scoresheet::finishEntry() {
    Impl& I = *impl_;
    if (I.entries.empty()) return;
    Entry& e = I.entries.front();
    I.addInk(e.page, e.glyphs);
    I.entries.pop_front();
    I.entryReady = false;
    I.now = -1.0f;
}

int Scoresheet::pendingEntries() const { return int(impl_->entries.size()); }

const sheet::PenPath* Scoresheet::writingPath() const {
    return impl_->entries.empty() ? nullptr : &impl_->entries.front().path;
}

void Scoresheet::writeHeaderInstant(const Header& h) {
    Impl& I = *impl_;
    Entry e;
    std::vector<std::pair<WriteBox, std::string>> parts = {
        {fieldBox(Field::Event), h.event},     {fieldBox(Field::Date), h.date},
        {fieldBox(Field::Round), h.round},     {fieldBox(Field::Board), h.board},
        {fieldBox(Field::WhiteName), h.white}, {fieldBox(Field::WhiteElo), h.whiteElo},
        {fieldBox(Field::BlackName), h.black}, {fieldBox(Field::BlackElo), h.blackElo},
    };
    if (!h.note.empty()) parts.push_back({fieldBox(Field::Note), h.note});
    if (!h.reference.empty()) parts.push_back({fieldBox(Field::Reference), h.reference});
    if (!I.taken(0, Field::Page)) parts.push_back({fieldBox(Field::Page), pageLabel(0)});
    if (composeEntry(e, parts, cfg_.handStyle, mix32(cfg_.seed, 0x4EADu))) I.addInk(0, e.glyphs);
    for (int f = 0; f <= int(Field::Page); ++f) I.taken(0, Field(f)) = true;
    I.taken(0, Field::Note) = I.taken(0, Field::Reference) = true;
}

void Scoresheet::writeMoveInstant(int ply, const std::string& san) {
    Impl& I = *impl_;
    int page = pageOfPly(ply);
    while (page_ < page) {
        if (pendingTurns_ == 0) ++pendingTurns_;
        finishPageTurn();
    }
    Entry e;
    std::vector<std::pair<WriteBox, std::string>> parts;
    bool pageNo = page > 0 && !I.taken(page, Field::Page);
    if (pageNo) parts.push_back({fieldBox(Field::Page), pageLabel(page)});
    parts.push_back({moveBox(ply), localizeSan(san, cfg_.letters)});
    if (composeEntry(e, parts, cfg_.handStyle, mix32(cfg_.seed, 0x10000u + uint32_t(ply)))) I.addInk(page, e.glyphs);
    if (pageNo) I.taken(page, Field::Page) = true;
}

void Scoresheet::writeFieldInstant(Field f, const std::string& text) {
    Impl& I = *impl_;
    int page = isHeaderField(f) ? 0 : page_ + pendingTurns_;
    Entry e;
    if (composeEntry(e, {{fieldBox(f), text}}, cfg_.handStyle, mix32(cfg_.seed, 0x20000u + uint32_t(f) * 131u + uint32_t(page))))
        I.addInk(page, e.glyphs);
    I.taken(page, f) = true;
}

void Scoresheet::writeResultInstant(const std::string& result) { writeFieldInstant(Field::Result, result); }

// ---- Pages --------------------------------------------------------------------------------------
bool Scoresheet::pageTurnNeeded(int ply) const { return pageOfPly(ply) > page_ + pendingTurns_; }

void Scoresheet::beginPageTurn() { ++pendingTurns_; }

void Scoresheet::setTurnProgress(float s) {
    if (pendingTurns_ <= 0) return;
    s = clamp(s, 0.0f, 1.0f);
    if (s != impl_->turnS) impl_->flipDirty = true;
    impl_->turnS = s;
}

vec3 Scoresheet::pageCorner(float s) const {
    vec3 p = flipPoint(outerEdgeX(frame_), PAGE_H, clamp(s, 0.0f, 1.0f), FlipParams{page_, frame_.outerSign});
    return frame_.padToWorld(p);
}

void Scoresheet::finishPageTurn() {
    Impl& I = *impl_;
    if (pendingTurns_ <= 0) return;
    --pendingTurns_;
    if (I.gl) {
        flipGrid(I.gx, I.gy, 1.0f, FlipParams{page_, frame_.outerSign}, I.grid);
        MeshData d;
        buildPageMesh(I.gx, I.gy, I.grid, d);
        Mesh m;
        m.upload(d, "scoresheet_turned_page");
        I.turned.push_back(m);
    }
    ++page_;
    I.turnS = -1.0f;
}

m::mat4 Scoresheet::penRestTransform() const { return sheet::penRestTransform(frame_); }

vec3 Scoresheet::writingRest(int nextPly) const {
    Rect r = cellRect(cellOf(std::max(0, nextPly)));
    float x = frame_.outerSign > 0.0f ? PAGE_W - 4.0f : 4.0f;
    return frame_.padToWorld(pageToPad(x, r.cy(), SURFACE_MM));
}

// ---- Frame --------------------------------------------------------------------------------------
void Scoresheet::update() {
    Impl& I = *impl_;
    if (!I.gl) return;
    const std::string& lang = i18n::language();
    if (lang != I.language) {
        I.language = lang;
        I.layerPage[0] = I.layerPage[1] = -1;
    }
    bool work = I.layerPage[page_ % 2] != page_ || I.layerPage[(page_ + 1) % 2] != page_ + 1 ||
                !I.layerPending[0].empty() || !I.layerPending[1].empty() || (!I.entries.empty() && !I.entryReady);
    if (work) font::flushUploads();
    for (int p : {page_, page_ + 1}) {
        int L = p % 2;
        if (I.layerPage[L] != p) I.renderLayer(L, p, true);
        else if (!I.layerPending[L].empty()) I.renderLayer(L, p, false);
    }
    if (!I.entries.empty() && !I.entryReady) {
        I.renderEntry(I.entries.front());
        I.entryReady = true;
    }
    if (I.flipDirty && I.turnS >= 0.0f) {
        flipGrid(I.gx, I.gy, I.turnS, FlipParams{page_, frame_.outerSign}, I.grid);
        buildPageMesh(I.gx, I.gy, I.grid, I.flipData);
        glNamedBufferSubData(I.flipMesh.vbo, 0, GLsizeiptr(I.flipData.vertices.size() * sizeof(Vertex)),
                             I.flipData.vertices.data());
    }
    I.flipDirty = false;
}

void Scoresheet::submit(render::Renderer& r, uint32_t id) const {
    const Impl& I = *impl_;
    if (!I.gl || !g_shared.built) return;
    const mat4 M = frame_.toWorld();
    const float seed = float(cfg_.owner) * 0.5f + 0.13f;
    auto item = [&](const Mesh& mesh, const Material* mat, vec4 inst0) {
        render::DrawItem d;
        d.mesh = &mesh;
        d.material = mat;
        d.model = M;
        d.inst[0] = inst0;
        d.flags = render::DRAW_CAST_SHADOW;
        d.objectId = id++;
        r.submit(d);
    };
    const Material& card = materials::get(MaterialId::ScoresheetCard);
    item(g_shared.board, &card, vec4(0.0f));
    item(g_shared.tape, &card, g_shared.tapeInst);
    item(g_shared.stackEdges, &I.paper, vec4(-1.0f, -1.0f, 0.0f, seed));
    item(g_shared.stackTop, &I.paper, vec4(float((page_ + 1) % 2), -1.0f, 0.0f, seed + float(page_ + 1) * 0.071f));
    // The page on top, lying flat or being turned, with the entry being written.
    float clock = -1.0f, top = 0.0f;
    if (!I.entries.empty() && I.entryReady && I.entries.front().page == page_ && I.now >= 0.0f) {
        clock = I.now;
        top = I.entries.front().top;
    }
    bool turning = pendingTurns_ > 0 && I.turnS >= 0.0f;
    item(turning ? I.flipMesh : g_shared.flatPage, &I.paper, vec4(float(page_ % 2), clock, top, seed + float(page_) * 0.071f));
    for (size_t k = 0; k < I.turned.size(); ++k) item(I.turned[k], &I.paper, vec4(-1.0f, -1.0f, 0.0f, seed + float(k) * 0.071f));
}

void Scoresheet::submitPen(render::Renderer& r, const mat4& penToWorld, uint32_t objectId, const mat4* prev) {
    buildPen();
    for (int k = 0; k < 2; ++k) {
        render::DrawItem d;
        d.mesh = k ? &g_shared.penBody : &g_shared.penMetal;
        d.material = &materials::get(k ? MaterialId::PenBody : MaterialId::PenMetal);
        d.model = penToWorld;
        if (prev) {
            d.prevModel = *prev;
            d.hasPrevModel = true;
        }
        d.flags = render::DRAW_CAST_SHADOW;
        d.objectId = objectId + uint32_t(k);
        r.submit(d);
    }
}

}  // namespace game
