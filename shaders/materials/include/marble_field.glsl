// Procedural marble "field": the geological structure of a marble block, shared by the live
// surface shaders (pieces, board squares, frame: 3D, positionOS) and the floor slab bake
// (bake/marble_slab.comp: periodic 2D slice). Colour is applied by the caller (palettes).
//
// Model (all in "marble units": 1 unit ~ spacing of the main veins):
//   q       = p + folds(p)                  large tectonic folding (domain warp)
//   main    = fbm(q)                        main veins = zero crossing; they swell, thin and
//                                           vanish along their length (width / presence noise)
//   bundle  = level sets main = +-c         thin veins running parallel to the main ones
//   sec     = fbm(2q'), patchy              secondary veins crossing the main ones
//   cap     = fbm(5q), near the veins       capillaries branching off the veins
//   edge    = voronoi F2-F1 of warped q     fracture network (Nero Marquina, Portoro, breccias)
//   cloud   = soft low-frequency tone, pulled towards the veins (grey clouding)
// Veins are shaped with box-filtered bands against each field's pixel footprint (mat_band), so
// they never alias: thin veins fade to their correct average instead of sparkling.
//
// Requires matlib.glsl.

struct MarbleRaw {
    float main;      // signed main field (veins at 0), smooth path
    float braid1;    // main + a*noise: braided companions that split off and rejoin
    float braid2;
    float jag;       // [-1,1] fine edge jaggedness (added to the vein distance)
    float width;     // [0,1] width modulation along the veins
    float presence;  // [0,1] vein visibility along its length
    float sec;       // signed secondary field
    float secMask;   // [0,1] where secondary veins exist (tapers them)
    float cap;       // signed capillary field
    float capMask;   // [0,1] capillary taper
    float edge;      // fracture distance (F2-F1, >= 0)
    float cloud;     // [0,1] clouding
    float mottle;    // [-1,1] fine mottling
};

struct MarbleFootprint { float main, braid1, braid2, sec, cap, edge; };

struct MarbleMask {
    float vein;   // main vein core coverage (crisp line)
    float body;   // broad soft body of the veins (swells where the field is flat)
    float halo;   // diffuse halo around veins (pigment diffused in the calcite)
    float sec;    // secondary + bundle veins
    float cap;    // capillaries
    float net;    // fracture network
    float cloud;  // clouding
    float mottle;
};

// Periodic 3D Voronoi: x = F1, y = F2 (euclidean), z = id hash of the nearest cell.
vec3 mf_voronoi(vec3 p, vec3 per) {
    vec3 i = floor(p), f = fract(p);
    float f1 = 8.0, f2 = 8.0, id = 0.0;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                vec3 g = vec3(x, y, z);
                vec3 c = mod(i + g, per);
                vec3 o = hash33(c + 0.5) * 0.85 + 0.075;
                vec3 r = g + o - f;
                float d = dot(r, r);
                if (d < f1) { f2 = f1; f1 = d; id = hash13(c + 0.5); }
                else if (d < f2) f2 = d;
            }
    return vec3(sqrt(f1), sqrt(f2), id);
}

// p: marble-space point, per: lattice period of p (MAT_NO_PERIOD when not tiling), warp: fold
// amount (0.5..1.5), net: evaluate the fracture network, detail: 0 low (reflections) / 1 full.
MarbleRaw mf_raw(vec3 p, vec3 per, float warp, bool net, int detail) {
    MarbleRaw r;
    int o1 = detail > 0 ? 4 : 2;
    // Large folds.
    vec3 w = vec3(mat_fbm(p * 0.5 + vec3(3.0, 11.0, 7.0), per * 0.5, o1, 0.5),
                  mat_fbm(p * 0.5 + vec3(23.0, 5.0, 13.0), per * 0.5, o1, 0.5),
                  mat_fbm(p * 0.5 + vec3(41.0, 29.0, 2.0), per * 0.5, o1, 0.5));
    vec3 q = p + warp * w;
    // Main veins: smooth long paths; the fine structure only roughens their edges.
    r.main = mat_fbm(q, per, detail > 0 ? 4 : 3, 0.45);
    r.jag = detail > 0 ? mat_fbm(q * 8.0 + vec3(61.0, 7.0, 3.0), per * 8.0, 3, 0.55) : 0.0;
    r.braid1 = r.main + 0.09 * mat_fbm(q * 2.0 + vec3(19.0, 43.0, 5.0), per * 2.0, 2, 0.5);
    r.braid2 = r.main - 0.14 * mat_fbm(q * 2.0 + vec3(37.0, 3.0, 17.0), per * 2.0, 2, 0.5);
    vec2 wp = vec2(mat_gnoise(q * 0.5 + vec3(9.0, 3.0, 5.0), per * 0.5), mat_gnoise(q + vec3(51.0, 13.0, 7.0), per));
    r.width = saturate(0.5 + 0.8 * wp.x);
    r.presence = smoothstep(-0.45, 0.25, wp.y + 0.35 * wp.x);
    // Secondary veins crossing at an angle (different warp), tapering in and out.
    vec3 q2 = q * 2.0 + vec3(7.0, 1.0, 19.0) + 1.3 * w.zxy;
    r.sec = mat_fbm(q2, per * 2.0, detail > 0 ? 3 : 2, 0.45);
    r.secMask = smoothstep(0.05, 0.55, mat_gnoise(q * 2.0 + vec3(31.0, 17.0, 3.0), per * 2.0));
    // Capillaries: short tapered hairlines, mostly near the veins.
    if (detail > 0) {
        r.cap = mat_fbm(q * 4.0 + vec3(3.0, 37.0, 11.0) + 0.8 * w.yzx, per * 4.0, 3, 0.5);
        r.capMask = smoothstep(0.0, 0.5, mat_gnoise(q * 2.0 + vec3(13.0, 3.0, 23.0), per * 2.0));
    } else {
        r.cap = 1.0;
        r.capMask = 0.0;
    }
    // Clouding: low frequency, attracted by the veins.
    float c = mat_fbm(q * 0.5 + vec3(13.0, 7.0, 29.0), per * 0.5, 5, 0.6);
    r.cloud = saturate(smoothstep(-0.2, 0.6, c) * 0.75 + 0.4 * exp(-abs(r.main) * 5.0) * r.presence - 0.1);
    r.mottle = detail > 0 ? mat_fbm(q * 8.0 + vec3(5.0), per * 8.0, 2, 0.5) : 0.0;
    // Fracture network: long polygonal fractures, many of them closed (invisible).
    r.edge = 8.0;
    if (net) {
        // Integer frequency multipliers keep the bake periodic.
        vec3 qn = q + 0.3 * vec3(r.sec, r.main, c);
        vec3 v1 = mf_voronoi(qn, per);
        vec3 v2 = mf_voronoi(qn * 3.0 + vec3(5.0, 9.0, 1.0), per * 3.0);
        float e1 = v1.y - v1.x, e2 = (v2.y - v2.x) * 2.5;
        // Smooth masks: fractures open and close gradually (per-cell masks would give dashes).
        float m1 = smoothstep(-0.1, 0.35, mat_gnoise(qn + vec3(7.0, 1.0, 3.0), per));
        float m2 = smoothstep(0.1, 0.5, mat_gnoise(qn * 2.0 + vec3(2.0, 9.0, 5.0), per * 2.0));
        r.edge = min(e1 + (1.0 - m1) * 2.0, e2 + (1.0 - m2) * 2.0);
    }
    return r;
}

// Shapes raw fields into coverages. Crisp features (vein cores, crossings, capillaries,
// fractures) use a distance estimate d = f / |grad f| (from the field footprint and the
// footprint fpP of p itself) so their widths are true widths in marble units; the vein body is
// banded in field units so it swells and thins irregularly like real veins.
//   veinWidth: core half-width (marble units); netWidth: fracture half-width;
//   presence: global vein density [0,1].
MarbleMask mf_shape(MarbleRaw r, MarbleFootprint fp, float fpP, float veinWidth, float netWidth, float presence) {
    MarbleMask m;
    float pres = smoothstep(0.0, 1.0, saturate(r.presence + presence - 0.5));
    float vw = veinWidth;
    // The gradient is clamped from below (gMin, field units per marble unit): where a field is
    // nearly flat the distance estimate would explode into wide ink puddles.
    float gMin = 0.6 * fpP + 1e-7;
    float dMain = r.main * fpP / max(fp.main, gMin) + r.jag * vw * 1.0;
    float d1 = r.braid1 * fpP / max(fp.braid1, gMin) + r.jag * vw * 0.8;
    float d2 = r.braid2 * fpP / max(fp.braid2, gMin) - r.jag * vw * 0.6;
    float dSec = r.sec * fpP / max(fp.sec, gMin * 2.0) + r.jag * vw * 0.5;
    float dCap = r.cap * fpP / max(fp.cap, gMin * 4.0);
    float dEdge = r.edge * fpP / max(fp.edge, 1e-7);
    float ad = min(abs(dMain), min(abs(d1), abs(d2)) * 1.3);
    // Core lines: the vein and its braided companions, width varying along the vein.
    float wc = vw * (0.3 + 1.4 * r.width * r.width);
    float core = max(mat_band(dMain, wc, fpP), max(mat_band(d1, wc * 0.55, fpP) * 0.85, mat_band(d2, wc * 0.4, fpP) * 0.7));
    m.vein = core * mix(0.05, 1.0, pres);
    // Body: soft shoulders of pigment around the cores (+ a little swelling where the field is flat).
    float wb = vw * (2.0 + 7.0 * r.width);
    float flatness = saturate(1.0 - abs(r.main) / (0.02 + fp.main));
    m.body = (exp(-sq(ad / wb)) * (0.35 + 0.65 * r.width) + flatness * flatness * 0.15) * pres;
    m.halo = exp(-ad / (vw * 18.0)) * (0.3 + 0.7 * r.width) * pres;
    m.sec = mat_band(dSec, vw * 0.6 * r.secMask * (0.3 + r.width), fpP) * mix(0.5, 1.0, pres);
    float nearVein = exp(-ad / (vw * 40.0));
    m.cap = mat_band(dCap, vw * 0.3 * r.capMask, fpP) * smoothstep(0.1, 0.6, nearVein * pres + 0.3 * r.secMask);
    m.net = mat_band(dEdge, netWidth, fpP);
    m.cloud = r.cloud;
    m.mottle = r.mottle;
    return m;
}
