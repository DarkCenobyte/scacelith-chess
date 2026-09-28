// Scoresheet page fields, see scoresheet_page.vert. Every quad writes a signed distance to its ink
// (positive inside) in page texels, encoded 0.5 + d / (2 S) (S = uSpread texels each side):
//   page layers (blend MAX: the union of the shapes): r = handwriting, g = print, b = pen pressure
//   ENTRY (depth test keeps the shape nearest to its ink): r = handwriting, g = reveal time (s):
//     when the passing pen tip reaches this texel (min over the band's pen-down segments of
//     time + distance / speed within a radius, else the band's end time), b = pressure.
layout(binding = 0) uniform sampler2D uAtlas;
uniform float uDensity;  // texels per mm
uniform float uSpread;   // S

#ifdef ENTRY
layout(std430, binding = 5) readonly buffer PenKeys { vec4 keys[]; };  // x, y (page mm), t (s), down
uniform float uRevealSpeed;   // mm/s
uniform float uRevealRadius;  // mm
#endif

in vec2 vUv;
in vec2 vPage;
flat in vec4 vP0;
flat in vec4 vP1;

layout(location = 0) out vec4 oField;

void main() {
    float mode = vP0.x;
    float dMm;
    if (mode > 1.5) {
        // Printed rule: box distance (inside positive).
        vec2 q = abs(vUv) - vP1.xy;
        dMm = -(length(max(q, 0.0)) + min(max(q.x, q.y), 0.0));
    } else {
        float v = textureLod(uAtlas, vUv, 0.0).r;
        dMm = (v - 0.5) * vP0.y;
    }
    dMm += vP0.z;
    float d = dMm * uDensity;  // page texels
    float enc = clamp(0.5 + d / (2.0 * uSpread), 0.0, 1.0);
    if (enc <= 0.0) discard;
    float cov = clamp(d + 0.5, 0.0, 1.0);
#ifdef ENTRY
    int k0 = int(vP1.x + 0.5), k1 = int(vP1.y + 0.5);
    float best = vP1.z;
    for (int k = k0; k < k1; ++k) {
        vec4 A = keys[k];
        if (A.w < 0.5) continue;
        vec4 B = keys[k + 1];
        vec2 ab = B.xy - A.xy;
        float l2 = dot(ab, ab);
        float u = l2 > 1e-12 ? clamp(dot(vPage - A.xy, ab) / l2, 0.0, 1.0) : 0.0;
        float dist = length(vPage - (A.xy + ab * u));
        if (dist > uRevealRadius) continue;
        best = min(best, mix(A.z, B.z, u) + dist / uRevealSpeed);
    }
    gl_FragDepth = enc;
    oField = vec4(enc, best, vP0.w, 1.0);
#else
    if (mode < 0.5) oField = vec4(enc, 0.0, vP0.w * cov, 0.0);
    else oField = vec4(0.0, enc, 0.0, 0.0);
#endif
}
