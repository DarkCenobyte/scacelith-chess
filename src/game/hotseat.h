// Hot-seat: two people play on one PC, in turn, each from their own robot's eyes
// (docs/MULTIPLAYER_PLAN.md). These are the engine-free parts, compiled into the core library and
// unit-tested (tests/hotseat_tests.cpp): who has the controls, which hand plays, the rematch swap,
// the clock stepping with the handover freeze, the handover itself (a camera flight, or a cut
// through black for players who dislike camera motion) and the latch on the buttons still held by
// the previous player. GameScene (game_scene.cpp) drives them.
#pragma once
#include "../chess/chess.h"
#include "camera_flight.h"
#include <string>

namespace game {
namespace hotseat {

// The seat whose player uses the mouse and the keyboard: the human in a game against Stockfish or
// online, the seat to move in a hot-seat game.
inline int inputSeat(bool hotSeat, int humanSeat, int seatToMove) { return hotSeat ? (seatToMove & 1) : (humanSeat & 1); }

// Seat 0 (White) sits at +Z facing -Z, so its right is +X; Black's right is -X.
inline bool clockOnPositiveX(int clockRightOfSeat) { return (clockRightOfSeat & 1) == 0; }
// The seat whose clock stands on its left plays (moves and presses the clock) with its left hand,
// and writes with its right one.
inline bool playsLeftHanded(int seat, bool clockOnPositiveX) { return ((seat & 1) == 0) != clockOnPositiveX; }

// The two players of a hot-seat game, by colour (0 White, 1 Black), as the New Game page set them.
struct Players {
    std::string names[2];
    int hands[2] = {0, 1};  // handwriting of each (ui::font::HandStyle)
    int clockRightOf = 0;   // colour at whose right the clock stands (FIDE 6.5: the arbiter decides)
    bool rated = false;     // rated between the two names (local ratings, not the rating against Stockfish)
    // The rematch: colours swapped. The clock follows its player, so each keeps their hand.
    Players swapped() const;
};

// Advances the running clock by dt (seconds), carrying the fractional milliseconds in 'accumMs'.
// Frozen (the handover), nothing is counted and the accumulator is not fed: the running side's
// time and its delay window (Bronstein / US delay) wait, so the delay starts when the view lands.
void advanceClock(chess::Clock& clock, double& accumMs, float dt, bool frozen);

// Buttons still held by the previous player when the view left them are ignored until released:
// arm() at the clock press, then blocked(anyHeld) once per frame after the landing.
class InputGate {
public:
    void arm() { armed_ = true; }
    void reset() { armed_ = false; }
    bool armed() const { return armed_; }
    // True while the input must be ignored: armed and something is still held.
    bool blocked(bool anyHeld) {
        if (armed_ && !anyHeld) armed_ = false;
        return armed_;
    }

private:
    bool armed_ = false;
};

// The handover from the mover's eyes to the next player's, while the clock is frozen.
//   Flight: CameraFlight with handoverShape() (rise, arc over the board looking down at it, settle
//           into the other head); the end pose follows the next player's eyes (a moving head).
//   Cut:    the mover's view fades to black, the view changes seats, then fades in.
class Handover {
public:
    enum class Phase { Idle, Flight, FadeOut, FadeIn };
    static constexpr float kFadeOut = 0.18f, kFadeIn = 0.32f;
    static constexpr float kMinFlight = 0.8f, kMaxFlight = 2.0f;

    // duration > 0: a flight of that length (clamped to kMinFlight..kMaxFlight) from 'fromPose'
    // (the mover's view) to 'toPose' (the next player's), arching over 'centre'; <= 0: a cut.
    void start(int fromSeat, int toSeat, float duration, const CameraPose& fromPose, const CameraPose& toPose,
               m::vec3 centre);
    struct Step {
        bool cut = false;     // a cut is at black: the view changes seats now
        bool landed = false;  // over: the next player has the view and the controls
    };
    // One frame; 'toPose' is the next player's view now.
    Step update(float dt, const CameraPose& toPose);
    void cancel() { phase_ = Phase::Idle; }

    bool active() const { return phase_ != Phase::Idle; }
    bool flying() const { return phase_ == Phase::Flight; }
    Phase phase() const { return phase_; }
    int fromSeat() const { return from_; }
    int toSeat() const { return to_; }
    // The seat whose eyes are the view: the mover until the cut, then the next player; -1 during
    // a flight (pose()).
    int viewSeat() const;
    const CameraPose& pose() const { return pose_; }
    float fade() const;              // 0 clear .. 1 black (a cut)
    float elapsed() const { return elapsed_; }
    float totalTime() const;         // the whole handover (the time the clock stays frozen)
    float progress() const;          // 0..1

private:
    Phase phase_ = Phase::Idle;
    int from_ = 0, to_ = 1;
    CameraFlight flight_;
    CameraPose pose_;
    float phaseTime_ = 0.0f, elapsed_ = 0.0f, flightDuration_ = 0.0f;
};

}  // namespace hotseat
}  // namespace game
