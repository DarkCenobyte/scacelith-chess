// GPU data shared by the lighting sub-systems (render-lighting package).
// LightingUBOData mirrors LightingUBO in shaders/lighting/lighting_ubo.glsl (std140, keep in sync)
// and is bound on UBO_LIGHTING (= 2) for every pass, including post-processing.
#pragma once
#include "../../math/math.h"
#include <cstddef>
#include <type_traits>

namespace render {

constexpr int MAX_LIGHT_PROBES = 16;

struct LightingUBOData {
    m::vec4 probeInfo;                          // x count, y specular max mip, z exposure rescale, w mode (0 hemisphere, 1 probes, 2 none)
    m::vec4 probePos[MAX_LIGHT_PROBES];         // xyz position, w outer influence radius
    m::vec4 probeBoxMin[MAX_LIGHT_PROBES];      // xyz parallax box min, w priority
    m::vec4 probeBoxMax[MAX_LIGHT_PROBES];      // xyz parallax box max, w inner radius
    m::vec4 shadowScale[4];                     // xy metres per shadow uv, z metres per depth unit, w texel (m)
    m::vec4 sunParams;                          // x angular radius, y tan(radius) * softness, z softness, w TOA luminance (pre-exposed)
    m::vec4 sunTOA;                             // rgb TOA solar illuminance (pre-exposed), w viewer altitude (km)
    m::vec4 skyParams2;                         // x cloud coverage, y cloud time, z mie scale, w sky intensity
    m::vec4 planarInfo[4];                      // x enabled, y max lod, z width, w height
    m::vec4 lightingMisc;                       // x specular AA, y cascade blend band (uv), z ambient intensity, w shadow filter (RenderSettings::shadowFilter)
    // Written on the GPU (compute), copied from the SH storage buffer.
    m::vec4 probeSH[MAX_LIGHT_PROBES * 9];
    m::vec4 skySH[9];                           // unused (nothing projects the sky any more), keeps the layout
};

constexpr size_t LIGHTING_UBO_CPU_SIZE = offsetof(LightingUBOData, probeSH);
constexpr size_t LIGHTING_UBO_PROBE_SH_OFFSET = offsetof(LightingUBOData, probeSH);
constexpr size_t LIGHTING_UBO_SKY_SH_OFFSET = offsetof(LightingUBOData, skySH);
static_assert(LIGHTING_UBO_PROBE_SH_OFFSET == 976 && LIGHTING_UBO_SKY_SH_OFFSET == 3280 && sizeof(LightingUBOData) == 3424 &&
                  std::is_trivially_copyable_v<LightingUBOData>,
              "LightingUBOData mirrors LightingUBO");
// SH storage buffer layout (SSBO_USER while computing): probe k at [k * 9].
constexpr size_t SH_BUFFER_VEC4S = MAX_LIGHT_PROBES * 9;

}  // namespace render
