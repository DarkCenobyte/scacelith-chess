// The live side of online play (server games and direct matches alike): the engine-free rules
// that GameScene (game_scene_online.cpp) applies every frame, header-only so that the unit tests
// (tests/online_live_tests.cpp) reach them without the scene.
//   - My gesture (net/gesture.h): built from what my hand and my head do, with a short dwell before
//     an aimed square counts and the Side test of the look; sent when it changed, when the head
//     moved noticeably, and at least once per keepalive (Welcome.gestureIdleMs, 1 s by default).
//   - The opponent's gestures: when their piece fields may move the opponent's robot, which of
//     their squares and moves are valid here (gestures are untrusted and cosmetic), how long they
//     stay valid (in keepalives: their client sends at the same interval), the pace of the robot's
//     hand (one step at a time, whatever their rate), what becomes of its live work when their
//     move comes, and the spring the robot's head follows them with.
//   - My clock display while my move is on its way, and the resend of a move the authority never
//     got (a connection lost at the wrong moment).
//   - The RatingRestored notice, held back while a game is being played.
//   - Which realtime errors belong to the game being played, which refuse the challenge being
//     created, and which refuse a search (OnlineSession's handling of them).
//   - Which game a realtime message is about, whether a RatingUpdate rates the game shown, and
//     which ServerInfoResult answers Options' "Test connection" (OnlineSession's routing); the
//     late messages of an earlier game the scene ignores all the same.
//   - When the pin saved at sign-in no longer applies (Options' pin field emptied): on Apply and
//     for "Test connection" alike.
#pragma once
#include "../anim/stance.h"
#include "../chess/chess.h"
#include "../math/math.h"
#include "../net/gesture.h"
#include "../net/online_client.h"
#include "../net/protocol_gen.h"
#include "layout.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace game {
namespace live {

constexpr int kNoSquare = net::Gesture::kNoSquare;

// =============================================================================================
// My gesture
// =============================================================================================

constexpr float kAimDwell = 0.12f;          // s the pointer rests on a square before the aim counts
constexpr float kPoseStep = 1.0f * m::DEG;  // a head turn worth a message
constexpr float kLeanStep = 0.05f;          // a lean change worth a message

// What my hand does (the piece fields of net/gesture.h).
struct Hand {
    int touch = kNoSquare;   // square of the piece in hand
    int aim = kNoSquare;     // legal destination under the pointer (after the dwell)
    uint16_t placed = 0;     // move on the board waiting for my clock press (net::packMove)
    bool promoting = false;  // the promotion picker is open
    bool idle() const { return touch == kNoSquare && aim == kNoSquare && placed == 0 && !promoting; }
};

// A value that changes only once the new one has held for 'dwell' seconds (the aimed square: the
// pointer crossing squares on its way does not make the opponent's robot hand wander).
class Dwell {
public:
    explicit Dwell(float dwell) : dwell_(dwell) {}
    void reset(int value = kNoSquare) {
        value_ = candidate_ = value;
        held_ = 0.0f;
    }
    int update(int candidate, float dt) {
        if (candidate != candidate_) {
            candidate_ = candidate;
            held_ = 0.0f;
        } else {
            held_ += dt;
        }
        if (candidate_ != value_ && held_ >= dwell_) value_ = candidate_;
        return value_;
    }
    int value() const { return value_; }

private:
    float dwell_;
    int value_ = kNoSquare, candidate_ = kNoSquare;
    float held_ = 0.0f;
};

inline bool sameHand(const net::Gesture& a, const net::Gesture& b) {
    return a.touch == b.touch && a.aim == b.aim && a.placed == b.placed &&
           (a.flags & net::proto::GestureFlag::Promoting) == (b.flags & net::proto::GestureFlag::Promoting);
}

// My gesture this frame. 'plies' is the number of plies played; 'previous' the gesture built last
// time (nullptr: none yet): ply is the number of plies played when the hand's current state began,
// so a change of the head alone (Glance and Side included) keeps it. A game set back to fewer plies
// (a move refused, the board rebuilt from the authority's) starts the state again at 'plies': a
// ply ahead of the game would make the opponent's client wait for a MoveMade that never comes.
inline net::Gesture buildGesture(const Hand& hand, int plies, float yaw, float pitch, float lean, bool glance, bool side,
                                 const net::Gesture* previous) {
    net::Gesture g;
    g.touch = hand.touch;
    g.aim = hand.aim;
    g.placed = hand.placed;
    if (hand.promoting) g.flags |= net::proto::GestureFlag::Promoting;
    if (glance) g.flags |= net::proto::GestureFlag::Glance;
    // The scoresheet lies beside the board: a glance is a look to the side.
    if (glance || side) g.flags |= net::proto::GestureFlag::Side;
    g.yaw = yaw;
    g.pitch = pitch;
    g.lean = m::clamp(lean, 0.0f, 1.0f);
    g.ply = previous && sameHand(*previous, g) && previous->ply <= plies ? previous->ply : plies;
    return g;
}

// Whether 'now' is worth sending after 'last' went 'sinceLastMs' ago: the state changed (hand,
// ply, Glance, Side), the head turned by about a degree or leaned, or the keepalive is due: a
// gesture goes at least every 'keepaliveMs' (GameLink::gestureKeepaliveMs, clamped here too), even
// when nothing moved.
inline bool gestureDue(const net::Gesture& last, const net::Gesture& now, double sinceLastMs, int keepaliveMs) {
    if (!last.sameState(now) || sinceLastMs >= double(net::gestureKeepaliveMs(keepaliveMs))) return true;
    return std::fabs(now.yaw - last.yaw) > kPoseStep || std::fabs(now.pitch - last.pitch) > kPoseStep ||
           std::fabs(now.lean - last.lean) > kLeanStep;
}

// The Side flag: the look (a ray from the eyes) falls on the table top beside the board, where the
// clock, the captured pieces and the scoresheets lie. Those are mirrored between the two clients
// (each puts the clock at its own player's right), so the receiver turns such a look the other way.
inline bool lookBesideBoard(m::vec3 origin, m::vec3 dir) {
    float t = m::rayPlane(m::Ray{origin, dir}, m::vec3(0, layout::TABLE_TOP_Y, 0), m::vec3(0, 1, 0));
    if (!(t > 0.0f)) return false;
    m::vec3 p = origin + dir * t;
    return std::fabs(p.x) > 0.5f * layout::BOARD_SIZE && std::fabs(p.x) <= 0.5f * layout::TABLE_WIDTH &&
           std::fabs(p.z) <= 0.5f * layout::TABLE_DEPTH;
}

// =============================================================================================
// The opponent's gestures
// =============================================================================================

// Their client sends a gesture at least once per keepalive (both clients read the same
// Welcome.gestureIdleMs), so the timeouts of their gestures count in keepalives: 2.5 s, 5 s and
// 5 s at the shortest keepalive (1 s, the default), never less.
constexpr float kHeadTimeoutKeepalives = 2.5f;    // an older gesture no longer drives the robot's head
constexpr float kHoldTimeoutKeepalives = 5.0f;    // without a gesture: a piece held live is put back
constexpr float kPlacedTimeoutKeepalives = 5.0f;  // a move put down waits for its MoveMade once the gestures left it
constexpr float kFollowDwell = 0.15f;   // s an aim holds before the robot carries the piece over it
constexpr float kAimLost = 0.6f;        // s without an aim before the piece goes back over its square
constexpr float kHandSlack = 0.05f;     // s of work left to the robot's hand when it may take its next step

// Those timeouts in seconds, for the keepalive of the link (ms, GameLink::gestureKeepaliveMs,
// clamped here too).
inline float keepaliveSeconds(int keepaliveMs) { return float(net::gestureKeepaliveMs(keepaliveMs)) / 1000.0f; }
inline float headTimeout(int keepaliveMs) { return kHeadTimeoutKeepalives * keepaliveSeconds(keepaliveMs); }
inline float holdTimeout(int keepaliveMs) { return kHoldTimeoutKeepalives * keepaliveSeconds(keepaliveMs); }
inline float placedTimeout(int keepaliveMs) { return kPlacedTimeoutKeepalives * keepaliveSeconds(keepaliveMs); }

// The opponent's stance as this client shows it (protocol minor 2, net/gesture.h): the latest
// OpponentStance; back to Seated once net::kStanceExpiryKeepalives keepalives pass without one
// while it is not Seated (their client refreshes a standing stance at every keepalive); an
// unknown value reads as Seated. reset() at a new game, at its end and when the link is lost.
class StanceTracker {
public:
    void reset() { code_ = 0; age_ = 0.0f; }
    void heard(int code) { code_ = code; age_ = 0.0f; }   // an OpponentStance event
    void advance(float dt) { age_ += dt; }                 // every frame
    anim::Stance current(int keepaliveMs) const {
        anim::Stance s = anim::stanceFromCode(code_);
        if (s != anim::Stance::Seated && age_ > float(net::kStanceExpiryKeepalives) * keepaliveSeconds(keepaliveMs))
            return anim::Stance::Seated;
        return s;
    }

private:
    int code_ = 0;
    float age_ = 0.0f;
};

// The local state that decides whether the piece fields of the opponent's latest gesture apply
// (touch, aim, placed): only to the move being prepared, never to one already known.
struct PieceGate {
    int plies = 0;              // plies of the local game
    bool remoteToMove = false;  // the opponent is to move in the local game
    bool waiting = false;       // the local robots are done (Turn::RemoteWaiting)
    bool moveQueued = false;    // the opponent's MoveMade for this ply arrived (the robot will play it)
    bool playing = false;       // State::Playing, the game ongoing and not ending
    bool resync = false;        // a rebuild from the authority is pending
    bool fresh = false;         // the gesture came after the last snapshot and our last reconnection
};

// True when the gesture describes the move the opponent prepares now. A gesture of an earlier ply
// is stale (that move is known); one of a later ply is ahead of its MoveMade: neither counts, and
// in particular neither lets go of a piece held live.
inline bool piecesApply(const net::Gesture& g, const PieceGate& s) {
    return s.fresh && s.playing && !s.resync && s.remoteToMove && s.waiting && !s.moveQueued && g.ply == s.plies;
}

// The piece fields of a gesture, checked against the local position (the opponent, 'remote', to
// move): a square of theirs to hold, a legal destination of it, a legal move of it to put down.
// Anything else is dropped (kNoSquare / 0): gestures come from the other client and are not
// trusted. A valid 'placed' also sets the aim to its destination.
struct PieceIntent {
    int touch = kNoSquare;
    int aim = kNoSquare;
    uint16_t placed = 0;
};

inline PieceIntent pieceIntent(const net::Gesture& g, const chess::Position& pos, chess::Color remote) {
    PieceIntent in;
    if (g.touch < 0 || g.touch >= 64) return in;
    const chess::Square from = chess::Square(g.touch);
    const chess::Piece p = pos.at(from);
    if (p.empty() || p.color != remote || pos.sideToMove() != remote) return in;
    in.touch = g.touch;
    auto legalTo = [&](int to) {
        if (to < 0 || to >= 64) return false;
        bool promo = p.type == chess::Pawn && (chess::rankOf(chess::Square(to)) == 7 || chess::rankOf(chess::Square(to)) == 0);
        return pos.findLegal(from, chess::Square(to), promo ? chess::Queen : chess::NoPiece).valid();
    };
    if (g.placed != 0 && net::moveFrom(g.placed) == g.touch &&
        pos.findLegal(from, chess::Square(net::moveTo(g.placed)), chess::PieceType(net::movePromo(g.placed))).valid()) {
        in.placed = g.placed;
        in.aim = net::moveTo(g.placed);
        return in;
    }
    if (legalTo(g.aim)) in.aim = g.aim;
    return in;
}

// The robot's hand follows their gestures one step at a time: it takes a new step (a carry, or a
// change of piece) only once it has at most kHandSlack left of the previous one, and then from
// their latest gesture. Gestures may come faster than the robot plays them (a modified client can
// send whatever it likes at the relay's rate): its work never piles up.
inline bool handReady(float busy) { return busy <= kHandSlack; }

// What the robot's hand does this frame with the piece fields of their latest gesture ('in',
// pieceIntent), holding live the piece of square 'held' (kNoSquare: none), with 'busy' seconds
// left of what it does (Animator::remainingTime).
struct HandStep {
    bool letGo = false;   // the piece held goes back on its square
    bool take = false;    // then the hand takes the piece of in.touch
    bool place = false;   // the piece in hand is put down as in.placed
    bool follow = false;  // it is carried over in.aim (one carry at a time, see handReady)
};

inline HandStep handStep(int held, const PieceIntent& in, float busy) {
    HandStep s;
    const bool ready = handReady(busy);
    if (in.touch == kNoSquare) {
        s.letGo = held != kNoSquare && ready;
        return s;
    }
    if (held != in.touch) {
        // Another piece: once the hand is free (their latest gesture then decides).
        if (!ready) return s;
        s.letGo = held != kNoSquare;
        s.take = true;
    }
    s.place = in.placed != 0;
    s.follow = !s.place;
    return s;
}

// The live work of the opponent's robot when their MoveMade is played (startRemoteMove).
struct LiveWork {
    int held = kNoSquare;   // square of the piece held live (kNoSquare: none)
    int ply = -1;           // the ply it was taken for
    uint16_t placed = 0;    // the move put down live (0: none)
    bool takeBack = false;  // a move put down waits to be taken back
    bool before = false;    // the hand has not finished what it did before it went for that piece
    bool busy = false;      // the hand still plays live tasks (a piece going back included)
};

// What becomes of it for their move 'move' of ply 'ply'.
enum class LiveStart {
    Fresh,   // nothing live: the move is played from the start (the hand reaches for the piece)
    Held,    // the hand holds the moving piece, or is on its way to it: it goes on from there
    Placed,  // the move stands on the board, or is being put down: only the clock press is left
    Cut      // anything else: the live work is dropped at once and the board set back from the
             // game, then the move is played from the start, in the usual time
};

inline LiveStart liveStart(const LiveWork& w, int ply, uint16_t move) {
    const bool clean = !w.takeBack && !w.before && w.ply == ply;
    if (clean && w.placed != 0 && w.placed == move) return LiveStart::Placed;
    if (clean && w.placed == 0 && w.held != kNoSquare && w.held == net::moveFrom(move)) return LiveStart::Held;
    if (w.held != kNoSquare || w.placed != 0 || w.takeBack || w.busy) return LiveStart::Cut;
    return LiveStart::Fresh;
}

// The opponent's head drives their robot: the option to ignore it is off, the last gesture is
// recent (under headTimeout) and fresh, the opponent is connected and so are we.
inline bool headActive(float gestureAge, bool fresh, bool ignored, bool opponentAway, bool reconnecting, int keepaliveMs) {
    return fresh && !ignored && !opponentAway && !reconnecting && gestureAge < headTimeout(keepaliveMs);
}

// A piece held live is put back when the gestures stopped coming (five keepalives without one).
inline bool holdExpired(float gestureAge, int keepaliveMs) { return gestureAge >= holdTimeout(keepaliveMs); }

// A critically damped spring towards the latest head angles (radians), sub-stepped: the robot's
// head follows the opponent's samples (a few per second) smoothly, without overshoot.
class HeadSpring {
public:
    void snap(float yaw, float pitch) {
        yaw_ = yaw;
        pitch_ = pitch;
        vy_ = vp_ = 0.0f;
    }
    void update(float targetYaw, float targetPitch, float dt, float w0 = 10.0f) {
        if (!(dt > 0.0f)) return;
        int n = std::max(1, int(std::ceil(dt / (1.0f / 240.0f))));
        float h = dt / float(n);
        for (int i = 0; i < n; ++i) {
            vy_ += (w0 * w0 * (targetYaw - yaw_) - 2.0f * w0 * vy_) * h;
            vp_ += (w0 * w0 * (targetPitch - pitch_) - 2.0f * w0 * vp_) * h;
            yaw_ += vy_ * h;
            pitch_ += vp_ * h;
        }
    }
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }

private:
    float yaw_ = 0.0f, pitch_ = 0.0f, vy_ = 0.0f, vp_ = 0.0f;
};

// =============================================================================================
// My clock and my move on its way
// =============================================================================================

// My clock display stands still from my move's send until its confirmation, but no longer than
// max(1 s, 3 pings) (pingMs < 0: unknown) and never while the connection is being restored: the
// authority's clock runs meanwhile, and so does the display.
inline bool clockFreezeHolds(double sinceSendMs, int pingMs, bool reconnecting) {
    double window = std::max(1000.0, 3.0 * double(std::max(0, pingMs)));
    return !reconnecting && sinceSendMs < window;
}

// That freeze: when my move went and the time my clock showed then, set together. A move sent
// again after a reconnection starts it again at the authority's time of that moment (the outage
// was charged to my clock), not at the time it showed before the outage.
struct ClockFreeze {
    double sentMs = 0.0;   // localMs() of the send
    int64_t shownMs = 0;   // my clock then
    void start(double nowMs, int64_t clockMs) {
        sentMs = nowMs;
        shownMs = clockMs;
    }
    bool holds(double nowMs, int pingMs, bool reconnecting) const { return clockFreezeHolds(nowMs - sentMs, pingMs, reconnecting); }
};

// After a snapshot (typically at a reconnection): true when the authority has every move of the
// local game but my pending one, the game goes on and it is still my turn there. The same {ply,
// move} is then sent again (a duplicate is answered with the original MoveMade) instead of the
// board being rebuilt without it. 'myColor' 0 White, 1 Black.
inline bool resendPendingMove(const std::vector<uint16_t>& local, const std::vector<uint16_t>& authority, int pendingPly,
                              int myColor, bool ongoing) {
    if (!ongoing || pendingPly < 0 || local.size() != authority.size() + 1 || size_t(pendingPly) != authority.size()) return false;
    if (int(authority.size() % 2) != (myColor & 1)) return false;
    return std::equal(authority.begin(), authority.end(), local.begin());
}

// =============================================================================================
// RatingRestored
// =============================================================================================

// The toast of the RatingRestored notice (an opponent of our rated games was banned for cheating)
// never shows while a game is being played: the notice waits for the end of the game, and the
// points of several notices add up.
class HeldNotice {
public:
    void add(double points) {
        points_ += points;
        pending_ = true;
    }
    // The points to announce now (and forgets them), or false: nothing waits or a game goes on.
    bool take(bool inGame, double& points) {
        if (!pending_ || inGame) return false;
        points = points_;
        points_ = 0.0;
        pending_ = false;
        return true;
    }
    bool pending() const { return pending_; }

private:
    bool pending_ = false;
    double points_ = 0.0;
};

// =============================================================================================
// Realtime errors
// =============================================================================================

// The errors of a game (net::proto ErrorCode 100..112), which the scene shows. AlreadyInGame and
// InvalidCategory are in that range but answer the menus with no game named (a QueueJoin, a
// challenge accepted or joined): not a game's.
inline bool gameError(int code) {
    using E = net::proto::ErrorCode;
    return code >= int(E::NotInGame) && code <= int(E::FlagFell) && code != int(E::AlreadyInGame) &&
           code != int(E::InvalidCategory);
}

// The refusals of a challenge or private game being created, which come before any status gives it
// an id (UserUnavailable and RatedRepeatLimit may also answer a challenge accepted or a code
// joined). The other errors of 200..210 answer other commands (a QueueJoin, a challenge accepted or
// declined, a rematch): a challenge of mine still pending stays.
inline bool challengeRefused(int code) {
    using E = net::proto::ErrorCode;
    return code == int(E::UserUnavailable) || code == int(E::ChallengeLimit) || code == int(E::CannotChallengeSelf) ||
           code == int(E::RatedRequiresOfficialTc) || code == int(E::InvalidTimeControl) ||
           code == int(E::RatedRepeatLimit);
}

// The refusals of a QueueJoin, which end the search it began (AlreadyInGame and InvalidCategory
// may also answer a challenge accepted or joined).
inline bool queueRefused(int code) {
    using E = net::proto::ErrorCode;
    return code == int(E::QueueNotAllowed) || code == int(E::MatchmakingCooldown) || code == int(E::AlreadyInGame) ||
           code == int(E::InvalidCategory);
}

// =============================================================================================
// Routing
// =============================================================================================

// The game a realtime message is about: the one it names, or the one its snapshot carries. e.game
// is the game shown now, not the one of a late message (a rematch may have begun since).
inline uint64_t eventGameId(const net::Event& e) { return e.gameId ? e.gameId : e.game.id; }

// The scene's own guard (GameScene::onlineEvent), whatever the routing above it: a GameEvent,
// GameEnd or RatingUpdate naming another game than the one played is a late message of an earlier
// game (its rematch offered or refused, its rating) and changes nothing in this one.
inline bool aboutAnotherGame(const net::Event& e, uint64_t playedGame) {
    using K = net::Event::Kind;
    return (e.kind == K::GameEvent || e.kind == K::GameEnd || e.kind == K::RatingUpdate) && e.gameId != 0 &&
           e.gameId != playedGame;
}

// A RatingUpdate follows the commit of its game, and a rematch may have begun meanwhile: only the
// update of the game shown (e.game, whose 'you' is our side) changes the account's ratings in
// place, in its queue's category or else (a challenge) the game's. For an earlier game the account
// is fetched again.
inline bool ratesShownGame(const net::Event& e) { return e.gameId == e.game.id; }
inline const std::string& ratingCategory(const net::Event& e) {
    return e.queueCategory.empty() ? e.game.category : e.queueCategory;
}

// Options' "Test connection": a ServerInfoResult answers it only from the tested server (the info
// of the server in use, asked before the test, may come first).
inline bool testAnswer(const net::Event& e, bool testing, const std::string& testOrigin) {
    return e.kind == net::Event::Kind::ServerInfoResult && testing && e.origin == testOrigin;
}

// The pin field of the server applied emptied (a custom server, same host and port): Apply
// forgets the pin saved with its session at sign-in (OnlineSession::applyServer), and "Test
// connection" goes without it, so that the test tells what Apply will give.
inline bool savedPinDropped(const net::ServerEndpoint& ep, const net::ServerEndpoint& applied, bool custom) {
    return custom && ep.origin() == applied.origin() && !applied.pinnedSha256.empty() && ep.pinnedSha256.empty();
}

}  // namespace live
}  // namespace game
