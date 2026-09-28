// Forward shading pass (main view, planar reflections, probe capture).
#include "shaders/include/common.glsl"
#include "shaders/include/surface.glsl"
#include "shaders/include/noise.glsl"
#pragma material
#include "shaders/include/lighting.glsl"
#include "shaders/passes/fragment_input.glsl"

#ifdef MATERIAL_TRANSPARENT
// Dual-source blending: dst = src0 + dst * src1 (src1 = coloured transmittance).
layout(location = 0, index = 0) out vec4 outColor;
layout(location = 0, index = 1) out vec4 outTransmittance;
#else
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormalRough;
layout(location = 2) out vec4 outSpecular;
layout(location = 3) out vec2 outVelocity;
#endif

// Geometric specular anti-aliasing (Kaplanyan & Tokuyoshi 2019, Filament variant): widen the lobe
// by the screen-space variance of the normal.
float specularAA(vec3 n, float perceptualRoughness) {
    float strength = lighting.lightingMisc.x;
    if (strength <= 0.0) return perceptualRoughness;
    vec3 du = dFdx(n), dv = dFdy(n);
    float variance = 0.15 * strength * (dot(du, du) + dot(dv, dv));
    float a = perceptualRoughness * perceptualRoughness;
    float kernel = min(2.0 * variance, 0.18);
    return sqrt(sqrt(clamp(a * a + kernel, 0.0, 1.0)));
}

void main() {
    SurfaceInput i = buildSurfaceInput();
    Surface s = defaultSurface(i);
    surface(i, s);
#ifdef MATERIAL_ALPHA_TEST
    if (s.alpha < 0.5) discard;
#endif
    s.normalWS = normalize(s.normalWS);
    s.clearcoatNormalWS = normalize(s.clearcoatNormalWS);
    s.roughness = clamp(s.roughness, 0.02, 1.0);
#ifdef PASS_MAIN
    s.roughness = specularAA(i.normalWS, s.roughness);
    s.clearcoatRoughness = specularAA(i.normalWS, clamp(s.clearcoatRoughness, 0.02, 1.0));
#endif
    float planarLayer = draws[vin.draw].info.y;
    vec3 c = shadeSurface(i, s, planarLayer);
    c = min(c, vec3(60000.0));
#ifdef MATERIAL_TRANSPARENT
    if (s.transmission > 0.0) {
        // Glass contract: the specular reflection is never scaled by alpha; the diffuse
        // (scattering) part is already weighted by 1 - transmission, and the background is
        // multiplied by transmission * (1 - F)^2 * albedo tint.
        outColor = vec4(c, 1.0);
        outTransmittance = vec4(transmittanceOf(i, s), 1.0);
    } else {
        // Plain coverage blending (fades, decals).
        float a = clamp(s.alpha, 0.0, 1.0);
        outColor = vec4(c * a, a);
        outTransmittance = vec4(vec3(1.0 - a), 1.0);
    }
#else
#ifdef PASS_PLANAR
    // Planar reflections keep the distance to the mirror plane in alpha (roughness-aware blur).
    outColor = vec4(c, max(dot(i.positionWS, frame.clipPlane.xyz) + frame.clipPlane.w, 1e-3));
#else
    outColor = vec4(c, 1.0);
#endif
    float rough = s.clearcoat > 0.5 ? min(s.roughness, s.clearcoatRoughness) : s.roughness;
    vec3 nOut = s.clearcoat > 0.5 ? s.clearcoatNormalWS : s.normalWS;
    outNormalRough = vec4(nOut, rough);
    vec3 f0 = mix(vec3(0.16 * s.specular * s.specular), s.albedo, s.metallic);
    if (s.clearcoat > 0.5) f0 = max(f0, vec3(0.04 * s.clearcoat));
    bool planar = planarLayer >= 0.0 && frame.passInfo.w > planarLayer && lighting.planarInfo[int(planarLayer)].x > 0.5;
    outSpecular = vec4(f0, planar ? 0.0 : (rough < 0.6 ? 1.0 : 0.0));
    outVelocity = motionVector();
#endif
}
