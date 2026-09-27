// Baseline display transform: ACES fitted tonemap, vignette, grain, fade, sRGB encode.
#include "shaders/include/common.glsl"
in vec2 vUV;
layout(binding = 0) uniform sampler2D uHDR;
uniform float uFade;
uniform float uVignette;
uniform float uGrain;
out vec4 outColor;

vec3 acesFitted(vec3 v) {
    const mat3 i = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 o = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    v = i * v;
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return clamp(o * (a / b), 0.0, 1.0);
}
vec3 srgbEncode(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }

void main() {
    vec3 c = texture(uHDR, vUV).rgb;
    c = acesFitted(c);
    vec2 q = vUV - 0.5;
    c *= mix(1.0, smoothstep(0.85, 0.2, length(q)), uVignette);
    c = srgbEncode(c);
    c += (ign(gl_FragCoord.xy) - 0.5) * uGrain;
    c *= 1.0 - uFade;
    outColor = vec4(c, 1.0);
}
