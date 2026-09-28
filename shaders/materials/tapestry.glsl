// Woven wall hangings (render-materials): royal blue or royal red ground with gold-thread
// fleur-de-lis / damask motifs and an ornamental border. uv spans [0,1] over the whole hanging
// (u left->right, v bottom->top); physical size from inst[1].xy.
//
// Layers: baked motif array (unit 0, bake/tapestry.comp: r gold, g damask tone, b ivory, a relief)
// composed as a field repeat + a mitred border; procedural plain weave micro-normal (weft ribs
// over warps) with brocaded gold floats (anisotropic, metallic), per-thread colour jitter,
// abrash (horizontal dye-lot banding), fading, sheen and fuzz.
//
// Params:
//   [0] royal blue ground.rgb, dye variation     [1] royal red ground.rgb, sun fading
//   [2] gold thread colour.rgb, gold roughness   [3] ivory silk colour.rgb, sheen amount
//   [4] repeat size (m), border width (m), weft threads/m, warp threads/m
//   [5] default size (w, h in m), damask tone strength, relief depth (m)
// inst: inst[0].x = ground colour (0 blue, 1 red), inst[0].y = pattern seed (< 0.5 fleur-de-lis
//       trellis, >= 0.5 damask pomegranate; also seeds the dye variation), inst[0].z = extra
//       seed; inst[1].xy = size in m (0 = params[5].xy).
#include "shaders/materials/include/matlib.glsl"

layout(binding = 0) uniform sampler2DArray uMotif;

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P2 = i.matParams[2], P3 = i.matParams[3], P4 = i.matParams[4], P5 = i.matParams[5];
    bool red = i.instParams[0].x > 0.5;
    float design = i.instParams[0].y > 0.5 ? 1.0 : 0.0;
    float seed = i.instParams[0].y * 7.31 + i.instParams[0].z + i.objectSeed;
    vec2 size = i.instParams[1].x > 0.0 ? i.instParams[1].xy : P5.xy;
    vec2 xm = clamp(i.uv, 0.0, 1.0) * size;  // meters from the bottom-left corner
    float bw = P4.y;

    // ---- Motif lookup: field repeat or mitred border ------------------------------------------
    vec2 dEdges = min(xm, size - xm);
    float dEdge = min(dEdges.x, dEdges.y);
    vec4 motif;
    vec2 dx = dFdx(xm), dy = dFdy(xm);  // derivatives before any divergent branch
    float fwE = fwidth(dEdge);
    if (dEdge < bw) {
        // Border: u runs along the edge, measured from the nearest corner (each side is mirrored
        // about its middle, so all four mitres are symmetric and seamless), with a whole number
        // of repeats per half side; v from the outer edge inwards.
        bool vertical = dEdges.x < dEdges.y;
        float L = vertical ? size.y : size.x;
        float a = vertical ? xm.y : xm.x;
        float along = min(a, L - a);
        float period = 0.5 * L / max(1.0, floor(0.5 * L / (bw * 2.0) + 0.5));
        vec2 buv = vec2(along / period, dEdge / bw);
        vec2 ax = vertical ? dx.yx : dx, ay = vertical ? dy.yx : dy;
        motif = textureGrad(uMotif, vec3(buv, 2.0), ax / vec2(period, bw), ay / vec2(period, bw));
    } else {
        // Field: motifs centred on the vertical axis, starting on the border.
        vec2 fuv = vec2((xm.x - 0.5 * size.x) / P4.x + 0.5, (xm.y - bw) / P4.x);
        motif = textureGrad(uMotif, vec3(fuv, design), dx / P4.x, dy / P4.x);
        // Guard lines where the field meets the border: ivory then gold.
        motif.b = max(motif.b, mat_band(dEdge - bw - 0.012, 0.003, fwE));
        motif.r = max(motif.r, mat_band(dEdge - bw - 0.004, 0.0025, fwE));
    }
    float gold = motif.r, tone = motif.g, ivory = motif.b;

    // ---- Weave --------------------------------------------------------------------------------
    float fp = mat_footprint(i.positionWS);
    vec2 w = vec2(xm.x * P4.w, xm.y * P4.z);           // warp index across, weft index up
    float row = floor(w.y);
    float xo = w.x + 0.5 * mod(row, 2.0);               // plain weave: alternate rows offset
    vec2 cell = vec2(floor(xo), row);
    vec2 f = vec2(fract(xo), fract(w.y)) - 0.5;
    // Gold brocade floats over 4 warps: long horizontal threads.
    float floatX = fract(w.x / 4.0 + 0.25 * mod(row, 4.0)) - 0.5;
    float vis = 1.0 - mat_subpixel(1.0 / P4.z, fp);
    vec2 slopeGround = vec2(-sin(PI * f.x) * cos(PI * f.y) * 0.6, -cos(PI * f.x) * sin(PI * f.y));
    vec2 slopeGold = vec2(-sin(PI * floatX) * 0.15, -sin(PI * f.y) * 1.2);
    vec2 slope = mix(slopeGround, slopeGold, gold) * 0.5 * vis;
    float threadH = mix(cos(PI * f.x) * cos(PI * f.y), cos(PI * f.y), gold);

    // ---- Colour -------------------------------------------------------------------------------
    vec3 ground = red ? i.matParams[1].rgb : i.matParams[0].rgb;
    float dyeVar = i.matParams[0].a;
    // Abrash: horizontal bands where a new dye lot was used, plus slow blotches.
    float abrash = mat_gnoise(vec3(xm.y * 1.7 + seed * 11.0, 0.3, seed)) * 0.7 + mat_gnoise(vec3(xm * 0.9, seed * 3.0)) * 0.3;
    ground *= 1.0 + abrash * dyeVar;
    vec4 th = mat_hash4(cell.x * 0.37 + cell.y * 91.7 + seed);
    float jitter = (th.x - 0.5) * 0.12 * vis;
    vec3 toneCol = red ? ground * vec3(0.55, 0.45, 0.5) : ground * vec3(0.5, 0.55, 0.7);
    vec3 col = mix(ground, toneCol, tone * P5.z);
    col = mix(col, P3.rgb, ivory);
    // Sun fading: towards the top and in the lighter tones, a little desaturated.
    float fade = i.matParams[1].a * (0.4 + 0.6 * i.uv.y);
    col = mix(col, vec3(luminance(col)) * 1.1, fade);
    col *= 1.0 + jitter;
    // Thread shading: yarn is darker in the valleys between threads (cavity).
    float cavity = mix(1.0, 0.72 + 0.28 * threadH, vis);

    vec3 goldCol = P2.rgb * (1.0 + jitter * 0.5);
    s.albedo = mix(col * cavity, goldCol * (0.75 + 0.25 * threadH), gold);
    s.metallic = gold * 0.85;
    s.roughness = mix(0.85, P2.a, gold);
    s.roughness = mix(s.roughness, s.roughness + 0.1, 1.0 - vis);
    s.specular = 0.45;
    s.occlusion = mix(1.0, cavity, 0.6);

    // Normals: thread relief + padded embroidery (baked height).
    vec3 N = i.normalWS;
    vec3 T = i.tangentWS, B = i.bitangentWS;
    N = normalize(N + (T * slope.x + B * slope.y));
#if !defined(MAT_LOW_DETAIL)
    N = bumpFromHeight(i.positionWS, N, (motif.a - 0.5) * P5.w, 1.0);
#endif
    s.normalWS = N;
    s.anisotropy = gold * 0.7;
    s.anisotropyDirWS = T;
    // Wool / silk sheen and fuzz (not on the metal threads).
    s.sheenColor = mix(col * 0.6 + vec3(0.02), vec3(0.0), gold) * P3.a;
    s.sheenRoughness = 0.5;
    s.subsurface = 0.15 * (1.0 - gold);
    s.subsurfaceColor = col;
    mat_debugAlbedo(s);
}
