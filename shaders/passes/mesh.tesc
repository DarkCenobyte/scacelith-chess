// Tessellation control: screen-space adaptive factors for Phong tessellation (smooth
// silhouettes on curved marble pieces, porcelain shells, etc).
#include "shaders/include/common.glsl"
layout(vertices = 3) out;

in TessData { vec3 posOS; vec3 normalOS; vec4 tangentOS; vec2 uv; } tin[];
out TessData { vec3 posOS; vec3 normalOS; vec4 tangentOS; vec2 uv; } tout[];

float edgeFactor(vec3 a, vec3 b) {
    DrawData dd = draws[uDraw];
    vec4 ca = frame.viewProj * (dd.model * vec4(a, 1.0));
    vec4 cb = frame.viewProj * (dd.model * vec4(b, 1.0));
    if (ca.w <= 0.0 || cb.w <= 0.0) return 1.0;
    vec2 sa = ca.xy / ca.w * frame.resolution.xy * 0.5, sb = cb.xy / cb.w * frame.resolution.xy * 0.5;
    const float pixelsPerEdge = 6.0;
    return clamp(length(sa - sb) / pixelsPerEdge, 1.0, 16.0);
}

void main() {
    tout[gl_InvocationID].posOS = tin[gl_InvocationID].posOS;
    tout[gl_InvocationID].normalOS = tin[gl_InvocationID].normalOS;
    tout[gl_InvocationID].tangentOS = tin[gl_InvocationID].tangentOS;
    tout[gl_InvocationID].uv = tin[gl_InvocationID].uv;
    if (gl_InvocationID == 0) {
        float e0 = edgeFactor(tin[1].posOS, tin[2].posOS);
        float e1 = edgeFactor(tin[2].posOS, tin[0].posOS);
        float e2 = edgeFactor(tin[0].posOS, tin[1].posOS);
        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        gl_TessLevelInner[0] = max(e0, max(e1, e2));
    }
}
