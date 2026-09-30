// Coach marks on the playing surface (Coach mode; transparent, no shadows, not reflected): the
// square the coach talks about and the arrow of a move, in the coach's cobalt light (the blue of
// the "COACH" printed on its chest; the player's own markers are gold and ivory, game_marker.glsl).
// Shapes are drawn in world XZ on a flat quad that covers them with their halo (the quad's uv is
// not used). As with the game markers, a bright line sits in a thin dark rim, so it reads on sunlit
// white marble as on shaded black marble, and every level is relative to the exposure. A soft halo
// spreads the light on the board around the line: a small alpha with the emission divided by it,
// so it adds light without darkening.
//   params[0] = rgb line colour (linear), w = line level after exposure (sunlit white marble ~2.5)
//   params[1] = x dark rim alpha, y halo level, z inner wash level (square), w breathing rate (Hz)
//   inst[0]   = (kind: 0 square, 1 arrow; strength [0,1]; age = seconds since the mark appeared; 0)
//   square:  inst[1] = (centre x, centre z, half side (m), 0)
//   arrow:   inst[1] = (start x, z, corner x, z)   (corner = start when the arrow is straight)
//            inst[2] = (tip x, z, shaft half width (m), head length (m))
//            inst[3] = (head half width (m), flow period (m), flow speed (m/s), 0)
// Timing: a square is announced by a ring that closes onto its outline, then breathes; an arrow
// draws itself from its start to its tip (as the coach's finger traces it), then light flows
// along it towards the tip.

float cm_roundBox(vec2 p, vec2 halfSize, float r) {
    vec2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Distance to segment ab; t receives the position along it in [0,1].
float cm_segment(vec2 p, vec2 a, vec2 b, out float t) {
    vec2 pa = p - a, ba = b - a;
    t = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-12), 0.0, 1.0);
    return length(pa - ba * t);
}

// Isosceles triangle, tip at the origin, base centre at (0, q.y), half width q.x (Quilez).
float cm_triangle(vec2 p, vec2 q) {
    p.x = abs(p.x);
    vec2 a = p - q * clamp(dot(p, q) / dot(q, q), 0.0, 1.0);
    vec2 b = p - q * vec2(clamp(p.x / q.x, 0.0, 1.0), 1.0);
    float k = sign(q.y);
    float d = min(dot(a, a), dot(b, b));
    float s = max(k * (p.x * q.y - p.y * q.x), k * (p.y - q.y));
    return sqrt(d) * sign(s);
}

void surface(in SurfaceInput i, inout Surface s) {
    vec2 pos = i.positionWS.xz;
    float kind = i.instParams[0].x;
    float strength = i.instParams[0].y;
    float age = max(i.instParams[0].z, 0.0);
    float aa = max(max(fwidth(pos.x), fwidth(pos.y)) * 0.75, 1e-5);   // metres per pixel
    float rimAlpha = i.matParams[1].x, haloLevel = i.matParams[1].y;
    float line = 0.0, shade = 0.0, halo = 0.0, wash = 0.0, level = 1.0;

    if (kind < 0.5) {
        // ---- square: rounded outline, clear of every piece base (<= 20 mm; outline at 23 mm) --
        float h = i.instParams[1].z;
        vec2 q = (pos - i.instParams[1].xy) / h;
        float u = aa / h;
        float lw = 0.045;
        float d = cm_roundBox(q, vec2(0.84), 0.26);
        float ad = abs(d);
        line = 1.0 - smoothstep(lw - u, lw + u, ad);
        shade = 1.0 - smoothstep(lw + 0.05 - u, lw + 0.05 + u, ad);
        halo = exp(-max(ad - lw, 0.0) / 0.09) * (d > 0.0 ? 1.0 : 0.3);
        wash = (1.0 - smoothstep(-u, u, d)) * i.matParams[1].z;
        // Arrival: a ring closes onto the outline over 0.7 s and merges with it.
        float t = clamp(age / 0.7, 0.0, 1.0);
        float k = 0.55 * (1.0 - t) * (1.0 - t);
        float ring = 1.0 - smoothstep(lw - u, lw + u, abs(cm_roundBox(q, vec2(0.84 + k), 0.26 + k)));
        line = max(line, ring * t * (1.0 - t) * 3.0);
        // Then a slow breath.
        level = 0.86 + 0.14 * sin(6.2831853 * i.matParams[1].w * age - 1.5707963);
    } else {
        // ---- arrow: polyline start -> corner -> tip, round join, triangular head ----------------
        vec2 a = i.instParams[1].xy, b = i.instParams[1].zw, c = i.instParams[2].xy;
        float hw = i.instParams[2].z, headLen = i.instParams[2].w, headHw = i.instParams[3].x;
        vec2 dir = normalize(c - b + vec2(1e-7, 0.0));
        vec2 shaftEnd = c - dir * headLen * 0.8;
        float t1, t2;
        float l1 = length(b - a), l2 = length(shaftEnd - b);
        float d1 = cm_segment(pos, a, b, t1);
        float d2 = cm_segment(pos, b, shaftEnd, t2);
        float arc = d1 < d2 ? t1 * l1 : l1 + t2 * l2;   // distance along the arrow
        float d = min(d1, d2) - hw;
        vec2 perp = vec2(-dir.y, dir.x);
        vec2 hp = vec2(dot(pos - c, perp), dot(c - pos, dir));
        float dh = cm_triangle(hp, vec2(headHw, headLen)) - 0.0004;   // slightly rounded corners
        if (dh < d) arc = l1 + l2 + max(headLen * 0.8 - hp.y, 0.0);
        d = min(d, dh);
        line = 1.0 - smoothstep(-aa, aa, d);
        shade = 1.0 - smoothstep(0.0012 - aa, 0.0012 + aa, d);
        halo = exp(-max(d, 0.0) / 0.0035);
        // Drawn from the start to the tip over 0.45 s.
        float total = l1 + l2 + headLen * 0.8;
        float g = clamp(age / 0.45, 0.0, 1.0);
        g = g * g * (3.0 - 2.0 * g);
        float front = mix(-0.004, total + 0.004, g);
        float reveal = 1.0 - smoothstep(front - 0.004, front, arc);
        line *= reveal;
        shade *= reveal;
        halo *= reveal;
        // Light flowing towards the tip: soft brighter pulses along the line.
        float period = i.instParams[3].y, speed = i.instParams[3].z;
        float ph = fract((arc - age * speed) / period);
        level = 0.78 + 0.22 * pow(0.5 + 0.5 * cos(6.2831853 * ph), 3.0);
    }

    float aLine = 0.92 * line;
    float a = max(max(aLine, rimAlpha * shade), max(0.04 * halo, wash));
    s.alpha = clamp(a * strength, 0.0, 1.0);
    s.albedo = vec3(0.0);
    s.roughness = 1.0;
    s.specular = 0.0;
    s.clearcoat = 0.0;
    // Coverage blending outputs emission * alpha: the line, the halo and the wash emit, the rim
    // only darkens.
    float light = i.matParams[0].w * (level * (aLine + haloLevel * halo) + wash) / max(a, 1e-4);
    s.emission = i.matParams[0].rgb * light * frame.exposure.y;
}
