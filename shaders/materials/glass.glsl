// Transparent materials (render-materials). Variants by define:
//   (default)      WindowGlass: old cylinder/crown glass. Faint waviness drawn along one
//                  direction (cords), rare seed bubbles, very slight green tint (greener where
//                  thicker), dust settling towards the bottom of each pane, smudges.
//   GLASS_CRYSTAL  Crystal: lead crystal chandelier drops (ior 1.65), perfectly polished, a hint
//                  of dispersion "fire" in the tint.
// Transparency contract: albedo = transmittance tint, transmission = fraction transmitted,
// alpha = coverage of the scattering part (dust); the specular reflection is not scaled by
// alpha physically (the transparent pass of the lighting package composes them).
//
// Params:
//   [0] tint.rgb (transmittance), roughness
//   [1] ior, waviness amount (rad), wave scale (1/m), dust amount
//   [2] panes across u, panes across v (uv grid, dust gathers at each pane bottom), seed amount, smudge amount
// inst: WindowGlass inst[0].xy = pane grid override (0 = params[2].xy).
#define MAT_USE_POLISH
#include "shaders/materials/include/matlib.glsl"

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P0 = i.matParams[0], P1 = i.matParams[1], P2 = i.matParams[2];
    vec3 N = i.normalWS;
    vec3 T = i.tangentWS, B = i.bitangentWS;
    float ior = P1.x;
    vec3 tint = P0.rgb;
    float dust = 0.0;
    float rough = P0.a;
#if defined(GLASS_CRYSTAL)
    // Fire: the refracted colours of a dispersive glass, seen as a faint view-dependent tint.
    vec3 R = reflect(-i.viewDirWS, N);
    float hue = fract(dot(R, vec3(2.7, 3.1, 1.9)) + i.objectSeed);
    vec3 rainbow = 0.5 + 0.5 * cos(TAU * (hue + vec3(0.0, 0.33, 0.67)));
    tint = mix(tint, rainbow, 0.18);
    float alpha = 0.06;
#else
    // Cords: waves elongated along the drawing direction (uv.x).
    vec3 wq = vec3(i.positionOS * P1.z);
    float w1 = mat_fbm(wq * vec3(0.25, 1.0, 1.0), 3);
    float w2 = mat_fbm(wq * vec3(0.25, 1.0, 1.0) + vec3(11.0, 3.0, 7.0), 3);
    N = normalize(N + (T * w1 * 0.3 + B * w2) * P1.y);
    // Thickness varies with the waves: slightly greener in the thick parts.
    tint *= mix(vec3(1.0), vec3(0.97, 0.995, 0.975), saturate(w1 * 0.5 + 0.5));
    // Panes: dust gathers towards the bottom rail of each pane and in the corners.
    vec2 pg = max(i.instParams[0].x > 0.0 ? i.instParams[0].xy : P2.xy, vec2(1.0));
    vec2 pf = fract(i.uv * pg);
    float bottom = exp(-pf.y * 9.0) + 0.4 * exp(-min(pf.x, 1.0 - pf.x) * 14.0);
    // A fine, patchy film (not a fog): large-scale presence times a fine speckle.
    float dn = mat_fbm(vec3(i.positionOS * 9.0), 3) * 0.5 + 0.5;
    float sp = mat_fbm(vec3(i.positionOS * 140.0), 2) * 0.5 + 0.5;
    dust = saturate((0.06 + bottom * 0.7) * P1.w * smoothstep(0.35, 0.75, dn) * (0.5 + sp));
    // Smudges from the polish texture, rare seed bubbles.
    vec4 pol = mat_polish2D(i.positionOS.xy + i.positionOS.z);
    rough += pol.w * 0.08 * P2.w + dust * 0.4;
    float seedB = step(0.9997, hash13(floor(i.positionOS * 400.0))) * P2.z;
    float alpha = 0.04 + dust * 0.35 + seedB * 0.4;
#endif
    s.albedo = mix(tint, vec3(0.55, 0.52, 0.47), dust);
    s.alpha = alpha;
    s.normalWS = N;
    s.clearcoatNormalWS = N;
    s.roughness = rough;
    s.metallic = 0.0;
    s.specular = mat_ior2specular(ior);
    s.ior = ior;
    s.transmission = saturate(1.0 - alpha);
    s.thickness = 0.004;
}
