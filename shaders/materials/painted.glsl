// Painted joinery / ironwork (render-materials): WindowFrame.
// Several coats of oil paint over wood (or iron): satin gloss, brush strokes along the member,
// wood grain telegraphing through the paint, chipped paint on worn edges revealing the primer /
// wood (or rust on iron), dirt in the corners and on the sills. Object space (positionOS).
//
// Params:
//   [0] paint colour.rgb, roughness
//   [1] under colour.rgb (primer / bare wood / rust), chip amount
//   [2] grain telegraph amount, brush amount, dirt amount, metallic under layer (1 = iron)
//   [3] long axis of the members (object space xyz, 0 = auto from the normal), clear coat
#include "shaders/materials/include/matlib.glsl"

void surface(in SurfaceInput i, inout Surface s) {
    vec4 P0 = i.matParams[0], P1 = i.matParams[1], P2 = i.matParams[2], P3 = i.matParams[3];
    vec3 p = i.positionOS;
    vec3 N = i.normalWS;
    vec3 nOS = i.normalOS;
    float fp = mat_footprint(p);
    // Brush direction: along the member (param) or, automatically, vertical unless the face is
    // horizontal.
    vec3 axis = dot(P3.xyz, P3.xyz) > 0.1 ? normalize(P3.xyz) : (abs(nOS.y) > 0.7 ? vec3(1, 0, 0) : vec3(0, 1, 0));
    vec3 up = abs(axis.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 U = normalize(cross(up, axis)), V = cross(axis, U);
    vec3 pa = vec3(dot(p, axis), dot(p, U), dot(p, V));
    // Brush strokes: long along the axis, fine across.
    float brush = mat_fbm(pa * vec3(8.0, 700.0, 700.0), 3);
    // Grain telegraphing: growth rings of the wood under the paint.
    float rings = sin(length(pa.yz + vec2(0.1, 0.3)) * 900.0 + 3.0 * mat_gnoise(pa * vec3(3.0, 40.0, 40.0)));
    float h = brush * 0.00002 * P2.y + rings * 0.000008 * P2.x;
    // Chipped edges and random chips.
    float curv = mat_curvature(i.normalWS, i.positionWS);
    float edge = smoothstep(60.0, 250.0, curv);
    float cn = mat_fbm(p * 90.0 + i.objectSeed * 5.0, 4) * 0.5 + 0.5;
    float chip = smoothstep(0.66, 0.7, cn + edge * 0.3) * P1.a;
    // Dirt in hollows and on upward faces.
    float dirt = (smoothstep(-60.0, -250.0, curv) + saturate(N.y) * 0.4) * P2.z * (0.6 + 0.4 * cn);
    vec3 col = P0.rgb * (1.0 + brush * 0.04);
    col = mix(col, P1.rgb, chip);
    col = mix(col, col * 0.55 + vec3(0.02, 0.018, 0.015), saturate(dirt));
    s.albedo = col;
    s.metallic = chip * P2.w * 0.6;
    s.roughness = mix(P0.a + brush * 0.08, 0.7, max(chip, saturate(dirt) * 0.6));
    s.specular = 0.5;
    s.clearcoat = P3.w * (1.0 - chip);
    s.clearcoatRoughness = P0.a * 0.6;
    s.normalWS = bumpFromHeight(i.positionWS, N, h - chip * 0.0002, 1.0);
    s.clearcoatNormalWS = s.normalWS;
    s.occlusion = 1.0 - saturate(dirt) * 0.3;
    mat_debugAlbedo(s);
}
