// The validation scenes of the renderer on the headless game: the material library's shader balls
// (src/render/materials/materials_viewer.cpp), the box-built hall of the lighting checks
// (src/app/lightbox_scene.cpp), the outdoor BRDF spheres (src/app/testbed_scene.cpp) and the room
// of the post effects (src/app/posttest_scene.cpp). Their camera presets and keys, their sun and
// exposure options, and what they change in the renderer (settings, post settings, fade, render
// scale) and in the material library (restored when they end).
#include "test.h"
#include "headless.h"
#include "null_gl.h"
#include "viewer_helpers.h"
#include "game/layout.h"
#include "render/materials/material_library.h"
#include "render/post/postfx.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using viewer_test::LogCapture;
using viewer_test::near;
using viewer_test::orbits;
using viewer_test::sees;

namespace {

constexpr float kFrame = 1.0f / 60.0f;
const float kDeg = m::DEG;

const render::Camera& cam(headless::App& app) { return app.renderer().camera(); }
const render::Environment& env(headless::App& app) { return app.renderer().environment(); }
PostSettings& post(headless::App& app) { return app.renderer().post().settings; }
bool uploaded(const std::string& meshName) {
    const std::vector<std::string>& l = nullgl::stats().labels;
    return std::find(l.begin(), l.end(), meshName) != l.end();
}
// Whether every material of the library has the define (or none has it).
bool everyMaterialHas(const char* define, bool has) {
    for (int k = 0; k < int(MaterialId::Count); ++k) {
        const std::vector<std::string>& d = materials::get(MaterialId(k)).defines;
        if ((std::find(d.begin(), d.end(), define) != d.end()) != has) return false;
    }
    return true;
}
// A sun direction of the testbed and the lightbox: azimuth from -X towards +Z, elevation (degrees).
m::vec3 sunFromAzEl(float az, float el) {
    return m::normalize(m::vec3(-std::cos(el * kDeg) * std::cos(az * kDeg), std::sin(el * kDeg), std::cos(el * kDeg) * std::sin(az * kDeg)));
}

}  // namespace

// ---- materials ------------------------------------------------------------------------------

// --albedo puts the unlit albedo define on every material and --orbit turns the chosen close-up;
// the digit keys pick the first ten close-ups (0 is the tenth). The floor samples the scene's
// planar reflection while it runs. At the end the library is as before: no albedo define, no
// planar reflection on the floor.
TEST(viewer_materials_close_ups_keys_and_albedo_debug) {
    struct Preset {
        m::vec3 target;
        float yaw, pitch, distance, fov;   // degrees, metres, degrees
    };
    // materials_viewer.cpp's kViews: overview, marble, board, pieces, floor, wood, tabletop, carved,
    // fabric, tapestry.
    const Preset presets[10] = {{{0.0f, 0.55f, -0.3f}, 20, 18, 4.2f, 45},  {{0.0f, 0.79f, 0.02f}, 8, 38, 0.62f, 45},
                                {{-0.06f, 0.782f, 0.06f}, 25, 30, 0.34f, 40}, {{0.02f, 0.81f, 0.13f}, -12, 14, 0.24f, 35},
                                {{-0.2f, 0.0f, 1.2f}, 30, 28, 2.2f, 50},    {{0.25f, 0.70f, 0.25f}, 35, 30, 1.0f, 45},
                                {{-0.38f, 0.76f, 0.28f}, 20, 32, 0.45f, 45}, {{1.0f, 0.45f, 0.3f}, 20, 10, 0.75f, 45},
                                {{2.8f, 1.0f, -1.9f}, 0, 8, 3.2f, 45},      {{1.55f, 1.25f, -2.2f}, 0, 0, 1.0f, 45}};
    headless::App* app = headless::shared({"--scene", "materials", "--albedo", "--view", "glass", "--orbit", "30", "--sun", "90,45"});
    REQUIRE(app);
    CHECK(uploaded("floor") && uploaded("crystalDrop") && uploaded("velvetBall") && uploaded("floorSwatch"));
    CHECK(everyMaterialHas("MAT_DEBUG_ALBEDO", true));
    CHECK(materials::get(MaterialId::FloorMarble).planarReflector >= 0);
    CHECK_EQ(materials::get(MaterialId::FloorMarbleInlay).planarReflector, materials::get(MaterialId::FloorMarble).planarReflector);
    app->frames(2);
    CHECK(orbits(cam(*app), m::vec3(-2.9f, 1.2f, -1.9f), 45 * kDeg, 5 * kDeg, 2.3f, 45));   // glass, 30 degrees round
    CHECK(near(env(*app).sunDirection, m::vec3(std::sqrt(0.5f), std::sqrt(0.5f), 0)));
    CHECK(near(env(*app).time, 2 * kFrame, 1e-5f));
    for (int k = 1; k <= 10; ++k) {
        app->key(plat::KEY_0 + k % 10);
        const Preset& p = presets[k - 1];
        CHECK(orbits(cam(*app), p.target, p.yaw * kDeg, p.pitch * kDeg, p.distance, p.fov));
    }
    app->wheel(1.0f);
    const Preset& last = presets[9];
    CHECK(orbits(cam(*app), last.target, last.yaw * kDeg, last.pitch * kDeg, last.distance * 0.9f, last.fov));
    CHECK(!viewer_test::frameWithKey(*app, plat::KEY_ESCAPE));

    // An unknown close-up is reported and leaves the orbit camera's defaults.
    LogCapture log;
    app = headless::shared({"--scene", "materials", "--view", "nowhere", "--sun", "south"});
    REQUIRE(app);
    CHECK(log.has("materials viewer: unknown view 'nowhere'"));
    CHECK(everyMaterialHas("MAT_DEBUG_ALBEDO", false));   // the first scene's end took them away
    app->frame();
    CHECK(orbits(cam(*app), m::vec3(0, 0.8f, 0), 0.0f, 0.35f, 1.2f, 40.0f));
    CHECK(near(env(*app).sunDirection, render::Environment().sunDirection));   // unreadable: the default
    headless::stopShared();
    CHECK_EQ(materials::get(MaterialId::FloorMarble).planarReflector, -1);
    CHECK_EQ(materials::get(MaterialId::FloorMarbleInlay).planarReflector, -1);
}

// ---- lightbox -------------------------------------------------------------------------------

// The five views (overview towards the windows, the player's view of the board, the ceiling, the
// window wall, the floor's reflections), the default sun and exposure, and those of the command line.
TEST(viewer_lightbox_views_sun_and_exposure) {
    struct View {
        m::vec3 eye, target;
        float fov;
    };
    const View views[5] = {{{6.2f, 2.1f, 9.0f}, {-4.0f, 2.6f, -2.0f}, 62},
                           {{0.42f, 1.08f, 0.50f}, {-0.02f, layout::BOARD_TOP_Y, -0.06f}, 42},
                           {{4.5f, 1.6f, 3.0f}, {-1.0f, 8.0f, -2.5f}, 70},
                           {{-5.2f, 1.7f, 10.0f}, {-5.5f, 2.2f, -4.0f}, 60},
                           {{5.5f, 0.45f, 2.5f}, {-6.5f, 1.2f, -1.0f}, 60}};
    for (int v = 0; v < 5; ++v) {
        const std::string view = std::to_string(v);
        headless::App* app = v == 4 ? headless::shared({"--scene", "lightbox", "--view", view, "--sun", "90,30", "--ev", "9"})
                                    : headless::shared({"--scene", "lightbox", "--view", view});
        REQUIRE(app);
        if (v == 0) CHECK(uploaded("lightbox.walls") && uploaded("lightbox.glass") && uploaded("lightbox.king"));
        app->frames(2);
        CHECK(sees(cam(*app), views[v].eye, views[v].target, views[v].fov));
        if (v < 4) {
            CHECK(near(env(*app).sunDirection, render::Environment().sunDirection));
            CHECK(near(env(*app).exposureEV100, render::Environment().exposureEV100));
        } else {
            CHECK(near(env(*app).sunDirection, m::vec3(0, 0.5f, std::sqrt(0.75f))));
            CHECK(near(env(*app).exposureEV100, 9.0f));
        }
        CHECK(near(env(*app).time, 2 * kFrame, 1e-5f));
    }
    headless::App* app = headless::shared({"--scene", "lightbox", "--view", "4", "--sun", "90,30", "--ev", "9"});
    CHECK(!viewer_test::frameWithKey(*app, plat::KEY_ESCAPE));
}

// ---- testbed --------------------------------------------------------------------------------

// The three orbit presets, the sunny default (sun at azimuth 25, elevation 35, EV 14.7), dusk (the
// sun 4 degrees below the horizon, EV 8.5) and the overrides of --sun and --ev.
TEST(viewer_testbed_views_dusk_and_sun) {
    headless::App* app = headless::shared({"--scene", "testbed"});
    REQUIRE(app);
    app->frame();
    CHECK(orbits(cam(*app), m::vec3(0, 0.1f, 0), 0.6f, 0.3f, 1.9f, 40));
    CHECK(near(env(*app).sunDirection, sunFromAzEl(25, 35)));
    CHECK(near(env(*app).exposureEV100, 14.7f));
    app->wheel(2.0f);
    CHECK(orbits(cam(*app), m::vec3(0, 0.1f, 0), 0.6f, 0.3f, 1.9f * 0.81f, 40));

    app = headless::shared({"--scene", "testbed", "--view", "1", "--dusk"});
    REQUIRE(app);
    app->frame();
    CHECK(orbits(cam(*app), m::vec3(-0.2f, 0.12f, 0), 1.2f, 0.08f, 2.2f, 40));
    CHECK(near(env(*app).sunDirection, sunFromAzEl(25, -4)));
    CHECK(env(*app).sunDirection.y < 0.0f);
    CHECK(near(env(*app).exposureEV100, 8.5f));

    app = headless::shared({"--scene", "testbed", "--view", "2", "--dusk", "--sun", "10,20", "--ev", "9.5", "--shot", "--time", "4"});
    REQUIRE(app);
    app->frames(2);
    CHECK(orbits(cam(*app), m::vec3(0, 0.04f, 0.35f), 0.3f, 0.35f, 0.55f, 40));
    CHECK(near(env(*app).sunDirection, sunFromAzEl(10, 20)));
    CHECK(near(env(*app).exposureEV100, 9.5f));
    CHECK(near(env(*app).time, 4.0f));
    CHECK(!viewer_test::frameWithKey(*app, plat::KEY_ESCAPE));
}

// ---- posttest -------------------------------------------------------------------------------

// The low preset with every effect switched off at half the resolution, a fade, a frozen
// animation and the exposure, sky, focus and aperture of the command line.
TEST(viewer_posttest_low_quality_without_effects) {
    headless::App* app = headless::shared({"--scene", "posttest", "--quality", "0", "--no-ssao", "--no-ssr", "--no-vol", "--no-taa", "--no-mb",
                                           "--no-dof", "--no-bloom", "--scale", "0.5", "--fade", "0.25", "--static", "--ev", "9", "--sky",
                                           "1000", "--focus", "3", "--fstop", "4"});
    REQUIRE(app);
    CHECK(uploaded("posttest.shell") && uploaded("posttest.pawn"));
    app->frames(3);
    const render::RenderSettings& s = app->renderer().settings();
    CHECK(s.quality == render::Quality::Low);
    CHECK(!s.ssao && !s.ssr && !s.volumetrics && !s.taa && !s.motionBlur && !s.dof && !s.bloom);
    CHECK_EQ(s.shadowMapSize, 2048);
    CHECK(!s.tessellation);
    CHECK(near(s.renderScale, 0.5f));
    CHECK_EQ(app->renderer().targets().w, 640);
    CHECK_EQ(app->renderer().targets().h, 360);
    CHECK(near(app->renderer().fade, 0.25f));
    CHECK(near(env(*app).time, 0.0f));   // --static
    CHECK(near(env(*app).exposureEV100, 9.0f));
    CHECK(near(env(*app).skyIlluminance, 1000.0f));
    CHECK(near(post(*app).dofFocusDistance, 3.0f));
    CHECK(near(post(*app).dofFStop, 4.0f));
    CHECK(!post(*app).autoExposure);
}

// The ultra preset with auto exposure and a debug view, the animation at twice the speed from the
// --time given; the keys 1-3 pick the camera presets (the focus on each one's subject), F1-F6 the
// debug views and 0 the final image.
TEST(viewer_posttest_presets_and_debug_view_keys) {
    headless::App* app = headless::shared({"--scene", "posttest", "--quality", "3", "--autoexposure", "--post-debug", "7", "--speed", "2", "--time",
                                           "1", "--cam", "2"});
    REQUIRE(app);
    app->frames(3);
    const render::RenderSettings& s = app->renderer().settings();
    CHECK(s.quality == render::Quality::Ultra);
    CHECK_EQ(s.probeBounces, 3);
    CHECK_EQ(s.shadowFilter, 3);
    CHECK(post(*app).autoExposure);
    CHECK_EQ(post(*app).debugView, 7);
    CHECK(near(env(*app).time, 1.0f + 3 * 2 * kFrame, 1e-4f));
    struct Preset {
        m::vec3 eye, target, subject;
    };
    const Preset presets[3] = {{{4.3f, 1.65f, 3.3f}, {-1.6f, 1.05f, -1.0f}, {0.0f, 0.6f, -0.4f}},
                               {{2.6f, 1.25f, -2.2f}, {-5.0f, 2.3f, 0.2f}, {-1.5f, 1.4f, -0.8f}},
                               {{3.3f, 1.05f, 1.9f}, {1.6f, 0.62f, 0.6f}, {1.6f, 0.62f, 0.6f}}};
    auto checkPreset = [&](int k) {
        const Preset& p = presets[k];
        CHECK(sees(cam(*app), p.eye, p.target, 50));
        CHECK(near(post(*app).dofFocusDistance, m::dot(p.subject - p.eye, m::normalize(p.target - p.eye))));
    };
    checkPreset(2);
    CHECK(near(post(*app).dofFocusDistance, m::length(presets[2].target - presets[2].eye)));
    for (int k : {0, 1, 2}) {
        app->key(plat::KEY_0 + k + 1);
        checkPreset(k);
    }
    for (int k = 0; k < 6; ++k) {
        app->key(plat::KEY_F1 + k);
        CHECK_EQ(post(*app).debugView, k + 1);
    }
    app->key(plat::KEY_0);
    CHECK_EQ(post(*app).debugView, 0);
    CHECK(!viewer_test::frameWithKey(*app, plat::KEY_ESCAPE));
}
