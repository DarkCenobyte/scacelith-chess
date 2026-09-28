// Tessellation evaluation: Phong tessellation (Boubekeur & Alexa 2008).
layout(triangles, fractional_odd_spacing, ccw) in;
in TessData { vec3 posOS; vec3 normalOS; vec4 tangentOS; vec2 uv; } tin[];
#include "shaders/passes/mesh_common.glsl"

vec3 projectToPlane(vec3 p, vec3 planeP, vec3 n) { return p - dot(p - planeP, n) * n; }

void main() {
    vec3 b = gl_TessCoord;
    vec3 p = b.x * tin[0].posOS + b.y * tin[1].posOS + b.z * tin[2].posOS;
    vec3 n = normalize(b.x * tin[0].normalOS + b.y * tin[1].normalOS + b.z * tin[2].normalOS);
    vec3 q = b.x * projectToPlane(p, tin[0].posOS, tin[0].normalOS) + b.y * projectToPlane(p, tin[1].posOS, tin[1].normalOS) +
             b.z * projectToPlane(p, tin[2].posOS, tin[2].normalOS);
    const float alpha = 0.75;
    p = mix(p, q, alpha);
    vec4 t = b.x * tin[0].tangentOS + b.y * tin[1].tangentOS + b.z * tin[2].tangentOS;
    vec2 uv = b.x * tin[0].uv + b.y * tin[1].uv + b.z * tin[2].uv;
    emitVertex(p, n, vec4(normalize(t.xyz), tin[0].tangentOS.w), uv, uDraw);
}
