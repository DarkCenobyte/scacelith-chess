// Game modes: camera flights, the viewer's observer camera, untimed games (the Elo rating:
// elo_tests.cpp).
#include "test.h"
#include "game/camera_flight.h"
#include "game/clock_rules.h"
#include "game/layout.h"
#include "game/observer_camera.h"
#include <cmath>

using namespace m;
using game::CameraFlight;
using game::CameraPose;
using game::ObserverCamera;

namespace {
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool near(vec3 a, vec3 b, float eps = 1e-3f) { return length(a - b) <= eps; }
}  // namespace

// ---- Camera flights -----------------------------------------------------------------------------

TEST(camera_pose_orientation) {
    // Matches render::Camera::lookAt (lookRotation(eye - target, up)).
    vec3 eye(1.35f, 1.28f, 0.05f), target(0.0f, 0.86f, 0.0f);
    CameraPose p = CameraPose::looking(eye, target, 50.0f * DEG);
    vec3 want = normalize(target - eye);
    CHECK(near(p.forward(), want));
    CHECK(near(rotate(p.orientation(), vec3(0, 0, -1)), want));
    quat ref = lookRotation(eye - target, vec3(0, 1, 0));
    CHECK(near(rotate(p.orientation(), vec3(1, 0, 0)), rotate(ref, vec3(1, 0, 0))));
    CHECK(near(rotate(p.orientation(), vec3(0, 1, 0)), rotate(ref, vec3(0, 1, 0))));
    // Round trip through an orientation with some roll.
    quat q = normalize(axisAngle(vec3(0, 1, 0), 2.5f) * axisAngle(vec3(1, 0, 0), -0.4f) * axisAngle(vec3(0, 0, 1), 0.12f));
    CameraPose r = CameraPose::fromOrientation(vec3(0), q, 1.0f);
    CHECK(near(r.roll, 0.12f));
    CHECK(near(r.pitch, -0.4f));
    CHECK(near(std::fabs(game::angleDelta(r.yaw, 2.5f)), 0.0f));
    CHECK(near(rotate(r.orientation(), vec3(0, 1, 0)), rotate(q, vec3(0, 1, 0))));
}

TEST(camera_angle_delta) {
    CHECK(near(game::angleDelta(0.0f, 0.5f), 0.5f));
    CHECK(near(game::angleDelta(3.0f, -3.0f), 2.0f * PI - 6.0f));
    CHECK(near(game::angleDelta(-3.0f, 3.0f), 6.0f - 2.0f * PI));
    CHECK(near(game::angleDelta(1.0f, 1.0f + 4.0f * PI), 0.0f));
}

TEST(camera_flight_endpoints) {
    CameraPose a = CameraPose::looking(vec3(4.5f, 2.2f, 8.5f), vec3(-2.0f, 1.8f, 0.0f), 52.0f * DEG);
    CameraPose b = CameraPose::looking(vec3(0.0f, 1.55f, 0.35f), vec3(0.0f, layout::BOARD_TOP_Y, 0.0f), 40.0f * DEG);
    CameraFlight f;
    f.start(a, b);
    CHECK(f.active());
    CHECK(f.duration() >= 0.7f && f.duration() <= 2.6f);
    CHECK(near(f.sample(0.0f).position, a.position));
    CHECK(near(f.sample(1.0f).position, b.position));
    CHECK(near(f.sample(1.0f).forward(), b.forward()));
    CHECK(near(f.sample(1.0f).fovY, b.fovY));
    // Runs to the end and stops there.
    CameraPose p;
    vec3 prev = a.position;
    float maxStep = 0.0f;
    for (int i = 0; i < 400 && f.active(); ++i) {
        p = f.update(1.0f / 60.0f);
        maxStep = std::max(maxStep, length(p.position - prev));
        prev = p.position;
    }
    CHECK(!f.active());
    CHECK(near(p.position, b.position));
    CHECK(maxStep < 0.3f);  // no jump along the way
}

TEST(camera_flight_handover) {
    // Between the two players' eyes: rises above both, arcs over the board, turns 180 degrees
    // without rolling, looks down on the way.
    CameraPose white = CameraPose::looking(vec3(0.0f, layout::EYE_HEIGHT, 0.52f), vec3(0, layout::BOARD_TOP_Y, 0), 52.0f * DEG);
    CameraPose black = CameraPose::looking(vec3(0.0f, layout::EYE_HEIGHT, -0.52f), vec3(0, layout::BOARD_TOP_Y, 0), 52.0f * DEG);
    game::FlightShape shape = CameraFlight::handoverShape(white, black, vec3(0, layout::BOARD_TOP_Y, 0));
    CameraFlight f;
    f.start(white, black, CameraFlight::kHandoverDuration, shape);
    CameraPose mid = f.sample(0.5f);
    CHECK(mid.position.y > layout::EYE_HEIGHT + 0.3f);
    CHECK(std::fabs(mid.position.z) < 0.05f);
    CHECK(mid.pitch < white.pitch - 0.3f);
    CHECK(near(mid.roll, 0.0f));
    // Yaw turns one way only (counter-clockwise), by half a turn.
    float prevYaw = f.sample(0.0f).yaw;
    bool monotonic = true;
    for (int i = 1; i <= 50; ++i) {
        float y = f.sample(float(i) / 50.0f).yaw;
        if (y < prevYaw - 1e-5f) monotonic = false;
        prevYaw = y;
    }
    CHECK(monotonic);
    CHECK(near(std::fabs(game::angleDelta(f.sample(1.0f).yaw, black.yaw)), 0.0f));
    // The target moves (the other head): the flight still lands on it.
    CameraPose moved = black;
    moved.position = moved.position + vec3(0.03f, -0.02f, 0.0f);
    moved.yaw += 0.1f;
    f.retarget(moved);
    CHECK(near(f.sample(1.0f).position, moved.position));
    CHECK(near(std::fabs(game::angleDelta(f.sample(1.0f).yaw, moved.yaw)), 0.0f));
}

// ---- Observer camera ----------------------------------------------------------------------------

TEST(observer_keys_qwerty_azerty) {
    plat::Input in;
    in.keyDown['W'] = true;
    CHECK(ObserverCamera::read(in, true, false).move.z > 0.5f);
    in.keyDown['W'] = false;
    in.keyDown['Z'] = true;  // AZERTY forward
    CHECK(ObserverCamera::read(in, true, false).move.z > 0.5f);
    in.keyDown['Z'] = false;
    in.keyDown['Q'] = true;  // AZERTY left
    CHECK(ObserverCamera::read(in, true, false).move.x < -0.5f);
    in.keyDown['Q'] = false;
    in.keyDown['A'] = true;  // QWERTY left
    CHECK(ObserverCamera::read(in, true, false).move.x < -0.5f);
    in.keyDown['A'] = false;
    in.keyDown[plat::KEY_SPACE] = true;
    CHECK(ObserverCamera::read(in, true, false).move.y > 0.5f);
    in.keyDown[plat::KEY_SPACE] = false;
    in.keyDown[plat::KEY_LCTRL] = true;
    CHECK(ObserverCamera::read(in, true, false).move.y < -0.5f);
    // Keys ignored while a menu has the keyboard; mouse motion only while looking.
    CHECK(!ObserverCamera::read(in, false, false).any());
    in.mouseDX = 12.0f;
    CHECK(!ObserverCamera::read(in, false, false).any());
    CHECK(ObserverCamera::read(in, false, true).lookX == 12.0f);
}

TEST(observer_moves_and_stays_in_hall) {
    ObserverCamera cam;
    cam.setPose(CameraPose::looking(vec3(0, 1.5f, 3.0f), vec3(0, 1.5f, 0), 52.0f * DEG));
    ObserverCamera::Controls fwd;
    fwd.move = vec3(0, 0, 1);
    for (int i = 0; i < 60; ++i) cam.update(1.0f / 60.0f, fwd);
    CHECK(cam.pose().position.z < 3.0f - 0.5f);  // moved towards -Z (the view direction)
    CHECK(near(cam.pose().position.x, 0.0f));
    // Fly far through a wall and into the floor: clamped inside the hall.
    ObserverCamera::Controls escape;
    escape.move = vec3(1, -1, 1);
    escape.fast = true;
    for (int i = 0; i < 60 * 30; ++i) cam.update(1.0f / 60.0f, escape);
    vec3 p = cam.pose().position;
    CHECK(p.x < layout::HALL_MAX_X && p.x > layout::HALL_MIN_X);
    CHECK(p.z < layout::HALL_MAX_Z && p.z > layout::HALL_MIN_Z);
    CHECK(p.y > 0.1f && p.y < layout::HALL_HEIGHT);
    // Mouse look turns; the pitch is limited.
    ObserverCamera::Controls look;
    look.lookY = -100000.0f;
    cam.update(1.0f / 60.0f, look);
    CHECK(cam.pose().pitch < 89.0f * DEG && cam.pose().pitch > 80.0f * DEG);
    // Wheel changes the speed within limits.
    ObserverCamera::Controls wheel;
    wheel.wheel = 100.0f;
    cam.update(1.0f / 60.0f, wheel);
    CHECK(near(cam.speed(), ObserverCamera::kMaxSpeed));
}

TEST(observer_flight_is_interrupted_by_controls) {
    ObserverCamera cam;
    CameraPose start = CameraPose::looking(vec3(3, 2, 3), vec3(0, 1, 0), 52.0f * DEG);
    CameraPose goal = CameraPose::looking(vec3(-3, 2, -3), vec3(0, 1, 0), 52.0f * DEG);
    cam.setPose(start);
    cam.flyTo(goal);
    ObserverCamera::Controls none;
    for (int i = 0; i < 20; ++i) cam.update(1.0f / 60.0f, none);
    CHECK(cam.flying());
    vec3 mid = cam.pose().position;
    ObserverCamera::Controls fwd;
    fwd.move = vec3(0, 0, 1);
    cam.update(1.0f / 60.0f, fwd);
    CHECK(!cam.flying());
    CHECK(length(cam.pose().position - mid) < 0.1f);  // takes over where the flight was
    // An uninterrupted flight lands exactly.
    cam.flyTo(goal, 0.5f);
    for (int i = 0; i < 60; ++i) cam.update(1.0f / 60.0f, none);
    CHECK(!cam.flying());
    CHECK(near(cam.pose().position, goal.position));
}

// ---- Untimed games ------------------------------------------------------------------------------

TEST(untimed_game_only_offline_without_time_control) {
    const std::vector<chess::TimeControl>& presets = chess::timeControlPresets();
    // "No clock" (preset 0, offered to Play, hot-seat and Watch) is the only untimed preset.
    CHECK(game::untimedGame(false, presets[0]));
    for (size_t i = 1; i < presets.size(); ++i) CHECK(!game::untimedGame(false, presets[i]));
    // A custom time control is always timed (at least 10 s, GameScene::chosenTimeControl).
    chess::TimeControl custom;
    custom.unlimited = false;
    custom.baseMs = 10000;
    CHECK(!game::untimedGame(false, custom));
    // Online and direct games are never untimed, whatever they carry.
    CHECK(!game::untimedGame(true, presets[0]));
    CHECK(!game::untimedGame(true, presets[5]));
}

TEST(untimed_clock_keeps_used_time_without_press) {
    // Nobody presses the clock of an untimed game: each completed move switches it (completeMove),
    // so the used time of each side stays known (a coach's statistics), shown as dashes only.
    chess::Clock c;
    c.setup(chess::timeControlPresets()[0]);
    c.start(chess::White);
    c.update(4200);          // White thinks and moves
    c.press(chess::White);   // the move is completed as its piece is released
    c.update(1300);          // Black
    c.press(chess::Black);
    c.update(800);           // White again
    CHECK_EQ(c.remainingMs(chess::White), int64_t(5000));
    CHECK_EQ(c.remainingMs(chess::Black), int64_t(1300));
    CHECK(c.running() == chess::White);
    CHECK(!c.flagged(chess::White) && !c.flagged(chess::Black));
}
