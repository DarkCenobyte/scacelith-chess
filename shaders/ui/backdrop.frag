// Dark veined-marble backdrop for the "ui" viewer scene (stands in for the 3D hall).
#include "shaders/include/common.glsl"
#include "shaders/include/noise.glsl"
in vec2 vUV;
uniform vec2 uResolution;
out vec4 outColor;

void main() {
    vec2 p = vec2(vUV.x * uResolution.x / uResolution.y, vUV.y);
    // Nero Marquina-like: near-black stone, warm grey veins, faint gold dust.
    vec3 q = vec3(p * 2.2, 0.3);
    vec3 w = warp(q, 0.55, 3);
    float v = abs(fbm(w * 1.3 + vec3(0.0, 0.0, 1.7), 4));
    float veins = pow(1.0 - smoothstep(0.0, 0.035, v), 3.0);
    float fine = pow(1.0 - smoothstep(0.0, 0.012, abs(fbm(w * 4.1 + 3.1, 3))), 2.0);
    float cloud = fbm(vec3(p * 3.0, 4.2), 3) * 0.5 + 0.5;
    vec3 stone = mix(vec3(0.028, 0.024, 0.022), vec3(0.060, 0.050, 0.043), cloud);
    stone += vec3(0.30, 0.27, 0.22) * veins * 0.10 + vec3(0.20, 0.17, 0.12) * fine * 0.07;
    // Warm light from the upper left (as from the hall's windows) and a vignette.
    float light = exp(-2.2 * length(vUV - vec2(0.25, 0.85)));
    stone *= 0.35 + 1.1 * light;
    float vig = smoothstep(1.15, 0.25, length((vUV - 0.5) * vec2(1.3, 1.0)));
    stone *= mix(0.35, 1.0, vig);
    vec3 c = pow(max(stone, 0.0), vec3(1.0 / 2.2));
    c += (fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5) / 255.0;
    outColor = vec4(c, 1.0);
}
