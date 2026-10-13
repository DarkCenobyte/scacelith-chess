// The "scoresheet" viewer scene (src/game/scoresheet_viewer.cpp) on the headless game: both
// players' pads on the table in the hall, with its demos (moves written one after the other, a
// page turned over, the header written, everything already written), its camera presets and
// options (the pad looked at, the clock's side, the names, the hand styles, the language of the
// form, the sun). A page turned over adds a page to draw (Scoresheet::submit), so the turns of
// the demos show in the draw calls of a frame.
#include "test.h"
#include "headless.h"
#include "null_gl.h"
#include "viewer_helpers.h"
#include "game/layout.h"
#include "game/scoresheet_layout.h"
#include "i18n/i18n.h"
#include "render/post/postfx.h"
#include "scene/hall.h"
#include <cmath>
#include <string>
#include <vector>

using viewer_test::near;

namespace {

constexpr float kFrame = 1.0f / 60.0f;

const render::Camera& cam(headless::App& app) { return app.renderer().camera(); }
const render::Environment& env(headless::App& app) { return app.renderer().environment(); }
float focus(headless::App& app) { return app.renderer().post().settings.dofFocusDistance; }
uint64_t frameDraws(headless::App& app) {
    nullgl::resetStats();
    app.frame();
    return nullgl::stats().draws;
}
// An orbit camera at these angles (radians), this far from what it looks at, this wide (degrees).
bool orbitAt(headless::App& app, float yaw, float pitch, float distance, float fovDeg) {
    const m::vec3 forward = -m::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch));
    return near(cam(app).forward(), forward, 1e-4f) && near(focus(app), distance) && near(cam(app).fovY / m::DEG, fovDeg, 0.01f);
}

// The language of the UI back to English when a test ends.
struct EnglishAfter {
    ~EnglishAfter() { i18n::setLanguage("en"); }
};

}  // namespace

// The write demo from the last move of the first page: the pen writes it, the next move needs a
// new page, so both pads turn one over (more to draw); Space starts the demo again with the first
// page on top. The default camera looks down at White's pad.
TEST(viewer_scoresheet_write_demo_turns_the_page_and_restarts) {
    const int lastOfPage = 2 * game::sheet::MOVES_PER_PAGE - 1;   // Black's last move of page 1
    headless::App* app = headless::shared({"--scene", "scoresheet", "--moves", std::to_string(lastOfPage)});
    REQUIRE(app);
    app->frames(2);
    CHECK(orbitAt(*app, 0.15f, 1.05f, 0.42f, 40.0f));
    CHECK(near(env(*app).sunDirection, hall::recommendedSunDirection()));
    CHECK(near(env(*app).time, 2 * kFrame, 1e-5f));
    app->frames(2);
    const uint64_t onePage = frameDraws(*app);
    app->frames(12, 0.5f);   // 6 s: the move written, the page turned, the next move begun
    const uint64_t turned = frameDraws(*app);
    CHECK(turned > onePage);
    app->key(plat::KEY_SPACE);
    app->frames(2);
    CHECK_EQ(frameDraws(*app), onePage);
    CHECK(!viewer_test::frameWithKey(*app, plat::KEY_ESCAPE));
}

// The turn demo turns the top page over in 1.4 s from 0.6 s, and starts again every 4 s; with --s
// the page stays where it is told. The close-up of Black's pad (--sheet 1) looks from Black's side.
TEST(viewer_scoresheet_turn_demo_loops_and_holds) {
    headless::App* app = headless::shared({"--scene", "scoresheet", "--demo", "turn", "--view", "macro", "--names", "cjk"});
    REQUIRE(app);
    app->frame();
    CHECK(orbitAt(*app, 0.3f, 0.9f, 0.075f, 35.0f));
    const uint64_t flat = frameDraws(*app);   // 2 frames in: not turning yet
    app->frames(4, 0.5f);                      // 2.03 s: the turn has ended
    const uint64_t turned = frameDraws(*app);
    CHECK(turned > flat);
    app->frames(4, 0.5f);                      // past 4 s: again from the start
    CHECK_EQ(frameDraws(*app), flat);

    app = headless::shared({"--scene", "scoresheet", "--demo", "turn", "--s", "0.5", "--sheet", "1", "--view", "close", "--clock-left", "--names", "latin",
                            "--style0", "2", "--style1", "2", "--sun", "0,1,0"});
    REQUIRE(app);
    app->frame();
    CHECK(orbitAt(*app, m::PI + 0.2f, 1.0f, 0.24f, 40.0f));
    CHECK(near(env(*app).sunDirection, m::vec3(0, 1, 0)));
    const uint64_t held = frameDraws(*app);
    app->frames(4, 0.5f);
    CHECK_EQ(frameDraws(*app), held);   // half turned, all the time
}

// The header demo writes the names and ratings on a form printed in French and starts again; the
// static demo shows everything written, seen from the player's eyes; --shot with --time freezes the
// scene clock; the overrides of the orbit camera.
TEST(viewer_scoresheet_header_static_and_overrides) {
    EnglishAfter english;
    headless::App* app = headless::shared({"--scene", "scoresheet", "--demo", "header", "--view", "header", "--lang", "fr", "--names", "cyrillic"});
    REQUIRE(app);
    CHECK_EQ(i18n::language(), std::string("fr"));
    app->frame();
    CHECK(orbitAt(*app, 0.0f, 1.05f, 0.2f, 40.0f));
    app->frames(30, 0.5f);   // 15 s: written, shown, and begun again
    CHECK(near(env(*app).time, kFrame + 15.0f, 1e-3f));
    i18n::setLanguage("en");

    app = headless::shared({"--scene", "scoresheet", "--demo", "static", "--view", "player", "--names", "arabic", "--shot", "--time", "7"});
    REQUIRE(app);
    app->frames(3);
    CHECK(near(cam(*app).position, m::vec3(0, layout::EYE_HEIGHT, layout::PLAYER_PELVIS_Z)));
    CHECK(near(cam(*app).fovY / m::DEG, 50.0f, 0.01f));
    CHECK(cam(*app).forward().y < -0.5f);   // down at the pad
    CHECK(near(env(*app).time, 7.0f));

    app = headless::shared({"--scene", "scoresheet", "--demo", "static", "--view", "overview", "--yaw", "1", "--pitch", "0.3", "--dist", "2",
                            "--fov", "30", "--target", "0,1,0"});
    REQUIRE(app);
    app->frame();
    CHECK(orbitAt(*app, 1.0f, 0.3f, 2.0f, 30.0f));
    CHECK(near(cam(*app).position, viewer_test::orbitEye(m::vec3(0, 1, 0), 1.0f, 0.3f, 2.0f)));
}
