// Sky seen through the windows: physically based atmosphere (Hillaire 2020 sky-view LUT), sun disk
// with limb darkening (main view only: reflections and probes get the sun from the analytic
// light), soft procedural clouds. Drawn where depth == 0 (reverse-Z far plane).
#include "shaders/include/common.glsl"
#include "shaders/lighting/lighting_ubo.glsl"
#include "shaders/lighting/atmosphere.glsl"
#include "shaders/lighting/sky_common.glsl"
in vec2 vUV;
layout(binding = 0) uniform sampler2D uSkyTransmittance;
layout(binding = 1) uniform sampler2D uSkyView;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormalRough;
layout(location = 2) out vec4 outSpecular;
layout(location = 3) out vec2 outVelocity;

void main() {
    vec4 p = frame.invViewProj * vec4(vUV * 2.0 - 1.0, 0.5, 1.0);
    vec3 d = normalize(p.xyz / p.w - frame.cameraPos.xyz);
    int pass = int(frame.passInfo.x);
    vec3 c = skyRadiance(uSkyTransmittance, uSkyView, d, pass == PASS_ID_MAIN);
    // Planar reflections store the reflected hit distance (m) in alpha: the sky is "far".
    outColor = vec4(min(c, vec3(60000.0)), pass == PASS_ID_PLANAR ? 1000.0 : 1.0);
    outNormalRough = vec4(0.0, 0.0, 0.0, 1.0);
    outSpecular = vec4(0.0);
    vec4 cur = frame.viewProjNoJitter * vec4(d, 0.0);
    vec4 prev = frame.prevViewProj * vec4(d, 0.0);
    outVelocity = (cur.xy / cur.w - prev.xy / prev.w) * 0.5;
}
