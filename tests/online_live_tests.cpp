// The live side of online play (src/game/online_live.h): my gesture (the aim's dwell, the ply of
// the state's start, the flags, when a gesture is worth sending, the Side test of the look), the
// opponent's gestures (when their piece fields apply, which squares and moves are valid, the pace
// of their robot's hand and its live work at their move, even under a flood of gestures, the
// timeouts, the head's spring), my clock's freeze and the resend of my move after a
// reconnection, the RatingRestored notice held back during a game, and the realtime errors that
// belong to a game or refuse a challenge being created.
#include "test.h"
#include "anim/animator.h"
#include "chess/chess.h"
#include "game/layout.h"
#include "game/online_live.h"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace m;
namespace live = game::live;
namespace flag = net::proto::GestureFlag;
using chess::Black;
using chess::White;

namespace {
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

chess::Position fromFEN(const char* fen) {
    chess::Position pos;
    CHECK(pos.setFEN(fen));
    return pos;
}

net::Gesture handGesture(int ply, int touch, int aim = live::kNoSquare, uint16_t placed = 0) {
    net::Gesture g;
    g.ply = ply;
    g.touch = touch;
    g.aim = aim;
    g.placed = placed;
    return g;
}

// White's eyes at the table (seat 0 at +Z, facing -Z).
const vec3 kEye(0.0f, layout::EYE_HEIGHT, 0.52f);
bool lookAt(vec3 target) { return live::lookBesideBoard(kEye, normalize(target - kEye)); }

const int E2 = 12, E4 = 28, E5 = 36, E7 = 52, D7 = 51, D5 = 35, E1 = 4, G1 = 6, E8 = 60;
}  // namespace

// ---- My gesture ------------------------------------------------------------------------------

TEST(live_aim_dwell_waits_before_a_square_counts) {
    live::Dwell d(live::kAimDwell);
    d.reset();
    // The time counts from the frame the square appears in.
    CHECK_EQ(d.update(E4, 0.05f), live::kNoSquare);
    CHECK_EQ(d.update(E4, 0.05f), live::kNoSquare);
    CHECK_EQ(d.update(E4, 0.05f), live::kNoSquare);
    CHECK_EQ(d.update(E4, 0.03f), E4);
    // The pointer crossing squares on its way does not move the aim.
    int crossed[] = {E5, D5, E5, D5, E5, D5};
    for (int sq : crossed) CHECK_EQ(d.update(sq, 0.05f), E4);
    CHECK_EQ(d.update(D5, 0.13f), D5);
    // Back to nothing aimed: after the same dwell.
    CHECK_EQ(d.update(live::kNoSquare, 0.1f), D5);
    CHECK_EQ(d.update(live::kNoSquare, 0.1f), D5);
    CHECK_EQ(d.update(live::kNoSquare, 0.1f), live::kNoSquare);
    d.reset(E2);
    CHECK_EQ(d.value(), E2);
}

TEST(live_gesture_ply_is_the_start_of_the_hand_state) {
    live::Hand idle;
    CHECK(idle.idle());
    net::Gesture g = live::buildGesture(idle, 4, 0.0f, 0.0f, 0.0f, false, false, nullptr);
    CHECK_EQ(g.ply, 4);
    CHECK_EQ(g.touch, live::kNoSquare);
    CHECK_EQ(g.aim, live::kNoSquare);
    CHECK_EQ(int(g.placed), 0);
    CHECK_EQ(int(g.flags), 0);
    // Still idle after the opponent's move: the state began before it.
    g = live::buildGesture(idle, 5, 0.0f, 0.0f, 0.0f, false, false, &g);
    CHECK_EQ(g.ply, 4);
    // A piece touched: the move being prepared is ply 5.
    live::Hand h;
    h.touch = E2;
    CHECK(!h.idle());
    g = live::buildGesture(h, 5, 0.0f, 0.0f, 0.0f, false, false, &g);
    CHECK_EQ(g.ply, 5);
    CHECK_EQ(g.touch, E2);
    // The head alone (a turn, the glance, a look aside) keeps the ply.
    net::Gesture head = live::buildGesture(h, 6, 0.3f, -0.2f, 0.5f, true, false, &g);
    CHECK_EQ(head.ply, 5);
    // The aim changes the hand's state.
    h.aim = E4;
    g = live::buildGesture(h, 6, 0.0f, 0.0f, 0.0f, false, false, &head);
    CHECK_EQ(g.ply, 6);
    CHECK_EQ(g.aim, E4);
}

TEST(live_gesture_ply_follows_a_game_set_back) {
    // My knight in hand at ply 8, the move sent (the game plays it: 9 plies, my hand idle again),
    // then refused: the board is rebuilt with 8 plies and my hand stays idle.
    live::Hand h;
    h.touch = G1;
    net::Gesture g = live::buildGesture(h, 8, 0.0f, 0.0f, 0.0f, false, false, nullptr);
    CHECK_EQ(g.ply, 8);
    g = live::buildGesture(live::Hand(), 9, 0.0f, 0.0f, 0.0f, false, false, &g);
    CHECK_EQ(g.ply, 9);
    g = live::buildGesture(live::Hand(), 8, 0.0f, 0.0f, 0.0f, false, false, &g);
    CHECK_EQ(g.ply, 8);  // not 9: the opponent's client would keep the knight in the air
    // Head changes and keepalives keep the corrected ply.
    g = live::buildGesture(live::Hand(), 8, 0.4f, -0.1f, 0.3f, true, false, &g);
    CHECK_EQ(g.ply, 8);
    // Once their move comes, the idle state still began at ply 8.
    g = live::buildGesture(live::Hand(), 9, 0.0f, 0.0f, 0.0f, false, false, &g);
    CHECK_EQ(g.ply, 8);
}

TEST(live_gesture_flags_and_head) {
    live::Hand h;
    h.touch = E7;
    h.aim = E8;
    h.promoting = true;
    net::Gesture g = live::buildGesture(h, 20, 0.25f, -0.4f, 1.7f, false, false, nullptr);
    CHECK_EQ(int(g.flags), int(flag::Promoting));
    CHECK(near(g.yaw, 0.25f));
    CHECK(near(g.pitch, -0.4f));
    CHECK(near(g.lean, 1.0f));  // clamped to 0..1
    CHECK(near(live::buildGesture(h, 20, 0.0f, 0.0f, -0.5f, false, false, nullptr).lean, 0.0f));
    // The glance at my scoresheet is a look beside the board.
    g = live::buildGesture(live::Hand(), 20, 0.0f, 0.0f, 0.0f, true, false, nullptr);
    CHECK_EQ(int(g.flags), int(flag::Glance | flag::Side));
    g = live::buildGesture(live::Hand(), 20, 0.0f, 0.0f, 0.0f, false, true, nullptr);
    CHECK_EQ(int(g.flags), int(flag::Side));
    // A staged move: placed, with its squares.
    live::Hand staged;
    staged.touch = E2;
    staged.aim = E4;
    staged.placed = net::packMove(E2, E4, 0);
    g = live::buildGesture(staged, 0, 0.0f, 0.0f, 0.0f, false, false, nullptr);
    CHECK_EQ(int(g.placed), int(net::packMove(E2, E4, 0)));
    CHECK(!live::sameHand(g, live::buildGesture(h, 0, 0.0f, 0.0f, 0.0f, false, false, nullptr)));
}

TEST(live_gesture_due_on_change_head_turn_and_keepalive) {
    net::Gesture last = handGesture(8, E2);
    last.yaw = 0.1f;
    last.pitch = -0.3f;
    last.lean = 0.2f;
    net::Gesture now = last;
    CHECK(!live::gestureDue(last, now, 100.0));
    CHECK(!live::gestureDue(last, now, 999.0));
    CHECK(live::gestureDue(last, now, live::kKeepaliveMs));
    // The head: about a degree, or 0.05 of lean.
    now.yaw = last.yaw + 0.5f * DEG;
    CHECK(!live::gestureDue(last, now, 100.0));
    now.yaw = last.yaw - 1.5f * DEG;
    CHECK(live::gestureDue(last, now, 100.0));
    now = last;
    now.pitch = last.pitch + 1.5f * DEG;
    CHECK(live::gestureDue(last, now, 100.0));
    now = last;
    now.lean = last.lean + 0.04f;
    CHECK(!live::gestureDue(last, now, 100.0));
    now.lean = last.lean + 0.06f;
    CHECK(live::gestureDue(last, now, 100.0));
    // The state: hand, ply, flags.
    now = last;
    now.aim = E4;
    CHECK(live::gestureDue(last, now, 0.0));
    now = last;
    now.ply = 9;
    CHECK(live::gestureDue(last, now, 0.0));
    now = last;
    now.flags = flag::Side;
    CHECK(live::gestureDue(last, now, 0.0));
}

TEST(live_side_look_falls_on_the_table_beside_the_board) {
    // The board, its frame and the squares near the edge: not Side.
    CHECK(!lookAt(vec3(0.0f, layout::BOARD_TOP_Y, 0.0f)));
    CHECK(!lookAt(layout::squareCenter(0, 0)));
    CHECK(!lookAt(layout::squareCenter(7, 7)));
    CHECK(!lookAt(vec3(0.24f, layout::TABLE_TOP_Y, -0.1f)));
    // The clock, the captured pieces, the scoresheets: on the table, either side.
    CHECK(lookAt(vec3(0.40f, layout::TABLE_TOP_Y, 0.05f)));
    CHECK(lookAt(vec3(-0.40f, layout::TABLE_TOP_Y, 0.20f)));
    CHECK(lookAt(vec3(0.30f, layout::TABLE_TOP_Y, -0.30f)));
    // Off the table, or up: not Side.
    CHECK(!lookAt(vec3(0.80f, layout::TABLE_TOP_Y, 0.0f)));
    CHECK(!lookAt(vec3(0.0f, layout::TABLE_TOP_Y, -0.60f)));
    CHECK(!live::lookBesideBoard(kEye, vec3(0.0f, 0.0f, -1.0f)));
    CHECK(!live::lookBesideBoard(kEye, normalize(vec3(0.5f, 0.3f, -1.0f))));
}

// ---- The opponent's gestures -------------------------------------------------------------------

TEST(live_piece_fields_apply_only_to_the_move_being_prepared) {
    live::PieceGate s;
    s.plies = 10;
    s.remoteToMove = s.waiting = s.playing = s.fresh = true;
    net::Gesture g = handGesture(10, E7);
    CHECK(live::piecesApply(g, s));
    // A gesture of an earlier ply (that move is known) or ahead of its MoveMade.
    CHECK(!live::piecesApply(handGesture(9, E7), s));
    CHECK(!live::piecesApply(handGesture(11, E7), s));
    live::PieceGate t = s;
    t.remoteToMove = false;
    CHECK(!live::piecesApply(g, t));
    t = s;
    t.waiting = false;  // my robot still presses the clock
    CHECK(!live::piecesApply(g, t));
    t = s;
    t.moveQueued = true;  // the robot plays their MoveMade
    CHECK(!live::piecesApply(g, t));
    t = s;
    t.playing = false;
    CHECK(!live::piecesApply(g, t));
    t = s;
    t.resync = true;
    CHECK(!live::piecesApply(g, t));
    t = s;
    t.fresh = false;  // sent before the snapshot or our reconnection
    CHECK(!live::piecesApply(g, t));
}

TEST(live_piece_intent_checks_squares_colours_and_moves) {
    chess::Position pos;
    pos.makeMove(pos.findLegal(chess::Square(E2), chess::Square(E4)));  // 1. e4, Black to move
    auto intent = [&](const net::Gesture& g) { return live::pieceIntent(g, pos, Black); };
    live::PieceIntent in = intent(handGesture(1, E7, E5));
    CHECK_EQ(in.touch, E7);
    CHECK_EQ(in.aim, E5);
    CHECK_EQ(int(in.placed), 0);
    // Out of the board, not their piece, an empty square, not their turn: nothing.
    CHECK_EQ(intent(handGesture(1, live::kNoSquare)).touch, live::kNoSquare);
    CHECK_EQ(intent(handGesture(1, -1)).touch, live::kNoSquare);
    CHECK_EQ(intent(handGesture(1, 200)).touch, live::kNoSquare);
    CHECK_EQ(intent(handGesture(1, E4)).touch, live::kNoSquare);
    CHECK_EQ(intent(handGesture(1, 40)).touch, live::kNoSquare);
    CHECK_EQ(live::pieceIntent(handGesture(1, E4), pos, White).touch, live::kNoSquare);
    // Their piece, a square it cannot go to: held, aimed nowhere.
    in = intent(handGesture(1, E7, E4));
    CHECK_EQ(in.touch, E7);
    CHECK_EQ(in.aim, live::kNoSquare);
    CHECK_EQ(intent(handGesture(1, E7, 99)).aim, live::kNoSquare);
    // A move put down: legal, of the piece in hand.
    in = intent(handGesture(1, E7, E5, net::packMove(E7, E5, 0)));
    CHECK_EQ(int(in.placed), int(net::packMove(E7, E5, 0)));
    CHECK_EQ(in.aim, E5);
    in = intent(handGesture(1, E7, live::kNoSquare, net::packMove(E7, E5, 0)));
    CHECK_EQ(in.aim, E5);  // its destination
    in = intent(handGesture(1, E7, E5, net::packMove(D7, D5, 0)));  // of another piece
    CHECK_EQ(int(in.placed), 0);
    CHECK_EQ(in.aim, E5);
    in = intent(handGesture(1, E7, live::kNoSquare, net::packMove(E7, E4, 0)));  // illegal
    CHECK_EQ(int(in.placed), 0);
    CHECK_EQ(in.aim, live::kNoSquare);
}

TEST(live_piece_intent_castling_and_promotion) {
    chess::Position castle = fromFEN("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    live::PieceIntent in = live::pieceIntent(handGesture(0, E1, G1), castle, White);
    CHECK_EQ(in.touch, E1);
    CHECK_EQ(in.aim, G1);
    in = live::pieceIntent(handGesture(0, E1, G1, net::packMove(E1, G1, 0)), castle, White);
    CHECK_EQ(int(in.placed), int(net::packMove(E1, G1, 0)));
    // A pawn on its last rank: aimed without a piece, put down with its piece.
    chess::Position promo = fromFEN("8/4P3/8/8/8/8/8/k6K w - - 0 1");
    in = live::pieceIntent(handGesture(0, E7, E8), promo, White);
    CHECK_EQ(in.aim, E8);
    in = live::pieceIntent(handGesture(0, E7, E8, net::packMove(E7, E8, chess::Knight)), promo, White);
    CHECK_EQ(int(in.placed), int(net::packMove(E7, E8, chess::Knight)));
    in = live::pieceIntent(handGesture(0, E7, E8, net::packMove(E7, E8, 0)), promo, White);
    CHECK_EQ(int(in.placed), 0);
    CHECK_EQ(in.aim, E8);
}

TEST(live_robot_hand_takes_one_step_at_a_time) {
    live::PieceIntent none, e2, e2aim, d2, placed;
    e2.touch = e2aim.touch = placed.touch = E2;
    e2aim.aim = placed.aim = E4;
    placed.placed = net::packMove(E2, E4, 0);
    d2.touch = 11;
    // Nothing held: the piece is taken once the hand is free.
    live::HandStep s = live::handStep(live::kNoSquare, e2, 0.0f);
    CHECK(s.take && !s.letGo && s.follow && !s.place);
    s = live::handStep(live::kNoSquare, e2, live::kHandSlack);
    CHECK(s.take);
    s = live::handStep(live::kNoSquare, e2, 0.3f);
    CHECK(!s.take && !s.letGo && !s.follow && !s.place);
    // The piece in hand follows the aim (followRemoteAim paces the carries) or is put down, busy
    // or not.
    s = live::handStep(E2, e2aim, 0.3f);
    CHECK(!s.take && !s.letGo && s.follow && !s.place);
    s = live::handStep(E2, placed, 0.3f);
    CHECK(!s.take && s.place && !s.follow);
    // Another piece: back on its square and on to the next one, once the hand is free.
    s = live::handStep(E2, d2, 0.3f);
    CHECK(!s.take && !s.letGo && !s.follow);
    s = live::handStep(E2, d2, 0.04f);
    CHECK(s.letGo && s.take && s.follow);
    // Nothing in their hand: the piece goes back once the hand is free.
    s = live::handStep(E2, none, 0.3f);
    CHECK(!s.letGo);
    s = live::handStep(E2, none, 0.0f);
    CHECK(s.letGo && !s.take && !s.follow);
    s = live::handStep(live::kNoSquare, none, 0.0f);
    CHECK(!s.letGo && !s.take && !s.follow && !s.place);
}

TEST(live_move_made_keeps_only_what_prepared_it) {
    const uint16_t e2e4 = net::packMove(E2, E4, 0), d7d5 = net::packMove(D7, D5, 0);
    live::LiveWork w;
    CHECK(live::liveStart(w, 6, e2e4) == live::LiveStart::Fresh);
    // Their piece in hand, taken for this ply: the hand goes on.
    w.held = E2;
    w.ply = 6;
    CHECK(live::liveStart(w, 6, e2e4) == live::LiveStart::Held);
    w.busy = true;  // still reaching for it
    CHECK(live::liveStart(w, 6, e2e4) == live::LiveStart::Held);
    // ... but not while another piece is still going back first, nor for another piece or ply.
    w.before = true;
    CHECK(live::liveStart(w, 6, e2e4) == live::LiveStart::Cut);
    w.before = false;
    CHECK(live::liveStart(w, 6, d7d5) == live::LiveStart::Cut);
    CHECK(live::liveStart(w, 8, e2e4) == live::LiveStart::Cut);
    // The move put down: only the press is left; another move put down is dropped.
    w.placed = e2e4;
    CHECK(live::liveStart(w, 6, e2e4) == live::LiveStart::Placed);
    CHECK(live::liveStart(w, 6, net::packMove(E2, 20, 0)) == live::LiveStart::Cut);
    w.takeBack = true;  // their gestures had left it: it was being taken back
    CHECK(live::liveStart(w, 6, e2e4) == live::LiveStart::Cut);
    // Nothing held any more, but a piece is still going back.
    live::LiveWork back;
    back.busy = true;
    CHECK(live::liveStart(back, 6, e2e4) == live::LiveStart::Cut);
}

namespace {
// The opponent's robot as GameScene drives it (updateRemoteLive, startRemoteMove), with the task
// durations of the animator: the queue of its hand and the piece it holds live.
struct RobotHand {
    float t = 0.0f, busy = 0.0f, liveEnd = 0.0f, reachAt = 0.0f, worst = 0.0f;
    int held = live::kNoSquare, ply = -1;
    void enqueue(float seconds) {
        busy += seconds;
        worst = std::max(worst, busy);
    }
    void enqueueLive(float seconds) {
        enqueue(seconds);
        liveEnd = t + busy;
    }
    // One frame with their latest gesture (no aim: no carry).
    void frame(const live::PieceIntent& in, int plies, float dt) {
        live::HandStep s = live::handStep(held, in, busy);
        if (s.letGo) {
            enqueueLive(anim::Timing::Place + (s.take ? 0.0f : anim::Timing::Retract));
            held = live::kNoSquare;
        }
        if (s.take) {
            reachAt = t + busy;
            enqueueLive(anim::Timing::Reach + anim::Timing::Lift);
            held = in.touch;
            ply = plies;
        }
        t += dt;
        busy = std::max(0.0f, busy - dt);
    }
    // Their MoveMade: the seconds until its pieces are down (my turn begins).
    float moveMade(int plies, uint16_t move) {
        live::LiveWork w;
        w.held = held;
        w.ply = ply;
        w.before = held != live::kNoSquare && reachAt > t + live::kHandSlack;
        w.busy = liveEnd > t;
        live::LiveStart start = live::liveStart(w, plies, move);
        if (start == live::LiveStart::Cut) busy = 0.0f;
        if (start != live::LiveStart::Held) enqueue(anim::Timing::Reach + anim::Timing::Lift);
        enqueue(anim::Timing::Carry + anim::Timing::Place);
        return busy;
    }
};

// Their gestures at 'rate' per second for 20 s, touching their pieces one after the other (a
// modified client), then 'settle' seconds touching the piece they move, then their MoveMade.
float floodedMove(float rate, float settle, float* worst) {
    chess::Position pos;
    std::vector<int> theirs;
    for (int sq = 0; sq < 16; ++sq) theirs.push_back(sq);
    const uint16_t move = net::packMove(6, 21, 0);  // Ng1-f3
    RobotHand robot;
    net::Gesture latest = handGesture(0, live::kNoSquare);
    const float dt = 1.0f / 60.0f, flood = 20.0f;
    int sent = 0;
    while (robot.t < flood + settle) {
        if (robot.t >= float(sent) / rate) {
            int touch = robot.t < flood ? theirs[size_t(sent) % theirs.size()] : int(net::moveFrom(move));
            latest = handGesture(0, touch);
            ++sent;
        }
        robot.frame(live::pieceIntent(latest, pos, White), 0, dt);
    }
    *worst = robot.worst;
    return robot.moveMade(0, move);
}
}  // namespace

TEST(live_gesture_flood_never_delays_the_opponents_move) {
    // The time a move normally takes from their MoveMade to its pieces being down.
    const float normal = anim::Timing::Reach + anim::Timing::Lift + anim::Timing::Carry + anim::Timing::Place;
    // The longest step: a piece put back on its way to another (or retracting), then the next one
    // taken.
    const float step = anim::Timing::Place + anim::Timing::Retract + anim::Timing::Reach + anim::Timing::Lift + live::kHandSlack;
    // The server relays 4 gestures a second, a direct match 10; the move comes at once, or once
    // the robot had time to take the piece they move.
    for (float rate : {4.0f, 10.0f}) {
        for (float settle : {0.0f, 0.2f, 0.45f, 0.7f, 1.5f}) {
            float worst = 0.0f;
            float delay = floodedMove(rate, settle, &worst);
            CHECK(worst <= step + 1e-4f);
            CHECK(delay <= normal + 1e-4f);
        }
    }
}

TEST(live_head_and_hold_timeouts) {
    CHECK(live::headActive(0.0f, true, false, false, false));
    CHECK(live::headActive(2.4f, true, false, false, false));
    CHECK(!live::headActive(live::kHeadTimeout, true, false, false, false));
    CHECK(!live::headActive(0.0f, false, false, false, false));  // stale
    CHECK(!live::headActive(0.0f, true, true, false, false));    // the option ignores it
    CHECK(!live::headActive(0.0f, true, false, true, false));    // the opponent is away
    CHECK(!live::headActive(0.0f, true, false, false, true));    // we are reconnecting
    CHECK(!live::holdExpired(4.9f));
    CHECK(live::holdExpired(live::kHoldTimeout));
    // A keepalive a second never lets a held piece go.
    CHECK(!live::holdExpired(float(live::kKeepaliveMs) / 1000.0f + 1.0f));
}

TEST(live_head_spring_follows_without_overshoot) {
    live::HeadSpring s;
    s.snap(0.1f, -0.2f);
    CHECK(near(s.yaw(), 0.1f));
    CHECK(near(s.pitch(), -0.2f));
    s.update(0.8f, -0.6f, 0.0f);  // no time, no move
    CHECK(near(s.yaw(), 0.1f));
    float prevYaw = s.yaw(), prevPitch = s.pitch();
    bool monotonic = true;
    for (int i = 0; i < 60; ++i) {
        s.update(0.8f, -0.6f, 1.0f / 60.0f);
        monotonic = monotonic && s.yaw() >= prevYaw - 1e-6f && s.yaw() <= 0.8f + 1e-4f && s.pitch() <= prevPitch + 1e-6f &&
                    s.pitch() >= -0.6f - 1e-4f;
        prevYaw = s.yaw();
        prevPitch = s.pitch();
    }
    CHECK(monotonic);
    CHECK(near(s.yaw(), 0.8f, 0.01f));
    CHECK(near(s.pitch(), -0.6f, 0.01f));
    // A long frame is sub-stepped: still stable.
    s.update(-0.5f, 0.2f, 0.1f);
    CHECK(s.yaw() < 0.8f && s.yaw() > -0.5f);
    for (int i = 0; i < 20; ++i) s.update(-0.5f, 0.2f, 0.1f);
    CHECK(near(s.yaw(), -0.5f, 1e-3f));
    CHECK(near(s.pitch(), 0.2f, 1e-3f));
}

// ---- My clock and my move on its way ---------------------------------------------------------

TEST(live_clock_freeze_lasts_until_a_timeout) {
    CHECK(live::clockFreezeHolds(0.0, -1, false));
    CHECK(live::clockFreezeHolds(999.0, -1, false));
    CHECK(!live::clockFreezeHolds(1000.0, -1, false));
    CHECK(!live::clockFreezeHolds(1000.0, 100, false));  // 3 pings under a second
    CHECK(live::clockFreezeHolds(1400.0, 500, false));   // 3 pings: 1.5 s
    CHECK(!live::clockFreezeHolds(1500.0, 500, false));
    CHECK(!live::clockFreezeHolds(10.0, 50, true));  // reconnecting: the authority's clock runs
}

TEST(live_clock_freeze_starts_again_with_a_resend) {
    live::ClockFreeze f;
    // My move at 4:58.967, the connection lost right after: the authority's running clock shows.
    f.start(11267.0, 298967);
    CHECK(f.holds(11500.0, 40, false));
    CHECK_EQ(f.shownMs, int64_t(298967));
    CHECK(!f.holds(11500.0, 40, true));
    // Back 7.5 s later, the move sent again: the display stands still at the authority's time
    // then, which counted the outage, not at the time it showed before it.
    f.start(18817.0, 291417);
    CHECK(f.holds(18900.0, 40, false));
    CHECK_EQ(f.shownMs, int64_t(291417));
    CHECK(!f.holds(18817.0 + 1000.0, 40, false));
}

TEST(live_pending_move_resent_after_a_reconnection) {
    std::vector<uint16_t> local = {net::packMove(E2, E4, 0), net::packMove(E7, E5, 0), net::packMove(6, 21, 0)};
    std::vector<uint16_t> authority(local.begin(), local.end() - 1);
    // White's third ply never reached the authority.
    CHECK(live::resendPendingMove(local, authority, 2, 0, true));
    CHECK(!live::resendPendingMove(local, authority, 2, 1, true));   // not my turn there
    CHECK(!live::resendPendingMove(local, authority, 2, 0, false));  // the game is over
    CHECK(!live::resendPendingMove(local, authority, -1, 0, true));  // nothing pending
    CHECK(!live::resendPendingMove(local, authority, 1, 0, true));
    CHECK(!live::resendPendingMove(local, local, 2, 0, true));  // it has it
    std::vector<uint16_t> other = {net::packMove(E2, E4, 0), net::packMove(D7, D5, 0)};
    CHECK(!live::resendPendingMove(local, other, 2, 0, true));  // another game
    std::vector<uint16_t> shorter(local.begin(), local.begin() + 1);
    CHECK(!live::resendPendingMove(local, shorter, 2, 0, true));  // moves missed: a rebuild
}

// ---- RatingRestored --------------------------------------------------------------------------

TEST(live_rating_restored_waits_for_the_end_of_the_game) {
    live::HeldNotice n;
    double points = -1.0;
    CHECK(!n.take(false, points));
    // Out of a game: at once.
    n.add(6.0);
    CHECK(n.take(false, points));
    CHECK(near(float(points), 6.0f));
    CHECK(!n.pending());
    // During a game: kept, and the points add up.
    n.add(12.5);
    CHECK(!n.take(true, points));
    n.add(7.5);
    CHECK(n.pending());
    CHECK(!n.take(true, points));
    CHECK(n.take(false, points));
    CHECK(near(float(points), 20.0f));
    CHECK(!n.take(false, points));
}

// ---- Realtime errors -------------------------------------------------------------------------

TEST(live_game_errors_leave_the_menus_refusals_out) {
    for (int code = 100; code <= 112; ++code)
        if (code != 106 && code != 107) CHECK(live::gameError(code));
    // AlreadyInGame and InvalidCategory answer a QueueJoin or a challenge (no game): the menus show them.
    CHECK(!live::gameError(106));
    CHECK(!live::gameError(107));
    CHECK(!live::gameError(0));
    CHECK(!live::gameError(11));
    CHECK(!live::gameError(113));
    CHECK(!live::gameError(201));
    CHECK(!live::gameError(207));
}

TEST(live_challenge_refusals_are_only_those_of_its_creation) {
    for (int code : {202, 203, 204, 206, 208, 210}) CHECK(live::challengeRefused(code));
    // QueueNotAllowed and MatchmakingCooldown answer a QueueJoin, ChallengeNotFound an accept, a
    // decline or a cancel, CodeInvalid a code joined, RematchUnavailable a rematch.
    for (int code : {200, 201, 205, 207, 209}) CHECK(!live::challengeRefused(code));
    CHECK(!live::challengeRefused(0));
    CHECK(!live::challengeRefused(106));
}

TEST(live_queue_refusals_end_the_search) {
    // QueueNotAllowed, MatchmakingCooldown, AlreadyInGame and InvalidCategory refuse a QueueJoin.
    for (int code : {200, 207, 106, 107}) CHECK(live::queueRefused(code));
    // The refusals of a challenge, a code joined, a rematch and the game errors leave it.
    for (int code : {0, 100, 105, 108, 201, 202, 203, 204, 205, 206, 208, 209, 210}) CHECK(!live::queueRefused(code));
}

TEST(live_routing_follows_the_game_a_message_names) {
    using K = net::Event::Kind;
    net::Event e;
    e.kind = K::MoveMade;
    e.gameId = 7;
    e.game.id = 7;
    CHECK_EQ(live::eventGameId(e), uint64_t(7));
    // A late message of game 7 while the rematch 8 is shown: game 7's.
    e.game.id = 8;
    CHECK_EQ(live::eventGameId(e), uint64_t(7));
    // No game named: the one its snapshot carries.
    e.gameId = 0;
    CHECK_EQ(live::eventGameId(e), uint64_t(8));

    // A RatingUpdate of the game shown changes the account in place, in its queue's category or
    // else (a challenge) the game's; one of an earlier game does not.
    net::Event r;
    r.kind = K::RatingUpdate;
    r.gameId = 8;
    r.game.id = 8;
    r.game.category = "5+3";
    CHECK(live::ratesShownGame(r));
    CHECK_EQ(live::ratingCategory(r), std::string("5+3"));
    r.queueCategory = "3+2";
    CHECK_EQ(live::ratingCategory(r), std::string("3+2"));
    r.gameId = 7;
    CHECK(!live::ratesShownGame(r));
}

// The scene's own guard, should a late message of an earlier game ever be routed to it: its
// GameEvent (a rematch refused or offered), GameEnd and RatingUpdate change nothing in game 8.
TEST(live_scene_ignores_another_games_events) {
    using K = net::Event::Kind;
    net::Event e;
    e.game.id = 8;   // what the client fills in: the game shown
    for (K k : {K::GameEvent, K::GameEnd, K::RatingUpdate}) {
        e.kind = k;
        e.gameId = 7;
        CHECK(live::aboutAnotherGame(e, 8));
        e.gameId = 8;
        CHECK(!live::aboutAnotherGame(e, 8));
        e.gameId = 0;   // no game named: the one played
        CHECK(!live::aboutAnotherGame(e, 8));
    }
    // Only those three: the other kinds are left to the session's routing.
    for (K k : {K::GameSnapshot, K::MoveMade, K::OpponentGesture, K::ServerError}) {
        e.kind = k;
        e.gameId = 7;
        CHECK(!live::aboutAnotherGame(e, 8));
    }
}

TEST(live_test_answer_comes_from_the_tested_server) {
    net::Event e;
    e.kind = net::Event::Kind::ServerInfoResult;
    e.origin = "other.test:443";
    CHECK(live::testAnswer(e, true, "other.test:443"));
    // The info of the server in use (asked before the test), or no test going on.
    CHECK(!live::testAnswer(e, true, "play.example:443"));
    CHECK(!live::testAnswer(e, false, "other.test:443"));
    e.kind = net::Event::Kind::AccountResult;
    CHECK(!live::testAnswer(e, true, "other.test:443"));
}

// Options' pin field emptied for the custom server applied: Apply forgets the pin saved at sign-in,
// and "Test connection" goes without it (one rule for both, so that the test tells what Apply gives).
TEST(live_saved_pin_dropped_with_the_pin_field) {
    const std::string pin(64, 'a');
    net::ServerEndpoint applied;
    applied.host = "chess.example.org";
    applied.apiPort = 8443;
    applied.pinnedSha256 = pin;
    net::ServerEndpoint ep = applied;
    ep.pinnedSha256.clear();
    CHECK(live::savedPinDropped(ep, applied, true));
    ep.host = "CHESS.example.org";              // the same origin
    CHECK(live::savedPinDropped(ep, applied, true));
    // The official server chosen, a pin still given, another server, or none applied with a pin:
    // the saved pin stays.
    CHECK(!live::savedPinDropped(ep, applied, false));
    ep.pinnedSha256 = std::string(64, 'b');
    CHECK(!live::savedPinDropped(ep, applied, true));
    ep.pinnedSha256.clear();
    ep.apiPort = 443;
    CHECK(!live::savedPinDropped(ep, applied, true));
    ep.apiPort = 8443;
    applied.pinnedSha256.clear();
    CHECK(!live::savedPinDropped(ep, applied, true));
}
