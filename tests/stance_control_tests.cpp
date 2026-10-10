// Standing up during a game (game/stance_control.h): the stance keys, the play lock, the
// scorekeeping hold, the first-person base look and the online rules.
#include "test.h"
#include "game/online_live.h"
#include "game/stance_control.h"
#include <cmath>

using namespace game;
using namespace game::stance;
using anim::Stance;

namespace {

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

KeyContext playing(Turn t, Stance target) {
    KeyContext c;
    c.playing = true;
    c.firstPerson = true;
    c.turn = t;
    c.target = target;
    return c;
}

const Turn kAllTurns[] = {Turn::None,        Turn::HumanIdle,   Turn::HumanTouched, Turn::HumanPlacing, Turn::HumanPromotion,
                          Turn::HumanPlaced, Turn::HumanPressing, Turn::AiThinking, Turn::AiMoving,     Turn::RemoteWaiting,
                          Turn::RemoteMoving, Turn::CoachTable, Turn::LessonWait};

}  // namespace

TEST(stance_keys_ask_for_their_stance) {
    CHECK_EQ(targetOf(Key::Up), Stance::Standing);
    CHECK_EQ(targetOf(Key::Left), Stance::SideLeft);
    CHECK_EQ(targetOf(Key::Right), Stance::SideRight);
    CHECK_EQ(targetOf(Key::Down), Stance::Seated);
    // From every stance each key goes to its own (the animator routes through Standing).
    const Stance all[] = {Stance::Seated, Stance::Standing, Stance::SideLeft, Stance::SideRight};
    const Key keys[] = {Key::Up, Key::Left, Key::Right, Key::Down};
    for (Stance from : all)
        for (Key k : keys) {
            Decision d = decide(k, playing(Turn::HumanIdle, from));
            CHECK_EQ(d.target, targetOf(k));
            CHECK_EQ(d.verdict, from == targetOf(k) ? Verdict::Same : Verdict::Change);
        }
}

TEST(stance_keys_need_free_hands) {
    for (Turn t : kAllTurns) {
        bool busy = t == Turn::HumanTouched || t == Turn::HumanPlacing || t == Turn::HumanPromotion || t == Turn::HumanPressing;
        CHECK_EQ(handsFree(t), !busy);
        Decision d = decide(Key::Up, playing(t, Stance::Seated));
        CHECK_EQ(d.verdict, busy ? Verdict::HandsBusy : Verdict::Change);
    }
    // On the opponent's turn and with the move waiting for the clock press the player may stand.
    CHECK_EQ(decide(Key::Left, playing(Turn::AiThinking, Stance::Seated)).verdict, Verdict::Change);
    CHECK_EQ(decide(Key::Right, playing(Turn::RemoteMoving, Stance::Standing)).verdict, Verdict::Change);
    CHECK_EQ(decide(Key::Up, playing(Turn::HumanPlaced, Stance::Seated)).verdict, Verdict::Change);
    // Asking for the stance one is going to already is no refusal, busy hands or not.
    CHECK_EQ(decide(Key::Down, playing(Turn::HumanTouched, Stance::Seated)).verdict, Verdict::Same);
}

TEST(stance_keys_only_while_playing_in_first_person) {
    KeyContext c = playing(Turn::HumanIdle, Stance::Seated);
    CHECK_EQ(decide(Key::None, c).verdict, Verdict::Ignore);
    c.playing = false;
    CHECK_EQ(decide(Key::Up, c).verdict, Verdict::Ignore);
    c.playing = true;
    c.firstPerson = false;
    CHECK_EQ(decide(Key::Up, c).verdict, Verdict::Ignore);
    // Not playing outranks busy hands: no notice from a menu or a card.
    c = playing(Turn::HumanTouched, Stance::Seated);
    c.playing = false;
    CHECK_EQ(decide(Key::Up, c).verdict, Verdict::Ignore);
}

TEST(stance_play_lock_and_writing_hold) {
    CHECK(mayPlay(true, Stance::Seated));
    // Asked to stand, still seated (the robot waits for its hands): locked already.
    CHECK(!mayPlay(true, Stance::Standing));
    CHECK(!mayPlay(true, Stance::SideLeft));
    // Asked to sit, still on its way back: locked until seated.
    CHECK(!mayPlay(false, Stance::Seated));
    CHECK(!mayPlay(false, Stance::SideRight));
    // The writing waits exactly while the seat may not play, or while hot-seat holds it.
    for (bool seated : {false, true})
        for (Stance t : {Stance::Seated, Stance::Standing, Stance::SideLeft, Stance::SideRight})
            for (bool hs : {false, true}) CHECK_EQ(holdWriting(seated, t, hs), hs || !mayPlay(seated, t));
}

TEST(stance_dev_argument) {
    Stance s = Stance::Seated;
    CHECK(parseStanceArg("standing", s));
    CHECK_EQ(s, Stance::Standing);
    CHECK(parseStanceArg("side-left", s));
    CHECK_EQ(s, Stance::SideLeft);
    CHECK(parseStanceArg("side-right", s));
    CHECK_EQ(s, Stance::SideRight);
    s = Stance::Standing;
    CHECK(!parseStanceArg("seated", s));
    CHECK(!parseStanceArg("", s));
    CHECK(!parseStanceArg("Standing", s));
    CHECK_EQ(s, Stance::Standing);   // unchanged on refusal
}

TEST(stance_notice_limiter) {
    NoticeLimiter n(2.0f);
    CHECK(n.allow(10.0f));
    CHECK(!n.allow(10.5f));
    CHECK(!n.allow(11.9f));
    CHECK(n.allow(12.0f));
    n.reset();
    CHECK(n.allow(12.1f));
}

TEST(stance_body_angles) {
    // Character space: +Z forward, +X the character's left, +Y up.
    Angles a = bodyAngles(m::vec3(0, 0, 1));
    CHECK(near(a.yaw, 0.0f) && near(a.pitch, 0.0f));
    a = bodyAngles(m::vec3(1, 0, 0));
    CHECK(near(a.yaw, 0.5f * m::PI));   // to the left: yaw > 0
    a = bodyAngles(m::vec3(-1, 0, 1));
    CHECK(near(a.yaw, -0.25f * m::PI));
    a = bodyAngles(m::vec3(0, -1, 1));
    CHECK(near(a.pitch, -0.25f * m::PI));   // down: pitch < 0
    // A robot standing in front of White's chair (pelvis at z 0.70, eyes about 0.66 m higher):
    // the board's centre is ahead and well down, below the seated look.
    const anim::StanceSpot spot = anim::stanceSpot(Stance::Standing, 1.0f);
    m::vec3 eye = spot.pelvis + m::vec3(0.0f, 0.66f, -0.08f);
    m::vec3 d = m::vec3(0.0f, 0.80f, 0.0f) - eye;
    // White faces -Z: the character's +Z is the world's -Z, its left the world's -X.
    a = bodyAngles(m::vec3(-d.x, d.y, -d.z));
    CHECK(near(a.yaw, 0.0f));
    CHECK(a.pitch < -0.62f && a.pitch > kPitchMinStanding);
}

TEST(stance_base_look_blend) {
    Angles seated{0.0f, -0.62f}, board{0.1f, -0.95f};
    Angles l = baseLook(seated, board, 0.0f);
    CHECK(near(l.yaw, seated.yaw) && near(l.pitch, seated.pitch));
    l = baseLook(seated, board, 1.0f);
    CHECK(near(l.yaw, board.yaw) && near(l.pitch, board.pitch));
    // Smooth and monotonic in between.
    float prev = seated.pitch;
    for (int i = 1; i <= 100; ++i) {
        float p = baseLook(seated, board, float(i) / 100.0f).pitch;
        CHECK(p <= prev + 1e-6f);
        prev = p;
    }
    // A yaw across the wrap goes the shorter way.
    l = baseLook(Angles{3.0f, 0.0f}, Angles{-3.0f, 0.0f}, 0.5f);
    CHECK(std::fabs(l.yaw) > 3.0f);
    // The blend weight: rises while upright, falls once seated, within [0, 1].
    float w = 0.0f;
    for (int i = 0; i < 10; ++i) w = advanceLookBlend(w, true, 0.1f);
    CHECK(w > 0.99f);
    for (int i = 0; i < 4; ++i) w = advanceLookBlend(w, false, 0.1f);
    CHECK(w > 0.4f && w < 0.6f);
    for (int i = 0; i < 20; ++i) w = advanceLookBlend(w, false, 0.1f);
    CHECK_EQ(w, 0.0f);
    // The pitch limits: seated as before, lower standing; the head follows the gaze down standing.
    CHECK(near(pitchMin(0.0f), kPitchMinSeated));
    CHECK(near(pitchMin(1.0f), kPitchMinStanding));
    CHECK(kPitchMinStanding < kPitchMinSeated);
    CHECK(near(headPitchMin(0.0f), -45.0f * m::DEG));
    CHECK(near(headPitchMin(1.0f), kPitchMinStanding));
}

TEST(stance_remote_head_modes) {
    // Seated: as before (their head while it comes, not while the hand goes to the clock).
    CHECK_EQ(remoteHeadMode(false, true, false), RemoteHead::Relayed);
    CHECK_EQ(remoteHeadMode(false, true, true), RemoteHead::Gaze);
    CHECK_EQ(remoteHeadMode(false, false, false), RemoteHead::Gaze);
    // Standing: their head when relayed, else the board.
    CHECK_EQ(remoteHeadMode(true, true, false), RemoteHead::Relayed);
    CHECK_EQ(remoteHeadMode(true, true, true), RemoteHead::Relayed);
    CHECK_EQ(remoteHeadMode(true, false, false), RemoteHead::Board);
    CHECK_EQ(remoteHeadMode(true, false, true), RemoteHead::Board);
}

TEST(stance_opponent_moved_in_snapshot) {
    // I play White (the opponent is Black, colour 1): plies 1, 3, ... are theirs.
    CHECK(!opponentMovedSince(2, 2, 1));
    CHECK(!opponentMovedSince(2, 3, 1));   // my own move confirmed
    CHECK(opponentMovedSince(3, 4, 1));
    CHECK(opponentMovedSince(0, 5, 1));
    // I play Black: White's plies 0, 2, ...
    CHECK(opponentMovedSince(0, 1, 0));
    CHECK(!opponentMovedSince(1, 2, 0));
    CHECK(opponentMovedSince(1, 3, 0));
    CHECK(!opponentMovedSince(5, 3, 0));   // nothing new
}

TEST(stance_tracker_reads_the_opponent) {
    // The receiver's view of the opponent's stance (online_live.h), as the scene uses it.
    live::StanceTracker t;
    const int keepalive = 1000;
    CHECK_EQ(t.current(keepalive), Stance::Seated);
    t.heard(int(Stance::SideLeft));
    t.advance(1.0f);
    CHECK_EQ(t.current(keepalive), Stance::SideLeft);
    // Their MoveMade: seated at once.
    t.heard(0);
    CHECK_EQ(t.current(keepalive), Stance::Seated);
    // Not refreshed for kStanceExpiryKeepalives keepalives: seated.
    t.heard(int(Stance::Standing));
    t.advance(float(net::kStanceExpiryKeepalives) + 0.1f);
    CHECK_EQ(t.current(keepalive), Stance::Seated);
    // An unknown value (a later version's) reads as seated; reset too.
    t.heard(9);
    CHECK_EQ(t.current(keepalive), Stance::Seated);
    t.heard(int(Stance::Standing));
    t.reset();
    CHECK_EQ(t.current(keepalive), Stance::Seated);
}
