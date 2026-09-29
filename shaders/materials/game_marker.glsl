// Board markers drawn by the game on the playing surface (transparent, no shadows): the hovered
// piece, the touched piece's square, the legal destinations when hints are shown and the square
// under the pointer while a piece is in hand. Drawn on a flat quad covering one square, uv
// spanning [0,1]^2. Every mark is a thin bright line inside a dark rim, like a pen line with its
// shadow: the line carries on shaded black marble, the rim on sunlit white marble. Nothing fills
// the square under a piece, and the line's level is relative to the exposure (a fixed luminance
// glowed in the shade and vanished in the sun).
//   params[0]  = rgb accent tint (gold), w = line level after exposure (sunlit white marble ~2.5)
//   params[1]  = rgb neutral tint (ivory)
//   inst[0]    = (kind, strength, 0, 0)
//                kind 0 = hovered piece (corner brackets, neutral), 1 = touched piece (outline),
//                     2 = quiet destination (dot), 3 = capture destination (corner triangles),
//                     4 = pointer target (outline and a light wash),
//                     5 = pointer target the touched piece cannot go to (neutral outline)

float band(float d, float a, float b, float aa) {
    return smoothstep(a - aa, a + aa, d) * (1.0 - smoothstep(b - aa, b + aa, d));
}

float below(float d, float r, float aa) { return 1.0 - smoothstep(r - aa, r + aa, d); }

void surface(in SurfaceInput i, inout Surface s) {
    vec2 p = i.uv * 2.0 - 1.0;
    float kind = i.instParams[0].x;
    float strength = i.instParams[0].y;
    float aa = max(max(fwidth(p.x), fwidth(p.y)) * 0.75, 1e-4);
    float box = max(abs(p.x), abs(p.y));
    float rim = 0.06;
    float line = 0.0, shade = 0.0, wash = 0.0;
    vec3 tint = i.matParams[0].rgb;
    if (kind < 0.5) {
        // Corner brackets a quarter of a side long.
        float arm = min(abs(p.x), abs(p.y));
        line = band(box, 0.80, 0.87, aa) * smoothstep(0.50 - aa, 0.50 + aa, arm);
        shade = band(box, 0.80 - rim, 0.87 + rim, aa) * smoothstep(0.50 - rim - aa, 0.50 - rim + aa, arm);
        tint = i.matParams[1].rgb;
    } else if (kind < 1.5) {
        line = band(box, 0.80, 0.88, aa);
        shade = band(box, 0.80 - rim, 0.88 + rim, aa);
    } else if (kind < 2.5) {
        float r = length(p);
        line = below(r, 0.15, aa);
        shade = below(r, 0.15 + rim, aa);
    } else if (kind < 3.5) {
        float t = abs(p.x) + abs(p.y);
        line = smoothstep(1.52 - aa, 1.52 + aa, t) * below(box, 0.90, aa);
        shade = smoothstep(1.52 - rim - aa, 1.52 - rim + aa, t) * below(box, 0.90 + rim, aa);
    } else {
        line = band(box, 0.80, 0.88, aa);
        shade = band(box, 0.80 - rim, 0.88 + rim, aa);
        if (kind > 4.5) {
            tint = i.matParams[1].rgb;
        } else {
            wash = below(box, 0.80, aa) * 0.12;
        }
    }
    float aLine = 0.95 * line;
    float a = max(max(aLine, 0.42 * shade), wash);
    s.alpha = clamp(a * strength, 0.0, 1.0);
    s.albedo = vec3(0.0);
    s.roughness = 1.0;
    s.specular = 0.0;
    s.clearcoat = 0.0;
    // Coverage blending outputs emission * alpha: the line and the wash emit, the rim only darkens.
    float level = i.matParams[0].w * (aLine + wash) / max(a, 1e-4);
    s.emission = tint * level * frame.exposure.y;
}
