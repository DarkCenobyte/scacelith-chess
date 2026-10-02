// Character animation: procedural posing of a seated robot (IK arms, finger poses, head/eye
// look-at, blinks, idle life) driven by queued hand tasks with FIXED durations.
// Implemented by the animation work package (src/anim/animator.cpp, viewer: --scene anim).
//
// Fairness rule (game design): every task type has a fixed duration that does not depend on
// distance or on which player performs it, so both players spend exactly the same clock time on
// the same physical action. Trajectories are fast and decisive, like a player short on time.
// The coach's gestures (Point, Trace, Gesture) are exempt: they never take part in a timed game
// (the Coach clock is unlimited), a Trace lasts as long as its path, and the caller times every
// gesture freely (duration, notBefore, endHold) to follow the speech. Demonstration moves may use
// the move tasks with custom (slower) durations for the same reason.
// The game builds move animations as task sequences (see game/ for the composition) and gets
// events at the exact physical instants (piece released on its square, clock lever pressed...).
//
// Typical sequences (right hand):
//   quiet move : Reach(piece) Lift Carry(to) Place(to) PressClock(lever) Retract
//   capture    : Reach(piece) Lift Carry(to) TakeCaptured(victim) Place(to) Discard(spot)
//                PressClock(lever) Retract
//                (TakeCaptured right after the Carry that brought the own piece above the victim:
//                 the ring/pinky take it while thumb/index/middle keep the own piece.)
//   castling   : Reach(king) Lift Carry Place, Reach(rook) Lift Carry Place, PressClock, Retract
//   promotion  : ... Place(pawn on the last rank) is replaced by the game's own composition, e.g.
//                Reach(pawn) Lift Carry(off-board spot) Place, Reach(new piece) Lift Carry Place
// A task starts from whatever state the hand is in (even mid-air or at the chin) and still lasts
// exactly its Timing duration. Consecutive tasks that are already queued blend into one fluid
// motion (no stop between Lift and Carry, for example); queue a whole move at once.
// The hand, fingers and forearm keep clear of the other pieces (grip orientation, pre-grasp
// opening, arcs, elbow lift, resting spots). The animator learns where they stand from the
// optional obstacle callbacks (pathObstacleTop, obstacleTopNear); without them it reads
// pieceTransform for the ids 0, 1, 2... when a task starts (see pieceTransform). Diagnostics:
// SCACELITH_ANIM_DEBUG (set to any value) logs the planning, SCACELITH_ANIM_ARMTRACE (set) logs
// joint-limit clamps.
#pragma once
#include "../character/skeleton.h"
#include "../math/math.h"
#include <cstdint>
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
    // Instants inside the handshake (seconds from its start) of the two handshake events.
    static constexpr float HandshakeClaspAt = 0.92f;
    static constexpr float HandshakeReleaseAt = 1.96f;
    // Writing hand (see WriteTask). A Write lasts WriteApproach + the path + WriteRetract.
    static constexpr float PickPen = 0.70f;       // rest -> pen lifted from the table, writing grip
    static constexpr float PutPen = 0.60f;        // pen laid down on the table, hand back to rest
    static constexpr float WriteApproach = 0.30f; // rest -> pen tip on the first path key
    static constexpr float WriteRetract = 0.30f;  // last key -> rest (pen kept in hand)
    static constexpr float PageTurn = 1.40f;      // pinch the page corner, flip it over the top edge
    // Coach gestures (exempt from the fairness rule, see above).
    static constexpr float PointApproach = 0.45f;     // anywhere -> index tip on its spot (PointReached)
    static constexpr float PointHold = 1.00f;         // default hold: a Point lasts PointApproach + PointHold
    static constexpr float TraceSpeed = 0.18f;        // m/s of the fingertip along an indicated path
    static constexpr float TraceDwell = 0.12f;        // pause on the first waypoint before moving off
    static constexpr float TraceCornerPause = 0.08f;  // pause on each inner waypoint (a crisp corner)
    static constexpr float TraceSettle = 0.35f;       // hold on the last waypoint (PointReleased at the end)
    static constexpr float GestureDefault = 0.90f;    // speaking gestures (Present / Beat / Open)
};

enum class TaskType {
    Reach,          // target = piece object id: grip it (attaches at the end)
    Lift,           // raise the held piece by 'height' (0 = layout::PIECE_LIFT_HEIGHT)
    Carry,          // move the held piece above 'position' (piece base position)
    Place,          // lower the held piece to 'position' and release it (detaches)
    TakeCaptured,   // grab piece 'pieceId' (secondary attachment, held under the palm)
    Discard,        // put the secondary piece down at 'position' (piece base) and release it
    PressClock,     // press the lever at 'position' with the index/middle fingers
    Retract,        // return the hand to its resting pose
    Handshake,      // shake hands with 'partner' (both characters must receive it together)
    Wait,           // hold for 'duration' (duration is the only parameter)
    // Coach gestures (the playing hand, nothing gripped). They end holding their last pose: queue
    // the next gesture before the hold ends for a fluid chain (or cut the hold with endHold()), and
    // finish a chain with a Retract (the hand only goes idle again after a Retract). A hand that
    // holds a piece does not gesture: the task then only holds, its events still fire on time.
    Point,          // point at piece 'pieceId' (>= 0) or at the world point 'position' (a square
                    // centre...) with the index extended: the tip stops 'height' (0 = 0.045 m) above
                    // the highest piece top around the target and around itself, the finger aimed at
                    // the target (a piece: at 0.8 of its height). PointReached when it gets there
                    // (after PointApproach, or 0.6 * duration if shorter), PointReleased at the end.
    Trace,          // indicate a path: the index tip goes over 'path'[0] and follows the waypoints
                    // (world board points; a knight's L as from, corner, to: see moveTracePath) at a
                    // constant height, 'height' (0 = 0.035 m) above the highest piece top along the
                    // way, pausing on each. PointReached on the first waypoint, TraceCorner on each
                    // inner one, TraceDone on the last, PointReleased at the end.
    Gesture         // speaking gesture of shape 'shape' (see HandShape); GestureBeat at each stroke
};

// Gesture shapes (Task::shape).
enum class HandShape : uint8_t {
    Present,   // the open hand, palm up, held out towards 'position' (0 = the board centre): "look
               // at this". GestureBeat when it arrives. The eyes go to 'position' (see gazeHold).
    Beat,      // baton beats of the loosely open hand in front of the body (where a Present / Open
               // hand already is, else beside the board edge): n = max(1, round(duration / 0.45))
               // down-strokes, stroke k at (k + 0.6) * duration / n; GestureBeat at each. The gaze
               // is left to lookAt().
    Open       // the open hand, palm up, turned towards the listener at 'position' (0 = straight
               // ahead at face height): a question, an offer. GestureBeat when it arrives. The eyes
               // go to the listener.
};

struct Task {
    TaskType type = TaskType::Wait;
    int pieceId = -1;
    m::vec3 position{0, 0, 0};
    float height = 0.0f;
    float duration = 0.0f;           // 0 = use the Timing default for the type
    class Animator* partner = nullptr;
    // ---- Coach gestures
    std::vector<m::vec3> path;       // Trace: world waypoints (board points), at least one
    // Any task: it does not start before this instant of the animator clock (time()); meanwhile the
    // hand holds where the previous task left it. -1 = as soon as the previous task ends.
    float notBefore = -1.0f;
    int tag = 0;                     // echoed in every event of this task (e.g. a speech cue id)
    bool emphasis = false;           // Point: two small jabs along the finger when it arrives
    // Point / Trace / Present: seconds the eyes stay on the target once it is reached (a Trace:
    // once its path is done); then they go back to the lookAt() target (the listener's face) while
    // the hand keeps pointing. -1 = the whole task.
    float gazeHold = -1.0f;
    HandShape shape = HandShape::Present;   // Gesture
};

// Duration the animator will use for this task (Timing default unless t.duration > 0). A Trace
// lasts PointApproach + TraceDwell + its path length / TraceSpeed + TraceCornerPause per inner
// waypoint + TraceSettle; with a custom duration shorter than that, the approach, the pauses and
// the path are compressed in proportion (a longer one holds longer at the end).
float taskDuration(const Task& t);

// World waypoints for a Trace of the move from square 'from' to square 'to' (0..63, a1 = 0): the
// two square centres, or for a knight's jump the L through the corner square two steps along the
// long leg (g1-f3: g1, g3, f3).
std::vector<m::vec3> moveTracePath(int from, int to);

enum class EventType {
    PieceGripped,      // Reach finished: pieceId now follows the hand
    PieceReleased,     // Place finished: pieceId rests at the position
    CapturedGripped,
    CapturedReleased,
    ClockPressed,      // lever contact instant (the clock switches now)
    HandshakeClasp,
    HandshakeRelease,
    TaskStarted,
    QueueEmpty,
    // Writing hand
    PenPicked,         // the pen leaves the table (now follows the hand: penTransform())
    PenPut,            // the pen rests on the table again ('transform' = its resting transform)
    PenDown,           // tip touches the paper ('position' = tip; lasts until PenUp)
    PenUp,
    WritingDone,       // a Write task's path is finished (the hand retracts)
    PageGripped,       // the page corner is pinched (the page starts turning)
    PageTurned,        // the page lies flipped over the top edge
    WritingQueueEmpty,
    // Coach gestures ('position' = the target: the aimed point of a Point, the waypoint of a Trace,
    // the presented point / the listener of a Present / Open, the stroke point of a Beat)
    PointReached,      // the finger points at the target (a Trace: its tip is over the first waypoint)
    PointReleased,     // a Point / Trace ends (at the task's end, or when endHold() cuts it short); a
                       // Gesture only reports GestureBeat
    TraceCorner,       // the tip is over an inner waypoint of the path
    TraceDone,         // the tip is over the last waypoint
    GestureBeat        // the stroke of a Gesture (arrival of Present / Open, each Beat down-stroke)
};
struct Event {
    EventType type;
    int pieceId = -1;
    m::vec3 position{0, 0, 0};
    // Piece events: exact world transform of the piece at that instant (for PieceReleased /
    // CapturedReleased this is where the piece now rests: upright, base on 'position', turned about
    // the vertical by the small yaw the hand gave it while carrying; a game that wants knights to
    // face straight can drop that yaw).
    m::mat4 transform;
    float time = 0.0f;               // animator clock (seconds since init) of the physical instant
    int tag = 0;                     // Task::tag of the task (playing hand; QueueEmpty: the last task)
};

// ---- Writing hand ------------------------------------------------------------------------------
// The hand that does not play (the one on the scoresheet side) runs its own task queue, at the same
// time as the playing hand: writing never delays a move or a clock press.
// Protocol (all positions in world space):
//   game start : PickPen(frame of the pen lying beside the pad)
//   each move  : setWritingRest(start of the next row), Write(path of the move's text)
//   page full  : TurnPage(pageCorner), then write on the fresh page
//   game end   : PutPen(frame), wait for WritingQueueEmpty, then the Handshake
// Write and TurnPage expect the pen in the hand (PickPen first). PickPen and PutPen always take
// Timing::PickPen / Timing::PutPen. Event instants: PenPicked 0.36 s and PenPut 0.34 s after their
// task starts, PenDown / PenUp / WritingDone at the path key times + WriteApproach, PageGripped /
// PageTurned at 0.33 / 0.90 of the TurnPage duration. The pageCorner callback is called during the
// whole task, keep it valid.
// Left-handed player (init with Side::Left): the right hand writes, and the handshake needs it: a
// running writing task is cut short when the handshake starts (the events already due fire at their
// own instants, its remaining path / page events at once), a held pen is laid down first (at the
// frame of the next queued PutPen, which is
// then dropped, or where it was picked up; PenPut fires as usual) and the queued writing tasks
// wait for the end of the handshake. Its rest (setRestHand) is on the clock side as well.
// Idle: the writing hand never goes to the chin while it holds the pen or has work queued.
struct PenKey {
    float t = 0.0f;           // seconds from the start of the path (increasing)
    m::vec3 tip{0, 0, 0};     // pen tip, world (on the paper surface while down)
    bool down = false;        // tip touches the paper from this key to the next
};

enum class WriteTaskType {
    PickPen,    // pick up the pen lying at 'frame'
    Write,      // follow 'path' with the pen tip (pen held)
    TurnPage,   // pinch the page corner and follow it while the page flips (pen held in the palm)
    PutPen,     // lay the pen down at 'frame'
    Wait        // hold for 'duration'
};

struct WriteTask {
    WriteTaskType type = WriteTaskType::Wait;
    // Pen transform on the table (PickPen / PutPen). Pen frame: tip at the origin, +Y along the
    // barrel towards the back end (length layout::PEN_LENGTH).
    m::mat4 frame;
    std::vector<PenKey> path;           // Write
    // TurnPage: world position of the lifted page corner for a flip progress s in [0,1] (the page
    // geometry uses the same s, see Animator::pageTurnProgress()).
    std::function<m::vec3(float s)> pageCorner;
    float duration = 0.0f;              // Wait; TurnPage (0 = Timing::PageTurn)
};

// Duration the animator will use for a writing-hand task.
float writeTaskDuration(const WriteTask& t);
// Page flip progress s for a TurnPage task's time fraction u in [0,1] (what pageTurnProgress()
// returns): 0 until the corner is pinched (u = 0.33, PageGripped), lifted briskly past the vertical
// until the hand lets go (u = 0.70, s = 0.60), then the page falls over by itself and lies flipped
// at u = 0.90 (PageTurned). Monotone, continuous, smooth apart from the pinch instant.
float pageTurnEase(float u);
// The pen tip curve through a path's keys at time t (seconds from the path start), exactly as the
// hand follows it: Catmull-Rom through the keys in the paper plane; while down the height is
// interpolated linearly between the keys, while up it leaves and reaches the paper with no
// vertical speed and never dips below the lower key. Clamped to the first / last key. The
// scoresheet can use it to lay the ink where the tip really went.
m::vec3 penPathPoint(const std::vector<PenKey>& path, float t);
bool penPathDown(const std::vector<PenKey>& path, float t);   // tip on the paper at time t

class Animator {
public:
    Animator();
    // Not copyable (a copy would drive the same character state); movable, e.g. to start afresh
    // with 'a = Animator()'. A moved-from animator needs init() before any other call.
    Animator(const Animator&) = delete;
    Animator& operator=(const Animator&) = delete;
    Animator(Animator&&) = default;
    Animator& operator=(Animator&&) = default;
    // pelvisWorld: hip joint centre in the world; facing: +1 = faces -Z (White, sitting at +Z),
    // -1 = faces +Z (Black). playHand: the hand that plays and presses the clock (the one on the
    // clock side); the other hand writes (WriteTask). With Side::Left every playing-hand task is
    // done with the left hand. Handshakes always use the right hand.
    void init(const character::Skeleton& sk, m::vec3 pelvisWorld, float facing,
              character::Side playHand = character::Side::Right);
    character::Side playHand() const;
    character::Side writingHand() const;
    // Where the playing hand / the other hand rests on the table (the other one has a default in
    // front of the body). A spot next to pieces standing on the table (spare or captured pieces) is
    // shifted back or outwards until the hand is clear of them. (Named after the right-handed
    // default: setRestHand = playing hand, setLeftRestHand = writing hand.)
    void setRestHand(m::vec3 worldPos);
    void setLeftRestHand(m::vec3 worldPos);              // optional
    // Game callback: world transform of a piece object (base centre at the origin, +Y up).
    // When neither obstacle callback is set, the animator also calls it (and pieceGripInfo) for
    // the ids 0, 1, 2... when a task starts, to see which pieces stand on the board and the table,
    // until 16 ids in a row do not exist: return the identity matrix (or any transform below the
    // table) for an id that does not exist or whose piece is out of the game.
    std::function<m::mat4(int pieceId)> pieceTransform;
    // Game callback: piece dimensions: x = height (m), y = grip height (m above the base; a value
    // larger than the height is read as a fraction of the height, e.g. layout::PIECE_GRIP_HEIGHT),
    // z = radius at the grip height (m).
    std::function<m::vec3(int pieceId)> pieceGripInfo;
    // Optional game callback: world Y of the highest piece top near the segment from->to (board
    // positions). Used to keep carried pieces PIECE_LIFT_HEIGHT above the pieces they pass over.
    // When neither callback is set the animator looks at the pieces itself (see pieceTransform);
    // when it cannot, it assumes a king may stand anywhere on the way.
    // Both obstacle callbacks are called from inside update(), when a task starts, before the game
    // has seen that call's events: skip the pieces for which holding(id) is true (on either
    // animator: gripped pieces may still look free in the game's state) and pieces in the air.
    // Pieces this animator put down earlier in the same update() call are added by the animator.
    std::function<float(m::vec3 from, m::vec3 to)> pathObstacleTop;
    // Optional game callback: world Y of the highest top among the standing pieces whose base
    // comes within 'radius' (horizontally) of point p, ignoring piece 'ignoreId' and pieces being
    // held; BOARD_TOP_Y when there is none. Lets the hand and the elbow keep clear of the
    // neighbours of the piece it grabs or sets down. Without it pathObstacleTop is used.
    std::function<float(m::vec3 p, float radius, int ignoreId)> obstacleTopNear;

    void enqueue(const Task& t);
    void enqueue(const std::vector<Task>& tasks);
    bool busy() const;                                   // tasks pending or running
    bool runningTask(TaskType type) const;               // a task of that type is under way

    // Writing hand. setWritingRest: where the writing hand waits while it holds the pen, e.g.
    // resting on the scoresheet beside the next row (world point on the paper). Its events come
    // out of update() like the others.
    void setWritingRest(m::vec3 worldPos);
    void enqueueWriting(const WriteTask& t);
    void enqueueWriting(const std::vector<WriteTask>& tasks);
    bool writingBusy() const;                            // writing-hand tasks pending or running
    void clearWritingQueue();                            // drops pending writing tasks (running one finishes)
    float writingRemainingTime() const;                  // running writing task remainder + pending durations
    // Time along the running Write path in seconds (-1 when no path is being followed): the ink
    // is laid down wherever the tip has been with down = true up to this time.
    float writingPathTime() const;
    // Progress s in [0,1] of the running TurnPage (-1 otherwise); the page mesh follows it.
    float pageTurnProgress() const;
    // World transform of the pen while the hand holds it (pen frame as in WriteTask::frame).
    bool penTransform(m::mat4& out) const;
    bool holdsPen() const;
    void clearQueue();                                   // drops pending tasks (running one finishes)
    // Drops the pending tasks and cuts the running one short, without its remaining events (a
    // handshake cut short leaves its partner to finish alone): the playing hand lets go of what it
    // holds where it is, and the game puts those pieces back itself. The next task starts from
    // wherever the hand is (queue one, a Retract at least).
    void cancelTasks();
    // Running task remainder + pending durations, including the waits for their notBefore.
    float remainingTime() const;

    // ---- Coach gestures and speech (see TaskType::Point / Trace / Gesture)
    // Cuts the hold of the running Point / Trace / Gesture short: it ends now, but never before it
    // has arrived (a Trace: before its path is done; a Beat: before its last stroke is over). A
    // Point / Trace fires PointReleased then, and the next queued task starts from the held pose.
    // No effect on other tasks.
    void endHold();
    // World position of the playing hand's index fingertip (after the last update); false before
    // init. A highlight can follow it during a Trace.
    bool pointerTip(m::vec3& out) const;
    // Head gestures on top of the gaze (the eyes keep their target): a nod (down and back up) and
    // a head shake (two turns each way). amplitude in radians, duration in seconds.
    void nod(float amplitude = 0.07f, float duration = 0.45f);
    void shakeHead(float amplitude = 0.06f, float duration = 0.6f);
    // Voice envelope 0..1, every frame while speaking (e.g. the speech output level; 0 when
    // silent; smoothed inside): the head bobs with the syllables and dips on the stressed ones, the
    // torso leans in a little, the eyes widen slightly. The robot has no jaw: head, eyes, torso and
    // the playing hand's gestures show the speech.
    void setSpeechLevel(float level);
    void blink();                                        // a blink now (e.g. at the end of a phrase)

    // Gaze: world point to look at (head + eyes, with natural limits and saccades). weight 0..1.
    void lookAt(m::vec3 target, float weight = 1.0f);
    // First-person player: head orientation comes from the camera (yaw/pitch relative to the
    // body forward, radians; yaw > 0 turns to the character's left, pitch > 0 looks up); the
    // animator applies it to Neck/Head (30/70) so the body matches. Clamped to yaw +-70 deg,
    // pitch -45..+30 deg. The torso lean of reaches is compensated, so the view does not tilt.
    void setHeadOverride(bool enabled, float yaw = 0.0f, float pitch = 0.0f);
    // The head's orientation now, in setHeadOverride's terms (the override's angles, or where the
    // gaze controller has turned it): an override driven from outside can start there, without a
    // jump.
    void headAngles(float& yaw, float& pitch) const;
    // Leaning towards the board, 0 (upright) .. 1 (about 11 degrees forward from the hips), reached
    // smoothly: the online opponent's lean (mouse wheel). The head keeps its orientation (an
    // override is in character space); the eyes move forward and down with the chest.
    void setLean(float lean);
    void setThinking(bool thinking);                     // idle variations (chin on hand, etc.)

    void update(float dt, std::vector<Event>& events);   // advances tasks, IK, idle; appends events

    const character::Pose& pose() const { return pose_; }
    const m::mat4* globals() const { return globals_; }  // world matrix per bone (after update)
    m::mat4 eyeCameraTransform() const;                  // midpoint between the eyes, -Z = gaze
    // World transform of an attached piece (true while the hand holds it).
    bool heldPieceTransform(int pieceId, m::mat4& out) const;
    bool holding(int pieceId) const;                     // pieceId is attached to the hand
    float time() const;                                  // animator clock (sum of update dt)

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    character::Pose pose_;
    m::mat4 globals_[character::BoneCount];
};

}  // namespace anim
