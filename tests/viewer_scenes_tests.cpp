// The viewer scenes of the porcelain robot (src/character/robot_viewer.cpp), the royal hall
// (src/scene/hall_viewer.cpp) and the chess set on its table (src/scene/tabletop_viewer.cpp) on the
// headless game: their poses and camera presets, the overrides of the command line, the orbit
// camera (right-drag, middle-drag, wheel), their debug options and keys. What a viewer shows is
// read back from the renderer's last frame (camera, environment, depth of field focus), from the
// meshes it uploaded (nullgl labels) and from its draw calls.
#include "test.h"
#include "headless.h"
#include "null_gl.h"
#include "viewer_helpers.h"
#include "game/layout.h"
#include "render/post/postfx.h"
#include "scene/hall.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using viewer_test::LogCapture;
using viewer_test::nearly;
using viewer_test::orbits;
using viewer_test::sees;

namespace {

constexpr float kFrame = 1.0f / 60.0f;

const render::Camera& cam(headless::App& app) { return app.renderer().camera(); }
const render::Environment& env(headless::App& app) { return app.renderer().environment(); }
float focus(headless::App& app) { return app.renderer().post().settings.dofFocusDistance; }
bool uploaded(const std::string& meshName) {
    const std::vector<std::string>& l = nullgl::stats().labels;
    return std::find(l.begin(), l.end(), meshName) != l.end();
}
uint64_t frameDraws(headless::App& app) {
    nullgl::resetStats();
    app.frame();
    return nullgl::stats().draws;
}
// The point an orbit camera turns round: its distance (the focus) ahead of the eye.
m::vec3 orbitTarget(headless::App& app) { return cam(app).position + cam(app).forward() * focus(app); }
// The forward axis of an orbit camera at these angles.
m::vec3 orbitForward(float yaw, float pitch) { return -m::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)); }

std::vector<std::string> robot(std::initializer_list<const char*> options) {
    std::vector<std::string> args = {"--scene", "robot"};
    args.insert(args.end(), options.begin(), options.end());
    return args;
}

}  // namespace

// ---- robot ----------------------------------------------------------------------------------

// The face close-up looks at the eyes from 42 cm, 28 degrees wide; in the reaching pose the robot
// leans towards the table and looks down at its hand, so its eyes come forward and lower. The
// wheel zooms (0.9 of the distance a notch, the focus with it), a right-drag turns round the face,
// B blinks and Escape quits. The first-person flags hide the head and eyes: fewer draws.
TEST(viewer_robot_face_view_follows_the_pose) {
    headless::App* app = headless::shared(robot({"--view", "face"}));
    REQUIRE(app);
    CHECK(uploaded("floor") && uploaded("seat") && uploaded("table"));
    CHECK(uploaded("eye_cornea") && uploaded("lid_upper"));
    app->frames(2);
    CHECK(nearly(focus(*app), 0.42f));
    CHECK(nearly(cam(*app).fovY / m::DEG, 28.0f, 0.01f));
    CHECK(nearly(cam(*app).forward(), orbitForward(0.30f, 0.02f), 1e-4f));
    const m::vec3 restEyes = orbitTarget(*app) + m::vec3(0, 0.03f, 0);
    CHECK(restEyes.y > 1.0f && restEyes.y < layout::EYE_HEIGHT + 0.1f);

    LogCapture log;
    const std::vector<std::string> reach =
        robot({"--pose", "reach", "--view", "face", "--blink", "1", "--gaze", "10,-5", "--pupil", "0.6", "--sun", "0,2,0", "--coach"});
    app = headless::shared(reach);
    REQUIRE(app);
    CHECK(log.has("robot: chest marking \"COACH\""));
    CHECK(app->frame());
    const m::vec3 reachEyes = orbitTarget(*app) + m::vec3(0, 0.03f, 0);
    CHECK(reachEyes.z > restEyes.z + 0.02f);   // leaning towards the table (+Z)
    CHECK(reachEyes.y < restEyes.y);
    CHECK(nearly(env(*app).sunDirection, m::vec3(0, 1, 0)));
    CHECK(nearly(env(*app).time, kFrame, 1e-5f));   // not a shot: the clock runs
    const uint64_t withHead = frameDraws(*app);

    // The wheel: 0.9 of the distance, towards the same point.
    const m::vec3 target = orbitTarget(*app);
    app->wheel(1.0f);
    CHECK(nearly(focus(*app), 0.42f * 0.9f));
    CHECK(nearly(cam(*app).position, target - cam(*app).forward() * (0.42f * 0.9f)));
    // A right-drag of 40 px to the left turns the camera 0.2 rad the other way.
    viewer_test::drag(*app, plat::MOUSE_RIGHT, 640.0f, 360.0f, 600.0f, 360.0f);
    CHECK(nearly(cam(*app).forward(), orbitForward(0.50f, 0.02f), 1e-4f));
    CHECK(nearly(orbitTarget(*app), target));
    // B closes and opens the lids: the camera stays.
    const m::vec3 before = cam(*app).position;
    app->key('B');
    app->key('B');
    CHECK(nearly(cam(*app).position, before));
    CHECK(!viewer_test::frameWithKey(*app, plat::KEY_ESCAPE));

    std::vector<std::string> fp = reach;
    fp.push_back("--fp");
    app = headless::shared(fp);
    REQUIRE(app);
    app->frame();
    CHECK(frameDraws(*app) < withHead);
}

// The hand view frames the right hand from the eyes, 30 cm away (the first-person view of a
// move), 50 degrees wide. --only keeps the parts whose name contains its text (the two eyes'
// sclera, iris and cornea), --wire draws their triangles' edges, --coach names its chest marking.
TEST(viewer_robot_hand_view_looks_from_the_eyes_at_the_hand) {
    headless::App* app = headless::shared(robot({"--pose", "reach", "--view", "face"}));
    REQUIRE(app);
    app->frame();
    const m::vec3 eyes = orbitTarget(*app) + m::vec3(0, 0.03f, 0);

    LogCapture log;
    app = headless::shared(robot({"--pose", "hand", "--wire", "--only", "eye_", "--coach", "MENTOR"}));
    REQUIRE(app);
    CHECK(log.has("robot viewer: 6 draws"));
    CHECK(log.has("robot: chest marking \"MENTOR\""));
    app->frame();
    CHECK(nearly(focus(*app), 0.30f));
    CHECK(nearly(cam(*app).fovY / m::DEG, 50.0f, 0.01f));
    const m::vec3 hand = orbitTarget(*app);
    CHECK(nearly(m::normalize(eyes - hand), -cam(*app).forward(), 1e-3f));
    CHECK(hand.y > layout::TABLE_TOP_Y && hand.y < layout::TABLE_TOP_Y + 0.2f);   // pinching a piece on the table
    CHECK(hand.z > 0.2f);
    CHECK(frameDraws(*app) > 0);
}

// The fixed presets (full, side, back, top), the table slab of --table (more draws from the same
// camera), a chest marking without ink, and every orbit override of the command line; --shot with
// --time freezes the clock.
TEST(viewer_robot_camera_presets_and_overrides) {
    struct Preset {
        const char* view;
        m::vec3 target;
        float yaw, pitch, distance;
    };
    const Preset presets[] = {{"full", {0, 0.88f, 0.12f}, 0.55f, 0.12f, 2.5f},
                              {"side", {0, 0.85f, 0.15f}, -1.5708f, 0.08f, 2.6f},
                              {"back", {0, 0.9f, 0.0f}, 3.4f, 0.2f, 2.4f},
                              {"top", {0, 0.9f, 0.1f}, 0.3f, 1.1f, 2.4f}};
    uint64_t bare = 0;
    for (const Preset& p : presets) {
        headless::App* app = headless::shared(robot({"--view", p.view}));
        REQUIRE(app);
        app->frame();
        CHECK(orbits(cam(*app), p.target, p.yaw, p.pitch, p.distance, 35.0f));
        CHECK(nearly(focus(*app), p.distance));
        if (std::string(p.view) == "full") bare = frameDraws(*app);
    }
    LogCapture log;
    headless::App* app = headless::shared(robot({"--table", "--coach", " "}));
    REQUIRE(app);
    CHECK(log.has("robot: chest marking \" \" not rendered"));   // no ink: a plain chest
    app->frame();
    CHECK(frameDraws(*app) > bare);

    app = headless::shared(robot({"--view", "side", "--yaw", "0.1", "--pitch", "0.2", "--dist", "0.5", "--fov", "60", "--target", "0,1,0", "--shot",
                                  "--time", "3"}));
    REQUIRE(app);
    app->frames(3);
    CHECK(orbits(cam(*app), m::vec3(0, 1, 0), 0.1f, 0.2f, 0.5f, 60.0f));
    CHECK(nearly(focus(*app), 0.5f));
    CHECK(nearly(env(*app).time, 3.0f));
}

// ---- hall -----------------------------------------------------------------------------------

// The player's view: from White's eyes over the board towards the far wall, 70 degrees wide, under
// the sun and the exposure of the command line. --noglass hides the window glass and --skip the
// parts whose name contains its text (here the whole hall: only the table and the chairs remain),
// each with fewer draws; an unreadable --sun keeps the hall's recommended sun.
TEST(viewer_hall_player_view_and_debug_options) {
    LogCapture log;
    headless::App* app = headless::shared({"--scene", "hall", "--view", "player", "--sun", "1,1,0", "--ev", "10"});
    REQUIRE(app);
    CHECK(log.has("hall viewer: hall "));
    CHECK(uploaded("hall_paintings") && uploaded("hall_windowglass"));
    app->frames(2);
    const m::vec3 eye(0, layout::EYE_HEIGHT, layout::PLAYER_PELVIS_Z);
    CHECK(sees(cam(*app), eye, m::vec3(-0.1f, 0.84f, -0.45f), 70.0f));
    CHECK(nearly(env(*app).sunDirection, m::normalize(m::vec3(1, 1, 0))));
    CHECK(nearly(env(*app).exposureEV100, 10.0f));
    CHECK(nearly(env(*app).time, 2 * kFrame, 1e-5f));
    const uint64_t all = frameDraws(*app);

    app = headless::shared({"--scene", "hall", "--view", "player", "--sun", "1,1,0", "--ev", "10", "--noglass"});
    REQUIRE(app);
    app->frame();
    const uint64_t noGlass = frameDraws(*app);
    CHECK(noGlass < all);

    app = headless::shared({"--scene", "hall", "--view", "player", "--sun", "north", "--skip", "hall_"});
    REQUIRE(app);
    app->frame();
    CHECK(frameDraws(*app) < noGlass);
    CHECK(nearly(env(*app).sunDirection, hall::recommendedSunDirection()));
    CHECK(nearly(env(*app).exposureEV100, 11.5f));
    CHECK(!viewer_test::frameWithKey(*app, plat::KEY_ESCAPE));
}

// The other presets, the orbit camera of the default view, and a camera of the command line
// (--cam, --target, --fov) that becomes an orbit round its target at the first wheel notch or drag.
TEST(viewer_hall_views_and_custom_camera_turn_into_an_orbit) {
    struct View {
        const char* name;
        m::vec3 eye, target;
        float fov;
    };
    const View views[] = {{"wide", {5.6f, 3.1f, 10.2f}, {-1.8f, 2.4f, -3.5f}, 64.0f},
                          {"windows", {5.0f, 1.7f, 5.0f}, {-7.0f, 3.6f, -1.5f}, 62.0f},
                          {"ceiling", {3.5f, 1.6f, 6.5f}, {0.0f, 6.5f, -1.0f}, 70.0f},
                          {"table", {1.9f, 1.25f, 1.6f}, {0.0f, 0.5f, 0.0f}, 45.0f}};
    for (const View& v : views) {
        headless::App* app = headless::shared({"--scene", "hall", "--view", v.name});
        REQUIRE(app);
        app->frame();
        CHECK(sees(cam(*app), v.eye, v.target, v.fov));
    }
    headless::App* app = headless::shared({"--scene", "hall", "--view", "nowhere"});
    REQUIRE(app);
    app->frame();
    CHECK(orbits(cam(*app), m::vec3(0, 1.5f, 0), 0.7f, 0.25f, 9.0f, 55.0f));

    app = headless::shared({"--scene", "hall", "--cam", "1,2,3", "--target", "0,1,0", "--fov", "30"});
    REQUIRE(app);
    app->frames(2);
    const m::vec3 eye(1, 2, 3), target(0, 1, 0);
    CHECK(sees(cam(*app), eye, target, 30.0f));
    app->wheel(1.0f);
    CHECK(sees(cam(*app), target + (eye - target) * 0.9f, target, 30.0f));
    // Now an orbit: a 100 px right-drag turns it 0.5 rad.
    const m::vec3 d = eye - target;
    const float yaw = std::atan2(d.x, d.z), pitch = std::asin(d.y / m::length(d));
    viewer_test::drag(*app, plat::MOUSE_RIGHT, 500.0f, 300.0f, 400.0f, 300.0f);
    CHECK(orbits(cam(*app), target, yaw + 0.5f, pitch, m::length(d) * 0.9f, 30.0f));
}

// ---- tabletop -------------------------------------------------------------------------------

// The overview of the set: the six piece types (body and felt), the board, the clock and the
// table uploaded; an orbit round the board that zooms with the wheel; Space presses the clock's
// lever and the scene goes on; Escape quits.
TEST(viewer_tabletop_overview_orbits_the_board) {
    LogCapture log;
    headless::App* app = headless::shared({"--scene", "tabletop"});
    REQUIRE(app);
    CHECK(log.has("tabletop: pieces "));
    CHECK(uploaded("piece") && uploaded("felt") && uploaded("table"));
    app->frame();
    const m::vec3 board(0, layout::BOARD_TOP_Y + 0.02f, 0);
    CHECK(orbits(cam(*app), board, 0.55f, 0.62f, 0.95f, 40.0f));
    app->wheel(-1.0f);
    CHECK(orbits(cam(*app), board, 0.55f, 0.62f, 0.95f / 0.9f, 40.0f));
    // A middle-drag 40 px down pans the camera up (the target with it).
    const render::Camera before = cam(*app);
    viewer_test::drag(*app, plat::MOUSE_MIDDLE, 600.0f, 300.0f, 600.0f, 340.0f);
    CHECK(nearly(cam(*app).position, before.position + before.up() * (40.0f * 0.0015f * 0.95f / 0.9f)));
    CHECK(nearly(cam(*app).forward(), before.forward(), 1e-4f));
    CHECK(viewer_test::frameWithKey(*app, plat::KEY_SPACE));
    CHECK(app->frame());
    CHECK(!viewer_test::frameWithKey(*app, plat::KEY_ESCAPE));
}

// The presets: the player's eyes over the board, White's b1 knight, the clock, the twelve pieces
// in a row and a corner of the board; the orbit overrides of the command line with a clock state.
TEST(viewer_tabletop_presets_and_overrides) {
    headless::App* app = headless::shared({"--scene", "tabletop", "--view", "player"});
    REQUIRE(app);
    app->frame();
    CHECK(sees(cam(*app), m::vec3(0, layout::EYE_HEIGHT, layout::PLAYER_PELVIS_Z), m::vec3(0, layout::BOARD_TOP_Y, 0), 50.0f));

    struct Preset {
        const char* view;
        m::vec3 target;
        float yaw, pitch, distance, fov;
    };
    const m::vec3 clock(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y, layout::CLOCK_Z);
    const Preset presets[] = {
        {"knight", layout::squareCenter(1, 0) + m::vec3(0, 0.04f, 0), 2.6f, 0.45f, 0.2f, 40.0f},
        {"clock", clock + m::vec3(0, 0.035f, 0), -1.5708f - 0.35f, 0.32f, 0.42f, 35.0f},
        {"pieces", {0, layout::BOARD_TOP_Y + 0.04f, 0.1925f + 0.0275f}, 0.0f, 0.08f, 0.36f, 40.0f},
        {"board", {-0.19f, layout::BOARD_TOP_Y, 0.19f}, -0.6f, 0.5f, 0.25f, 40.0f},
    };
    for (const Preset& p : presets) {
        app = headless::shared({"--scene", "tabletop", "--view", p.view});
        REQUIRE(app);
        app->frame();
        CHECK(orbits(cam(*app), p.target, p.yaw, p.pitch, p.distance, p.fov));
        CHECK(frameDraws(*app) > 0);
    }
    app = headless::shared({"--scene", "tabletop", "--view", "clock", "--yaw", "1", "--pitch", "0.5", "--dist", "0.3", "--fov", "25", "--target",
                            "0.1,0.9,0.2", "--lever", "0", "--running", "1", "--ms0", "1000", "--ms1", "2000", "--flags", "1", "--shot", "--time",
                            "2"});
    REQUIRE(app);
    app->frames(2);
    CHECK(orbits(cam(*app), m::vec3(0.1f, 0.9f, 0.2f), 1.0f, 0.5f, 0.3f, 25.0f));
    CHECK(nearly(env(*app).time, 2.0f));
}
