// The live side of online play (src/game/online_live.h): my gesture (the aim's dwell, the ply of
// the state's start, the flags, when a gesture is worth sending, the Side test of the look), the
// opponent's gestures (when their piece fields apply, which squares and moves are valid, the
// timeouts, the head's spring), my clock's freeze and the resend of my move after a
// reconnection, and the RatingRestored notice held back during a game.
#include "test.h"
#include "chess/chess.h"
#include "game/layout.h"
#include "game/online_live.h"
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
