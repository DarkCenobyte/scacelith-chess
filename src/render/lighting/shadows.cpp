#include "shadows.h"
#include "lighting_data.h"
#include "../renderer.h"
#include "../../core/log.h"
#include <cstring>

using namespace m;

namespace render {
namespace lighting {

bool SunShadows::init(int size, int cascades, bool cache) {
    shutdown();
    size_ = size;
    cascades_ = std::max(1, std::min(3, cascades));
    cacheEnabled_ = cache;
    // 16-bit depth: the fitted depth ranges (< ~40 m) give sub-millimetre precision, half the
    // memory and bandwidth of D32F.
    live_ = gpu::createTexture2DArray(size, size, cascades_, GL_DEPTH_COMPONENT16);
    glObjectLabel(GL_TEXTURE, live_.id, -1, "shadow.cascades");
    for (int i = 0; i < cascades_; ++i) liveFbs_.push_back(gpu::createFramebufferLayer(nullptr, 0, &live_, i));
    if (cacheEnabled_) {
        cache_ = gpu::createTexture2DArray(size, size, cascades_, GL_DEPTH_COMPONENT16);
        glObjectLabel(GL_TEXTURE, cache_.id, -1, "shadow.static_cache");
        for (int i = 0; i < cascades_; ++i) cacheFbs_.push_back(gpu::createFramebufferLayer(nullptr, 0, &cache_, i));
    }
    for (bool& v : cacheValid_) v = false;
    return true;
}

void SunShadows::shutdown() {
    for (auto& fb : liveFbs_) fb.destroy();
    for (auto& fb : cacheFbs_) fb.destroy();
    liveFbs_.clear();
    cacheFbs_.clear();
    live_.destroy();
    cache_.destroy();
    for (bool& v : cacheValid_) v = false;
}

void SunShadows::setRegions(const AABB* regions, int count) {
    regionCount_ = std::max(0, std::min(3, count));
    for (int i = 0; i < regionCount_; ++i) regions_[i] = regions[i];
    for (bool& v : cacheValid_) v = false;
}

void SunShadows::fit(Renderer& r, vec3 sunDir) {
    vec3 L = normalize(sunDir);
    // Light "up": world +Y projected (falls back to +X for a sun near the zenith).
    vec3 up = std::fabs(L.y) > 0.98f ? vec3(1, 0, 0) : vec3(0, 1, 0);
    const AABB& scene = r.sceneBounds_;
    for (int c = 0; c < cascades_; ++c) {
        // With 2 cascades use the finest and the coarsest region.
        int ri = regionCount_ <= 0 ? 0 : (cascades_ == 2 && regionCount_ == 3 && c == 1 ? 2 : std::min(c, regionCount_ - 1));
        AABB reg = regionCount_ > 0 ? regions_[ri] : scene;
        vec3 ctr = reg.center();
        mat4 V = lookAt(ctr + L, ctr, up);
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, d0 = 1e30f, d1 = -1e30f;
        for (int k = 0; k < 8; ++k) {
            vec3 p((k & 1) ? reg.hi.x : reg.lo.x, (k & 2) ? reg.hi.y : reg.lo.y, (k & 4) ? reg.hi.z : reg.lo.z);
            vec3 v = transformPoint(V, p);
            x0 = std::min(x0, v.x); x1 = std::max(x1, v.x);
            y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
            d0 = std::min(d0, -v.z); d1 = std::max(d1, -v.z);
            vec3 s((k & 1) ? scene.hi.x : scene.lo.x, (k & 2) ? scene.hi.y : scene.lo.y, (k & 4) ? scene.hi.z : scene.lo.z);
            vec3 w = transformPoint(V, s);
            d0 = std::min(d0, -w.z); d1 = std::max(d1, -w.z);
        }
        // Pad for the widest PCSS kernel (48 texels) plus a margin.
        float ext = std::max(x1 - x0, y1 - y0);
        float pad = ext * (52.0f / float(size_)) + 0.01f;
        x0 -= pad; x1 += pad; y0 -= pad; y1 += pad;
        d0 -= 0.25f; d1 += 0.25f;
        // Snap the window to whole texels of a fixed grid so a slowly moving sun does not crawl.
        float tx = (x1 - x0) / float(size_), ty = (y1 - y0) / float(size_);
        x0 = std::floor(x0 / tx) * tx; x1 = x0 + tx * float(size_);
        y0 = std::floor(y0 / ty) * ty; y1 = y0 + ty * float(size_);
        mat4 P = ortho01(x0, x1, y0, y1, d0, d1);
        vp_[c] = P * V;
        scale_[c] = vec4(x1 - x0, y1 - y0, d1 - d0, std::max(tx, ty));
        center_[c] = ctr;
        radius_[c] = 0.5f * std::max(x1 - x0, y1 - y0);
    }
}

void SunShadows::render(Renderer& r, vec3 sunDir, float softness, bool staticDirty) {
    gpu::DebugGroup g("shadows");
    gpu::ProfileScope prof("shadows");
    fit(r, sunDir);
    FrameUBOData& f = r.frame_;
    LightingUBOData& lub = *r.lub_;
    mat4 bias = translate(vec3(0.5f, 0.5f, 0.0f)) * scale(vec3(0.5f, 0.5f, 1.0f));
    for (int c = 0; c < 4; ++c) {
        if (c < cascades_) {
            f.shadowMatrix[c] = bias * vp_[c];
            f.shadowCascade[c] = vec4(center_[c], radius_[c]);
            lub.shadowScale[c] = scale_[c];
        } else {
            f.shadowMatrix[c] = mat4();
            f.shadowCascade[c] = vec4(0);
            lub.shadowScale[c] = vec4(1);
        }
    }
    f.shadowParams = vec4(float(cascades_), scale_[0].w * 1.5f, 0.0f, softness);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.25f, 2.0f);
    glViewport(0, 0, size_, size_);
    float one = 1.0f;
    for (int c = 0; c < cascades_; ++c) {
        FrameUBOData lf = f;
        lf.viewProj = vp_[c];
        lf.viewProjNoJitter = vp_[c];
        lf.prevViewProj = vp_[c];
        lf.passInfo = vec4(float(PassId::Shadow), float(c), 0, 0);
        lf.resolution = vec4(float(size_), float(size_), 1.0f / float(size_), 1.0f / float(size_));
        lf.cameraPos = vec4(center_[c] + normalize(sunDir) * 50.0f, f.cameraPos.w);
        lf.clipPlane = vec4(0, 0, 0, 1);
        r.uploadFrameUBO(lf);
        vec4 planes[6];
        DrawFilter flt;
        flt.planes = planes;
        flt.planeCount = gpu::frustumPlanes(vp_[c], planes, true);
        // Small dynamic objects (pieces) are covered by the finest cascade: skip them in the
        // coarser ones when they stand inside its region.
        if (c > 0 && regionCount_ > 0) {
            flt.minRadius = scale_[c].w * 8.0f;
            flt.minRadiusRegion = regions_[0];
        }
        if (cacheEnabled_) {
            bool changed = std::memcmp(&cachedVp_[c], &vp_[c], sizeof(mat4)) != 0;
            if (!cacheValid_[c] || changed || staticDirty) {
                gpu::DebugGroup cg("shadow.static");
                glBindFramebuffer(GL_FRAMEBUFFER, cacheFbs_[size_t(c)].id);
                glClearNamedFramebufferfv(cacheFbs_[size_t(c)].id, GL_DEPTH, 0, &one);
                DrawFilter sf = flt;
                sf.requireFlags = DRAW_STATIC;
                sf.minRadius = 0.0f;
                r.drawScene(PassId::Shadow, false, sf);
                cachedVp_[c] = vp_[c];
                cacheValid_[c] = true;
            }
            glCopyImageSubData(cache_.id, GL_TEXTURE_2D_ARRAY, 0, 0, 0, c, live_.id, GL_TEXTURE_2D_ARRAY, 0, 0, 0, c, size_, size_, 1);
            glBindFramebuffer(GL_FRAMEBUFFER, liveFbs_[size_t(c)].id);
            flt.skipFlags = DRAW_STATIC;
            r.drawScene(PassId::Shadow, false, flt);
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, liveFbs_[size_t(c)].id);
            glClearNamedFramebufferfv(liveFbs_[size_t(c)].id, GL_DEPTH, 0, &one);
            r.drawScene(PassId::Shadow, false, flt);
        }
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_DEPTH_CLAMP);
}

}  // namespace lighting
}  // namespace render
