// Shared declarations of the render-post passes. Mirrors PostUBOData in src/render/post/postfx.cpp.
// Texture unit conventions inside post passes: 7 = blue noise (R16, 64x64, repeat).
#include "shaders/include/common.glsl"

layout(std140, binding = 3) uniform PostUBO {
    vec4 renderSize;   // w, h, 1/w, 1/h
    vec4 halfSize;     // hw, hh, 1/hw, 1/hh (checkerboard half resolution)
    vec4 outputSize;   // backbuffer w, h, 1/w, 1/h
    vec4 timing;       // x dt, y post frame counter (wrapped at 2^23), z time (s), w 1080p scale (h / 1080)
    vec4 ao;           // x radius (m), y power, z max screen radius (px, half res), w history valid
    vec4 aoB;          // x slices, y steps per side, z falloff range fraction, w -
    vec4 ssr;          // x max roughness, y thickness (relative), z intensity, w max iterations
    vec4 ssrB;         // x edge fade (uv), y max HiZ level, z history valid, w brdf bias
    vec4 vol;          // x scattering (1/m), y HG g, z ambient amount, w max distance
    vec4 volB;         // x steps, y noise amount, z history valid, w motes intensity
    vec4 volC;         // xyz dust drift (m/s), w noise frequency (1/m)
    vec4 taa;          // x history valid, y sharpen, z - (taa.comp's variance gamma is speed-adaptive), w -
    vec4 mb;           // x shutter, y max radius (px), z samples, w tile size (px)
    vec4 dof;          // x focus distance (m), y CoC scale (full-res px radius), z max radius (full px), w radius step
    vec4 bloom;        // x intensity, y scatter, z levels (not read by shaders), w -
    vec4 expo;         // x compensation EV, y auto on (not read by shaders), z min EV, w max EV
    vec4 expoB;        // x speed up, y speed down, z target (pre-exposed middle grey), w history valid
    vec4 display;      // x grain, y vignette, z chromatic aberration (px), w fade
    vec4 grade;        // x contrast, y saturation, z split tone, w -
    vec4 misc;         // x debug view, y ssr composite in resolve (not read by shaders), z volumetric sky march distance, w blue-noise phase
} post;

layout(binding = 7) uniform sampler2D uBlueNoise;

// Blue noise in [0,1), animated with the golden ratio so successive frames are well distributed.
float blueNoise(ivec2 p, int channel) {
    ivec2 o = ivec2(channel * 19 + 7, channel * 41 + 3);
    float v = texelFetch(uBlueNoise, (p + o) & 63, 0).r;
    return fract(v + post.misc.w * float(1 + channel));  // misc.w = frac(frame * 0.61803398875)
}
float blueNoiseStatic(ivec2 p, int channel) {
    ivec2 o = ivec2(channel * 19 + 7, channel * 41 + 3);
    return texelFetch(uBlueNoise, (p + o) & 63, 0).r;
}

// Reverse-Z (1 = near, 0 = far) raw depth <-> positive view distance along -Z.
float linearFromRaw(float d) { return frame.exposure.z / max(d, 1e-7); }
float rawFromLinear(float z) { return frame.exposure.z / max(z, 1e-7); }
const float SKY_DEPTH = 6.0e4;  // fits in fp16 (history alpha)
// Log2-luminance range of the auto-exposure histogram (exposure_histogram / exposure_average).
const float EXPO_MIN_LOG = -14.0, EXPO_RANGE_LOG = 20.0;

// View-space position from uv and positive linear depth (current, jittered projection).
vec3 viewPosFromLinear(vec2 uv, float z) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3((ndc.x + frame.proj[2][0]) * z / frame.proj[0][0], (ndc.y + frame.proj[2][1]) * z / frame.proj[1][1], -z);
}
vec3 worldPosFromLinear(vec2 uv, float z) { return (frame.invView * vec4(viewPosFromLinear(uv, z), 1.0)).xyz; }

bool badValue(vec3 c) { return any(isnan(c)) || any(isinf(c)); }
vec3 sanitize(vec3 c) { return badValue(c) ? vec3(0.0) : clamp(c, vec3(0.0), vec3(60000.0)); }
float maxc(vec3 c) { return max(c.r, max(c.g, c.b)); }

vec3 rgbToYCoCg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 yCoCgToRgb(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
// Reversible tonemap (Karis) used to make HDR neighbourhoods well-behaved.
vec3 tmForward(vec3 c) { return c / (1.0 + maxc(c)); }
vec3 tmInverse(vec3 c) { return c / max(1.0 - maxc(c), 1e-4); }

// Clips 'h' towards the centre of the AABB [mn, mx] (Playdead / Salvi).
vec3 clipAABB(vec3 h, vec3 mn, vec3 mx) {
    vec3 c = 0.5 * (mx + mn), e = 0.5 * (mx - mn) + 1e-5;
    vec3 v = h - c;
    vec3 a = abs(v / e);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? c + v / m : h;
}

// 5-tap Catmull-Rom (bilinear optimised, corners dropped).
vec4 sampleCatmullRom(sampler2D tex, vec2 uv, vec2 size, vec2 invSize) {
    vec2 sp = uv * size;
    vec2 t1 = floor(sp - 0.5) + 0.5;
    vec2 f = sp - t1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t12 = (t1 + w2 / w12) * invSize;
    vec2 t0 = (t1 - 1.0) * invSize, t3 = (t1 + 2.0) * invSize;
    vec4 r = textureLod(tex, vec2(t12.x, t0.y), 0.0) * (w12.x * w0.y) + textureLod(tex, vec2(t0.x, t12.y), 0.0) * (w0.x * w12.y) +
             textureLod(tex, vec2(t12.x, t12.y), 0.0) * (w12.x * w12.y) + textureLod(tex, vec2(t3.x, t12.y), 0.0) * (w3.x * w12.y) +
             textureLod(tex, vec2(t12.x, t3.y), 0.0) * (w12.x * w3.y);
    float w = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return r / w;
}

// Henyey-Greenstein phase function; cosTheta between light propagation and view-towards-eye.
float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}
