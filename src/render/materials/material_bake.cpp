// Compute-shader bakes of the material library. Everything is generated at startup from code
// (demoscene rule: no texture files). Dispatches are split into 512x512 chunks with flushes so a
// weak iGPU never trips the driver watchdog.
#include "material_bake.h"
#include "../shader.h"
#include "../../core/log.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace materials {
namespace {

size_t textureBytes(const gpu::Texture& t, size_t bytesPerTexel) {
    size_t total = 0;
    int w = t.width, h = t.height;
    for (int l = 0; l < t.levels; ++l) {
        total += size_t(w) * size_t(h) * size_t(t.depth) * bytesPerTexel;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

// Runs a 2D bake shader over one level-0 layer of 'tex'. The shader receives uOffset (ivec2),
// uSize (int) and whatever 'uniforms' sets. Returns false when the program failed to build.
bool dispatchBake(const char* path, const gpu::Texture& tex, int layer, const std::function<void(GLuint)>& uniforms) {
    const ShaderProgram& p = shaders::compute(path);
    if (!p.valid()) {
        LOGE("materials: bake shader %s failed", path);
        return false;
    }
    p.use();
    GLboolean layered = GL_FALSE;
    glBindImageTexture(0, tex.id, 0, layered, layer, GL_WRITE_ONLY, tex.format);
    glProgramUniform1i(p.id, p.loc("uSize"), tex.width);
    if (uniforms) uniforms(p.id);
    const int chunk = 512;
    GLint offLoc = p.loc("uOffset");
    for (int y = 0; y < tex.height; y += chunk)
        for (int x = 0; x < tex.width; x += chunk) {
            glProgramUniform2i(p.id, offLoc, x, y);
            int w = std::min(chunk, tex.width - x), h = std::min(chunk, tex.height - y);
            gpu::dispatch2D(w, h, 8, 8);
            glFlush();
        }
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    return true;
}

void finishTileable(const gpu::Texture& t, float aniso) {
    glGenerateTextureMipmap(t.id);
    gpu::setWrap(t, GL_REPEAT);
    gpu::setFilter(t, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
    gpu::setAnisotropy(t, aniso);
}

gpu::Texture fallback(const uint8_t rgba[4], GLenum target = GL_TEXTURE_2D, int layers = 1) {
    gpu::Texture t = target == GL_TEXTURE_2D_ARRAY ? gpu::createTexture2DArray(1, 1, layers, GL_RGBA8) : gpu::createTexture2D(1, 1, GL_RGBA8);
    for (int l = 0; l < layers; ++l) {
        if (target == GL_TEXTURE_2D_ARRAY) glTextureSubImage3D(t.id, 0, 0, 0, l, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        else glTextureSubImage2D(t.id, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    gpu::setWrap(t, GL_REPEAT);
    return t;
}

}  // namespace

BakedTextures bakeAll() {
    BakedTextures b;
    gpu::DebugGroup dg("materials.bake");
    auto t0 = std::chrono::steady_clock::now();
    bool ok = true;

    // Marble slab for the floor tiles (and the inlay): 2048^2 over 1.6 m = 0.78 mm/texel.
    {
        b.marbleSlab = gpu::createTexture2D(2048, 2048, GL_RGBA8, 0);
        bool r = dispatchBake("shaders/materials/bake/marble_slab.comp", b.marbleSlab, 0, [](GLuint p) {
            glProgramUniform2f(p, glGetUniformLocation(p, "uCells"), 2.0f, 8.0f);
            glProgramUniform4f(p, glGetUniformLocation(p, "uStyle"), 0.008f, 0.004f, 1.1f, 0.37f);
            glProgramUniform1f(p, glGetUniformLocation(p, "uPresence"), 0.45f);
        });
        if (r) finishTileable(b.marbleSlab, 16.0f);
        else { const uint8_t v[4] = {0, 0, 128, 0}; b.marbleSlab.destroy(); b.marbleSlab = fallback(v); ok = false; }
    }
    // Polish micro history: 1024^2 over 0.5 m.
    {
        b.polish = gpu::createTexture2D(1024, 1024, GL_RGBA8, 0);
        bool r = dispatchBake("shaders/materials/bake/polish.comp", b.polish, 0, nullptr);
        if (r) finishTileable(b.polish, 8.0f);
        else { const uint8_t v[4] = {128, 128, 0, 0}; b.polish.destroy(); b.polish = fallback(v); ok = false; }
    }
    // Tapestry motifs: 3 layers (fleur-de-lis field, damask field, border strip).
    {
        b.tapestry = gpu::createTexture2DArray(1024, 1024, 3, GL_RGBA8, 0);
        bool r = true;
        for (int layer = 0; layer < 3 && r; ++layer)
            r = dispatchBake("shaders/materials/bake/tapestry.comp", b.tapestry, layer,
                             [layer](GLuint p) { glProgramUniform1i(p, glGetUniformLocation(p, "uLayer"), layer); });
        if (r) finishTileable(b.tapestry, 8.0f);
        else { const uint8_t v[4] = {0, 0, 0, 128}; b.tapestry.destroy(); b.tapestry = fallback(v, GL_TEXTURE_2D_ARRAY, 3); ok = false; }
    }
    glFinish();
    auto t1 = std::chrono::steady_clock::now();
    b.bakeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    b.bytes = textureBytes(b.marbleSlab, 4) + textureBytes(b.polish, 4) + textureBytes(b.tapestry, 4);
    b.ok = ok;
    LOGI("materials: baked procedural textures in %.1f ms (%.1f MB incl. mips)%s", b.bakeMs, double(b.bytes) / (1024.0 * 1024.0),
         ok ? "" : " -- some bakes FAILED, using fallbacks");
    return b;
}

void destroy(BakedTextures& t) {
    t.marbleSlab.destroy();
    t.polish.destroy();
    t.tapestry.destroy();
    t.ok = false;
}

}  // namespace materials
