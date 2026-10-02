// Internal header of the animation package: shared by animator.cpp (playing hand, body, gaze,
// task machine), animator_gesture.cpp (the coach's gestures, speech, nods and head shakes) and
// animator_writing.cpp (writing hand, pen, page turns, left-handed mirror).
// Not part of the public API (see animator.h).
//
// Everything is planned in CHARACTER space (+Y up, +Z forward, +X = character's left, origin at
// the pelvis joint). The root never moves, so character space is inertial; world inputs (pieces,
// clock, partner) are converted once when a task starts.
//
// Left-handed play (Animator::init with Side::Left) runs this right-handed solver in a world
// mirrored about X = 0 (both players sit on that plane): hands[1] ("right") is always the playing
// hand and hands[0] ("left") the writing hand INSIDE the solver; the public API mirrors its inputs
// and outputs (see Impl::mirrored).
#pragma once
#include "animator.h"
#include "../core/log.h"
#include "../game/layout.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>

namespace anim {
namespace detail {
using namespace m;
using namespace character;

// =============================================================================================
// 1. Math helpers
// =============================================================================================
const vec3 kX(1, 0, 0), kY(0, 1, 0), kZ(0, 0, 1);

inline float minJerk(float u) { return smootherstep(u); }
inline quat qx(float a) { return axisAngle(kX, a); }
inline quat qy(float a) { return axisAngle(kY, a); }
inline quat qz(float a) { return axisAngle(kZ, a); }
inline quat rotOf(const mat4& mm) { return fromMat3(mm.upper3()); }
inline vec3 perp(vec3 v, vec3 n) { return v - n * dot(v, n); }  // n unit
inline vec3 safeNormalize(vec3 v, vec3 fallback) {
    float l = length(v);
    return l > 1e-7f ? v / l : fallback;
}
inline float wrapPi(float a) {
    while (a > PI) a -= TAU;
    while (a < -PI) a += TAU;
    return a;
}
inline quat qslerp(quat a, quat b, float t) { return normalize(slerp(a, b, t)); }
inline float softLimit(float x, float knee, float limit) {
    // Identity below 'knee', smoothly saturating to 'limit' above it (x >= 0).
    if (x <= knee) return x;
    float r = limit - knee;
    return knee + r * std::tanh((x - knee) / r);
}

// Quintic Hermite (minimum jerk with boundary velocity/acceleration) over duration T.
struct Quintic1 {
    static void eval(float p0, float v0, float a0, float p1, float v1, float a1, float T, float t, float& p, float& v, float& a) {
        if (T <= 1e-6f) { p = p1; v = v1; a = a1; return; }
        float u = clamp(t / T, 0.0f, 1.0f);
        float V0 = v0 * T, V1 = v1 * T, A0 = a0 * T * T, A1 = a1 * T * T;
        float u2 = u * u, u3 = u2 * u, u4 = u3 * u, u5 = u4 * u;
        float h0 = 1 - 10 * u3 + 15 * u4 - 6 * u5, h1 = u - 6 * u3 + 8 * u4 - 3 * u5;
        float h2 = 0.5f * u2 - 1.5f * u3 + 1.5f * u4 - 0.5f * u5, h3 = 0.5f * u3 - u4 + 0.5f * u5;
        float h4 = -4 * u3 + 7 * u4 - 3 * u5, h5 = 10 * u3 - 15 * u4 + 6 * u5;
        float d0 = -30 * u2 + 60 * u3 - 30 * u4, d1 = 1 - 18 * u2 + 32 * u3 - 15 * u4;
        float d2 = u - 4.5f * u2 + 6 * u3 - 2.5f * u4, d3 = 1.5f * u2 - 4 * u3 + 2.5f * u4;
        float d4 = -12 * u2 + 28 * u3 - 15 * u4, d5 = 30 * u2 - 60 * u3 + 30 * u4;
        float s0 = -60 * u + 180 * u2 - 120 * u3, s1 = -36 * u + 96 * u2 - 60 * u3;
        float s2 = 1 - 9 * u + 18 * u2 - 10 * u3, s3 = 3 * u - 12 * u2 + 10 * u3;
        float s4 = -24 * u + 84 * u2 - 60 * u3, s5 = 60 * u - 180 * u2 + 120 * u3;
        p = h0 * p0 + h1 * V0 + h2 * A0 + h3 * A1 + h4 * V1 + h5 * p1;
        v = (d0 * p0 + d1 * V0 + d2 * A0 + d3 * A1 + d4 * V1 + d5 * p1) / T;
        a = (s0 * p0 + s1 * V0 + s2 * A0 + s3 * A1 + s4 * V1 + s5 * p1) / (T * T);
        if (t > T) { p += v1 * (t - T); v = v1; a = 0; }  // (callers never extrapolate far)
    }
};

// Smooth bump: 0 at u=0 and u=1 with zero velocity and acceleration, 1 at u=peak.
inline float bump(float u, float peak) {
    if (u <= 0.0f || u >= 1.0f) return 0.0f;
    peak = clamp(peak, 0.2f, 0.8f);
    float a = 3.0f, b = a * (1.0f - peak) / peak;
    float norm = std::pow(peak, a) * std::pow(1.0f - peak, b);
    return std::pow(u, a) * std::pow(1.0f - u, b) / norm;
}

// =============================================================================================
// 2. Hand model
// =============================================================================================
enum Finger { Thumb = 0, Index = 1, Middle = 2, Ring = 3, Pinky = 4 };

// Per finger: [0] spread (fingers: + towards the thumb side) / opposition (thumb, towards the
// palm), [1..3] flexion of joints 1..3 (radians, + = curl towards the palm).
struct FingerPose {
    float v[5][4] = {};
};
inline FingerPose fpLerp(const FingerPose& a, const FingerPose& b, float t) {
    FingerPose r;
    for (int f = 0; f < 5; ++f)
        for (int j = 0; j < 4; ++j) r.v[f][j] = a.v[f][j] + (b.v[f][j] - a.v[f][j]) * t;
    return r;
}
inline FingerPose fpMake(std::initializer_list<std::initializer_list<float>> rows) {
    FingerPose p;
    int f = 0;
    for (auto& row : rows) {
        int j = 0;
        for (float x : row) p.v[f][j++] = x;
        ++f;
    }
    return p;
}
// Adds a little per-finger variation (so the hand does not look like a stamped part). seed 0..1.
inline FingerPose fpHumanize(FingerPose p, float seed, float amount) {
    for (int f = 1; f < 5; ++f)
        for (int j = 1; j < 4; ++j) {
            float n = std::sin(seed * 17.3f + float(f) * 2.1f + float(j) * 0.7f);
            p.v[f][j] += n * amount * (0.6f + 0.25f * float(f));
        }
    return p;
}

// Presets (tuned in the anim viewer, right-hand convention; the left hand uses the same values).
// Thumb row: {opposition, CMC flex, MCP flex, IP flex}. Finger rows: {spread, MCP, PIP, DIP}.
inline const FingerPose& poseRelaxed() {
    static FingerPose p = fpMake({{0.30f, 0.15f, 0.20f, 0.15f}, {0.04f, 0.22f, 0.38f, 0.22f}, {0.0f, 0.28f, 0.44f, 0.24f},
                                  {-0.04f, 0.34f, 0.50f, 0.28f}, {-0.09f, 0.40f, 0.55f, 0.30f}});
    return p;
}
inline const FingerPose& poseTableRest() {
    static FingerPose p = fpMake({{0.20f, 0.10f, 0.15f, 0.12f}, {0.05f, 0.10f, 0.32f, 0.18f}, {0.0f, 0.14f, 0.36f, 0.20f},
                                  {-0.05f, 0.19f, 0.40f, 0.22f}, {-0.11f, 0.24f, 0.44f, 0.24f}});
    return p;
}
// Ring/pinky closed around a captured piece (applied on top of the pinch values for thumb..middle).
inline const FingerPose& posePocketClosed() {
    static FingerPose p = fpMake({{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {-0.02f, 1.00f, 1.45f, 0.85f}, {-0.06f, 1.08f, 1.45f, 0.85f}});
    return p;
}
inline const FingerPose& posePocketOpen() {
    static FingerPose p = fpMake({{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {-0.08f, 0.55f, 0.70f, 0.35f}, {-0.16f, 0.60f, 0.70f, 0.35f}});
    return p;
}
inline const FingerPose& posePress() {
    static FingerPose p = fpMake({{0.55f, 0.20f, 0.35f, 0.30f}, {0.02f, 0.30f, 0.30f, 0.12f}, {-0.02f, 0.30f, 0.32f, 0.12f},
                                  {-0.06f, 1.05f, 1.25f, 0.65f}, {-0.12f, 1.15f, 1.25f, 0.65f}});
    return p;
}
inline const FingerPose& poseShakeOpen() {
    static FingerPose p = fpMake({{0.18f, -0.12f, 0.10f, 0.08f}, {0.06f, 0.10f, 0.12f, 0.06f}, {0.0f, 0.12f, 0.14f, 0.06f},
                                  {-0.05f, 0.16f, 0.16f, 0.08f}, {-0.10f, 0.20f, 0.18f, 0.08f}});
    return p;
}
inline const FingerPose& poseShakeGrip() {
    static FingerPose p = fpMake({{0.25f, 0.10f, 0.25f, 0.20f}, {0.02f, 0.55f, 0.85f, 0.45f}, {0.0f, 0.60f, 0.90f, 0.45f},
                                  {-0.03f, 0.65f, 0.92f, 0.45f}, {-0.07f, 0.72f, 0.92f, 0.45f}});
    return p;
}
inline const FingerPose& poseLooseFist() {
    static FingerPose p = fpMake({{0.75f, 0.30f, 0.40f, 0.30f}, {0.02f, 1.05f, 1.30f, 0.70f}, {0.0f, 1.12f, 1.35f, 0.72f},
                                  {-0.03f, 1.18f, 1.38f, 0.72f}, {-0.06f, 1.24f, 1.40f, 0.72f}});
    return p;
}
// Pointing: the index extended (a slight natural flexion), the other fingers curled into the palm,
// the thumb tucked against the curled middle finger (its pad beside the middle phalanx).
inline const FingerPose& posePoint() {
    static FingerPose p = fpMake({{1.50f, -0.35f, 1.05f, 0.85f}, {0.02f, 0.10f, 0.10f, 0.05f}, {0.0f, 1.45f, 1.70f, 1.00f},
                                  {-0.03f, 1.50f, 1.65f, 0.95f}, {-0.06f, 1.55f, 1.50f, 0.80f}});
    return p;
}
// Presenting / open hand: fingers loosely extended and a little spread, thumb out.
inline const FingerPose& poseOpenPalm() {
    static FingerPose p = fpMake({{0.10f, -0.05f, 0.05f, 0.05f}, {0.10f, 0.08f, 0.10f, 0.05f}, {0.02f, 0.10f, 0.12f, 0.06f},
                                  {-0.08f, 0.14f, 0.15f, 0.07f}, {-0.16f, 0.18f, 0.17f, 0.08f}});
    return p;
}
// Baton beats: loosely open, the fingers a little more curled towards the little finger.
inline const FingerPose& poseBeat() {
    static FingerPose p = fpMake({{0.30f, 0.10f, 0.18f, 0.12f}, {0.04f, 0.18f, 0.26f, 0.14f}, {0.0f, 0.26f, 0.36f, 0.20f},
                                  {-0.04f, 0.36f, 0.46f, 0.26f}, {-0.09f, 0.46f, 0.54f, 0.30f}});
    return p;
}

// Finger bone of 'side': finger f (0 thumb .. 4 pinky), joint j (0..2).
inline Bone fingerBone(Side s, int f, int j) { return Bone((s == Side::Right ? ThumbR1 : ThumbL1) + f * 3 + j); }
inline Bone armBone(Side s, Bone leftBone) { return sideBone(leftBone, s); }
inline float palmSign(Side s) { return s == Side::Right ? 1.0f : -1.0f; }   // palm normal = (palmSign,0,0)
inline float sideX(Side s) { return s == Side::Right ? -1.0f : 1.0f; }      // the arm's side along X

// Local rotations of the three joints of finger f.
inline void fingerLocal(Side side, const FingerPose& fp, int f, quat out[3]) {
    if (f == Thumb) {
        float ps = palmSign(side);
        out[0] = qy(ps * fp.v[0][0]) * qx(fp.v[0][1]);
        out[1] = qx(fp.v[0][2]);
        out[2] = qx(fp.v[0][3]);
        return;
    }
    vec3 curl = fingerCurlAxis(side);
    out[0] = qx(-fp.v[f][0]) * axisAngle(curl, fp.v[f][1]);
    out[1] = axisAngle(curl, fp.v[f][2]);
    out[2] = axisAngle(curl, fp.v[f][3]);
}
inline void fingerLocals(Side side, const FingerPose& fp, quat out[5][3]) {
    for (int f = 0; f < 5; ++f) fingerLocal(side, fp, f, out[f]);
}
inline void applyFingers(const Skeleton&, Pose& pose, Side side, const FingerPose& fp) {
    quat q[5][3];
    fingerLocals(side, fp, q);
    for (int f = 0; f < 5; ++f)
        for (int j = 0; j < 3; ++j) pose.local[fingerBone(side, f, j)] = q[f][j];
}

constexpr float kPadRadius = 0.0068f;   // distal finger half-thickness (contact pads)
constexpr float kPalmHalf = 0.0135f;    // half thickness of the palm

// Hand-local frames of the three phalanges of finger f.
inline void fingerFrames(const Skeleton& sk, Side side, const FingerPose& fp, int f, mat4 out[3]) {
    quat q[3];
    fingerLocal(side, fp, f, q);
    mat4 m;
    for (int j = 0; j < 3; ++j) {
        Bone b = fingerBone(side, f, j);
        m = m * toMat4(q[j], sk.restOffset[b]);
        out[j] = m;
    }
}
// Local direction of a finger bone (towards its tip).
inline vec3 boneDir(const Skeleton& sk, Bone b) {
    // The last phalanx points along its own rest offset (fingers: -Y, thumb: diagonal).
    return normalize(sk.restOffset[b]);
}
// Hand-local tip (bone end) and pad contact point of finger f.
inline vec3 fingerTipFrom(const Skeleton& sk, Side side, const mat4 fr[3], int f) {   // fr: its fingerFrames
    Bone b3 = fingerBone(side, f, 2);
    return transformPoint(fr[2], boneDir(sk, b3) * sk.boneLength[b3]);
}
inline vec3 fingerTip(const Skeleton& sk, Side side, const FingerPose& fp, int f) {
    mat4 fr[3];
    fingerFrames(sk, side, fp, f, fr);
    return fingerTipFrom(sk, side, fr, f);
}
inline vec3 fingerPad(const Skeleton& sk, Side side, const FingerPose& fp, int f) {
    mat4 fr[3];
    fingerFrames(sk, side, fp, f, fr);
    Bone b3 = fingerBone(side, f, 2);
    vec3 d = boneDir(sk, b3);
    vec3 padDir = f == Thumb ? normalize(cross(kX, d)) : vec3(palmSign(side), 0, 0);
    return transformPoint(fr[2], d * (sk.boneLength[b3] * 0.72f) + padDir * kPadRadius);
}
// Hand-local point on the middle of a phalanx (for the pocket grip).
inline vec3 phalanxMidFrom(const Skeleton& sk, Side side, const mat4 fr[3], int f, int j) {   // fr: its fingerFrames
    Bone b = fingerBone(side, f, j);
    return transformPoint(fr[j], boneDir(sk, fingerBone(side, f, 2)) * (sk.boneLength[b] * 0.5f));
}
inline vec3 phalanxMid(const Skeleton& sk, Side side, const FingerPose& fp, int f, int j) {
    mat4 fr[3];
    fingerFrames(sk, side, fp, f, fr);
    return phalanxMidFrom(sk, side, fr, f, j);
}
// Hand-local palm centre on the palm surface.
inline vec3 palmCenter(Side side) { return vec3(palmSign(side) * kPalmHalf, -0.052f, 0.003f); }

// ---- Finger IK ------------------------------------------------------------------------------
constexpr float kDipCoupling = 0.62f;   // DIP flexion = k * PIP flexion (tendon coupling)

// Pad of a long finger in its flexion plane: (b = towards the palm, a = along the finger).
inline vec2 padPlanar(float L1, float L2, float L3, float t1, float t2) {
    float p1 = t1, p2 = t1 + t2, p3 = t1 + t2 + kDipCoupling * t2;
    return vec2(std::sin(p1), std::cos(p1)) * L1 + vec2(std::sin(p2), std::cos(p2)) * L2 +
           vec2(std::sin(p3), std::cos(p3)) * (0.72f * L3) + vec2(std::cos(p3), -std::sin(p3)) * kPadRadius;
}
// Spread/MCP/PIP/DIP so the pad of finger f (1..4) reaches the hand-local target (least squares
// when out of reach). Returns the remaining distance.
inline float solveLongFinger(const Skeleton& sk, Side side, int f, vec3 target, float out[4], float t1 = 0.5f, float t2 = 0.6f) {
    Bone b1 = fingerBone(side, f, 0);
    vec3 v = target - sk.restOffset[b1];
    float s = std::atan2(v.z, std::max(1e-4f, -v.y));
    vec3 d0(0, -std::cos(s), std::sin(s));
    vec2 want(v.x * palmSign(side), dot(v, d0));
    float L1 = sk.boneLength[b1], L2 = sk.boneLength[b1 + 1], L3 = sk.boneLength[b1 + 2];
    vec2 p;
    for (int it = 0; it < 40; ++it) {
        p = padPlanar(L1, L2, L3, t1, t2);
        vec2 e = want - p;
        if (length(e) < 2e-5f) break;
        const float h = 1e-3f;
        vec2 j1 = (padPlanar(L1, L2, L3, t1 + h, t2) - p) / h, j2 = (padPlanar(L1, L2, L3, t1, t2 + h) - p) / h;
        const float lambda = 1e-4f;
        float a11 = dot(j1, j1) + lambda, a12 = dot(j1, j2), a22 = dot(j2, j2) + lambda;
        float r1 = dot(j1, e), r2 = dot(j2, e), det = a11 * a22 - a12 * a12;
        if (std::fabs(det) < 1e-12f) break;
        float d1 = (a22 * r1 - a12 * r2) / det, d2 = (a11 * r2 - a12 * r1) / det;
        float step = std::min(1.0f, 0.35f / std::max(1e-6f, std::sqrt(d1 * d1 + d2 * d2)));
        t1 = clamp(t1 + d1 * step, -0.35f, 1.60f);
        t2 = clamp(t2 + d2 * step, 0.0f, 1.85f);
    }
    out[0] = clamp(s, -0.35f, 0.35f);
    out[1] = t1;
    out[2] = t2;
    out[3] = kDipCoupling * t2;
    return length(want - padPlanar(L1, L2, L3, t1, t2));
}
// Thumb opposition/CMC/MCP (IP coupled) so its pad reaches the hand-local target.
inline float solveThumb(const Skeleton& sk, Side side, vec3 target, float out[4], vec3 guess = vec3(0.8f, 0.15f, 0.3f)) {
    auto padOf = [&](vec3 x) {
        FingerPose p;
        p.v[0][0] = x.x;
        p.v[0][1] = x.y;
        p.v[0][2] = x.z;
        p.v[0][3] = 0.8f * x.z;
        return fingerPad(sk, side, p, Thumb);
    };
    vec3 x = guess;
    const vec3 lo(-0.3f, -0.6f, -0.3f), hi(1.5f, 1.0f, 1.3f);
    for (int it = 0; it < 40; ++it) {
        vec3 p = padOf(x), e = target - p;
        if (length(e) < 2e-5f) break;
        const float h = 1e-3f;
        mat3 J(( padOf(x + vec3(h, 0, 0)) - p) / h, (padOf(x + vec3(0, h, 0)) - p) / h, (padOf(x + vec3(0, 0, h)) - p) / h);
        mat3 Jt = transpose(J);
        mat3 A = Jt * J;
        for (int i = 0; i < 3; ++i) A.c[i][i] += 2e-4f;
        vec3 d = inverse(A) * (Jt * e);
        float step = std::min(1.0f, 0.3f / std::max(1e-6f, length(d)));
        x = clamp(x + d * step, -10.0f, 10.0f);
        x = vec3(clamp(x.x, lo.x, hi.x), clamp(x.y, lo.y, hi.y), clamp(x.z, lo.z, hi.z));
    }
    out[0] = x.x;
    out[1] = x.y;
    out[2] = x.z;
    out[3] = 0.8f * x.z;
    return length(target - padOf(x));
}

struct PinchGeo {
    FingerPose pose;        // fingers closed on a piece of the given grip radius
    FingerPose open;        // pre-grasp aperture for the same piece
    vec3 point;             // hand-local grip point (on the piece axis, between the pads)
    vec3 axis;              // hand-local unit vector thumb pad -> index pad
    float err = 0;          // worst pad miss (m)
};
// Hand-local grip point of the pinch (5 cm in front of the palm, 10 cm from the wrist).
const vec3 kPinchPoint(0.045f, -0.104f, 0.029f);
// Preferred thumb->index direction (hand-local): towards the back of the hand, along the fingers
// and a little towards the little finger: thumb on the near-left, index on the far-right side.
const vec3 kPinchAxisPref = normalize(vec3(-0.95f, -1.0f, -0.32f));

// Pinch grasp for hand rotation R (character space) on a vertical piece of grip radius r:
// the pads of thumb, index and middle are solved onto the piece surface around a horizontal axis.
// 'aperture' scales the pre-grasp opening (smaller between close neighbours). 'closed': a pinch
// already solved for the same R and r, whose closed pose (independent of the aperture) is reused.
inline PinchGeo pinchFor(const Skeleton& sk, quat R, float r, float aperture = 1.0f, const PinchGeo* closed = nullptr) {
    PinchGeo g;
    g.point = kPinchPoint;
    vec3 aw = rotate(R, kPinchAxisPref);
    aw.y = 0.0f;
    aw = safeNormalize(aw, safeNormalize(vec3(rotate(R, vec3(0, -1, 0)).x, 0, rotate(R, vec3(0, -1, 0)).z), vec3(0, 0, 1)));
    vec3 al = rotate(conjugate(R), aw);           // hand-local horizontal pinch axis
    vec3 up = rotate(conjugate(R), vec3(0, 1, 0)); // hand-local piece axis
    g.axis = al;
    // Middle finger: on the far side too, turned 38 degrees about the piece towards the ulnar
    // side (hand -Z) and 9 mm lower than the index.
    vec3 side = safeNormalize(cross(up, al), vec3(0, 0, -1));
    if (dot(side, vec3(0, 0, -1)) < 0) side = -side;
    vec3 am = al * std::cos(0.66f) + side * std::sin(0.66f);
    auto solve = [&](float gap, float thumbBack, FingerPose& fp) {
        float e = 0, v[4];
        e = std::max(e, solveThumb(sk, Side::Right, g.point - al * (r + gap + thumbBack) - up * 0.002f, v));
        for (int j = 0; j < 4; ++j) fp.v[Thumb][j] = v[j];
        e = std::max(e, solveLongFinger(sk, Side::Right, Index, g.point + al * (r + gap), v));
        for (int j = 0; j < 4; ++j) fp.v[Index][j] = v[j];
        e = std::max(e, solveLongFinger(sk, Side::Right, Middle, g.point + am * (r + gap) - up * 0.009f, v));
        for (int j = 0; j < 4; ++j) fp.v[Middle][j] = v[j];
        // Ring and little finger follow, tucked a little more each.
        // They stay clear of the neighbouring pieces (tips well above the pinch height).
        fp.v[Ring][0] = -0.04f;
        fp.v[Ring][1] = std::min(1.35f, fp.v[Middle][1] + 0.36f);
        fp.v[Ring][2] = std::min(1.65f, fp.v[Middle][2] + 0.42f);
        fp.v[Ring][3] = kDipCoupling * fp.v[Ring][2];
        fp.v[Pinky][0] = -0.10f;
        fp.v[Pinky][1] = std::min(1.45f, fp.v[Middle][1] + 0.55f);
        fp.v[Pinky][2] = std::min(1.70f, fp.v[Middle][2] + 0.50f);
        fp.v[Pinky][3] = kDipCoupling * fp.v[Pinky][2];
        return e;
    };
    if (closed) {
        g.pose = closed->pose;
        g.err = closed->err;
    } else {
        g.err = solve(0.0f, 0.0f, g.pose);
    }
    solve(0.013f * aperture, 0.006f * aperture, g.open);
    return g;
}
// Merges ring/pinky of 'rp' into 'base' (used to hold a captured piece while pinching).
inline FingerPose withRingPinky(FingerPose base, const FingerPose& rp) {
    for (int f = Ring; f <= Pinky; ++f)
        for (int j = 0; j < 4; ++j) base.v[f][j] = rp.v[f][j];
    return base;
}
// Hand-local pocket: where a captured piece sits between the curled ring/pinky and the palm.
inline vec3 pocketPoint(const Skeleton& sk, const FingerPose& fp) {
    vec3 a = phalanxMid(sk, Side::Right, fp, Ring, 1), b = phalanxMid(sk, Side::Right, fp, Pinky, 1);
    vec3 fingers = (a + b) * 0.5f;
    vec3 palm(kPalmHalf, fingers.y, fingers.z);
    return (fingers + palm) * 0.5f;
}
// Hand-local press point: between the index and middle pads.
inline vec3 pressPoint(const Skeleton& sk, const FingerPose& fp) {
    vec3 a = fingerTip(sk, Side::Right, fp, Index), b = fingerTip(sk, Side::Right, fp, Middle);
    return (a + b) * 0.5f;
}
inline vec3 mirrorX(vec3 v) { return vec3(-v.x, v.y, v.z); }
inline vec3 handPoint(Side s, vec3 rightHandLocal) { return s == Side::Right ? rightHandLocal : mirrorX(rightHandLocal); }

// Hand rotation (character space) from yaw (azimuth of the fingers' horizontal direction,
// 0 = forward, + = towards the character's left), pitch (fingers below the horizontal) and roll
// (+ = thumb side raised). Base orientation: palm down, fingers forward.
inline quat handBasis(Side s) {
    float ps = palmSign(s);
    vec3 X = vec3(0, -ps, 0);          // palm normal (ps,0,0) -> down
    vec3 Y = vec3(0, 0, -1);           // fingers (-Y) -> forward
    vec3 Z = cross(X, Y);
    return fromMat3(mat3(X, Y, Z));
}
inline quat handRot(Side s, float yaw, float pitch, float roll) {
    // roll about the forward axis; + raises the thumb side (thumb side = +X char for the right).
    float rs = s == Side::Right ? 1.0f : -1.0f;
    return qy(yaw) * qx(pitch) * qz(rs * roll) * handBasis(s);
}

// =============================================================================================
// 3. Trajectories
// =============================================================================================
template <class T>
struct Track {
    struct Key { float u; T v; };
    std::vector<Key> keys;
    void add(float u, const T& v) { keys.push_back({u, v}); }
};
// Track value at u: 'lerpFn' between the keys around u (minimum jerk), empty() without keys.
template <class T, class Empty, class Lerp>
inline T evalTrack(const Track<T>& tr, float u, Empty empty, Lerp lerpFn) {
    const auto& k = tr.keys;
    if (k.empty()) return empty();
    if (u <= k.front().u) return k.front().v;
    for (size_t i = 1; i < k.size(); ++i)
        if (u <= k[i].u) {
            float s = (u - k[i - 1].u) / std::max(1e-6f, k[i].u - k[i - 1].u);
            return lerpFn(k[i - 1].v, k[i].v, minJerk(s));
        }
    return k.back().v;
}
inline quat evalTrack(const Track<quat>& tr, float u) { return evalTrack(tr, u, [] { return quat(); }, qslerp); }
inline FingerPose evalTrack(const Track<FingerPose>& tr, float u) { return evalTrack(tr, u, poseRelaxed, fpLerp); }

// Pen frame (tip at the origin, +Y along the barrel to the back end) in the hand bone's frame.
struct PenPose {
    quat q;
    vec3 p{0, 0, 0};
};
inline PenPose penLerp(const PenPose& a, const PenPose& b, float t) { return {qslerp(a.q, b.q, t), lerp(a.p, b.p, t)}; }
inline PenPose evalTrack(const Track<PenPose>& tr, float u) { return evalTrack(tr, u, [] { return PenPose(); }, penLerp); }

struct HandSample {
    vec3 p{0, 0, 0}, v{0, 0, 0}, a{0, 0, 0};   // wrist (character space)
    quat q;                                    // hand bone rotation (character space)
    FingerPose f;
    float elbow = 0.0f;                        // elbow raised about the shoulder-wrist axis (rad)
    PenPose pen;                               // writing hand holding the pen: pen in the hand frame
    bool tipLock = false;                      // path following: the pen tip must be exactly at 'tip'
    vec3 tip{0, 0, 0};                         // (character space; the pen slides in the fingers by
                                               // the tiny amount the arm solve cannot reach)
    float lockW = 0.0f;                        // pen taken from / laid on the table: blend weight of
    PenPose lockC;                             // its exact table frame (character space) over 'pen'
    float pinW = 0.0f;                         // weight of the point lock: the hand point 'pinLocal'
    vec3 pinLocal{0, 0, 0};                    // stays where p/q put it even where the wrist clamps
};

struct Segment {
    float T = 0.1f;
    vec3 p0{0, 0, 0}, v0{0, 0, 0}, a0{0, 0, 0}, p1{0, 0, 0}, v1{0, 0, 0};
    float hs = 0, he = 1;          // horizontal (XZ) motion window, fractions of T
    float vs = 0, ve = 1;          // vertical motion window
    float arcH = 0, arcPeak = 0.5f;
    float oscAmp = 0, oscCycles = 0, os = 0, oe = 1;   // oscillation (handshake pumps, pointing jabs)
    vec3 oscAxis{0, 1, 0};                             // its direction (vertical unless set)
    Track<quat> rot;
    Track<FingerPose> fing;
    float swing = 0;               // dynamic tilt of the held piece (rad per m/s^2 of horizontal acc)
    vec3 pivot{0, 0, 0};           // hand-local pivot of that tilt (the pinch point)
    float elbow0 = 0, elbow1 = 0;  // elbow lift (over the pieces), min-jerk over the segment
    quat rotCorr;                  // mid-segment bend of the rotation (wrist comfort), bump-weighted
    float corrPeak = 0.5f;
    bool usePivot = false;         // rotate about rotPivot: that hand point follows the clean path
    vec3 rotPivot{0, 0, 0};
    Track<PenPose> pen;            // pen in the hand (regrips); empty = keep the default PenPose
    // Pen lock (see HandSample::lockW): weight lockFrom -> lockTo (min-jerk) over u in [lockU0, lockU1].
    PenPose lockC;
    float lockU0 = 0.0f, lockU1 = 1.0f, lockFrom = 0.0f, lockTo = 0.0f;
    // Point lock (see HandSample::pinW), same weighting.
    vec3 pinLocal{0, 0, 0};
    float pinU0 = 0.0f, pinU1 = 1.0f, pinFrom = 0.0f, pinTo = 0.0f;
    // Procedural segment (the writing hand following a pen path): when set, sample(t) is follow(t)
    // (t from the segment start) and every field above except T is ignored.
    std::function<HandSample(float t)> follow;

    // Extras on top of the quintic: the arc bump (vertical) and the oscillation along oscAxis (the
    // handshake pumps, the jabs of a pointing finger). Both are zero with zero slope and curvature
    // outside the segment, so symmetric differences work anywhere.
    float osc(float t) const {
        float u = t / T;
        if (oscAmp == 0.0f || u <= os || u >= oe) return 0.0f;
        float w = (u - os) / (oe - os);
        float env = std::sin(PI * w);
        env *= env;
        return oscAmp * env * std::sin(TAU * oscCycles * w);
    }
    float extraY(float t) const {
        float u = t / T, y = 0.0f;
        if (arcH != 0.0f) y += arcH * bump(u, arcPeak);
        if (oscAmp != 0.0f) y += osc(t) * oscAxis.y;
        return y;
    }
    bool oscFlat() const { return oscAmp != 0.0f && (oscAxis.x != 0.0f || oscAxis.z != 0.0f); }   // a horizontal part
    vec3 basePos(float t, vec3* vel = nullptr, vec3* acc = nullptr) const {
        vec3 p, v, a;
        auto axis = [&](int i, float ws, float we) {
            float t0 = ws * T, t1 = we * T;
            float tt = clamp(t - t0, 0.0f, t1 - t0);
            float P, V, A;
            Quintic1::eval(p0[i], v0[i], a0[i], p1[i], v1[i], 0.0f, t1 - t0, tt, P, V, A);
            if (t < t0) { P = p0[i]; V = 0; A = 0; }
            if (t > t1) { P = p1[i] + v1[i] * (t - t1); V = v1[i]; A = 0; }
            p[i] = P; v[i] = V; a[i] = A;
        };
        axis(0, hs, he);
        axis(2, hs, he);
        axis(1, vs, ve);
        p.y += extraY(t);
        if (oscFlat()) {
            float o = osc(t);
            p.x += o * oscAxis.x;
            p.z += o * oscAxis.z;
        }
        if (vel) *vel = v;
        if (acc) *acc = a;
        return p;
    }
    HandSample sample(float t) const {
        if (follow) return follow(clamp(t, 0.0f, T));
        HandSample s;
        float u = clamp(t / T, 0.0f, 1.0f);
        s.p = basePos(t, &s.v, &s.a);
        if (arcH != 0.0f || oscAmp != 0.0f) {
            const float h = 1.0f / 480.0f;
            float ya = extraY(t - h), y0 = extraY(t), yb = extraY(t + h);
            s.v.y += (yb - ya) / (2.0f * h);
            s.a.y += (yb - 2.0f * y0 + ya) / (h * h);
        }
        if (oscFlat()) {
            const float h = 1.0f / 480.0f;
            float oa = osc(t - h), o0 = osc(t), ob = osc(t + h);
            vec3 dir(oscAxis.x, 0.0f, oscAxis.z);
            s.v += dir * ((ob - oa) / (2.0f * h));
            s.a += dir * ((ob - 2.0f * o0 + oa) / (h * h));
        }
        s.q = evalTrack(rot, u);
        if (rotCorr.w < 0.99999f) {
            // Gone by the time the rotation reaches its end key (the grip, the tap...).
            float uEnd = rot.keys.empty() ? 1.0f : std::max(0.3f, rot.keys.back().u);
            s.q = normalize(qslerp(quat(), rotCorr, bump(u / uEnd, corrPeak)) * s.q);
        }
        s.f = evalTrack(fing, u);
        if (!pen.keys.empty()) s.pen = evalTrack(pen, u);
        if (lockFrom != 0.0f || lockTo != 0.0f) {
            float w = clamp((u - lockU0) / std::max(1e-4f, lockU1 - lockU0), 0.0f, 1.0f);
            s.lockW = lockFrom + (lockTo - lockFrom) * minJerk(w);
            s.lockC = lockC;
        }
        if (pinFrom != 0.0f || pinTo != 0.0f) {
            float w = clamp((u - pinU0) / std::max(1e-4f, pinU1 - pinU0), 0.0f, 1.0f);
            s.pinW = pinFrom + (pinTo - pinFrom) * minJerk(w);
            s.pinLocal = pinLocal;
        }
        s.elbow = elbow0 + (elbow1 - elbow0) * minJerk(u);
        if (usePivot && !rot.keys.empty()) {
            auto prog = [&](float ws, float we) { return minJerk(clamp((u - ws) / std::max(1e-4f, we - ws), 0.0f, 1.0f)); };
            vec3 oa = rotate(rot.keys.front().v, rotPivot), ob = rotate(rot.keys.back().v, rotPivot);
            float wh = prog(hs, he), wv = prog(vs, ve);
            vec3 off(oa.x + (ob.x - oa.x) * wh, oa.y + (ob.y - oa.y) * wv, oa.z + (ob.z - oa.z) * wh);
            s.p = s.p + off - rotate(s.q, rotPivot);
        }
        if (swing != 0.0f) {
            vec3 ah(s.a.x, 0, s.a.z);
            float mag = length(ah);
            if (mag > 1e-4f) {
                float ang = clamp(mag * swing, 0.0f, 0.16f) * std::sin(PI * u);
                quat tilt = axisAngle(normalize(cross(kY, ah)), -ang);  // top trails the acceleration
                vec3 piv = s.p + rotate(s.q, pivot);
                s.q = normalize(tilt * s.q);
                s.p = piv - rotate(s.q, pivot);
            }
        }
        return s;
    }
};

struct Motion {
    float start = 0.0f;
    std::vector<Segment> segs;
    float duration() const {
        float d = 0;
        for (auto& s : segs) d += s.T;
        return d;
    }
    HandSample sample(float tAbs) const {
        HandSample s;
        if (segs.empty()) return s;
        float t = tAbs - start;
        if (t <= 0.0f) return segs.front().sample(0.0f);
        for (size_t i = 0; i < segs.size(); ++i) {
            if (t <= segs[i].T || i + 1 == segs.size()) {
                if (t <= segs[i].T) return segs[i].sample(t);
                // After the end: hold, letting a residual velocity (clock tap) settle quickly.
                HandSample e = segs[i].sample(segs[i].T);
                float over = t - segs[i].T;
                const float tau = 0.022f;
                float k = std::exp(-over / tau);
                e.p = e.p + e.v * (tau * (1.0f - k));
                e.v = e.v * k;
                e.a = vec3(0);
                return e;
            }
            t -= segs[i].T;
        }
        return segs.back().sample(segs.back().T);
    }
};

// Timeline of a Trace (seconds): the approach onto the first waypoint, the pause there, one leg
// per pair of waypoints (horizontal length / TraceSpeed) with a pause on each inner waypoint, the
// settle on the last one. T > 0: fitted into that duration (see taskDuration).
struct TraceSchedule {
    float approach = 0.0f, dwell = 0.0f, corner = 0.0f, settle = 0.0f;
    std::vector<float> legs;
    float travel() const {   // first waypoint -> last one, pauses included
        float s = dwell;
        for (size_t i = 0; i < legs.size(); ++i) s += legs[i] + (i + 1 < legs.size() ? corner : 0.0f);
        return s;
    }
    float total() const { return approach + travel() + settle; }
};
inline TraceSchedule traceSchedule(const std::vector<vec3>& path, float T) {
    TraceSchedule s;
    s.approach = Timing::PointApproach;
    s.dwell = Timing::TraceDwell;
    s.corner = Timing::TraceCornerPause;
    s.settle = Timing::TraceSettle;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const vec3 d = path[i + 1] - path[i];
        s.legs.push_back(length(vec3(d.x, 0.0f, d.z)) / Timing::TraceSpeed);
    }
    if (T <= 0.0f) return s;
    const float natural = s.total();
    if (T >= natural) {
        s.settle += T - natural;   // a longer hold on the last waypoint
        return s;
    }
    // Shorter: everything but a short settle shrinks in proportion.
    const float settle = std::min(s.settle, 0.15f * T), k = (T - settle) / std::max(1e-6f, natural - s.settle);
    s.approach *= k;
    s.dwell *= k;
    s.corner *= k;
    for (float& l : s.legs) l *= k;
    s.settle = T - s.approach - s.travel();
    return s;
}

// =============================================================================================
// 4. Body solver
// =============================================================================================
struct SpineParams {
    float flex = 0, twist = 0, side = 0;   // radians: forward flexion, twist to the left, bend to the right
};
// Soft strain penalty of an arm solve near the wrist and forearm limits (wrist flexion and
// deviation, forearm pronation; radians).
inline float softWristStrain(float flex, float dev, float pron) {
    return std::max(0.0f, std::fabs(flex) - 1.10f) + std::max(0.0f, dev - 0.35f) + std::max(0.0f, -dev - 0.55f) +
           std::max(0.0f, std::fabs(pron) - 1.60f);
}

}  // namespace detail
using namespace detail;

// ---------------------------------------------------------------------------------------------
struct Animator::Impl {
    const Skeleton* sk = nullptr;
    vec3 pelvisWorld{0, 0, 0};
    float facing = 1.0f;
    quat rootQ;
    mat4 root, invRoot;
    float time = 0.0f;
    double timeD = 0.0;   // the clock summed in double: 'time' neither drifts nor stalls in a long session
    Rng rng;
    float seed = 0.0f;

    // Arm dimensions (from the skeleton).
    float L1 = 0.3f, L2 = 0.265f;

    // ---- hands
    struct Hand {
        Side side = Side::Right;
        Motion motion;
        HandSample rest;           // resting target
        int heldId = -1;
        mat4 heldAttach;           // piece relative to the hand bone (world)
        int capId = -1;
        mat4 capAttach;
        quat gripQ;                // hand rotation (char) at grip time
        quat carryQ;               // hand rotation while carrying / placing (gripQ turned about the vertical)
        vec3 gripPos{0, 0, 0};     // character-space pinch point of the last Reach
        quat capQ;                 // hand rotation (char) when the captured piece was taken
        float liftH = layout::PIECE_LIFT_HEIGHT;
        PinchGeo pinch;
        vec3 pocket{0, 0, 0};
        float gripBelow = 0.0f;    // held piece: base below the pinch point (m)
        int releasedId = -1;       // piece let go at the end of the previous task (the hand starts on it)
        vec3 restContact{0, 0, 0}; // requested resting spot (character space); 'rest' may shift away from pieces
        bool chinFollow = false;   // idle chin pose: follows the head
        HandSample chinPlanned;
        // Playing hand: the point lock's shift at the last frame (character space), and that shift
        // fading out from pinCarryStart after a task let the lock go (no jump at the boundary).
        vec3 pinApplied{0, 0, 0};
        vec3 pinCarry{0, 0, 0};
        float pinCarryStart = 0.0f;
    } hands[2];                    // [0] = left, [1] = right
    Hand& right() { return hands[1]; }
    Hand& left() { return hands[0]; }

    // ---- tasks
    std::deque<Task> queue;
    bool running = false;
    Task cur;
    float curStart = 0, curT = 0;
    struct TimedEvent {
        float t;
        EventType type;
        int action;
        bool done;
        bool hasPos = false;       // the event's own position (solver world) instead of the task's
        vec3 pos{0, 0, 0};
    };
    std::vector<TimedEvent> curEvents;
    enum Action { ActNone, ActGripPrimary, ActReleasePrimary, ActGripCaptured, ActReleaseCaptured, ActPutPen };
    mat4 shakePutFrame;                    // ActPutPen: where the handshake lays the pen (solver world)
    vec3 curTargetWorld{0, 0, 0};
    bool prevWasClock = false;             // the previous task ended with the clock tap
    TaskType prevType = TaskType::Wait;

    // ---- coach gestures (animator_gesture.cpp)
    float curArrive = 0.0f;                // gesture arrived (from curStart): endHold never cuts before
    bool curLook = true;                   // the running task's target draws the eyes (see gazeHold)
    // Running Trace: the tip path (character space) from traceStart, for the eyes to pursue it.
    struct TracePlan;
    std::shared_ptr<TracePlan> trace;
    float traceStart = 0.0f;
    // Speech and head gestures (updateGaze).
    float speechLevel = 0.0f, speechEnv = 0.0f, speechFast = 0.0f, speechSlow = 0.0f, speechStress = 0.0f;
    float nodT = -1.0f, nodDur = 0.45f, nodAmp = 0.0f;
    float shakeHT = -1.0f, shakeHDur = 0.6f, shakeHAmp = 0.0f;
    float gestYaw = 0.0f, gestPitch = 0.0f;   // nod / shake offsets added after the head spring
    bool isGesture(TaskType ty) const { return ty == TaskType::Point || ty == TaskType::Trace || ty == TaskType::Gesture; }
    // Pointing: hand pose with the index tip at 'tip' (aimed at 'aim' for a Point).
    struct PointChoice {
        quat q;
        vec3 wrist{0, 0, 0}, tip{0, 0, 0};
        float cost = 1e9f, strain = 0.0f, flex = 0.0f, clear = 0.0f;
    };
    PointChoice choosePoint(vec3 aim, float topAim, float hover, const vec3* fixedTip, const quat* prevQ, const FingerPose& fp);
    // How deep the hand (wrist at w, rotation q, fingers f) dips into the space 'margin' above the
    // standing pieces (m, the worst point; >= 0: planGesture's orientation searches rely on it);
    // ignoreId is not an obstacle.
    float handDepth(vec3 w, quat q, const FingerPose& f, float margin, int ignoreId) const;
    bool gestureBlocked(const HandSample& from, float T, Motion& mo);
    Segment pointApproach(const HandSample& from, float Ta, vec3 w, quat q, const FingerPose& fp, vec3 tipL) const;
    void planPoint(const Task& t, float start, float T, const HandSample& from, Motion& mo);
    void planTrace(const Task& t, float start, float T, const HandSample& from, Motion& mo);
    void planGesture(const Task& t, float start, float T, const HandSample& from, Motion& mo);
    void finishTracePlan(Hand& h);    // after liftForearm: the elbow along the path
    void updateSpeech(float dt, float& hy, float& hp);

    // ---- handshake (clasp point, character space) for gaze
    Animator* partner = nullptr;
    float shakeStart = -100.0f;
    float shakeScale = 1.0f;   // its duration / Timing::Handshake (every phase scales with it)

    // ---- gaze / head
    vec3 gazeTarget{0, layout::BOARD_TOP_Y, 0};
    float gazeWeightTarget = 0.0f, gazeWeight = 0.0f;
    bool headOverride = false;
    float ovYaw = 0, ovPitch = 0;
    float headYaw = 0, headPitch = 0, headYawV = 0, headPitchV = 0;   // char-space angles + vel
    vec3 fixFrom{0, 0, 1}, fixTo{0, 0, 1};   // eye fixation points (world)
    float sacT = 1.0f, sacDur = 0.05f;
    float microTimer = 0.8f;
    vec3 microOffset{0, 0, 0};
    float blinkTimer = 2.5f, blinkPhase = -1.0f;
    float eyeYaw = 0, eyePitch = 0;          // for the lids

    // ---- idle / thinking
    bool thinking = false;
    int thinkPose = 0;                        // 0 rest, 1 left chin, 2 right chin
    float thinkTimer = 3.0f;
    bool rightIdle = true;                    // right hand free (no piece, at rest or idling)
    int leftChin = 0, rightChin = 0;          // idle motion currently driven towards the chin
    float taskGaze = 0.0f;                    // 0..1: gaze follows the running task's target
    float thinkLean = 0, thinkLeanTarget = 0;
    float lean = 0, leanTarget = 0;           // setLean (0..1), smoothed in updateIdle

    bool debugLog = std::getenv("SCACELITH_ANIM_DEBUG") != nullptr;

    // ---- solve outputs
    mat4 G[BoneCount];        // character-space globals
    float wristClamp = 0;     // diagnostics
    float reachShort = 0, pronClamp = 0, lastFlex = 0, lastDev = 0, lastPron = 0;

    // ------------------------------------------------------------------------------------------
    vec3 toChar(vec3 w) const { return transformPoint(invRoot, w); }
    vec3 toWorld(vec3 c) const { return transformPoint(root, c); }
    quat qToChar(quat w) const { return normalize(conjugate(rootQ) * w); }

    mat4 localMat(const Pose& p, int b) const { return toMat4(p.local[b], sk->restOffset[b]); }
    void fkChain(const Pose& p, int from, int to) {  // bones [from, to] in index order
        for (int b = from; b <= to; ++b) {
            int par = sk->parent[b];
            G[b] = par < 0 ? localMat(p, b) : G[par] * localMat(p, b);
        }
    }
    void fkAll(const Pose& p) { fkChain(p, 0, BoneCount - 1); }

    // Local rotations of the pelvis, the spine and the legs for spine params s.
    void applySpine(Pose& p, const SpineParams& s) const {
        float fp = s.flex * 0.24f, f1 = s.flex * 0.42f, f2 = s.flex * 0.34f;
        p.local[Pelvis] = qx(fp);
        p.local[Spine1] = qy(s.twist * 0.40f) * qx(f1) * qz(s.side * 0.5f);
        p.local[Spine2] = qy(s.twist * 0.60f) * qx(f2) * qz(s.side * 0.5f);
        // Thighs keep their orientation (the pelvis rocks on the seat).
        quat legs = qx(-fp);
        p.local[ThighL] = legs * qy(0.035f) * qz(0.02f);
        p.local[ThighR] = legs * qy(-0.05f) * qz(-0.02f);
        p.local[ShinL] = qx(0.05f);
        p.local[ShinR] = qx(-0.03f);
        p.local[FootL] = qx(-0.05f);
        p.local[FootR] = qx(0.03f);
    }
    quat clavicleFor(Side s, vec3 wrist, const mat4& spine2) const {
        Bone clav = armBone(s, ClavicleL), upper = armBone(s, UpperArmL);
        mat4 c = spine2 * toMat4(quat(), sk->restOffset[clav]);
        vec3 S0 = transformPoint(c, sk->restOffset[upper]);
        vec3 d = wrist - S0;
        float dist = length(d);
        vec3 dl = rotate(conjugate(rotOf(spine2)), d / std::max(dist, 1e-4f));  // chest frame
        float reach = dist / (L1 + L2);
        float sx = sideX(s);
        float crossAmt = clamp(-dl.x * sx, 0.0f, 1.0f);            // towards the other side
        float fwd = std::max(0.0f, dl.z);
        float elev = clamp(0.10f * std::max(0.0f, dl.y + 0.35f) + 0.35f * std::max(0.0f, reach - 0.78f) + 0.10f * crossAmt, 0.0f, 0.30f);
        float prot = clamp(0.55f * std::max(0.0f, reach - 0.62f) * (0.4f + fwd) + 0.20f * crossAmt, 0.0f, 0.34f);
        return qy(prot * -sx) * qz(elev * sx);
    }
    // Shoulder of 'side' for given spine params and clavicle rotation (character space).
    vec3 shoulderFor(Pose& p, Side s, const SpineParams& sp, vec3 wrist) {
        applySpine(p, sp);
        mat4 pel = localMat(p, Pelvis);
        mat4 s1 = pel * localMat(p, Spine1);
        mat4 s2 = s1 * localMat(p, Spine2);
        Bone clav = armBone(s, ClavicleL), upper = armBone(s, UpperArmL);
        mat4 c = s2 * toMat4(clavicleFor(s, wrist, s2), sk->restOffset[clav]);
        return transformPoint(c, sk->restOffset[upper]);
    }

    // Torso lean/twist so the right wrist target stays comfortably within reach.
    SpineParams solveSpine(Pose& p, vec3 wristR, float extraFlex, float idleFlex, float idleTwist, float idleSide) {
        SpineParams sp;
        Side s = Side::Right;
        float reach = L1 + L2;
        // Reference shoulder (upright) to measure the target azimuth.
        SpineParams up0;
        vec3 S0 = shoulderFor(p, s, up0, wristR);
        vec3 d0 = wristR - S0;
        float az = std::atan2(d0.x, std::max(0.05f, d0.z));          // + = towards the left
        float horiz = length(vec3(d0.x, 0, d0.z));
        float twistW = smoothstep(0.18f, 0.45f, horiz);
        sp.twist = clamp(0.42f * az * twistW, -0.30f, 0.42f) + idleTwist;
        // Lateral reaches bend the spine slightly towards the target side.
        sp.side = clamp(-0.10f * az * twistW, -0.06f, 0.06f) + idleSide;
        float D0 = length(d0);
        sp.flex = clamp((D0 - 0.42f) * 0.45f, 0.0f, 0.10f) + extraFlex + idleFlex;
        float comfy = 0.80f * reach, hard = 0.955f * reach;
        auto Dof = [&](float flex) {
            SpineParams t = sp;
            t.flex = flex;
            return length(wristR - shoulderFor(p, s, t, wristR));
        };
        float D = Dof(sp.flex);
        float Dt = softLimit(D, comfy, hard);
        if (D > Dt + 1e-4f) {
            float f = sp.flex;
            for (int it = 0; it < 6; ++it) {
                float Df = Dof(f), Dg = Dof(f + 0.01f);
                float der = (Dg - Df) / 0.01f;
                if (der > -1e-3f) break;
                f = clamp(f - (Df - Dt) / der, 0.0f, 0.80f);
                if (std::fabs(Df - Dt) < 2e-4f) break;
            }
            sp.flex = f;
        }
        return sp;
    }

    // Analytic two-bone IK. G must hold valid globals for Spine2 (and parents).
    void solveArm(Pose& p, Side s, vec3 W, quat R, float elbowLift = 0.0f) {
        Bone clav = armBone(s, ClavicleL), upper = armBone(s, UpperArmL), fore = armBone(s, ForeArmL), hand = armBone(s, HandL);
        p.local[clav] = clavicleFor(s, W, G[Spine2]);
        G[clav] = G[Spine2] * localMat(p, clav);
        vec3 S = transformPoint(G[clav], sk->restOffset[upper]);
        float sx = sideX(s), ps = palmSign(s);
        vec3 d = W - S;
        float D = length(d);
        vec3 u = safeNormalize(d, vec3(0, -1, 0));
        float Dc = clamp(D, std::fabs(L1 - L2) + 1e-3f, (L1 + L2) * 0.9995f);
        if (s == diagSide) reachShort = std::max(reachShort, D - Dc);
        float cosA = clamp((L1 * L1 + Dc * Dc - L2 * L2) / (2.0f * L1 * Dc), -1.0f, 1.0f);
        float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
        // Elbow pole: down and out, a little back; further out for cross-body reaches and for a
        // pronated (palm-down) hand.
        vec3 Sl = rotate(conjugate(rotOf(G[Spine2])), d);            // chest frame
        float crossAmt = clamp(-Sl.x * sx / 0.35f, 0.0f, 1.0f);
        float palmDown = std::max(0.0f, -rotate(R, vec3(ps, 0, 0)).y);
        float high = clamp((W.y - S.y + 0.15f) / 0.25f, 0.0f, 1.0f);
        vec3 pole = vec3(0, -1.0f + 0.35f * crossAmt, 0) + vec3(sx, 0, 0) * (0.30f + 0.25f * palmDown + 0.55f * crossAmt - 0.15f * high) +
                    vec3(0, 0, -0.25f + 0.10f * high);
        vec3 pp = perp(normalize(pole), u);
        if (length(pp) < 1e-3f) pp = perp(vec3(0, -1, 0), u);
        if (length(pp) < 1e-3f) pp = perp(vec3(sx, 0, 0), u);
        pp = normalize(pp);
        if (elbowLift > 1e-4f) {
            // Swivel the elbow up and out about the shoulder-wrist axis (reaching over pieces).
            vec3 tg = cross(u, pp);
            if (dot(tg, vec3(sx * 0.3f, 1.0f, 0.0f)) < 0.0f) tg = -tg;
            pp = normalize(pp * std::cos(elbowLift) + tg * std::sin(elbowLift));
        }
        vec3 E = S + u * (L1 * cosA) + pp * (L1 * sinA);
        vec3 Wr = S + u * Dc;
        vec3 du = normalize(E - S), df = normalize(Wr - E);
        vec3 Yu = -du;
        vec3 Zu = safeNormalize(perp(-pp, du), safeNormalize(perp(df, du), vec3(0, 0, 1)));
        vec3 Xu = cross(Yu, Zu);
        quat gClav = rotOf(G[clav]);
        quat gUpper = fromMat3(mat3(Xu, Yu, Zu));
        p.local[upper] = normalize(conjugate(gClav) * gUpper);
        G[upper] = G[clav] * localMat(p, upper);
        vec3 Yf = -df, Xf = Xu, Zf = cross(Xf, Yf);
        quat gF0 = fromMat3(mat3(Xf, Yf, Zf));
        quat rel = normalize(conjugate(gF0) * R);
        if (rel.w < 0) rel = quat(-rel.x, -rel.y, -rel.z, -rel.w);
        float tw = 2.0f * std::atan2(rel.y, rel.w);
        tw = wrapPi(tw);
        float pron = clamp(tw * ps, -1.75f, 1.95f);
        if (s == diagSide) pronClamp = std::max(pronClamp, std::fabs(pron - tw * ps));
        tw = pron * ps;
        quat gF = normalize(gF0 * qy(tw));
        p.local[fore] = normalize(conjugate(gUpper) * gF);
        G[fore] = G[upper] * localMat(p, fore);
        quat hl = normalize(conjugate(gF) * R);
        // Wrist limits (flexion towards the palm / extension, radial / ulnar deviation).
        vec3 fd = rotate(hl, vec3(0, -1, 0));
        float flex = std::atan2(fd.x * ps, -fd.y), dev = std::atan2(fd.z, -fd.y);
        float flexC = clamp(flex, -1.30f, 1.40f), devC = clamp(dev, -0.75f, 0.50f);
        if (s == diagSide) { lastFlex = flex; lastDev = dev; lastPron = pron; }
        if (flexC != flex || devC != dev) {
            vec3 nd = normalize(vec3(std::tan(flexC) * ps, -1.0f, std::tan(devC)));
            if (-fd.y < 0.05f) nd = normalize(vec3(fd.x, std::max(-fd.y, 0.2f) * -1.0f, fd.z));
            hl = normalize(fromTo(fd, nd) * hl);
            if (s == diagSide) wristClamp = std::max(wristClamp, std::fabs(flex - flexC) + std::fabs(dev - devC));
        }
        p.local[hand] = hl;
        G[hand] = G[fore] * localMat(p, hand);
    }

    // ------------------------------------------------------------------------------------------
    // Resting targets (character space)
    HandSample restSample(Side s, vec3 contactChar) const {
        HandSample h;
        float yawIn = s == Side::Right ? 0.34f : -0.30f;    // fingers point a little inwards
        h.q = handRot(s, yawIn, 0.13f, s == Side::Right ? 0.10f : 0.08f);
        h.f = fpHumanize(poseTableRest(), s == Side::Right ? 0.3f : 0.7f, 0.05f);
        // Lowest fingertip pad touches the table; palm centre above the contact point.
        vec3 pc = rotate(h.q, handPoint(s, palmCenter(Side::Right)));
        float lowest = 1e9f;
        for (int f = 1; f < 5; ++f) {
            vec3 tip = rotate(h.q, handPoint(s, fingerTip(*sk, Side::Right, h.f, f)));
            lowest = std::min(lowest, tip.y - kPadRadius);
        }
        vec3 thumb = rotate(h.q, handPoint(s, fingerTip(*sk, Side::Right, h.f, Thumb)));
        lowest = std::min(lowest, thumb.y - kPadRadius);
        h.p = vec3(contactChar.x - pc.x, contactChar.y - lowest + 0.0005f, contactChar.z - pc.z);
        // Resting on the table beside the board: keep every fingertip clear of the board frame.
        const vec3 bc = toChar(vec3(0, layout::BOARD_TOP_Y, 0));
        const float hb = layout::BOARD_SIZE * 0.5f, margin = 0.010f + kPadRadius;
        const float sx = s == Side::Right ? -1.0f : 1.0f;   // outwards (character space)
        float push = 0.0f;
        for (int f = 0; f < 5; ++f) {
            vec3 tip = h.p + rotate(h.q, handPoint(s, fingerTip(*sk, Side::Right, h.f, f)));
            if (std::fabs(tip.z - bc.z) > hb + margin) continue;
            float out = (tip.x - bc.x) * sx;                   // distance outwards from the board centre
            push = std::max(push, hb + margin - out);
        }
        h.p.x += sx * push;
        return h;
    }

    // Resting hand clear of the pieces standing on the table (captured pieces, spare pieces):
    // palm, knuckles, fingertips and wrist keep 12 mm from them.
    bool restClear(Side s, const HandSample& r) const {
        auto hit = [&](vec3 lp, float rad) {
            vec3 pw = toWorld(r.p + rotate(r.q, handPoint(s, lp)));
            return topNear(pw, rad + 0.012f, -1) > layout::BOARD_TOP_Y + 1e-3f;
        };
        if (hit(vec3(0), 0.022f) || hit(palmCenter(Side::Right), 0.028f)) return false;
        for (int f = 0; f < 5; ++f) {
            mat4 fr[3];
            fingerFrames(*sk, Side::Right, r.f, f, fr);
            for (int j = 0; j < 3; ++j)
                if (hit(fr[j].translation(), 0.010f) || hit(phalanxMidFrom(*sk, Side::Right, fr, f, j), 0.009f)) return false;
            if (hit(fingerTipFrom(*sk, Side::Right, fr, f), 0.008f)) return false;
        }
        return true;
    }
    // The requested resting spot, or the nearest one further back / further out that is clear.
    HandSample safeRest(Side s, vec3 contact) const {
        HandSample r0 = restSample(s, contact);
        if (!knowsAnyPiece() || restClear(s, r0)) return r0;
        const float out = s == Side::Right ? -1.0f : 1.0f;
        // The palm stays on the table (its near edge, character space).
        const float minZ = toChar(vec3(0, layout::TABLE_TOP_Y, facing * layout::TABLE_DEPTH * 0.5f)).z + 0.045f;
        HandSample best = r0;
        float bestCost = 1e9f;
        for (int b = 0; b <= 6; ++b)
            for (int o = 0; o <= 5; ++o) {
                float back = 0.02f * float(b), side = 0.02f * float(o), cost = back + 1.4f * side;
                if ((b == 0 && o == 0) || cost >= bestCost) continue;
                vec3 c = contact + vec3(out * side, 0, -back);
                if (c.z < minZ) continue;
                HandSample r = restSample(s, c);
                if (!restClear(s, r)) continue;
                best = r;
                bestCost = cost;
            }
        if (debugLog && bestCost < 1e9f) LOGI("anim: %s hand rests %.0f mm away from the pieces on the table", s == Side::Right ? "right" : "left", bestCost * 1000.0f);
        return best;
    }
    void validateRests(const Task* t);

    // ------------------------------------------------------------------------------------------
    // Planning helpers
    vec3 shoulderRest(Side s) const {
        vec3 p = sk->restOffset[Spine1] + sk->restOffset[Spine2] + sk->restOffset[armBone(s, ClavicleL)] + sk->restOffset[armBone(s, UpperArmL)];
        return p;
    }
    vec3 gripInfo(int id) const {
        vec3 gi(0.05f, 0.62f, 0.008f);
        if (id >= 0 && owner && owner->pieceGripInfo) gi = owner->pieceGripInfo(id);
        if (gi.y > gi.x) gi.y *= gi.x;          // fraction -> meters
        gi.z = std::max(gi.z, 0.003f);
        return gi;
    }
    mat4 pieceWorld(int id) const {
        if (id >= 0 && owner && owner->pieceTransform) return mm(owner->pieceTransform(id));
        return mat4();
    }
    // Pieces this animator put down during the current update() call: the game only learns
    // about them from the events after update() returns, so its obstacle callbacks cannot know
    // them yet when the next task is planned in the same call.
    struct Fresh {
        int id;
        vec3 base;       // world
        float top, radius;
    };
    std::vector<Fresh> fresh;
    // Pieces this animator left standing on the table (captured pieces, promoted pawns): kept
    // until it picks them up again, so the hands keep clear of them even without the callbacks.
    // With the callbacks they are the game's to report (from the next update() on), and it may
    // move them itself (a board set back from the game, a lesson reset): not looked at then.
    std::vector<Fresh> tableLeft;
    // The piece the current task is about to set down off the board (resting-hand checks only).
    std::vector<Fresh> pending;
    // Without the obstacle callbacks: the pieces standing on the board and the table, read from
    // pieceTransform when a task starts (ids 0, 1, 2... until 16 ids in a row do not exist).
    std::vector<Fresh> scene;
    bool sceneKnown = false;
    bool restsDirty = true;   // resting spots not checked against the pieces yet
    static constexpr int kMaxPieceIds = 128;
    Fresh knownPiece(int id, vec3 baseW) const {
        vec3 gi = gripInfo(id);
        // Base radius estimated from the grip radius and the height (Staunton proportions).
        float r = clamp(std::max(1.9f * gi.z, 0.2f * gi.x + 0.004f), 0.014f, 0.021f);
        return {id, baseW, baseW.y + gi.x, r};
    }
    void forgetTable(int id) {
        for (size_t i = 0; i < tableLeft.size(); ++i)
            if (tableLeft[i].id == id) {
                tableLeft.erase(tableLeft.begin() + long(i));
                break;
            }
    }
    void noteReleased(int id, const mat4& xf) {
        Fresh f = knownPiece(id, xf.translation());
        fresh.push_back(f);
        forgetTable(id);
        if (f.base.y < layout::BOARD_TOP_Y - 0.01f) tableLeft.push_back(f);
    }
    float freshTop(vec3 fromW, vec3 toW, float radius, int ignoreId) const {
        float top = -1e9f;
        vec3 d(toW.x - fromW.x, 0, toW.z - fromW.z);
        float len2 = std::max(1e-8f, length2(d));
        const bool gameSees = owner && (owner->obstacleTopNear || owner->pathObstacleTop);
        for (const std::vector<Fresh>* list : {&fresh, &tableLeft, &pending, &scene})
            for (const Fresh& f : *list) {
                if (f.id == ignoreId || isHeld(f.id) || (gameSees && list == &tableLeft)) continue;
                float s = clamp(dot(vec3(f.base.x - fromW.x, 0, f.base.z - fromW.z), d) / len2, 0.0f, 1.0f);
                vec3 c = fromW + d * s;
                if (length(vec3(f.base.x - c.x, 0, f.base.z - c.z)) < f.radius + radius) top = std::max(top, f.top);
            }
        return top;
    }
    bool isHeld(int id) const { return id >= 0 && (id == hands[1].heldId || id == hands[1].capId); }
    void snapshotPieces() {
        scene.clear();
        sceneKnown = false;
        if (!owner || !owner->pieceTransform || owner->obstacleTopNear || owner->pathObstacleTop) return;
        for (int id = 0, misses = 0; id < kMaxPieceIds && misses < 16; ++id) {
            vec3 b = mw(owner->pieceTransform(id).translation());
            if (!(b.y > layout::TABLE_TOP_Y - 0.10f)) {   // no such piece (identity), or out of the game
                ++misses;
                continue;
            }
            misses = 0;
            sceneKnown = true;
            if (b.y > layout::BOARD_TOP_Y + 0.01f) continue;    // in the air
            scene.push_back(knownPiece(id, b));
        }
    }
    float obstacleTop(vec3 fromW, vec3 toW) const {
        if (owner && owner->pathObstacleTop) return std::max(owner->pathObstacleTop(mw(fromW), mw(toW)), freshTop(fromW, toW, 0.028f, -1));
        if (sceneKnown) return std::max(layout::BOARD_TOP_Y, freshTop(fromW, toW, 0.028f, -1));
        // No knowledge of the pieces: assume a king anywhere on the board (plus a margin).
        const float hb = layout::BOARD_SIZE * 0.5f + 0.03f;
        const float known = freshTop(fromW, toW, 0.028f, -1);
        for (int i = 0; i <= 16; ++i) {
            vec3 p = fromW + (toW - fromW) * (float(i) / 16.0f);
            if (std::fabs(p.x) < hb && std::fabs(p.z) < hb) return std::max(known, layout::BOARD_TOP_Y + layout::PIECE_HEIGHT[6]);
        }
        return std::max(known, layout::TABLE_TOP_Y);
    }
    // Highest obstacle (character Y) under the wrist and the fingertips moving between two hand
    // poses of hand s (the right one by default; the finger geometry is the right hand's, mirrored by
    // handPoint).
    float pathTop(const HandSample& a, vec3 pb, quat qb, const FingerPose& fb, Side s = Side::Right) const {
        vec3 ta = a.p + rotate(a.q, handPoint(s, fingerTip(*sk, Side::Right, a.f, Middle)));
        vec3 tb = pb + rotate(qb, handPoint(s, fingerTip(*sk, Side::Right, fb, Middle)));
        float top = std::max(obstacleTop(toWorld(a.p), toWorld(pb)), obstacleTop(toWorld(ta), toWorld(tb)));
        return top - pelvisWorld.y;
    }
    // Raises a segment's arc so the hand passes over the pieces on its way (for moves that end
    // on the table: the horizontal part finishes first, then the hand settles down).
    void clearPath(Segment& sg, const HandSample& from, const FingerPose& fm, float tableC, Side s = Side::Right) const {
        float top = pathTop(from, sg.p1, sg.rot.keys.back().v, fm, s);
        if (top <= tableC + 0.005f) return;
        float below = handBelow(qslerp(from.q, sg.rot.keys.back().v, 0.35f), fm, s);
        sg.arcH = std::max(sg.arcH, arcFor(from.p, sg.p1, top + 0.015f, below));
        sg.he = std::min(sg.he, 0.80f);
        sg.arcPeak = 0.42f;
        // Starting from the table among pieces: up first, then across.
        if (from.p.y - tableC < 0.12f && length(from.v) < 0.05f) sg.hs = std::max(sg.hs, 0.10f);
    }
    // How far the lowest fingertip pad hangs below the wrist (hand s, the right one by default;
    // rotation q).
    float handBelow(quat q, const FingerPose& f, Side s = Side::Right) const {
        float below = 0.0f;
        for (int i = 0; i < 5; ++i) below = std::max(below, -rotate(q, handPoint(s, fingerTip(*sk, Side::Right, f, i))).y + kPadRadius);
        return below;
    }
    // The obstacle queries know where the pieces stand (the game's callbacks, or the pieces read when
    // the task started) / know at least one piece (also one this animator sets down off the board).
    bool knowsPieces() const { return (owner && (owner->obstacleTopNear || owner->pathObstacleTop)) || sceneKnown; }
    bool knowsAnyPiece() const { return knowsPieces() || !tableLeft.empty() || !pending.empty(); }
    // Highest piece top (world Y) whose base comes within 'radius' of pW (world), ignoring piece
    // ignoreId when the game supports it.
    float topNear(vec3 pW, float radius, int ignoreId) const {
        float fr = freshTop(pW, pW, radius, ignoreId);
        if (owner && owner->obstacleTopNear) return std::max(owner->obstacleTopNear(mw(pW), radius, ignoreId), fr);
        if (owner && owner->pathObstacleTop) return std::max(owner->pathObstacleTop(mw(pW), mw(pW)), fr);
        return std::max(layout::BOARD_TOP_Y, fr);
    }
    // Natural azimuth of the fingers for a pinch at g: turned inwards, the thumb near-left of the piece.
    float pinchYawFor(vec3 g) const {
        vec3 d = g - shoulderRest(Side::Right);
        return std::atan2(d.x, std::max(0.08f, d.z)) * 0.80f + 0.22f;
    }
    // Far squares: flatter hand; near squares: fingers more vertical.
    float pinchPitchFor(vec3 g) const {
        vec3 d = g - shoulderRest(Side::Right);
        return lerp(0.80f, 0.52f, smoothstep(0.35f, 0.80f, length(vec3(d.x, 0, d.z))));
    }
    quat pinchRotFor(vec3 g) const { return handRot(Side::Right, pinchYawFor(g), pinchPitchFor(g), -0.10f); }

    // How deep the hand (pinch pose and its open variant, hand rotation q, pinch point on the
    // character-space point g) dips into the space of the neighbouring pieces (m, summed).
    float neighbourDepth(vec3 g, quat q, const PinchGeo& pg, int pieceId, float pieceR) const {
        float sum = 0.0f;
        const bool canIgnore = (owner && owner->obstacleTopNear) || sceneKnown;
        auto test = [&](vec3 lp, float rad) {
            vec3 pc = g + rotate(q, lp - pg.point);
            vec3 pw = toWorld(pc);
            if (!canIgnore && length(vec3(pc.x - g.x, 0, pc.z - g.z)) < pieceR * 2.0f + 0.03f) return;
            // Above their foot, Staunton pieces are much slimmer than the base (about 60%).
            float q = pw.y - layout::BOARD_TOP_Y > 0.012f ? std::max(0.001f, rad - 0.006f) : rad;
            float top = topNear(pw, q, pieceId);
            if (top <= layout::BOARD_TOP_Y + 1e-3f) return;
            sum += std::max(0.0f, top + 0.003f - (pw.y - rad));
        };
        for (const FingerPose* fp : {&pg.pose, &pg.open}) {
            for (int f = 0; f < 5; ++f) {
                mat4 fr[3];
                fingerFrames(*sk, Side::Right, *fp, f, fr);
                for (int j = 0; j < 3; ++j) {
                    Bone b = fingerBone(Side::Right, f, j);
                    vec3 d = boneDir(*sk, fingerBone(Side::Right, f, 2));
                    test(fr[j].translation(), j == 0 ? 0.011f : 0.009f);                       // joint
                    test(transformPoint(fr[j], d * (sk->boneLength[b] * 0.5f)), 0.009f);      // phalanx middle
                }
                test(fingerTipFrom(*sk, Side::Right, fr, f), 0.008f);
            }
        }
        return sum;
    }
    // How hard it is for the right arm to put the hand bone at wristC with rotation q (rad): joint
    // limit clamps, plus a soft penalty near the wrist and forearm limits.
    // 'achieved' (optional): the hand rotation the arm really gets to (joint limits).
    // 'flexOut' (optional): the torso flexion the reach takes.
    float armStrain(vec3 wristC, quat q, quat* achieved = nullptr, float* flexOut = nullptr) {
        Pose tmp;
        reachShort = wristClamp = pronClamp = 0;
        SpineParams sp = solveSpine(tmp, wristC, 0.0f, 0.0f, 0.0f, 0.0f);
        if (flexOut) *flexOut = sp.flex;
        applySpine(tmp, sp);
        fkChain(tmp, Pelvis, Spine2);
        solveArm(tmp, Side::Right, wristC, q);
        if (achieved) *achieved = rotOf(G[HandR]);
        float soft = softWristStrain(lastFlex, lastDev, lastPron);
        return wristClamp + pronClamp + reachShort * 10.0f + 0.5f * soft;
    }
    // Grip orientation for piece 'pieceId' pinched at g: the natural one, turned and pitched
    // steeper when the fingers or knuckles would touch the neighbours (here and where the
    // piece will be set down, 'dst' when known).
    quat chooseGrip(vec3 g, float r, int pieceId, const vec3* dst, PinchGeo& out) {
        const float yaw0 = pinchYawFor(g), pitch0 = pinchPitchFor(g);
        quat best = handRot(Side::Right, yaw0, pitch0, -0.10f);
        out = pinchFor(*sk, best, r);
        if (!knowsPieces()) return best;
        // Cost: neighbours touched (checked with the hand where the arm really puts it: a clamped
        // wrist turns the fingers elsewhere), arm strain here and at the destination, and how far
        // the orientation is from the natural one.
        const float dstYaw = dst ? wrapPi(pinchYawFor(*dst) - pinchYawFor(g)) * 0.9f : 0.0f;
        float bestCost = 1e9f, bestDepth = 0.0f;
        auto consider = [&](float dp, float dy) {
            quat q = handRot(Side::Right, yaw0 + dy, std::min(1.30f, pitch0 + dp), -0.10f);
            PinchGeo pg = pinchFor(*sk, q, r);
            quat qa, qda;
            vec3 wa = g - rotate(q, pg.point), wda;
            float strain = armStrain(wa, q, &qa);
            if (dst) {
                quat qd = normalize(qy(dstYaw) * q);
                wda = *dst - rotate(qd, pg.point);
                strain += armStrain(wda, qd, &qda);
            }
            const float base = 8.0f * strain + 0.25f * std::fabs(dp) + 0.20f * std::fabs(dy) + 30.0f * pg.err;
            if (base >= bestCost) return;   // cannot win whatever the neighbours
            auto depth = [&](const PinchGeo& p) {
                float d = neighbourDepth(wa + rotate(qa, p.point), qa, p, pieceId, r);
                if (dst) d += neighbourDepth(wda + rotate(qda, p.point), qda, p, pieceId, r);
                return d;
            };
            float d = depth(pg);
            if (base + 100.0f * d < bestCost) {
                bestCost = base + 100.0f * d;
                bestDepth = d;
                best = q;
                out = pg;
            }
            // Between close neighbours: a smaller pre-grasp opening (fingers barely open, as
            // people do in a crowded corner).
            const float tight = 0.3f;
            if (d > 0.0005f && base + tight < bestCost) {
                PinchGeo pt = pinchFor(*sk, q, r, 0.45f, &pg);
                float dt = depth(pt);
                if (base + tight + 100.0f * dt < bestCost) {
                    bestCost = base + tight + 100.0f * dt;
                    bestDepth = dt;
                    best = q;
                    out = pt;
                }
            }
        };
        consider(0.0f, 0.0f);
        if (debugLog) LOGI("anim: grip natural cost %.3f (err %.1f mm)", bestCost, out.err * 1000.0f);
        if (bestCost < 0.01f + 30.0f * out.err) return best;
        // Turned and pitched variants.
        const float dps[] = {0.0f, -0.15f, 0.15f, 0.30f, 0.45f}, dys[] = {0.0f, -0.15f, 0.15f, -0.30f, 0.30f, -0.45f, 0.45f, -0.60f, 0.60f};
        for (float dp : dps)
            for (float dy : dys)
                if (dp != 0.0f || dy != 0.0f) consider(dp, dy);
        if (debugLog) {
            mat4 fr[3];
            fingerFrames(*sk, Side::Right, out.open, Middle, fr);
            vec3 mcp = toWorld(g + rotate(best, fr[0].translation() - out.point)), pip = toWorld(g + rotate(best, fr[1].translation() - out.point));
            vec3 wr = toWorld(g - rotate(best, out.point));
            LOGI("anim: grip predicted open mcp %.3f %.3f %.3f pip %.3f %.3f %.3f wrist %.3f %.3f %.3f", mcp.x, mcp.y, mcp.z, pip.x, pip.y, pip.z, wr.x, wr.y, wr.z);
            quat rel = normalize(best * conjugate(handRot(Side::Right, yaw0, pitch0, -0.10f)));
            LOGI("anim: grip orientation cost %.3f (turned %.2f rad, err %.1f mm, neighbours %.1f mm)", bestCost,
                 2.0f * std::acos(clamp(std::fabs(rel.w), 0.0f, 1.0f)), out.err * 1000.0f, bestDepth * 1000.0f);
        }
        return best;
    }
    // Wrist so that hand-local point 'lp' lands on character-space point 'target' with rotation q.
    static vec3 wristFor(vec3 target, quat q, vec3 lp) { return target - rotate(q, lp); }

    // Capture while the own piece is pinched: the hand rolls thumb side up so the ulnar pocket
    // (curled ring/pinky) reaches the victim while the pinched piece stays clear of the board and
    // of the victim; the victim is taken as low as that allows. Also checks that the victim stays
    // above the board when the hand rolls back level to set the own piece down.
    struct CapturePlan {
        quat q;
        float gy = 0.0f;
    };
    static float lowestRim(vec3 base, vec3 up, float r) { return base.y - r * std::sqrt(std::max(0.0f, 1.0f - up.y * up.y)); }
    CapturePlan capturePlan(const Hand& h, int victimId, vec3 baseC, float boardY) const {
        vec3 gi = gripInfo(victimId);
        float lo = gi.y * 0.85f, hi = std::max(lo, gi.x - 0.008f);
        CapturePlan best;
        best.q = h.carryQ;
        best.gy = hi;
        const quat qG = normalize(h.carryQ);
        const vec3 fwd = rotate(qG, vec3(0, -1, 0));
        const vec3 fh = safeNormalize(vec3(fwd.x, 0, fwd.z), vec3(0, 0, 1));
        const vec3 lat = safeNormalize(cross(kY, fh), vec3(1, 0, 0));
        const vec3 thumbSide = rotate(qG, vec3(0, 0, 1));
        const float sgn = rotate(axisAngle(fh, 0.1f), thumbSide).y > thumbSide.y ? 1.0f : -1.0f;
        const vec3 hp = h.heldAttach.translation();
        const vec3 hu = rotate(rotOf(h.heldAttach), kY);
        vec3 hgi = gripInfo(h.heldId);
        const float rbH = clamp(hgi.z * 1.9f, 0.012f, 0.021f), rbV = clamp(gi.z * 1.9f, 0.012f, 0.021f);
        float bestSlack = -1e9f;
        for (int i = 0; i <= 18; ++i) {
            float roll = 0.15f + 0.05f * float(i);
            quat q = normalize(axisAngle(fh, sgn * roll) * axisAngle(lat, -0.10f) * qG);
            // Own piece relative to the pocket at the grab.
            vec3 rel = rotate(q, hp - h.pocket);
            vec3 up = rotate(q, hu);
            float relLow = lowestRim(rel, up, rbH);
            float gyNeed = boardY + 0.008f - baseC.y - relLow;
            // Above the victim's top where the two overlap horizontally.
            float dh = length(vec3(rel.x, 0, rel.z));
            if (dh < rbH + gi.z * 1.6f) gyNeed = std::max(gyNeed, baseC.y + gi.x + 0.003f - baseC.y - relLow);
            float gy = clamp(gyNeed, lo, hi);
            // Victim when the hand is back at the grip rotation with the own piece on the board.
            vec3 vb = h.pocket - rotate(conjugate(q), vec3(0, gy, 0));
            vec3 vu = rotate(qG, rotate(conjugate(q), kY));
            float placeLow = lowestRim(rotate(qG, vb - hp), vu, rbV);   // relative to the board
            float slack = std::min(hi - gyNeed, placeLow - 0.006f);
            if (slack >= 0.0f) {
                best.q = q;
                best.gy = gy;
                return best;
            }
            if (slack > bestSlack) {
                bestSlack = slack;
                best.q = q;
                best.gy = gy;
            }
        }
        return best;
    }

    Animator* owner = nullptr;

    // Segment builder: from the current sample to (p1, v1) with keys at the ends.
    Segment makeSeg(const HandSample& from, float T, vec3 p1, vec3 v1, quat q1, const FingerPose& f1) const {
        Segment sg;
        sg.T = T;
        sg.p0 = from.p;
        sg.v0 = from.v;
        sg.a0 = from.a;
        sg.p1 = p1;
        sg.v1 = v1;
        sg.rot.add(0.0f, from.q);
        sg.rot.add(1.0f, q1);
        sg.fing.add(0.0f, from.f);
        sg.fing.add(1.0f, f1);
        sg.pen.add(0.0f, from.pen);   // a held pen stays where it is in the fingers
        sg.pen.add(1.0f, from.pen);
        sg.elbow0 = from.elbow;
        sg.elbow1 = 0.0f;
        return sg;
    }
    // Arc height so a hand point that lies 'below' metres under the wrist clears height 'clearY'
    // on the way from p0 to p1 (character space); scaled by the horizontal distance.
    static float arcFor(vec3 p0, vec3 p1, float clearY, float below) {
        float h = length(vec3(p1.x - p0.x, 0, p1.z - p0.z));
        float need = clearY + below - 0.5f * (p0.y + p1.y);
        float s = smoothstep(0.03f, 0.12f, h);
        return std::max(0.0f, need) * 1.15f * s + 0.02f * s;
    }

    void planTask(const Task& t, float start, float T);
    void startTask(const Task& t, std::vector<Event>& ev);
    void fireDue(float upTo, std::vector<Event>& ev);
    void checkRelease(const mat4& actual, vec3 wanted, int id);
    void liftForearm(Hand& h);
    void relaxWrist(Hand& h);
    void finishTask(std::vector<Event>& ev);

    // Pose evaluation at time t (hands from their motions), head from its state.
    void evaluate(float t, Pose& pose, mat4* worldOut);
    void updateGaze(float dt);
    void updateIdle(float dt);
    vec3 headPointWorld() const;
    HandSample chinTarget(Side s) const;
    HandSample handTarget(const Hand& h, float t) const { return handTarget(h, t, h.motion.sample(t)); }
    HandSample handTarget(const Hand& h, float t, HandSample motionSample) const;
    void bakeFollow(Hand& h);

    // ==========================================================================================
    // Left-handed play (animator_writing.cpp). The solver's world is the real one mirrored about
    // X = 0 (S = diag(-1, 1, 1)): the playing hand is the solver's right hand, the writing hand its
    // left one. Inputs are mirrored when they enter (task positions, callbacks, rests, gaze, head
    // yaw), outputs when they leave (bone matrices with left/right swapped, events, held pieces,
    // the pen). Identity when the player plays with the right hand.
    // ==========================================================================================
    bool mirrored = false;
    vec3 mw(vec3 v) const { return mirrored ? vec3(-v.x, v.y, v.z) : v; }   // real <-> solver world
    mat4 mm(const mat4& m) const;                                            // S m S
    vec3 partnerPoint(vec3 partnerSolverWorld) const;                        // partner's solver world -> ours
    void exportPose(const Pose& in, const mat4* worldIn, Pose& out, mat4* worldOut) const;
    void exportEvents(std::vector<Event>& ev, size_t from) const;
    Pose poseI;                    // last evaluated pose / world bone matrices (solver world)
    mat4 worldI[BoneCount];
    // Hand that shakes hands: always the real right hand (the solver's left one when mirrored).
    Hand& shakeHand() { return mirrored ? left() : right(); }
    Side shakeSide() const { return mirrored ? Side::Left : Side::Right; }
    // Side-aware arm strain (armStrain() is the playing hand's).
    float armStrainSide(Side s, vec3 wristC, quat q);
    Side diagSide = Side::Right;   // arm whose solve updates the diagnostics (reachShort, clamps...)

    // ==========================================================================================
    // Writing hand (animator_writing.cpp): hands[0] in the solver, with its own task queue.
    // ==========================================================================================
    struct PenGrip {
        // Everything in the hand-bone frame of the solver's LEFT hand.
        PenPose tripod;             // writing grip (dynamic tripod), fingers at rest extension
        vec3 axis{0, 1, 0};         // pen axis (tip -> back end) in the hand frame
        vec3 heel{0, 0, 0};         // ulnar heel of the palm (support on the paper)
        static constexpr int kSupports = 9;
        vec3 support[kSupports];    // hand points that may rest on the paper (heel, curled ring/little
        float supportR[kSupports];  // finger), with their radii
        FingerPose fingers[3];      // tripod at extension -kExt, 0, +kExt (pen pushed out along -axis)
        float ext = 0.005f;         // kExt (m)
        PenPose tucked;             // pen tucked by middle/ring/pinky (page turn)
        FingerPose tuckOpen, tuckPinch;   // thumb/index open / pinching a page, pen tucked
        vec3 pagePinch{0, 0, 0};    // point between the thumb and index pads (page corner)
        vec3 pageAxis{0, 0, 1};     // thumb pad -> index pad
        float gripDist = 0.03f;     // tip -> index contact along the pen (where it is pinched on the table)
    } grip;
    FingerPose tripodFingers(float ext) const;   // tripod with the pen pushed out by ext along -axis
    // Pen lying on the table at a frame (character space), pinched from above.
    struct TablePinch {
        quat R;                     // hand rotation
        vec3 wrist{0, 0, 0};
        FingerPose closed, open;
        PenPose pen;                // pen in the hand at that grip
    };
    TablePinch tablePinch(const mat4& frameC, float aperture);
    // One segment laying the held pen on the table at frameC (character space), pinched at the end.
    void penPutSegments(const mat4& frameC, const HandSample& from, float T, Motion& mo, FingerPose* openOut);
    struct Writing {
        std::deque<WriteTask> queue;
        bool running = false;
        WriteTask cur;              // running task (solver world)
        float start = 0, T = 0;
        std::vector<TimedEvent> events;
        float pathStart = -1.0f, pathEnd = -1.0f;   // running path phase (absolute times)
        float turnStart = -1.0f, turnT = 0.0f;      // running page turn
        std::vector<PenKey> path;   // running path, character space
        bool penHeld = false;
        mat4 penTable;              // where the pen lies (solver world): initial / last pick / put frame
        vec3 rest{0, 0, 0};         // writing rest point on the paper (character space)
        float restYaw = 0.3f;       // pen azimuth there (see choosePenYaw)
        float suspendUntil = -1.0f; // the writing hand is busy shaking hands (left-handed player)
        std::function<vec3(float s)> corner;   // running page turn: corner (solver world)
        float lean = 0.0f;          // 0..1: the body leans towards the sheet while writing
        float look = 0.0f;          // 0..1: the eyes follow the pen
    } wr;
    enum WAction { WActNone, WActPick, WActPut, WActGripPage, WActTurned, WActDone, WActDown, WActUp };
    void initWriting();
    void solveGrip();               // fills 'grip' for the skeleton
    bool writingHandFree() const;   // no writing task, no pen, no handshake: idle behaviours allowed
    bool nextWriteBoundary(float& t) const;
    void stepWriting(std::vector<Event>& ev);   // starts or finishes the writing task at 'time'
    void startWriteTask(const WriteTask& t);
    void finishWriteTask(std::vector<Event>& ev);
    void fireWriteDue(float upTo, std::vector<Event>& ev);
    void interruptWriting(std::vector<Event>& ev);   // the handshake takes the writing hand
    void planPickPen(const WriteTask& t, float start, float T);
    void planPutPen(const WriteTask& t, float start, float T);
    void planWrite(const WriteTask& t, float start, float T);
    void planTurnPage(const WriteTask& t, float start, float T);
    // Pen-hand pose with the heel resting near the anchor (character space) and the tip at 'tip'
    // (fingers, wrist yaw/pitch about the heel and a small slide share the offset).
    HandSample penHandPose(vec3 anchor, vec3 tip, float yawIn) const;
    quat penBase(vec3 anchor, float yawIn) const;
    float choosePenYaw(vec3 anchor);
    HandSample writingRestSample() const;
    float paperY = 0.0f;            // last paper height seen (character space)
    PenPose evalPen;                // pen in the writing hand at the last evaluate() (tip lock applied)
    mat4 toCharM(const mat4& worldSolver) const { return toMat4(qToChar(rotOf(worldSolver)), toChar(worldSolver.translation())); }
    // Handshake plan for the real right hand (the solver's left one when mirrored, which first lays
    // the pen down if it holds it).
    void planHandshake(const Task& t, float start, float T, HandSample from, Motion& mo);
    bool shakeTookPut = false;      // the handshake took over a queued PutPen
    void writingSpine(SpineParams& sp, const HandSample& hl);
};

// The path of a Trace (character space): the index tip rests on waypoint i from arrive[i] to
// leave[i] (seconds from the start of the path) and moves in a straight line to the next one in
// between (minimum jerk, the hand rotation and the elbow interpolated alongside).
struct Animator::Impl::TracePlan {
    std::vector<vec3> tip;              // tip over each waypoint
    std::vector<quat> q;                // hand rotation there
    std::vector<float> elbow;           // elbow lift there
    std::vector<float> arrive, leave;
    FingerPose f;
    vec3 tipL{0, 0, 0};                 // hand-local index tip
    float P = 0.0f;                     // arrival on the last waypoint
    // Waypoint i and the progress s (0..1, minimum jerk) towards i + 1 at time tt.
    void at(float tt, size_t& i, float& s) const {
        i = 0;
        s = 0.0f;
        const size_t n = tip.size();
        if (n < 2 || tt <= leave[0]) return;
        for (size_t k = 0; k + 1 < n; ++k) {
            if (tt < arrive[k + 1]) {
                i = k;
                s = minJerk(clamp((tt - leave[k]) / std::max(1e-5f, arrive[k + 1] - leave[k]), 0.0f, 1.0f));
                return;
            }
            if (k + 2 == n || tt < leave[k + 1]) {
                i = k + 1;
                return;
            }
        }
    }
    vec3 tipAt(float tt) const {
        size_t i;
        float s;
        at(tt, i, s);
        return s > 0.0f ? lerp(tip[i], tip[i + 1], s) : tip[i];
    }
    HandSample poseAt(float tt) const {
        size_t i;
        float s;
        at(tt, i, s);
        HandSample h;
        const vec3 tp = s > 0.0f ? lerp(tip[i], tip[i + 1], s) : tip[i];
        h.q = s > 0.0f ? qslerp(q[i], q[i + 1], s) : q[i];
        h.elbow = s > 0.0f ? lerp(elbow[i], elbow[i + 1], s) : elbow[i];
        h.f = f;
        h.p = tp - rotate(h.q, tipL);
        h.pinW = 1.0f;   // the tip exactly on its path
        h.pinLocal = tipL;
        return h;
    }
    // With the wrist velocity and acceleration (symmetric differences, as the pen paths).
    HandSample sample(float tt) const {
        const float hstep = 1.0f / 480.0f;
        HandSample s = poseAt(tt);
        const float ta = std::max(0.0f, tt - hstep), tb = std::min(P, tt + hstep);
        const vec3 pa = poseAt(ta).p, pb = poseAt(tb).p;
        if (tb - ta > 1e-5f) {
            s.v = (pb - pa) / (tb - ta);
            s.a = tb - ta > 1.9f * hstep ? (pb - s.p * 2.0f + pa) / (hstep * hstep) : vec3(0);
        }
        return s;
    }
};

}  // namespace anim
