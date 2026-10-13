// The "anim" viewer scene (src/anim/anim_viewer.cpp) on the headless game: two debug robots at the
// table playing its scripted timelines (the default opening, the left-handed player who writes his
// moves and turns the page, castling and promotion with the left hand, the coach's gestures and
// demonstration, the stance changes), its keys (pause, restart, slow motion, next view, quit), every
// camera view, the porcelain robot, the --shot freeze and the numeric self tests. The scene reports
// its timeline in the log; its clock and camera are those of the renderer's last frame.
#include "test.h"
#include "fake_platform.h"
#include "headless.h"
#include "null_gl.h"
#include "viewer_helpers.h"
#include "game/layout.h"
#include "render/post/postfx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using viewer_test::LogCapture;
using viewer_test::near;

namespace {

const std::vector<std::string> kDefault = {"--scene", "anim"};
constexpr float kFrame = 1.0f / 60.0f;
constexpr int kViewCount = 46;   // kViews in anim_viewer.cpp

float sceneTime(headless::App& app) { return app.renderer().environment().time; }
m::vec3 eye(headless::App& app) { return app.renderer().camera().position; }
float fovDeg(headless::App& app) { return app.renderer().camera().fovY / m::DEG; }
float focus(headless::App& app) { return app.renderer().post().settings.dofFocusDistance; }
bool uploaded(const std::string& meshName) {
    const std::vector<std::string>& l = nullgl::stats().labels;
    return std::find(l.begin(), l.end(), meshName) != l.end();
}

// One frame's draw calls (the renderer's passes over every item the scene submitted).
uint64_t frameDraws(headless::App& app) {
    nullgl::resetStats();
    app.frame();
    return nullgl::stats().draws;
}

// What the camera of a view shows: a fixed eye (and vertical field of view), or, for the
// close-ups that follow a hand, the offset from the point looked at to the eye (anim_viewer.cpp's
// camera()), whose length is the depth of field focus distance.
struct ViewCheck {
    int view;
    bool fixedEye;
    m::vec3 v;     // the eye, or the offset of the eye from the point looked at
    float fov;     // degrees
};
const float kPelvisZ = layout::PLAYER_PELVIS_Z;
const ViewCheck kViewChecks[] = {
    {0, true, {1.50f, 1.18f, 0.0f}, 44.0f},           // side
    {1, true, {-1.50f, 1.18f, 0.0f}, 44.0f},          // sidel
    {2, true, {0.35f, 1.35f, -1.75f}, 40.0f},         // front
    {3, true, {-0.35f, 1.35f, 1.75f}, 40.0f},         // back
    {4, true, {0.0f, 2.30f, 0.02f}, 40.0f},           // top
    {7, false, {0.30f, 0.04f, 0.02f}, 34.0f},         // hand: White's playing hand from its right-front
    {8, false, {0.30f, 0.04f, 0.02f}, 34.0f},         // handb
    {9, false, {0.30f, 0.04f, 0.02f}, 34.0f},         // handl
    {10, true, {0.42f, 1.08f, 0.02f}, 34.0f},         // shake
    {12, false, {0.34f, 0.07f, 0.08f}, 32.0f},        // pinch (from the front)
    {13, false, {0.34f, 0.07f, 0.06f}, 32.0f},        // pinchs (from the side)
    {14, false, {0.34f, 0.07f, 0.08f}, 32.0f},        // pinchb
    {15, false, {0.34f, 0.07f, 0.06f}, 32.0f},        // pinchbs
    {16, false, {0.21f, 0.15f, 0.06f}, 34.0f},        // pen
    {17, false, {0.21f, 0.10f, 0.07f}, 34.0f},        // pens
    {18, false, {0.21f, 0.15f, 0.06f}, 34.0f},        // penb
    {19, false, {0.21f, 0.10f, 0.07f}, 34.0f},        // penbs
    {20, false, {0.30f, 0.22f, 0.22f}, 40.0f},        // page
    {21, false, {0.30f, 0.22f, 0.22f}, 40.0f},        // pageb
    {22, false, {0.30f, 0.05f, 0.03f}, 34.0f},        // lhand
    {23, false, {0.0f, 0.30f, 0.10f}, 34.0f},         // pad
    {24, false, {0.0f, 0.30f, 0.10f}, 34.0f},         // padb
    {25, true, {1.05f, 1.05f, -0.35f}, 36.0f},        // clock
    {26, false, {0.04f, 0.13f, 0.30f}, 34.0f},        // coachhand
    {27, false, {0.30f, 0.08f, 0.05f}, 34.0f},        // coachhands
    {28, false, {0.26f, 0.06f, 0.06f}, 34.0f},        // coachhandt
    {29, false, {0.30f, 0.02f, 0.0f}, 30.0f},         // shakex
    {30, false, {0.30f, 0.02f, 0.0f}, 30.0f},         // shakexl
    {31, false, {0.0f, 0.30f, 0.001f}, 30.0f},        // shakeu
    {33, false, {0.10f, 0.08f, 0.30f}, 30.0f},        // shakew
    {34, false, {0.10f, 0.08f, 0.30f}, 30.0f},        // shakeb
    {35, false, {0.22f, 0.18f, 0.12f}, 30.0f},        // shakeq
    {36, true, {0.0f, 1.23f, -(kPelvisZ - 0.08f)}, 64.0f},   // opp: White from Black's eyes
    {37, true, {0.0f, 1.23f, kPelvisZ - 0.08f}, 64.0f},      // oppb
    {38, true, {-1.75f, 1.50f, -0.95f}, 46.0f},       // q3
    {39, true, {1.75f, 1.50f, 0.95f}, 46.0f},         // q3b
    {40, true, {-2.70f, 1.40f, -0.55f}, 50.0f},       // end
    {41, true, {-1.55f, 1.45f, 2.10f}, 46.0f},        // q3r
    {42, true, {0.0f, 4.20f, 0.30f}, 44.0f},          // topw
    {43, true, {-2.30f, 1.10f, 1.00f}, 46.0f},        // stand
    {44, true, {0.55f, 1.25f, 2.60f}, 46.0f},         // standb
    {45, true, {-0.55f, 0.95f, 0.05f}, 40.0f},        // hips
};

// Checks the camera of the last frame against the view's entry (when it has one).
void checkView(headless::App& app, int view) {
    for (const ViewCheck& c : kViewChecks) {
        if (c.view != view) continue;
        const bool ok = c.fixedEye ? near(eye(app), c.v) : near(focus(app), m::length(c.v), 1e-4f);
        CHECK(ok);
        CHECK(near(fovDeg(app), c.fov, 0.01f));
        if (!ok) std::fprintf(stderr, "  (the camera of view %d)\n", view);
    }
}

}  // namespace

// The default timeline: the handshake, then 1.e4 d5 2.exd5 with a clock press after each move,
// Black answering 0.7 s after White's press. The scene's clock runs with the frames.
TEST(viewer_anim_default_timeline_plays_the_opening) {
    LogCapture log;
    headless::App* app = headless::shared(kDefault);
    REQUIRE(app);
    app->key('R');      // from the start of the timeline, whatever ran before
    app->frames(640);   // to 10.7 s
    CHECK(log.inOrder({"anim viewer: handshake at t=", "handshake clasp (0)", "anim viewer: White e2-e4 (piece 4)",
                       "White pressed the clock", "anim viewer: Black d7-d5 (piece 19)", "Black pressed the clock",
                       "anim viewer: White e4xd5 (piece 4 takes 19)", "White pressed the clock"}));
    CHECK_EQ(log.count("handshake clasp"), 2);
    CHECK_EQ(log.count("pressed the clock"), 3);
    CHECK(near(sceneTime(*app), 642 * kFrame));
    // The default view looks at the table from its +X end.
    CHECK(near(eye(*app), m::vec3(1.50f, 1.18f, 0.0f)));
    CHECK(near(fovDeg(*app), 44.0f, 0.01f));
}

// Space pauses and resumes, S slows the timeline down to a fifth and back, R restarts it, and
// Escape asks main.cpp's loop to end.
TEST(viewer_anim_keys_pause_slow_down_restart_and_quit) {
    headless::App* app = headless::shared(kDefault);
    REQUIRE(app);
    app->frames(30);
    app->key('R');   // the timeline from 0, then the two frames of the key
    CHECK(near(sceneTime(*app), 2 * kFrame, 1e-4f));
    // Space acts in the first frame of the key, before the timeline moves.
    const float paused = sceneTime(*app);
    app->key(plat::KEY_SPACE);
    app->frames(30);
    CHECK(near(sceneTime(*app), paused, 1e-6f));
    app->key(plat::KEY_SPACE);
    CHECK(near(sceneTime(*app), paused + 2 * kFrame, 1e-4f));
    app->key('S');
    float t = sceneTime(*app);
    app->frames(10);
    CHECK(near(sceneTime(*app), t + 10 * 0.2f * kFrame, 1e-4f));
    app->key('S');
    t = sceneTime(*app);
    app->frames(10);
    CHECK(near(sceneTime(*app), t + 10 * kFrame, 1e-4f));
    plat::Input& in = fakeplat::next();
    in.keyDown[plat::KEY_ESCAPE] = in.keyPressed[plat::KEY_ESCAPE] = true;
    CHECK(!app->frame());
    in.keyDown[plat::KEY_ESCAPE] = false;
    CHECK(app->frame());
}

// V steps through the 46 views and back to the first one; the orbit view turns round the table
// with a right-drag, pans with a middle-drag and zooms with the wheel.
TEST(viewer_anim_view_key_steps_through_every_camera) {
    headless::App* app = headless::shared(kDefault);
    REQUIRE(app);
    app->key('R');
    app->frames(240);   // 4 s: White's hand is on its way to the e-pawn
    const m::vec3 first = eye(*app);
    CHECK(near(first, m::vec3(1.50f, 1.18f, 0.0f)));
    for (int v = 0; v < kViewCount; ++v) {
        checkView(*app, v);
        if (v == 5 || v == 6) {
            // Through White's (+Z) / Black's (-Z) eyes, 55 degrees wide.
            CHECK(v == 5 ? eye(*app).z > 0.3f : eye(*app).z < -0.3f);
            CHECK(eye(*app).y > 1.0f);
            CHECK(near(fovDeg(*app), 55.0f, 0.01f));
        }
        if (v == 32) CHECK(focus(*app) <= 0.2001f);   // shaked: from below, at most 20 cm
        if (v == 11) {
            // The orbit camera: yaw 1.2, pitch 0.35, 1.9 m from the table's centre above the board.
            const m::vec3 target(0.0f, 0.95f, 0.0f);
            CHECK(near(eye(*app), viewer_test::orbitEye(target, 1.2f, 0.35f, 1.9f)));
            CHECK(near(focus(*app), 1.9f));
            app->wheel(2.0f);   // 0.9 of the distance per notch
            const float d = 1.9f * 0.81f;
            CHECK(near(eye(*app), viewer_test::orbitEye(target, 1.2f, 0.35f, d)));
            CHECK(near(focus(*app), d));
            plat::Input& in = fakeplat::next();
            app->moveMouse(600.0f, 300.0f);
            in.mouseDown[plat::MOUSE_RIGHT] = true;
            app->moveMouse(700.0f, 300.0f);   // 100 px: 0.5 rad the other way round
            in.mouseDown[plat::MOUSE_RIGHT] = false;
            app->frame();
            CHECK(near(eye(*app), viewer_test::orbitEye(target, 0.7f, 0.35f, d)));
            // A middle-drag moves the target (and the eye with it) along the camera's up axis.
            const render::Camera before = app->renderer().camera();
            in.mouseDown[plat::MOUSE_MIDDLE] = true;
            app->moveMouse(700.0f, 340.0f);
            in.mouseDown[plat::MOUSE_MIDDLE] = false;
            app->frame();
            CHECK(near(eye(*app), before.position + before.up() * (40.0f * 0.0015f * d)));
            CHECK(near(app->renderer().camera().forward(), before.forward(), 1e-4f));
        }
        app->key('V');
    }
    CHECK(near(eye(*app), first));
}

// --solo draws only the pieces held or near a playing fingertip (none during the handshake, White's
// e-pawn once its hand has come for it), --only 1 Black's robot alone; --shot with --time and
// --play starts the timeline there and keeps it running.
TEST(viewer_anim_solo_and_only_draw_less) {
    headless::App* app = headless::shared(kDefault);
    REQUIRE(app);
    const uint64_t all = frameDraws(*app);
    app = headless::shared({"--scene", "anim", "--shot", "--time", "1.5", "--play", "--solo", "--only", "1"});
    REQUIRE(app);
    app->frames(3);
    CHECK(near(sceneTime(*app), 1.5f + 3 * kFrame, 1e-4f));
    const uint64_t few = frameDraws(*app);
    CHECK(few > 0);
    CHECK(few < all / 2);
    app = headless::shared({"--scene", "anim", "--shot", "--time", "4.2", "--play", "--solo", "--only", "1"});
    REQUIRE(app);
    app->frames(3);
    CHECK(near(sceneTime(*app), 4.2f + 3 * kFrame, 1e-4f));
    CHECK(frameDraws(*app) > few);
}

// The left-handed timeline with scoresheets: pens picked up, every move written (the ink stays on
// the page), a page turned by each player, White's pen laid down, and the final handshake while
// Black still holds its pen, which it lays down before the clasp. Every view is drawn on the way.
TEST(viewer_anim_lefty_demo_writes_the_moves_and_turns_the_pages) {
    LogCapture log;
    headless::App* app = headless::shared({"--scene", "anim", "--demo", "lefty"});
    REQUIRE(app);
    for (int v = 0; v < kViewCount; ++v) {
        app->frames(20);
        app->key('V');
    }
    app->frames(20);   // 17.2 s
    CHECK(log.inOrder({"anim viewer: handshake at", "(Black does not hold its pen)", "White picked up its pen", "White e2-e4",
                       "Black d7-d5", "White e4xd5 (piece 4 takes 19)", "Black d8xd5 (piece 27 takes 4)", "White turned a page",
                       "(Black holds its pen)", "Black put down its pen", "handshake clasp"}));
    CHECK_EQ(log.count("picked up its pen"), 2);
    CHECK_EQ(log.count("turned a page"), 2);
    CHECK_EQ(log.count("put down its pen"), 2);
    CHECK_EQ(log.count("pressed the clock"), 4);
    CHECK_EQ(log.count("handshake clasp"), 4);
    // The ink of the written moves and the page being turned were drawn.
    CHECK(uploaded("ink"));
    CHECK(uploaded("page"));
    CHECK(uploaded("page-flipped"));
}

// The coach demo (Black, left hand) with --time 30: after its gestures it demonstrates 2...dxe4
// 3.Nxe4 Nf6 and takes the three moves back, the last one first. The close-up views of its hand.
TEST(viewer_anim_coach_demo_demonstrates_and_takes_back) {
    LogCapture log;
    headless::App* app = headless::shared({"--scene", "anim", "--demo", "coach", "--time", "30", "--view", "coachhand"});
    REQUIRE(app);
    app->frame();
    CHECK(near(sceneTime(*app), 30.0f + kFrame, 1e-3f));
    CHECK(log.inOrder({"coach demonstrates d5-e4 (piece 19, a capture)", "coach demonstrates c3-e4 (piece 9, a capture)",
                       "coach demonstrates g8-f6 (piece 30)", "coach takes back its demonstration move (piece 30)",
                       "coach takes back its demonstration move (piece 9)", "coach takes back its demonstration move (piece 19)"}));
    checkView(*app, 26);
    app->key('V');
    checkView(*app, 27);
    app->key('V');
    checkView(*app, 28);
}

// Black castles short and promotes b2-b1=Q with its left hand, pressing the clock after each;
// --only 1 and --cam (eye, target, field of view of a camera of its own).
TEST(viewer_anim_left_hand_castles_and_promotes) {
    {
        LogCapture log;
        headless::App* app = headless::shared({"--scene", "anim", "--demo", "lcastle", "--time", "4", "--only", "1"});
        REQUIRE(app);
        app->frames(2);
        CHECK(log.inOrder({"anim viewer: Black O-O (left hand)", "Black pressed the clock"}));
        CHECK(!log.has("White pressed the clock"));
    }
    {
        LogCapture log;
        headless::App* app = headless::shared({"--scene", "anim", "--demo", "lpromo", "--time", "5", "--cam", "0.5,1.6,1.4,0,0.8,0,30"});
        REQUIRE(app);
        app->frames(2);
        CHECK(log.inOrder({"anim viewer: Black b2-b1=Q (left hand)", "Black pressed the clock"}));
        CHECK(near(eye(*app), m::vec3(0.5f, 1.6f, 1.4f)));
        CHECK(near(fovDeg(*app), 30.0f, 0.01f));
        CHECK(near(focus(*app), std::sqrt(0.25f + 0.64f + 1.96f)));
    }
}

// The stance script ("t:who:stance,..." after --stance's own step at 0.3 s): White stands up
// (pushing its chair back, stepping aside), walks to the side of the table, comes back and sits
// down; Black goes to its right; malformed steps are reported and skipped. With --head White's
// eyes keep the pitch given whatever its stance, and --pen gives both robots their pens first.
TEST(viewer_anim_stance_script_stands_walks_and_sits_down) {
    LogCapture log;
    headless::App* app = headless::shared({"--scene", "anim", "--stance", "standing", "--stances",
                                           "2.6:w:side-left,6:w:seated,0.5:b:side-right,1:x:bogus,junk", "--left", "wb",
                                           "--head", "-20", "--pen", "--time", "10", "--view", "opp"});
    REQUIRE(app);
    app->frame();
    CHECK_EQ(log.lines("bad stance step").size(), size_t(2));
    CHECK(log.has("bad stance step '1:x:bogus'"));
    CHECK(log.has("bad stance step 'junk'"));
    CHECK(log.inOrder({"White asks for stance 1 at t=0.300", "Black asks for stance 3", "White pushes its chair back", "White footstep",
                       "White asks for stance 2", "White reaches its stance", "White asks for stance 0", "White draws its chair in"}));
    CHECK(log.inOrder({"White picked up its pen", "White put down its pen"}));   // a stance change lays it down
    CHECK(log.has("Black reaches its stance"));
    // The views made for the stances, opp .. hips, then round to White's eyes.
    for (int v = 36; v < kViewCount + 5; ++v) {
        checkView(*app, v % kViewCount);
        app->key('V');
    }
    CHECK(near(app->renderer().camera().forward().y, std::sin(-20.0f * m::DEG), 2e-3f));
}

// --robot draws the porcelain robot (and the game's table and chairs) instead of the debug
// capsules; --shot with --time freezes the timeline at that instant.
TEST(viewer_anim_porcelain_robot_frozen_for_the_shot) {
    headless::App* app = headless::shared({"--scene", "anim", "--robot", "--solo", "--only", "0", "--shot", "--time", "2", "--view", "hand"});
    REQUIRE(app);
    CHECK(uploaded("eye_cornea"));
    app->frames(5);
    CHECK(near(sceneTime(*app), 2.0f, 1e-5f));
    checkView(*app, 7);
    CHECK(frameDraws(*app) > 0);
}

// --selftest: every square for both players, the full starting position, the game's move
// compositions (captures into every capture slot, castling, promotions; with and without the
// obstacle callbacks), the head override, the writing hands, the mirror image of a left-handed
// player, the handshake with a pen in hand, the timeline and the coach demo; all logged. The scene
// then plays its default timeline.
TEST(viewer_anim_self_test_passes_its_checks) {
    LogCapture log;
    headless::App* app = headless::shared({"--scene", "anim", "--selftest"});
    REQUIRE(app);
    CHECK(log.has("selftest: 0 sequences failed"));
    CHECK(log.has("selftest board: "));
    const std::vector<std::string> comps = log.lines("selftest composition ");
    CHECK_EQ(comps.size(), size_t(2 * (layout::captureSlotEarlyCount() + 8)));
    for (const std::string& line : comps) CHECK(line.find(" 0 wrong,") != std::string::npos);
    CHECK(log.has("selftest: head override yaw"));
    CHECK(log.has("selftest writing White (right-handed): 0 wrong"));
    CHECK(log.has("selftest writing Black (left-handed): 0 wrong"));
    CHECK(log.has(" 0 event mismatches"));
    CHECK(log.has("selftest handshake with a left-handed player holding the pen"));
    CHECK(log.lines("W] selftest writing").empty());
    CHECK(log.lines("W] selftest mirror").empty());
    CHECK(log.lines("W] selftest handshake").empty());
    CHECK(log.has("selftest timeline: lowest fingertip"));
    CHECK(log.has("after the rewind the farthest piece is 0.00 mm"));
    app->frames(2);
    CHECK(near(sceneTime(*app), 2 * kFrame, 1e-4f));
}
