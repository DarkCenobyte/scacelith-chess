// Scoresheet pad (scoresheet package): the pages (ScoresheetPaper) and, with PAPER_CARD, the grey
// card back board and the cloth binding tape (ScoresheetCard).
//
// ---- ScoresheetPaper ------------------------------------------------------------------------
// The ink is not baked as colour: the page texture holds distance fields (crisp at any distance,
// see game/scoresheet.cpp) and the colours, the fibres and the ink finish are applied here.
// uv selects the surface of the page mesh:
//   uv.x in [0,1]  front of a page: uv = page point / page size (0,0 = top-left as its owner reads it)
//   uv.x in [2,3]  back of a page: uv - (2,0) = the same page point (blank; the writing embossed)
//   uv.x >= 4      edge of the page stack: uv.x - 4 = position along the edge (m), uv.y = height
//                  above the table (mm): one line per sheet
// Params:
//   [0] paper colour.rgb (linear), roughness
//   [1] handwriting ink colour.rgb (as it looks on this paper), ink roughness
//   [2] printed ink colour.rgb, printed ink roughness
//   [3] page texture size (w, h texels), distance range S (texels each side), fibre strength
//   [4] entry texture size (w, h texels), page size (w, h mm)
// inst[0] = (page texture layer (< 0: none), entry clock s (< 0: no entry on this surface),
//            entry top (page mm), paper seed)
// Textures: 0 = page fields (2D array RGBA8: r = handwriting, g = print, b = pen pressure;
//               r/g: 0.5 + signed distance / (2 S) at level 0, coverage-preserving mips),
//           1 = entry being written (2D RGBA16F: r = ink field as above, g = reveal time s,
//               b = pressure); covers the full page width from 'entry top' down.
//
// ---- ScoresheetCard (PAPER_CARD) --------------------------------------------------------------
// Object space (positionOS, m). inst[0].x: 0 = card board (grey chipboard, fibrous, speckled),
// 1 = cloth binding tape (fine linen weave, sized, slightly glossy).
// Params: [0] board colour.rgb, roughness; [1] tape colour.rgb, roughness.
#include "shaders/materials/include/matlib.glsl"

#ifndef PAPER_CARD
layout(binding = 0) uniform sampler2DArray uPage;
layout(binding = 1) uniform sampler2D uEntry;

// Coverage of an ink field sample 'v' seen through a pixel footprint of 'fw' level-0 texels (the
// mips store coverage scaled so that this single formula holds at every level, see
// shaders/materials/bake/scoresheet_mips.comp).
float paper_cov(float v, float fw, float S) {
    float k = clamp(fw, 1.0, 2.0 * S);
    return saturate((v - 0.5) * 2.0 * S / k + 0.5);
}
// Footprint (texels) of the level anisotropic filtering samples: the major axis divided by the
// number of taps.
float paper_fw(vec2 tc) {
    vec2 dx = dFdx(tc), dy = dFdy(tc);
    float a = length(dx), b = length(dy);
    float mx = max(a, b), mn = max(min(a, b), 1e-4);
    float taps = min(ceil(mx / mn), 16.0);
    return mx / taps;
}
#endif

// Paper surface micro-structure (page mm): x = height (um, ~[-1,1] * 5), y = albedo variation.
// Formation "clouds" of a few mm, then fibres: short strands in random directions, mostly below
// the pixel at normal viewing distances (then they fade into roughness).
vec2 paper_fibres(vec2 p, float seed, float fpMm) {
    float clouds = mat_fbm(vec3(p * 0.22, seed * 7.0), 3);
    float h = 0.0, a = clouds * 0.012;
    float vis1 = 1.0 - mat_subpixel(0.35, fpMm), vis2 = 1.0 - mat_subpixel(0.08, fpMm);
    if (vis1 > 0.0) {
        // Two families of stretched noise at crossed angles read as a felted mat of fibres.
        vec2 q1 = mat_rot2(0.6 + seed) * p, q2 = mat_rot2(2.1 + seed) * p;
        float f1 = mat_gnoise(vec3(q1 * vec2(1.6, 7.0), seed * 3.0));
        float f2 = mat_gnoise(vec3(q2 * vec2(1.4, 6.0), seed * 5.0 + 1.0));
        float fib = 0.5 * (f1 + f2);
        h += fib * vis1;
        a += fib * 0.006 * vis1;
    }
    if (vis2 > 0.0) h += 0.6 * mat_gnoise(vec3(p * 9.0, seed * 11.0)) * vis2;
    return vec2(h, a);
}

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P0 = i.matParams[0], P1 = i.matParams[1];
    float fp = mat_footprint(i.positionWS);
#ifdef PAPER_CARD
    vec3 pm = i.positionOS * 1000.0;  // mm
    vec3 an = abs(i.normalOS);
    vec2 pl = an.x > an.y && an.x > an.z ? pm.zy : (an.y > an.z ? pm.xz : pm.xy);
    float fpMm = fp * 1000.0;
    float h;
    if (i.instParams[0].x < 0.5) {
        // Chipboard: recycled fibre board, grey-brown with darker and lighter flecks.
        float cl = mat_fbm(vec3(pl * 0.3, 1.0), 3);
        float fleck = mat_gnoise(vec3(pl * 2.2, 3.0));
        float fl = smoothstep(0.55, 0.75, fleck) - 0.6 * smoothstep(0.6, 0.8, -fleck);
        fl *= 1.0 - mat_subpixel(0.4, fpMm);
        s.albedo = P0.rgb * (1.0 + cl * 0.06 + fl * 0.12);
        s.roughness = P0.a;
        s.specular = 0.35;
        h = mat_fbm(vec3(pl * 1.5, 5.0), 2) * (1.0 - mat_subpixel(0.6, fpMm));
        s.normalWS = bumpFromHeight(i.positionWS, i.normalWS, h * 0.00002, 1.0);
    } else {
        // Cloth tape: plain weave (0.25 mm threads) under a sizing that fills and smooths it.
        vec2 w = pl / 0.25;
        float warp = sin(w.x * PI) * 0.5 + 0.5, weft = sin(w.y * PI) * 0.5 + 0.5;
        float cell = mod(floor(w.x) + floor(w.y), 2.0);
        float weave = mix(warp, weft, cell);
        float vis = 1.0 - mat_subpixel(0.25, fpMm);
        float slub = mat_gnoise(vec3(pl.x * 0.4, pl.y * 3.0, 7.0));
        s.albedo = P1.rgb * (1.0 + (weave - 0.5) * 0.25 * vis + slub * 0.05);
        s.roughness = P1.a - 0.1 * weave * vis;
        s.specular = 0.4;
        s.sheenColor = P1.rgb * 0.6 + vec3(0.02);
        s.sheenRoughness = 0.5;
        s.normalWS = bumpFromHeight(i.positionWS, i.normalWS, weave * 0.00003 * vis, 1.0);
    }
#else
    vec4 P2 = i.matParams[2], P3 = i.matParams[3], P4 = i.matParams[4];
    vec4 I0 = i.instParams[0];
    vec2 pageMm = P4.zw;
    float S = P3.z;
    vec2 uv = i.uv;
    float fpMm = fp * 1000.0;
    vec3 paper = P0.rgb;
    float rough = P0.a;
    float heightUm = 0.0;
    float spec = 0.45;
    if (uv.x >= 3.5) {
        // Stack edge: sheets of slightly different tone with dark hairline gaps between them.
        float along = (uv.x - 4.0) * 1000.0;  // mm
        float hMm = uv.y + 0.012 * mat_gnoise(vec3(along * 0.5, 0.0, 2.0));
        float sheet = floor(hMm / 0.1);
        float f = fract(hMm / 0.1);
        float fw = fpMm / 0.1;
        float gap = mat_band(f - 0.5, 0.5 - 0.16, fw);  // 1 inside the sheet body
        float vis = 1.0 - mat_subpixel(0.1, fpMm);
        float hs = mat_hash1(sheet + I0.w * 17.0);
        float tone = 0.84 + 0.16 * hs;
        float body = mix(0.8, gap * tone + (1.0 - gap) * 0.38, vis);
        paper *= body * (1.0 + 0.03 * mat_gnoise(vec3(along * 0.05, hMm * 0.3, 1.0)));
        rough = 0.85;
        spec = 0.3;
        s.occlusion = 0.85;
        // The sheets are not cut perfectly flush: each edge faces slightly up or down.
        heightUm = 0.0;
        vec3 up = normalize(mat3(draws[uDraw].model) * vec3(0.0, 1.0, 0.0));
        s.normalWS = normalize(i.normalWS + up * (hs - 0.5) * 0.5 * vis);
    } else {
        bool back = uv.x >= 1.5;
        vec2 u = back ? uv - vec2(2.0, 0.0) : uv;
        vec2 p = u * pageMm;
        vec2 fib = paper_fibres(p, I0.w + (back ? 0.37 : 0.0), fpMm);
        heightUm = fib.x * 4.0 * P3.w;
        paper *= 1.0 + fib.y * P3.w;
        vec3 alb = paper;
        if (I0.x >= 0.0) {
            vec2 tc = u * P3.xy;
            float fw = paper_fw(tc);
            vec4 f = texture(uPage, vec3(u, I0.x));
            float hand = paper_cov(f.r, fw, S), print = paper_cov(f.g, fw, S);
            float pressure = f.b;
            // The entry being written: ink appears where the pen tip has passed.
            if (I0.y >= 0.0 && !back) {
                float dens = P4.x / pageMm.x;  // entry texels per mm (the page's density)
                vec2 etc = vec2(p.x, p.y - I0.z) * dens;
                // Sampled before the per-pixel range test, so its derivatives see whole quads
                // (the texture clamps to its edge; the value is unused outside the range).
                vec4 e = texture(uEntry, etc / P4.xy);
                float efw = paper_fw(etc);
                if (etc.y >= 0.0 && etc.y <= P4.y) {
                    float shown = smoothstep(0.0, 0.025, I0.y - e.g);
                    float ec = paper_cov(e.r, efw, S) * shown;
                    pressure = mix(pressure, e.b, step(hand, ec));
                    hand = max(hand, ec);
                }
            }
            if (back) {
                // Seen from the back the writing is only a faint embossed ridge and show-through.
                heightUm += hand * 5.0 * (1.0 - mat_subpixel(0.3, fpMm));
                alb *= 1.0 - 0.04 * hand;
            } else {
                // Ballpoint ink: a paste laid in the groove the ball presses into the paper; thin
                // where the ball skips over fibre ridges, glossy where it is thick.
                float skip = saturate(fib.x * 0.8 + 0.2) * (1.0 - mat_subpixel(0.15, fpMm));
                float density = hand * (0.86 + 0.14 * pressure) * (1.0 - 0.15 * skip);
                alb = mix(alb, P1.rgb, density);
                rough = mix(rough, P1.a + 0.15 * (1.0 - pressure), hand);
                spec = mix(spec, 0.55, hand);
                heightUm -= hand * (3.0 + 3.0 * pressure) * (1.0 - mat_subpixel(0.3, fpMm));
                // Printed form: matte offset ink.
                alb = mix(alb, P2.rgb, print * 0.97);
                rough = mix(rough, P2.a, print);
            }
        }
        paper = alb;
    }
    s.albedo = paper;
    s.roughness = rough;
    s.specular = spec;
    s.subsurface = 0.25;
    s.subsurfaceColor = vec3(1.0, 0.97, 0.9);
    s.subsurfaceRadius = 0.0005;
    s.thickness = 0.0001;
    if (heightUm != 0.0) s.normalWS = bumpFromHeight(i.positionWS, i.normalWS, heightUm * 1e-6, 1.0);
#endif
    mat_debugAlbedo(s);
}
