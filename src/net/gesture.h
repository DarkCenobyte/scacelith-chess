// Live gestures of a player in an online game (server or direct match): the protocol's Gesture
// message (dedicated-server/src/protocol/schema.js). Cosmetic only: the opponent's robot mirrors
// them (head, the piece in hand, where it is aimed, the move placed before the clock press), but
// they never change the game, the clocks or the board state. The whole state travels every time,
// so a message that is lost or rate-limited heals with the next one.
//
// What a client sends (the online mock opponent of src/game/online_mock.cpp follows the same
// rules), as the state of its own player:
//   - idle (nothing in hand, including the whole opponent's turn): touch = aim = 64, placed = 0;
//   - a piece in hand (touched, lifted, carried): touch = its square; aim = the legal destination
//     under the pointer, else 64;
//   - the promotion piece being chosen (the pawn on the last rank, the picker open): touch =
//     from, aim = to, flags |= Promoting;
//   - the destination chosen and the move on its way to the board, the clock not pressed yet:
//     placed = packMove(from, to, promo), touch = from, aim = to;
//   - once the move is confirmed (or pressed, when the robots do not press the clock by
//     themselves): idle again.
//   ply is the number of plies played when the current one of these states began: while a move
//   is being prepared it is the ply of that move. A change of the head alone keeps it.
//   The head, in every state: yaw and pitch of the look relative to the seat (0 = straight
//   ahead and level, yaw > 0 to the left, pitch < 0 down), lean 0..1 (the mouse wheel lean
//   towards the board). flags: Glance while the player looks at their own scoresheet; Side when
//   the look falls on the table beside the board (the clock, the captured pieces or the
//   scoresheet, Glance included). Each client puts the clock at its own player's right, so what
//   lies beside the board is mirrored between the two clients: the receiver negates the yaw of
//   a Side look (or aims the head at its own copy of the thing looked at).
// A client sends a Gesture when this state changes, and when the head moved noticeably; the
// network layer keeps only the latest one and paces them (OnlineClient::sendGesture,
// DirectMatch::sendGesture), so calling it every frame costs nothing.
//
// What a receiver does with the OpponentGesture events: the latest one wins. The head (yaw,
// pitch, lean, Glance, Side) always applies. touch, aim, placed and Promoting describe a move
// being prepared: they apply only while the local game has exactly 'ply' plies and it is the
// sender's turn (a gesture of an earlier ply is stale: the move it prepared is already known),
// and only to the sender's own pieces and legal destinations. A move 'placed' is shown on the
// board until the MoveMade that confirms it (or a newer gesture that takes it back).
#pragma once
#include <cmath>
#include <cstdint>

namespace net {

struct Gesture {
    static constexpr int kNoSquare = 64;
    int ply = 0;                // plies played when the current state began (see above)
    int touch = kNoSquare;      // square of the piece in hand (touched, lifted or carried)
    int aim = kNoSquare;        // square the piece in hand is aimed at
    uint16_t placed = 0;        // move put on the board and not pressed yet (packMove), 0 = none
    uint8_t flags = 0;          // net::proto::GestureFlag bits: Glance, Promoting, Side
    float yaw = 0.0f;           // seat-relative look, radians: 0 = straight ahead, > 0 = to the left
    float pitch = 0.0f;         // radians: 0 = level, < 0 = down
    float lean = 0.0f;          // 0..1 (mouse wheel lean towards the board)

    bool sameState(const Gesture& o) const {
        return ply == o.ply && touch == o.touch && aim == o.aim && placed == o.placed && flags == o.flags;
    }
};

// Wire units (Gesture.yaw / pitch in milliradians, lean in percent), clamped to the schema bounds.
inline int32_t gestureYawToWire(float rad) {
    if (!std::isfinite(rad)) return 0;
    long v = std::lround(double(rad) * 1000.0);
    return int32_t(v < -3142 ? -3142 : v > 3142 ? 3142 : v);
}
inline int32_t gesturePitchToWire(float rad) {
    if (!std::isfinite(rad)) return 0;
    long v = std::lround(double(rad) * 1000.0);
    return int32_t(v < -1571 ? -1571 : v > 1571 ? 1571 : v);
}
inline uint8_t gestureLeanToWire(float lean) {
    if (!std::isfinite(lean)) return 0;
    long v = std::lround(double(lean) * 100.0);
    return uint8_t(v < 0 ? 0 : v > 100 ? 100 : v);
}
inline float gestureAngleFromWire(int32_t mrad) { return float(mrad) * 0.001f; }
inline float gestureLeanFromWire(uint8_t pct) { return float(pct) * 0.01f; }

// A Gesture into the fields of a C_Gesture or S_Gesture (protocol_gen.h; the caller sets 'game'
// and, for C_Gesture, the seq): out-of-range values are brought within the schema's bounds, so
// the message is always valid.
template <class M> void gestureToWire(const Gesture& g, M& m) {
    auto square = [](int s) { return uint8_t(s >= 0 && s < Gesture::kNoSquare ? s : Gesture::kNoSquare); };
    m.ply = uint16_t(g.ply < 0 ? 0 : g.ply > 1199 ? 1199 : g.ply);
    m.touch = square(g.touch);
    m.aim = square(g.aim);
    m.placed = uint16_t(g.placed & 0x7fff);
    m.flags = uint8_t(g.flags & 7);
    m.yaw = gestureYawToWire(g.yaw);
    m.pitch = gesturePitchToWire(g.pitch);
    m.lean = gestureLeanToWire(g.lean);
}

// The Gesture of a decoded C_Gesture or S_Gesture.
template <class M> Gesture gestureFromWire(const M& m) {
    Gesture g;
    g.ply = m.ply;
    g.touch = m.touch;
    g.aim = m.aim;
    g.placed = m.placed;
    g.flags = m.flags;
    g.yaw = gestureAngleFromWire(m.yaw);
    g.pitch = gestureAngleFromWire(m.pitch);
    g.lean = gestureLeanFromWire(m.lean);
    return g;
}

// A token bucket for Gesture messages: refilled at 'rate' messages per second, holding at most
// 'capacity' (full after reset). Time in milliseconds of a monotonic clock. rate 0: none may go.
class GestureBucket {
public:
    void reset(double nowMs, int rate, int capacity) {
        rate_ = rate > 0 ? double(rate) : 0.0;
        cap_ = capacity > 1 ? double(capacity) : 1.0;
        tokens_ = cap_;
        at_ = nowMs;
    }
    bool enabled() const { return rate_ > 0.0; }
    // True when one message may go now; it is then counted.
    bool take(double nowMs) {
        if (!enabled()) return false;
        tokens_ = tokensAt(nowMs);
        at_ = std::fmax(at_, nowMs);
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }
    // When the next message may go (nowMs when it may go at once, or when none ever may).
    double readyAtMs(double nowMs) const {
        double t = tokensAt(nowMs);
        return !enabled() || t >= 1.0 ? nowMs : nowMs + (1.0 - t) * 1000.0 / rate_;
    }

private:
    double rate_ = 0.0, cap_ = 1.0, tokens_ = 0.0, at_ = 0.0;
    double tokensAt(double nowMs) const {
        return nowMs > at_ ? std::fmin(cap_, tokens_ + (nowMs - at_) * rate_ / 1000.0) : tokens_;
    }
};

// The capacity a sender paces its Gestures with, for a receiver whose bucket is (gestureRate,
// gestureBurst) (Welcome): one message less than the burst, so that a message the network delayed
// after a full burst still finds a token at the receiver, which then never has to drop one.
inline int gestureSendCapacity(int burst) { return burst > 1 ? burst - 1 : 1; }

}  // namespace net
