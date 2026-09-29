// Hot-seat (two players on one PC): who has the controls, which hand plays, the rematch swap, the
// clock frozen during the handover, the handover itself, the held-button latch, the two-player Elo.
#include "test.h"
#include "game/elo.h"
#include "game/hotseat.h"
#include "game/layout.h"
#include <cmath>
#include <cstdlib>

using namespace m;
using game::CameraPose;
using game::hotseat::Handover;

namespace {
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool near(vec3 a, vec3 b, float eps = 1e-3f) { return length(a - b) <= eps; }

CameraPose eyes(int seat) {
    float z = seat == 0 ? 0.52f : -0.52f;
    return CameraPose::looking(vec3(0.0f, layout::EYE_HEIGHT, z), vec3(0, layout::BOARD_TOP_Y, 0), 52.0f * DEG);
}

chess::TimeControl timeControl(int64_t baseMs, int64_t incMs, int64_t delayMs) {
    chess::TimeControl tc;
    tc.unlimited = false;
    tc.baseMs = baseMs;
    tc.incrementMs = incMs;
    tc.delayMs = delayMs;
    return tc;
}
}  // namespace

// ---- Seats ----------------------------------------------------------------------------------------

TEST(hotseat_input_seat_is_the_seat_to_move) {
    using game::hotseat::inputSeat;
    // Against Stockfish (or online) the human keeps the controls whoever is to move.
    CHECK_EQ(inputSeat(false, 1, 0), 1);
    CHECK_EQ(inputSeat(false, 1, 1), 1);
    CHECK_EQ(inputSeat(false, 0, 1), 0);
    // Hot-seat: the seat to move.
    CHECK_EQ(inputSeat(true, 0, 0), 0);
    CHECK_EQ(inputSeat(true, 0, 1), 1);
    CHECK_EQ(inputSeat(true, 1, 0), 0);
}

TEST(hotseat_clock_side_and_playing_hands) {
    using game::hotseat::clockOnPositiveX;
    using game::hotseat::playsLeftHanded;
    // White sits at +Z facing -Z: its right is +X.
    CHECK(clockOnPositiveX(0));
    CHECK(!clockOnPositiveX(1));
    // Clock at White's right: White plays right-handed, Black (clock on its left) left-handed.
    CHECK(!playsLeftHanded(0, clockOnPositiveX(0)));
    CHECK(playsLeftHanded(1, clockOnPositiveX(0)));
    // Clock at Black's right: the other way round.
    CHECK(playsLeftHanded(0, clockOnPositiveX(1)));
    CHECK(!playsLeftHanded(1, clockOnPositiveX(1)));
    // In a hot-seat game exactly one player plays left-handed.
    for (int side = 0; side < 2; ++side)
        CHECK(playsLeftHanded(0, clockOnPositiveX(side)) != playsLeftHanded(1, clockOnPositiveX(side)));
}

TEST(hotseat_rematch_swaps_colours_and_keeps_hands) {
    game::hotseat::Players p;
    p.names[0] = "Alice";
    p.names[1] = "Bob";
    p.hands[0] = 0;
    p.hands[1] = 2;
    p.clockRightOf = 1;  // Bob's right: Alice (White) is left-handed
    p.rated = true;
    game::hotseat::Players q = p.swapped();
    CHECK_EQ(q.names[0], std::string("Bob"));
    CHECK_EQ(q.names[1], std::string("Alice"));
    CHECK_EQ(q.hands[0], 2);
    CHECK_EQ(q.hands[1], 0);
    CHECK(q.rated);
    // Each person keeps the hand they play with: the clock follows its player.
    using game::hotseat::clockOnPositiveX;
    using game::hotseat::playsLeftHanded;
    bool aliceBefore = playsLeftHanded(0, clockOnPositiveX(p.clockRightOf));
    bool aliceAfter = playsLeftHanded(1, clockOnPositiveX(q.clockRightOf));
    CHECK(aliceBefore);
    CHECK_EQ(aliceBefore, aliceAfter);
    CHECK_EQ(q.names[q.clockRightOf], std::string("Bob"));
    // Two rematches: the first set-up again.
    game::hotseat::Players r = q.swapped();
    CHECK_EQ(r.names[0], p.names[0]);
    CHECK_EQ(r.hands[1], p.hands[1]);
    CHECK_EQ(r.clockRightOf, p.clockRightOf);
}

// ---- Clock and handover -----------------------------------------------------------------------------

TEST(hotseat_clock_frozen_during_the_flight) {
    chess::Clock clock;
    clock.setup(timeControl(60000, 2000, 0));
    clock.start(chess::White);
    double accum = 0.0;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 60; ++i) game::hotseat::advanceClock(clock, accum, dt, false);
    CHECK(std::llabs(clock.remainingMs(chess::White) - 59000) <= 1);
    // White presses: the increment is added at the press, before the freeze.
    clock.press(chess::White);
    int64_t white = clock.remainingMs(chess::White), black = clock.remainingMs(chess::Black);
    CHECK(std::llabs(white - 61000) <= 1);
    double accumAtPress = accum;
    // The view flies to Black's eyes: nothing counts, the accumulator is not fed.
    Handover h;
    h.start(0, 1, game::CameraFlight::kHandoverDuration, eyes(0), eyes(1), vec3(0, layout::BOARD_TOP_Y, 0));
    int frames = 0, landings = 0;
    while (h.active() && frames < 1000) {
        game::hotseat::advanceClock(clock, accum, dt, h.active());
        Handover::Step st = h.update(dt, eyes(1));
        if (st.landed) ++landings;
        ++frames;
    }
    CHECK_EQ(landings, 1);
    CHECK(std::abs(frames - 96) <= 1);  // 1.6 s at 60 Hz
    CHECK_EQ(clock.remainingMs(chess::White), white);
    CHECK_EQ(clock.remainingMs(chess::Black), black);
    CHECK(accum == accumAtPress);
    CHECK(clock.running() == chess::Black);
    // Landed: Black's clock runs.
    for (int i = 0; i < 30; ++i) game::hotseat::advanceClock(clock, accum, dt, h.active());
    CHECK(std::llabs(clock.remainingMs(chess::Black) - 59500) <= 1);
    CHECK_EQ(clock.remainingMs(chess::White), white);
}

TEST(hotseat_delay_window_starts_on_landing) {
    // Bronstein delay: the flight never eats into the next player's delay window.
    chess::Clock clock;
    clock.setup(timeControl(60000, 0, 3000));
    clock.start(chess::White);
    double accum = 0.0;
    game::hotseat::advanceClock(clock, accum, 1.0f, false);
    clock.press(chess::White);
    CHECK_EQ(clock.delayLeftMs(), 3000);
    for (int i = 0; i < 96; ++i) game::hotseat::advanceClock(clock, accum, 1.0f / 60.0f, true);
    CHECK_EQ(clock.delayLeftMs(), 3000);
    CHECK_EQ(clock.remainingMs(chess::Black), 60000);
    game::hotseat::advanceClock(clock, accum, 1.0f, false);
    CHECK_EQ(clock.delayLeftMs(), 2000);
    // Black presses within the delay: the time used comes back.
    clock.press(chess::Black);
    CHECK_EQ(clock.remainingMs(chess::Black), 60000);
}

TEST(hotseat_handover_flight) {
    Handover h;
    CHECK(!h.active());
    CameraPose from = eyes(0), to = eyes(1);
    h.start(0, 1, 1.6f, from, to, vec3(0, layout::BOARD_TOP_Y, 0));
    CHECK(h.active());
    CHECK(h.flying());
    CHECK_EQ(h.viewSeat(), -1);
    CHECK_EQ(h.fromSeat(), 0);
    CHECK_EQ(h.toSeat(), 1);
    CHECK(near(h.totalTime(), 1.6f));
    // Mid-flight: above the board, looking down, no fade.
    Handover::Step st;
    for (int i = 0; i < 48; ++i) st = h.update(1.0f / 60.0f, to);
    CHECK(!st.landed && !st.cut);
    CHECK(h.pose().position.y > layout::EYE_HEIGHT + 0.3f);
    CHECK(std::fabs(h.pose().position.z) < 0.1f);
    CHECK(h.pose().pitch < from.pitch - 0.3f);
    CHECK(near(h.fade(), 0.0f));
    CHECK(near(h.progress(), 0.5f, 0.02f));
    // The next player's head moved: the flight lands on it.
    CameraPose moved = to;
    moved.position = moved.position + vec3(0.02f, -0.03f, 0.01f);
    moved.yaw += 0.08f;
    bool landed = false, cut = false;
    for (int i = 0; i < 60 && h.active(); ++i) {
        st = h.update(1.0f / 60.0f, moved);
        landed = landed || st.landed;
        cut = cut || st.cut;
    }
    CHECK(landed);
    CHECK(!cut);
    CHECK(!h.active());
    CHECK(near(h.pose().position, moved.position));
    CHECK(near(std::fabs(game::angleDelta(h.pose().yaw, moved.yaw)), 0.0f));
    CHECK_EQ(h.viewSeat(), 1);
    // The flight length is kept between 0.8 and 2 s.
    h.start(1, 0, 5.0f, to, from, vec3(0));
    CHECK(near(h.totalTime(), Handover::kMaxFlight));
    h.start(1, 0, 0.2f, to, from, vec3(0));
    CHECK(near(h.totalTime(), Handover::kMinFlight));
}

TEST(hotseat_flight_keeps_the_board_in_view) {
    // The arc orbits the board: all along the flight the middle of the view stays on the board
    // (it is 8 squares wide), with the horizon level.
    CameraPose from = eyes(0), to = eyes(1);
    vec3 centre(0, layout::BOARD_TOP_Y, 0);
    game::CameraFlight f;
    f.start(from, to, game::CameraFlight::kHandoverDuration, game::CameraFlight::handoverShape(from, to, centre));
    float worst = 0.0f;
    for (int i = 0; i <= 40; ++i) {
        CameraPose p = f.sample(float(i) / 40.0f);
        vec3 d = p.forward();
        CHECK(d.y < -0.3f);
        vec3 hit = p.position + d * ((centre.y - p.position.y) / d.y);
        worst = std::max(worst, length(vec3(hit.x, 0.0f, hit.z)));
        CHECK(near(p.roll, 0.0f));
    }
    CHECK(worst < 1.5f * layout::SQUARE_SIZE);
    // Also from Black's eyes to White's.
    f.start(to, from, game::CameraFlight::kHandoverDuration, game::CameraFlight::handoverShape(to, from, centre));
    CameraPose mid = f.sample(0.5f);
    vec3 d = mid.forward();
    vec3 hit = mid.position + d * ((centre.y - mid.position.y) / d.y);
    CHECK(length(vec3(hit.x, 0.0f, hit.z)) < 0.02f);
}

TEST(hotseat_handover_cut) {
    // Instant cut (motion sickness): fade to black in the mover's eyes, change seats, fade in.
    Handover h;
    h.start(1, 0, 0.0f, eyes(1), eyes(0), vec3(0));
    CHECK(h.active());
    CHECK(!h.flying());
    CHECK_EQ(h.viewSeat(), 1);
    CHECK(near(h.totalTime(), Handover::kFadeOut + Handover::kFadeIn));
    int cuts = 0, landings = 0;
    float maxFade = 0.0f, t = 0.0f;
    const float dt = 1.0f / 120.0f;
    while (h.active() && t < 2.0f) {
        Handover::Step st = h.update(dt, eyes(0));
        t += dt;
        if (st.cut) {
            ++cuts;
            CHECK(t >= Handover::kFadeOut - 1e-4f);
            CHECK_EQ(h.viewSeat(), 0);
        }
        if (st.landed) ++landings;
        maxFade = std::max(maxFade, h.fade());
        if (cuts == 0) CHECK_EQ(h.viewSeat(), 1);
    }
    CHECK_EQ(cuts, 1);
    CHECK_EQ(landings, 1);
    CHECK(maxFade > 0.97f);
    CHECK(near(h.fade(), 0.0f));
    CHECK(near(t, Handover::kFadeOut + Handover::kFadeIn, 0.02f));
    // One very long frame crosses both fades.
    h.start(0, 1, 0.0f, eyes(0), eyes(1), vec3(0));
    Handover::Step st = h.update(1.0f, eyes(1));
    CHECK(st.cut && st.landed);
    CHECK(!h.active());
}

TEST(hotseat_held_buttons_are_ignored_until_released) {
    game::hotseat::InputGate g;
    CHECK(!g.blocked(true));  // not armed: nothing is held back
    g.arm();
    CHECK(g.blocked(true));   // still held from the previous turn
    CHECK(g.blocked(true));
    CHECK(!g.blocked(false)); // released
    CHECK(!g.blocked(true));  // a new press counts
    g.arm();
    CHECK(!g.blocked(false)); // nothing held at the landing
}

// ---- Two-player Elo -----------------------------------------------------------------------------------

TEST(hotseat_elo_pair) {
    // Two new players at 1500: +20 / -20 (K 40 each).
    elo::Record w, b;
    elo::PairChange c = elo::applyPair(w, b, 1.0);
    CHECK_EQ(c.white.before, 1500);
    CHECK_EQ(c.white.after, 1520);
    CHECK_EQ(c.black.after, 1480);
    CHECK_EQ(w.wins, 1);
    CHECK_EQ(b.losses, 1);
    CHECK_EQ(w.games, 1);
    CHECK_EQ(b.games, 1);
    // Both are rated against the other's rating BEFORE the game: a draw between 1520 and 1480.
    c = elo::applyPair(w, b, 0.5);
    CHECK_EQ(c.white.delta(), -c.black.delta());
    CHECK(c.white.delta() < 0);
    CHECK_EQ(w.draws, 1);
    CHECK_EQ(b.draws, 1);
    // Different K factors: the changes do not cancel out.
    elo::Record est, fresh;
    est.games = 50;
    est.rating = est.peak = 1600;
    fresh.rating = fresh.peak = 1600;
    c = elo::applyPair(est, fresh, 0.0);
    CHECK_EQ(c.white.delta(), -10);  // K 20
    CHECK_EQ(c.black.delta(), 20);   // K 40
    CHECK_EQ(c.white.k, 20);
    CHECK_EQ(c.black.k, 40);
}
