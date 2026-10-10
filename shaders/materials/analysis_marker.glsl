// The review's marks (Analysis mode; transparent, no shadows, not reflected), in the colour of the
// judged move's symbol (analysis::nagColor) or the better move's green (betterMoveColor):
//   tint:  a soft wash over the square the judged move went to, a thin brighter rim along its
//          edge and a faint glow inside the rim; fades in over 0.3 s.
//   arrow: the better move, broad and flat on the board (a square-cornered L for a knight), a
//          flat tail at the edge of the piece on its square, a wide head short of the target's
//          centre. Unlike the coach's thin cobalt lines (coach_marker.glsl) it is a filled shape
//          with soft edges, the board showing through. Its growth is driven by the caller (the
//          tip and corner it gets are where the arrow has grown to this frame).
//   badge: the "pastille": a disc of tinted glass in the symbol's colour with a thin lighter rim,
//          the symbol in white on it (!, ?, and the doubles kerned as one word), a faint shadow
//          under the symbol; on a quad facing the camera, whose uv spans the quad (its pop-in
//          scale is the caller's: the quad's size). It goes through the forward pass's glass path
//          (coloured transmittance), the tint and the arrow through plain coverage blending.
// The tint and the arrow are drawn in world XZ on a flat quad that covers them (the quad's uv is
// not used). Colours come as radiance after exposure (inst): World::submitAnalysisMarks inverts
// the display transform for the symbol's sRGB colour, so the badge shows that colour once
// tonemapped (exactly over black; over a lit square its glass lets a little more of the hue
// through); the tint and the arrow scale it by their levels. White is an absolute level after
// exposure (sunlit white marble ~2.5). Every edge is anti-aliased over a pixel (fwidth), nothing
// writes depth.
//   params[0] = tint: x wash alpha, y rim alpha, z level on a light square, w level on a dark one
//   params[1] = tint: x rim level (x the square's level), y inner glow alpha; arrow: z alpha, w level
//   params[2] = badge: x disc opacity (the light behind it passes in its hue for the rest), y rim
//               lightening [0,1], z symbol level (white), w symbol shadow
//   inst[0]   = (kind: 0 tint, 1 arrow, 2 badge; strength [0,1]; age = seconds since the mark
//                appeared; arrow: head half width (m))
//   tint:  inst[1] = (centre x, centre z, half side (m), 1 on a light square / 0 on a dark one)
//          inst[2] = (rgb colour radiance, 0)
//   arrow: inst[1] = (start x, z, corner x, z)   (corner = start when the arrow is straight)
//          inst[2] = (tip x, z, shaft half width (m), head length (m))
//          inst[3] = (rgb colour radiance, 0)
//   badge: inst[1] = (symbol: 1 !, 2 ?, 3 !!, 4 ??, 5 !?, 6 ?! (analysis::Nag); quad half side
//                     / disc radius; 0; 0)
//          inst[2] = (rgb colour radiance, 0)

float am_roundBox(vec2 p, vec2 halfSize, float r) {
    vec2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// A straight band from a to b, half width hw, flat ends with corners rounded by r; 'along'
// receives the distance from a along it.
float am_band(vec2 p, vec2 a, vec2 b, float hw, float r, out float along) {
    vec2 ab = b - a;
    float l = length(ab);
    vec2 d = l > 1e-6 ? ab / l : vec2(1.0, 0.0);
    vec2 q = p - a;
    along = dot(q, d);
    vec2 local = vec2(along - 0.5 * l, dot(q, vec2(-d.y, d.x)));
    return am_roundBox(local, vec2(0.5 * l, hw), r);
}

// Isosceles triangle, tip at the origin, base centre at (0, q.y), half width q.x (Quilez).
float am_triangle(vec2 p, vec2 q) {
    p.x = abs(p.x);
    vec2 a = p - q * clamp(dot(p, q) / dot(q, q), 0.0, 1.0);
    vec2 b = p - q * vec2(clamp(p.x / q.x, 0.0, 1.0), 1.0);
    float k = sign(q.y);
    float d = min(dot(a, a), dot(b, b));
    float s = max(k * (p.x * q.y - p.y * q.x), k * (p.y - q.y));
    return sqrt(d) * sign(s);
}

// ---- The symbols -------------------------------------------------------------------------------
// Glyph units: a symbol stands from y = -0.5 to 0.5, centred on x = 0. Distances are to the
// centre line of a stroke; the shapes are those strokes widened, with round ends.

// Distance to the arc of radius r around c whose middle is in direction 'mid' (radians), with
// half aperture 'halfAngle'.
float am_arc(vec2 p, vec2 c, float r, float mid, float halfAngle) {
    vec2 q = p - c;
    float cs = cos(mid), sn = sin(mid);
    q = vec2(cs * q.x + sn * q.y, -sn * q.x + cs * q.y);   // the arc's middle on +x
    float phi = atan(q.y, q.x);
    if (abs(phi) <= halfAngle) return abs(length(q) - r);
    return length(q - r * vec2(cos(halfAngle), phi < 0.0 ? -sin(halfAngle) : sin(halfAngle)));
}

// Vertical capsule widening upwards (Quilez's uneven capsule): bottom circle r1 at the origin,
// top circle r2 at height h.
float am_taperedBar(vec2 p, float r1, float r2, float h) {
    p.x = abs(p.x);
    float b = (r1 - r2) / h, a = sqrt(1.0 - b * b);
    float k = dot(p, vec2(-b, a));
    if (k < 0.0) return length(p) - r1;
    if (k > a * h) return length(p - vec2(0.0, h)) - r2;
    return dot(p, vec2(a, b)) - r1;
}

const float AM_DOT_Y = -0.395, AM_DOT_R = 0.098;   // the dot under both symbols

// '!': a bar tapering downwards, then the dot.
float am_exclamation(vec2 p) {
    float bar = am_taperedBar(p - vec2(0.0, -0.10), 0.072, 0.098, 0.505);
    return min(bar, length(p - vec2(0.0, AM_DOT_Y)) - AM_DOT_R);
}

// '?': a hook (an arc from its left end over the top and round to below its centre), an S-bend
// of the opposite curvature that turns it straight down on the axis, a short stem, the dot.
const float AM_HOOK_Y = 0.215, AM_HOOK_R = 0.19, AM_HOOK_W = 0.088;   // centre, radius, half stroke
const float AM_HOOK_FROM = 2.7576, AM_HOOK_TO = -1.2217;              // 158 and -70 degrees
float am_question(vec2 p) {
    float hook = am_arc(p, vec2(0.0, AM_HOOK_Y), AM_HOOK_R, 0.5 * (AM_HOOK_FROM + AM_HOOK_TO),
                        0.5 * (AM_HOOK_FROM - AM_HOOK_TO));
    // The bend leaves the hook's end tangentially and ends heading down at x = 0.
    vec2 e = vec2(0.0, AM_HOOK_Y) + AM_HOOK_R * vec2(cos(AM_HOOK_TO), sin(AM_HOOK_TO));
    float r2 = e.x / (1.0 - cos(AM_HOOK_TO));
    vec2 c2 = e + r2 * vec2(cos(AM_HOOK_TO), sin(AM_HOOK_TO));
    float b0 = AM_HOOK_TO + 3.14159265;
    float bend = am_arc(p, c2, r2, 0.5 * (b0 + 3.14159265), 0.5 * (3.14159265 - b0));
    vec2 sp = p - vec2(0.0, clamp(p.y, -0.115, c2.y));
    float stem = length(sp);
    float stroke = min(min(hook, bend), stem) - AM_HOOK_W;
    return min(stroke, length(p - vec2(0.0, AM_DOT_Y)) - AM_DOT_R);
}

// The symbol in disc units (disc radius 1): a single one 1.1 tall, a double 0.96 tall per glyph,
// kerned tight (the gap between their nearest strokes about a dot's radius).
float am_symbol(int nag, vec2 p) {
    const float S1 = 1.10, S2 = 0.96;
    if (nag == 1) return am_exclamation(p / S1) * S1;
    if (nag == 2) return am_question(p / S1) * S1;
    vec2 q = p / S2;
    float d;
    if (nag == 3) d = min(am_exclamation(q + vec2(0.172, 0.0)), am_exclamation(q - vec2(0.172, 0.0)));
    else if (nag == 4) d = min(am_question(q + vec2(0.273, 0.0)), am_question(q - vec2(0.273, 0.0)));
    else if (nag == 5) d = min(am_exclamation(q + vec2(0.266, 0.0)), am_question(q - vec2(0.130, 0.0)));
    else d = min(am_question(q + vec2(0.130, 0.0)), am_exclamation(q - vec2(0.266, 0.0)));
    return d * S2;
}

void surface(in SurfaceInput i, inout Surface s) {
    float kind = i.instParams[0].x;
    float strength = i.instParams[0].y;
    float age = max(i.instParams[0].z, 0.0);
    float alpha = 0.0;
    vec3 radiance = vec3(0.0);   // after exposure

    if (kind < 0.5) {
        // ---- tint: the square, a hair inside its edges, corners slightly rounded -------------
        vec2 pos = i.positionWS.xz;
        float aa = max(max(fwidth(pos.x), fwidth(pos.y)) * 0.75, 1e-5);
        float h = i.instParams[1].z;
        float d = am_roundBox(pos - i.instParams[1].xy, vec2(h - 0.0005), 0.0025);
        float inside = 1.0 - smoothstep(-aa, aa, d);
        // The rim: 1.4 mm wide, its outer edge on the shape's.
        float rim = 1.0 - smoothstep(0.0007 - aa, 0.0007 + aa, abs(d + 0.0007));
        float glow = inside * exp(d / 0.006);
        vec3 color = i.instParams[2].rgb;
        float level = mix(i.matParams[0].w, i.matParams[0].z, i.instParams[1].w);
        float aWash = inside * i.matParams[0].x, aGlow = glow * i.matParams[1].y, aRim = rim * i.matParams[0].y;
        alpha = max(max(aWash + aGlow * (1.0 - aWash), aRim), 0.0);
        float rimShare = aRim / max(alpha, 1e-4);
        radiance = color * level * mix(1.0, i.matParams[1].x, rimShare);
        float t = clamp(age / 0.3, 0.0, 1.0);
        alpha *= t * t * (3.0 - 2.0 * t);
    } else if (kind < 1.5) {
        // ---- arrow: start -> corner -> tip; two bands meeting square, a triangular head ------
        vec2 pos = i.positionWS.xz;
        float aa = max(max(fwidth(pos.x), fwidth(pos.y)) * 0.75, 1e-5);
        vec3 color = i.instParams[3].rgb;
        vec2 a = i.instParams[1].xy, b = i.instParams[1].zw, c = i.instParams[2].xy;
        float hw = i.instParams[2].z, headLen = i.instParams[2].w, headHw = i.instParams[0].w;
        vec2 dir = normalize(c - b + vec2(1e-7, 0.0));
        // A short arrow (while it grows) has a smaller head; the shaft ends inside the head.
        float len2 = length(c - b) + length(b - a);
        float k = clamp(len2 / headLen, 0.0, 1.0);
        headLen *= k;
        headHw *= mix(0.6, 1.0, k);
        vec2 shaftEnd = c - dir * headLen * 0.6;
        float t;
        float d = 1e9;
        if (length(b - a) > 1e-5) {
            // First leg, run on by the half width so the outer corner is square.
            vec2 d1 = normalize(b - a);
            d = am_band(pos, a, b + d1 * hw, hw, 0.0012, t);
        }
        if (dot(shaftEnd - b, dir) > 0.0)
            d = min(d, am_band(pos, b - (length(b - a) > 1e-5 ? dir * hw : vec2(0.0)), shaftEnd, hw, 0.0012, t));
        vec2 perp = vec2(-dir.y, dir.x);
        vec2 hp = vec2(dot(pos - c, perp), dot(c - pos, dir));
        float dh = am_triangle(hp, vec2(headHw, headLen)) - 0.0006;   // softly rounded corners
        d = min(d, dh);
        alpha = (1.0 - smoothstep(-aa * 1.3, aa * 1.3, d)) * i.matParams[1].z;
        radiance = color * i.matParams[1].w;
    } else {
        // ---- badge: the disc, its rim, the symbol and its shadow ------------------------------
        vec2 p = (i.uv * 2.0 - 1.0) * i.instParams[1].y;     // disc units, +y up
        float aa = max(length(fwidth(p)) * 0.6, 1e-4);
        float d = length(p) - 1.0;
        float rim = 1.0 - smoothstep(0.035 - aa, 0.035 + aa, abs(d + 0.04));
        int nag = int(i.instParams[1].x + 0.5);
        float glyph = 1.0 - smoothstep(-aa, aa, am_symbol(nag, p));
        float shadow = (1.0 - smoothstep(-0.02, 0.10, am_symbol(nag, p - vec2(0.025, -0.045)))) * i.matParams[2].w;
        // Coverage: the disc's anti-aliased edge, the mark's strength, a fade in with the pop
        // (World::submitAnalysisMarks scales the quad). Nothing outside the disc.
        float show = (1.0 - smoothstep(-aa, aa, d)) * strength * clamp(age / 0.08, 0.0, 1.0);
        if (show < 0.002) discard;
        // The disc is tinted glass: of what lies behind it, only its own hue comes through, so
        // its colour holds over a sunlit white square as over a black one, while the board still
        // shows faintly through it. A pastille lit from above: a little lighter at the top; the
        // rim towards white and more opaque.
        vec3 color = i.instParams[2].rgb, white = vec3(i.matParams[2].z);
        vec3 filt = color / max(max(color.r, color.g), max(color.b, 1e-4));
        vec3 body = color * (1.0 + 0.08 * p.y) * (1.0 - shadow);
        body = mix(body, mix(color, white, i.matParams[2].y), rim);
        float opacity = mix(i.matParams[2].x, 0.97, rim);
        vec3 emitted = body * opacity * show;
        vec3 through = mix(vec3(1.0), (1.0 - opacity) * filt, show);
        // The symbol over it, opaque white.
        float ag = glyph * show;
        emitted = white * ag + emitted * (1.0 - ag);
        through *= 1.0 - ag;
        // The forward pass's glass path: dst = emitted + dst * through. Index 1 and a normal
        // facing the eye: no Fresnel reflection, the transmittance is the albedo as it is.
        s.transmission = 1.0;
        s.ior = 1.0;
        s.albedo = through;
        s.normalWS = i.viewDirWS;
        s.roughness = 1.0;
        s.specular = 0.0;
        s.clearcoat = 0.0;
        s.emission = emitted * frame.exposure.y;
        return;
    }

    s.alpha = clamp(alpha * strength, 0.0, 1.0);
    s.albedo = vec3(0.0);
    s.roughness = 1.0;
    s.specular = 0.0;
    s.clearcoat = 0.0;
    // Coverage blending outputs emission * alpha: the levels are after exposure.
    s.emission = radiance * frame.exposure.y;
}
