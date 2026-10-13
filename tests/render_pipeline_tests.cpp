// The renderer (src/render/renderer.cpp, shader.cpp, gpu.cpp, post/, lighting/) and the OpenGL
// context's capabilities (src/gl/gl_context.cpp) on the null driver (null_gl.h): the quality
// presets of the test scenes and of Options > Graphics, window resizes (a minimised window
// included), the backbuffer read-back, the frame constants of a non-physical sky, shader reloads
// and how the renderer copes with shaders that do not build, incomplete framebuffers and drivers
// without tessellation or depth clamping, the KHR_debug messages, the procedural texture bakes'
// fallbacks, and the GL objects left after a scene and its renderer end.
#include "test.h"
#include "fake_platform.h"
#include "headless.h"
#include "null_gl.h"
#include "viewer_helpers.h"
#include "game/graphics_quality.h"
#include "gl/gl46.h"
#include "gl/gl_context.h"
#include "render/materials/material_bake.h"
#include "render/post/postfx.h"
#include "render/shader.h"
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

using viewer_test::LogCapture;
using viewer_test::nearly;

namespace {

const std::vector<std::string> kTestbed = {"--scene", "testbed"};

uint64_t frameDraws(headless::App& app) {
    nullgl::resetStats();
    app.frame();
    return nullgl::stats().draws;
}

// An environment variable set for the life of the object, then put back as it was.
class EnvVar {
public:
    EnvVar(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            had_ = true;
            old_ = old;
        }
        set(value);
    }
    ~EnvVar() { set(had_ ? old_.c_str() : nullptr); }
    EnvVar(const EnvVar&) = delete;
    EnvVar& operator=(const EnvVar&) = delete;

private:
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(name_, value ? value : "");
#else
        if (value) setenv(name_, value, 1);
        else unsetenv(name_);
#endif
    }
    const char* name_;
    bool had_ = false;
    std::string old_;
};

// The null driver as it was (every feature, shaders that build, complete framebuffers), the
// capabilities read from it again, and the graphics options that depend on them.
struct DriverRestore {
    ~DriverRestore() {
        nullgl::driver() = nullgl::Driver();
        nullgl::load();
        gl46::afterContextCreated(false);
        game::setGraphicsOptionAvailable(game::GfxTessellation, gl46::caps().tessellation);
    }
};

// The capabilities read from the null driver as it is now.
const gl46::Caps& probeCaps() {
    nullgl::load();
    gl46::afterContextCreated(false);
    return gl46::caps();
}

void dropExtension(const std::string& name) {
    std::vector<std::string>& e = nullgl::driver().extensions;
    for (size_t i = 0; i < e.size(); ++i)
        if (e[i] == name) {
            e.erase(e.begin() + std::ptrdiff_t(i));
            return;
        }
}

bool sameSettings(const render::RenderSettings& a, const render::RenderSettings& b) {
    return a.quality == b.quality && a.renderScale == b.renderScale && a.shadowMapSize == b.shadowMapSize &&
           a.planarReflections == b.planarReflections && a.planarDivisor == b.planarDivisor && a.ssao == b.ssao && a.ssr == b.ssr &&
           a.volumetrics == b.volumetrics && a.taa == b.taa && a.motionBlur == b.motionBlur && a.dof == b.dof && a.bloom == b.bloom &&
           a.tessellation == b.tessellation && a.shadowCascades == b.shadowCascades && a.staticShadowCache == b.staticShadowCache &&
           a.lightProbes == b.lightProbes && a.probeResolution == b.probeResolution && a.probeBounces == b.probeBounces &&
           a.shadowFilter == b.shadowFilter && a.aoQuality == b.aoQuality && a.ssrQuality == b.ssrQuality &&
           a.volumetricQuality == b.volumetricQuality && a.dofQuality == b.dofQuality && a.motionBlurQuality == b.motionBlurQuality;
}

struct LiveObjects {
    int64_t textures, buffers, framebuffers, vertexArrays, programs;
    static LiveObjects now() {
        const nullgl::Stats& s = nullgl::stats();
        return {s.liveTextures, s.liveBuffers, s.liveFramebuffers, s.liveVertexArrays, s.livePrograms};
    }
    bool operator==(const LiveObjects& o) const {
        return textures == o.textures && buffers == o.buffers && framebuffers == o.framebuffers && vertexArrays == o.vertexArrays &&
               programs == o.programs;
    }
};

}  // namespace

// ---- settings without a GPU -----------------------------------------------------------------

// The four levels of the test scenes (--quality): what each one turns on, and Ultra's full scale.
TEST(render_quality_presets_of_the_test_scenes) {
    render::RenderSettings s;
    s.renderScale = 0.5f;
    s.aoQuality = 2;
    s.applyPreset(render::Quality::Low);
    CHECK(s.quality == render::Quality::Low);
    CHECK(!s.planarReflections && !s.ssr && !s.volumetrics && !s.motionBlur && !s.dof && !s.tessellation);
    CHECK(s.ssao && s.taa && s.bloom);
    CHECK_EQ(s.shadowCascades, 2);
    CHECK_EQ(s.probeBounces, 1);
    CHECK_EQ(s.aoQuality, -1);   // the post effects follow the level again
    CHECK(nearly(s.renderScale, 0.5f));
    s.applyPreset(render::Quality::Medium);
    CHECK(s.planarReflections && s.volumetrics && s.motionBlur && !s.ssr && !s.dof);
    CHECK_EQ(s.probeResolution, 64);
    s.applyPreset(render::Quality::High);
    CHECK(s.ssr && s.dof && s.tessellation);
    CHECK_EQ(s.shadowMapSize, 4096);
    CHECK_EQ(s.shadowFilter, 2);
    CHECK(nearly(s.renderScale, 0.5f));
    s.applyPreset(render::Quality::Ultra);
    CHECK_EQ(s.probeBounces, 3);
    CHECK_EQ(s.shadowFilter, 3);
    CHECK(nearly(s.renderScale, 1.0f));
}

// The depth of field looks: off, then the apertures and blur radii of Subtle, Cinematic, Shallow.
TEST(render_depth_of_field_presets) {
    PostSettings s;
    applyDofPreset(s, DofPreset::Off);
    CHECK(!s.dof);
    applyDofPreset(s, DofPreset::Subtle);
    CHECK(s.dof && nearly(s.dofFStop, 5.6f) && nearly(s.dofMaxRadius, 10.0f));
    applyDofPreset(s, DofPreset::Cinematic);
    CHECK(nearly(s.dofFStop, 2.8f) && nearly(s.dofMaxRadius, 14.0f));
    applyDofPreset(s, DofPreset::Shallow);
    CHECK(nearly(s.dofFStop, 1.8f) && nearly(s.dofMaxRadius, 16.0f));
}

// The ray through a pixel: through the centre of the view along the camera's axis, through the
// left edge at half the horizontal field of view, through the top at half the vertical one.
TEST(render_camera_screen_ray) {
    render::Camera c;
    c.position = m::vec3(0, 1, 2);
    c.fovY = 90.0f * m::DEG;
    c.lookAt(m::vec3(0, 1, 0));
    const m::Ray centre = c.screenRay(50.0f, 50.0f, 101, 101);
    CHECK(nearly(centre.o, c.position));
    CHECK(nearly(centre.d, m::vec3(0, 0, -1), 1e-5f));
    CHECK(nearly(c.right(), m::vec3(1, 0, 0), 1e-5f));
    // A 200 x 100 view: the left edge of the first column at -2 (aspect 2, tan 45 = 1), the top at +1.
    const m::Ray left = c.screenRay(-0.5f, 49.5f, 200, 100);
    CHECK(nearly(left.d, m::normalize(m::vec3(-2, 0, -1)), 1e-5f));
    const m::Ray top = c.screenRay(99.5f, -0.5f, 200, 100);
    CHECK(nearly(top.d, m::normalize(m::vec3(0, 1, -1)), 1e-5f));
}

// ---- the renderer of a running scene --------------------------------------------------------

// Every preset of Options > Graphics, then every level of each option, goes to the renderer as
// the settings give it (the targets follow the render scale: 2560 x 1440 at 2x), and a frame is
// drawn with each.
TEST(render_graphics_presets_and_levels_reach_the_renderer) {
    headless::App* app = headless::shared(kTestbed);
    REQUIRE(app);
    render::Renderer& r = app->renderer();
    for (int p = game::PresetVeryLow; p <= game::PresetUltra; ++p) {
        const render::RenderSettings rs = game::renderSettingsFor(game::presetLevels(p), 1.0f);
        r.setSettings(rs);
        CHECK(frameDraws(*app) > 0);
        CHECK(sameSettings(r.settings(), rs));
        CHECK_EQ(r.targets().w, 1280);
    }
    game::GraphicsLevels levels = game::presetLevels(game::PresetHigh);
    for (int o = 0; o < game::GfxOptionCount; ++o) {
        const int high = levels[size_t(o)];
        for (int l = 0; l < game::graphicsLevelCount(o); ++l) {
            levels[size_t(o)] = l;
            const render::RenderSettings rs = game::renderSettingsFor(levels, 1.0f);
            r.setSettings(rs);
            app->frame();
            CHECK(sameSettings(r.settings(), rs));
        }
        levels[size_t(o)] = high;
    }
    // Twice the window's resolution, then the static shadow cache and the light probes off.
    render::RenderSettings rs = game::renderSettingsFor(levels, 2.0f);
    r.setSettings(rs);
    app->frame();
    CHECK_EQ(r.targets().w, 2560);
    CHECK_EQ(r.targets().h, 1440);
    rs.renderScale = 1.0f;
    rs.staticShadowCache = false;
    rs.lightProbes = false;
    r.setSettings(rs);
    CHECK(frameDraws(*app) > 0);
    CHECK_EQ(r.targets().w, 1280);
    CHECK(!r.settings().staticShadowCache && !r.settings().lightProbes);
}

// The window resized: new targets at the new size (at the render scale); minimised (0 x 0) the
// scene goes on without drawing and the targets stay; restored, it draws again.
TEST(render_window_resizes_and_minimised_window) {
    headless::App* app = headless::shared(kTestbed);
    REQUIRE(app);
    render::Renderer& r = app->renderer();
    r.setSettings(game::renderSettingsFor(game::presetLevels(game::PresetHigh), 1.0f));
    fakeplat::setSize(800, 600);
    app->frame();
    CHECK_EQ(r.width(), 800);
    CHECK_EQ(r.targets().w, 800);
    CHECK_EQ(r.targets().h, 600);
    CHECK(nearly(r.frameData().resolution.x, 800.0f) && nearly(r.frameData().resolution.y, 600.0f));
    render::RenderSettings half = r.settings();
    half.renderScale = 0.5f;
    r.setSettings(half);
    CHECK_EQ(r.targets().w, 400);
    CHECK_EQ(r.targets().h, 300);

    fakeplat::setSize(0, 0);
    nullgl::resetStats();
    CHECK(app->frame());
    CHECK(app->frame());
    CHECK_EQ(nullgl::stats().draws, uint64_t(0));
    CHECK_EQ(r.targets().w, 400);

    fakeplat::setSize(1280, 720);
    CHECK(frameDraws(*app) > 0);
    CHECK_EQ(r.targets().w, 640);
    CHECK_EQ(r.targets().h, 360);
    half.renderScale = 1.0f;
    r.setSettings(half);
    CHECK_EQ(r.targets().w, 1280);
}

// The backbuffer read back after a frame (the screenshots of --shot): the window's size, RGB, top
// row first (the null driver's pixels are black).
TEST(render_backbuffer_read_back) {
    headless::App* app = headless::shared(kTestbed);
    REQUIRE(app);
    app->frame();
    std::vector<uint8_t> rgb(7, 0xFF);
    int w = 0, h = 0;
    app->renderer().readBackbuffer(rgb, w, h);
    CHECK_EQ(w, 1280);
    CHECK_EQ(h, 720);
    CHECK_EQ(rgb.size(), size_t(1280) * 720 * 3);
    bool black = true;
    for (uint8_t v : rgb) black = black && v == 0;
    CHECK(black);
}

// A frame with the sun and the sky given in lux (no atmosphere): the sun's luminance and the sky
// level in the frame constants, scaled by the exposure; no jitter without TAA. More light probes
// than the renderer holds are cut to its 16 (with a warning).
TEST(render_frame_constants_without_the_physical_sky) {
    headless::App* app = headless::shared(kTestbed);
    REQUIRE(app);
    render::Renderer& r = app->renderer();
    render::RenderSettings rs = r.settings();
    rs.taa = false;
    r.setSettings(rs);
    render::Camera cam;
    cam.position = m::vec3(0, 1, 3);
    cam.lookAt(m::vec3(0, 0.5f, 0));
    render::Environment env;
    env.physicalSky = false;
    env.sunColor = m::vec3(1, 1, 1);
    env.sunIlluminance = 50000.0f;
    env.skyIlluminance = 8000.0f;
    env.exposureEV100 = 10.0f;
    env.time = 2.5f;
    r.beginFrame(cam, env, 1.0f / 60.0f);
    render::PointLight lamp;
    lamp.position = m::vec3(0, 2, 0);
    lamp.setSpot(m::vec3(0, -2, 0), 0.2f, 0.4f);
    CHECK(nearly(lamp.direction, m::vec3(0, -1, 0)));
    CHECK(nearly(lamp.spotCosOuter, std::cos(0.4f)));
    r.addLight(lamp);
    r.endFrame();
    const render::FrameUBOData& f = r.frameData();
    const float exposure = 1.0f / (1.2f * std::pow(2.0f, 10.0f));
    CHECK(nearly(f.exposure.x, exposure, 1e-8f));
    CHECK(nearly(f.sunRadiance.w, 50000.0f, 1.0f));
    CHECK(nearly(f.skyParams.y, 8000.0f * exposure, 1e-4f));
    CHECK(nearly(f.cameraPos.w, 2.5f));
    CHECK(f.jitter.x == 0.0f && f.jitter.y == 0.0f);

    LogCapture log;
    m::AABB room;
    room.add(m::vec3(-3, 0, -3));
    room.add(m::vec3(3, 3, 3));
    r.setSceneBounds(room);
    std::vector<render::LightProbeDesc> probes(17);
    for (size_t i = 0; i < probes.size(); ++i) probes[i].position = m::vec3(float(i) * 0.2f - 1.6f, 1.0f, 0.0f);
    r.setLightProbes(probes);
    CHECK(frameDraws(*app) > 0);
    CHECK(log.has("light probes: 17 requested, keeping the first 16"));
}

// ---- shaders that do not build ----------------------------------------------------------------

// Shaders that fail to compile: the renderer starts and the scene runs, drawing nothing; once
// they compile, a reload (F5) builds every program and the frames draw again. A reload whose
// programs fail to link keeps the programs that worked. Reloads neither leak nor lose programs.
TEST(render_shader_failures_and_reloads) {
    DriverRestore restore;
    LogCapture log;
    nullgl::driver().compileFails = true;
    headless::App* app = headless::shared({"--scene", "testbed", "--view", "2"});
    REQUIRE(app);
    app->frame();
    CHECK_EQ(frameDraws(*app), uint64_t(0));
    CHECK(app->frame());

    nullgl::driver().compileFails = false;
    shaders::reloadAll();
    const uint64_t draws = frameDraws(*app);
    CHECK(draws > 0);
    const int64_t programs = nullgl::stats().livePrograms;

    nullgl::driver().linkFails = true;
    shaders::reloadAll();
    CHECK_EQ(frameDraws(*app), draws);
    nullgl::driver().linkFails = false;
    shaders::reloadAll();
    CHECK_EQ(frameDraws(*app), draws);
    CHECK_EQ(nullgl::stats().livePrograms, programs);

    const std::string& t = log.text();
    CHECK(t.find("shader compile failed: shaders/") != std::string::npos);
    CHECK(t.find("program link failed (") != std::string::npos);
    CHECK(t.find(" ok, 0 failed") != std::string::npos);    // the reload after the fix
    CHECK(t.find("shader reload: 0 ok, ") != std::string::npos);   // the reload that failed to link
}

// The procedural textures of the material library: baked at their full size, or, when the bake
// shaders do not build, neutral 1 x 1 stand-ins (three layers for the tapestry) and a warning.
TEST(render_material_bakes_fall_back_when_their_shaders_fail) {
    DriverRestore restore;
    headless::stopShared();   // no renderer: the shader cache is empty
    nullgl::load();
    materials::BakedTextures ok = materials::bakeAll();
    CHECK(ok.ok);
    CHECK_EQ(ok.marbleSlab.width, 2048);
    CHECK_EQ(ok.tapestry.depth, 3);
    for (gpu::Texture* t : {&ok.marbleSlab, &ok.polish, &ok.tapestry}) t->destroy();
    shaders::shutdown();

    LogCapture log;
    nullgl::driver().linkFails = true;
    materials::BakedTextures failed = materials::bakeAll();
    CHECK(!failed.ok);
    CHECK_EQ(failed.marbleSlab.width, 1);
    CHECK_EQ(failed.polish.width, 1);
    CHECK_EQ(failed.tapestry.width, 1);
    CHECK_EQ(failed.tapestry.depth, 3);
    CHECK(log.has("materials: bake shader shaders/materials/bake/marble_slab.comp failed"));
    CHECK(log.has("materials: bake shader shaders/materials/bake/tapestry.comp failed"));
    for (gpu::Texture* t : {&failed.marbleSlab, &failed.polish, &failed.tapestry}) t->destroy();
    shaders::shutdown();
}

// Framebuffers the driver calls incomplete are reported by name when the targets are made, and
// the scene goes on.
TEST(render_incomplete_framebuffers_are_reported) {
    DriverRestore restore;
    headless::App* app = headless::shared(kTestbed);
    REQUIRE(app);
    LogCapture log;
    nullgl::driver().framebufferIncomplete = true;
    fakeplat::setSize(640, 480);
    CHECK(app->frame());
    nullgl::driver().framebufferIncomplete = false;
    fakeplat::setSize(1280, 720);
    CHECK(app->frame());
    CHECK(log.has("framebuffer 'main' incomplete: 0x"));
    CHECK(log.has("framebuffer 'prepass' incomplete: 0x"));
}

// ---- the driver's capabilities ----------------------------------------------------------------

// Features left out of the extension list are probed: a driver that answers has them, one that
// does not is without (and says so); the core extensions it does not list are named.
TEST(render_gl_capabilities_probed_when_not_listed) {
    DriverRestore restore;
    dropExtension("GL_ARB_tessellation_shader");
    dropExtension("GL_ARB_depth_clamp");
    dropExtension("GL_ARB_clip_control");
    LogCapture log;
    const gl46::Caps& caps = probeCaps();
    CHECK(caps.tessellation);   // GL_MAX_TESS_GEN_LEVEL answers 64
    CHECK(caps.depthClamp);     // glEnable(GL_DEPTH_CLAMP) takes
    nullgl::driver().tessellation = false;
    nullgl::driver().depthClamp = false;
    CHECK(!probeCaps().tessellation);
    CHECK(!gl46::caps().depthClamp);
    CHECK(log.has("GL: no tessellation on this driver"));
    CHECK(log.has("GL: no depth clamping on this driver"));
    CHECK(log.has("GL: extensions not listed by the driver: GL_ARB_clip_control"));
}

// Zink over KosmicKrisp lists tessellation but draws nothing with it: turned off, unless
// SCACELITH_GL_FORCE names it; SCACELITH_GL_DISABLE turns features off on any driver.
TEST(render_gl_capabilities_quirks_and_environment) {
    DriverRestore restore;
    LogCapture log;
    nullgl::driver().renderer = "zink Vulkan 1.4(Apple M3 Pro (MESA_KOSMICKRISP))";
    CHECK(!probeCaps().tessellation);
    CHECK(gl46::caps().depthClamp);
    {
        EnvVar force("SCACELITH_GL_FORCE", "bloom,tessellation");
        CHECK(probeCaps().tessellation);
    }
    {
        EnvVar force("SCACELITH_GL_FORCE", "tessellation2");
        CHECK(!probeCaps().tessellation);   // a name in the list, not a part of one
    }
    nullgl::driver().renderer = "Scacelith null GL";
    {
        EnvVar disable("SCACELITH_GL_DISABLE", "tessellation,depth_clamp");
        const gl46::Caps& caps = probeCaps();
        CHECK(!caps.tessellation);
        CHECK(!caps.depthClamp);
    }
    CHECK(probeCaps().tessellation && gl46::caps().depthClamp);
    CHECK(log.has("GL: tessellation is listed but known to draw nothing on this driver"));
    CHECK(log.has("GL: tessellation is known to draw nothing on this driver; kept on (SCACELITH_GL_FORCE)"));
    CHECK(log.has("GL: no depth clamping on this driver"));
}

// A driver without tessellation and depth clamping: Options > Graphics greys tessellation out,
// the renderer keeps it off whatever the settings ask, and the sun's shadow cascades are fitted
// to the casters; the scene draws.
TEST(render_renderer_without_tessellation_and_depth_clamp) {
    DriverRestore restore;
    {
        EnvVar disable("SCACELITH_GL_DISABLE", "tessellation,depth_clamp");
        headless::App* app = headless::shared({"--scene", "testbed", "--view", "1"});
        REQUIRE(app);
        CHECK(!game::graphicsOptionAvailable(game::GfxTessellation));
        render::RenderSettings rs = app->renderer().settings();
        rs.tessellation = true;
        app->renderer().setSettings(rs);
        CHECK(!app->renderer().settings().tessellation);
        CHECK(frameDraws(*app) > 0);
        headless::stopShared();
    }
}

// The KHR_debug messages of a debug context: errors and warnings logged, notifications and the
// known chatter of NVIDIA's driver left out.
namespace {
GLDEBUGPROC g_debugCallback = nullptr;
void APIENTRY keepDebugCallback(GLDEBUGPROC callback, const void*) { g_debugCallback = callback; }
}  // namespace

TEST(render_gl_debug_messages_are_logged) {
    DriverRestore restore;
    nullgl::load();
    glDebugMessageCallback = keepDebugCallback;
    gl46::afterContextCreated(true);
    REQUIRE(g_debugCallback != nullptr);
    LogCapture log;
    g_debugCallback(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_ERROR, 1, GL_DEBUG_SEVERITY_HIGH, -1, "an invalid operation", nullptr);
    g_debugCallback(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_PERFORMANCE, 2, GL_DEBUG_SEVERITY_MEDIUM, -1, "a slow path", nullptr);
    g_debugCallback(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_OTHER, 3, GL_DEBUG_SEVERITY_NOTIFICATION, -1, "a notification", nullptr);
    g_debugCallback(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_OTHER, 131185, GL_DEBUG_SEVERITY_LOW, -1, "buffer chatter", nullptr);
    const std::string& t = log.text();
    CHECK(t.find(" E] GL: an invalid operation") != std::string::npos);
    CHECK(t.find(" W] GL: a slow path") != std::string::npos);
    CHECK(t.find("a notification") == std::string::npos);
    CHECK(t.find("buffer chatter") == std::string::npos);
    g_debugCallback = nullptr;
}

// ---- GL objects left behind -------------------------------------------------------------------

// A scene that ends (the material library's viewer) and its renderer delete every texture,
// buffer, framebuffer, vertex array and program they made: the live objects are those of before
// its start (the first run of a scene in the process makes the caches that stay: the material
// library's textures, the fullscreen triangle).
TEST(render_scene_and_renderer_end_without_leaving_gl_objects) {
    headless::stopShared();
    const std::vector<std::string> args = {"--scene", "materials", "--view", "marble"};
    REQUIRE(headless::shared(args));
    headless::shared(args)->frames(2);
    headless::stopShared();
    const LiveObjects before = LiveObjects::now();
    headless::App* app = headless::shared(args);
    REQUIRE(app);
    app->frames(3);
    fakeplat::setSize(1024, 640);
    app->frames(2);
    fakeplat::setSize(1280, 720);
    CHECK(LiveObjects::now().textures > before.textures);
    headless::stopShared();
    CHECK(LiveObjects::now() == before);
}
