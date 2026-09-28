// Black lacquer and satin black metal (render-materials): ClockLever, ClockPanel.
// Deep black base with a clear polished lacquer (lever) or a brushed satin finish (panel):
// brush lines along a direction (anisotropic, micro-normal), polishing scratches and fingerprint
// smudges from the polish texture. Object space (positionOS).
//
// Params:
//   [0] colour.rgb, roughness
//   [1] metallic, clear coat, clear coat roughness, brushed amount
//   [2] brush direction (object space xyz), smudge amount
#define MAT_USE_POLISH
#include "shaders/materials/include/matlib.glsl"

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P0 = i.matParams[0], P1 = i.matParams[1], P2 = i.matParams[2];
    vec3 p = i.positionOS;
    vec3 nOS = i.normalOS;
    vec3 dir = normalize(P2.xyz + vec3(1e-4));
    vec3 up = abs(dir.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 U = normalize(cross(up, dir)), V = cross(dir, U);
    vec3 pa = vec3(dot(p, dir), dot(p, U), dot(p, V));
    float fp = mat_footprint(p);
    // Brushed lines: very long along 'dir', ~20 um across.
    float lines = mat_gnoise(pa * vec3(20.0, 9000.0, 9000.0)) * 0.6 + mat_gnoise(pa * vec3(60.0, 26000.0, 26000.0)) * 0.4;
    float vis = 1.0 - mat_subpixel(0.00008, fp);
    vec3 slopeOS;
    vec4 pol = mat_polishTriplanar(p * 3.0, nOS, slopeOS);
    vec3 dirWS = normalize(mat_modelRot() * dir);
    vec3 N = i.normalWS;
    vec3 Ut = normalize(cross(N, dirWS));
    N = normalize(N + Ut * lines * 0.08 * P1.w * vis);
    s.albedo = P0.rgb * (1.0 + lines * 0.08 * P1.w);
    s.metallic = P1.x;
    s.roughness = P0.a + pol.w * 0.2 * P2.w + (1.0 - vis) * 0.05 * P1.w;
    s.specular = 0.5;
    s.normalWS = N;
    s.anisotropy = 0.7 * P1.w;
    s.anisotropyDirWS = dirWS;
    s.clearcoat = P1.y;
    s.clearcoatRoughness = P1.z + pol.z * 0.03 + pol.w * 0.15 * P2.w;
    s.clearcoatNormalWS = mat_normalToWorld(normalize(nOS - slopeOS * 0.02));
    mat_debugAlbedo(s);
}
