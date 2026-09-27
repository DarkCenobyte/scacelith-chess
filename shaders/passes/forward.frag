// Forward shading pass (main view, planar reflections, probe capture).
#include "shaders/include/common.glsl"
#include "shaders/include/surface.glsl"
#include "shaders/include/noise.glsl"
#pragma material
#include "shaders/include/lighting.glsl"
#include "shaders/passes/fragment_input.glsl"

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormalRough;
layout(location = 2) out vec4 outSpecular;
layout(location = 3) out vec2 outVelocity;

void main() {
    SurfaceInput i = buildSurfaceInput();
    Surface s = defaultSurface(i);
    surface(i, s);
#ifdef MATERIAL_ALPHA_TEST
    if (s.alpha < 0.5) discard;
#endif
    s.normalWS = normalize(s.normalWS);
    s.roughness = clamp(s.roughness, 0.02, 1.0);
    vec3 c = shadeSurface(i, s, draws[vin.draw].info.y);
    c = min(c, vec3(60000.0));
#ifdef MATERIAL_TRANSPARENT
    outColor = vec4(c * s.alpha, s.alpha);
#else
    outColor = vec4(c, 1.0);
#endif
    float rough = s.clearcoat > 0.5 ? min(s.roughness, s.clearcoatRoughness) : s.roughness;
    vec3 nOut = s.clearcoat > 0.5 ? s.clearcoatNormalWS : s.normalWS;
    outNormalRough = vec4(nOut, rough);
    vec3 f0 = mix(vec3(0.16 * s.specular * s.specular), s.albedo, s.metallic);
    if (s.clearcoat > 0.5) f0 = max(f0, vec3(0.04 * s.clearcoat));
    bool planar = draws[vin.draw].info.y >= 0.0 && frame.passInfo.w > draws[vin.draw].info.y;
    outSpecular = vec4(f0, planar ? 0.0 : (rough < 0.6 ? 1.0 : 0.0));
    outVelocity = motionVector();
}
