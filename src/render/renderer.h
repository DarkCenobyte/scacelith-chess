// Scacelith renderer: forward+ PBR pipeline, HDR, reverse-Z, OpenGL 4.6 DSA.
//
// Frame outline (Renderer::endFrame):
//   1. upload FrameUBO + DrawData SSBO
//   2. sun shadow cascades                         (render-lighting)
//   3. light probes / IBL refresh when dirty       (render-lighting)
//   4. planar reflection passes                    (render-lighting)
//   5. depth + normal + velocity prepass
//   6. PostFX::computeAO                           (render-post)
//   7. opaque forward pass -> HDR colour, normal/roughness, specular, velocity
//   8. sky
//   9. transparents (glass, eyes' cornea)
//  10. PostFX::resolve -> SSR, volumetrics, TAA, DOF, motion blur, bloom, tonemap -> backbuffer
// The UI is drawn by the caller on the backbuffer after endFrame().
#pragma once
#include "../math/math.h"
#include "gpu.h"
#include "material.h"
#include "mesh.h"
#include <memory>
#include <vector>

class PostFX;
class ShaderProgram;

namespace render {

// View camera. Local frame: -Z forward, +Y up, +X right (OpenGL convention).
struct Camera {
    m::vec3 position{0, 1.2f, 1.0f};
    m::quat orientation;
    float fovY = 50.0f * m::DEG;
    float nearZ = 0.03f;
    m::mat4 view() const;
    m::mat4 proj(float aspect) const;
    m::vec3 forward() const { return m::rotate(orientation, m::vec3(0, 0, -1)); }
    m::vec3 right() const { return m::rotate(orientation, m::vec3(1, 0, 0)); }
    m::vec3 up() const { return m::rotate(orientation, m::vec3(0, 1, 0)); }
    // World-space ray through pixel (px,py) (origin top-left) of a w*h viewport.
    m::Ray screenRay(float px, float py, int w, int h) const;
    void lookAt(m::vec3 target, m::vec3 upHint = m::vec3(0, 1, 0));
};

// Lighting environment. Photometric-ish units: the sun in lux, emissive and sky in nits; the
// exposure (EV100) maps them to display. Shaders receive values pre-multiplied by exposure.
struct Environment {
    m::vec3 sunDirection = m::normalize(m::vec3(-0.75f, 0.45f, 0.35f));  // towards the sun
    m::vec3 sunColor{1.0f, 0.95f, 0.88f};                                  // chromaticity
    float sunIlluminance = 80000.0f;                                       // lux (direct)
    float skyIlluminance = 12000.0f;                                       // lux-ish scale for sky dome
    float turbidity = 2.6f;
    float exposureEV100 = 11.5f;  // manual exposure (render-post may add auto exposure on top)
    float time = 0.0f;            // seconds, drives subtle animation (dust, flicker)
};

enum DrawFlags : uint32_t {
    DRAW_CAST_SHADOW = 1u << 0,
    DRAW_STATIC = 1u << 1,          // never moves (shadow/probe caching may rely on it)
    DRAW_NO_REFLECTION = 1u << 2,   // skipped in planar reflection and probe passes
    DRAW_HIDDEN_MAIN = 1u << 3,     // skipped in the main camera pass (e.g. the player's own head)
    DRAW_NO_VELOCITY = 1u << 4,
};

struct DrawItem {
    const Mesh* mesh = nullptr;
    const Material* material = nullptr;
    m::mat4 model;
    m::mat4 prevModel;          // previous frame transform (motion vectors). Defaults to model.
    bool hasPrevModel = false;
    m::vec4 inst[4] = {};       // per-instance parameters (SurfaceInput.instParams)
    uint32_t flags = DRAW_CAST_SHADOW;
    uint32_t objectId = 0;      // stable id: seeds per-object randomness (objectSeed)
};

struct PointLight {
    m::vec3 position;
    float radius = 5.0f;        // influence range (m)
    m::vec3 color{1, 1, 1};
    float intensity = 100.0f;   // candela
};

// Planar reflector (floor, table top, board): renders the mirrored scene into a layer of the
// planar reflection texture array (TEXUNIT_PLANAR). Materials pick a layer via planarReflector.
struct PlanarReflector {
    m::vec3 point{0, 0, 0};
    m::vec3 normal{0, 1, 0};
    float resolutionScale = 0.5f;
    bool enabled = true;
};

enum class Quality { Low, Medium, High, Ultra };

struct RenderSettings {
    Quality quality = Quality::High;
    float renderScale = 1.0f;
    int shadowMapSize = 4096;
    bool planarReflections = true;
    bool ssao = true;
    bool ssr = true;
    bool volumetrics = true;
    bool taa = true;
    bool motionBlur = true;
    bool dof = true;
    bool bloom = true;
    bool tessellation = true;
    void applyPreset(Quality q);
};

// Mirrors FrameUBO in shaders/include/common.glsl (std140, keep in sync!).
struct FrameUBOData {
    m::mat4 view, proj, viewProj, invView, invProj, invViewProj;
    m::mat4 prevViewProj;        // previous frame, unjittered
    m::mat4 viewProjNoJitter;    // current frame, unjittered
    m::vec4 cameraPos;           // xyz, w = time (s)
    m::vec4 resolution;          // w, h, 1/w, 1/h of the current render target
    m::vec4 jitter;              // xy = current jitter (NDC), zw = previous
    m::vec4 sunDirection;        // xyz towards sun, w = angular radius (rad)
    m::vec4 sunRadiance;         // rgb = illuminance * colour * exposure, w = raw lux
    m::vec4 skyParams;           // x = turbidity, y = sky illuminance * exposure, z = EV100, w = frame index
    m::vec4 exposure;            // x = pre-exposure multiplier, y = 1/x, z = near plane, w = aspect
    m::vec4 ambientSky;          // fallback hemisphere ambient (pre-exposed)
    m::vec4 ambientGround;
    m::mat4 shadowMatrix[4];     // world -> shadow NDC-to-[0,1] (xy uv, z depth) per cascade
    m::vec4 shadowCascade[4];    // xyz = cascade centre, w = radius (world) per cascade
    m::vec4 shadowParams;        // x = cascade count, y = normal bias (m), z = depth bias, w = light size factor
    m::vec4 clipPlane;           // world plane (xyz n, w d): keep dot(n,p)+d >= 0. (0,0,0,1) = off
    m::vec4 passInfo;            // x = pass id (0 main,1 prepass,2 shadow,3 planar,4 probe), y = layer, z = lightCount, w = planar count
    m::vec4 planarPlanes[4];     // planar reflector planes (xyz n, w d)
    m::mat4 planarViewProj[4];   // reflected view-proj of each planar reflector (for projective lookup)
};

// Mirrors DrawData in shaders/include/common.glsl (std430).
struct DrawDataGPU {
    m::mat4 model;
    m::mat4 prevModel;
    m::mat4 normalMatrix;   // upper 3x3 used
    m::vec4 matParams[8];
    m::vec4 instParams[4];
    m::vec4 info;           // x = objectSeed, y = planarReflector index (-1 none), z = flags, w = objectId
};

enum class PassId { Main = 0, Prepass = 1, Shadow = 2, Planar = 3, Probe = 4 };

class Renderer {
public:
    Renderer();
    ~Renderer();
    bool init(const RenderSettings& settings);
    void shutdown();
    void resize(int width, int height);
    void setSettings(const RenderSettings& s);
    const RenderSettings& settings() const { return settings_; }

    void beginFrame(const Camera& cam, const Environment& env, float dt);
    void submit(const DrawItem& item);
    void addLight(const PointLight& light);
    int addPlanarReflector(const PlanarReflector& r);  // returns index, call once at scene setup
    PlanarReflector& planarReflector(int i) { return planar_[size_t(i)]; }
    void endFrame();

    // Marks the static environment as changed (re-bake probes / static shadows).
    void invalidateStatic() { staticDirty_ = true; }

    // Fade to black overlay (0 = none, 1 = black) and HUD tint, forwarded to post.
    float fade = 0.0f;

    PostFX& post() { return *post_; }
    int width() const { return width_; }
    int height() const { return height_; }
    const FrameUBOData& frameData() const { return frame_; }
    const Camera& camera() const { return camera_; }

    // Reads the backbuffer (after endFrame, before swap) as tightly packed RGB8, top row first.
    void readBackbuffer(std::vector<uint8_t>& rgb, int& w, int& h);

    // Render targets (valid after resize). Owned here, consumed by PostFX.
    struct Targets {
        gpu::Texture hdr;          // RGBA16F
        gpu::Texture depth;        // DEPTH32F, reverse-Z
        gpu::Texture normalRough;  // RGBA16F: world normal xyz, roughness w
        gpu::Texture specular;     // RGBA8: F0 rgb, a = reflection mask (1 = wants SSR)
        gpu::Texture velocity;     // RG16F: uv(current) - uv(previous)
        gpu::Framebuffer fbMain, fbPrepass;
        int w = 0, h = 0;
    };
    const Targets& targets() const { return rt_; }

    // Draws the submitted items with one pass type into the currently bound framebuffer.
    // Used internally and by lighting code (probe capture, planar reflections).
    void drawScene(PassId pass, bool transparents, uint32_t skipFlags);
    // Uploads a modified copy of the frame UBO (e.g. reflected camera) for sub-passes.
    void uploadFrameUBO(const FrameUBOData& data);
    GLuint frameUBO() const { return frameUbo_.id; }

private:
    struct Item {
        DrawItem d;
        uint32_t drawIndex;
        float viewDepth;
    };
    const ::ShaderProgram* programFor(const Material& mat, PassId pass);
    void createTargets(int w, int h);
    void destroyTargets();
    void renderShadows();
    void renderPlanarReflections();
    void renderSky();
    void bindGlobalTextures();

    RenderSettings settings_;
    int width_ = 0, height_ = 0;
    Targets rt_;
    Camera camera_;
    Environment env_;
    FrameUBOData frame_{};
    m::mat4 prevViewProj_;
    m::vec2 prevJitter_{0, 0};
    uint32_t frameIndex_ = 0;
    float dt_ = 0.0f;
    bool staticDirty_ = true;
    std::vector<Item> items_;
    std::vector<DrawDataGPU> drawData_;
    std::vector<PointLight> lights_;
    std::vector<PlanarReflector> planar_;
    gpu::Buffer frameUbo_, drawSsbo_, lightSsbo_;
    // Sun shadows
    gpu::Texture shadowArray_;
    std::vector<gpu::Framebuffer> shadowFbs_;
    int shadowCascades_ = 2;
    // Planar reflections
    gpu::Texture planarColor_, planarDepth_;
    std::vector<gpu::Framebuffer> planarFbs_;
    int planarW_ = 0, planarH_ = 0;
    // Default textures bound to unused units so samplers are always complete
    gpu::Texture dummy2D_, dummyArray_, dummyCube_, dummyCubeArray_, dummy3D_, dummyShadow_;
    std::unique_ptr<PostFX> post_;
};

// Global renderer instance (created by the app).
Renderer& renderer();
void setRenderer(Renderer* r);

}  // namespace render
