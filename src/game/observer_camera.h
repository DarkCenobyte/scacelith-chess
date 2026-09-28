// The viewer mode's observer: an invisible free-flying camera (no body, never drawn, the players
// do not react to it) kept inside the hall and above the floor. Engine-free (only reads the
// plat::Input struct), compiled into the core library and unit-tested.
//
// Controls (the same keys work on QWERTY and AZERTY keyboards):
//   W / Z / Up       forward (along the view)      S / Down      back
//   A / Q / Left     left                          D / Right     right
//   E / Space / PgUp up                            C / Ctrl / PgDn  down
//   Shift            3x faster                     mouse wheel   move speed
//   right mouse button held: look around (as in the game)
// Viewpoint presets are reached with flyTo(); any movement key or mouse look takes over at once.
#pragma once
#include "../platform/platform.h"
#include "camera_flight.h"

namespace game {

class ObserverCamera {
public:
    // One frame of player intent, in camera space.
    struct Controls {
        m::vec3 move{0, 0, 0};  // x right, y up (world), z forward; each -1..1
        float lookX = 0.0f, lookY = 0.0f;  // mouse motion to apply (pixels)
        float wheel = 0.0f;     // notches
        bool fast = false;
        bool any() const;       // something asks the camera to move
    };
    // Reads the keys listed above. keys = keyboard not used by a menu; looking = the mouse look
    // button is held (motion is applied only then).
    static Controls read(const plat::Input& in, bool keys, bool looking);

    ObserverCamera();
    // The camera stays inside this box (default: the hall with a margin, above the floor).
    void setBounds(m::vec3 lo, m::vec3 hi);
    void setPose(const CameraPose& p);                     // jump
    void flyTo(const CameraPose& p, float duration = 0.0f, const FlightShape& shape = {});
    // A flight that is never cancelled by the controls and whose target may move (retarget each
    // frame with retarget()); used to follow a player's eyes.
    void retarget(const CameraPose& p);
    bool flying() const { return flight_.active(); }
    const CameraFlight& flight() const { return flight_; }

    // Applies one frame. sensitivity scales the mouse look (Settings::mouseSensitivity).
    void update(float dt, const Controls& c, float sensitivity = 1.0f, bool invertY = false);

    const CameraPose& pose() const { return pose_; }
    float speed() const { return speed_; }                 // metres per second (without Shift)
    void setSpeed(float metresPerSecond);

    static constexpr float kMinSpeed = 0.15f, kMaxSpeed = 8.0f, kDefaultSpeed = 1.2f;

private:
    CameraPose pose_;
    CameraFlight flight_;
    m::vec3 velocity_{0, 0, 0};
    m::vec3 lo_, hi_;
    float speed_ = kDefaultSpeed;
    void clampToBounds();
};

}  // namespace game
