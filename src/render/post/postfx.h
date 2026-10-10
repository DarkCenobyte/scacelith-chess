// Post-processing and screen-space effects (owned by the render-post work package).
//
// Called by render::Renderer at two points of the frame:
//   computeAO(): after the depth/normal prepass, before the opaque forward pass. Must leave the
//                AO texture bound on TEXUNIT_AO (r = ambient occlusion, 1 = unoccluded).
//   resolve():   after opaques, sky and transparents. Consumes the HDR target and writes the final
//                display-referred image to framebuffer 0 (the backbuffer) in sRGB-encoded 8-bit.
//
// Frame graph (render resolution W x H, "half" = W/2 x H/2, all compute unless noted):
//   computeAO:  depth prep (linear depth, HiZ level 0 when SSR is on, checkerboard half-res
//               depth + normal)
//               -> GTAO (half, 1-3 slices) -> temporal + edge-aware denoise (half)
//               -> joint-bilateral upsample (full, R8) bound on TEXUNIT_AO
//               also binds the previous frame's SSR (rgb radiance, a confidence) on TEXUNIT_SSR.
//   resolve:    HiZ min/max pyramid (SSR only) -> colour pyramid -> SSR trace (half, HiZ, GGX VNDF)
//               -> SSR resolve (full, 4-ray reuse) -> SSR temporal
//               -> volumetric sun shafts (half, shadow-cascade raymarch) -> temporal
//               -> combine (HDR + SSR + volumetrics, NaN guard)
//               -> TAA -> dust motes (raster) -> DOF (half-res gather) -> motion blur
//               -> bloom pyramid -> auto-exposure histogram -> display transform (fragment,
//               backbuffer, AgX + grade + grain + CA + vignette + fade).
//
// Output contracts for other packages:
//   TEXUNIT_AO  (sampler2D, render resolution): r = GTAO visibility (1 = open).
//               Multi-bounce is left to the lighting (it knows the albedo).
//   TEXUNIT_SSR (sampler2D, render resolution): previous frame's temporally filtered SSR,
//               rgb = pre-exposed reflected radiance (not multiplied by F), a = confidence.
//               Sample at screenUV - velocity. Only meaningful when the forward pass wants to
//               replace probe specular itself; then set settings.ssrCompositeInResolve = false.
#pragma once
#include "../gpu.h"
#include "../renderer.h"

struct PostSettings {
    bool ssao = true;
    bool ssr = true;
    bool volumetrics = true;
    bool taa = true;
    bool motionBlur = true;
    bool dof = true;
    bool bloom = true;
    bool autoExposure = false;
    float exposureCompensation = 0.0f;  // EV
    float fade = 0.0f;                  // 0 = none, 1 = black
    float dofFocusDistance = 0.8f;      // m
    float dofFStop = 2.8f;
    float filmGrain = 0.02f;
    float vignette = 0.25f;
    float motionBlurShutter = 0.5f;     // fraction of the frame time (180 degree shutter)

    // ---- Extensions (render-post) -------------------------------------------------------
    int quality = 2;                    // 0 Low .. 3 Ultra (sample counts); renderer copies RenderSettings::quality via PostInputs
    // Sample counts of each effect, 0 Low .. 3 Ultra, or -1 for 'quality' (RenderSettings::*Quality).
    int aoQuality = -1, ssrQuality = -1, volumetricQuality = -1, dofQuality = -1, motionBlurQuality = -1;
    bool resetHistory = false;          // one-shot: drop every temporal history (camera cut). Cleared when consumed.
    int debugView = 0;                  // 0 final, 1 AO, 2 SSR, 3 volumetrics, 4 DOF CoC, 5 motion vectors, 6 bloom, 7 HiZ
    // GTAO
    float aoRadius = 0.30f;             // world-space radius (m)
    float aoPower = 1.35f;              // visibility^power
    // SSR
    float ssrIntensity = 1.0f;
    float ssrMaxRoughness = 0.6f;       // fades out towards this roughness
    float ssrThickness = 0.035f;        // assumed object thickness, relative to view depth
    bool ssrCompositeInResolve = false; // the forward pass consumes TEXUNIT_SSR (lighting.glsl)
    // Volumetric sun shafts
    float volumetricDensity = 0.007f;    // scattering coefficient of the dusty air (1/m)
    float volumetricAnisotropy = 0.6f;  // Henyey-Greenstein g
    float volumetricAmbient = 0.06f;    // sky-light in-scattering relative to the fallback ambient (keep low: no fog)
    float volumetricNoise = 0.7f;       // dust density variation [0,1]
    float volumetricMaxDistance = 40.0f;  // march length for surfaces (m)
    float volumetricSkyDistance = 12.0f;  // march length for pixels that see the sky (outdoor air is far cleaner than the hall's dust)
    float dustMotes = 1.0f;             // glinting motes in the beams (0 = off)
    // TAA
    float taaSharpness = 0.3f;          // post-TAA contrast-adaptive sharpening
    // DOF
    float dofFocalLengthMM = 0.0f;      // 0 = derived from the vertical FOV on a 35 mm (24 mm high) sensor
    float dofMaxRadius = 14.0f;         // max CoC radius in pixels at 1080p (scaled with height; <= 16, the tile dilation reach)
    // Bloom
    float bloomIntensity = 0.045f;      // energy-conserving mix
    float bloomScatter = 0.68f;         // weight of the wider levels
    // Auto exposure (histogram, relative to the renderer's manual EV100)
    float autoExposureMinEV = -3.0f;
    float autoExposureMaxEV = 3.0f;
    float autoExposureSpeedUp = 1.0f;   // 1/s when the exposure rises (scene got darker: slow, like dark adaptation)
    float autoExposureSpeedDown = 2.5f; // 1/s when the exposure falls (scene got brighter: fast)
    // Display
    float chromaticAberration = 0.4f;   // lateral CA, pixels at the frame corner (1080p)
    float contrast = 1.12f;             // S-curve slope at mid-grey in the AgX domain (endpoints kept: no clipping)
    float saturation = 1.06f;
    float splitTone = 1.0f;             // warm highlights / cool shadows strength
};

// Depth-of-field looks (35 mm equivalent lens derived from the camera FOV; the game keeps setting
// dofFocusDistance, e.g. to the board at 0.6-0.9 m). Subtle keeps the hall readable, Cinematic
// is the default of PostSettings, Shallow is for close-ups / menus.
enum class DofPreset { Off, Subtle, Cinematic, Shallow };
inline void applyDofPreset(PostSettings& s, DofPreset p) {
    s.dof = p != DofPreset::Off;
    switch (p) {
        case DofPreset::Off: break;
        case DofPreset::Subtle: s.dofFStop = 5.6f; s.dofMaxRadius = 10.0f; break;
        case DofPreset::Cinematic: s.dofFStop = 2.8f; s.dofMaxRadius = 14.0f; break;
        case DofPreset::Shallow: s.dofFStop = 1.8f; s.dofMaxRadius = 16.0f; break;
    }
}

struct PostInputs {
    const render::Renderer::Targets* rt = nullptr;
    const render::FrameUBOData* frame = nullptr;
    GLuint frameUbo = 0;
    GLuint shadowArray = 0;     // sun cascades (raw depth, sampler2DArray)
    float dt = 0.016f;
    int backbufferW = 0, backbufferH = 0;
    int quality = -1;           // RenderSettings::quality (0..3), -1 = use PostSettings::quality
};

class PostFX {
public:
    PostFX();
    ~PostFX();
    PostFX(const PostFX&) = delete;  // owns impl_ and its GL objects
    PostFX& operator=(const PostFX&) = delete;
    bool init();
    void shutdown();
    void resize(int renderW, int renderH);
    void computeAO(const PostInputs& in);
    void resolve(const PostInputs& in);
    PostSettings settings;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
