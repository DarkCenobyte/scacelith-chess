// Materials = one GLSL "surface function" file + parameters + textures.
//
// A surface file (shaders/materials/*.glsl) implements
//     void surface(in SurfaceInput i, inout Surface s);
// (types in shaders/include/surface.glsl). The renderer wraps it with the shared vertex stage,
// lighting and pass outputs, so the same material works in every pass (main view, planar
// reflections, probe capture, shadows).
//
// Optional displacement: Material::displacement names a second GLSL file implementing
//   vec3 displace(vec3 posOS, vec3 normalOS, vec2 uv, int draw);  // returns the displaced posOS
// It runs in the vertex stage (or tess-eval when tessellated) of every pass, so shadows and
// depth stay consistent. Add "MATERIAL_ALPHA_TEST" to defines to discard when s.alpha < 0.5.
#pragma once
#include "../math/math.h"
#include "gpu.h"
#include <string>
#include <vector>

// Texture units reserved by the renderer. Material textures use units 0..7.
enum TextureUnit : int {
    TEXUNIT_MATERIAL0 = 0,        // .. 7: material textures (declare samplers with layout(binding=N))
    TEXUNIT_SHADOW = 8,           // sampler2DArrayShadow: sun cascades (compare mode on)
    TEXUNIT_SHADOW_DEPTH = 9,     // sampler2DArray: same cascades, raw depth (PCSS blocker search)
    TEXUNIT_IRRADIANCE = 10,      // light-probe irradiance (render-lighting defines the format)
    TEXUNIT_SPECULAR = 11,        // samplerCubeArray: prefiltered specular probes
    TEXUNIT_AO = 12,              // sampler2D: screen-space ambient occlusion (r) + bent cone (gba)
    TEXUNIT_PLANAR = 13,          // sampler2DArray: planar reflections, one layer per reflector
    TEXUNIT_BRDF_LUT = 14,        // sampler2D: split-sum DFG LUT (rg) + sheen/cloth (b)
    TEXUNIT_SSR = 15,             // sampler2D: screen-space reflection result (rgb, a = confidence)
    TEXUNIT_SKY = 16,             // samplerCube: sky radiance (no geometry)
    TEXUNIT_VOLUMETRIC = 17,      // sampler3D / 2D: volumetric lighting (render-post defines)
    TEXUNIT_NOISE = 18,           // sampler2DArray: blue noise
    TEXUNIT_GLOBAL0 = 19,         // .. 23: global textures shared by several materials (render::GlobalTex)
    TEXUNIT_COUNT = 24
};

// Buffer binding points reserved by the renderer.
enum BufferBinding : int {
    UBO_FRAME = 0,     // FrameUBO (shaders/include/common.glsl)
    SSBO_DRAWS = 1,    // DrawData[]
    UBO_LIGHTING = 2,  // lighting/probe data (render-lighting)
    UBO_POST = 3,      // post-processing parameters (render-post)
    SSBO_LIGHTS = 4,   // point/spot lights
    SSBO_USER = 5      // free for passes
};

struct Material {
    std::string name;
    std::string surface = "shaders/materials/standard.glsl";
    std::string displacement;                 // optional, see above
    std::vector<std::string> defines;
    // Available in GLSL as draws[uDraw].matParams[i]; meaning is defined by the surface file.
    m::vec4 params[8] = {};
    GLuint textures[8] = {};                  // bound to units 0..7 when non-zero (any target)
    bool doubleSided = false;
    bool transparent = false;                 // drawn after opaques, sorted back to front, no depth write
    bool tessellated = false;                 // PN-triangle / Phong tessellation for smooth silhouettes
    bool castShadow = true;
    int planarReflector = -1;                 // index of the planar reflection this surface samples
};
