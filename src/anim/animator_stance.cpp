// Stances of the robot (see animator.h: setStance; the spots: stance.h).
//
// The robot pushes its chair back and gets up, stands in front of its chair looking down at the
// board, walks round the table corner to an end of the table and back, and sits down again, one
// LEG at a time: Rise (Seated -> Standing), Sit (Standing -> Seated) and Walk (Standing <-> an end
// of the table). Each leg is planned in full when it starts, in character space (the seat's frame,
// see animator_impl.h), and only evaluated afterwards: a target that changes meanwhile is taken up
// when the leg ends on its spot.
//
// The body. Hand tasks are planned in character space and only run while the robot is seated, so
// that frame stays the planning frame; a stance moves the BODY inside it. Impl::body maps the body
// space (origin at the pelvis joint wherever it is, turned by the body's yaw) to character space;
// the spine, the arms and the head are solved in body space as they always were, and the world
// root of the pose is root * body. Seated, body is the identity and nothing changes.
//   * The legs: the seated legs come from applySpine; out of the seat they are solved
//     analytically onto the feet (two bones, the knee bending forward along the foot, the ankle
//     turning the foot flat on its frame: sole 0.09 below the ankle). Feet are planned as planted
//     frames and swings between them, so a loaded foot never slides; a walking foot rolls off its
//     ball and strikes with its heel. Each landing fires a Footstep (the sole's centre).
//   * The trunk: hip flexion (the pelvis bone pitching forward over the legs) and spine flexion
//     from the leg's curves, plus, standing, the part of a look down the neck cannot take (the
//     back bends so the board stays comfortably in sight) and setLean (bending over the board).
//   * The hands: a path through hand poses (on the table edge pushing the chair back, on the
//     thighs pushing up, beside the hips, clasped behind the back), each pose attached to what it
//     rests on: the table (character space), a thigh, the lower back. The seated hand (its own
//     motion, in character space) is one of the poses, so a Rise starts from and a Sit ends on
//     exactly the seated hands. Standing and walking, the hands are clasped behind the back: from
//     the opponent's seat the robot reads as a player studying the board, and in first person the
//     arms never come into view (palms on the table edge would need the back bent ~35 degrees, the
//     arms being too short for the table standing upright).
// Left-handed robots run all of this in the solver's mirrored world (animator_impl.h): the spots
// are mirrored on the way in, so the real robot's SideLeft is its own left.
#include "animator_impl.h"

namespace anim {

namespace {
using namespace detail;

constexpr float kAnkleY = 0.09f;          // ankle joint above the floor, foot flat (the sole 0.09 below)
constexpr float kSoleZ = 0.066f;          // the sole's centre, ahead of the ankle (foot space)
const vec3 kBall(0.0f, -0.090f, 0.130f);  // foot space: where the foot rolls off (ball) / lands (heel)
const vec3 kHeel(0.0f, -0.090f, -0.050f);
constexpr float kRollOff = 0.30f;         // heel off before a walking foot lifts (rad, toes down)
constexpr float kRollOn = 0.20f;          // toes up at a heel strike (rad)
constexpr float kRollOffT = 0.10f;        // the heel rises over this long before the lift
constexpr float kRollOnT = 0.08f;         // the foot comes flat this long after the heel strike
constexpr float kFootSide = 0.100f;       // standing: ankles this far from the body's middle line
constexpr float kFootAhead = 0.010f;      // and this much ahead of the hips
constexpr float kToeOut = 0.08f;          // the feet turned out a little (rad)
constexpr float kWalkSide = 0.080f;       // walking: ankles this far off the pelvis path
constexpr float kLegSoft = 0.876f;        // hip-ankle distance from which the pelvis comes down a little
constexpr float kLegMax = 0.8795f;        // ... so it never exceeds this (the leg is 0.88 straight)
constexpr float kLegPlan = 0.868f;        // what the walk plans for (the knees never lock)

// Rise (Seated -> Standing), seconds: the hands go to the table edge and push the robot back with
// its chair while the feet are drawn back under the knees, then it leans forward and rises over
// its feet. A Sit plays the same choreography backwards, after a short hold (the hands come
// forward from the back first).
constexpr float kRiseT = 1.60f;
constexpr float kSitHold = 0.15f;
constexpr float kPushAt = 0.10f, kPushT = 0.55f;   // the chair slides back
constexpr float kForwardAt = 0.62f, kForwardT = 0.80f;   // the pelvis comes forward over the feet
constexpr float kUpAt = 0.82f, kUpT = 0.78f;       // and rises
// Walk: steps every kStepPeriod, each foot swinging kStepSwing (the rest is double support).
constexpr float kStepPeriod = 0.47f, kStepSwing = 0.36f, kFirstLift = 0.05f, kStepLength = 0.34f;
constexpr float kWalkSettle = 0.20f;      // after the last landing
constexpr float kWalkDip = 0.018f;        // pelvis lower while walking (the knees soften)

// Looking down standing: below this pitch, the back takes kLookBack of the rest.
constexpr float kLookBendFrom = 30.0f * DEG, kLookBack = 0.85f;
constexpr float kStandPitchDown = -80.0f * DEG;   // lowest head override standing (seated: -45)
constexpr float kStandLean = 0.30f;               // setLean(1) standing: hip flexion (rad)

float ramp(float x, float a, float T) { return smootherstep((x - a) / T); }
// Up to 'peak' over [a, b], back to 0 over [b, c] (minimum jerk both ways).
float rise(float x, float a, float b, float c, float peak) {
    return x < b ? peak * smootherstep((x - a) / (b - a)) : peak * (1.0f - smootherstep((x - b) / (c - b)));
}
// A curve sampled at 'rate' per second, at time t: Catmull-Rom through the samples.
vec4 sampleCurve(const std::vector<vec4>& c, float rate, float t) {
    if (c.empty()) return vec4(0.0f);
    if (c.size() < 2) return c[0];
    const float x = clamp(t * rate, 0.0f, float(c.size() - 1));
    const int i = std::min(int(x), int(c.size()) - 2);
    const float u = x - float(i);
    const vec4 &p1 = c[size_t(i)], &p2 = c[size_t(i) + 1];
    const vec4 &p0 = c[size_t(std::max(0, i - 1))], &p3 = c[std::min(c.size() - 1, size_t(i) + 2)];
    const float u2 = u * u, u3 = u2 * u;
    vec4 r;
    for (int k = 0; k < 4; ++k)
        r[k] = 0.5f * (2.0f * p1[k] + (p2[k] - p0[k]) * u + (2.0f * p0[k] - 5.0f * p1[k] + 4.0f * p2[k] - p3[k]) * u2 +
                       (3.0f * p1[k] - p0[k] - 3.0f * p2[k] + p3[k]) * u3);
    return r;
}
// Foot frame pitched by 'a' (+ = toes down) about the foot point c (foot space): c stays put.
mat4 pivotFoot(const mat4& f, vec3 c, float a) { return f * translate(c) * toMat4(qx(a)) * translate(-c); }
// Hand rotation from the palm normal and the finger direction (any frame; side s).
quat handFrom(Side s, vec3 palmNormal, vec3 fingers) {
    vec3 n = normalize(palmNormal);
    vec3 f = safeNormalize(perp(fingers, n), orthogonal(n));
    vec3 X = n * palmSign(s), Y = -f;
    return fromMat3(mat3(X, Y, cross(X, Y)));
}
float wrapNear(float a, float ref) { return ref + wrapPi(a - ref); }
}  // namespace

// =============================================================================================
// Body frame
// =============================================================================================
void Animator::Impl::setBody(vec3 pelvisC, float yaw) {
    bodyQ = qy(yaw);
    body = toMat4(bodyQ, pelvisC);
    invBody = inverseAffine(body);
}

HandSample Animator::Impl::handToBody(const HandSample& c) const {
    HandSample b = c;
    const quat iq = conjugate(bodyQ);
    b.p = charToBody(c.p);
    b.v = rotate(iq, c.v);
    b.a = rotate(iq, c.a);
    b.q = normalize(iq * c.q);
    b.tip = charToBody(c.tip);
    b.lockC.q = normalize(iq * c.lockC.q);
    b.lockC.p = charToBody(c.lockC.p);
    return b;
}

// The spot of a stance in character space (the solver's: a left-handed robot's spots mirrored).
void Animator::Impl::spotChar(Stance s, vec3& pelvisC, float& yaw) const {
    if (s == Stance::Seated) {
        pelvisC = vec3(0.0f);
        yaw = 0.0f;
        return;
    }
    const StanceSpot sp = stanceSpot(s, facing);
    pelvisC = toChar(mw(sp.pelvis));
    const vec3 f = rotate(conjugate(rootQ), mw(sp.forward));
    yaw = std::atan2(f.x, f.z);
}

mat4 Animator::Impl::standFoot(int i, vec3 pelvisC, float yaw) const {
    const float sx = i == 0 ? 1.0f : -1.0f;   // the left foot on the body's +X side
    vec3 a = pelvisC + rotate(qy(yaw), vec3(sx * kFootSide, 0.0f, kFootAhead));
    a.y = kAnkleY - pelvisWorld.y;
    return toMat4(qy(yaw + sx * kToeOut), a);
}

void Animator::Impl::seatedFeet(mat4 out[2]) const {
    Pose p;
    applySpine(p, SpineParams());   // (the seated legs do not depend on the spine's flexion)
    for (int i = 0; i < 2; ++i) {
        const Bone t = i == 0 ? ThighL : ThighR;
        out[i] = localMat(p, t) * localMat(p, t + 1) * localMat(p, t + 2);
    }
}

// =============================================================================================
// The body at a time
// =============================================================================================
mat4 Animator::Impl::footAt(const StanceLeg& L, int i, float lt) const {
    mat4 cur = L.foot0[i];
    for (const FootSwing& s : L.steps[i]) {
        if (lt < s.t0 - kRollOffT || (s.roll <= 0.0f && lt <= s.t0)) return cur;
        if (lt <= s.t0) return pivotFoot(cur, kBall, s.roll * kRollOff * smootherstep((lt - (s.t0 - kRollOffT)) / kRollOffT));
        if (lt < s.t1) {
            // The swing: from the rolled-off foot to the heel strike, a low arc in between.
            const float u = (lt - s.t0) / std::max(1e-4f, s.t1 - s.t0), w = smootherstep(u);
            const mat4 a = pivotFoot(cur, kBall, s.roll * kRollOff), b = pivotFoot(s.to, kHeel, -s.roll * kRollOn);
            vec3 p = lerp(a.translation(), b.translation(), w);
            p.y += s.lift * bump(u, 0.45f);
            // (The pitch leads the yaw a little: the toes come up early, ready for the strike.)
            return toMat4(qslerp(rotOf(a), rotOf(b), smootherstep(std::min(1.0f, u * 1.15f))), p);
        }
        cur = s.to;
        if (s.roll > 0.0f && lt < s.t1 + kRollOnT) return pivotFoot(cur, kHeel, -s.roll * kRollOn * (1.0f - smootherstep((lt - s.t1) / kRollOnT)));
    }
    return cur;
}

Animator::Impl::StanceFrame Animator::Impl::stanceFrame(float t) const {
    StanceFrame f;
    // The idle sway of a robot standing still: its weight goes slowly from one foot to the other
    // (faded in after it arrives, out as the next leg starts).
    auto sway = [&](float since, float until) {
        const float w = smootherstep((t - since - 0.3f) / 1.5f) * (1.0f - smootherstep((t - until) / 0.4f));
        return w * (0.011f * std::sin(t * 0.71f + seed * 5.0f) + 0.004f * std::sin(t * 1.63f + 1.1f));
    };
    if (!leg) {
        if (stanceAt == Stance::Seated) return f;
        f.active = true;
        spotChar(stanceAt, f.pelvis, f.yaw);
        for (int i = 0; i < 2; ++i) f.foot[i] = standFoot(i, f.pelvis, f.yaw);
        f.pelvis += rotate(qy(f.yaw), vec3(sway(stanceSince, 1e30f), 0.0f, 0.0f));
        f.slide = kChairSlideMax;
        f.standW = 1.0f;
        f.seatW = 0.0f;
        return f;
    }
    const StanceLeg& L = *leg;
    f.active = true;
    f.leg = &L;
    f.lt = clamp(t - L.start, 0.0f, L.T);
    const vec4 pp = sampleCurve(L.pel, L.rate, f.lt), tr = sampleCurve(L.trunk, L.rate, f.lt);
    f.pelvis = pp.xyz();
    f.yaw = pp.w;
    f.hipFlex = tr.x;
    f.spineFlex = tr.y;
    f.slide = clamp(tr.z, 0.0f, kChairSlideMax);
    f.standW = clamp(tr.w, 0.0f, 1.0f);
    f.seatW = 0.0f;
    if (L.kind == StanceLeg::Rise && f.lt < L.seatIn) f.seatW = 1.0f - smootherstep(f.lt / L.seatIn);
    if (L.kind == StanceLeg::Sit && f.lt > L.seatOut) f.seatW = smootherstep((f.lt - L.seatOut) / (L.T - L.seatOut));
    if (L.kind != StanceLeg::Rise) f.pelvis += rotate(qy(f.yaw), vec3(sway(stanceSince, L.start), 0.0f, 0.0f));
    for (int i = 0; i < 2; ++i) f.foot[i] = footAt(L, i, f.lt);
    // At the very ends of a Rise / Sit the body is exactly the seated one.
    if ((L.kind == StanceLeg::Rise && f.lt <= 0.0f) || (L.kind == StanceLeg::Sit && f.lt >= L.T)) f.active = false;
    return f;
}

// =============================================================================================
// Spine, legs and hands out of the seat
// =============================================================================================
float Animator::Impl::headPitchMin(float w) const { return lerp(-45.0f * DEG, kStandPitchDown, w); }

float Animator::Impl::stanceSpine(const StanceFrame& f, SpineParams& sp, float lookPitch, float idleFlex, float idleTwist, float idleSide) const {
    // A look further down than the neck comfortably takes bends the back: mostly from the hips
    // (the pelvis pitches over the legs), the rest in the spine. setLean bends further over the
    // board. Speaking leans in a little, as seated.
    const float bend = f.standW * kLookBack * std::max(0.0f, -lookPitch - kLookBendFrom);
    const float leanBend = f.standW * kStandLean * lean;
    SpineParams st;
    st.flex = f.spineFlex + 0.40f * bend + idleFlex + 0.022f * speechEnv + 0.02f * speechStress;
    st.twist = idleTwist;
    st.side = idleSide;
    const float w = 1.0f - f.seatW;
    sp.flex = lerp(sp.flex, st.flex, w);
    sp.twist = lerp(sp.twist, st.twist, w);
    sp.side = lerp(sp.side, st.side, w);
    return w * (f.hipFlex + 0.60f * bend + leanBend);
}

// Lowers the pelvis where a leg would have to stretch beyond kLegSoft (smoothly, never beyond
// kLegMax): the safety net of the plans (bending over the board shifts the hips back a little).
float Animator::Impl::pelvisDrop(const Pose& p, const StanceFrame& f) const {
    float drop = 0.0f;
    for (int i = 0; i < 2; ++i) {
        const Bone t = i == 0 ? ThighL : ThighR;
        const vec3 hip = f.pelvis + rotate(qy(f.yaw), rotate(p.local[Pelvis], sk->restOffset[t]));
        const vec3 ankle = f.foot[i].translation();
        const vec3 d = hip - ankle;
        const float D = length(d);
        if (D <= kLegSoft) continue;
        const float Dt = softLimit(D, kLegSoft, kLegMax), r2 = d.x * d.x + d.z * d.z;
        drop = std::max(drop, d.y - std::sqrt(std::max(0.0f, Dt * Dt - r2)));
    }
    return drop;
}

void Animator::Impl::solveLegs(Pose& p, const StanceFrame& f) {
    const float w = 1.0f - f.seatW;
    const quat pel = rotOf(G[Pelvis]);
    for (int i = 0; i < 2; ++i) {
        const Bone tb = i == 0 ? ThighL : ThighR, sb = Bone(tb + 1), fb = Bone(tb + 2);
        const mat4 footB = invBody * f.foot[i];
        const vec3 hip = transformPoint(G[Pelvis], sk->restOffset[tb]);
        const vec3 ankle = footB.translation();
        const float a = length(sk->restOffset[sb]), b = length(sk->restOffset[fb]);
        const vec3 d = ankle - hip;
        const float D = length(d);
        const vec3 u = safeNormalize(d, vec3(0, -1, 0));
        const float Dc = clamp(D, std::fabs(a - b) + 1e-3f, (a + b) * 0.9999f);
        const float cosA = clamp((a * a + Dc * Dc - b * b) / (2.0f * a * Dc), -1.0f, 1.0f);
        const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
        // The knee bends forward along the foot (a little up: seated, the knee is above the line).
        vec3 pole = transformDir(footB, vec3(0.0f, 0.3f, 1.0f));
        vec3 pp = perp(normalize(pole), u);
        if (length(pp) < 1e-3f) pp = perp(rotate(pel, vec3(0, 0, 1)), u);
        pp = safeNormalize(pp, vec3(0, 0, 1));
        const vec3 knee = hip + u * (a * cosA) + pp * (a * sinA);
        const vec3 X = safeNormalize(cross(pp, u), vec3(1, 0, 0));   // the knee's hinge (rest: +X)
        const vec3 Zt = safeNormalize(knee - hip, vec3(0, 0, 1));    // the thigh points along its +Z
        const quat thigh = fromMat3(mat3(X, cross(Zt, X), Zt));
        const vec3 Ys = safeNormalize(knee - (hip + u * Dc), vec3(0, 1, 0));   // the shin along its -Y
        const quat shin = fromMat3(mat3(X, Ys, cross(X, Ys)));
        const quat foot = rotOf(footB);
        const quat lt = normalize(conjugate(pel) * thigh), ls = normalize(conjugate(thigh) * shin), lf = normalize(conjugate(shin) * foot);
        p.local[tb] = w >= 1.0f ? lt : qslerp(p.local[tb], lt, w);
        p.local[sb] = w >= 1.0f ? ls : qslerp(p.local[sb], ls, w);
        p.local[fb] = w >= 1.0f ? lf : qslerp(p.local[fb], lf, w);
    }
}

// The table hand (character space, it stays where it pushes): palm flat a few centimetres in from
// the table's near edge, in front of the shoulder, fingers forward and a little inwards.
HandSample Animator::Impl::tableHand(Side s) const {
    HandSample h;
    const float sx = s == Side::Right ? -1.0f : 1.0f;
    const float tableC = layout::TABLE_TOP_Y - pelvisWorld.y;
    const float edgeZ = toChar(vec3(0, layout::TABLE_TOP_Y, facing * layout::TABLE_DEPTH * 0.5f)).z;
    h.q = handRot(s, -sx * 0.22f, 0.08f, 0.06f);
    h.f = fpHumanize(fpLerp(poseOpenPalm(), poseTableRest(), 0.35f), s == Side::Right ? 0.4f : 0.6f, 0.03f);
    // Lowest point of the palm and the fingers on the table.
    float lowest = 1e9f;
    for (int i = 0; i < 5; ++i) lowest = std::min(lowest, rotate(h.q, handPoint(s, fingerTip(*sk, Side::Right, h.f, i))).y - kPadRadius);
    for (vec3 lp : {palmCenter(Side::Right), vec3(kPalmHalf, -0.020f, 0.0f), vec3(kPalmHalf, -0.075f, 0.02f), vec3(kPalmHalf, -0.075f, -0.02f)})
        lowest = std::min(lowest, rotate(h.q, handPoint(s, lp)).y);
    const vec3 pc = rotate(h.q, handPoint(s, palmCenter(Side::Right)));
    const vec3 contact(sx * 0.19f, tableC, edgeZ + 0.075f);
    h.p = vec3(contact.x - pc.x, contact.y - lowest + 0.0008f, contact.z - pc.z);
    return h;
}

HandSample Animator::Impl::handPoseSample(Side s, HandPose hp, const StanceFrame& f, const HandSample& seatB) const {
    const float sx = s == Side::Right ? -1.0f : 1.0f;   // the arm's side (body X)
    HandSample h;
    switch (hp) {
        case HandPose::Seat: return seatB;
        case HandPose::Table: return handToBody(tableHand(s));
        case HandPose::Thigh: {
            // Palm on the top of the thigh (its front when standing), fingers towards the knee.
            const Bone tb = s == Side::Right ? ThighR : ThighL;
            const mat4& T = G[tb];
            const quat ql = handFrom(s, vec3(0, -1, 0), vec3(-sx * 0.22f, 0.0f, 1.0f));
            h.f = fpHumanize(fpLerp(poseOpenPalm(), poseRelaxed(), 0.55f), s == Side::Right ? 0.2f : 0.9f, 0.04f);
            const vec3 contact(sx * 0.008f, 0.066f, 0.25f);
            h.q = normalize(rotOf(T) * ql);
            h.p = transformPoint(T, contact - rotate(ql, handPoint(s, palmCenter(Side::Right))));
            return h;
        }
        case HandPose::Side:
        case HandPose::BackSide:
        case HandPose::Behind: break;
    }
    // Poses on the lower back's frame (Spine1): the arms hang beside the hips, or the hands are
    // clasped behind the back, the solver's right hand holding its left wrist.
    const mat4& S = G[Spine1];
    vec3 wrist, n, fing;
    FingerPose fp;
    if (hp == HandPose::Side) {
        wrist = vec3(sx * 0.232f, -0.175f, 0.020f);
        n = vec3(-sx, 0.0f, 0.08f);
        fing = vec3(sx * 0.05f, -1.0f, 0.10f);
        fp = fpHumanize(poseRelaxed(), s == Side::Right ? 0.3f : 0.7f, 0.04f);
    } else if (hp == HandPose::BackSide) {
        wrist = vec3(sx * 0.205f, -0.150f, -0.120f);
        n = vec3(-sx * 0.6f, 0.0f, -1.0f);
        fing = vec3(-sx * 0.3f, -1.0f, -0.15f);
        fp = fpHumanize(poseRelaxed(), s == Side::Right ? 0.3f : 0.7f, 0.04f);
    } else if (s == Side::Left) {
        // The held hand: the back of the hand against the small of the back, palm out, the
        // fingers loosely curled towards the other side.
        wrist = vec3(0.050f, -0.040f, -0.142f);
        n = vec3(0.0f, -0.25f, -1.0f);
        fing = vec3(-1.0f, -0.40f, 0.0f);
        fp = fpHumanize(fpLerp(poseRelaxed(), poseLooseFist(), 0.30f), 0.7f, 0.03f);
    } else {
        // The holding hand: palm towards the body over the held wrist, fingers round it.
        wrist = vec3(-0.072f, -0.060f, -0.168f);
        n = vec3(0.0f, 0.10f, 1.0f);
        fing = vec3(1.0f, -0.55f, 0.0f);
        fp = fpHumanize(fpLerp(poseRelaxed(), poseLooseFist(), 0.75f), 0.2f, 0.03f);
    }
    const quat ql = handFrom(s, n, fing);
    h.q = normalize(rotOf(S) * ql);
    h.p = transformPoint(S, wrist);
    h.f = fp;
    return h;
}

HandSample Animator::Impl::stanceHand(Side s, const StanceFrame& f, const HandSample& seatB) const {
    const int hi = s == Side::Right ? 1 : 0;
    HandSample out;
    if (!f.leg) {
        out = handPoseSample(s, HandPose::Behind, f, seatB);
    } else {
        const std::vector<HandMove>& mv = f.leg->hands[hi];
        const HandMove* m = nullptr;
        for (const HandMove& x : mv)
            if (f.lt >= x.t0) m = &x;
        if (!m) {
            out = handPoseSample(s, mv.empty() ? f.leg->hand0 : mv.front().path.front(), f, seatB);
        } else if (f.lt >= m->t1 || m->path.size() < 2) {
            out = handPoseSample(s, m->path.back(), f, seatB);
        } else {
            // Along the path: positions on a Catmull-Rom curve through the poses (minimum jerk in
            // time over the whole move), rotations and fingers from pose to pose.
            const size_t n = m->path.size();
            HandSample k[8];
            for (size_t i = 0; i < n && i < 8; ++i) k[i] = handPoseSample(s, m->path[i], f, seatB);
            const float u = smootherstep((f.lt - m->t0) / std::max(1e-4f, m->t1 - m->t0)) * float(n - 1);
            const size_t i = std::min(n - 2, size_t(u));
            const float v = u - float(i);
            const vec3 p0 = k[i == 0 ? 0 : i - 1].p, p1 = k[i].p, p2 = k[i + 1].p, p3 = k[std::min(n - 1, i + 2)].p;
            const float v2 = v * v, v3 = v2 * v;
            out = k[i];
            out.p = (p1 * 2.0f + (p2 - p0) * v + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * v2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * v3) * 0.5f;
            // Leaving or reaching the table / the seated hand: up a little on the way (no finger
            // drags over the cloth).
            const bool low = m->path[i] == HandPose::Table || m->path[i] == HandPose::Seat || m->path[i + 1] == HandPose::Table ||
                             m->path[i + 1] == HandPose::Seat;
            if (low) out.p.y += 0.025f * bump(v, 0.45f);
            const float w = smootherstep(v);
            out.q = qslerp(k[i].q, k[i + 1].q, w);
            out.f = fpLerp(k[i].f, k[i + 1].f, w);
            out.pen = k[i].pen;
            out.v = out.a = vec3(0);
        }
    }
    // Nothing of the seated hand's special locks survives out of the seat.
    if (out.lockW > 0.0f || out.pinW > 0.0f || out.tipLock) {
        const float w = f.seatW;
        out.lockW *= w;
        out.pinW *= w;
        if (w < 1.0f) out.tipLock = false;
    }
    return out;
}

// =============================================================================================
// Planning
// =============================================================================================
void Animator::Impl::planRiseSit(StanceLeg& L, bool sit) {
    const float S = kChairSlideMax;
    vec3 spot;
    float spotYaw;
    spotChar(Stance::Standing, spot, spotYaw);
    mat4 seatF[2], standF[2];
    seatedFeet(seatF);
    for (int i = 0; i < 2; ++i) standF[i] = standFoot(i, spot, spotYaw);
    const float H = sit ? kSitHold : 0.0f;
    L.T = kRiseT + H;
    // Rise time of a leg time (a Sit plays the Rise backwards after its hold).
    auto riseT = [&](float lt) { return sit ? clamp(kRiseT - (lt - H), 0.0f, kRiseT) : lt; };
    auto legT = [&](float r) { return sit ? H + kRiseT - r : r; };
    auto slideAt = [&](float r) { return S * ramp(r, kPushAt, kPushT); };
    const int n = int(std::ceil(L.T * L.rate)) + 1;
    L.pel.resize(size_t(n));
    L.trunk.resize(size_t(n));
    for (int k = 0; k < n; ++k) {
        const float r = riseT(std::min(L.T, float(k) / L.rate));
        const float slide = slideAt(r);
        // The pelvis rides back with the chair, comes forward over the feet and rises. While the
        // hips pitch forward on the seat they come up a little (the buttocks roll forward).
        const float hip = rise(r, 0.50f, 1.00f, kRiseT, 0.62f);
        vec3 p(spot.x * ramp(r, kForwardAt, kForwardT), spot.y * ramp(r, kUpAt, kUpT),
               -slide + (spot.z + S) * ramp(r, kForwardAt, kForwardT));
        p.y += 0.022f * std::sin(std::min(hip, 0.62f)) * (1.0f - ramp(r, kUpAt, 0.3f));
        L.pel[size_t(k)] = vec4(p, spotYaw * ramp(r, kForwardAt, kForwardT));
        L.trunk[size_t(k)] = vec4(hip, rise(r, 0.55f, 1.00f, kRiseT, 0.22f), slide, ramp(r, 0.75f, 0.85f));
    }
    // Feet: drawn back one after the other while the chair slides (low swings, no roll).
    const float liftL[2] = {0.26f, 0.38f};
    for (int i = 0; i < 2; ++i) {
        FootSwing sw;
        const float r0 = liftL[i], r1 = liftL[i] + 0.30f;
        sw.t0 = std::min(legT(r0), legT(r1));
        sw.t1 = std::max(legT(r0), legT(r1));
        sw.to = sit ? seatF[i] : standF[i];
        sw.lift = 0.035f;
        L.foot0[i] = sit ? standF[i] : seatF[i];
        L.steps[i].push_back(sw);
        TimedEvent e{sw.t1, EventType::Footstep, ActNone, false, true, toWorld(transformPoint(sw.to, vec3(0, -kAnkleY, kSoleZ)))};
        L.events.push_back(e);
    }
    // The chair: pushed back (Rise) / drawn in (Sit) from the seat's centre.
    {
        const float r0 = sit ? kPushAt + kPushT : kPushAt;
        const float zc = -(layout::CHAIR_Z - layout::PLAYER_PELVIS_Z) - (sit ? S : 0.0f);
        TimedEvent e{legT(r0), sit ? EventType::ChairPulled : EventType::ChairPushed, ActNone, false, true,
                     toWorld(vec3(0.0f, layout::SEAT_HEIGHT - pelvisWorld.y, zc))};
        L.events.push_back(e);
    }
    // Hands: to the table edge, pushing; onto the thighs while the robot leans forward and rises;
    // beside the hips and behind the back once up (the writing hand a moment after the other).
    for (int hi = 0; hi < 2; ++hi) {
        const float d = hi == 0 ? 0.03f : 0.0f;
        struct M { float r0, r1; std::vector<HandPose> path; };
        const M rm[3] = {{0.0f + d, 0.24f + d, {HandPose::Seat, HandPose::Table}},
                         {0.42f + d, 0.76f + d, {HandPose::Table, HandPose::Thigh}},
                         {1.08f, kRiseT, {HandPose::Thigh, HandPose::Side, HandPose::BackSide, HandPose::Behind}}};
        for (const M& m : rm) {
            HandMove hm;
            hm.path = m.path;
            if (sit) {
                std::reverse(hm.path.begin(), hm.path.end());
                hm.t0 = legT(m.r1);
                hm.t1 = legT(m.r0);
            } else {
                hm.t0 = m.r0;
                hm.t1 = m.r1;
            }
            L.hands[hi].push_back(hm);
        }
        if (sit) {
            std::reverse(L.hands[hi].begin(), L.hands[hi].end());
            L.hands[hi].front().t0 = 0.0f;   // from the back as the hold starts
        }
    }
    L.hand0 = sit ? HandPose::Behind : HandPose::Seat;
    L.seatIn = 0.25f;
    L.seatOut = L.T - 0.25f;
}

void Animator::Impl::planWalk(StanceLeg& L, Stance from, Stance to) {
    vec3 P0, P1;
    float yaw0, yaw1;
    spotChar(from, P0, yaw0);
    spotChar(to, P1, yaw1);
    // The way round the table corner (character space, the robot's left end; mirrored for the
    // right end): along the front of the chair, a little towards the table to keep the calves
    // clear of the seat's front edge, past the corner, then towards the end of the table.
    const Stance side = from == Stance::Standing ? to : from;
    vec3 sideP;
    float sideYaw;
    spotChar(side, sideP, sideYaw);
    const float s = sideP.x >= 0.0f ? 1.0f : -1.0f;
    std::vector<vec3> pts = {P0};
    const vec3 via[3] = {vec3(s * 0.30f, 0.0f, -0.050f), vec3(s * 0.64f, 0.0f, -0.040f), vec3(s * 0.85f, 0.0f, 0.110f)};
    if (from == Stance::Standing)
        for (const vec3& v : via) pts.push_back(v);
    else
        for (int i = 2; i >= 0; --i) pts.push_back(via[i]);
    pts.push_back(P1);
    for (vec3& p : pts) p.y = 0.0f;
    // Dense polyline along a centripetal Catmull-Rom through the points, with its arc length.
    std::vector<vec3> poly;
    std::vector<float> arc;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const vec3 p0 = pts[i == 0 ? 0 : i - 1], p1 = pts[i], p2 = pts[i + 1], p3 = pts[std::min(pts.size() - 1, i + 2)];
        auto knot = [](vec3 a, vec3 b) { return std::sqrt(std::max(1e-6f, length(b - a))); };
        const float t0 = 0.0f, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
        for (int k = 0; k < 64; ++k) {
            const float t = t1 + (t2 - t1) * float(k) / 64.0f;
            auto L1 = [](vec3 a, vec3 b, float ta, float tb, float t) { return tb - ta < 1e-6f ? a : a * ((tb - t) / (tb - ta)) + b * ((t - ta) / (tb - ta)); };
            const vec3 A1 = L1(p0, p1, t0, t1, t), A2 = L1(p1, p2, t1, t2, t), A3 = L1(p2, p3, t2, t3, t);
            const vec3 B1 = L1(A1, A2, t0, t2, t), B2 = L1(A2, A3, t1, t3, t);
            poly.push_back(L1(B1, B2, t1, t2, t));
        }
    }
    poly.push_back(pts.back());
    arc.push_back(0.0f);
    for (size_t i = 1; i < poly.size(); ++i) arc.push_back(arc.back() + length(poly[i] - poly[i - 1]));
    const float len = arc.back();
    auto along = [&](float sArc, vec3* dir) {
        sArc = clamp(sArc, 0.0f, len);
        size_t i = 1;
        while (i + 1 < arc.size() && arc[i] < sArc) ++i;
        const float u = (sArc - arc[i - 1]) / std::max(1e-6f, arc[i] - arc[i - 1]);
        if (dir) *dir = safeNormalize(poly[i] - poly[i - 1], vec3(0, 0, 1));
        return lerp(poly[i - 1], poly[i], u);
    };
    // Steps: the foot on the side the robot sets off towards leads; the last two land on the
    // spot's feet. Timing from the length of the way.
    const int nSteps = std::max(4, int(std::ceil(len / kStepLength)) + 1);
    const float lastLand = kFirstLift + kStepSwing + float(nSteps - 1) * kStepPeriod;
    L.T = lastLand + kWalkSettle;
    const float Tw = lastLand + 0.10f;   // the pelvis arrives
    // Progress along the way: speeds up over the first quarter, slows down over the last third.
    const int n = int(std::ceil(L.T * L.rate)) + 1;
    std::vector<float> prog(static_cast<size_t>(n), 0.0f);
    {
        float acc = 0.0f;
        std::vector<float> v(static_cast<size_t>(n));
        for (int k = 0; k < n; ++k) {
            const float u = clamp((float(k) / L.rate) / Tw, 0.0f, 1.0f);
            v[size_t(k)] = smoothstep(0.0f, 0.30f, u) * smoothstep(1.0f, 0.62f, u);
        }
        for (int k = 1; k < n; ++k) {
            acc += 0.5f * (v[size_t(k)] + v[size_t(k) - 1]);
            prog[size_t(k)] = acc;
        }
        for (float& p : prog) p = acc > 0.0f ? p / acc * len : 0.0f;
    }
    // Body yaw: from the start's towards the way's direction, and to the end's over the last steps.
    std::vector<float> tangent(static_cast<size_t>(n));
    {
        float prev = yaw0;
        for (int k = 0; k < n; ++k) {
            vec3 d;
            along(std::max(prog[size_t(k)], 0.02f), &d);
            const float a = wrapNear(std::atan2(d.x, d.z), prev);
            tangent[size_t(k)] = a;
            prev = a;
        }
    }
    const float endYaw = wrapNear(yaw1, tangent.back());
    L.pel.resize(size_t(n));
    for (int k = 0; k < n; ++k) {
        const float t = float(k) / L.rate;
        float yaw = lerp(yaw0, tangent[size_t(k)], ramp(t, 0.0f, 0.60f));
        yaw = lerp(yaw, endYaw, ramp(t, Tw - 0.80f, 0.80f));
        const vec3 p = along(prog[size_t(k)], nullptr);
        L.pel[size_t(k)] = vec4(p.x, 0.0f, p.z, yaw);
    }
    auto pelAt = [&](float t) { return sampleCurve(L.pel, L.rate, t); };
    // Feet.
    mat4 startF[2], endF[2];
    for (int i = 0; i < 2; ++i) {
        startF[i] = standFoot(i, P0, yaw0);
        endF[i] = standFoot(i, P1, yaw1);
        L.foot0[i] = startF[i];
    }
    vec3 d0 = pts[1] - pts[0];
    const int lead = dot(d0, rotate(qy(yaw0), vec3(1, 0, 0))) >= 0.0f ? 0 : 1;
    for (int k = 0; k < nSteps; ++k) {
        const int i = (lead + k) & 1;
        const float sx = i == 0 ? 1.0f : -1.0f;
        FootSwing sw;
        sw.t1 = kFirstLift + kStepSwing + float(k) * kStepPeriod;
        sw.t0 = sw.t1 - kStepSwing;
        if (k >= nSteps - 2) {
            sw.to = endF[i];
        } else {
            // Where the pelvis will be halfway through this foot's stance.
            const float tau = std::min(Tw, sw.t1 + kStepPeriod - 0.5f * kStepSwing);
            const vec4 pp = pelAt(tau);
            vec3 a = vec3(pp.x, 0.0f, pp.z) + rotate(qy(pp.w), vec3(sx * kWalkSide, 0.0f, 0.0f));
            a.y = kAnkleY - pelvisWorld.y;
            sw.to = toMat4(qy(pp.w + sx * kToeOut * 0.5f), a);
        }
        sw.lift = 0.045f;
        sw.roll = (k == 0 || k >= nSteps - 1) ? 0.4f : 1.0f;
        L.steps[i].push_back(sw);
        TimedEvent e{sw.t1, EventType::Footstep, ActNone, false, true, toWorld(transformPoint(sw.to, vec3(0, -kAnkleY, kSoleZ)))};
        L.events.push_back(e);
    }
    // Pelvis height: lower while walking, and wherever a leg would have to stretch, low enough
    // for both (min filter, then smoothed: the head stays steady).
    std::vector<float> hy(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        const float t = float(k) / L.rate;
        const float walkW = ramp(t, 0.0f, 0.45f) * (1.0f - ramp(t, Tw - 0.45f, 0.45f));
        float y = lerp(P0.y, P1.y, ramp(t, 0.0f, Tw)) - kWalkDip * walkW;
        const vec4 pp = L.pel[size_t(k)];
        for (int i = 0; i < 2; ++i) {
            const Bone tb = i == 0 ? ThighL : ThighR;
            const vec3 hipOff = rotate(qy(pp.w), sk->restOffset[tb]);
            const vec3 ank = footAt(L, i, t).translation();
            const float dx = pp.x + hipOff.x - ank.x, dz = pp.z + hipOff.z - ank.z;
            const float r2 = dx * dx + dz * dz;
            const float maxY = ank.y + std::sqrt(std::max(0.0f, kLegPlan * kLegPlan - r2)) - hipOff.y;
            y = std::min(y, maxY);
        }
        hy[size_t(k)] = y;
    }
    {
        const int wmin = int(0.10f * L.rate), wblur = int(0.07f * L.rate);
        std::vector<float> lo(hy.size()), sm(hy.size());
        for (int k = 0; k < n; ++k) {
            float m = hy[size_t(k)];
            for (int j = std::max(0, k - wmin); j <= std::min(n - 1, k + wmin); ++j) m = std::min(m, hy[size_t(j)]);
            lo[size_t(k)] = m;
        }
        for (int k = 0; k < n; ++k) {
            float acc = 0.0f, wsum = 0.0f;
            for (int j = -2 * wblur; j <= 2 * wblur; ++j) {
                const float w = std::exp(-0.5f * float(j * j) / float(wblur * wblur));
                acc += w * lo[size_t(clamp(float(k + j), 0.0f, float(n - 1)))];
                wsum += w;
            }
            sm[size_t(k)] = acc / wsum;
        }
        // The ends exactly on the spots.
        const float e0 = P0.y - sm.front(), e1 = P1.y - sm.back();
        for (int k = 0; k < n; ++k) {
            const float t = float(k) / L.rate;
            L.pel[size_t(k)].y = sm[size_t(k)] + e0 * (1.0f - ramp(t, 0.0f, 0.3f)) + e1 * ramp(t, L.T - 0.35f, 0.3f);
        }
    }
    L.trunk.assign(size_t(n), vec4(0.0f, 0.0f, kChairSlideMax, 1.0f));
    for (int k = 0; k < n; ++k) {
        const float t = float(k) / L.rate;
        // A slight forward lean while walking.
        L.trunk[size_t(k)].x = 0.04f * ramp(t, 0.0f, 0.5f) * (1.0f - ramp(t, Tw - 0.5f, 0.5f));
    }
    L.pel.back() = vec4(P1, endYaw);
    L.hand0 = HandPose::Behind;
}

// =============================================================================================
// The leg machine
// =============================================================================================
void Animator::Impl::startLeg(Stance to) {
    auto L = std::make_shared<StanceLeg>();
    L->from = stanceAt;
    L->to = to;
    L->start = time;
    if (stanceAt == Stance::Seated) {
        L->kind = StanceLeg::Rise;
        planRiseSit(*L, false);
    } else if (to == Stance::Seated) {
        L->kind = StanceLeg::Sit;
        planRiseSit(*L, true);
    } else {
        L->kind = StanceLeg::Walk;
        planWalk(*L, stanceAt, to);
    }
    for (TimedEvent& e : L->events) e.t += time;
    leg = L;
    if (debugLog) LOGI("anim: stance %d -> %d (%.2f s) at t=%.3f", int(L->from), int(L->to), L->T, time);
}

bool Animator::Impl::nextStanceBoundary(float& t) const {
    if (leg) {
        t = leg->start + leg->T;
        return true;
    }
    if (stanceAt == stanceTarget) return false;
    if (stanceAt == Stance::Seated) {
        // A stance waits for both hands to be done (and no piece in the hand).
        if (running || !queue.empty() || wr.running || !wr.queue.empty() || time < wr.suspendUntil) return false;
        if (hands[1].heldId >= 0 || hands[1].capId >= 0) return false;
    }
    t = time;
    return true;
}

void Animator::Impl::fireStanceDue(float upTo, std::vector<Event>& ev) {
    if (!leg) return;
    for (TimedEvent& e : leg->events) {
        if (e.done || e.t > upTo + 1e-6f) continue;
        e.done = true;
        Event out;
        out.type = e.type;
        out.time = e.t;
        out.position = e.pos;
        out.tag = int(leg->to);
        ev.push_back(out);
    }
}

void Animator::Impl::stepStance(std::vector<Event>& ev) {
    if (leg && time >= leg->start + leg->T - 1e-6f) {
        fireStanceDue(leg->start + leg->T, ev);
        stanceAt = leg->to;
        leg.reset();
        stanceSince = time;
        if (stanceAt == stanceTarget) {
            Event e;
            e.type = EventType::StanceReached;
            e.time = time;
            e.tag = int(stanceAt);
            ev.push_back(e);
        }
        if (stanceAt == Stance::Seated && stanceTarget == Stance::Seated && penPutForStance) {
            // The pen the robot laid down to get up: back in the hand before anything is written.
            penPutForStance = false;
            if (!wr.penHeld && !(!wr.queue.empty() && wr.queue.front().type == WriteTaskType::PickPen)) {
                WriteTask pick;
                pick.type = WriteTaskType::PickPen;
                pick.frame = wr.penTable;
                wr.queue.push_front(pick);
            }
        }
    }
    if (leg || stanceAt == stanceTarget) return;
    if (stanceAt == Stance::Seated) {
        if (running || !queue.empty() || wr.running || !wr.queue.empty() || time < wr.suspendUntil) return;
        if (wr.penHeld) {
            // The pen goes down first, where it was taken from (the writing hand's own PutPen).
            WriteTask put;
            put.type = WriteTaskType::PutPen;
            put.frame = wr.penTable;
            wr.queue.push_back(put);
            penPutForStance = true;
            return;
        }
        // Idle chin poses end here. The seated hands (their motions) glide back to their rests
        // while the hands go to the table: a Sit ends on them there.
        for (Hand* h : {&left(), &right()}) {
            bakeFollow(*h);
            const HandSample from = h->motion.sample(time);
            if (length(from.p - h->rest.p) > 1e-3f || dot(from.q, h->rest.q) < 0.99999f) {
                Motion mo;
                mo.start = time;
                mo.segs.push_back(makeSeg(from, 0.40f, h->rest.p, vec3(0), h->rest.q, h->rest.f));
                h->motion = mo;
            }
        }
        leftChin = rightChin = 0;
        startLeg(Stance::Standing);
    } else if (stanceAt == Stance::Standing) {
        startLeg(stanceTarget);
    } else {
        startLeg(Stance::Standing);
    }
}

// =============================================================================================
// Public API
// =============================================================================================
void Animator::setStance(Stance target) {
    if (!impl_) return;
    impl_->stanceTarget = target;
}
Stance Animator::stanceTarget() const { return impl_ ? impl_->stanceTarget : Stance::Seated; }
Stance Animator::stance() const { return impl_ ? impl_->stanceAt : Stance::Seated; }
bool Animator::seated() const { return !impl_ || impl_->seatedNow(); }
bool Animator::stanceMoving() const { return impl_ && impl_->leg != nullptr; }
float Animator::chairSlide() const {
    if (!impl_ || !impl_->sk) return 0.0f;
    return impl_->stanceFrame(impl_->time).slide;
}

}  // namespace anim
