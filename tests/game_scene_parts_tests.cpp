// Small parts of the game scene's machinery that a game played on the headless App does not reach
// on its own: the observer camera kept inside other bounds and following a moving target with and
// without a flight (src/game/observer_camera.cpp, camera_flight.cpp), the capture slots of one half
// all taken (src/game/physical_board.cpp), and the reasons of an online ending as the saved games
// name them (src/game/game_saving.cpp). Engine-free, no App.
#include "game_scene_testkit.h"
#include "game/camera_flight.h"
#include "game/game_saving.h"
#include "game/observer_camera.h"
#include "game/physical_board.h"

using namespace gstest;

namespace {

bool nearly(m::vec3 a, m::vec3 b, float eps = 1e-4f) { return m::length(a - b) <= eps; }

}  // namespace

// Bounds set after the pose bring the camera inside them; a target moved while the camera is not
// flying is a jump, while it flies it moves the end of the flight, which ends there.
TEST(game_scene_parts_observer_bounds_and_retarget) {
    game::ObserverCamera cam;
    game::CameraPose p;
    p.position = m::vec3(0.0f, 1.2f, 0.0f);
    cam.setPose(p);
    cam.setBounds(m::vec3(1.0f, 1.0f, 1.0f), m::vec3(2.0f, 2.0f, 2.0f));
    CHECK(nearly(cam.pose().position, m::vec3(1.0f, 1.2f, 1.0f)));

    game::CameraPose q = p;
    q.position = m::vec3(1.5f, 1.5f, 1.5f);
    cam.retarget(q);
    CHECK(nearly(cam.pose().position, q.position));

    game::CameraPose r = p;
    r.position = m::vec3(1.9f, 1.2f, 1.9f);
    cam.flyTo(r, 1.0f);
    REQUIRE(cam.flying());
    CHECK(cam.flight().progress() == 0.0f);
    game::CameraPose s = p;
    s.position = m::vec3(1.2f, 1.1f, 1.8f);
    cam.retarget(s);
    CHECK(nearly(cam.flight().target().position, s.position));
    const game::ObserverCamera::Controls still;
    cam.update(0.5f, still);
    CHECK(std::fabs(cam.flight().progress() - 0.5f) < 1e-3f);
    for (int i = 0; i < 10 && cam.flying(); ++i) cam.update(0.25f, still);
    CHECK(!cam.flying());
    CHECK(nearly(cam.pose().position, s.position, 1e-3f));

    game::CameraFlight none;
    none.start(p, p, 0.0f);
    CHECK(none.progress() >= 0.0f && none.progress() <= 1.0f);
}

// More pieces sent beside one half of the board than it has slots (never in a game: at most 23 per
// half): the next piece goes to the roomiest slot, said in the log.
TEST(game_scene_parts_capture_slots_of_a_half_all_taken) {
    LogTail log;
    game::PhysicalBoard board;
    board.reset(true);
    const int slots = layout::captureSlotCount();
    std::vector<int> ids;
    for (const game::PieceObject& p : board.pieces()) ids.push_back(p.id);
    REQUIRE(int(ids.size()) > slots);
    for (int k = 0; k < slots; ++k) board.nextCaptureSlot(chess::White, ids[size_t(k)]);
    CHECK(!log.has("half is full"));
    const m::vec3 extra = board.nextCaptureSlot(chess::White, ids[size_t(slots)]);
    CHECK(log.has("capture slots: White's half is full"));
    bool isSlot = false;
    for (int k = 0; k < slots; ++k) isSlot = isSlot || nearly(extra, board.captureSlot(chess::White, k));
    CHECK(isSlot);
}

// The online reasons of the end of a game, as keys of the game over card and of the saved games.
TEST(game_scene_parts_online_end_keys) {
    using game::saving::onlineEndKey;
    CHECK_EQ(onlineEndKey(20), std::string("reason.online.abandonment"));
    CHECK_EQ(onlineEndKey(21), std::string("reason.online.abandonment_vs_insufficient"));
    CHECK_EQ(onlineEndKey(22), std::string("reason.online.aborted"));
    CHECK_EQ(onlineEndKey(23), std::string("reason.online.no_show"));
    CHECK_EQ(onlineEndKey(24), std::string("reason.online.forfeit"));
    CHECK_EQ(onlineEndKey(25), std::string("reason.online.server_aborted"));
    CHECK_EQ(onlineEndKey(26), std::string("reason.online.both_disconnected"));
    CHECK_EQ(onlineEndKey(999), std::string(""));
}
