// Pale limestone walls (render-materials): WallStone.
// Honed oolitic limestone (Caen / Saint-Maximin type): warm cream with soft tonal clouds, faint
// sedimentary bedding, scattered shell fragments and ooids, tiny pits, ashlar blocks with lime
// mortar joints (tooled, slightly recessed), dust on upward faces. World space (the hall is
// static): blocks are laid on the dominant-axis plane of the world normal.
//
// Params:
//   [0] stone colour.rgb, roughness
//   [1] variation colour.rgb (darker/greyer beds), variation amount
//   [2] block width (m), block height (m), joint width (m), joint relief (0..1)
//   [3] mortar colour.rgb, fossil amount
//   [4] bedding amount, pit amount, dust amount, block tint variation
//   [5] grid origin (world xyz), (unused)
// Blocks are disabled when block width = 0.
#include "shaders/materials/include/matlib.glsl"

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P0 = i.matParams[0], P1 = i.matParams[1], P2 = i.matParams[2], P3 = i.matParams[3], P4 = i.matParams[4];
    vec3 p = i.positionWS - i.matParams[5].xyz;
    vec3 N = i.normalWS;
    float fp = mat_footprint(p);
    vec3 an = abs(N);
    // Wall plane coordinates: horizontal along the wall, vertical = y.
    vec2 wp = an.x > an.z ? vec2(p.z, p.y) : vec2(p.x, p.y);
    if (an.y > max(an.x, an.z)) wp = p.xz;

    // ---- Ashlar blocks ------------------------------------------------------------------------
    float joint = 0.0;
    vec2 blockId = vec2(0.0);
    vec2 jslope = vec2(0.0);
    if (P2.x > 0.0) {
        vec2 bc = vec2(wp.x / P2.x, wp.y / P2.y);
        float course = floor(bc.y);
        bc.x += 0.5 * mod(course, 2.0);  // running bond
        blockId = floor(bc);
        vec2 dl = (fract(bc + 0.5) - 0.5) * P2.xy;  // distance to the joints (m)
        vec2 fw = fwidth(wp);
        joint = max(mat_band(dl.x, 0.5 * P2.z, fw.x), mat_band(dl.y, 0.5 * P2.z, fw.y));
        // Arrised block edges: the face rolls off into the joint.
        vec2 e = 1.0 - smoothstep(0.5 * P2.z, 0.5 * P2.z + 0.004, abs(dl));
        jslope = sign(dl) * e * 0.35 * P2.w;
    }
    vec4 bh = mat_hash4(blockId.x * 17.0 + blockId.y * 71.0 + 3.0);

    // ---- Stone body ---------------------------------------------------------------------------
    vec3 q = p;
    float clouds = mat_fbm(q * 1.3 + bh.x * 5.0, 4);
    float bedding = mat_fbm(vec3(wp.x * 1.5, wp.y * 9.0, bh.y * 3.0), 3);
    float fine = mat_fbm(q * 40.0, 3);
    // Each block comes from a different part of the quarry bed: brightness and hue (warmer ochre
    // vs cooler grey) vary per block.
    vec3 col = P0.rgb * (1.0 + (bh.z - 0.5) * P4.w);
    col *= mix(vec3(1.0), mix(vec3(1.03, 1.0, 0.93), vec3(0.97, 0.99, 1.02), bh.w), P4.w * 6.0);
    col = mix(col, P1.rgb, saturate(smoothstep(-0.3, 0.5, clouds) * P1.a + smoothstep(0.0, 0.45, bedding) * P4.x));
    // Soft darker blooms (iron-stained patches) and a fine mottle.
    float bloom = smoothstep(0.25, 0.7, mat_fbm(q * 3.1 + bh.y * 9.0, 3));
    col = mix(col, col * vec3(0.9, 0.86, 0.78), bloom * 0.5 * P1.a);
    col *= 1.0 + fine * 0.07;
#if !defined(MAT_LOW_DETAIL)
    // Shell fragments (curved slivers) and ooids (tiny round grains).
    float shellVis = 1.0 - mat_subpixel(0.0015, fp);
    vec3 v = voronoi(q * 70.0);
    float shell = smoothstep(0.06, 0.02, abs(v.x - 0.35 - 0.1 * v.z)) * step(0.82, v.z);
    float ooid = step(0.8, mat_grainId(q * 400.0)) * (1.0 - mat_subpixel(0.0005, fp));
    col = mix(col, col * vec3(1.08, 1.05, 0.98), shell * P3.a * shellVis);
    col = mix(col, col * 0.93, ooid * 0.3 * P3.a);
    // Pits (small vugs).
    float pit = smoothstep(0.78, 0.9, mat_gnoise(q * 260.0)) * (1.0 - mat_subpixel(0.0008, fp)) * P4.y;
    col *= 1.0 - pit * 0.35;
#else
    float pit = 0.0;
#endif
    // Dust settles on upward faces and on ledges.
    float dust = saturate(N.y) * P4.z;
    col = mix(col, vec3(0.55, 0.53, 0.5), dust * 0.5);

    s.albedo = mix(col, P3.rgb * (1.0 + fine * 0.08), joint);
    s.roughness = mix(P0.a + fine * 0.05 + pit * 0.2, 0.92, joint);
    s.specular = 0.45;
    vec3 T = an.x > an.z ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 B = vec3(0, 1, 0);
    vec3 Nb = normalize(N + T * jslope.x + B * jslope.y);
#if !defined(MAT_LOW_DETAIL)
    Nb = bumpFromHeight(i.positionWS, Nb, fine * 0.0002 - pit * 0.0004 - joint * 0.002, 1.0);
#endif
    s.normalWS = Nb;
    s.occlusion = 1.0 - joint * 0.45 - pit * 0.3;
    s.subsurface = 0.05;
    mat_debugAlbedo(s);
}
