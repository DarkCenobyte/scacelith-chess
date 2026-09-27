// Depth/normal/velocity prepass. Geometric normals only (the forward pass rewrites detailed ones).
#include "shaders/include/common.glsl"
#include "shaders/include/surface.glsl"
#include "shaders/passes/fragment_input.glsl"

layout(location = 0) out vec4 outNormalRough;
layout(location = 1) out vec2 outVelocity;

void main() {
    vec3 n = normalize(vin.normalWS);
#ifdef MATERIAL_DOUBLE_SIDED
    if (!gl_FrontFacing) n = -n;
#endif
    outNormalRough = vec4(n, 1.0);
    outVelocity = motionVector();
}
