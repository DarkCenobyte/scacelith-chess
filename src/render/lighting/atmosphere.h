// Physically based sky (Hillaire 2020): transmittance / multiple-scattering / sky-view LUTs on the
// GPU, the sky cubemap bound on TEXUNIT_SKY (+ its SH irradiance), and a CPU evaluation of the
// sun's transmittance so the sun colour and illuminance follow its elevation.
#pragma once
#include "../gpu.h"

namespace render {
namespace lighting {

class Atmosphere {
public:
    bool init();
    void shutdown();

    // Solar illuminance at the top of the atmosphere (lux, linear sRGB, luminance = 128 klux).
    static m::vec3 solarTOA();
    // Sun illuminance (lux, rgb) reaching a viewer at altitudeKm for a direction towards the sun.
    m::vec3 sunIlluminance(m::vec3 dirToSun, float mieScale, float altitudeKm) const;

    // Recomputes the LUTs that depend on the given parameters (cheap when nothing changed).
    void updateLuts(m::vec3 sunDir, float mieScale, float altitudeKm);
    // Renders the sky cubemap (no sun disk, with clouds) and projects it onto SH (slot SH_SLOT_SKY
    // of the SH storage buffer). Needs FrameUBO + LightingUBO bound and current.
    void captureSky(GLuint shBuffer);
    // Binds the LUTs used by shaders/passes/sky.frag (units 0 and 1).
    void bindSkyTextures() const;

    GLuint skyCube() const { return skyCube_.id; }
    GLuint transmittanceLut() const { return transmittance_.id; }
    GLuint skyViewLut() const { return skyView_.id; }
    int skyCubeSize() const { return skyCube_.width; }

private:
    gpu::Texture transmittance_, multiScat_, skyView_, skyCube_;
    float lutMie_ = -1.0f;
    m::vec3 viewSun_{0, -2, 0};
    float viewAlt_ = -1.0f;
};

}  // namespace lighting
}  // namespace render
