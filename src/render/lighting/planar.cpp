#include "planar.h"
#include "lighting_data.h"
#include "../renderer.h"
#include "../shader.h"
#include "../../core/log.h"

using namespace m;

namespace render {
namespace lighting {

void PlanarReflections::resize(int w, int h) {
    shutdown();
    w_ = std::max(1, w);
    h_ = std::max(1, h);
    levels_ = std::min(6, gpu::mipCount(w_, h_));
    color_ = gpu::createTexture2DArray(w_, h_, 4, GL_RGBA16F, levels_);
    temp_ = gpu::createTexture2DArray(w_, h_, 4, GL_RGBA16F, levels_);
    depth_ = gpu::createTexture2DArray(w_, h_, 4, GL_DEPTH_COMPONENT32F);
    glObjectLabel(GL_TEXTURE, color_.id, -1, "planar.color");
    for (int i = 0; i < 4; ++i) fbs_.push_back(gpu::createFramebufferLayer(&color_, i, &depth_, i));
}

void PlanarReflections::shutdown() {
    for (auto& fb : fbs_) fb.destroy();
    fbs_.clear();
    color_.destroy();
    temp_.destroy();
    depth_.destroy();
}

void PlanarReflections::prepare(Renderer& r) {
    LightingUBOData& l = *r.lub_;
    for (int i = 0; i < 4; ++i) {
        l.planarInfo[i] = vec4(0);
        active_[i] = false;
    }
    FrameUBOData& f = r.frame_;
    vec3 cam = r.camera_.position;
    for (size_t i = 0; i < r.planar_.size() && i < 4; ++i) {
        const PlanarReflector& pr = r.planar_[i];
        vec3 n = normalize(pr.normal);
        float d = -dot(n, pr.point);
        f.planarPlanes[i] = vec4(n, d);
        if (!pr.enabled || !r.settings_.planarReflections || !color_.id) continue;
        if (dot(n, cam) + d <= 1e-3f) continue;  // camera behind the mirror
        float x0 = 0, y0 = 0, x1 = 1, y1 = 1;
        if (pr.bounds.valid()) {
            bool behind = false;
            float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
            for (int k = 0; k < 8 && !behind; ++k) {
                vec3 p((k & 1) ? pr.bounds.hi.x : pr.bounds.lo.x, (k & 2) ? pr.bounds.hi.y : pr.bounds.lo.y,
                       (k & 4) ? pr.bounds.hi.z : pr.bounds.lo.z);
                vec4 c = f.viewProjNoJitter * vec4(p, 1.0f);
                if (c.w <= 1e-4f) { behind = true; break; }
                float u = c.x / c.w * 0.5f + 0.5f, v = c.y / c.w * 0.5f + 0.5f;
                mnx = std::min(mnx, u); mxx = std::max(mxx, u);
                mny = std::min(mny, v); mxy = std::max(mxy, v);
            }
            if (!behind) {
                x0 = std::max(mnx, 0.0f); x1 = std::min(mxx, 1.0f);
                y0 = std::max(mny, 0.0f); y1 = std::min(mxy, 1.0f);
                if (x0 >= x1 || y0 >= y1) continue;  // off screen
            }
        }
        // Margin for normal distortion and the widest blur level.
        float mx = 0.04f * float(w_) + float(1 << (levels_ - 1)), my = 0.04f * float(h_) + float(1 << (levels_ - 1));
        int sx0 = std::max(0, int(std::floor(x0 * float(w_) - mx))), sy0 = std::max(0, int(std::floor(y0 * float(h_) - my)));
        int sx1 = std::min(w_, int(std::ceil(x1 * float(w_) + mx))), sy1 = std::min(h_, int(std::ceil(y1 * float(h_) + my)));
        scissor_[i][0] = sx0;
        scissor_[i][1] = sy0;
        scissor_[i][2] = std::max(1, sx1 - sx0);
        scissor_[i][3] = std::max(1, sy1 - sy0);
        mat4 R({1 - 2 * n.x * n.x, -2 * n.x * n.y, -2 * n.x * n.z, 0}, {-2 * n.y * n.x, 1 - 2 * n.y * n.y, -2 * n.y * n.z, 0},
               {-2 * n.z * n.x, -2 * n.z * n.y, 1 - 2 * n.z * n.z, 0}, {-2 * d * n.x, -2 * d * n.y, -2 * d * n.z, 1});
        reflect_[i] = R;
        mat4 proj = r.camera_.proj(float(w_) / float(h_));
        f.planarViewProj[i] = proj * (f.view * R);
        active_[i] = true;
        l.planarInfo[i] = vec4(1.0f, float(levels_ - 1), float(w_), float(h_));
    }
}

void PlanarReflections::render(Renderer& r) {
    bool any = false;
    for (bool a : active_) any = any || a;
    if (!any) return;
    gpu::DebugGroup g("planar");
    gpu::ProfileScope prof("planar");
    FrameUBOData& f = r.frame_;
    glEnable(GL_CLIP_DISTANCE0);
    for (int i = 0; i < 4; ++i) {
        if (!active_[i]) continue;
        const PlanarReflector& pr = r.planar_[size_t(i)];
        vec4 plane = f.planarPlanes[i];
        const mat4& R = reflect_[i];
        FrameUBOData rf = f;
        mat4 proj = r.camera_.proj(float(w_) / float(h_));
        rf.view = f.view * R;
        rf.proj = proj;
        rf.viewProj = proj * rf.view;
        rf.viewProjNoJitter = rf.viewProj;
        rf.prevViewProj = rf.viewProj;
        rf.invView = inverse(rf.view);
        rf.invProj = inverse(proj);
        rf.invViewProj = inverse(rf.viewProj);
        vec3 eye = transformPoint(R, r.camera_.position);
        rf.cameraPos = vec4(eye, f.cameraPos.w);
        rf.resolution = vec4(float(w_), float(h_), 1.0f / float(w_), 1.0f / float(h_));
        rf.jitter = vec4(0);
        rf.exposure.w = float(w_) / float(h_);
        rf.clipPlane = vec4(plane.xyz(), plane.w + 0.0005f);
        rf.passInfo = vec4(float(PassId::Planar), float(i), float(r.lights_.size()), float(r.planar_.size()));
        f.passInfo.y = float(i);
        r.uploadFrameUBO(rf);
        const gpu::Framebuffer& fb = fbs_[size_t(i)];
        glBindFramebuffer(GL_FRAMEBUFFER, fb.id);
        glViewport(0, 0, w_, h_);
        const float clearC[4] = {0, 0, 0, 0};
        float zero = 0.0f;
        glDisable(GL_SCISSOR_TEST);
        glClearNamedFramebufferfv(fb.id, GL_COLOR, 0, clearC);
        glClearNamedFramebufferfv(fb.id, GL_DEPTH, 0, &zero);
        glEnable(GL_SCISSOR_TEST);
        glScissor(scissor_[i][0], scissor_[i][1], scissor_[i][2], scissor_[i][3]);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_TRUE);
        vec4 planes[7];
        int np = gpu::frustumPlanes(rf.viewProj, planes);
        planes[np++] = plane;  // only what is in front of the mirror
        DrawFilter flt;
        flt.skipFlags = DRAW_NO_REFLECTION | DRAW_HIDDEN_MAIN;
        flt.planes = planes;
        flt.planeCount = np;
        flt.eye = eye;
        flt.minSize = pr.minObjectSize;
        glFrontFace(GL_CW);  // mirrored winding
        r.drawScene(PassId::Planar, false, flt);
        glFrontFace(GL_CCW);
        r.renderSky();
        glDisable(GL_SCISSOR_TEST);
    }
    f.passInfo.y = 0;
    glDisable(GL_CLIP_DISTANCE0);

    // Gaussian mip chain.
    gpu::DebugGroup bg("planar.blur");
    const ShaderProgram& ph = shaders::compute("shaders/lighting/planar_blur.comp", {"PASS_H"});
    const ShaderProgram& pv = shaders::compute("shaders/lighting/planar_blur.comp", {"PASS_V"});
    if (!ph.valid() || !pv.valid()) return;
    for (int i = 0; i < 4; ++i) {
        if (!active_[i]) continue;
        for (int lv = 1; lv < levels_; ++lv) {
            int sw = std::max(1, w_ >> lv), sh = std::max(1, h_ >> lv);
            ph.use();
            ph.set("uLevel", lv);
            ph.set("uLayer", i);
            glProgramUniform2i(ph.id, ph.loc("uSize"), sw, sh);
            glBindTextureUnit(0, color_.id);
            glBindImageTexture(0, temp_.id, lv, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            gpu::dispatch2D(sw, sh);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            pv.use();
            pv.set("uLevel", lv);
            pv.set("uLayer", i);
            glProgramUniform2i(pv.id, pv.loc("uSize"), sw, sh);
            glBindTextureUnit(0, temp_.id);
            glBindImageTexture(0, color_.id, lv, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            gpu::dispatch2D(sw, sh);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }
    }
}

}  // namespace lighting
}  // namespace render
