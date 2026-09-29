// Depth/normal/velocity prepass. Geometric normals only (the forward pass rewrites detailed ones).
// Alpha-tested materials run their surface function here too so holes stay consistent.
#include "shaders/include/common.glsl"
#include "shaders/include/surface.glsl"
#include "shaders/passes/fragment_input.glsl"
#ifdef MATERIAL_ALPHA_TEST
#include "shaders/include/noise.glsl"
#pragma material
#endif

layout(location = 0) out vec4 outNormalRough;
layout(location = 1) out vec2 outVelocity;

void main() {
#ifdef MATERIAL_ALPHA_TEST
    {
        SurfaceInput si = buildSurfaceInput();
        Surface ss = defaultSurface(si);
        surface(si, ss);
        if (ss.alpha < 0.5) discard;
    }
#endif
    vec3 n = normalize(vin.normalWS);
#ifdef MATERIAL_DOUBLE_SIDED
    if (!gl_FrontFacing) n = -n;
#endif
    outNormalRough = vec4(n, 1.0);
    outVelocity = motionVector();
#ifdef MATERIAL_SCREEN_DOOR
    if (screenDoorHidden()) discard;   // the main pass leaves out the same pixels
#endif
}
