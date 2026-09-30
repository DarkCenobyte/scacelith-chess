// Character animation: procedural posing of a seated robot (IK arms, finger poses, head/eye
// look-at, blinks, idle life) driven by queued hand tasks with FIXED durations.
// Implemented by the animation work package (src/anim/animator.cpp, viewer: --scene anim).
//
// Fairness rule (game design): every task type has a fixed duration that does not depend on
// distance or on which player performs it, so both players spend exactly the same clock time on
// the same physical action. Trajectories are fast and decisive, like a player short on time.
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
// SCACELITH_ANIM_DEBUG=1 logs the planning, SCACELITH_ANIM_ARMTRACE=1 logs joint-limit clamps.
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
    // Instants inside the handshake (seconds from its start) of the two handshake events.
    static constexpr float HandshakeClaspAt = 0.92f;
    static constexpr float HandshakeReleaseAt = 1.96f;
    // Writing hand (see WriteTask). A Write lasts WriteApproach + the path + WriteRetract.
    static constexpr float PickPen = 0.70f;       // rest -> pen lifted from the table, writing grip
    static constexpr float PutPen = 0.60f;        // pen laid down on the table, hand back to rest
    static constexpr float WriteApproach = 0.30f; // rest -> pen tip on the first path key
    static constexpr float WriteRetract = 0.30f;  // last key -> rest (pen kept in hand)
    static constexpr float PageTurn = 1.40f;      // pinch the page corner, flip it over the top edge
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

// Duration the animator will use for this task (Timing default unless t.duration > 0).
float taskDuration(const Task& t);

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
    WritingQueueEmpty
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
};

// ---- Writing hand ------------------------------------------------------------------------------
// The hand that does not play (the one on the scoresheet side) runs its own task queue, at the same
// time as the playing hand: writing never delays a move or a clock press.
// Protocol (all positions in world space):
//   game start : PickPen(frame of the pen lying beside the pad)
//   each move  : setWritingRest(start of the next row), Write(path of the move's text)
//   page full  : TurnPage(pageCorner), then write on the fresh page
//   game end   : PutPen(frame), wait for WritingQueueEmpty, then the Handshake
// Write and TurnPage expect the pen in the hand (PickPen first). Event instants: PenPicked 0.36 s
// and PenPut 0.34 s after their task starts (scaled with a custom duration), PenDown / PenUp /
// WritingDone at the path key times + WriteApproach, PageGripped / PageTurned at 0.33 / 0.90 of the
// TurnPage duration. The pageCorner callback is called during the whole task, keep it valid.
// Left-handed player (init with Side::Left): the right hand writes, and the handshake needs it: a
// running writing task is cut short when the handshake starts (its remaining path / page events
// fire at once), a held pen is laid down first (at the frame of the next queued PutPen, which is
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
    float remainingTime() const;                         // running task remainder + pending durations

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
