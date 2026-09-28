// Shadow map pass: depth only (alpha-tested materials discard their holes).
#include "shaders/include/common.glsl"
#ifdef MATERIAL_ALPHA_TEST
#include "shaders/include/surface.glsl"
#include "shaders/passes/fragment_input.glsl"
#include "shaders/include/noise.glsl"
#pragma material
#endif
void main() {
#ifdef MATERIAL_ALPHA_TEST
    SurfaceInput si = buildSurfaceInput();
    Surface ss = defaultSurface(si);
    surface(si, ss);
    if (ss.alpha < 0.5) discard;
#endif
}
