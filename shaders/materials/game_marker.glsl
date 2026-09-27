// Board markers drawn by the game on the playing surface (transparent, emissive, no shadows):
// the touched piece's square, the legal destinations when hints are enabled, and the hovered
// piece. Drawn on a flat quad covering one square, uv spanning [0,1]^2.
//   params[0]  = rgb tint, w = luminance (nits, pre-exposure)
//   inst[0]    = (kind, strength, 0, 0)
//                kind 0 = hover (soft inner edge), 1 = touched (square outline),
//                     2 = quiet destination (dot), 3 = capture destination (ring)
void surface(in SurfaceInput i, inout Surface s) {
    vec2 p = i.uv * 2.0 - 1.0;
    float kind = i.instParams[0].x;
    float strength = i.instParams[0].y;
    float aa = max(fwidth(p.x), fwidth(p.y)) * 1.25;
    float a;
    if (kind < 0.5) {
        float d = max(abs(p.x), abs(p.y));
        a = 0.28 * smoothstep(0.62, 0.98, d) * (1.0 - smoothstep(0.98, 0.98 + aa, d));
    } else if (kind < 1.5) {
        float d = max(abs(p.x), abs(p.y));
        a = 0.9 * smoothstep(0.84 - aa, 0.84 + aa, d) * (1.0 - smoothstep(0.95 - aa, 0.95 + aa, d));
    } else if (kind < 2.5) {
        float r = length(p);
        a = 0.8 * (1.0 - smoothstep(0.24 - aa, 0.24 + aa, r));
    } else {
        float r = length(p);
        a = 0.85 * smoothstep(0.80 - aa, 0.80 + aa, r) * (1.0 - smoothstep(0.94 - aa, 0.94 + aa, r));
    }
    s.alpha = clamp(a * strength, 0.0, 1.0);
    s.albedo = vec3(0.0);
    s.roughness = 1.0;
    s.specular = 0.0;
    s.clearcoat = 0.0;
    s.emission = i.matParams[0].rgb * i.matParams[0].w;
}
