// Final display transform, drawn to the backbuffer (any size: Catmull-Rom upscale when the
// render scale differs). Order: contrast-adaptive sharpening (post-TAA) + lateral chromatic
// aberration -> energy-conserving bloom -> exposure (manual pre-exposure * 2^(EC + auto)) ->
// natural (cos^4) vignette -> AgX (Rec.2020 working space, smooth highlight desaturation so
// sunlit marble keeps its gradations) with a gentle look: pivoted contrast, warm highlights /
// cool shadows, saturation -> sRGB encode -> luminance-dependent film grain -> dither -> fade.
#include "shaders/post/post_common.glsl"
in vec2 vUV;
layout(binding = 0) uniform sampler2D uColor;
layout(binding = 1) uniform sampler2D uBloom;
layout(binding = 2) uniform sampler2D uExposure;
layout(binding = 3) uniform sampler2D uDebug;
layout(location = 1) uniform int uFlags;  // 1 bloom, 2 auto exposure, 4 rescale
out vec4 outColor;

const mat3 SRGB_TO_REC2020 = mat3(vec3(0.6274, 0.0691, 0.0164), vec3(0.3293, 0.9195, 0.0880), vec3(0.0433, 0.0113, 0.8956));
const mat3 REC2020_TO_SRGB = mat3(vec3(1.6605, -0.1246, -0.0182), vec3(-0.5876, 1.1329, -0.1006), vec3(-0.0728, -0.0083, 1.1187));
const mat3 AGX_INSET = mat3(vec3(0.856627153315983, 0.137318972929847, 0.11189821299995),
                            vec3(0.0951212405381588, 0.761241990602591, 0.0767994186031903),
                            vec3(0.0482516061458583, 0.101439036467562, 0.811302368396859));
const mat3 AGX_OUTSET = mat3(vec3(1.1271005818144368, -0.1413297634984383, -0.14132976349843826),
                             vec3(-0.11060664309660323, 1.157823702216272, -0.11060664309660294),
                             vec3(-0.016493938717834573, -0.016493938717834257, 1.2519364065950405));
const float AGX_MIN_EV = -12.47393, AGX_MAX_EV = 4.026069;

vec3 agxContrast(vec3 x) {
    vec3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// displayTransform() and srgbEncode() have a CPU port in src/render/post/display_transform.h (the
// brightness calibration draws its patches with it): change both together.
vec3 displayTransform(vec3 c) {
    c = SRGB_TO_REC2020 * c;
    c = AGX_INSET * c;
    c = clamp((log2(max(c, vec3(1e-10))) - AGX_MIN_EV) / (AGX_MAX_EV - AGX_MIN_EV), 0.0, 1.0);
    c = agxContrast(c);
    // Look (in the AgX encoded, perceptual domain). Contrast: S-curve through (0,0), (pivot,
    // pivot), (1,1) with slope grade.x at the pivot, so the highlights still roll off to white
    // instead of clipping.
    float l = dot(c, vec3(0.2626, 0.6780, 0.0593));
    const float pivot = 0.42;
    c = clamp(c, 0.0, 1.0);
    vec3 lo = pivot * pow(c / pivot, vec3(post.grade.x));
    vec3 hi = 1.0 - (1.0 - pivot) * pow((1.0 - c) / (1.0 - pivot), vec3(post.grade.x));
    c = mix(lo, hi, step(pivot, c));
    vec3 cool = vec3(0.985, 1.0, 1.03), warm = vec3(1.03, 1.0, 0.955);
    vec3 tone = mix(cool, warm, smoothstep(0.18, 0.72, l));
    c *= mix(vec3(1.0), tone, post.grade.z);
    l = dot(c, vec3(0.2626, 0.6780, 0.0593));
    c = l + (c - l) * post.grade.y;
    c = AGX_OUTSET * c;
    c = pow(max(c, vec3(0.0)), vec3(2.2));
    c = REC2020_TO_SRGB * c;
    return clamp(c, 0.0, 1.0);
}

vec3 srgbEncode(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }

vec3 fetchColor(vec2 uv) {
    if ((uFlags & 4) != 0) return max(sampleCatmullRom(uColor, uv, post.renderSize.xy, post.renderSize.zw).rgb, vec3(0.0));
    return textureLod(uColor, uv, 0.0).rgb;
}

vec3 debugView(int mode, vec2 uv) {
    vec4 v = textureLod(uDebug, uv, 0.0);
    if (mode == 1) return vec3(v.r);                                                // AO
    if (mode == 2) return v.rgb * v.a / (1.0 + v.rgb * v.a);          // SSR (radiance * confidence)
    if (mode == 3) return vec3(v.rgb * 6.0 / (1.0 + v.rgb * 6.0));    // volumetric in-scattering
    if (mode == 4) return v.a < 0.0 ? vec3(saturate(-v.a / 6.0), 0.0, 0.0) : vec3(0.0, 0.0, saturate(v.a / 6.0));  // CoC
    if (mode == 5) return vec3(0.5 + v.xy * 20.0, 0.5);              // motion vectors
    if (mode == 7) {                                                  // HiZ level 3: r closest, g farthest (linear depth / 10 m)
        vec2 h = texelFetch(uDebug, ivec2(uv * vec2(textureSize(uDebug, 3))), 3).rg;
        return vec3(linearFromRaw(h.r) * 0.1, linearFromRaw(h.g) * 0.1, 0.0);
    }
    return v.rgb / (1.0 + v.rgb);                                      // bloom
}

void main() {
    vec2 uv = vUV;
    int dbg = int(post.misc.x);
    if (dbg > 0) { outColor = vec4(srgbEncode(saturate(debugView(dbg, uv))), 1.0); return; }

    vec3 c = fetchColor(uv);
    // Contrast-adaptive sharpening (restores the detail TAA's reconstruction filter softens).
    vec2 t = post.renderSize.zw;
    vec3 n0 = textureLod(uColor, uv + vec2(t.x, 0.0), 0.0).rgb, n1 = textureLod(uColor, uv - vec2(t.x, 0.0), 0.0).rgb;
    vec3 n2 = textureLod(uColor, uv + vec2(0.0, t.y), 0.0).rgb, n3 = textureLod(uColor, uv - vec2(0.0, t.y), 0.0).rgb;
    {
        vec3 tc = tmForward(c), t0 = tmForward(n0), t1 = tmForward(n1), t2 = tmForward(n2), t3 = tmForward(n3);
        vec3 mn = min(tc, min(min(t0, t1), min(t2, t3))), mx = max(tc, max(max(t0, t1), max(t2, t3)));
        vec3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
        vec3 w = amp * (-0.2 * post.taa.y);
        vec3 s = (tc + (t0 + t1 + t2 + t3) * w) / (1.0 + 4.0 * w);
        c = tmInverse(clamp(s, mn, mx));
    }
    // Lateral chromatic aberration: radial, grows with r^2, px at the frame corner.
    vec2 d = uv - 0.5;
    float r2 = dot(d * vec2(post.outputSize.x / post.outputSize.y, 1.0), d * vec2(post.outputSize.x / post.outputSize.y, 1.0));
    float cornerR2 = 0.25 * (post.outputSize.x * post.outputSize.x / (post.outputSize.y * post.outputSize.y) + 1.0);
    vec2 ca = d / max(length(d), 1e-4) * (post.display.z * r2 / cornerR2) * post.renderSize.zw;
    if (post.display.z > 0.0) {
        vec3 base = textureLod(uColor, uv, 0.0).rgb;
        c.r += textureLod(uColor, uv - ca, 0.0).r - base.r;
        c.b += textureLod(uColor, uv + ca, 0.0).b - base.b;
        c = max(c, vec3(0.0));
    }
    if ((uFlags & 1) != 0) c = mix(c, textureLod(uBloom, uv, 0.0).rgb, post.bloom.x);
    float ev = post.expo.x;
    if ((uFlags & 2) != 0) {
        float a = texelFetch(uExposure, ivec2(0), 0).x;
        if (!isnan(a) && !isinf(a)) ev += a;
    }
    c *= exp2(ev);
    // Natural vignetting: cos^4 of the field angle, blended by the setting.
    float tanHalf = 1.0 / frame.proj[1][1];
    vec2 sp = d * 2.0 * vec2(tanHalf * post.outputSize.x / post.outputSize.y, tanHalf);
    float cos2 = 1.0 / (1.0 + dot(sp, sp));
    c *= mix(1.0, cos2 * cos2, post.display.y);
    c = sanitize(c);
    vec3 disp = displayTransform(c);
    vec3 enc = srgbEncode(disp);
    // Film grain: fine, mostly luminance, strongest in the mid-tones, animated.
    vec2 px = gl_FragCoord.xy;
    uint seed = uint(post.timing.y) * 1664525u;
    float h0 = float(hashU(uint(px.x) * 73856093u ^ uint(px.y) * 19349663u ^ seed)) * (1.0 / 4294967296.0);
    float h1 = float(hashU(uint(px.x) * 83492791u ^ uint(px.y) * 2654435761u ^ (seed + 7u))) * (1.0 / 4294967296.0);
    float h2 = float(hashU(uint(px.x) * 2246822519u ^ uint(px.y) * 3266489917u ^ (seed + 13u))) * (1.0 / 4294967296.0);
    float g = (h0 + h1 - 1.0) * 1.2247;  // triangular, unit variance-ish
    float le = dot(enc, vec3(0.2126, 0.7152, 0.0722));
    float gAmt = post.display.x * (0.35 + 2.6 * le * (1.0 - le));
    enc += vec3(g) * gAmt + (h2 - 0.5) * gAmt * 0.25 * vec3(1.0, -0.6, 0.4);
    // Dither against banding (8-bit output), then fade to black.
    enc += (h2 + h0 - 1.0) / 255.0;
    enc *= 1.0 - saturate(post.display.w);
    outColor = vec4(saturate(enc), 1.0);
}
