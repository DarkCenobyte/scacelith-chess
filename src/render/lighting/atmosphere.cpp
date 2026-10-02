#include "atmosphere.h"
#include "lighting_data.h"
#include "../material.h"
#include "../shader.h"

using namespace m;

namespace render {
namespace lighting {
namespace {
// Keep in sync with shaders/lighting/atmosphere.glsl (km units).
constexpr float BOTTOM = 6360.0f, TOP = 6460.0f;
const vec3 RAYLEIGH(5.802e-3f, 13.558e-3f, 33.1e-3f);
constexpr float RAYLEIGH_H = 8.0f, MIE_EXT = 4.440e-3f, MIE_H = 1.2f;
const vec3 OZONE(0.650e-3f, 1.881e-3f, 0.085e-3f);

float raySphere(vec3 ro, vec3 rd, float radius) {
    float b = dot(ro, rd), c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0) return -1;
    float s = std::sqrt(disc);
    float t0 = -b - s, t1 = -b + s;
    return t0 > 0 ? t0 : (t1 > 0 ? t1 : -1);
}

vec3 extinction(float h, float mieScale) {
    float dR = std::exp(-h / RAYLEIGH_H);
    float dM = std::exp(-h / MIE_H) * mieScale;
    float dO = std::max(0.0f, 1.0f - std::fabs(h - 25.0f) / 15.0f);
    return RAYLEIGH * dR + vec3(MIE_EXT * dM) + OZONE * dO;
}

void dispatchLut(const char* path, gpu::Texture& out, float mieScale, const char* name) {
    const ShaderProgram& p = shaders::compute(path);
    if (!p.valid()) return;
    gpu::DebugGroup g(name);
    p.use();
    p.set("uMieScale", mieScale);
    glBindImageTexture(0, out.id, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    gpu::dispatch2D(out.width, out.height);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}
}  // namespace

vec3 Atmosphere::solarTOA() {
    // Slightly warm extraterrestrial sun (D65-balanced camera), luminance 128 klux.
    vec3 c(1.0f, 0.975f, 0.95f);
    float lum = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    return c * (128000.0f / lum);
}

vec3 Atmosphere::sunIlluminance(vec3 dir, float mieScale, float altKm) const {
    dir = normalize(dir);
    vec3 ro(0, BOTTOM + altKm, 0);
    // Below the horizon: fade over the solar disk / refraction band instead of a hard cut.
    float horizonFade = smoothstep(-0.02f, 0.01f, dir.y);
    if (horizonFade <= 0.0f) return vec3(0);
    vec3 d = dir;
    if (d.y < 0.0015f) d = normalize(vec3(d.x, 0.0015f, d.z));
    float tTop = raySphere(ro, d, TOP);
    if (tTop <= 0) return vec3(0);
    const int N = 96;
    float dt = tTop / N;
    vec3 od(0);
    for (int i = 0; i < N; ++i) {
        vec3 p = ro + d * ((float(i) + 0.5f) * dt);
        od += extinction(std::max(length(p) - BOTTOM, 0.0f), mieScale) * dt;
    }
    vec3 T(std::exp(-od.x), std::exp(-od.y), std::exp(-od.z));
    return solarTOA() * T * horizonFade;
}

bool Atmosphere::init() {
    transmittance_ = gpu::createTexture2D(256, 64, GL_RGBA16F);
    multiScat_ = gpu::createTexture2D(32, 32, GL_RGBA16F);
    skyView_ = gpu::createTexture2D(192, 108, GL_RGBA16F);
    glObjectLabel(GL_TEXTURE, transmittance_.id, -1, "atmo.transmittance");
    glObjectLabel(GL_TEXTURE, skyView_.id, -1, "atmo.skyview");
    return true;
}

void Atmosphere::shutdown() {
    for (auto* t : {&transmittance_, &multiScat_, &skyView_}) t->destroy();
    lutMie_ = -1.0f;
}

void Atmosphere::updateLuts(vec3 sunDir, float mieScale, float altitudeKm) {
    bool baseDirty = std::fabs(mieScale - lutMie_) > 1e-4f;
    if (baseDirty) {
        dispatchLut("shaders/lighting/transmittance_lut.comp", transmittance_, mieScale, "atmo.transmittance");
        glBindTextureUnit(0, transmittance_.id);
        dispatchLut("shaders/lighting/multiscatter_lut.comp", multiScat_, mieScale, "atmo.multiscatter");
        lutMie_ = mieScale;
    }
    if (baseDirty || length2(sunDir - viewSun_) > 1e-9f || std::fabs(altitudeKm - viewAlt_) > 1e-5f) {
        const ShaderProgram& p = shaders::compute("shaders/lighting/skyview_lut.comp");
        if (p.valid()) {
            gpu::DebugGroup g("atmo.skyview");
            p.use();
            p.set("uMieScale", mieScale);
            p.set("uAltitude", altitudeKm);
            p.set("uSunDir", sunDir.x, sunDir.y, sunDir.z);
            glBindTextureUnit(0, transmittance_.id);
            glBindTextureUnit(1, multiScat_.id);
            glBindImageTexture(0, skyView_.id, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            gpu::dispatch2D(skyView_.width, skyView_.height);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }
        viewSun_ = sunDir;
        viewAlt_ = altitudeKm;
    }
}

void Atmosphere::bindSkyTextures() const {
    glBindTextureUnit(0, transmittance_.id);
    glBindTextureUnit(1, skyView_.id);
}

}  // namespace lighting
}  // namespace render
