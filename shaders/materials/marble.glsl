// Polished marble (render-materials). One file, four variants selected by a define:
//   MARBLE_PIECE  chess pieces: volumetric veins from positionOS (continuous around the
//                 silhouette), unique per piece: inst[0].x = piece seed (+ objectSeed).
//   MARBLE_BOARD  board squares: square = floor(uv*8) on the top face (board package convention:
//                 uv spans [file/8,(file+1)/8] x [rank/8,(rank+1)/8]); every square is a
//                 different slab (own seed, vein direction) sampled in positionOS; hairline joints.
//   MARBLE_FRAME  board border / any small marble object: positionOS, objectSeed.
//   MARBLE_FLOOR  hall floor tiles: positionWS.xz in meters (uv ignored); baked tileable slab
//                 (unit 0) sampled per tile with a random window + one of 8 orientations,
//                 per-tile tint and lippage, grout joints, a refracted "depth" layer.
//
// Params (Material::params):
//   [0] base colour.rgb, cloud amount          [1] cloud colour.rgb, vein presence [0,1] (3D only)
//   [2] vein colour.rgb, vein amount           [3] capillary/network colour.rgb, capillary amount
//   [4] 3D variants: vein frequency (1/m), vein width (field units), fold warp, anisotropy (>=1)
//       MARBLE_FLOOR: tile size (m), joint width (m), grid origin x, grid origin z (m)
//   [5] subsurface colour.rgb, subsurface amount
//   [6] base roughness (under the polish), clear coat, clear coat roughness, crystal sparkle
//   [7] network amount, network width, polish/scratch amount, vein body amount (3D variants)
// Constants: board squares 55 mm with 0.3 mm hairline joints; floor per-tile tint +-3%.
// inst (per draw): PIECE inst[0].x = piece seed; others unused.
// Textures: 0 = marble slab (FLOOR: r vein cores, g vein body, b cloud+halo, a fracture network),
//           7 = polish (bake/polish.comp).
#define MAT_USE_POLISH
#include "shaders/materials/include/matlib.glsl"
#include "shaders/materials/include/marble_field.glsl"

#ifdef MARBLE_FLOOR
layout(binding = 0) uniform sampler2D uSlab;
const float kSlabSize = 1.6;  // meters covered by the tileable slab texture (bake/marble_slab.comp)
#endif

#ifdef MAT_LOW_DETAIL
const int kDetail = 0;
#else
const int kDetail = 1;
#endif

// Palette mix of the marble masks. Returns albedo, vein coverage (for roughness/SSS) in w.
// bodyAmt: opacity of the soft vein body.
vec4 marblePalette(SurfaceInput i, MarbleMask m, float tint, float bodyAmt) {
    vec4 P0 = i.matParams[0], P1 = i.matParams[1], P2 = i.matParams[2], P3 = i.matParams[3];
    vec3 c = P0.rgb * (1.0 + tint);
    c = mix(c, P1.rgb, saturate(m.cloud * P0.a));
    c *= 1.0 + m.mottle * 0.06;
    float capAll = saturate(m.cap + m.net * i.matParams[7].x);
    c = mix(c, P3.rgb, saturate(capAll * P3.a));
    // Halo: pigment diffused around the veins (seen through the translucent calcite).
    vec3 veinSoft = mix(P1.rgb, P2.rgb, 0.5);
    c = mix(c, veinSoft, saturate(m.halo * bodyAmt * 0.5));
    c = mix(c, mix(veinSoft, P2.rgb, 0.6), saturate(m.body * bodyAmt));
    c = mix(c, mix(P2.rgb, P3.rgb, 0.3), saturate(m.sec * P2.a * 0.8));
    c = mix(c, P2.rgb, saturate(m.vein * P2.a));
    float veins = saturate(max(max(m.vein, m.sec), max(capAll * 0.7, m.body * bodyAmt)));
    return vec4(max(c, vec3(0.0)), veins);
}

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P4 = i.matParams[4], P5 = i.matParams[5], P6 = i.matParams[6], P7 = i.matParams[7];
    vec3 N = i.normalWS;
    float tint = 0.0;
    MarbleMask m;
    vec3 coatN = N;
    float coatRough = P6.z;
    float polish = P7.z;

#ifdef MARBLE_FLOOR
    // ---- Floor: baked slab, per-tile window/orientation --------------------------------------
    float tileSize = max(P4.x, 0.05), jointW = P4.y;
    vec2 xz = i.positionWS.xz - P4.zw;
    vec2 t = xz / tileSize;
    vec2 tile = floor(t), f = t - tile;
    vec4 h = mat_hash4(tile.x * 73.0 + tile.y * 311.0 + 0.5);
    vec4 h2 = mat_hash4(tile.x * 19.0 - tile.y * 97.0 + 7.5);
    int k = int(h.z * 8.0);
    // D4 orientation (4 rotations x mirror): veins change direction from tile to tile.
    mat2 D = mat2((k & 1) == 0 ? vec2(1, 0) : vec2(0, 1), (k & 1) == 0 ? vec2(0, 1) : vec2(-1, 0));
    if ((k & 2) != 0) D = -D;
    if ((k & 4) != 0) D[0] = -D[0];
    vec2 suv = h.xy + D * (f - 0.5) * (tileSize / kSlabSize);
    vec2 gx = D * dFdx(xz) / kSlabSize, gy = D * dFdy(xz) / kSlabSize;
    vec4 sl = textureGrad(uSlab, suv, gx, gy);
    tint = (h.w - 0.5) * 0.06;
#if !defined(MAT_LOW_DETAIL)
    // Refracted depth layer: veins seen a few mm inside the translucent stone.
    vec3 T = refract(-i.viewDirWS, N, 1.0 / 1.55);
    vec2 off = T.xz / max(abs(T.y), 0.2) * 0.004;
    vec4 deep = textureGrad(uSlab, suv + D * off / kSlabSize, gx * 6.0, gy * 6.0);
    sl.rgb = mix(sl.rgb, max(sl.rgb, deep.rgb), 0.45);
    sl.g = max(sl.g, deep.r * 0.8);
#endif
    m.vein = sl.r;
    m.body = sl.g * i.matParams[2].a;
    m.halo = 0.0;
    m.sec = 0.0;
    m.cloud = sl.b;
    m.mottle = (sl.b - 0.5) * 0.4;
    m.cap = 0.0;
    m.net = sl.a;
    // Lippage: every tile is very slightly tilted, the surface gently undulates.
    vec2 tilt = (h2.xy - 0.5) * 0.003;
    vec3 Tt = vec3(1, 0, 0), Bt = vec3(0, 0, 1);
    N = normalize(N + Tt * tilt.x + Bt * tilt.y);
    // Grout joints (box filtered) and eased tile edges.
    vec2 dl = (fract(t + 0.5) - 0.5) * tileSize;  // signed distance to the nearest grid lines (m)
    vec2 fw = fwidth(xz);
    float joint = max(mat_band(dl.x, 0.5 * jointW, fw.x), mat_band(dl.y, 0.5 * jointW, fw.y));
    float ease = 0.0006;
    vec2 e = 1.0 - smoothstep(0.5 * jointW, 0.5 * jointW + ease, abs(dl));
    vec2 slope = sign(dl) * e * 0.25 * (1.0 - joint);
    N = normalize(N + Tt * slope.x + Bt * slope.y);
    coatN = N;
    // Polish: rotary swirls and hairline scratches, faint haze and smudges.
    vec4 pol = mat_polish2D(i.positionWS.xz);
    coatN = normalize(coatN + (Tt * pol.x + Bt * pol.y) * 0.02 * polish);
    coatRough += pol.z * 0.05 * polish + pol.w * 0.04;
    float wav = mat_gnoise(vec3(i.positionWS.xz * 1.3, 3.1));
    coatN = normalize(coatN + Tt * wav * 0.0012 + Bt * mat_gnoise(vec3(i.positionWS.zx * 1.1, 7.7)) * 0.0012);
    vec4 pal = marblePalette(i, m, tint, 1.0);
    s.albedo = mix(pal.rgb, i.matParams[0].rgb * vec3(0.66, 0.63, 0.58), joint);
    s.roughness = mix(P6.x + pal.w * 0.08, 0.85, joint);
    s.clearcoat = P6.y * (1.0 - joint);
    s.clearcoatRoughness = coatRough;
    s.normalWS = N;
    s.clearcoatNormalWS = coatN;
    s.specular = mix(0.3, 0.5, joint);
    s.subsurface = P5.w * (1.0 - 0.5 * pal.w) * (1.0 - joint);
    s.subsurfaceColor = P5.rgb;
    s.subsurfaceRadius = 0.008;
    s.occlusion = 1.0 - joint * 0.5;
#else
    // ---- 3D variants ---------------------------------------------------------------------------
    float freq = P4.x;
    vec3 pos = i.positionOS;
    float seed = i.objectSeed;
#if defined(MARBLE_PIECE)
    seed = fract(i.instParams[0].x * 0.6180339 + i.objectSeed);
#elif defined(MARBLE_BOARD)
    vec2 sq = floor(clamp(i.uv, 0.0, 0.99999) * 8.0);
    seed = fract((sq.x + sq.y * 8.0 + 1.0) * 0.1234567 + i.objectSeed * 0.37);
    vec2 fl = fract(i.uv * 8.0);
#endif
    mat3 R = mat_randomRotation(seed);
    vec3 p = R * pos * freq;
    p.x /= max(P4.w, 1.0);  // anisotropy: veins elongate along one axis of the (rotated) block
    p += (mat_hash4(seed + 0.31).xyz - 0.5) * 97.0;
    MarbleRaw r = mf_raw(p, MAT_NO_PERIOD, P4.z, P7.x > 0.0, kDetail);
    MarbleFootprint fp = MarbleFootprint(fwidth(r.main), fwidth(r.braid1), fwidth(r.braid2), fwidth(r.sec), fwidth(r.cap), fwidth(r.edge));
    m = mf_shape(r, fp, length(fwidth(p)), P4.y, P7.y, i.matParams[1].a);
    tint = (mat_hash4(seed).w - 0.5) * 0.04;
    vec4 pal = marblePalette(i, m, tint, P7.w);
    vec3 albedo = pal.rgb;
    float fpOS = mat_footprint(pos);
    s.specular = 0.3;

    // Calcite crystals: a few facets under the polish catch the light (narrow glints); the rest
    // only shift the tone by a percent. Averaged into roughness when smaller than a pixel.
    vec3 baseN = N;
    float rough = P6.x + pal.w * 0.06;
#if !defined(MAT_LOW_DETAIL)
    float grain = 0.0005;
    float sub = mat_subpixel(grain, fpOS);
    float gid = mat_grainId(pos / grain + seed * 311.0);
    vec4 gh = mat_hash4(gid * 17.0 + 0.5);
    float glint = step(0.9, gh.w) * P6.w * (1.0 - sub);
    vec3 gdir = gh.xyz * 2.0 - 1.0;
    baseN = normalize(N + (gdir - N * dot(gdir, N)) * 0.12 * glint);
    albedo *= 1.0 + (gh.x - 0.5) * 0.02 * (1.0 - sub);
    rough = mix(rough, 0.12, glint);
    s.specular = mix(0.3, 0.9, glint);
    rough = mix(rough, rough + 0.05 * P6.w, sub);
#endif

    // Polish scratches (triplanar in object space so they stay on the piece when it moves).
    vec3 slopeOS;
    vec4 pol = mat_polishTriplanar(pos, i.normalOS, slopeOS);
    vec3 nOS = normalize(i.normalOS - slopeOS * 0.03 * polish);
    coatN = mat_normalToWorld(nOS);
    coatRough += pol.z * 0.04 * polish + pol.w * 0.03 * polish;

#if defined(MARBLE_BOARD)
    // Hairline joints between the square slabs.
    const float jw = 0.00015;
    vec2 dl = (fl - floor(fl + 0.5)) * 0.055;  // distance to the square edges (m), squares are 55 mm
    vec2 fwj = fwidth(i.uv * 8.0) * 0.055;
    float joint = max(mat_band(dl.x, jw, fwj.x), mat_band(dl.y, jw, fwj.y));
    albedo = mix(albedo, albedo * 0.35 + vec3(0.02), joint);
    rough = mix(rough, 0.7, joint);
    coatRough = mix(coatRough, 0.3, joint);
#else
    float joint = 0.0;
#endif
    s.albedo = albedo;
    s.normalWS = baseN;
    s.roughness = rough;
    s.clearcoat = P6.y * (1.0 - joint);
    s.clearcoatRoughness = coatRough;
    s.clearcoatNormalWS = coatN;
    // Veins: white calcite in dark marble is more translucent than the dark matrix, and vice versa.
    float veinLum = luminance(i.matParams[2].rgb);
    float sssVein = veinLum > 0.3 ? 0.35 : P5.w * 0.4;
    s.subsurface = mix(P5.w, sssVein, pal.w);
    s.subsurfaceColor = P5.rgb;
    s.subsurfaceRadius = 0.006;
#endif
    mat_debugAlbedo(s);
}
