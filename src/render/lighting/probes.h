// Runtime light probes (image based lighting + diffuse GI).
//
// Each probe captures the DRAW_STATIC scene into a cubemap (PassId::Probe: full forward shading
// with sun shadows, point lights and the sky). From the capture:
//   * L2 SH irradiance (compute, cosine-convolved, windowed) -> LightingUBO.probeSH
//   * GGX-prefiltered specular mips (compute, filtered importance sampling) -> cube array bound on
//     TEXUNIT_SPECULAR, box-projected in the shader against the probe's proxy box.
// Multiple bounces: bounce 0 is captured without ambient light; each further bounce re-captures
// with the previous bounce's probes active. Baked at startup, on invalidateStatic() and when the
// sun or the sky changes noticeably. Radiance is stored pre-exposed with the bake exposure and
// rescaled in the shader (LightingUBO.probeInfo.z) when the exposure changes.
#pragma once
#include "../gpu.h"
#include "../renderer.h"
#include <vector>

namespace render {
struct LightingUBOData;
namespace lighting {

class LightProbes {
public:
    bool init(int resolution);
    void shutdown();
    void setProbes(const std::vector<LightProbeDesc>& probes);
    const std::vector<LightProbeDesc>& probes() const { return probes_; }
    bool baked() const { return baked_; }
    void invalidate() { baked_ = false; }
    // Full synchronous bake. Needs the sun shadows rendered and the atmosphere LUTs updated this frame.
    void bake(Renderer& r, int bounces);
    // Probe section of the LightingUBO (mode: 1 when baked, else 0 = hemisphere fallback).
    void fillUBO(LightingUBOData& lub, float exposure, bool enabled) const;
    GLuint specularArray() const { return specular_.id; }
    int resolution() const { return res_; }
    int specularLevels() const { return levels_; }
    float bakeExposure() const { return bakeExposure_; }
    double lastBakeMs() const { return lastBakeMs_; }

private:
    void allocate();
    void captureProbe(Renderer& r, int probe);
    void processProbe(int probe, int samples);
    std::vector<LightProbeDesc> probes_;
    gpu::Texture capture_, specular_, depth_;
    gpu::Framebuffer fb_;
    gpu::Buffer sh_;
    int res_ = 128, levels_ = 6, allocated_ = 0;
    bool baked_ = false;
    float bakeExposure_ = 1.0f;
    double lastBakeMs_ = 0.0;
};

}  // namespace lighting
}  // namespace render
