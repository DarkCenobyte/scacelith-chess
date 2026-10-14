// The "coach" viewer scene (src/game/coach_viewer.cpp) on the headless game: the table with both
// robots seated, the coach wearing its "COACH" marking, the pieces it designates and its marks on
// the board (--demo, --hl, --square, --arrow), the camera presets of --view and the orbit
// overrides, the human's seat (--human), the moves played before (--moves) and what the scene says
// of the options it cannot use. What the scene shows is read back from the renderer's last frame
// (camera, sun, depth of field focus), from its draw calls and from the log. Six starts of the
// scene: each builds its world again (the meshes are kept for the run by the harness).
#include "test.h"
#include "headless.h"
#include "null_gl.h"
#include "viewer_helpers.h"
#include "game/layout.h"
#include "render/post/postfx.h"
#include <string>
#include <vector>

using viewer_test::LogCapture;
using viewer_test::nearly;

namespace {

const render::Camera& cam(headless::App& app) { return app.renderer().camera(); }
float focus(headless::App& app) { return app.renderer().post().settings.dofFocusDistance; }
uint64_t frameDraws(headless::App& app) {
    nullgl::resetStats();
    app.frame();
    return nullgl::stats().draws;
}

// The demonstration through the eyes of the human seated at Black's side (--human 1) at the
// coach's chest, 60 degrees wide, without the coach's marking; with marks and highlights of the
// command line (some of them unusable: an empty square, names that are no squares, an arrow
// without its end or going nowhere), after a game whose fifth move is illegal; the marks' age
// fixed, the sun overhead, the coordinates on the border. 'strength' is that of every highlight
// and mark.
std::vector<std::string> demoArgs(const char* strength) {
    return {"--scene", "coach", "--demo", "--human", "1", "--view", "chest", "--no-marking", "--fov", "60",
            "--moves", "e4 e5 Nf3 Nc6 Qz9 d4", "--hl", "e4,a5,zz", "--square", "h1,x9",
            "--arrow", "g1-g3-f3,b8-d7,e2,a1-q9,c1-c1", "--age", "2", "--sun", "0,1,0", "--coords", "--strength", strength};
}

}  // namespace

// --demo with marks and highlights of its own: the moves stop at the first illegal one, a
// highlight that names no piece (or no square) is reported and left out. The view is the human's,
// from Black's side (the eyes at -Z) on the coach's chest (at +Z), with the sun of --sun. With every
// strength at 0 the highlights and the marks are not drawn: fewer draws from the same camera.
TEST(coach_viewer_demo_marks_and_highlights) {
    LogCapture log;
    headless::App* app = headless::shared(demoArgs("1"));
    REQUIRE(app);
    CHECK(log.has("coach viewer: illegal move Qz9"));
    CHECK(log.has("coach viewer: no piece on a5"));
    CHECK(log.has("coach viewer: no piece on zz"));
    CHECK(!log.has("coach viewer: no piece on e4"));   // the pawn played there
    CHECK(!log.has("coach viewer: illegal move d4"));  // never tried after Qz9
    app->frames(2);
    const m::vec3 eye(0.0f, layout::EYE_HEIGHT, -0.55f), chest(0.0f, 0.97f, 0.5f);
    CHECK(viewer_test::sees(cam(*app), eye, chest, 60.0f));
    CHECK(nearly(focus(*app), m::length(eye - chest)));
    CHECK(nearly(app->renderer().environment().sunDirection, m::vec3(0, 1, 0)));
    const uint64_t marked = frameDraws(*app);
    CHECK(marked > 0u);
    // The human's eyes do not move with the mouse.
    viewer_test::drag(*app, plat::MOUSE_RIGHT, 640.0f, 360.0f, 600.0f, 360.0f);
    CHECK(viewer_test::sees(cam(*app), eye, chest, 60.0f));

    app = headless::shared(demoArgs("0"));
    REQUIRE(app);
    app->frames(2);
    CHECK(viewer_test::sees(cam(*app), eye, chest, 60.0f));
    CHECK(frameDraws(*app) < marked);
}

// The close-up of the marks from White's side (the marks preset: 0.45 m away, straight from the
// human's side, 40 degrees wide) with its target and its pitch given on the command line, over the
// starting position: the kings highlighted, a knight's arrow. The wheel zooms, the right button
// turns round the target; Escape quits.
TEST(coach_viewer_orbit_view) {
    LogCapture log;
    headless::App* app = headless::shared({"--scene", "coach", "--view", "marks", "--moves", "", "--pitch", "0.3",
                                           "--target", "0.1,0.8,-0.2", "--hl", "e1,e8", "--arrow", "b1-c3"});
    REQUIRE(app);
    CHECK(!log.has("coach viewer: no piece"));
    CHECK(!log.has("coach viewer: illegal move"));
    app->frames(2);
    const m::vec3 target(0.1f, 0.8f, -0.2f);
    CHECK(viewer_test::orbits(cam(*app), target, 0.0f, 0.3f, 0.45f, 40.0f));
    CHECK(nearly(focus(*app), 0.45f));
    CHECK(frameDraws(*app) > 0u);
    app->wheel(1.0f);
    CHECK(nearly(focus(*app), 0.45f * 0.9f));
    viewer_test::drag(*app, plat::MOUSE_RIGHT, 640.0f, 360.0f, 600.0f, 360.0f);
    CHECK(!viewer_test::orbits(cam(*app), target, 0.0f, 0.3f, 0.45f * 0.9f, 40.0f));
    CHECK(nearly(focus(*app), 0.45f * 0.9f));
    CHECK(!viewer_test::frameWithKey(*app, plat::KEY_ESCAPE));
}

// The other orbit presets, 40 degrees wide, focused on their targets: the side view of the coach
// (0.9 m away, turned 0.75 rad from the human's side, nearly level), the board from high above
// (from Black's side for the human at Black), and the human's near left corner with the
// coordinates, its angle and distance given on the command line (--yaw, --dist).
TEST(coach_viewer_camera_presets) {
    headless::App* app = headless::shared({"--scene", "coach", "--view", "side", "--moves", ""});
    REQUIRE(app);
    app->frames(2);
    const m::vec3 coachSide(0.0f, 0.96f, -0.48f);
    CHECK(viewer_test::orbits(cam(*app), coachSide, 0.75f, 0.12f, 0.9f, 40.0f));
    CHECK(nearly(focus(*app), 0.9f));

    app = headless::shared({"--scene", "coach", "--view", "board", "--human", "1", "--moves", ""});
    REQUIRE(app);
    app->frames(2);
    const m::vec3 centre(0.0f, layout::BOARD_TOP_Y, 0.0f);
    CHECK(viewer_test::orbits(cam(*app), centre, m::PI, 1.2f, 0.9f, 40.0f));
    CHECK(cam(*app).position.z < 0.0f);   // Black's side

    app = headless::shared({"--scene", "coach", "--view", "coords", "--coords", "--moves", "", "--yaw", "0.3", "--dist", "0.5"});
    REQUIRE(app);
    app->frames(2);
    const m::vec3 corner(-0.15f, layout::BOARD_TOP_Y, 0.17f);
    CHECK(viewer_test::orbits(cam(*app), corner, 0.3f, 0.62f, 0.5f, 40.0f));
    CHECK(nearly(focus(*app), 0.5f));
}
