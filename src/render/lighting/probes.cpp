#include "probes.h"
#include "lighting_data.h"
#include "../shader.h"
#include "../../core/log.h"
#include <chrono>

using namespace m;

namespace render {
namespace lighting {

bool LightProbes::init(int resolution) {
    shutdown();
    res_ = std::max(16, resolution);
    levels_ = std::max(2, gpu::mipCount(res_, res_) - 2);  // 128 -> 128..4 (6 levels)
    sh_ = gpu::createBuffer(sizeof(vec4) * SH_BUFFER_VEC4S, nullptr, GL_DYNAMIC_STORAGE_BIT);
    std::vector<vec4> zero(SH_BUFFER_VEC4S, vec4(0));
    glNamedBufferSubData(sh_.id, 0, GLsizeiptr(zero.size() * sizeof(vec4)), zero.data());
    depth_ = gpu::createTexture2D(res_, res_, GL_DEPTH_COMPONENT32F);
    glCreateFramebuffers(1, &fb_.id);
    glNamedFramebufferDrawBuffer(fb_.id, GL_COLOR_ATTACHMENT0);
    glNamedFramebufferTexture(fb_.id, GL_DEPTH_ATTACHMENT, depth_.id, 0);
    return true;
}

void LightProbes::shutdown() {
    capture_.destroy();
    specular_.destroy();
    depth_.destroy();
    fb_.destroy();
    sh_.destroy();
    allocated_ = 0;
    baked_ = false;
}

void LightProbes::setProbes(const std::vector<LightProbeDesc>& probes) {
    probes_ = probes;
    if (probes_.size() > size_t(MAX_LIGHT_PROBES)) {
        LOGW("light probes: %d requested, keeping the first %d", int(probes_.size()), MAX_LIGHT_PROBES);
        probes_.resize(size_t(MAX_LIGHT_PROBES));
    }
    baked_ = false;
}

void LightProbes::allocate() {
    int n = std::max(1, int(probes_.size()));
    if (allocated_ == n && capture_.id) return;
    capture_.destroy();
    specular_.destroy();
    capture_ = gpu::createCubemapArray(res_, n, GL_RGBA16F, 0);
    specular_ = gpu::createCubemapArray(res_, n, GL_RGBA16F, levels_);
    glObjectLabel(GL_TEXTURE, capture_.id, -1, "probes.capture");
    glObjectLabel(GL_TEXTURE, specular_.id, -1, "probes.specular");
    allocated_ = n;
}

void LightProbes::fillUBO(LightingUBOData& l, float exposure, bool enabled) const {
    int n = int(probes_.size());
    bool on = enabled && baked_ && n > 0;
    l.probeInfo = vec4(float(on ? n : 0), float(levels_ - 1), bakeExposure_ > 0 ? exposure / bakeExposure_ : 1.0f, on ? 1.0f : 0.0f);
    for (int k = 0; k < MAX_LIGHT_PROBES; ++k) {
        if (k < n) {
            const LightProbeDesc& p = probes_[size_t(k)];
            l.probePos[k] = vec4(p.position, std::max(p.radius, 0.01f));
            l.probeBoxMin[k] = vec4(p.box.lo, p.priority ? 1.0f : 0.0f);
            l.probeBoxMax[k] = vec4(p.box.hi, std::min(p.innerRadius, p.radius * 0.999f));
        } else {
            l.probePos[k] = vec4(0, -1000, 0, 0.01f);
            l.probeBoxMin[k] = vec4(0);
            l.probeBoxMax[k] = vec4(0);
        }
    }
}

void LightProbes::captureProbe(Renderer& r, int k) {
    static const vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const vec3 ups[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    vec3 pos = probes_[size_t(k)].position;
    const float nearZ = 0.05f;
    mat4 P = perspectiveReverseZ(90.0f * DEG, 1.0f, nearZ);
    glViewport(0, 0, res_, res_);
    for (int f = 0; f < 6; ++f) {
        FrameUBOData pf = r.frame_;
        mat4 V = lookAt(pos, pos + dirs[f], ups[f]);
        pf.view = V;
        pf.proj = P;
        pf.viewProj = P * V;
        pf.invView = inverseAffine(V);
        pf.invProj = inverse(P);
        pf.invViewProj = inverse(pf.viewProj);
        pf.prevViewProj = pf.viewProj;
        pf.viewProjNoJitter = pf.viewProj;
        pf.cameraPos = vec4(pos, r.frame_.cameraPos.w);
        pf.resolution = vec4(float(res_), float(res_), 1.0f / float(res_), 1.0f / float(res_));
        pf.jitter = vec4(0);
        pf.exposure.z = nearZ;
        pf.exposure.w = 1.0f;
        pf.clipPlane = vec4(0, 0, 0, 1);
        pf.passInfo = vec4(float(PassId::Probe), float(f), float(r.lights_.size()), float(r.planar_.size()));
        r.uploadFrameUBO(pf);
        glNamedFramebufferTextureLayer(fb_.id, GL_COLOR_ATTACHMENT0, capture_.id, 0, k * 6 + f);
        glBindFramebuffer(GL_FRAMEBUFFER, fb_.id);
        const float zero4[4] = {0, 0, 0, 0};
        float zero = 0.0f;
        glClearNamedFramebufferfv(fb_.id, GL_COLOR, 0, zero4);
        glClearNamedFramebufferfv(fb_.id, GL_DEPTH, 0, &zero);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_TRUE);
        vec4 planes[6];
        DrawFilter flt;
        flt.requireFlags = DRAW_STATIC;
        flt.skipFlags = DRAW_NO_REFLECTION;
        flt.planes = planes;
        flt.planeCount = gpu::frustumPlanes(pf.viewProj, planes);
        flt.eye = pos;
        flt.minSize = 0.004f;
        flt.allowTessellation = false;
        r.drawScene(PassId::Probe, false, flt);
        r.renderSky();
    }
}

void LightProbes::processProbe(int k, int samples) {
    // SH irradiance from the 16x16 mip.
    const ShaderProgram& sh = shaders::compute("shaders/lighting/sh_project.comp");
    int shLod = 0;
    while ((res_ >> shLod) > 16) ++shLod;
    if (sh.valid()) {
        sh.use();
        sh.set("uLayer", k);
        sh.set("uOutSlot", k);
        sh.set("uLod", float(shLod));
        sh.set("uFaceSize", std::max(1, res_ >> shLod));
        glBindTextureUnit(0, capture_.id);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_USER, sh_.id);
        glDispatchCompute(1, 1, 1);
    }
    const ShaderProgram& pf = shaders::compute("shaders/lighting/prefilter.comp");
    if (!pf.valid()) return;
    pf.use();
    glBindTextureUnit(0, capture_.id);
    pf.set("uSrcLayer", k);
    pf.set("uDstLayer", k);
    pf.set("uMipCount", levels_);
    pf.set("uSrcSize", float(res_));
    pf.set("uSrcMaxLod", float(capture_.levels - 1));
    for (int mip = 0; mip < levels_; ++mip) {
        int size = std::max(1, res_ >> mip);
        pf.set("uMip", mip);
        pf.set("uSize", size);
        pf.set("uSamples", mip <= 1 ? samples / 2 : samples);
        glBindImageTexture(0, specular_.id, mip, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(GLuint((size + 7) / 8), GLuint((size + 7) / 8), 6);
    }
}

void LightProbes::bake(Renderer& r, int bounces) {
    if (probes_.empty()) return;
    gpu::DebugGroup g("probes.bake");
    gpu::ProfileScope prof("probes.bake");
    glFinish();
    auto t0 = std::chrono::steady_clock::now();
    allocate();
    int n = int(probes_.size());
    bounces = std::max(1, bounces);
    LightingUBOData& lub = *r.lub_;
    for (int b = 0; b < bounces; ++b) {
        // Bounce 0: no ambient (direct light + sky through the openings); then the previous
        // bounce's probes light the capture.
        fillUBO(lub, r.frame_.exposure.x, true);
        lub.probeInfo = vec4(float(n), float(levels_ - 1), 1.0f, b == 0 ? 2.0f : 1.0f);
        glNamedBufferSubData(r.lightingUbo_.id, 0, GLsizeiptr(sizeof(vec4)), &lub.probeInfo);
        glNamedBufferSubData(r.lightingUbo_.id, GLintptr(offsetof(LightingUBOData, probePos)),
                             GLsizeiptr(offsetof(LightingUBOData, shadowScale) - offsetof(LightingUBOData, probePos)), &lub.probePos[0]);
        glBindTextureUnit(TEXUNIT_SPECULAR, specular_.id);
        for (int k = 0; k < n; ++k) captureProbe(r, k);
        glGenerateTextureMipmap(capture_.id);
        // From bounce 1 the captures sampled specular_, which the prefilter's image stores
        // below overwrite: they must wait for those fetches.
        if (b > 0) glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        bool last = b == bounces - 1;
        for (int k = 0; k < n; ++k) processProbe(k, last ? 48 : 16);
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                        GL_UNIFORM_BARRIER_BIT);
        glCopyNamedBufferSubData(sh_.id, r.lightingUbo_.id, 0, GLintptr(LIGHTING_UBO_PROBE_SH_OFFSET),
                                 GLsizeiptr(sizeof(vec4) * 9 * size_t(n)));
    }
    baked_ = true;
    bakeExposure_ = r.frame_.exposure.x;
    glFinish();
    lastBakeMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOGI("light probes: baked %d probes x %d bounce(s) at %d^2 in %.0f ms", n, bounces, res_, lastBakeMs_);
}

}  // namespace lighting
}  // namespace render
