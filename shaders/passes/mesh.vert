// Vertex stage for every mesh pass.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;

#ifdef MESH_TESSELLATED
#include "shaders/include/common.glsl"
out TessData {
    vec3 posOS;
    vec3 normalOS;
    vec4 tangentOS;
    vec2 uv;
} tout;
void main() {
    tout.posOS = aPos;
    tout.normalOS = aNormal;
    tout.tangentOS = aTangent;
    tout.uv = aUV;
}
#else
#include "shaders/passes/mesh_common.glsl"
void main() { emitVertex(aPos, aNormal, aTangent, aUV, uDraw); }
#endif
