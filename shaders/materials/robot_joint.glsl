// Robot joint mechanisms (RobotJoint): dark satin anodised metal, soft rubber, polished steel.
//
// matParams:
//   [0] anodised F0 rgb, roughness
//   [1] rubber albedo rgb, roughness
//   [2] polished steel F0 rgb, roughness
//   [3] x = machining strength, y = machining frequency (1/m)
// instParams[0].x = variant: 0 anodised (default), 1 rubber, 2 polished steel

void surface(in SurfaceInput i, inout Surface s) {
    vec3 p = i.positionOS;
    float variant = i.instParams[0].x;
    float n1 = gnoise(p * 180.0 + i.objectSeed * 13.0);
    float n2 = gnoise(p * 1400.0);
    if (variant < 0.5) {
        // Satin anodised aluminium: fine machining marks, slightly bluish graphite.
        // Machining marks fade out before they alias (phase change per pixel > ~1 rad).
        float ph = dot(p, normalize(vec3(0.3, 1.0, 0.2))) * i.matParams[3].y;
        float marks = sin(ph + n1 * 2.0) * (1.0 - smoothstep(0.35, 1.0, fwidth(ph)));
        n2 *= 1.0 - smoothstep(0.2, 0.5, max(max(fwidth(p.x), fwidth(p.y)), fwidth(p.z)) * 1400.0);
        s.metallic = 1.0;
        s.albedo = i.matParams[0].rgb * (1.0 + 0.08 * n1);
        s.roughness = clamp(i.matParams[0].a + 0.05 * n1 + 0.03 * marks * i.matParams[3].x, 0.08, 1.0);
        s.normalWS = bumpFromHeight(i.positionWS, i.normalWS, (marks * 0.3 + n2 * 0.7) * 0.000003, 1.0);
        s.clearcoat = 0.0;
    } else if (variant < 1.5) {
        // Soft rubber / elastomer (tendons, bellows).
        s.metallic = 0.0;
        s.albedo = i.matParams[1].rgb * (1.0 + 0.1 * n1);
        s.roughness = clamp(i.matParams[1].a + 0.08 * n1, 0.2, 1.0);
        s.specular = 0.45;
        s.normalWS = bumpFromHeight(i.positionWS, i.normalWS, n2 * 0.000006, 1.0);
    } else {
        // Polished steel (piston rods, bearing rings).
        s.metallic = 1.0;
        s.albedo = i.matParams[2].rgb;
        s.roughness = clamp(i.matParams[2].a + 0.02 * n1, 0.03, 1.0);
    }
}
