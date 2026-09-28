// Shared declarations for every Scacelith shader. Mirrors render/renderer.h (keep in sync).
// Conventions: world space, meters, +Y up. Reverse-Z depth ([0,1], 1 = near, 0 = infinity).
// All lighting values are pre-multiplied by frame.exposure.x (pre-exposure).

#define PI 3.14159265358979
#define TAU 6.28318530717959
#define INV_PI 0.318309886183791

layout(std140, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invView;
    mat4 invProj;
    mat4 invViewProj;
    mat4 prevViewProj;       // previous frame, unjittered
    mat4 viewProjNoJitter;   // current frame, unjittered
    vec4 cameraPos;          // xyz, w = time (s)
    vec4 resolution;         // w, h, 1/w, 1/h
    vec4 jitter;             // xy current NDC jitter, zw previous
    vec4 sunDirection;       // xyz towards sun, w = angular radius
    vec4 sunRadiance;        // rgb pre-exposed illuminance, w = raw lux
    vec4 skyParams;          // x turbidity, y pre-exposed sky illuminance, z EV100, w frame index
    vec4 exposure;           // x pre-exposure, y 1/x, z near plane, w aspect
    vec4 ambientSky;         // fallback hemisphere (pre-exposed)
    vec4 ambientGround;
    mat4 shadowMatrix[4];    // world -> [0,1] shadow uv + depth
    vec4 shadowCascade[4];   // xyz centre, w radius
    vec4 shadowParams;       // x count, y normal bias, z depth bias, w light size
    vec4 clipPlane;          // keep dot(n,p)+d >= 0
    vec4 passInfo;           // x pass (0 main,1 prepass,2 shadow,3 planar,4 probe), y layer, z light count, w planar count
    vec4 planarPlanes[4];
    mat4 planarViewProj[4];
} frame;

struct DrawData {
    mat4 model;
    mat4 prevModel;
    mat4 normalMatrix;
    vec4 matParams[8];
    vec4 instParams[4];
    vec4 info;               // x objectSeed, y planar reflector (-1 none), z flags, w objectId
};
layout(std430, binding = 1) readonly buffer DrawSSBO { DrawData draws[]; };

// Point / spot lights (render::PointLight, 64 bytes). Omni lights have spotCosOuter <= -1.
struct PointLightData {
    vec3 position; float radius;
    vec3 color; float intensity;
    vec3 direction; float spotCosOuter;   // spot axis (light -> scene), cos of the outer cone angle
    float spotCosInner; float sourceRadius; float pad0; float pad1;
};
layout(std430, binding = 4) readonly buffer LightSSBO { PointLightData pointLights[]; };

layout(location = 0) uniform int uDraw;

#define PASS_ID_MAIN 0
#define PASS_ID_PREPASS 1
#define PASS_ID_SHADOW 2
#define PASS_ID_PLANAR 3
#define PASS_ID_PROBE 4

float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }
float sq(float x) { return x * x; }
vec2 sq(vec2 x) { return x * x; }
vec3 sq(vec3 x) { return x * x; }
float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
float time() { return frame.cameraPos.w; }

// Reverse-Z infinite projection: view-space distance along -Z from a depth buffer value.
float linearDepth(float d) { return frame.exposure.z / max(d, 1e-7); }
vec3 worldFromDepth(vec2 uv, float d) {
    vec4 p = frame.invViewProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return p.xyz / p.w;
}
vec3 viewFromDepth(vec2 uv, float d) {
    vec4 p = frame.invProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return p.xyz / p.w;
}

// Hashes / noise helpers usable by any stage.
uint hashU(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
float hash11(float p) { return float(hashU(floatBitsToUint(p))) * (1.0 / 4294967296.0); }
float hash12(vec2 p) { return float(hashU(floatBitsToUint(p.x) ^ hashU(floatBitsToUint(p.y)))) * (1.0 / 4294967296.0); }
float hash13(vec3 p) { return float(hashU(floatBitsToUint(p.x) ^ hashU(floatBitsToUint(p.y) ^ hashU(floatBitsToUint(p.z))))) * (1.0 / 4294967296.0); }
// Interleaved gradient noise (per-pixel dither), animated with the frame index.
float ign(vec2 pixel) {
    pixel += 5.588238 * mod(frame.skyParams.w, 64.0);
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 p = n.xy;
    if (n.z < 0.0) p = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return p;
}
vec3 octDecode(vec2 p) {
    vec3 n = vec3(p, 1.0 - abs(p.x) - abs(p.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
