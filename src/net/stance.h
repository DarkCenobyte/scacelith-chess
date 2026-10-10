// When a client sends its player's stance (protocol minor 2, the Stance message; net/gesture.h
// has the rules): the engine-free part shared by OnlineClient (C_Stance to the server) and
// DirectMatch (C_Stance from the guest, S_Stance from the host), each keeping one StanceSender on
// its network thread, and unit-tested (tests/net_stance_tests.cpp).
//   - The game thread hands it its player's latest stance and the game it is in (set(); the scene
//     may call every frame: only a change does anything).
//   - The receiver starts every game, and every link, with the player seated (game/online_live.h,
//     StanceTracker): a stance other than Seated goes at once (a change), and again at least every
//     keepalive while it lasts (the refresh that keeps it from expiring on the receiver's side
//     after kStanceExpiryKeepalives keepalives). That is whatever Welcome.gestureRate says: the
//     keepalive is net::gestureKeepaliveMs of Welcome.gestureIdleMs, 1 s when it announces 0.
//   - A return to Seated goes once (no refresh: Seated is what a receiver falls back to anyway).
//   - Two messages are at least kStanceMinIntervalMs apart: a player who taps the arrow keys
//     sends the latest stance a quarter of a second later, never a burst (C_Stance counts towards
//     the server's message limit like any message, and so does it towards a direct-match host's).
//   - linkUp(): a new link (a Welcome; for a direct-match host, the guest's Hello): the other side
//     shows the player seated until told otherwise, so a stance other than Seated goes again.
//   - Whether the link may carry a Stance at all (online, a negotiated minor of at least
//     kStanceMinMinor, the game named still the one in progress there) is the caller's to say:
//     due() is asked only when it may, and nothing is kept back for later otherwise (the next
//     due() after the link allows it again sends the current stance if needed).
#pragma once
#include "gesture.h"
#include "protocol_gen.h"
#include <cmath>
#include <cstdint>

namespace net {

constexpr int kStanceMinIntervalMs = 250;   // between two Stance messages of a client

class StanceSender {
public:
    // The player's stance in game 'game' (net::proto::Stance values, those of anim::Stance; a
    // value this codec cannot send counts as Seated). The receiver of a game nothing was sent for
    // yet shows the player seated, as at the start of every game.
    void set(uint64_t game, int stance) {
        game_ = game;
        stance_ = stance >= 0 && stance <= 255 && proto::isValid(proto::Stance(stance)) ? uint8_t(stance) : kSeated;
    }
    // A new link: its receiver shows the player seated.
    void linkUp() {
        heard_ = kSeated;
        sentAt_ = -HUGE_VAL;
    }
    // Whether the current stance should go now (the link carrying it, see above), at 'nowMs' of a
    // monotonic clock, with the keepalive of the link (ms, clamped by gestureKeepaliveMs).
    bool due(double nowMs, int keepaliveMs) const { return nowMs >= nextAtMs(keepaliveMs); }
    // When due() becomes true if nothing changes (HUGE_VAL: never; earlier than now: now).
    double nextAtMs(int keepaliveMs) const {
        if (game_ == 0) return HUGE_VAL;
        if (stance_ != heard()) return sentAt_ + kStanceMinIntervalMs;
        if (stance_ != kSeated) return sentAt_ + gestureKeepaliveMs(keepaliveMs);
        return HUGE_VAL;
    }
    // The current stance went at 'nowMs'.
    void sent(double nowMs) {
        heardGame_ = game_;
        heard_ = stance_;
        sentAt_ = nowMs;
    }
    uint64_t game() const { return game_; }
    uint8_t stance() const { return stance_; }   // the value to send (proto::Stance)

private:
    static constexpr uint8_t kSeated = uint8_t(proto::Stance::Seated);
    uint64_t game_ = 0;
    uint8_t stance_ = kSeated;
    // What the receiver shows: the last stance sent (in heardGame_; Seated on a new link), and
    // Seated for any other game.
    uint64_t heardGame_ = 0;
    uint8_t heard_ = kSeated;
    double sentAt_ = -HUGE_VAL;       // when the last one went (whatever its game)
    uint8_t heard() const { return game_ == heardGame_ ? heard_ : kSeated; }
};

}  // namespace net
