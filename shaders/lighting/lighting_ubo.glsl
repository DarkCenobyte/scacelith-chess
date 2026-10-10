// LightingUBO (binding UBO_LIGHTING = 2): probes, shadow scales, sky/sun parameters.
// Mirrors render::LightingUBOData in src/render/lighting/lighting_data.h (std140, keep in sync).
// Everything radiometric is pre-exposed, except the probe data which is pre-exposed with the
// exposure of the bake: multiply by probeInfo.z to rescale to the current exposure.

#define MAX_LIGHT_PROBES 16

layout(std140, binding = 2) uniform LightingUBO {
    vec4 probeInfo;                         // x count, y specular max mip, z exposure rescale, w mode (0 hemisphere fallback, 1 probes, 2 no ambient)
    vec4 probePos[MAX_LIGHT_PROBES];        // xyz position, w outer influence radius (m)
    vec4 probeBoxMin[MAX_LIGHT_PROBES];     // xyz parallax box min, w priority (1 = local probe overriding the grid)
    vec4 probeBoxMax[MAX_LIGHT_PROBES];     // xyz parallax box max, w inner radius (full weight)
    vec4 shadowScale[4];                    // per cascade: xy metres per shadow uv, z metres per depth unit, w texel size (m)
    vec4 sunParams;                         // x angular radius (rad), y tan(radius) * softness, z softness, w TOA solar illuminance (pre-exposed luminance)
    vec4 sunTOA;                            // rgb top-of-atmosphere solar illuminance (pre-exposed), w viewer altitude (km)
    vec4 skyParams2;                        // x cloud coverage, y cloud time (s), z mie density scale, w sky intensity
    vec4 planarInfo[4];                     // x enabled, y max lod, z width, w height (texels)
    vec4 lightingMisc;                      // x specular AA strength, y shadow blend band, z ambient intensity, w shadow filter (0 PCF .. 3)
    vec4 probeSH[MAX_LIGHT_PROBES * 9];     // L2 SH of irradiance / PI (cosine-convolved, windowed), GPU-written
    vec4 skySH[9];                          // unused (nothing projects the sky any more), keeps the layout
} lighting;

// L2 SH evaluation of coefficients stored at base (9 consecutive vec4) in probeSH.
vec3 shEvalProbe(int base, vec3 n) {
    vec3 r = lighting.probeSH[base + 0].rgb * 0.282095;
    r += lighting.probeSH[base + 1].rgb * (0.488603 * n.y);
    r += lighting.probeSH[base + 2].rgb * (0.488603 * n.z);
    r += lighting.probeSH[base + 3].rgb * (0.488603 * n.x);
    r += lighting.probeSH[base + 4].rgb * (1.092548 * n.x * n.y);
    r += lighting.probeSH[base + 5].rgb * (1.092548 * n.y * n.z);
    r += lighting.probeSH[base + 6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0));
    r += lighting.probeSH[base + 7].rgb * (1.092548 * n.x * n.z);
    r += lighting.probeSH[base + 8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return r;
}
