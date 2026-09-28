// Procedural texture bakes of the material library (compute shaders in shaders/materials/bake/).
// Internal to src/render/materials.
#pragma once
#include "../gpu.h"
#include <cstddef>

namespace materials {

struct BakedTextures {
    gpu::Texture marbleSlab;   // RGBA8 2048^2 tileable, 1.6 m: veins, halo, cloud, network (floor)
    gpu::Texture polish;       // RGBA8 1024^2 tileable, 0.5 m: scratch slope, coat roughness, smudges
    gpu::Texture tapestry;     // RGBA8 1024^2 x 3 layers: fleur-de-lis field, damask field, border strip
    double bakeMs = 0.0;       // CPU wall time incl. shader compilation and glFinish
    size_t bytes = 0;          // GPU memory incl. mips
    bool ok = false;
};

// Bakes every texture (GL context required). Missing/failed bakes get neutral 1x1 fallbacks.
BakedTextures bakeAll();
void destroy(BakedTextures& t);

}  // namespace materials
