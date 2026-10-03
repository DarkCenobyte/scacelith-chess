// Planar reflections (floor y = 0, table top, board top): the mirrored scene is rendered at half
// resolution into one layer per reflector, restricted to the reflector's screen rectangle when
// its bounds are known (and skipped when off screen), with frustum + mirror-plane culling and
// small-object rejection. A compute Gaussian mip chain (instead of box mips) then provides the
// roughness- and distance-aware blur used by lighting.glsl; alpha stores the reflected distance.
#pragma once
#include "../gpu.h"
#include <vector>

namespace render {
class Renderer;
namespace lighting {

class PlanarReflections {
public:
    // (Re)creates the targets: w x h, one layer per reflector; nothing while layers == 0 (off).
    void resize(int w, int h, int layers);
    void shutdown();
    // Computes planes, mirrored matrices and which reflectors are visible (FrameUBO + LightingUBO).
    void prepare(Renderer& r);
    // Renders the visible reflectors and builds their Gaussian mip chains.
    void render(Renderer& r);
    GLuint colorArray() const { return color_.id; }
    int layers() const { return int(fbs_.size()); }

private:
    gpu::Texture color_, temp_, depth_;
    std::vector<gpu::Framebuffer> fbs_;
    int w_ = 0, h_ = 0, levels_ = 1;
    bool active_[4] = {};
    int scissor_[4][4] = {};
    m::mat4 reflect_[4];
};

}  // namespace lighting
}  // namespace render
