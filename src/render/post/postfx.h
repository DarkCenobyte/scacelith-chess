// Post-processing and screen-space effects (owned by the render-post work package).
//
// Called by render::Renderer at two points of the frame:
//   computeAO(): after the depth/normal prepass, before the opaque forward pass. Must leave the
//                AO texture bound on TEXUNIT_AO (r = ambient occlusion, 1 = unoccluded).
//   resolve():   after opaques, sky and transparents. Consumes the HDR target and writes the final
//                display-referred image to framebuffer 0 (the backbuffer) in sRGB-encoded 8-bit.
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
};

struct PostInputs {
    const render::Renderer::Targets* rt = nullptr;
    const render::FrameUBOData* frame = nullptr;
    GLuint frameUbo = 0;
    GLuint shadowArray = 0;     // sun cascades (raw depth, sampler2DArray)
    float dt = 0.016f;
    int backbufferW = 0, backbufferH = 0;
};

class PostFX {
public:
    PostFX();
    ~PostFX();
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
