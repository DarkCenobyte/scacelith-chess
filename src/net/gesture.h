// Live gestures of a player in an online game (server or direct match): the protocol's Gesture
// message (dedicated-server/src/protocol/schema.js). Cosmetic only: the opponent's robot mirrors
// them (head, the piece in hand, where it is aimed, the move placed before the clock press), but
// they never change the game, the clocks or the board state. The whole state travels every time,
// so a message that is lost or rate-limited heals with the next one.
#pragma once
#include <cmath>
#include <cstdint>

namespace net {

struct Gesture {
    static constexpr int kNoSquare = 64;
    int ply = 0;                // plies played when it was sent (the receiver drops stale ones)
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
    long v = std::lround(double(rad) * 1000.0);
    return int32_t(v < -3142 ? -3142 : v > 3142 ? 3142 : v);
}
inline int32_t gesturePitchToWire(float rad) {
    long v = std::lround(double(rad) * 1000.0);
    return int32_t(v < -1571 ? -1571 : v > 1571 ? 1571 : v);
}
inline uint8_t gestureLeanToWire(float lean) {
    long v = std::lround(double(lean) * 100.0);
    return uint8_t(v < 0 ? 0 : v > 100 ? 100 : v);
}
inline float gestureAngleFromWire(int32_t mrad) { return float(mrad) * 0.001f; }
inline float gestureLeanFromWire(uint8_t pct) { return float(pct) * 0.01f; }

}  // namespace net
