// Candle wax (render-materials): CandleWax.
// Ivory beeswax / stearin: strongly translucent (warm subsurface), satin surface with vertical
// drip runs (relief + slightly glossier, more translucent wax), faint dust. When lit, the flame
// glows through the top of the candle (emission falling off below the top).
//
// Params:
//   [0] wax colour.rgb, roughness
//   [1] subsurface colour.rgb, subsurface amount
//   [2] glow colour.rgb (nits at the top, 0 = unlit), glow falloff (m)
//   [3] top of the candle (object space y, m), drip amount, dust amount
#include "shaders/materials/include/matlib.glsl"

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P0 = i.matParams[0], P1 = i.matParams[1], P2 = i.matParams[2], P3 = i.matParams[3];
    vec3 p = i.positionOS;
    // Seam-free angular coordinate: noise on the unit circle.
    vec2 dirxz = normalize(p.xz + vec2(1e-6));
    vec3 dq = vec3(dirxz * 1.6, i.objectSeed * 50.0);
    float runLen = 0.03 + 0.08 * (mat_gnoise(dq + vec3(0.0, 0.0, 7.0)) * 0.5 + 0.5);
    float fromTop = P3.x - p.y;
    float drip = smoothstep(0.1, 0.5, mat_gnoise(dq)) * (1.0 - smoothstep(runLen * 0.7, runLen, fromTop)) * P3.y;
    float bumps = mat_fbm(vec3(dirxz * 3.5, p.y * 200.0), 3);
    float dust = saturate(i.normalWS.y) * P3.z;
    vec3 col = P0.rgb * (1.0 + bumps * 0.02) * (1.0 + drip * 0.04);
    col = mix(col, vec3(0.5, 0.48, 0.45), dust * 0.4);
    s.albedo = col;
    s.roughness = mix(P0.a, P0.a * 0.6, drip) + bumps * 0.04;
    s.specular = 0.45;
    s.normalWS = bumpFromHeight(i.positionWS, i.normalWS, drip * 0.0006 + bumps * 0.00005, 1.0);
    s.subsurface = saturate(P1.a + drip * 0.1);
    s.subsurfaceColor = P1.rgb;
    s.subsurfaceRadius = 0.01;
    // Flame glow through the wax.
    float glow = exp(-max(fromTop, 0.0) / max(P2.a, 1e-3));
    s.emission = P2.rgb * glow;
    mat_debugAlbedo(s);
}
