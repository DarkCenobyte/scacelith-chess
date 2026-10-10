#include "renderer.h"
#include "lighting/atmosphere.h"
#include "lighting/lighting_data.h"
#include "lighting/planar.h"
#include "lighting/probes.h"
#include "lighting/shadows.h"
#include "post/postfx.h"
#include "shader.h"
#include "../game/layout.h"
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
            taa = true; motionBlur = false; dof = false; bloom = true; tessellation = false;
            shadowCascades = 2; probeResolution = 64; probeBounces = 1; break;
        case Quality::Medium:
            shadowMapSize = 2048; planarReflections = true; ssao = true; ssr = false; volumetrics = true;
            taa = true; motionBlur = true; dof = false; bloom = true; tessellation = false;
            shadowCascades = 3; probeResolution = 64; probeBounces = 2; break;
        case Quality::High:
            shadowMapSize = 4096; planarReflections = true; ssao = true; ssr = true; volumetrics = true;
            taa = true; motionBlur = true; dof = true; bloom = true; tessellation = true;
            shadowCascades = 3; probeResolution = 128; probeBounces = 2; break;
        case Quality::Ultra:
            shadowMapSize = 4096; planarReflections = true; ssao = true; ssr = true; volumetrics = true;
            taa = true; motionBlur = true; dof = true; bloom = true; tessellation = true; renderScale = 1.0f;
            shadowCascades = 3; probeResolution = 128; probeBounces = 3; break;
    }
}

// ---------------------------------------------------------------------------------------------
Renderer::Renderer()
    : lub_(new LightingUBOData()),
      atmosphere_(new lighting::Atmosphere),
      shadows_(new lighting::SunShadows),
      probes_(new lighting::LightProbes),
      planarRefl_(new lighting::PlanarReflections),
      post_(new PostFX) {}
Renderer::~Renderer() { shutdown(); }

static GLuint g_samplerShadowCmp = 0, g_samplerShadowRaw = 0;

bool Renderer::init(const RenderSettings& s) {
    settings_ = s;
    gpu::ensureBuffer(frameUbo_, sizeof(FrameUBOData));
    gpu::ensureBuffer(drawSsbo_, sizeof(DrawDataGPU) * 256);
    gpu::ensureBuffer(lightSsbo_, sizeof(PointLight) * 16);
    gpu::ensureBuffer(lightingUbo_, sizeof(LightingUBOData));
    *lub_ = LightingUBOData();
    glNamedBufferSubData(lightingUbo_.id, 0, sizeof(LightingUBOData), lub_.get());

    auto fill = [](gpu::Texture& t, const void* px, GLenum fmt, GLenum type) {
        if (t.target == GL_TEXTURE_2D) glTextureSubImage2D(t.id, 0, 0, 0, 1, 1, fmt, type, px);
        else glTextureSubImage3D(t.id, 0, 0, 0, 0, 1, 1, t.depth, fmt, type, px);
    };
    const float black[4 * 6] = {};
    dummy2D_ = gpu::createTexture2D(1, 1, GL_RGBA16F);
    fill(dummy2D_, black, GL_RGBA, GL_FLOAT);
    dummyArray_ = gpu::createTexture2DArray(1, 1, 1, GL_RGBA16F);
    fill(dummyArray_, black, GL_RGBA, GL_FLOAT);
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

    // Split-sum DFG LUT (+ sheen albedo), computed once.
    brdfLut_ = gpu::createTexture2D(128, 128, GL_RGBA16F);
    glObjectLabel(GL_TEXTURE, brdfLut_.id, -1, "brdf.dfg");
    {
        const ShaderProgram& p = shaders::compute("shaders/lighting/dfg_lut.comp");
        if (p.valid()) {
            p.use();
            p.set("uSize", 128);
            glBindImageTexture(0, brdfLut_.id, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            gpu::dispatch2D(128, 128);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        }
    }

    atmosphere_->init();
    shadows_->init(settings_.shadowMapSize, settings_.shadowCascades, settings_.staticShadowCache);
    probes_->init(settings_.probeResolution);
    defaultLightingLayout();

    if (!post_->init()) return false;
    return true;
}

void Renderer::defaultLightingLayout() {
    using namespace layout;
    const float wt = WALL_THICKNESS + 0.1f;
    sceneBounds_ = AABB();
    sceneBounds_.add(vec3(HALL_MIN_X - wt, -0.2f, HALL_MIN_Z - wt));
    sceneBounds_.add(vec3(HALL_MAX_X + wt, HALL_HEIGHT + 0.8f, HALL_MAX_Z + wt));
    AABB regions[3];
    // The finest cascade: the table and the players, seated or standing (anim/stance.h: in front of
    // the chair at |z| 0.70, the head up to about 1.80 m; at an end of the table at |x| 0.88, the
    // shoulders and hands out to about 1.15). Wider than the seated players alone needed (|x|
    // 0.85, 1.70 m high): about 0.83 mm per texel instead of 0.72 at 4096 (1.70 / 1.48 at 2048).
    regions[0].add(vec3(-1.20f, 0.0f, -1.15f));
    regions[0].add(vec3(1.20f, 1.85f, 1.15f));
    regions[1].add(vec3(-3.2f, 0.0f, -3.4f));        // chairs, floor around the table
    regions[1].add(vec3(3.2f, 2.6f, 3.4f));
    regions[2] = sceneBounds_;                        // the whole hall and its thick walls
    shadows_->setRegions(regions, 3);

    AABB interior;
    interior.add(vec3(HALL_MIN_X, 0.0f, HALL_MIN_Z));
    interior.add(vec3(HALL_MAX_X, HALL_HEIGHT, HALL_MAX_Z));
    std::vector<LightProbeDesc> probes;
    LightProbeDesc table;
    table.position = vec3(0.0f, TABLE_TOP_Y + 0.32f, 0.0f);
    table.radius = 1.7f;
    table.innerRadius = 0.95f;
    table.priority = true;
    table.box = interior;
    probes.push_back(table);
    const float xs[3] = {HALL_MIN_X + 2.4f, 0.0f, HALL_MAX_X - 2.4f};
    for (int iz = 0; iz < 4; ++iz)
        for (int ix = 0; ix < 3; ++ix) {
            LightProbeDesc p;
            float z = HALL_MIN_Z + (HALL_MAX_Z - HALL_MIN_Z) * (float(iz) + 0.5f) / 4.0f;
            p.position = vec3(xs[ix], 1.8f, z);
            p.radius = 7.5f;
            p.box = interior;
            probes.push_back(p);
        }
    for (int k = 0; k < 3; ++k) {   // upper row: ceiling, vaults, the top of the windows
        LightProbeDesc p;
        p.position = vec3(0.0f, HALL_HEIGHT - 2.4f, 7.0f * float(k - 1));
        p.radius = 9.0f;
        p.box = interior;
        probes.push_back(p);
    }
    probes_->setProbes(probes);
}

void Renderer::setSceneBounds(const AABB& b) {
    sceneBounds_ = b;
    staticDirty_ = true;
}

void Renderer::setShadowRegions(const AABB* regions, int count) {
    shadows_->setRegions(regions, count);
    staticDirty_ = true;
}

void Renderer::setLightProbes(const std::vector<LightProbeDesc>& probes) {
    std::vector<LightProbeDesc> p = probes;
    for (auto& d : p)
        if (!d.box.valid()) d.box = sceneBounds_;
    probes_->setProbes(p);
    staticDirty_ = true;
}

GLuint Renderer::specularProbes() const { return probes_->baked() ? probes_->specularArray() : dummyCubeArray_.id; }

void Renderer::shutdown() {
    if (!frameUbo_.id) return;
    post_->shutdown();
    destroyTargets();
    shadows_->shutdown();
    probes_->shutdown();
    planarRefl_->shutdown();
    atmosphere_->shutdown();
    brdfLut_.destroy();
    for (auto* t : {&dummy2D_, &dummyArray_, &dummyCubeArray_}) t->destroy();
    frameUbo_.destroy();
    drawSsbo_.destroy();
    lightSsbo_.destroy();
    lightingUbo_.destroy();
    if (g_samplerShadowCmp) glDeleteSamplers(1, &g_samplerShadowCmp);
    if (g_samplerShadowRaw) glDeleteSamplers(1, &g_samplerShadowRaw);
    g_samplerShadowCmp = g_samplerShadowRaw = 0;
    shaders::shutdown();
}

void Renderer::setSettings(const RenderSettings& s) {
    bool resizeNeeded = s.renderScale != settings_.renderScale;
    bool shadowsChanged = s.shadowMapSize != settings_.shadowMapSize || s.shadowCascades != settings_.shadowCascades ||
                          s.staticShadowCache != settings_.staticShadowCache;
    bool probesChanged = s.probeResolution != settings_.probeResolution || s.probeBounces != settings_.probeBounces ||
                         s.lightProbes != settings_.lightProbes;
    bool planarChanged = s.planarReflections != settings_.planarReflections;
    settings_ = s;
    if (shadowsChanged && frameUbo_.id) shadows_->init(settings_.shadowMapSize, settings_.shadowCascades, settings_.staticShadowCache);
    if (probesChanged && frameUbo_.id) {
        std::vector<LightProbeDesc> keep = probes_->probes();
        probes_->init(settings_.probeResolution);
        probes_->setProbes(keep);
    }
    if (resizeNeeded && width_ > 0) { int w = width_, h = height_; width_ = 0; resize(w, h); }
    else if (planarChanged && width_ > 0) allocatePlanar();
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
    rt_.fbTransparent = gpu::createFramebuffer({&rt_.hdr}, &rt_.depth);
    gpu::checkFramebuffer(rt_.fbMain, "main");
    gpu::checkFramebuffer(rt_.fbPrepass, "prepass");
    gpu::checkFramebuffer(rt_.fbTransparent, "transparent");
}

void Renderer::destroyTargets() {
    for (auto* t : {&rt_.hdr, &rt_.depth, &rt_.normalRough, &rt_.specular, &rt_.velocity}) t->destroy();
    rt_.fbMain.destroy();
    rt_.fbPrepass.destroy();
    rt_.fbTransparent.destroy();
}

void Renderer::resize(int w, int h) {
    if (w == width_ && h == height_) return;
    width_ = w;
    height_ = h;
    int rw = std::max(1, int(float(w) * settings_.renderScale)), rh = std::max(1, int(float(h) * settings_.renderScale));
    destroyTargets();
    createTargets(rw, rh);
    allocatePlanar();
    post_->resize(rw, rh);
}

// Planar reflections: half resolution, one array layer per reflector (up to 4), no targets
// while they are off (Low preset).
void Renderer::allocatePlanar() {
    int layers = settings_.planarReflections ? std::max(1, int(planar_.size())) : 0;
    planarRefl_->resize(std::max(1, rt_.w / 2), std::max(1, rt_.h / 2), layers);
}

// ---------------------------------------------------------------------------------------------
static float halton(int i, int b) {
    float f = 1, r = 0;
    while (i > 0) { f /= float(b); r += f * float(i % b); i /= b; }
    return r;
}

static float mieScaleOf(const Environment& env) { return std::max(0.2f, env.turbidity / 2.0f); }

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
    vec3 sunDir = normalize(env.sunDirection);
    vec3 sunLux = env.physicalSky ? atmosphere_->sunIlluminance(sunDir, mieScaleOf(env), env.altitudeKm) * env.sunIntensityScale
                                  : env.sunColor * env.sunIlluminance;
    float sunLum = 0.2126f * sunLux.x + 0.7152f * sunLux.y + 0.0722f * sunLux.z;
    f.sunDirection = vec4(sunDir, 0.00465f);
    f.sunRadiance = vec4(sunLux * exposure, sunLum);
    float skyLux = env.physicalSky ? std::max(sunLum * 0.2f, 2000.0f) * env.skyIntensity : env.skyIlluminance;
    // Frame index wrapped below 2^24 (exact as a float; a multiple of 64 for its &7, >>3 and mod-64 readers).
    f.skyParams = vec4(env.turbidity, skyLux * exposure, env.exposureEV100, float(frameIndex_ & 0xFFFFFFu));
    // Fallback hemisphere ambient (only used when light probes are off / not baked yet).
    f.ambientSky = vec4(vec3(0.55f, 0.65f, 0.85f) * (skyLux * 0.12f / PI) * exposure, 0);
    f.ambientGround = vec4(vec3(0.85f, 0.78f, 0.68f) * (sunLum * 0.035f / PI) * exposure, 0);
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
    float sx = length(d.model.c[0].xyz()), sy = length(d.model.c[1].xyz()), sz = length(d.model.c[2].xyz());
    it.center = c;
    it.radius = length(d.mesh->bounds.extent()) * std::max(sx, std::max(sy, sz)) * 1.02f + 1e-3f;
    items_.push_back(it);

    DrawDataGPU g;
    g.model = d.model;
    g.prevModel = d.hasPrevModel ? d.prevModel : d.model;
    g.normalMatrix = mat4(normalMatrix(d.model));
    for (int i = 0; i < 8; ++i) g.matParams[i] = d.material->params[i];
    for (int i = 0; i < 4; ++i) g.instParams[i] = d.inst[i];
    g.info = vec4(float(hash32(d.objectId * 747796405u + 2891336453u) & 0xFFFFFF) / 16777216.0f,
                  float(d.material->planarReflector), float(d.flags), float(d.objectId));
    g.fade = vec4(clamp(d.opacity, 0.0f, 1.0f), settings_.taa ? 1.0f : 0.0f, 0.0f, 0.0f);
    g.highlight = vec4(d.highlight.xyz(), clamp(d.highlight.w, 0.0f, 1.0f));
    drawData_.push_back(g);
}

void Renderer::addLight(const PointLight& l) { lights_.push_back(l); }

int Renderer::addPlanarReflector(const PlanarReflector& r) {
    if (planar_.size() >= 4) return -1;
    planar_.push_back(r);
    if (planarRefl_->colorArray() && int(planar_.size()) > planarRefl_->layers()) allocatePlanar();
    return int(planar_.size() - 1);
}

void Renderer::uploadFrameUBO(const FrameUBOData& d) {
    glNamedBufferSubData(frameUbo_.id, 0, sizeof(FrameUBOData), &d);
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_FRAME, frameUbo_.id);
}

const ShaderProgram* Renderer::programFor(const Material& mat, PassId pass, bool allowTess) {
    ProgramDesc d;
    d.vs = "shaders/passes/mesh.vert";
    bool tess = mat.tessellated && settings_.tessellation && allowTess;
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
    // Keep every unit the lighting samples (8..15) complete with a dummy of the right type.
    for (int u = TEXUNIT_SHADOW; u <= TEXUNIT_SSR; ++u) glBindTextureUnit(GLuint(u), dummy2D_.id);
    glBindTextureUnit(TEXUNIT_SHADOW, shadows_->depthArray());
    glBindSampler(TEXUNIT_SHADOW, g_samplerShadowCmp);
    glBindTextureUnit(TEXUNIT_SHADOW_DEPTH, shadows_->depthArray());
    glBindSampler(TEXUNIT_SHADOW_DEPTH, g_samplerShadowRaw);
    glBindTextureUnit(TEXUNIT_SPECULAR, specularProbes());
    glBindTextureUnit(TEXUNIT_PLANAR, planarRefl_->colorArray() ? planarRefl_->colorArray() : dummyArray_.id);
    glBindTextureUnit(TEXUNIT_BRDF_LUT, brdfLut_.id);
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_LIGHTING, lightingUbo_.id);
}

void Renderer::drawScene(PassId pass, bool transparents, const DrawFilter& flt) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_DRAWS, drawSsbo_.id);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_LIGHTS, lightSsbo_.id);
    const ShaderProgram* current = nullptr;
    // Opaques are sorted by material: look its program up once per run of items, not per draw.
    const Material* memoMat = nullptr;
    const ShaderProgram* memoProg = nullptr;
    int planarLayer = pass == PassId::Planar ? int(frame_.passInfo.y) : -2;
    for (const Item& it : items_) {
        const Material& mat = *it.d.material;
        if (mat.transparent != transparents) continue;
        if (it.d.flags & flt.skipFlags) continue;
        if ((it.d.flags & flt.requireFlags) != flt.requireFlags) continue;
        if (pass == PassId::Shadow && !(mat.castShadow && (it.d.flags & DRAW_CAST_SHADOW))) continue;
        if (pass == PassId::Planar && mat.planarReflector == planarLayer) continue;  // don't reflect the mirror itself
        if (!(it.d.flags & DRAW_NO_CULL)) {
            if (flt.planeCount > 0 && !gpu::sphereVisible(flt.planes, flt.planeCount, it.center, it.radius)) continue;
            if (flt.minSize > 0.0f && it.radius < flt.minSize * distance(flt.eye, it.center)) continue;
            if (flt.minRadius > 0.0f && it.radius < flt.minRadius) {
                const AABB& b = flt.minRadiusRegion;
                vec3 c = it.center;
                if (c.x >= b.lo.x && c.y >= b.lo.y && c.z >= b.lo.z && c.x <= b.hi.x && c.y <= b.hi.y && c.z <= b.hi.z) continue;
            }
        }
        if (&mat != memoMat) {
            memoProg = programFor(mat, pass, flt.allowTessellation);
            memoMat = &mat;
        }
        const ShaderProgram* p = memoProg;
        if (!p) continue;
        if (p != current) { p->use(); current = p; }
        glProgramUniform1i(p->id, 0, int(it.drawIndex));
        for (int t = 0; t < 8; ++t)
            if (mat.textures[t]) glBindTextureUnit(GLuint(t), mat.textures[t]);
        if (mat.doubleSided || pass == PassId::Shadow) glDisable(GL_CULL_FACE);
        else glEnable(GL_CULL_FACE);
        it.d.mesh->bind();
        bool tess = mat.tessellated && settings_.tessellation && flt.allowTessellation;
        if (tess) glPatchParameteri(GL_PATCH_VERTICES, 3);
        glDrawElements(tess ? GL_PATCHES : GL_TRIANGLES, GLsizei(it.d.mesh->indexCount), GL_UNSIGNED_INT, nullptr);
    }
    glEnable(GL_CULL_FACE);
}

void Renderer::renderSky() {
    const ShaderProgram& p = shaders::fullscreen("shaders/passes/sky.frag");
    if (!p.valid()) return;
    atmosphere_->bindSkyTextures();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);  // only where nothing was drawn (depth == 0)
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    p.use();
    gpu::drawFullscreenTriangle();
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
}

void Renderer::updateLightingUBO() {
    LightingUBOData& l = *lub_;
    float exposure = frame_.exposure.x;
    probes_->fillUBO(l, exposure, settings_.lightProbes);
    float softness = std::max(env_.sunSoftness, 0.05f);
    vec3 toa = lighting::Atmosphere::solarTOA() * env_.sunIntensityScale;
    float toaLum = 0.2126f * toa.x + 0.7152f * toa.y + 0.0722f * toa.z;
    l.sunParams = vec4(frame_.sunDirection.w, std::tan(frame_.sunDirection.w * softness), softness, toaLum * exposure);
    l.sunTOA = vec4(toa * exposure, env_.altitudeKm);
    l.skyParams2 = vec4(clamp(env_.cloudCoverage, 0.0f, 1.0f), env_.time, mieScaleOf(env_), env_.skyIntensity);
    l.lightingMisc = vec4(settings_.specularAA, 0.06f, env_.ambientIntensity, 0.0f);
    glNamedBufferSubData(lightingUbo_.id, 0, GLsizeiptr(LIGHTING_UBO_CPU_SIZE), lub_.get());
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_LIGHTING, lightingUbo_.id);
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

    // ---- Lighting: atmosphere, shadows, probes, planar reflections ---------------------------
    vec3 sunDir = frame_.sunDirection.xyz();
    float mie = mieScaleOf(env_);
    {
        gpu::ProfileScope prof("sky");
        atmosphere_->updateLuts(sunDir, mie, env_.altitudeKm);
        updateLightingUBO();
        uploadFrameUBO(frame_);
        bindGlobalTextures();
    }
    // A frame with nothing submitted (the game's loading screen) bakes neither the static shadow
    // cache nor the probes: they would hold an empty world. staticDirty_ stays set until a frame
    // draws the scene.
    const bool drawsScene = !items_.empty();
    const bool bakeStatic = staticDirty_ && drawsScene;
    shadows_->render(*this, sunDir, std::max(env_.sunSoftness, 0.05f), bakeStatic);
    planarRefl_->prepare(*this);
    updateLightingUBO();
    uploadFrameUBO(frame_);
    bindGlobalTextures();

    // Light probes: bake on the first frame that draws items, on invalidateStatic() and when the
    // sun / sky changed a lot.
    if (settings_.lightProbes && drawsScene) {
        const float key[6] = {sunDir.x, sunDir.y, sunDir.z, mie, env_.cloudCoverage, env_.skyIntensity * env_.sunIntensityScale};
        const float* b = bakeKey_;
        bool sunMoved = key[0] * b[0] + key[1] * b[1] + key[2] * b[2] < std::cos(1.0f * DEG);
        bool skyChanged = std::fabs(key[3] - b[3]) > 0.05f || std::fabs(key[4] - b[4]) > 0.05f ||
                          std::fabs(key[5] - b[5]) > 0.02f * std::max(b[5], 1e-3f);
        if (bakeStatic || !probes_->baked() || sunMoved || skyChanged) {
            probes_->bake(*this, settings_.probeBounces);
            std::memcpy(bakeKey_, key, sizeof(key));
            updateLightingUBO();
            bindGlobalTextures();
        }
    }
    if (drawsScene) staticDirty_ = false;
    planarRefl_->render(*this);
    bindGlobalTextures();

    // Culling planes of the main view.
    vec4 viewPlanes[6];
    DrawFilter mainFilter;
    mainFilter.skipFlags = DRAW_HIDDEN_MAIN;
    mainFilter.planes = viewPlanes;
    mainFilter.planeCount = gpu::frustumPlanes(frame_.viewProjNoJitter, viewPlanes);

    // Prepass
    {
        gpu::DebugGroup pg("prepass");
        gpu::ProfileScope prof("prepass");
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
        drawScene(PassId::Prepass, false, mainFilter);
    }

    PostInputs pin;
    pin.rt = &rt_;
    pin.frame = &frame_;
    pin.frameUbo = frameUbo_.id;
    pin.shadowArray = shadows_->depthArray();
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
    {
        gpu::ProfileScope prof("post.ao");
        post_->computeAO(pin);
    }

    // Opaque forward pass
    {
        gpu::DebugGroup mg("opaque");
        gpu::ProfileScope prof("opaque");
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
        drawScene(PassId::Main, false, mainFilter);
        glDepthMask(GL_TRUE);
    }
    {
        gpu::DebugGroup sg("sky");
        gpu::ProfileScope prof("sky.draw");
        renderSky();
    }
    // Transparents: dst = src0 + dst * src1 (coloured transmittance, dual-source blending).
    {
        gpu::DebugGroup tg("transparent");
        gpu::ProfileScope prof("transparent");
        glBindFramebuffer(GL_FRAMEBUFFER, rt_.fbTransparent.id);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_SRC1_COLOR);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        drawScene(PassId::Main, true, mainFilter);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        // Mesa validates dual-source factors against the draw buffer count even with blending
        // disabled: restore a single-source function for the next MRT passes.
        glBlendFunc(GL_ONE, GL_ZERO);
    }
    {
        gpu::ProfileScope prof("post.resolve");
        post_->resolve(pin);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    gpu::profileEndFrame();
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
