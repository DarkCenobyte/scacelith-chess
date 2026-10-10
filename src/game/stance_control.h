// Standing up during a game (anim/stance.h): the rules GameScene applies to the first-person
// player's stance, apart so that the unit tests (tests/stance_control_tests.cpp) reach them
// without the scene.
//
//   - Keys (Playing state only, for the first-person seat: the human against Stockfish or the
//     coach, the hot-seat player to move, the online player): Up stands in front of the chair
//     (from an end of the table: back to the chair, standing), Left / Right go and stand at that
//     end of the table (the seated player's own left and right; from the chair the robot rises
//     first), Down sits back down. Refused while a hand works on a move (a piece in hand, a
//     placement, the promotion picker, the clock press), on either player's turn otherwise. The
//     animator routes every change through Standing and starts it once its hands are idle.
//   - The play lock: only a seated player plays. While the robot is not seated, or its target is
//     not Seated, nothing on the board or the clock answers the player (no touch, no clock press,
//     no auto-press, no glance at the scoresheet); a short notice says to sit back down. The clock
//     keeps running.
//   - Scorekeeping: the writing of a seat waits while it is locked so (Scorekeeper::setHold,
//     together with hot-seat's own hold) and catches up once the robot is back in its chair.
//   - The camera: seated, the base look is straight ahead and down at the board; standing, at the
//     board's centre from the eyes, in the body's frame (the angles of
//     anim::Animator::setHeadOverride), blended in over kLookBlendTime as the robot leaves its
//     chair. A standing look may go further down (the animator bends the back).
//   - Online: the opponent's head drives their standing robot when it is relayed, else that robot
//     looks at the board (remoteHeadMode); a snapshot holding a move of theirs means they sat down.
#pragma once
#include "../anim/stance.h"
#include "../math/math.h"
#include "turn.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace game {
namespace stance {

using anim::Stance;

// The arrow keys of the first-person player.
enum class Key { None, Up, Left, Right, Down };

// The stance a key asks for.
inline Stance targetOf(Key k) {
    switch (k) {
    case Key::Up: return Stance::Standing;
    case Key::Left: return Stance::SideLeft;
    case Key::Right: return Stance::SideRight;
    default: return Stance::Seated;
    }
}

// The hands are free for a stance change: no piece in hand, no hand on its way to the board or
// the clock (the turn's other states, the opponent's turn included, leave them free).
inline bool handsFree(Turn t) {
    return t != Turn::HumanTouched && t != Turn::HumanPlacing && t != Turn::HumanPromotion && t != Turn::HumanPressing;
}

// Only a seated player plays: in the chair, drawn in, and not asked to get up.
inline bool mayPlay(bool seated, Stance target) { return seated && target == Stance::Seated; }

// The writing of a seat's scoresheet waits (Scorekeeper::setHold) while the seat may not play, or
// while hot-seat holds it (the opponent's move written at the start of the player's own turn).
inline bool holdWriting(bool seated, Stance target, bool hotSeatHold) { return hotSeatHold || !mayPlay(seated, target); }

// What a stance key does now.
enum class Verdict {
    Ignore,     // not a key for the stance now (no key, not playing, no first-person seat, the keyboard elsewhere)
    Same,       // the stance asked for is the target already
    HandsBusy,  // a hand works on a move: finish it first (a notice says so)
    Change      // setStance(target)
};
struct Decision {
    Verdict verdict = Verdict::Ignore;
    Stance target = Stance::Seated;
};

// The scene's state for a stance key. 'playing': the Playing state, not paused, no card or
// dialog, no hot-seat handover, the keyboard free for the game. 'firstPerson': the seat whose eyes
// are the view is a local player's (not watching, a replay or an analysis).
struct KeyContext {
    bool playing = false;
    bool firstPerson = false;
    Turn turn = Turn::None;
    Stance target = Stance::Seated;   // the seat's current target (anim::Animator::stanceTarget)
};

inline Decision decide(Key key, const KeyContext& c) {
    Decision d;
    if (key == Key::None || !c.playing || !c.firstPerson) return d;
    d.target = targetOf(key);
    if (d.target == c.target) {
        d.verdict = Verdict::Same;
    } else if (!handsFree(c.turn)) {
        d.verdict = Verdict::HandsBusy;
    } else {
        d.verdict = Verdict::Change;
    }
    return d;
}

// --stance standing|side-left|side-right (screenshots): false for anything else.
inline bool parseStanceArg(const std::string& s, Stance& out) {
    if (s == "standing") out = Stance::Standing;
    else if (s == "side-left") out = Stance::SideLeft;
    else if (s == "side-right") out = Stance::SideRight;
    else return false;
    return true;
}

// A notice repeated at most once per 'interval' seconds (a player clicking away while standing
// gets one toast, not one per click).
class NoticeLimiter {
public:
    explicit NoticeLimiter(float interval = 2.5f) : interval_(interval) {}
    bool allow(float now) {
        if (now - last_ < interval_) return false;
        last_ = now;
        return true;
    }
    void reset() { last_ = -1e9f; }

private:
    float interval_;
    float last_ = -1e9f;
};

// ---- The first-person camera ----------------------------------------------------------------

// Seconds over which the base look goes from the seated one to the standing one (and back).
constexpr float kLookBlendTime = 0.8f;
// The lowest gaze pitch (radians, absolute, the body's frame): seated as before; standing further
// down, to the near edge of the board and the table's edge (the animator bends the back).
constexpr float kPitchMinSeated = -1.1f;
constexpr float kPitchMinStanding = -1.40f;
constexpr float kPitchMax = 0.75f;
// The lowest pitch handed to the head (setHeadOverride): seated the neck's own limit (the eyes do
// the rest), standing anything down to the gaze's limit (the animator bends the back for it).
constexpr float kHeadPitchMinSeated = -45.0f * m::DEG;
constexpr float kHeadPitchMinStanding = kPitchMinStanding;

// Yaw and pitch of a direction given in the body's frame (character space: +Z forward, +X the
// character's left, +Y up), in setHeadOverride's terms: yaw > 0 turns left, pitch > 0 looks up.
struct Angles {
    float yaw = 0.0f, pitch = 0.0f;
};
inline Angles bodyAngles(m::vec3 dirChar) {
    Angles a;
    a.yaw = std::atan2(dirChar.x, dirChar.z);
    a.pitch = std::atan2(dirChar.y, std::sqrt(dirChar.x * dirChar.x + dirChar.z * dirChar.z));
    return a;
}

// The weight of the standing base look (0 seated .. 1 standing), one frame on: it rises while the
// robot is out of its chair (or on its way), falls back once it is seated again.
inline float advanceLookBlend(float w, bool upright, float dt) {
    return m::clamp(w + (upright ? dt : -dt) / kLookBlendTime, 0.0f, 1.0f);
}

// The base look for blend weight w: from the seated look to the look at the board's centre.
inline Angles baseLook(Angles seated, Angles board, float w) {
    float b = m::smootherstep(m::saturate(w));
    // The board's yaw may wrap round while the robot walks: blend the shorter way.
    float dy = board.yaw - seated.yaw;
    while (dy > m::PI) dy -= 2.0f * m::PI;
    while (dy < -m::PI) dy += 2.0f * m::PI;
    return {seated.yaw + dy * b, m::lerp(seated.pitch, board.pitch, b)};
}

// The lowest gaze and head pitches for blend weight w.
inline float pitchMin(float w) { return m::lerp(kPitchMinSeated, kPitchMinStanding, m::saturate(w)); }
inline float headPitchMin(float w) { return m::lerp(kHeadPitchMinSeated, kHeadPitchMinStanding, m::saturate(w)); }

// ---- Online -------------------------------------------------------------------------------------

// What drives the opponent's robot's head. 'standing': the robot is out of its chair (not
// anim::Animator::seated()); 'following': their relayed head applies (live::headActive);
// 'pressingClock': the robot's hand is on its way to the clock (its eyes follow the hand).
enum class RemoteHead {
    Gaze,     // the scene's own gaze (updateGaze): the head override off
    Relayed,  // their head angles (body-relative), the override on
    Board     // standing without a head relayed: the override off, the robot looks at the board
};
inline RemoteHead remoteHeadMode(bool standing, bool following, bool pressingClock) {
    if (standing) return following ? RemoteHead::Relayed : RemoteHead::Board;
    return following && !pressingClock ? RemoteHead::Relayed : RemoteHead::Gaze;
}

// A snapshot (or any move list from the authority) of 'snapshotPlies' moves, after the
// 'localPlies' this client has played: true when one of the new plies is the opponent's (ply 0 is
// White's first move; colours 0 White, 1 Black). Only a seated player moves, so they sat down.
inline bool opponentMovedSince(int localPlies, int snapshotPlies, int opponentColor) {
    for (int p = std::max(0, localPlies); p < snapshotPlies; ++p)
        if (p % 2 == (opponentColor & 1)) return true;
    return false;
}

}  // namespace stance
}  // namespace game
