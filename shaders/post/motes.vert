// Glinting dust motes: procedural particles in a toroidal box centred in front of the camera
// (motes behind it would be wasted), drifting with the air; only the ones inside a sun beam (shadow-cascade test) get a sprite. Tumbling flakes flash
// when they catch the sun. Drawn after TAA with the unjittered projection (no history smearing).
#include "shaders/post/post_common.glsl"
layout(binding = 5) uniform sampler2DArrayShadow uShadow;
layout(location = 1) uniform float uBoxSize;
out vec2 vLocal;
out vec3 vColor;
out float vDepth;

float sunVisibility(vec3 p) {
    int n = int(frame.shadowParams.x);
    for (int c = 0; c < n; ++c) {
        vec3 s = (frame.shadowMatrix[c] * vec4(p, 1.0)).xyz;
        if (all(greaterThan(s.xy, vec2(0.005))) && all(lessThan(s.xy, vec2(0.995))) && s.z > 0.0 && s.z < 1.0)
            return texture(uShadow, vec4(s.xy, float(c), s.z - 0.0002));
    }
    return 1.0;
}

void main() {
    uint id = uint(gl_InstanceID);
    vec3 h = vec3(hashU(id * 3u + 1u), hashU(id * 3u + 2u), hashU(id * 3u + 3u)) * (1.0 / 4294967296.0);
    float h4 = float(hashU(id * 7u + 11u)) * (1.0 / 4294967296.0);
    float t = post.timing.z;
    vec3 cam = frame.cameraPos.xyz;
    vec3 fwd = -vec3(frame.view[0][2], frame.view[1][2], frame.view[2][2]);
    vec3 boxCentre = cam + fwd * (uBoxSize * 0.4);
    vec3 wobble = vec3(sin(t * (0.21 + 0.3 * h.x) + h.y * 40.0), sin(t * (0.17 + 0.25 * h.y) + h.z * 40.0),
                       sin(t * (0.19 + 0.3 * h.z) + h.x * 40.0)) * 0.03;
    vec3 drift = post.volC.xyz * t * (0.6 + 0.8 * h4) + wobble;
    vec3 local = (fract(h + (drift - boxCentre) / uBoxSize) - 0.5) * uBoxSize;
    vec3 pos = boxCentre + local;
    vec4 clip = frame.viewProjNoJitter * vec4(pos, 1.0);
    vec3 toCam = pos - cam;
    float dist = length(toCam);
    float vis = clip.w > frame.exposure.z * 4.0 ? sunVisibility(pos) : 0.0;
    // Fade near the box boundary so wrapping motes do not pop.
    float edge = 1.0 - smoothstep(0.35, 0.5, max(abs(local.x), max(abs(local.y), abs(local.z))) / uBoxSize);
    vec3 dir = toCam / max(dist, 1e-4);
    float phase = phaseHG(dot(dir, frame.sunDirection.xyz), 0.7);
    float spin = t * (0.8 + 2.5 * h.z) + h.x * 50.0;
    float glint = pow(saturate(sin(spin) * sin(spin * 0.37 + h.y * 9.0)), 24.0) * 30.0;
    float size = 0.6 + 1.4 * h4 * h4;  // relative particle size
    vec3 c = frame.sunRadiance.rgb * (phase + 0.02) * (0.6 + glint) * size * post.volB.w * 1.6 / (1.0 + dist * dist * 0.15);
    c *= vis * edge;
    if (vis * edge < 0.01 || clip.w <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // culled
        vLocal = vec2(0.0);
        vColor = vec3(0.0);
        vDepth = 0.0;
        return;
    }
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    float radiusPx = (1.1 + 0.6 * size) * max(post.timing.w, 0.5);
    clip.xy += corner * radiusPx * 2.0 * post.renderSize.zw * clip.w;
    gl_Position = clip;
    vLocal = corner;
    vColor = c;
    vDepth = clip.w;
}
