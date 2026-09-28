// Finished wood (render-materials): TableWood (waxed high-gloss walnut), TableWoodCarved (satin),
// ChairWood (carved, parcel-gilt ridges), WallPanelWood, ClockCase (satin lacquer).
//
// Solid texturing in object space (meters): the object is carved from a log whose axis is the
// grain direction. Growth rings = distance to a wandering pith (flat-sawn "cathedral" figure on
// faces parallel to the axis, end grain rings on cross cuts), long colour streaks, ribbon
// chatoyance (alternating fibre tilt under the finish), open pores (elongated along the grain,
// normal detail + darkening, averaged when below a pixel), faint medullary ray flecks.
// Finish: clear coat (wax / lacquer) with polish scratches, wax smears and haze; worn convex
// edges and grime in concave carvings from screen-space curvature.
//
// Params:
//   [0] light (earlywood) colour.rgb, ring contrast
//   [1] dark (latewood / streak) colour.rgb, streak (figure) amount
//   [2] default grain axis (object space xyz), rings per meter
//   [3] pith offset from the object origin in the ring plane (m, xy), log taper, ribbon amount
//   [4] base roughness, clear coat, clear coat roughness, pore strength
//   [5] wax smear amount, polish scratch amount, parcel gilding amount, edge wear amount
//   [6] gold leaf colour.rgb (linear F0), gold roughness
// inst (per draw): inst[0].xyz = grain axis override (object space, 0 = params[2]),
//                  inst[0].w = wood seed (0 = objectSeed): each part cut from a different log.
// Textures: 7 = polish.
#define MAT_USE_POLISH
#include "shaders/materials/include/matlib.glsl"

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P0 = i.matParams[0], P1 = i.matParams[1], P2 = i.matParams[2], P3 = i.matParams[3];
    vec4 P4 = i.matParams[4], P5 = i.matParams[5], P6 = i.matParams[6];
    vec3 axis = dot(i.instParams[0].xyz, i.instParams[0].xyz) > 0.25 ? normalize(i.instParams[0].xyz) : normalize(P2.xyz);
    float seed = i.instParams[0].w != 0.0 ? fract(i.instParams[0].w * 0.61803) : i.objectSeed;
    vec4 hs = mat_hash4(seed * 13.0 + 0.7);
    // Log frame: the log axis dips slightly relative to the part (cut obliquely through the
    // rings), which draws the cathedral arches of flat-sawn boards.
    vec3 up = abs(axis.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 U0 = normalize(cross(up, axis)), V0 = cross(axis, U0);
    vec3 lax = normalize(axis + U0 * (hs.y - 0.5) * 0.06 + V0 * (0.02 + 0.03 * hs.z));
    vec3 U = normalize(cross(up, lax)), V = cross(lax, U);
    vec3 p = i.positionOS;
    float sAx = dot(p, lax) + hs.x * 3.0;
    vec2 xy = vec2(dot(p, U), dot(p, V));
    float fpOS = mat_footprint(p);

    // ---- Growth rings around a wandering pith -----------------------------------------------
    vec2 pith = P3.xy + (hs.yz - 0.5) * 0.06;
    pith += 0.02 * vec2(mat_gnoise(vec3(sAx * 0.9, seed * 17.0, 1.0)), mat_gnoise(vec3(sAx * 0.9, seed * 17.0, 5.0)));
    vec2 rp = xy - pith;
    // Wavy/curly grain: ripples of the rings along the axis.
    rp += 0.0012 * vec2(mat_gnoise(vec3(sAx * 25.0, rp * 30.0)), mat_gnoise(vec3(sAx * 25.0 + 9.0, rp * 30.0)));
    float rad = length(rp);
    // Year-to-year ring width variation + slow distortions (no angular term: seam free).
    float phase = rad * P2.w + 2.5 * mat_fbm(vec3(rad * 7.0, seed * 3.0, 0.5), 3) + 0.8 * mat_fbm(vec3(rp * 9.0, sAx * 0.4), 3);
    float t = fract(phase);
    // Earlywood -> latewood: gradual darkening then a sharp return at the ring boundary.
    float late = smoothstep(0.3, 0.9, t) * (1.0 - smoothstep(0.94, 1.0, t));
    float fpRing = fwidth(phase);
    float ringVis = 1.0 - smoothstep(0.25, 0.7, fpRing);
    late = mix(0.4, late, ringVis);

    // ---- Figure: long streaks along the grain, broad colour drift -----------------------------
    float streak = mat_fbm(vec3(sAx * 1.8, rp * 22.0), 4);
    float streak2 = mat_fbm(vec3(sAx * 0.6 + 11.0, rp * 5.0), 3);
    float drift = mat_gnoise(vec3(sAx * 0.8, rp * 3.0));
    float dark = saturate(late * P0.a + smoothstep(-0.05, 0.45, streak) * P1.a + smoothstep(0.1, 0.5, streak2) * 0.35);
    vec3 col = mix(P0.rgb, P1.rgb, dark);
    col *= 1.0 + drift * 0.15;
    col = mix(col, col * vec3(0.92, 0.9, 1.0), smoothstep(0.0, 0.6, streak2) * 0.5);  // greyish-purple heart streaks
    col *= 1.0 + (hs.w - 0.5) * 0.12;

    // ---- Pores and ray flecks ------------------------------------------------------------------
    float poreH = 0.0;
    float pore = 0.0;
#if !defined(MAT_LOW_DETAIL)
    {
        vec3 pp = vec3(sAx * 450.0, xy * 2800.0);
        float n = mat_gnoise(pp) * 0.6 + mat_gnoise(pp * vec3(1.0, 2.1, 2.1) + 7.0) * 0.4;
        float thr = mix(0.62, 0.45, 1.0 - late);  // more pores in the earlywood
        float cov = smoothstep(thr, thr + 0.12, n);
        float vis = 1.0 - mat_subpixel(0.00025, fpOS);
        pore = mix(0.07 * (1.0 - late * 0.5), cov, vis) * P4.w;
        poreH = -cov * vis;
        // Medullary rays: short flecks across the grain (visible on quarter-sawn faces).
        vec3 rpp = vec3(sAx * 700.0, xy * 160.0);
        float ray = smoothstep(0.75, 0.92, mat_gnoise(rpp)) * (1.0 - mat_subpixel(0.0004, fpOS));
        col = mix(col, col * 1.18, ray * 0.5);
    }
#endif
    col *= 1.0 - pore * 0.55;

    // ---- Ribbon chatoyance: fibres alternately dip in and out of the surface -----------------
    vec3 axisWS = normalize(mat_modelRot() * lax);
    float ribbon = sin(rad * 160.0 + 3.0 * mat_gnoise(vec3(sAx * 1.5, rad * 20.0, 3.0)));
    ribbon = clamp(ribbon * 3.0, -1.0, 1.0) * P3.w;
    vec3 N = i.normalWS;
    vec3 baseN = normalize(N + (axisWS - N * dot(axisWS, N)) * ribbon * 0.1);
#if !defined(MAT_LOW_DETAIL)
    baseN = bumpFromHeight(i.positionWS, baseN, poreH * 0.00012, 1.0);
#endif

    // ---- Finish ----------------------------------------------------------------------------
    float rough = P4.x + pore * 0.3;
    float coat = P4.y;
    float coatRough = P4.z;
    vec3 slopeOS;
    vec4 pol = mat_polishTriplanar(p, i.normalOS, slopeOS);
    vec3 coatN = mat_normalToWorld(normalize(i.normalOS - slopeOS * 0.03 * P5.y));
    coatRough += pol.w * 0.12 * P5.x + pol.z * 0.04 * P5.y;

    // Carving: worn convex edges (lighter, smoother), grime/wax in hollows.
    float curv = mat_curvature(i.normalWS, i.positionWS);
    float edge = smoothstep(40.0, 160.0, curv);
    float hollow = smoothstep(-40.0, -160.0, curv);
    col = mix(col, col * 1.25 + vec3(0.01, 0.006, 0.0), edge * P5.w * 0.6);
    col *= 1.0 - hollow * P5.w * 0.45;
    coatRough = mix(coatRough, coatRough * 0.6, edge * P5.w);
    coatRough = mix(coatRough, max(coatRough, 0.45), hollow * P5.w);

    s.albedo = col;
    s.normalWS = baseN;
    s.roughness = rough;
    s.specular = 0.5;
    s.clearcoat = coat;
    s.clearcoatRoughness = coatRough;
    s.clearcoatNormalWS = coatN;
    s.anisotropy = 0.35;
    s.anisotropyDirWS = axisWS;
    s.occlusion = 1.0 - hollow * 0.4 * P5.w;

    // ---- Parcel gilding on carved ridges ------------------------------------------------------
    if (P5.z > 0.0) {
        float gild = smoothstep(60.0, 140.0, curv) * P5.z;
        float wear = smoothstep(0.55, 0.75, mat_fbm(p * 300.0, 3) * 0.5 + 0.5 + edge * 0.2);
        vec3 bole = vec3(0.30, 0.08, 0.04);
        vec3 gold = mix(P6.rgb, bole, wear * 0.8);
        s.albedo = mix(s.albedo, gold, gild);
        s.metallic = gild * (1.0 - wear * 0.8);
        s.roughness = mix(s.roughness, P6.w + wear * 0.3, gild);
        s.clearcoat = mix(coat, 0.0, gild);
        s.normalWS = normalize(mix(baseN, N, gild));
    }
    mat_debugAlbedo(s);
}
