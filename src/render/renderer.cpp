#include "renderer.h"
#include "post/postfx.h"
#include "shader.h"
#include "../core/log.h"
#include <algorithm>
#include <cstring>

using namespace m;

namespace render {

static Renderer* g_renderer = nullptr;
Renderer& renderer() { return *g_renderer; }
void setRenderer(Renderer* r) { g_renderer = r; }

// ---------------------------------------------------------------------------------------------
mat4 Camera::view() const {
    mat3 r = toMat3(conjugate(orientation));
    return mat4(r, -(r * position));
}
mat4 Camera::proj(float aspect) const { return perspectiveReverseZ(fovY, aspect, nearZ); }

Ray Camera::screenRay(float px, float py, int w, int h) const {
    float aspect = float(w) / float(h);
    float t = std::tan(fovY * 0.5f);
    float x = (2.0f * (px + 0.5f) / float(w) - 1.0f) * t * aspect;
    float y = (1.0f - 2.0f * (py + 0.5f) / float(h)) * t;
    vec3 d = normalize(forward() + right() * x + up() * y);
    return {position, d};
}

void Camera::lookAt(vec3 target, vec3 upHint) {
    // Our camera looks down local -Z, lookRotation maps local +Z: look away from the target.
    orientation = lookRotation(position - target, upHint);
}

void RenderSettings::applyPreset(Quality q) {
    quality = q;
    switch (q) {
        case Quality::Low:
            shadowMapSize = 2048; planarReflections = false; ssao = true; ssr = false; volumetrics = false;
            taa = true; motionBlur = false; dof = false; bloom = true; tessellation = false; break;
        case Quality::Medium:
            shadowMapSize = 2048; planarReflections = true; ssao = true; ssr = false; volumetrics = true;
            taa = true; motionBlur = true; dof = false; bloom = true; tessellation = false; break;
        case Quality::High:
            shadowMapSize = 4096; planarReflections = true; ssao = true; ssr = true; volumetrics = true;
            taa = true; motionBlur = true; dof = true; bloom = true; tessellation = true; break;
        case Quality::Ultra:
            shadowMapSize = 4096; planarReflections = true; ssao = true; ssr = true; volumetrics = true;
            taa = true; motionBlur = true; dof = true; bloom = true; tessellation = true; renderScale = 1.0f; break;
    }
}

// ---------------------------------------------------------------------------------------------
Renderer::Renderer() : post_(new PostFX) {}
Renderer::~Renderer() { shutdown(); }

static GLuint g_samplerShadowCmp = 0, g_samplerShadowRaw = 0;

bool Renderer::init(const RenderSettings& s) {
    settings_ = s;
    gpu::ensureBuffer(frameUbo_, sizeof(FrameUBOData));
    gpu::ensureBuffer(drawSsbo_, sizeof(DrawDataGPU) * 256);
    gpu::ensureBuffer(lightSsbo_, sizeof(PointLight) * 16);

    auto fill = [](gpu::Texture& t, const void* px, GLenum fmt, GLenum type) {
        if (t.target == GL_TEXTURE_2D) glTextureSubImage2D(t.id, 0, 0, 0, 1, 1, fmt, type, px);
        else glTextureSubImage3D(t.id, 0, 0, 0, 0, 1, 1, t.depth, fmt, type, px);
    };
    const float black[4 * 6] = {};
    dummy2D_ = gpu::createTexture2D(1, 1, GL_RGBA16F);
    fill(dummy2D_, black, GL_RGBA, GL_FLOAT);
    dummyArray_ = gpu::createTexture2DArray(1, 1, 1, GL_RGBA16F);
    fill(dummyArray_, black, GL_RGBA, GL_FLOAT);
    dummy3D_ = gpu::createTexture3D(1, 1, 1, GL_RGBA16F);
    glTextureSubImage3D(dummy3D_.id, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, black);
    dummyCube_ = gpu::createCubemap(1, GL_RGBA16F);
    fill(dummyCube_, black, GL_RGBA, GL_FLOAT);
    dummyCubeArray_ = gpu::createCubemapArray(1, 1, GL_RGBA16F);
    fill(dummyCubeArray_, black, GL_RGBA, GL_FLOAT);

    glCreateSamplers(1, &g_samplerShadowCmp);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glCreateSamplers(1, &g_samplerShadowRaw);
    glSamplerParameteri(g_samplerShadowRaw, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(g_samplerShadowRaw, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(g_samplerShadowRaw, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_samplerShadowRaw, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    shadowCascades_ = 2;
    shadowArray_ = gpu::createTexture2DArray(settings_.shadowMapSize, settings_.shadowMapSize, shadowCascades_, GL_DEPTH_COMPONENT32F);
    for (int i = 0; i < shadowCascades_; ++i) shadowFbs_.push_back(gpu::createFramebufferLayer(nullptr, 0, &shadowArray_, i));

    if (!post_->init()) return false;
    return true;
}

void Renderer::shutdown() {
    if (!frameUbo_.id) return;
    post_->shutdown();
    destroyTargets();
    for (auto& fb : shadowFbs_) fb.destroy();
    shadowFbs_.clear();
    shadowArray_.destroy();
    for (auto& fb : planarFbs_) fb.destroy();
    planarFbs_.clear();
    planarColor_.destroy();
    planarDepth_.destroy();
    for (auto* t : {&dummy2D_, &dummyArray_, &dummyCube_, &dummyCubeArray_, &dummy3D_, &dummyShadow_}) t->destroy();
    frameUbo_.destroy();
    drawSsbo_.destroy();
    lightSsbo_.destroy();
    shaders::shutdown();
}

void Renderer::setSettings(const RenderSettings& s) {
    bool resizeNeeded = s.renderScale != settings_.renderScale;
    settings_ = s;
    if (resizeNeeded && width_ > 0) { int w = width_, h = height_; width_ = 0; resize(w, h); }
}

void Renderer::createTargets(int w, int h) {
    rt_.w = w;
    rt_.h = h;
    rt_.hdr = gpu::createTexture2D(w, h, GL_RGBA16F);
    rt_.depth = gpu::createTexture2D(w, h, GL_DEPTH_COMPONENT32F);
    rt_.normalRough = gpu::createTexture2D(w, h, GL_RGBA16F);
    rt_.specular = gpu::createTexture2D(w, h, GL_RGBA8);
    rt_.velocity = gpu::createTexture2D(w, h, GL_RG16F);
    rt_.fbMain = gpu::createFramebuffer({&rt_.hdr, &rt_.normalRough, &rt_.specular, &rt_.velocity}, &rt_.depth);
    rt_.fbPrepass = gpu::createFramebuffer({&rt_.normalRough, &rt_.velocity}, &rt_.depth);
    gpu::checkFramebuffer(rt_.fbMain, "main");
    gpu::checkFramebuffer(rt_.fbPrepass, "prepass");
}

void Renderer::destroyTargets() {
    for (auto* t : {&rt_.hdr, &rt_.depth, &rt_.normalRough, &rt_.specular, &rt_.velocity}) t->destroy();
    rt_.fbMain.destroy();
    rt_.fbPrepass.destroy();
}

void Renderer::resize(int w, int h) {
    if (w == width_ && h == height_) return;
    width_ = w;
    height_ = h;
    int rw = std::max(1, int(float(w) * settings_.renderScale)), rh = std::max(1, int(float(h) * settings_.renderScale));
    destroyTargets();
    createTargets(rw, rh);
    // Planar reflections: one array layer per reflector, allocated for up to 4.
    for (auto& fb : planarFbs_) fb.destroy();
    planarFbs_.clear();
    planarColor_.destroy();
    planarDepth_.destroy();
    planarW_ = std::max(1, rw / 2);
    planarH_ = std::max(1, rh / 2);
    planarColor_ = gpu::createTexture2DArray(planarW_, planarH_, 4, GL_RGBA16F, 0);
    planarDepth_ = gpu::createTexture2DArray(planarW_, planarH_, 4, GL_DEPTH_COMPONENT32F);
    for (int i = 0; i < 4; ++i) planarFbs_.push_back(gpu::createFramebufferLayer(&planarColor_, i, &planarDepth_, i));
    post_->resize(rw, rh);
}

// ---------------------------------------------------------------------------------------------
static float halton(int i, int b) {
    float f = 1, r = 0;
    while (i > 0) { f /= float(b); r += f * float(i % b); i /= b; }
    return r;
}

void Renderer::beginFrame(const Camera& cam, const Environment& env, float dt) {
    camera_ = cam;
    env_ = env;
    dt_ = dt;
    items_.clear();
    drawData_.clear();
    lights_.clear();
    ++frameIndex_;

    int w = rt_.w, h = rt_.h;
    float aspect = float(w) / float(h);
    mat4 view = cam.view();
    mat4 projNoJ = cam.proj(aspect);
    vec2 jitter(0, 0);
    if (settings_.taa) {
        int k = int(frameIndex_ % 8) + 1;
        jitter = vec2((halton(k, 2) - 0.5f) * 2.0f / float(w), (halton(k, 3) - 0.5f) * 2.0f / float(h));
    }
    mat4 proj = projNoJ;
    // ndc' = ndc + jitter (clip.w = -z_view, so the offset goes negated into column 2).
    proj.c[2].x -= jitter.x;
    proj.c[2].y -= jitter.y;

    FrameUBOData& f = frame_;
    f.view = view;
    f.proj = proj;
    f.viewProj = proj * view;
    f.invView = inverseAffine(view);
    f.invProj = inverse(proj);
    f.invViewProj = inverse(f.viewProj);
    f.viewProjNoJitter = projNoJ * view;
    f.prevViewProj = frameIndex_ > 1 ? prevViewProj_ : f.viewProjNoJitter;
    f.cameraPos = vec4(cam.position, env.time);
    f.resolution = vec4(float(w), float(h), 1.0f / float(w), 1.0f / float(h));
    f.jitter = vec4(jitter.x, jitter.y, prevJitter_.x, prevJitter_.y);
    float exposure = 1.0f / (1.2f * std::pow(2.0f, env.exposureEV100));
    f.exposure = vec4(exposure, 1.0f / exposure, cam.nearZ, aspect);
    f.sunDirection = vec4(normalize(env.sunDirection), 0.00465f);
    f.sunRadiance = vec4(env.sunColor * env.sunIlluminance * exposure, env.sunIlluminance);
    f.skyParams = vec4(env.turbidity, env.skyIlluminance * exposure, env.exposureEV100, float(frameIndex_));
    // Fallback hemisphere ambient (only used until probes exist): sky ~ bluish, ground ~ warm marble bounce.
    f.ambientSky = vec4(vec3(0.55f, 0.65f, 0.85f) * (env.skyIlluminance * 0.12f / PI) * exposure, 0);
    f.ambientGround = vec4(vec3(0.85f, 0.78f, 0.68f) * (env.sunIlluminance * 0.035f / PI) * exposure, 0);
    f.clipPlane = vec4(0, 0, 0, 1);
    f.passInfo = vec4(0, 0, 0, float(planar_.size()));

    prevViewProj_ = f.viewProjNoJitter;
    prevJitter_ = jitter;
}

void Renderer::submit(const DrawItem& d) {
    if (!d.mesh || !d.material || d.mesh->indexCount == 0) return;
    Item it;
    it.d = d;
    it.drawIndex = uint32_t(drawData_.size());
    vec3 c = transformPoint(d.model, d.mesh->bounds.center());
    it.viewDepth = -transformPoint(frame_.view, c).z;
    items_.push_back(it);

    DrawDataGPU g;
    g.model = d.model;
    g.prevModel = d.hasPrevModel ? d.prevModel : d.model;
    g.normalMatrix = mat4(normalMatrix(d.model));
    for (int i = 0; i < 8; ++i) g.matParams[i] = d.material->params[i];
    for (int i = 0; i < 4; ++i) g.instParams[i] = d.inst[i];
    g.info = vec4(float(hash32(d.objectId * 747796405u + 2891336453u) & 0xFFFFFF) / 16777216.0f,
                  float(d.material->planarReflector), float(d.flags), float(d.objectId));
    drawData_.push_back(g);
}

void Renderer::addLight(const PointLight& l) { lights_.push_back(l); }

int Renderer::addPlanarReflector(const PlanarReflector& r) {
    if (planar_.size() >= 4) return -1;
    planar_.push_back(r);
    return int(planar_.size() - 1);
}

void Renderer::uploadFrameUBO(const FrameUBOData& d) {
    glNamedBufferSubData(frameUbo_.id, 0, sizeof(FrameUBOData), &d);
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_FRAME, frameUbo_.id);
}

const ShaderProgram* Renderer::programFor(const Material& mat, PassId pass) {
    ProgramDesc d;
    d.vs = "shaders/passes/mesh.vert";
    bool tess = mat.tessellated && settings_.tessellation;
    if (tess) {
        d.tcs = "shaders/passes/mesh.tesc";
        d.tes = "shaders/passes/mesh.tese";
        d.defines.push_back("MESH_TESSELLATED");
    }
    d.material = mat.surface;
    d.displacement = mat.displacement;
    switch (pass) {
        case PassId::Main: d.fs = "shaders/passes/forward.frag"; d.defines.push_back("PASS_MAIN"); break;
        case PassId::Planar: d.fs = "shaders/passes/forward.frag"; d.defines.push_back("PASS_PLANAR"); break;
        case PassId::Probe: d.fs = "shaders/passes/forward.frag"; d.defines.push_back("PASS_PROBE"); break;
        case PassId::Prepass: d.fs = "shaders/passes/prepass.frag"; d.defines.push_back("PASS_PREPASS"); break;
        case PassId::Shadow: d.fs = "shaders/passes/shadow.frag"; d.defines.push_back("PASS_SHADOW"); break;
    }
    if (mat.doubleSided) d.defines.push_back("MATERIAL_DOUBLE_SIDED");
    if (mat.transparent) d.defines.push_back("MATERIAL_TRANSPARENT");
    for (auto& def : mat.defines) d.defines.push_back(def);
    const ShaderProgram& p = shaders::get(d);
    return p.valid() ? &p : nullptr;
}

void Renderer::bindGlobalTextures() {
    // Keep every reserved unit complete with a dummy of the right type.
    for (int u = TEXUNIT_SHADOW; u < TEXUNIT_COUNT; ++u) glBindTextureUnit(GLuint(u), dummy2D_.id);
    glBindTextureUnit(TEXUNIT_SHADOW, shadowArray_.id);
    glBindSampler(TEXUNIT_SHADOW, g_samplerShadowCmp);
    glBindTextureUnit(TEXUNIT_SHADOW_DEPTH, shadowArray_.id);
    glBindSampler(TEXUNIT_SHADOW_DEPTH, g_samplerShadowRaw);
    glBindTextureUnit(TEXUNIT_IRRADIANCE, dummy2D_.id);
    glBindTextureUnit(TEXUNIT_SPECULAR, dummyCubeArray_.id);
    glBindTextureUnit(TEXUNIT_PLANAR, planarColor_.id ? planarColor_.id : dummyArray_.id);
    glBindTextureUnit(TEXUNIT_SKY, dummyCube_.id);
    glBindTextureUnit(TEXUNIT_VOLUMETRIC, dummy3D_.id);
    glBindTextureUnit(TEXUNIT_NOISE, dummyArray_.id);
}

void Renderer::drawScene(PassId pass, bool transparents, uint32_t skipFlags) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_DRAWS, drawSsbo_.id);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_LIGHTS, lightSsbo_.id);
    const ShaderProgram* current = nullptr;
    int planarLayer = pass == PassId::Planar ? int(frame_.passInfo.y) : -2;
    for (const Item& it : items_) {
        const Material& mat = *it.d.material;
        if (mat.transparent != transparents) continue;
        if (it.d.flags & skipFlags) continue;
        if (pass == PassId::Shadow && !(mat.castShadow && (it.d.flags & DRAW_CAST_SHADOW))) continue;
        if (pass == PassId::Planar && mat.planarReflector == planarLayer) continue;  // don't reflect the mirror itself
        const ShaderProgram* p = programFor(mat, pass);
        if (!p) continue;
        if (p != current) { p->use(); current = p; }
        glProgramUniform1i(p->id, 0, int(it.drawIndex));
        for (int t = 0; t < 8; ++t)
            if (mat.textures[t]) glBindTextureUnit(GLuint(t), mat.textures[t]);
        if (mat.doubleSided || pass == PassId::Shadow) glDisable(GL_CULL_FACE);
        else glEnable(GL_CULL_FACE);
        it.d.mesh->bind();
        bool tess = mat.tessellated && settings_.tessellation;
        if (tess) glPatchParameteri(GL_PATCH_VERTICES, 3);
        glDrawElements(tess ? GL_PATCHES : GL_TRIANGLES, GLsizei(it.d.mesh->indexCount), GL_UNSIGNED_INT, nullptr);
    }
    glEnable(GL_CULL_FACE);
}

// ---------------------------------------------------------------------------------------------
void Renderer::renderShadows() {
    gpu::DebugGroup g("shadows");
    // Fixed cascades (the camera barely moves): 0 = table and players, 1 = the whole hall.
    const vec3 centers[2] = {vec3(0, 0.95f, 0), vec3(0, 4.0f, 0)};
    const float radii[2] = {1.15f, 17.0f};
    vec3 L = normalize(env_.sunDirection);
    vec3 up = std::fabs(L.y) > 0.95f ? vec3(1, 0, 0) : vec3(0, 1, 0);
    FrameUBOData sf = frame_;
    for (int c = 0; c < shadowCascades_; ++c) {
        float R = radii[c];
        vec3 eye = centers[c] + L * (R * 2.5f);
        mat4 v = lookAt(eye, centers[c], up);
        // Snap the light-space origin to texel increments for stability.
        float texel = 2.0f * R / float(settings_.shadowMapSize);
        vec3 o = transformPoint(v, vec3(0));
        v.c[3].x -= std::fmod(o.x, texel);
        v.c[3].y -= std::fmod(o.y, texel);
        mat4 p = ortho01(-R, R, -R, R, 0.05f, R * 5.0f);
        mat4 vp = p * v;
        mat4 bias = translate(vec3(0.5f, 0.5f, 0.0f)) * scale(vec3(0.5f, 0.5f, 1.0f));
        frame_.shadowMatrix[c] = bias * vp;
        frame_.shadowCascade[c] = vec4(centers[c], R);
        sf.shadowMatrix[c] = frame_.shadowMatrix[c];
        sf.shadowCascade[c] = frame_.shadowCascade[c];
    }
    frame_.shadowParams = vec4(float(shadowCascades_), 0.0025f, 0.0f, 1.0f);
    sf.shadowParams = frame_.shadowParams;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.0f);
    glViewport(0, 0, settings_.shadowMapSize, settings_.shadowMapSize);
    for (int c = 0; c < shadowCascades_; ++c) {
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFbs_[size_t(c)].id);
        float one = 1.0f;
        glClearNamedFramebufferfv(shadowFbs_[size_t(c)].id, GL_DEPTH, 0, &one);
        // The shadow pass uses viewProj = light matrix (without the uv bias).
        FrameUBOData lf = sf;
        mat4 unbias = scale(vec3(2.0f, 2.0f, 1.0f)) * translate(vec3(-0.5f, -0.5f, 0.0f));
        lf.viewProj = unbias * frame_.shadowMatrix[c];
        lf.viewProjNoJitter = lf.viewProj;
        lf.prevViewProj = lf.viewProj;
        lf.passInfo = vec4(float(PassId::Shadow), float(c), 0, 0);
        lf.cameraPos = vec4(centers[c] + L * (radii[c] * 2.5f), frame_.cameraPos.w);
        uploadFrameUBO(lf);
        drawScene(PassId::Shadow, false, 0);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
}

void Renderer::renderPlanarReflections() {
    if (!settings_.planarReflections || planar_.empty()) return;
    gpu::DebugGroup g("planar");
    glEnable(GL_CLIP_DISTANCE0);
    for (size_t i = 0; i < planar_.size() && i < 4; ++i) {
        const PlanarReflector& pr = planar_[i];
        vec3 n = normalize(pr.normal);
        float d = -dot(n, pr.point);
        frame_.planarPlanes[i] = vec4(n, d);
        if (!pr.enabled) continue;
        // Reflection matrix about the plane.
        mat4 R({1 - 2 * n.x * n.x, -2 * n.x * n.y, -2 * n.x * n.z, 0}, {-2 * n.y * n.x, 1 - 2 * n.y * n.y, -2 * n.y * n.z, 0},
               {-2 * n.z * n.x, -2 * n.z * n.y, 1 - 2 * n.z * n.z, 0}, {-2 * d * n.x, -2 * d * n.y, -2 * d * n.z, 1});
        FrameUBOData rf = frame_;
        float aspect = float(planarW_) / float(planarH_);
        mat4 proj = camera_.proj(aspect);
        rf.view = frame_.view * R;
        rf.proj = proj;
        rf.viewProj = proj * rf.view;
        rf.viewProjNoJitter = rf.viewProj;
        rf.prevViewProj = rf.viewProj;
        rf.invView = inverse(rf.view);
        rf.invProj = inverse(proj);
        rf.invViewProj = inverse(rf.viewProj);
        rf.cameraPos = vec4(transformPoint(R, camera_.position), frame_.cameraPos.w);
        rf.resolution = vec4(float(planarW_), float(planarH_), 1.0f / planarW_, 1.0f / planarH_);
        rf.clipPlane = vec4(n, d + 0.0005f);
        rf.passInfo = vec4(float(PassId::Planar), float(i), float(lights_.size()), float(planar_.size()));
        frame_.planarViewProj[i] = rf.viewProj;
        frame_.passInfo.y = float(i);
        uploadFrameUBO(rf);
        const gpu::Framebuffer& fb = planarFbs_[i];
        glBindFramebuffer(GL_FRAMEBUFFER, fb.id);
        glViewport(0, 0, planarW_, planarH_);
        float zero = 0.0f;
        const float clearC[4] = {0, 0, 0, 0};
        glClearNamedFramebufferfv(fb.id, GL_COLOR, 0, clearC);
        glClearNamedFramebufferfv(fb.id, GL_DEPTH, 0, &zero);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_TRUE);
        glFrontFace(GL_CW);  // mirrored winding
        drawScene(PassId::Planar, false, DRAW_NO_REFLECTION | DRAW_HIDDEN_MAIN);
        glFrontFace(GL_CCW);
        renderSky();
    }
    frame_.passInfo.y = 0;
    glDisable(GL_CLIP_DISTANCE0);
    glGenerateTextureMipmap(planarColor_.id);
}

void Renderer::renderSky() {
    const ShaderProgram& p = shaders::fullscreen("shaders/passes/sky.frag");
    if (!p.valid()) return;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);  // only where nothing was drawn (depth == 0)
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    p.use();
    gpu::drawFullscreenTriangle();
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
}

void Renderer::endFrame() {
    gpu::DebugGroup g("frame");
    frame_.passInfo = vec4(0, 0, float(lights_.size()), float(planar_.size()));
    gpu::ensureBuffer(drawSsbo_, std::max<size_t>(1, drawData_.size()) * sizeof(DrawDataGPU));
    if (!drawData_.empty()) glNamedBufferSubData(drawSsbo_.id, 0, GLsizeiptr(drawData_.size() * sizeof(DrawDataGPU)), drawData_.data());
    gpu::ensureBuffer(lightSsbo_, std::max<size_t>(1, lights_.size()) * sizeof(PointLight));
    if (!lights_.empty()) glNamedBufferSubData(lightSsbo_.id, 0, GLsizeiptr(lights_.size() * sizeof(PointLight)), lights_.data());

    // Sort opaques by program/material to limit state changes, transparents back to front.
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        if (a.d.material->transparent != b.d.material->transparent) return !a.d.material->transparent;
        if (a.d.material->transparent) return a.viewDepth > b.viewDepth;
        if (a.d.material != b.d.material) return a.d.material < b.d.material;
        return a.viewDepth < b.viewDepth;
    });

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);

    bindGlobalTextures();
    renderShadows();
    bindGlobalTextures();
    renderPlanarReflections();

    // Prepass
    {
        gpu::DebugGroup pg("prepass");
        frame_.passInfo.x = float(PassId::Prepass);
        uploadFrameUBO(frame_);
        glBindFramebuffer(GL_FRAMEBUFFER, rt_.fbPrepass.id);
        glViewport(0, 0, rt_.w, rt_.h);
        const float zero4[4] = {0, 0, 0, 0};
        float zero = 0.0f;
        glClearNamedFramebufferfv(rt_.fbPrepass.id, GL_COLOR, 0, zero4);
        glClearNamedFramebufferfv(rt_.fbPrepass.id, GL_COLOR, 1, zero4);
        glClearNamedFramebufferfv(rt_.fbPrepass.id, GL_DEPTH, 0, &zero);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_TRUE);
        drawScene(PassId::Prepass, false, DRAW_HIDDEN_MAIN);
    }

    PostInputs pin;
    pin.rt = &rt_;
    pin.frame = &frame_;
    pin.frameUbo = frameUbo_.id;
    pin.shadowArray = shadowArray_.id;
    pin.dt = dt_;
    pin.backbufferW = width_;
    pin.backbufferH = height_;
    pin.quality = int(settings_.quality);  // render-post: sample-count preset (PostInputs::quality)
    post_->settings.ssao = settings_.ssao;
    post_->settings.ssr = settings_.ssr;
    post_->settings.volumetrics = settings_.volumetrics;
    post_->settings.taa = settings_.taa;
    post_->settings.motionBlur = settings_.motionBlur;
    post_->settings.dof = settings_.dof;
    post_->settings.bloom = settings_.bloom;
    post_->settings.fade = fade;
    post_->computeAO(pin);

    // Opaque forward pass
    {
        gpu::DebugGroup mg("opaque");
        frame_.passInfo.x = float(PassId::Main);
        uploadFrameUBO(frame_);
        glBindFramebuffer(GL_FRAMEBUFFER, rt_.fbMain.id);
        glViewport(0, 0, rt_.w, rt_.h);
        const float zero4[4] = {0, 0, 0, 0};
        glClearNamedFramebufferfv(rt_.fbMain.id, GL_COLOR, 0, zero4);
        glClearNamedFramebufferfv(rt_.fbMain.id, GL_COLOR, 2, zero4);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GEQUAL);
        glDepthMask(GL_FALSE);
        drawScene(PassId::Main, false, DRAW_HIDDEN_MAIN);
        glDepthMask(GL_TRUE);
        renderSky();
    }
    // Transparents
    {
        gpu::DebugGroup tg("transparent");
        glEnable(GL_BLEND);
        glBlendFunci(0, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied
        glDisablei(GL_BLEND, 1);
        glDisablei(GL_BLEND, 2);
        glDisablei(GL_BLEND, 3);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        GLenum onlyColor[] = {GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE, GL_NONE};
        glNamedFramebufferDrawBuffers(rt_.fbMain.id, 4, onlyColor);
        drawScene(PassId::Main, true, DRAW_HIDDEN_MAIN);
        GLenum all[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
        glNamedFramebufferDrawBuffers(rt_.fbMain.id, 4, all);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    post_->resolve(pin);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
}

void Renderer::readBackbuffer(std::vector<uint8_t>& rgb, int& w, int& h) {
    w = width_;
    h = height_;
    std::vector<uint8_t> tmp(size_t(w) * size_t(h) * 3);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, tmp.data());
    rgb.resize(tmp.size());
    for (int y = 0; y < h; ++y) std::memcpy(&rgb[size_t(y) * size_t(w) * 3], &tmp[size_t(h - 1 - y) * size_t(w) * 3], size_t(w) * 3);
}

}  // namespace render
