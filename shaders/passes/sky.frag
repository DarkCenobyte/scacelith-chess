// Sky seen through the windows. Baseline analytic gradient + sun disk; render-lighting replaces
// it with a physically based atmosphere and clouds.
#include "shaders/include/common.glsl"
in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormalRough;
layout(location = 2) out vec4 outSpecular;
layout(location = 3) out vec2 outVelocity;

vec3 skyRadiance(vec3 d) {
    vec3 L = frame.sunDirection.xyz;
    float h = max(d.y, 0.0);
    vec3 zenith = vec3(0.25, 0.45, 0.95), horizon = vec3(0.75, 0.85, 1.0);
    vec3 sky = mix(horizon, zenith, pow(h, 0.5));
    float mu = max(dot(d, L), 0.0);
    sky += vec3(1.0, 0.85, 0.6) * pow(mu, 8.0) * 0.6;
    if (d.y < 0.0) sky = mix(vec3(0.35, 0.38, 0.30), horizon, exp(d.y * 30.0));  // distant ground
    vec3 c = sky * frame.skyParams.y / PI * 0.35;
    if (mu > cos(frame.sunDirection.w)) c += frame.sunRadiance.rgb / (PI * frame.sunDirection.w * frame.sunDirection.w) * 0.02;
    return c;
}

void main() {
    vec4 p = frame.invViewProj * vec4(vUV * 2.0 - 1.0, 0.5, 1.0);
    vec3 d = normalize(p.xyz / p.w - frame.cameraPos.xyz);
    outColor = vec4(min(skyRadiance(d), vec3(60000.0)), 1.0);
    outNormalRough = vec4(0.0, 0.0, 0.0, 1.0);
    outSpecular = vec4(0.0);
    vec4 cur = frame.viewProjNoJitter * vec4(d, 0.0);
    vec4 prev = frame.prevViewProj * vec4(d, 0.0);
    outVelocity = (cur.xy / cur.w - prev.xy / prev.w) * 0.5;
}
