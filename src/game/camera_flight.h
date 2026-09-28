// Camera poses and smooth camera flights between them (engine-free, unit-tested).
//
// Used by the viewer's observer camera (viewpoint presets 1-9 are reached by a flight) and meant
// for the offline hot-seat mode (docs/MULTIPLAYER_PLAN.md): after a move, the camera leaves the
// mover's eyes, rises, arcs over the table and settles into the opponent's eyes (handoverShape()).
//
// A pose has no roll by construction except for first-person eye poses (the head may tilt): the
// flight interpolates yaw, pitch and roll separately, so the horizon stays level in between and the
// camera never flips over when it turns around (a quaternion slerp between two views 180 degrees
// apart would roll or pass through a look straight down).
#pragma once
#include "../math/math.h"

namespace game {

struct CameraPose {
    m::vec3 position{0, 1.2f, 1.0f};
    float yaw = 0.0f;    // about +Y; 0 = looking towards -Z, +PI/2 = looking towards -X
    float pitch = 0.0f;  // + = looking up
    float roll = 0.0f;   // about the view axis
    float fovY = 52.0f * m::DEG;

    m::quat orientation() const;
    m::vec3 forward() const;
    // Pose at 'eye' looking at 'target' (no roll).
    static CameraPose looking(m::vec3 eye, m::vec3 target, float fovY);
    // Pose from a camera orientation (-Z = view direction, +Y = up), e.g. a robot's eye transform.
    static CameraPose fromOrientation(m::vec3 position, m::quat orientation, float fovY);
};

// Shortest signed difference b - a between two angles, in (-PI, PI].
float angleDelta(float a, float b);

// Shape of a flight path: a cubic Bezier from the start to the end position whose inner control
// points are start + departure and end + arrival (zero = straight line).
struct FlightShape {
    m::vec3 departure{0, 0, 0};
    m::vec3 arrival{0, 0, 0};
    // Extra downward pitch at mid-flight (radians, sin-shaped), e.g. to look at the board while
    // flying over it.
    float pitchDip = 0.0f;
    // Direction of the yaw turn: 0 = shortest, +1 = counter-clockwise seen from above (yaw
    // increasing), -1 = clockwise. Matters for half turns.
    int yawTurn = 0;
};

class CameraFlight {
public:
    // Starts a flight; duration <= 0 picks autoDuration().
    void start(const CameraPose& from, const CameraPose& to, float duration = 0.0f, const FlightShape& shape = {});
    // Moves the end pose while flying (a head that moves): the path bends smoothly towards it.
    void retarget(const CameraPose& to);
    void cancel() { active_ = false; }
    bool active() const { return active_; }
    float progress() const;               // 0..1 (linear time)
    float duration() const { return duration_; }
    const CameraPose& target() const { return to_; }
    // Advances by dt and returns the current pose; once the flight is over it returns the end
    // pose and active() turns false.
    CameraPose update(float dt);
    // Pose at linear time u in [0,1] (the easing is applied inside).
    CameraPose sample(float u) const;

    // Flight time that feels natural for this distance and turn (0.7 .. 2.6 s).
    static float autoDuration(const CameraPose& from, const CameraPose& to);
    // Hot-seat handover between the two players' eyes: leave towards the table while rising,
    // arc over the board looking down at it, come down into the other head from the front.
    // 'centre' is the point the arc passes over (the board centre).
    static FlightShape handoverShape(const CameraPose& from, const CameraPose& to, m::vec3 centre);
    static constexpr float kHandoverDuration = 1.6f;

private:
    CameraPose from_, to_;
    FlightShape shape_;
    float duration_ = 1.0f, time_ = 0.0f;
    float yawSpan_ = 0.0f;  // total yaw change, fixed at start (turn direction)
    bool active_ = false;
};

}  // namespace game
