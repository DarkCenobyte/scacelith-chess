// Painted coffers (render-materials): CeilingPainted.
// Each coffer panel is fract(uv) (the hall package tiles uv once per coffer). Painted decoration:
// pale sky-blue field lightening towards the centre, ivory border band with a gold fillet and a
// blue pinstripe, a gilded eight-petal rosette with a domed boss in relief, a ring of painted
// laurel leaves, gold fleurons in the corners. Aged oil/distemper paint: brush strokes, fine
// craquelure, soot darkening towards the mouldings.
//
// Params:
//   [0] ivory colour.rgb, border width (uv)
//   [1] pale blue colour.rgb, rosette radius (uv)
//   [2] gold colour.rgb (F0), gold roughness
//   [3] craquelure amount, brush amount, soot amount, paint roughness
//   [4] laurel colour.rgb, relief depth (m)
#include "shaders/materials/include/matlib.glsl"

float cf_rosette(vec2 p, float r) {
    float a = atan(p.y, p.x);
    float petals = length(p) - r * (0.62 + 0.38 * pow(abs(cos(a * 4.0)), 0.6));
    return petals;
}

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P0 = i.matParams[0], P1 = i.matParams[1], P2 = i.matParams[2], P3 = i.matParams[3], P4 = i.matParams[4];
    vec2 f = fract(i.uv);
    vec2 c = f - 0.5;
    vec2 cellId = floor(i.uv);
    vec4 ch = mat_hash4(cellId.x * 13.0 + cellId.y * 57.0 + 1.0);
    float aa = fwidth(i.uv.x) + fwidth(i.uv.y);  // not of c: fract wraps inside some quads
    float r = length(c);
    float box = max(abs(c.x), abs(c.y));

    // Field and border.
    float bw = P0.a;
    float inField = mat_band(box, 0.5 - bw, aa) ;
    vec3 blue = P1.rgb * (1.0 + 0.12 * (1.0 - smoothstep(0.0, 0.45, r)));
    vec3 col = mix(P0.rgb, blue, inField);
    float fillet = mat_band(box - (0.5 - bw) - 0.012, 0.006, aa);
    float pin = mat_band(box - (0.5 - bw) + 0.02, 0.0025, aa);
    col = mix(col, P1.rgb * 0.45, pin);
    float gold = fillet;

    // Rosette (gilded, in relief) and its boss.
    float rr = P1.a;
    float dRos = cf_rosette(c, rr);
    float ros = mat_band(dRos + 0.5, 0.5, aa);  // fill
    float boss = 1.0 - smoothstep(rr * 0.22 - aa, rr * 0.22 + aa, r);
    gold = max(gold, ros);
    float relief = ros * (0.4 + 0.6 * saturate(-dRos / (rr * 0.25))) + boss * sqrt(saturate(1.0 - sq(r / (rr * 0.22))));
    // Petal veins (painted lines on the gold).
    float ang = atan(c.y, c.x);
    float a8 = ang * 8.0 / TAU;
    // Footprint of a8 without atan's seam (Tarini): fract(a8 + 0.5) wraps between the veins.
    float fa = min(fwidth(a8), fwidth(fract(a8 + 0.5)));
    float veins = mat_band(fract(a8 + 0.5) - 0.5, 0.02, fa) * ros * (1.0 - boss);

    // Laurel wreath: pairs of pointed leaves (one each side of the stem) pointing along the ring,
    // with berries between the pairs; a thin gold stem line.
    float ringR = rr + 0.09;
    float nLeaf = 28.0;
    float la = ang * nLeaf / TAU;
    vec2 lp = vec2((fract(la) - 0.5) * TAU * ringR / nLeaf, r - ringR);
    float side = lp.y >= 0.0 ? 1.0 : -1.0;
    vec2 lq = vec2(lp.x, abs(lp.y));
    lq = mat_rot2(-0.55) * (lq - vec2(-0.004, 0.0));
    // Pointed leaf: intersection of two offset circles (vesica), 30 x 11 (uv units x 1e-3).
    float lr = 0.022;
    float leaf = max(length(lq - vec2(0.0, -lr + 0.0055)), length(lq - vec2(0.0, lr - 0.0055))) - lr;
    leaf = max(leaf, abs(lq.x) - 0.016);
    float laurel = mat_band(leaf + 0.5, 0.5, aa) * inField;
    // Midrib (darker line) and a tone that alternates with the side (light from one side).
    float midrib = mat_band(lq.y, 0.0007, aa) * laurel;
    vec3 lcol = P4.rgb * (side > 0.0 ? 1.08 : 0.9) * (1.0 - midrib * 0.35);
    float berry = mat_band(length(vec2((fract(la + 0.5) - 0.5) * TAU * ringR / nLeaf, r - ringR)), 0.003, aa) * inField;
    col = mix(col, lcol, laurel);
    float ringLine = mat_band(r - ringR, 0.0015, aa) * (1.0 - laurel);
    gold = max(gold, max(ringLine * 0.8, berry));
    // Bead-and-reel painted just inside the fillet.
    float bd = box - (0.5 - bw) + 0.045;
    float along = (abs(c.x) > abs(c.y) ? c.y : c.x) * 60.0;
    vec2 bp = vec2((fract(along) - 0.5) / 60.0, bd);
    float bead = mat_band(length(bp * vec2(0.7, 1.0)), 0.0035, aa) * inField;
    gold = max(gold, bead * 0.9);
    relief += bead * 0.3;

    // Corner fleurons (gold) inside the field.
    vec2 cc = abs(c) - vec2(0.5 - bw - 0.095);
    float fl = cf_rosette(cc, 0.035);
    gold = max(gold, mat_band(fl + 0.5, 0.5, aa) * inField);

    // Aged paint: brush strokes, craquelure, soot towards the edges.
    vec3 pw = vec3(i.uv * 60.0, ch.x * 7.0);
    float brush = mat_fbm(pw * vec3(1.0, 6.0, 1.0), 3) * P3.y;
    col *= 1.0 + brush * 0.06;
    vec3 cv = voronoi(vec3(i.uv * 90.0, ch.y * 3.0));
    float crackField = cv.y - cv.x;
    float crack = mat_band(crackField, 0.03, fwidth(crackField)) * P3.x * (1.0 - mat_subpixel(0.002, mat_footprint(i.positionWS)));
    float soot = smoothstep(0.25, 0.5, box) * P3.z;
    col *= 1.0 - soot * 0.35;
    col = mix(col, col * 0.55, crack);

    vec3 goldCol = P2.rgb * (1.0 - veins * 0.5) * (1.0 - soot * 0.3);
    s.albedo = mix(col, goldCol, gold);
    s.metallic = gold * (1.0 - veins * 0.6);
    s.roughness = mix(P3.w + brush * 0.1 + crack * 0.2, P2.a, gold);
    s.specular = 0.4;
    s.normalWS = bumpFromHeight(i.positionWS, i.normalWS, (relief * P4.a - crack * 0.0002 + brush * 0.00003), 1.0);
    s.occlusion = 1.0 - crack * 0.4 - soot * 0.2;
    mat_debugAlbedo(s);
}
