// Metals (render-materials). Variants by define:
//   METAL_GILDED  GildedTrim: water-gilded gold leaf on carved mouldings. Individual leaves
//                 (square, slightly different tone/burnish, brighter double-layer overlaps),
//                 fine wrinkles and cracks, red bole showing where the leaf is worn (raised
//                 edges and noise), dark grime in crevices (screen-space curvature).
//   METAL_BRASS   Brass: polished yellow brass with patchy tarnish (more in hollows), polishing
//                 scratches and fingerprint smudges (polish texture).
// Patterns in object space (positionOS, meters): triplanar with the dominant axis.
//
// Params:
//   [0] metal F0.rgb (linear), base roughness
//   [1] under colour.rgb (gilded: bole; brass: tarnish), amount (wear / tarnish)
//   [2] grime colour.rgb, grime amount (crevices)
//   [3] gilded: leaf size (m), seam strength, burnish variation, crack amount
//       brass : scratch amount, smudge amount, (unused), (unused)
// Textures: 7 = polish.
#define MAT_USE_POLISH
#include "shaders/materials/include/matlib.glsl"

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P0 = i.matParams[0], P1 = i.matParams[1], P2 = i.matParams[2], P3 = i.matParams[3];
    vec3 p = i.positionOS;
    vec3 nOS = i.normalOS;
    vec3 N = i.normalWS;
    float fp = mat_footprint(p);
    float curv = mat_curvature(i.normalWS, i.positionWS);
    float convex = smoothstep(30.0, 180.0, curv);
    float concave = smoothstep(-30.0, -180.0, curv);
    // Dominant-axis planar coordinates (object space).
    vec3 an = abs(nOS);
    vec2 pl = an.x > an.y && an.x > an.z ? p.zy : (an.y > an.z ? p.xz : p.xy);
    float axisId = an.x > an.y && an.x > an.z ? 0.0 : (an.y > an.z ? 1.0 : 2.0);

    vec3 albedo = P0.rgb;
    float rough = P0.a;
    float metal = 1.0;
#if defined(METAL_GILDED)
    // Gold leaves: a square grid with a jittered row offset (leaves are laid in rows).
    float leaf = P3.x;
    vec2 lc = pl / leaf;
    float rowId = floor(lc.y);
    lc.x += mat_hash1(rowId + axisId * 31.0) * 0.8;
    vec2 cellId = floor(lc);
    vec2 lf = fract(lc);
    vec4 lh = mat_hash4(cellId.x * 7.0 + cellId.y * 131.0 + axisId * 17.0 + i.objectSeed);
    // Overlaps: a narrow strip along two sides of each leaf is doubled (brighter, a ridge).
    float fw = fwidth(lc.x) + fwidth(lc.y);
    float overlap = max(mat_band(lf.x - 0.015, 0.012, fw), mat_band(lf.y - 0.015, 0.012, fw)) * P3.y;
    albedo *= 1.0 + (lh.x - 0.5) * 0.06 + overlap * 0.05;
    rough += (lh.y - 0.5) * P3.z + overlap * 0.03;
    // Wrinkles and cracks in the leaf.
    // Wrinkles are ~1 mm features: they fade out (into roughness) well before they alias into
    // glitter.
    float wr = mat_fbm(vec3(pl * 700.0, lh.z * 10.0), 2);
    float vis = 1.0 - mat_subpixel(0.0025, fp);
    rough += (1.0 - vis) * 0.05;
    // Cracks: sparse hairlines, only in patches (craquelure is not uniform).
    float cn = mat_gnoise(vec3(pl * 220.0, lh.w * 5.0));
    float cpatch = smoothstep(0.2, 0.5, mat_gnoise(p * 30.0 + 3.0));
    float crack = mat_band(cn, 0.012, fwidth(cn)) * P3.w * vis * cpatch;
    // Wear: bole shows where the leaf is rubbed through: on raised edges (curvature) and in a few
    // broad rubbed zones, with ragged borders (fine noise only modulates the border).
    float broad = mat_fbm(p * 22.0 + i.objectSeed * 13.0, 3) * 0.5 + 0.5;
    float ragged = mat_fbm(p * 400.0, 2) * 0.12;
    float wear = smoothstep(0.7, 0.76, broad * 0.7 + convex * 0.6 + ragged) * P1.a;
    albedo = mix(albedo, P1.rgb, wear);
    // Cracks read as fine dark hairlines (the gap shadows the bole), not as bole colour.
    albedo *= 1.0 - crack * 0.6;
    rough += crack * 0.25;
    metal = 1.0 - wear;
    rough = mix(rough, 0.6, wear);
    // Grime in the hollows of the carving.
    float grime = concave * P2.a * (0.6 + 0.4 * mat_gnoise(p * 300.0));
    albedo = mix(albedo, P2.rgb, grime * 0.8);
    rough = mix(rough, 0.7, grime);
    metal *= 1.0 - grime * 0.7;
    // Leaf wrinkles as normal detail.
    s.normalWS = bumpFromHeight(i.positionWS, N, (wr * 0.000006 + overlap * 0.00001) * vis, 1.0);
#else  // METAL_BRASS
    // Tarnish patches, heavier in hollows; polishing scratches and smudges.
    float tn = mat_fbm(p * 25.0 + i.objectSeed * 7.0, 5) * 0.5 + 0.5;
    float tarnish = saturate(smoothstep(0.45, 0.75, tn) * P1.a + concave * 0.5 * P1.a);
    albedo = mix(albedo, P1.rgb, tarnish);
    rough = mix(rough, 0.45, tarnish);
    vec3 slopeOS;
    vec4 pol = mat_polishTriplanar(p * 2.0, nOS, slopeOS);
    rough += pol.z * 0.08 * P3.x + pol.w * 0.15 * P3.y;
    float grime = concave * P2.a;
    albedo = mix(albedo, P2.rgb, grime * 0.6);
    rough = mix(rough, 0.6, grime);
    metal = 1.0 - grime * 0.5;
    s.normalWS = mat_normalToWorld(normalize(nOS - slopeOS * 0.04 * P3.x));
#endif
    s.albedo = albedo;
    s.metallic = metal;
    s.roughness = clamp(rough, 0.05, 1.0);
    s.specular = 0.5;
    s.occlusion = 1.0 - concave * 0.5;
    mat_debugAlbedo(s);
}
