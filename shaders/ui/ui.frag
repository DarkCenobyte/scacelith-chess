// UI fragment shader: every primitive is a quad evaluated as a signed distance.
//   mode 0  rounded box        vUV = local px from centre, P0 = (half w, half h, radius, stroke), P1.y = AA width
//   mode 1  soft shadow        same box, P1.y = gaussian sigma (px)
//   mode 2  SDF text           vUV = atlas uv, P0 = (softness px, dilation px, atlas px range, -)
//   mode 3  radial falloff     vUV = local px, P0 = (radius x, radius y, t0, t1)
// Output is premultiplied alpha (blend ONE, ONE_MINUS_SRC_ALPHA) in display space.
in vec2 vUV;
in vec4 vColor;
flat in vec4 vP0;
flat in vec4 vP1;

layout(binding = 0) uniform sampler2D uAtlas;

out vec4 outColor;

float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Abramowitz-Stegun style erf approximation (max error ~1e-3, plenty for shadows).
float erfApprox(float x) {
    float s = sign(x);
    float a = abs(x);
    float t = 1.0 + a * (0.278393 + a * (0.230389 + a * (0.000972 + a * 0.078108)));
    t *= t;
    return s - s / (t * t);
}

void main() {
    int mode = int(vP1.x + 0.5);
    float a = 1.0;
    if (mode == 0) {
        float d = sdRoundBox(vUV, vP0.xy, vP0.z);
        if (vP0.w > 0.0) d = abs(d + vP0.w * 0.5) - vP0.w * 0.5;
        a = clamp(0.5 - d / max(vP1.y, 1e-3), 0.0, 1.0);
    } else if (mode == 1) {
        float d = sdRoundBox(vUV, vP0.xy, vP0.z);
        a = 0.5 - 0.5 * erfApprox(d / (vP1.y * 1.41421356));
    } else if (mode == 2) {
        float v = texture(uAtlas, vUV).r;
        vec2 unitRange = vec2(vP0.z) / vec2(textureSize(uAtlas, 0));
        vec2 screenTexSize = 1.0 / max(fwidth(vUV), vec2(1e-7));
        float range = max(0.5 * dot(unitRange, screenTexSize), 1.0);
        float d = range * (v - 0.5) + vP0.y;
        a = clamp(d / max(vP0.x, 1.0) + 0.5, 0.0, 1.0);
    } else {
        float t = length(vUV / max(vP0.xy, vec2(1e-3)));
        a = 1.0 - smoothstep(vP0.z, vP0.w, t);
    }
    vec4 c = vColor;
    c.a *= a;
    if (c.a <= 0.0) discard;
    // Interleaved gradient noise dither: dark translucent gradients would band in 8 bits.
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    c.rgb = max(c.rgb + (n - 0.5) / 255.0, 0.0);
    outColor = vec4(c.rgb * c.a, c.a);
}
