// Baseline post chain: exposure + filmic tonemap + fade. The render-post work package replaces
// this with the full stack (GTAO, SSR, volumetrics, TAA, motion blur, DOF, bloom, grading).
#include "postfx.h"
#include "../shader.h"
#include "../../core/log.h"

struct PostFX::Impl {
    gpu::Texture aoWhite;
};

PostFX::PostFX() : impl_(new Impl) {}
PostFX::~PostFX() { shutdown(); delete impl_; impl_ = nullptr; }

bool PostFX::init() {
    impl_->aoWhite = gpu::createTexture2D(1, 1, GL_R8);
    unsigned char one = 255;
    glTextureSubImage2D(impl_->aoWhite.id, 0, 0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &one);
    return true;
}

void PostFX::shutdown() {
    if (impl_) impl_->aoWhite.destroy();
}

void PostFX::resize(int, int) {}

void PostFX::computeAO(const PostInputs&) {
    glBindTextureUnit(TEXUNIT_AO, impl_->aoWhite.id);
}

void PostFX::resolve(const PostInputs& in) {
    gpu::DebugGroup g("post.resolve");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, in.backbufferW, in.backbufferH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    const ShaderProgram& p = shaders::fullscreen("shaders/post/tonemap.frag");
    if (!p.valid()) return;
    p.use();
    glBindTextureUnit(0, in.rt->hdr.id);
    p.set("uFade", settings.fade);
    p.set("uVignette", settings.vignette);
    p.set("uGrain", settings.filmGrain);
    gpu::drawFullscreenTriangle();
}
