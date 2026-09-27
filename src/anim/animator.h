// Character animation: procedural posing of a seated robot (IK arms, finger poses, head/eye
// look-at, blinks, idle life) driven by queued hand tasks with FIXED durations.
// Implemented by the animation work package.
//
// Fairness rule (game design): every task type has a fixed duration that does not depend on
// distance or on which player performs it, so both players spend exactly the same clock time on
// the same physical action. Trajectories are fast and decisive, like a player short on time.
// The game builds move animations as task sequences (see game/ for the composition) and gets
// events at the exact physical instants (piece released on its square, clock lever pressed...).
#pragma once
#include "../character/skeleton.h"
#include "../math/math.h"
#include <functional>
#include <memory>
#include <vector>

namespace anim {

// Durations in seconds (tuned by the animation package; the game only reads them).
struct Timing {
    static constexpr float Reach = 0.34f;         // rest/anywhere -> fingers closed on a piece
    static constexpr float Lift = 0.10f;          // raise the gripped piece
    static constexpr float Carry = 0.30f;         // move above the destination square
    static constexpr float Place = 0.14f;         // lower + release on the square
    static constexpr float TakeCaptured = 0.16f;  // grab the captured piece with the free fingers
    static constexpr float Discard = 0.34f;       // carry the captured piece off-board + release
    static constexpr float PressClock = 0.30f;    // anywhere -> lever pressed (event at contact)
    static constexpr float Retract = 0.35f;       // back to the resting pose
    static constexpr float Handshake = 2.60f;     // extend, clasp, 2 pumps, release, retract
};

enum class TaskType {
    Reach,          // target = piece object id: grip it (attaches at the end)
    Lift,           // raise the held piece by 'height'
    Carry,          // move the held piece above 'position' (piece base position)
    Place,          // lower the held piece to 'position' and release it (detaches)
    TakeCaptured,   // grab piece 'pieceId' (secondary attachment, held under the palm)
    Discard,        // put the secondary piece down at 'position' (piece base) and release it
    PressClock,     // press the lever at 'position' with the index/middle fingers
    Retract,        // return the hand to its resting pose
    Handshake,      // shake hands with 'partner' (both characters must receive it together)
    Wait            // hold for 'duration' (duration is the only parameter)
};

struct Task {
    TaskType type = TaskType::Wait;
    int pieceId = -1;
    m::vec3 position{0, 0, 0};
    float height = 0.0f;
    float duration = 0.0f;           // 0 = use the Timing default for the type
    class Animator* partner = nullptr;
};

enum class EventType {
    PieceGripped,      // Reach finished: pieceId now follows the hand
    PieceReleased,     // Place finished: pieceId rests at the position
    CapturedGripped,
    CapturedReleased,
    ClockPressed,      // lever contact instant (the clock switches now)
    HandshakeClasp,
    HandshakeRelease,
    TaskStarted,
    QueueEmpty
};
struct Event {
    EventType type;
    int pieceId = -1;
    m::vec3 position{0, 0, 0};
};

class Animator {
public:
    // pelvisWorld: hip joint centre in the world; facing: +1 = faces -Z (White, sitting at +Z),
    // -1 = faces +Z (Black). The robot is right-handed.
    void init(const character::Skeleton& sk, m::vec3 pelvisWorld, float facing);
    void setRestHand(m::vec3 worldPos);                  // where the right hand rests on the table
    // Game callback: world transform of a piece object (base centre at the origin, +Y up).
    std::function<m::mat4(int pieceId)> pieceTransform;
    // Game callback: piece dimensions (height, grip height, grip radius) for grasp poses.
    std::function<m::vec3(int pieceId)> pieceGripInfo;

    void enqueue(const Task& t);
    void enqueue(const std::vector<Task>& tasks);
    bool busy() const;                                   // tasks pending or running
    void clearQueue();                                   // drops pending tasks (running one finishes)
    float remainingTime() const;                         // sum of pending durations

    // Gaze: world point to look at (head + eyes, with natural limits and saccades). weight 0..1.
    void lookAt(m::vec3 target, float weight = 1.0f);
    // First-person player: head orientation comes from the camera (yaw/pitch relative to the
    // body forward, radians); the animator applies it to Neck/Head so the body matches.
    void setHeadOverride(bool enabled, float yaw = 0.0f, float pitch = 0.0f);
    void setThinking(bool thinking);                     // idle variations (chin on hand, etc.)

    void update(float dt, std::vector<Event>& events);   // advances tasks, IK, idle; appends events

    const character::Pose& pose() const { return pose_; }
    const m::mat4* globals() const { return globals_; }  // world matrix per bone (after update)
    m::mat4 eyeCameraTransform() const;                  // midpoint between the eyes, -Z = gaze
    // World transform of an attached piece (true while the hand holds it).
    bool heldPieceTransform(int pieceId, m::mat4& out) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    character::Pose pose_;
    m::mat4 globals_[character::BoneCount];
};

}  // namespace anim
