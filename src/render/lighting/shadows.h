// Sun shadows: up to 3 cascades fitted to fixed receiver regions (the camera barely moves in this
// game): 0 = table + players (PCSS contact hardening at ~0.6 mm texels), 1 = around the table,
// 2 = the whole hall including the thick window walls. Each cascade's depth range spans the whole
// scene towards the sun so every caster (window mullions, reveals) is included; depth clamping
// catches the rest. DRAW_STATIC casters are cached per cascade and only re-rendered when the sun
// moves or invalidateStatic() is called; each frame copies the cache and adds dynamic casters.
#pragma once
#include "../gpu.h"
#include <vector>

namespace render {
class Renderer;
namespace lighting {

class SunShadows {
public:
    bool init(int size, int cascades, bool cache);
    void shutdown();
    void setRegions(const m::AABB* regions, int count);
    // Fits the cascades for sunDir, renders them and fills the shadow fields of the renderer's
    // FrameUBO / LightingUBO.
    void render(Renderer& r, m::vec3 sunDir, float softness, bool staticDirty);
    GLuint depthArray() const { return live_.id; }
    int size() const { return size_; }
    int cascades() const { return cascades_; }
    bool cacheEnabled() const { return cacheEnabled_; }

private:
    void fit(Renderer& r, m::vec3 sunDir);
    gpu::Texture live_, cache_;
    std::vector<gpu::Framebuffer> liveFbs_, cacheFbs_;
    m::AABB regions_[3];
    int regionCount_ = 0;
    int cascades_ = 0, size_ = 0;
    bool cacheEnabled_ = false;
    m::mat4 vp_[3];            // light view-projection (clip [0,1] depth) per cascade
    m::vec4 scale_[3];         // metres per uv (xy), metres per depth unit (z), texel (w)
    m::vec3 center_[3];
    float radius_[3] = {};
    m::mat4 cachedVp_[3];
    bool cacheValid_[3] = {};
};

}  // namespace lighting
}  // namespace render
