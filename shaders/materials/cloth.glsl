// Pile fabrics (render-materials). Variants by define:
//   CLOTH_VELVET  ChairVelvet / Curtain: deep red silk velvet. Strong sheen, pile direction
//                 (lighter when looking against the pile), crushed patches where the pile lies
//                 differently, fine fibre glitter, optional thin translucency (curtains).
//   CLOTH_FELT    PieceFelt: green baize under the pieces. Matte fuzzy fibres, soft sheen,
//                 slight pilling / mottling.
// Object space patterns (positionOS, meters) so the cloth pattern follows animated parts.
//
// Params:
//   [0] base colour.rgb, crush amount (velvet) / mottling (felt)
//   [1] sheen colour.rgb, sheen roughness
//   [2] crush / mottle scale (1/m), thin thickness (m, 0 = opaque), subsurface, fibre glitter
//   [3] pile direction (object space xyz), directional sheen strength
#include "shaders/materials/include/matlib.glsl"

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P0 = i.matParams[0], P1 = i.matParams[1], P2 = i.matParams[2], P3 = i.matParams[3];
    vec3 p = i.positionOS;
    vec3 N = i.normalWS;
    float fp = mat_footprint(p);
#if defined(CLOTH_VELVET)
    // Crushed pile: the pile direction wanders in patches.
    vec3 cp = p * P2.x + i.objectSeed * 7.0;
    float c1 = mat_fbm(cp, 4), c2 = mat_fbm(cp + vec3(17.0, 5.0, 9.0), 4);
    float crush = P0.a;
    vec3 pileOS = normalize(normalize(P3.xyz) + vec3(c1, c2, c1 * c2) * 2.0 * crush);
    vec3 pileWS = normalize(mat_modelRot() * pileOS);
    vec3 pileT = pileWS - N * dot(pileWS, N);
    // Fibre tips lean along the pile.
    vec3 Np = normalize(N + pileT * 0.12);
    // Looking against the pile shows the fibre tips (lighter), along it the fibre sides (darker).
    float against = -dot(i.viewDirWS, pileT);
    float dirSheen = 1.0 + P3.w * clamp(against, -1.0, 1.0);
    // Fine fibre glitter (silk).
    float g = hash13(floor(p * 4000.0)) * (1.0 - mat_subpixel(0.00025, fp));
    float glitter = smoothstep(0.93, 1.0, g) * P2.w;
    vec3 base = P0.rgb * (1.0 + 0.25 * c1 * crush) * (1.0 - 0.15 * glitter);
    s.albedo = base;
    s.normalWS = Np;
    s.roughness = 0.9;
    s.specular = 0.35;
    s.sheenColor = P1.rgb * dirSheen * (1.0 + 0.35 * c2 * crush + glitter * 0.8);
    s.sheenRoughness = P1.a;
    s.subsurface = P2.z;
    s.subsurfaceColor = normalize(P0.rgb + 1e-4) * 1.2;
    s.thickness = P2.y;
    s.occlusion = 1.0 - 0.1 * crush * saturate(-c1);
#else  // CLOTH_FELT
    // Fibres: many short random fibres at ~0.3 mm, mottling at a few cm, pilling.
    float vis = 1.0 - mat_subpixel(0.0003, fp);
    vec3 fq = p * 3500.0;
    float fib = mat_gnoise(fq * vec3(1.0, 1.0, 0.3)) * 0.5 + mat_gnoise(fq.yzx * vec3(0.3, 1.0, 1.0) + 13.0) * 0.5;
    float mott = mat_fbm(p * P2.x + i.objectSeed * 9.0, 4);
    float pill = smoothstep(0.55, 0.8, mat_gnoise(p * 900.0 + 3.0)) * (1.0 - mat_subpixel(0.001, fp));
    vec3 base = P0.rgb * (1.0 + mott * P0.a) * (1.0 + fib * 0.18 * vis) * (1.0 + pill * 0.25);
    s.albedo = base;
    s.normalWS = bumpFromHeight(i.positionWS, N, (fib * 0.00004 + pill * 0.00008) * vis, 1.0);
    s.roughness = 0.95;
    s.specular = 0.3;
    s.sheenColor = P1.rgb * (1.0 + 0.3 * fib * vis);
    s.sheenRoughness = P1.a;
    s.subsurface = P2.z;
    s.subsurfaceColor = base * 2.0;
#endif
    mat_debugAlbedo(s);
}
