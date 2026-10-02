// The writing hand and left-handed play (see animator.h and the header comment of animator_impl.h).
//
// Writing hand (the solver's left hand, hands[0], with its own task queue running at the same time
// as the playing hand's):
//   * Grips, solved once on the robot's hand at init: the dynamic tripod (thumb and index pads and
//     the side of the middle finger on the barrel, the barrel resting in the thumb-index web), the
//     pen tucked under the last three fingers while thumb and index pinch a page, and the pinch that
//     picks the pen up from / lays it on the table.
//   * Path following: the hand rests on the ulnar heel of the palm near an anchor that slides
//     smoothly along the line (the pen path low-passed over ~0.2 s); the tip's offset from that
//     anchor is shared (weighted least squares) by the fingers pushing the pen along its axis, a
//     turn and a pitch of the hand about the heel, and a small slide of the whole hand. The tip is
//     exact (penPathPoint) whatever the arm reaches: the pen slides in the fingers by the residual.
//   * Page turn: the hand tucks the pen, pinches the corner, lifts the page past the vertical and
//     lets it go; the page falls over the binding by itself. pageTurnEase() drives both.
// Left-handed play: the solver runs in the world mirrored about X = 0 (Impl::mirrored).
#include "animator_impl.h"
#include <algorithm>

namespace anim {

namespace {

// Page turn phases (fractions of the task): pinch closes, release (the page is past the vertical),
// the page lies flipped.
constexpr float kTurnGrip = 0.33f, kTurnRelease = 0.70f, kTurnReleaseS = 0.60f, kTurnLand = 0.90f;

inline float hermite(float p0, float m0, float p1, float m1, float u) {
    float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1;
}
inline vec3 hermite(vec3 p0, vec3 m0, vec3 p1, vec3 m1, float u) {
    float u2 = u * u, u3 = u2 * u;
    return p0 * (2 * u3 - 3 * u2 + 1) + m0 * (u3 - 2 * u2 + u) + p1 * (-2 * u3 + 3 * u2) + m1 * (u3 - u2);
}
// Key i with k[i].t <= t < k[i + 1].t (k.size() >= 2, k[0].t <= t < k.back().t).
size_t pathSegment(const std::vector<PenKey>& k, float t) {
    size_t lo = 0, hi = k.size() - 1;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (k[mid].t <= t) lo = mid;
        else hi = mid;
    }
    return lo;
}

inline quat mirrorQ(quat q) { return quat(q.x, -q.y, -q.z, q.w); }   // S R S with S = diag(-1, 1, 1)
inline PenPose mirrorPen(const PenPose& p) { return {mirrorQ(p.q), mirrorX(p.p)}; }
Bone mirrorBone(Bone b) {
    if (b >= ClavicleL && b <= PinkyL3) return Bone(b + (ClavicleR - ClavicleL));
    if (b >= ClavicleR && b <= PinkyR3) return Bone(b - (ClavicleR - ClavicleL));
    if (b >= ThighL && b <= FootL) return Bone(b + (ThighR - ThighL));
    if (b >= ThighR && b <= FootR) return Bone(b - (ThighR - ThighL));
    switch (b) {
        case EyeL: return EyeR;
        case EyeR: return EyeL;
        case LidUpperL: return LidUpperR;
        case LidUpperR: return LidUpperL;
        case LidLowerL: return LidLowerR;
        case LidLowerR: return LidLowerL;
        default: return b;
    }
}

// ---- grip geometry (right-hand convention; the writing hand gets the mirror image) -------------
// Dynamic tripod preset, tuned on the robot's hand in the anim viewer (the finger solve below snaps
// the thumb, index and middle onto the barrel from there). Rows as in animator_impl.h.
FingerPose poseTripodPreset() {
    return fpMake({{1.05f, 0.30f, 0.30f, 0.30f}, {0.02f, 0.62f, 0.62f, 0.22f}, {-0.03f, 0.80f, 1.05f, 0.55f},
                   {-0.07f, 1.10f, 1.35f, 0.80f}, {-0.13f, 1.22f, 1.40f, 0.85f}});
}
// Page turn: the pen lies under the curled middle/ring/little fingers, thumb and index are free.
FingerPose poseTuckPreset() {
    return fpMake({{0.70f, 0.20f, 0.25f, 0.20f}, {0.02f, 0.45f, 0.45f, 0.20f}, {-0.02f, 1.05f, 1.35f, 0.75f},
                   {-0.05f, 1.10f, 1.45f, 0.85f}, {-0.09f, 1.15f, 1.45f, 0.85f}});
}
struct Contact {
    vec3 p, n;   // hand-local point on the skin, outward normal
};
Contact padContact(const Skeleton& sk, const FingerPose& fp, int f) {
    mat4 fr[3];
    fingerFrames(sk, Side::Right, fp, f, fr);
    Bone b3 = fingerBone(Side::Right, f, 2);
    vec3 d = boneDir(sk, b3);
    vec3 padDir = f == Thumb ? normalize(cross(kX, d)) : vec3(1, 0, 0);
    return {transformPoint(fr[2], d * (sk.boneLength[b3] * 0.72f) + padDir * kPadRadius), normalize(transformDir(fr[2], padDir))};
}
// Radial side of the middle finger's distal phalanx (the barrel rests on it).
Contact middleSide(const Skeleton& sk, const FingerPose& fp) {
    mat4 fr[3];
    fingerFrames(sk, Side::Right, fp, Middle, fr);
    Bone b3 = fingerBone(Side::Right, Middle, 2);
    vec3 d = boneDir(sk, b3), side = normalize(vec3(0.45f, 0.0f, 1.0f));
    return {transformPoint(fr[2], d * (sk.boneLength[b3] * 0.55f) + side * kPadRadius), normalize(transformDir(fr[2], side))};
}
Contact contactOf(const Skeleton& sk, const FingerPose& fp, int f) { return f == Middle ? middleSide(sk, fp) : padContact(sk, fp, f); }
// Moves finger f (thumb, index or middle) so its contact point reaches 'target'.
float snapFinger(const Skeleton& sk, FingerPose& fp, int f, vec3 target) {
    float err = 0.0f;
    for (int it = 0; it < 3; ++it) {
        Contact c = contactOf(sk, fp, f);
        vec3 want = fingerPad(sk, Side::Right, fp, f) + (target - c.p);
        float v[4];
        if (f == Thumb) solveThumb(sk, Side::Right, want, v, vec3(fp.v[0][0], fp.v[0][1], fp.v[0][2]));
        else solveLongFinger(sk, Side::Right, f, want, v, fp.v[f][1], fp.v[f][2]);
        for (int j = 0; j < 4; ++j) fp.v[f][j] = v[j];
        err = length(contactOf(sk, fp, f).p - target);
        if (err < 2e-4f) break;
    }
    return err;
}
// Weighted least-squares line through points (centroid c, unit direction d).
void fitLine(const vec3* p, const float* w, int n, vec3& c, vec3& d) {
    float W = 0.0f;
    c = vec3(0);
    for (int i = 0; i < n; ++i) { c += p[i] * w[i]; W += w[i]; }
    c /= W;
    mat3 M(vec3(0), vec3(0), vec3(0));
    for (int i = 0; i < n; ++i) {
        vec3 q = p[i] - c;
        M.c[0] += q * (w[i] * q.x);
        M.c[1] += q * (w[i] * q.y);
        M.c[2] += q * (w[i] * q.z);
    }
    d = safeNormalize(p[n - 1] - p[0], vec3(0, 1, 0));
    for (int it = 0; it < 40; ++it) d = safeNormalize(M * d, d);
}
// Pen pinched as it lies on the table (right-hand convention): thumb pad on one side of the barrel,
// index and middle pads on the other, above its equator so the fingertips clear the table. R: hand
// rotation, penDir: pen axis (tip -> back), both in the same space. 'axis' = thumb -> index.
PinchGeo penPinch(const Skeleton& sk, quat R, vec3 penDir, float aperture) {
    const float r = layout::PEN_RADIUS, rise = 0.80f;   // contacts 46 degrees above the equator
    PinchGeo g;
    g.point = kPinchPoint;
    vec3 across = safeNormalize(cross(kY, penDir), vec3(1, 0, 0));
    if (dot(across, rotate(R, kPinchAxisPref)) < 0.0f) across = -across;
    vec3 al = rotate(conjugate(R), across), up = rotate(conjugate(R), kY), pd = rotate(conjugate(R), penDir);
    g.axis = al;
    const float rr = r * std::cos(rise);
    auto solve = [&](float gap, FingerPose& fp) {
        float e = 0, v[4];
        e = std::max(e, solveThumb(sk, Side::Right, g.point - al * (rr + gap), v));
        for (int j = 0; j < 4; ++j) fp.v[Thumb][j] = v[j];
        e = std::max(e, solveLongFinger(sk, Side::Right, Index, g.point + al * (rr + gap), v));
        for (int j = 0; j < 4; ++j) fp.v[Index][j] = v[j];
        e = std::max(e, solveLongFinger(sk, Side::Right, Middle, g.point + al * (rr + gap) + pd * 0.013f + up * 0.001f, v));
        for (int j = 0; j < 4; ++j) fp.v[Middle][j] = v[j];
        fp.v[Ring][0] = -0.05f;
        fp.v[Ring][1] = std::min(1.30f, fp.v[Middle][1] + 0.30f);
        fp.v[Ring][2] = std::min(1.60f, fp.v[Middle][2] + 0.40f);
        fp.v[Ring][3] = kDipCoupling * fp.v[Ring][2];
        fp.v[Pinky][0] = -0.11f;
        fp.v[Pinky][1] = std::min(1.40f, fp.v[Middle][1] + 0.45f);
        fp.v[Pinky][2] = std::min(1.65f, fp.v[Middle][2] + 0.45f);
        fp.v[Pinky][3] = kDipCoupling * fp.v[Pinky][2];
        return e;
    };
    g.err = solve(0.0f, g.pose);
    solve(0.012f * aperture, g.open);
    return g;
}

// Optional tuning of the writing posture: SCACELITH_PEN_POSE="elevMin elevMax palmDown rollOff"
// (radians), for experiments in the anim viewer.
struct PenPoseParams {
    float elevMin = 0.72f, elevMax = 1.10f;       // pen elevation above the paper (~41..63 degrees)
    float palmDown = 0.62f;                       // palm normal: that far from straight down, inwards
    float rollOff = 0.0f;                         // extra roll of the hand about the pen
};
const PenPoseParams& penParams() {
    static PenPoseParams p = [] {
        PenPoseParams q;
        if (const char* e = std::getenv("SCACELITH_PEN_POSE"))
            std::sscanf(e, "%f %f %f %f", &q.elevMin, &q.elevMax, &q.palmDown, &q.rollOff);
        return q;
    }();
    return p;
}

}  // namespace

// =============================================================================================
// Public helpers
// =============================================================================================
float writeTaskDuration(const WriteTask& t) {
    switch (t.type) {
        case WriteTaskType::PickPen: return Timing::PickPen;
        case WriteTaskType::PutPen: return Timing::PutPen;
        case WriteTaskType::TurnPage: return t.duration > 0.0f ? t.duration : Timing::PageTurn;
        case WriteTaskType::Write: return Timing::WriteApproach + (t.path.empty() ? 0.0f : t.path.back().t) + Timing::WriteRetract;
        case WriteTaskType::Wait: return std::max(0.0f, t.duration);
    }
    return 0.0f;
}

float pageTurnEase(float u) {
    if (u <= kTurnGrip) return 0.0f;
    if (u >= kTurnLand) return 1.0f;
    if (u <= kTurnRelease) {
        // The hand lifts the page from rest and is moving briskly when it lets go.
        const float d = kTurnRelease - kTurnGrip;
        return hermite(0.0f, 0.0f, kTurnReleaseS, 1.9f * d, (u - kTurnGrip) / d);
    }
    // Past the vertical the page falls over by itself and lands on the far side.
    const float d = kTurnLand - kTurnRelease;
    return hermite(kTurnReleaseS, 1.9f * d, 1.0f, 1.2f * d, (u - kTurnRelease) / d);
}

vec3 penPathPoint(const std::vector<PenKey>& k, float t) {
    const size_t n = k.size();
    if (n == 0) return vec3(0);
    if (n == 1 || t <= k[0].t) return k[0].tip;
    if (t >= k[n - 1].t) return k[n - 1].tip;
    const size_t i = pathSegment(k, t);
    const float h = std::max(1e-6f, k[i + 1].t - k[i].t), u = clamp((t - k[i].t) / h, 0.0f, 1.0f);
    // Catmull-Rom velocities for non-uniform key times, scaled to the segment.
    auto slope = [&](size_t j) {
        size_t a = j > 0 ? j - 1 : j, b = j + 1 < n ? j + 1 : j;
        float dt = k[b].t - k[a].t;
        return dt > 1e-6f ? (k[b].tip - k[a].tip) * (h / dt) : vec3(0);
    };
    const vec3 p0 = k[i].tip, p1 = k[i + 1].tip;
    vec3 m0 = slope(i), m1 = slope(i + 1);
    vec3 p = hermite(p0, m0, p1, m1, u);
    // Heights: straight on the paper while down; off and onto the paper with no vertical speed and
    // monotone in between, so the tip never dips below the paper.
    if (k[i].down) {
        p.y = lerp(p0.y, p1.y, u);
    } else {
        float d = p1.y - p0.y, a = m0.y, b = m1.y;
        if (i == 0 || k[i - 1].down) a = 0.0f;
        if (k[i + 1].down) b = 0.0f;
        if (a * d <= 0.0f) a = 0.0f;
        if (b * d <= 0.0f) b = 0.0f;
        a = clamp(a, -3.0f * std::fabs(d), 3.0f * std::fabs(d));
        b = clamp(b, -3.0f * std::fabs(d), 3.0f * std::fabs(d));
        p.y = hermite(p0.y, a, p1.y, b, u);
    }
    return p;
}

bool penPathDown(const std::vector<PenKey>& k, float t) {
    if (k.size() < 2 || t < k.front().t || t >= k.back().t) return false;
    return k[pathSegment(k, t)].down;
}

// =============================================================================================
// Left-handed play: the mirror layer
// =============================================================================================
mat4 Animator::Impl::mm(const mat4& a) const {
    if (!mirrored) return a;
    // S a S with S = diag(-1, 1, 1, 1): row 0 and column 0 change sign (element 0,0 twice).
    mat4 r = a;
    r.c[0] = vec4(a.c[0].x, -a.c[0].y, -a.c[0].z, -a.c[0].w);
    for (int c = 1; c < 4; ++c) r.c[c].x = -a.c[c].x;
    return r;
}

vec3 Animator::Impl::partnerPoint(vec3 p) const {
    if (!partner || !partner->impl_) return p;
    return mw(partner->impl_->mw(p));
}

void Animator::Impl::exportPose(const Pose& in, const mat4* win, Pose& out, mat4* wout) const {
    if (!mirrored) {
        out = in;
        for (int b = 0; b < BoneCount; ++b) wout[b] = win[b];
        return;
    }
    // The left bones of the real body are the mirrored right bones of the solver's body.
    for (int b = 0; b < BoneCount; ++b) {
        Bone src = mirrorBone(Bone(b));
        out.local[b] = mirrorQ(in.local[src]);
        wout[b] = mm(win[src]);
    }
    out.rootPosition = mw(in.rootPosition);
    out.rootRotation = mirrorQ(in.rootRotation);
}

void Animator::Impl::exportEvents(std::vector<Event>& ev, size_t from) const {
    if (!mirrored) return;
    for (size_t i = from; i < ev.size(); ++i) {
        ev[i].position = mw(ev[i].position);
        ev[i].transform = mm(ev[i].transform);
    }
}

float Animator::Impl::armStrainSide(Side s, vec3 wristC, quat q) {
    if (s == Side::Right) return armStrain(wristC, q);
    // The mirror image of armStrain(): the torso leans and turns for this hand the way it does for
    // the right one.
    Pose tmp;
    const Side keep = diagSide;
    diagSide = s;
    SpineParams sp = solveSpine(tmp, mirrorX(wristC), 0.0f, 0.0f, 0.0f, 0.0f);
    sp.twist = -sp.twist;
    sp.side = -sp.side;
    reachShort = wristClamp = pronClamp = 0;
    applySpine(tmp, sp);
    fkChain(tmp, Pelvis, Spine2);
    solveArm(tmp, s, wristC, q);
    float soft = softWristStrain(lastFlex, lastDev, lastPron);
    float r = wristClamp + pronClamp + reachShort * 10.0f + 0.5f * soft;
    diagSide = keep;
    reachShort = wristClamp = pronClamp = 0;
    return r;
}

// =============================================================================================
// Grips
// =============================================================================================
void Animator::Impl::initWriting() {
    wr = Writing();
    // The grips only depend on the skeleton: solved once.
    static const Skeleton* solvedFor = nullptr;
    static PenGrip solved;
    if (solvedFor != sk) {
        solveGrip();
        solved = grip;
        solvedFor = sk;
    }
    grip = solved;

    // Defaults: the pen beside the pad's outer edge, the writing rest on the pad's margin.
    const float r = layout::PEN_RADIUS;
    const float paper = layout::TABLE_TOP_Y + layout::SCORESHEET_THICKNESS - pelvisWorld.y;
    const float padZ = layout::PLAYER_PELVIS_Z - layout::SCORESHEET_Z;   // pad centre, character space
    wr.rest = vec3(layout::SCORESHEET_X + 0.035f, paper, padZ + 0.02f);
    wr.restYaw = choosePenYaw(wr.rest);
    paperY = paper;
    {
        vec3 tipC(layout::SCORESHEET_X + layout::SCORESHEET_WIDTH * 0.5f + 0.03f, layout::TABLE_TOP_Y + r - pelvisWorld.y, padZ + 0.07f);
        mat4 penC = toMat4(fromTo(kY, vec3(0, 0, -1)), tipC);   // tip towards the board
        wr.penTable = root * penC;
    }
}

void Animator::Impl::solveGrip() {
    const Skeleton& S = *sk;
    const float r = layout::PEN_RADIUS;
    PenGrip& g = grip;

    // ---- dynamic tripod (right-hand convention first)
    FingerPose f = poseTripodPreset();
    // Where the barrel rests on the radial side of the index finger's base (the web).
    auto webPoint = [&](const FingerPose& fp) {
        mat4 fr[3];
        fingerFrames(S, Side::Right, fp, Index, fr);
        Bone b1 = fingerBone(Side::Right, Index, 0);
        return transformPoint(fr[0], vec3(0.0f, -0.28f * S.boneLength[b1], 0.0f) + vec3(0.001f, 0.0f, 0.0086f + r));
    };
    const int fingers3[3] = {Thumb, Index, Middle};
    vec3 pts[4];
    float w[4] = {1.0f, 1.0f, 0.6f, 1.2f};
    for (int i = 0; i < 3; ++i) {
        Contact c = contactOf(S, f, fingers3[i]);
        pts[i] = c.p + c.n * r;
    }
    pts[3] = webPoint(f);
    vec3 c, D;
    fitLine(pts, w, 4, c, D);
    if (dot(D, pts[3] - pts[1]) < 0.0f) D = -D;   // towards the back end (the web)
    float fitErr = 0.0f;
    for (int i = 0; i < 4; ++i) fitErr = std::max(fitErr, length(perp(pts[i] - c, D)));
    // Snap the pads onto the barrel surface.
    vec3 onPen[3];
    float err = 0.0f;
    for (int i = 0; i < 3; ++i) {
        Contact cc = contactOf(S, f, fingers3[i]);
        vec3 q = c + D * dot(cc.p - c, D);
        vec3 out = safeNormalize(perp(cc.p - q, D), -cc.n);
        onPen[i] = q + out * r;
        err = std::max(err, snapFinger(S, f, fingers3[i], onPen[i]));
    }
    // Tip: 2 cm beyond the most distal contact, and clear of the fingertips.
    float sTip = 1e9f;
    for (int i = 0; i < 3; ++i) sTip = std::min(sTip, dot(onPen[i] - c, D) - 0.021f);
    for (int fi : {Thumb, Index, Middle}) sTip = std::min(sTip, dot(fingerTip(S, Side::Right, f, fi) - c, D) - 0.013f);
    vec3 tipR = c + D * sTip;
    vec3 zr = safeNormalize(perp(vec3(-1, 0, 0), D), vec3(0, 0, 1));
    quat qR = fromMat3(mat3(cross(D, zr), D, zr));
    g.gripDist = dot(onPen[1] - tipR, D);
    // Pushed out / drawn in by the fingers: the contacts move along the axis with the pen.
    FingerPose fe[3] = {f, f, f};
    for (int k = 0; k < 3; k += 2) {
        float e = k == 0 ? -g.ext : g.ext;
        for (int i = 0; i < 3; ++i) err = std::max(err, snapFinger(S, fe[k], fingers3[i], onPen[i] - D * e));
    }
    // ---- to the writing (left) hand
    g.tripod = mirrorPen({qR, tipR});
    g.axis = mirrorX(D);
    for (int k = 0; k < 3; ++k) g.fingers[k] = fe[k];
    // Ulnar heel of the palm (it rests on the paper while writing).
    g.heel = mirrorX(vec3(0.010f, -0.035f, -0.033f));
    {
        // Whatever of the hand may touch the paper: the heel, the curled ring and little fingers.
        int n = 0;
        auto add = [&](vec3 pR, float rad) {
            g.support[n] = mirrorX(pR);
            g.supportR[n++] = rad;
        };
        add(vec3(0.011f, -0.022f, -0.030f), 0.003f);
        add(vec3(0.009f, -0.048f, -0.032f), 0.004f);
        const FingerPose& fp = g.fingers[1];
        for (int fi : {Ring, Pinky}) {
            mat4 fr[3];
            fingerFrames(S, Side::Right, fp, fi, fr);
            add(fr[1].translation(), 0.0080f);
            add(fr[2].translation(), 0.0072f);
            add(fingerTip(S, Side::Right, fp, fi), 0.0066f);
        }
        add(phalanxMid(S, Side::Right, fp, Pinky, 0), 0.0085f);
    }

    // ---- page pinch with the pen tucked
    FingerPose t = poseTuckPreset();
    const vec3 G = kPinchPoint + vec3(-0.004f, 0.006f, 0.0f), n = normalize(kPinchAxisPref);
    FingerPose tp = t, to = t;
    {
        float v[4];
        err = std::max(err, solveThumb(S, Side::Right, G - n * 0.0004f, v));
        for (int j = 0; j < 4; ++j) tp.v[Thumb][j] = v[j];
        err = std::max(err, solveLongFinger(S, Side::Right, Index, G + n * 0.0004f, v));
        for (int j = 0; j < 4; ++j) tp.v[Index][j] = v[j];
        solveThumb(S, Side::Right, G - n * 0.016f, v);
        for (int j = 0; j < 4; ++j) to.v[Thumb][j] = v[j];
        solveLongFinger(S, Side::Right, Index, G + n * 0.012f, v);
        for (int j = 0; j < 4; ++j) to.v[Index][j] = v[j];
    }
    g.tuckPinch = tp;
    g.tuckOpen = to;
    g.pagePinch = mirrorX(G);
    g.pageAxis = mirrorX(n);
    {
        // Barrel from the pocket under the curled last fingers to the web; the tip sticks out on the
        // little-finger side.
        vec3 P = pocketPoint(S, tp) + vec3(0.001f, 0.0f, 0.0f), B = webPoint(tp);
        vec3 Dt = normalize(B - P);
        vec3 tipT = P - Dt * 0.034f;
        vec3 zt = safeNormalize(perp(vec3(-1, 0, 0), Dt), vec3(0, 0, 1));
        g.tucked = mirrorPen({fromMat3(mat3(cross(Dt, zt), Dt, zt)), tipT});
    }
    LOGI("anim: pen grip solved (tripod fit %.1f mm, worst pad miss %.1f mm, pinch at %.1f mm from the tip)", fitErr * 1000.0f, err * 1000.0f,
         g.gripDist * 1000.0f);
    if (debugLog) {
        const FingerPose& p = g.fingers[1];
        for (int fi = 0; fi < 5; ++fi) LOGI("anim:   tripod finger %d: %.2f %.2f %.2f %.2f", fi, p.v[fi][0], p.v[fi][1], p.v[fi][2], p.v[fi][3]);
        LOGI("anim:   tripod tip (right) %.3f %.3f %.3f axis %.2f %.2f %.2f", tipR.x, tipR.y, tipR.z, D.x, D.y, D.z);
    }
}

// Fingers of the tripod for a pen pushed out by e (m) along its axis (quadratic through the three
// solved poses, a little extrapolation allowed).
FingerPose Animator::Impl::tripodFingers(float e) const {
    const PenGrip& g = grip;
    float u = clamp(e / g.ext, -1.6f, 1.6f);
    FingerPose r;
    for (int f = 0; f < 5; ++f)
        for (int j = 0; j < 4; ++j) {
            float a = g.fingers[0].v[f][j], b = g.fingers[1].v[f][j], c = g.fingers[2].v[f][j];
            r.v[f][j] = b + u * (c - a) * 0.5f + u * u * (c - 2.0f * b + a) * 0.5f;
        }
    return r;
}

// =============================================================================================
// Writing posture
// =============================================================================================
// Base orientation of the writing hand: the back of the pen points back and 'yawIn' inwards (about
// along the forearm, see choosePenYaw), the palm faces down and inwards, and the pen is as steep as
// it takes for the lowest point of the hand (the ulnar heel or the curled little finger) to rest on
// the paper while the tip is at the anchor A.
quat Animator::Impl::penBase(vec3 A, float yawIn) const {
    const PenGrip& g = grip;
    const PenPoseParams& pp = penParams();
    const vec3 h(-std::sin(yawIn), 0.0f, -std::cos(yawIn));
    const vec3 want = normalize(vec3(-std::sin(pp.palmDown), -std::cos(pp.palmDown), 0.0f));   // down and inwards (-X)
    auto orient = [&](float elev) {
        vec3 D = h * std::cos(elev) + kY * std::sin(elev);
        quat Ra = fromTo(g.axis, D);
        vec3 a = perp(rotate(Ra, vec3(-1, 0, 0)), D), b = perp(want, D);
        float ang = std::atan2(dot(cross(a, b), D), dot(a, b));
        return normalize(axisAngle(D, ang + pp.rollOff) * Ra);
    };
    auto lowY = [&](float elev) {
        quat R = orient(elev);
        vec3 p = A - rotate(R, g.tripod.p);
        float y = 1e9f;
        for (int i = 0; i < PenGrip::kSupports; ++i) y = std::min(y, (p + rotate(R, g.support[i])).y - g.supportR[i]);
        return y - A.y;
    };
    float lo = pp.elevMin, hi = pp.elevMax;
    const float yLo = lowY(lo), yHi = lowY(hi);
    float elev;
    if (yLo * yHi > 0.0f) {
        elev = std::fabs(yLo) < std::fabs(yHi) ? lo : hi;
    } else {
        const bool rising = yHi > yLo;
        for (int it = 0; it < 14; ++it) {
            float mid = 0.5f * (lo + hi);
            if ((lowY(mid) < 0.0f) == rising) lo = mid;
            else hi = mid;
        }
        elev = 0.5f * (lo + hi);
    }
    return orient(elev);
}

// Azimuth of the pen for writing around A: the one that bends the wrist least (the forearm comes
// from the elbow, out to the side of the body; a pen pointing at the shoulder would need a radial
// deviation the wrist does not have), a slight ulnar deviation preferred.
float Animator::Impl::choosePenYaw(vec3 A) {
    float best = 0.3f, bestCost = 1e9f;
    for (int k = -12; k <= 30; ++k) {
        const float d = 0.05f * float(k);
        const quat R = penBase(A, d);
        const vec3 p = A - rotate(R, grip.tripod.p);
        float c = armStrainSide(Side::Left, p, R);
        c += 0.6f * std::fabs(lastDev + 0.12f) + 0.3f * std::fabs(lastFlex + 0.35f) + 0.05f * std::fabs(d - 0.3f);
        if (c < bestCost) {
            bestCost = c;
            best = d;
        }
    }
    if (debugLog) LOGI("anim: pen azimuth %.0f deg inwards at %.3f %.3f %.3f (cost %.3f)", best / DEG, A.x, A.y, A.z, bestCost);
    return best;
}

HandSample Animator::Impl::penHandPose(vec3 A, vec3 T, float yawIn) const {
    const PenGrip& g = grip;
    const quat R0 = penBase(A, yawIn);
    const vec3 h(-std::sin(yawIn), 0.0f, -std::cos(yawIn));
    const vec3 p0 = A - rotate(R0, g.tripod.p);
    const vec3 H = p0 + rotate(R0, g.heel);
    const vec3 D = rotate(R0, g.axis);

    // Offset of the tip from the anchor, shared by (tip metres): slide x/y/z, turn about the heel,
    // pitch about the heel, fingers along the pen. Costs: the heel stays put on the paper, the fingers
    // do the small strokes, the wrist turns for the sideways ones.
    const vec3 r = T - A;
    const vec3 k = safeNormalize(cross(kY, h), vec3(1, 0, 0));
    vec3 col[6] = {vec3(1, 0, 0), kY, vec3(0, 0, 1), cross(kY, A - H), cross(k, A - H), -D};
    const float cost[6] = {2.5f, 30.0f, 2.5f, 1.0f, 1.4f, 0.45f};
    float len[6];
    mat3 M(vec3(0), vec3(0), vec3(0));
    for (int i = 0; i < 6; ++i) {
        len[i] = std::max(1e-4f, length(col[i]));
        vec3 u = col[i] / len[i];
        float wi = 1.0f / cost[i];
        M.c[0] += u * (wi * u.x);
        M.c[1] += u * (wi * u.y);
        M.c[2] += u * (wi * u.z);
    }
    const vec3 lam = inverse(M) * r;
    float x[6];
    for (int i = 0; i < 6; ++i) x[i] = dot(col[i] / len[i], lam) / cost[i] / len[i];
    const float e = clamp(x[5], -1.6f * g.ext, 1.6f * g.ext);
    const quat Q = normalize(axisAngle(kY, x[3]) * axisAngle(k, x[4]));
    HandSample s;
    s.q = normalize(Q * R0);
    s.f = tripodFingers(e);
    s.pen = {g.tripod.q, g.tripod.p - g.axis * e};
    s.p = T - rotate(s.q, s.pen.p);   // exact tip: the slide takes whatever the rest did not
    s.tip = T;
    return s;
}

HandSample Animator::Impl::writingRestSample() const {
    // Heel on the paper beside the next row, the tip lifted a little off it.
    return penHandPose(wr.rest, wr.rest + vec3(0.0f, 0.011f, 0.0f), wr.restYaw);
}

// Hand pose for a pen lying on the table at Fc (character space): pinched from above, the hand
// turned so the pinch is across the barrel, as comfortable for the arm as it gets.
Animator::Impl::TablePinch Animator::Impl::tablePinch(const mat4& Fc, float aperture) {
    const float r = layout::PEN_RADIUS;
    const vec3 tip = Fc.translation(), dir = safeNormalize(transformDir(Fc, kY), vec3(0, 0, -1));
    const vec3 G = tip + dir * grip.gripDist + kY * (r * std::sin(0.80f));
    TablePinch best;
    float bestCost = 1e9f;
    const vec3 S = shoulderRest(Side::Left);
    const float yawNat = std::atan2(G.x - S.x, std::max(0.08f, G.z - S.z)) * 0.8f - 0.22f;
    for (int k = -14; k <= 14; ++k) {
        const float yaw = 0.1f * float(k);
        for (float pitch : {0.75f, 0.95f}) {
            quat Rl = handRot(Side::Left, yaw, pitch, -0.10f);
            PinchGeo pg = penPinch(*sk, mirrorQ(Rl), mirrorX(dir), aperture);
            vec3 wrist = G - rotate(Rl, mirrorX(pg.point));
            float cost = armStrainSide(Side::Left, wrist, Rl) + 30.0f * pg.err + 0.25f * std::fabs(yaw - yawNat) + 0.2f * std::fabs(pitch - 0.85f);
            // The fingertips stay above the table.
            float lowTip = 1e9f;
            for (int f = 0; f < 5; ++f) lowTip = std::min(lowTip, (wrist + rotate(Rl, mirrorX(fingerTip(*sk, Side::Right, pg.pose, f)))).y - kPadRadius);
            cost += 200.0f * std::max(0.0f, tip.y - r + 0.001f - lowTip);
            if (cost < bestCost) {
                bestCost = cost;
                best.R = Rl;
                best.wrist = wrist;
                best.closed = pg.pose;
                best.open = pg.open;
            }
        }
    }
    // Pen in the hand at that grip.
    best.pen.q = normalize(conjugate(best.R) * rotOf(Fc));
    best.pen.p = rotate(conjugate(best.R), tip - best.wrist);
    return best;
}

// =============================================================================================
// Planning
// =============================================================================================
namespace {
// Pen frame (character space) as a lock target.
PenPose frameOf(const mat4& Fc) { return {rotOf(Fc), Fc.translation()}; }
}  // namespace

void Animator::Impl::planPickPen(const WriteTask& t, float start, float T) {
    Hand& h = left();
    HandSample from = h.motion.sample(start);
    const mat4 Fc = toCharM(t.frame);
    const TablePinch tp = tablePinch(Fc, 1.0f);
    const float sc = T / Timing::PickPen;
    const float tA = 0.36f * sc, tB = 0.10f * sc, tC = T - tA - tB;
    const float tableC = layout::TABLE_TOP_Y - pelvisWorld.y;
    Motion mo;
    mo.start = start;
    // Reach: over the pen, fingers open, down onto it, pinch.
    Segment a = makeSeg(from, tA, tp.wrist, vec3(0), tp.R, tp.closed);
    a.he = 0.74f;
    a.arcH = 0.03f;
    a.arcPeak = 0.40f;
    if (from.p.y - tableC < 0.10f && length(from.v) < 0.05f) a.hs = 0.07f;
    a.rot.keys.clear();
    a.rot.add(0.0f, from.q);
    a.rot.add(0.72f, tp.R);
    a.fing.keys.clear();
    a.fing.add(0.0f, from.f);
    a.fing.add(0.50f, tp.open);
    a.fing.add(0.84f, tp.open);
    a.fing.add(1.0f, tp.closed);
    a.pen.keys.clear();
    a.pen.add(0.0f, tp.pen);
    clearPath(a, from, fpLerp(from.f, tp.open, 0.5f), tableC, Side::Left);
    mo.segs.push_back(a);
    HandSample s = a.sample(tA);
    // Lift: the pen leaves the table at the start (exactly where it lay: locked to its frame, the
    // lock fades out while it rises).
    const HandSample rest = writingRestSample();
    Segment b = makeSeg(s, tB, s.p + vec3(0, 0.030f, 0) + (rest.p - s.p) * 0.12f, (rest.p - s.p) * (0.8f / tC) + vec3(0, 0.05f, 0), tp.R, tp.closed);
    b.pen.keys.clear();
    b.pen.add(0.0f, tp.pen);
    b.lockC = frameOf(Fc);
    b.lockFrom = 1.0f;
    b.lockTo = 0.0f;
    mo.segs.push_back(b);
    s = b.sample(tB);
    // Into the writing grip on the way to the writing rest.
    Segment c = makeSeg(s, tC, rest.p, vec3(0), rest.q, rest.f);
    c.rot.keys.clear();
    c.rot.add(0.0f, s.q);
    c.rot.add(0.85f, rest.q);
    c.fing.keys.clear();
    c.fing.add(0.0f, s.f);
    c.fing.add(0.15f, s.f);
    c.fing.add(0.90f, rest.f);
    c.pen.keys.clear();
    c.pen.add(0.0f, tp.pen);
    c.pen.add(0.10f, tp.pen);
    c.pen.add(0.85f, rest.pen);
    mo.segs.push_back(c);
    h.motion = mo;
    wr.events.push_back({start + tA, EventType::PenPicked, WActPick, false});
}

void Animator::Impl::penPutSegments(const mat4& Fc, const HandSample& from, float T, Motion& mo, FingerPose* openOut) {
    const TablePinch tp = tablePinch(Fc, 0.8f);
    const float tableC = layout::TABLE_TOP_Y - pelvisWorld.y;
    Segment a = makeSeg(from, T, tp.wrist, vec3(0), tp.R, tp.closed);
    a.he = 0.72f;
    a.arcH = 0.02f;
    a.arcPeak = 0.40f;
    a.rot.keys.clear();
    a.rot.add(0.0f, from.q);
    a.rot.add(0.72f, tp.R);
    a.fing.keys.clear();
    a.fing.add(0.0f, from.f);
    a.fing.add(0.12f, from.f);
    a.fing.add(0.70f, tp.closed);
    a.pen.keys.clear();
    a.pen.add(0.0f, from.pen);
    a.pen.add(0.10f, from.pen);
    a.pen.add(0.70f, tp.pen);
    // Set down exactly on its spot.
    a.lockC = frameOf(Fc);
    a.lockU0 = 0.72f;
    a.lockFrom = 0.0f;
    a.lockTo = 1.0f;
    clearPath(a, from, from.f, tableC, Side::Left);
    mo.segs.push_back(a);
    if (openOut) *openOut = tp.open;
}

void Animator::Impl::planPutPen(const WriteTask& t, float start, float T) {
    Hand& h = left();
    HandSample from = h.motion.sample(start);
    const mat4 Fc = toCharM(t.frame);
    const float sc = T / Timing::PutPen;
    const float tA = 0.34f * sc;
    Motion mo;
    mo.start = start;
    FingerPose open;
    penPutSegments(Fc, from, tA, mo, &open);
    HandSample s = mo.segs.back().sample(tA);
    // Let go, lift off the pen and back to the resting spot.
    const HandSample& r = h.rest;
    Segment b = makeSeg(s, T - tA, r.p, vec3(0), r.q, r.f);
    b.hs = 0.22f;
    b.arcH = 0.025f;
    b.arcPeak = 0.35f;
    b.fing.keys.clear();
    b.fing.add(0.0f, s.f);
    b.fing.add(0.25f, open);
    b.fing.add(1.0f, r.f);
    mo.segs.push_back(b);
    h.motion = mo;
    wr.events.push_back({start + tA, EventType::PenPut, WActPut, false});
}

void Animator::Impl::planWrite(const WriteTask& t, float start, float T) {
    Hand& h = left();
    HandSample from = h.motion.sample(start);
    // Path in character space.
    auto data = std::make_shared<std::vector<PenKey>>(t.path);
    std::vector<PenKey>& path = *data;
    for (PenKey& k : path) k.tip = toChar(k.tip);
    // Nothing to write: the tip goes just over the writing rest and back (the events still fire).
    const bool noPath = path.empty();
    if (noPath) path.push_back({0.0f, wr.rest + vec3(0.0f, 0.004f, 0.0f), false});
    wr.path = path;
    const float P = std::max(0.0f, path.back().t);
    const float tA = Timing::WriteApproach, tR = std::max(1e-3f, T - tA - P);
    wr.pathStart = start + tA;
    wr.pathEnd = start + tA + P;
    float paper = 1e9f;
    for (const PenKey& k : path)
        if (k.down) paper = std::min(paper, k.tip.y);
    if (paper > 1e8f && !noPath)
        for (const PenKey& k : path) paper = std::min(paper, k.tip.y);
    if (paper > 1e8f) paper = paperY;
    paperY = paper;
    // Anchor: the tip low-passed (Gaussian, sigma 0.2 s), on the paper, sampled every 1/120 s.
    auto anchor = std::make_shared<std::vector<vec3>>();
    const float step = 1.0f / 120.0f, sigma = 0.20f;
    const int n = int(std::ceil(P / step)) + 1;
    {
        std::vector<vec3> tips;
        const int ext = int(std::ceil(3.0f * sigma / step));
        for (int i = -ext; i < n + ext; ++i) tips.push_back(penPathPoint(path, clamp(float(i) * step, 0.0f, P)));
        // The kernel weights and their sum are the same for every sample.
        std::vector<float> wts;
        float wsum = 0.0f;
        for (int j = -ext; j <= ext; ++j) {
            float tt = float(j) * step, wj = std::exp(-0.5f * tt * tt / (sigma * sigma));
            wts.push_back(wj);
            wsum += wj;
        }
        for (int i = 0; i < n; ++i) {
            vec3 acc(0);
            for (int j = -ext; j <= ext; ++j) acc += tips[size_t(i + j + ext)] * wts[size_t(j + ext)];
            acc /= wsum;
            acc.y = paper;
            anchor->push_back(acc);
        }
    }
    const Impl* self = this;
    vec3 mean(0);
    for (const vec3& a : *anchor) mean += a;
    const float yaw = choosePenYaw(mean / float(std::max<size_t>(1, anchor->size())));
    auto poseAt = [self, data, anchor, step, yaw](float tt) {
        const std::vector<vec3>& an = *anchor;
        float fi = clamp(tt / step, 0.0f, float(an.size() - 1));
        size_t i0 = size_t(fi), i1 = std::min(an.size() - 1, i0 + 1);
        vec3 a = lerp(an[i0], an[i1], fi - float(i0));
        return self->penHandPose(a, penPathPoint(*data, tt), yaw);
    };
    auto follow = [poseAt, P](float tt) {
        const float hstep = 1.0f / 240.0f;
        HandSample s = poseAt(tt);
        vec3 pa = poseAt(std::max(0.0f, tt - hstep)).p, pb = poseAt(std::min(P, tt + hstep)).p;
        float span = std::min(P, tt + hstep) - std::max(0.0f, tt - hstep);
        if (span > 1e-5f) {
            s.v = (pb - pa) / span;
            s.a = (pb - s.p * 2.0f + pa) / (hstep * hstep) * (span > 1.9f * hstep ? 1.0f : 0.0f);
        }
        s.tipLock = true;
        return s;
    };
    const HandSample s0 = follow(0.0f), s1 = follow(P);
    Motion mo;
    mo.start = start;
    // Approach: onto the first key.
    Segment a = makeSeg(from, tA, s0.p, s0.v, s0.q, s0.f);
    const float dist = length(vec3(s0.p.x - from.p.x, 0, s0.p.z - from.p.z));
    a.arcH = 0.012f + 0.03f * smoothstep(0.04f, 0.20f, dist);
    a.arcPeak = 0.45f;
    a.rot.keys.clear();
    a.rot.add(0.0f, from.q);
    a.rot.add(0.85f, s0.q);
    a.fing.keys.clear();
    a.fing.add(0.0f, from.f);
    a.fing.add(0.85f, s0.f);
    a.pen.keys.clear();
    a.pen.add(0.0f, from.pen);
    a.pen.add(0.85f, s0.pen);
    mo.segs.push_back(a);
    // The path.
    Segment b;
    b.T = std::max(1e-4f, P);
    b.follow = follow;
    mo.segs.push_back(b);
    // Back to the writing rest, the tip leaving the paper first.
    const HandSample rest = writingRestSample();
    Segment c = makeSeg(s1, tR, rest.p, vec3(0), rest.q, rest.f);
    c.hs = 0.10f;
    c.arcH = 0.008f;
    c.arcPeak = 0.30f;
    c.pen.keys.clear();
    c.pen.add(0.0f, s1.pen);
    c.pen.add(1.0f, rest.pen);
    mo.segs.push_back(c);
    h.motion = mo;
    // Events at the exact key times.
    for (size_t i = 0; i < path.size(); ++i) {
        const bool downNow = i + 1 < path.size() && path[i].down, downBefore = i > 0 && path[i - 1].down;
        if (downNow && !downBefore) wr.events.push_back({wr.pathStart + path[i].t, EventType::PenDown, WActDown, false});
        if (!downNow && downBefore) wr.events.push_back({wr.pathStart + path[i].t, EventType::PenUp, WActUp, false});
    }
    wr.events.push_back({wr.pathEnd, EventType::WritingDone, WActDone, false});
}

void Animator::Impl::planTurnPage(const WriteTask& t, float start, float T) {
    Hand& h = left();
    HandSample from = h.motion.sample(start);
    if (t.pageCorner) {
        wr.corner = t.pageCorner;
    } else {
        // No page geometry given: a pad at the layout position, bound along its top edge. The outer
        // corner of the bottom edge turns about the binding with a slight lift.
        const float top = layout::TABLE_TOP_Y + layout::SCORESHEET_THICKNESS - pelvisWorld.y;
        const float L = layout::SCORESHEET_LENGTH, x = layout::SCORESHEET_X + layout::SCORESHEET_WIDTH * 0.5f;
        const float bindZ = layout::PLAYER_PELVIS_Z - layout::SCORESHEET_Z + L * 0.5f;   // towards the board
        const mat4 rt = root;
        wr.corner = [=](float s) {
            const float a = PI * s, lift = 0.012f * std::sin(a);
            return transformPoint(rt, vec3(x, top + (L + lift) * std::sin(a) + 0.0006f, bindZ - (L + lift) * std::cos(a)));
        };
    }
    // (A copy: the motion outlives wr.corner when a handshake interrupts the page turn.)
    const std::function<vec3(float)> cornerW = wr.corner;
    auto corner = [this, cornerW](float s) { return toChar(cornerW(clamp(s, 0.0f, 1.0f))); };
    wr.turnStart = start;
    wr.turnT = T;
    // Binding line: the circle through three corner positions.
    const vec3 c0 = corner(0.0f), c5 = corner(0.5f), c1 = corner(1.0f);
    vec3 axis = safeNormalize(cross(c5 - c0, c1 - c0), vec3(1, 0, 0));
    vec3 O;
    {
        vec3 a = c0 - c1, b = c5 - c1, axb = cross(a, b);
        float d = 2.0f * length2(axb);
        O = d > 1e-12f ? c1 + cross(b * length2(a) - a * length2(b), axb) / d : (c0 + c1) * 0.5f;
    }
    if (dot(cross(c0 - O, c5 - O), axis) < 0.0f) axis = -axis;   // positive angle turns the page over
    // Pinch at the corner: the thumb under the page from beyond its edge, the index on top, the
    // pinch axis tilted from the page normal towards the inside of the page. The hand keeps that
    // pinch while the page turns; its rotation about the pinch axis is free and chosen (dynamic
    // programming over a few flip positions) so the arm stays comfortable and the hand turns
    // smoothly. 'outAxis': along the binding, away from the body's middle (the corner is on the
    // outer edge).
    const vec3 outAxis = axis * (axis.x * c0.x >= 0.0f ? 1.0f : -1.0f);
    const float sRel = kTurnReleaseS;
    auto pinchAxisAt = [=](float s, vec3* inside) {
        vec3 r = perp(corner(s) - O, axis);
        vec3 n = safeNormalize(cross(axis, r), kY);                            // front side of the page
        vec3 d = safeNormalize(perp(-outAxis, n) - safeNormalize(r, n) * 0.8f, -outAxis);   // into the page
        if (inside) *inside = d;
        const float gam = 0.50f - 0.25f * smoothstep(0.0f, sRel, s);
        return normalize(n * std::cos(gam) + d * std::sin(gam));
    };
    auto rotAt = [&](float s, float psi) {
        vec3 m = pinchAxisAt(s, nullptr);
        return normalize(axisAngle(m, psi) * fromTo(grip.pageAxis, m));
    };
    constexpr int kS = 9, kPsi = 72;
    float psiAt[kS];
    {
        float cost[kS][kPsi];
        int from[kS][kPsi];
        for (int k = 0; k < kS; ++k) {
            const float s = sRel * float(k) / float(kS - 1);
            vec3 inside;
            pinchAxisAt(s, &inside);
            for (int j = 0; j < kPsi; ++j) {
                const float psi = TAU * float(j) / float(kPsi);
                const quat R = rotAt(s, psi);
                const vec3 fingers = rotate(R, vec3(0, -1, 0));
                float c = armStrainSide(Side::Left, corner(s) - rotate(R, grip.pagePinch), R) * 4.0f + 0.6f * (1.0f - dot(fingers, inside));
                float bestPrev = 0.0f;
                from[k][j] = j;
                if (k > 0) {
                    bestPrev = 1e9f;
                    for (int i = 0; i < kPsi; ++i) {
                        float dj = std::fabs(wrapPi(TAU * float(j - i) / float(kPsi)));
                        float pc = cost[k - 1][i] + 1.5f * dj * dj;
                        if (pc < bestPrev) {
                            bestPrev = pc;
                            from[k][j] = i;
                        }
                    }
                }
                cost[k][j] = c + bestPrev;
            }
        }
        int j = 0;
        for (int i = 1; i < kPsi; ++i)
            if (cost[kS - 1][i] < cost[kS - 1][j]) j = i;
        for (int k = kS - 1; k >= 0; --k) {
            psiAt[k] = TAU * float(j) / float(kPsi);
            j = from[k][j];
        }
        for (int k = 1; k < kS; ++k) psiAt[k] = psiAt[k - 1] + wrapPi(psiAt[k] - psiAt[k - 1]);   // unwrapped
    }
    std::vector<float> psis(psiAt, psiAt + kS);
    const vec3 pinch = grip.pagePinch, pageAxis = grip.pageAxis;
    auto handAt = [=](float s) {
        // Smooth rotation about the pinch axis through the chosen samples (Catmull-Rom).
        float x = clamp(s / sRel, 0.0f, 1.0f) * float(kS - 1);
        int i = std::min(kS - 2, int(x));
        float u = x - float(i);
        float p0 = psis[size_t(std::max(0, i - 1))], p1 = psis[size_t(i)], p2 = psis[size_t(i + 1)], p3 = psis[size_t(std::min(kS - 1, i + 2))];
        float psi = 0.5f * (2.0f * p1 + (p2 - p0) * u + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u * u + (3.0f * p1 - p0 - 3.0f * p2 + p3) * u * u * u);
        const vec3 m = pinchAxisAt(s, nullptr);
        HandSample hs;
        hs.q = normalize(axisAngle(m, psi) * fromTo(pageAxis, m));
        hs.p = corner(s) - rotate(hs.q, pinch);
        return hs;
    };
    if (debugLog) {
        for (int k = 0; k < kS; ++k) {
            float s = sRel * float(k) / float(kS - 1);
            HandSample hs = handAt(s);
            LOGI("anim: page turn s=%.2f psi %.0f deg strain %.3f", s, psis[size_t(k)] / DEG, armStrainSide(Side::Left, hs.p, hs.q));
        }
    }
    const float tG = kTurnGrip * T, tR = kTurnRelease * T;
    const float tPre = std::min(std::max(0.05f, tG - 0.12f * T / Timing::PageTurn), tG - 1e-3f);   // (a very short turn)
    const HandSample g0 = handAt(0.0f);
    Motion mo;
    mo.start = start;
    // Tuck the pen, go over the corner, thumb and index open.
    const vec3 pre = g0.p + kY * 0.014f;
    Segment a = makeSeg(from, tPre, pre, vec3(0, -0.06f, 0), g0.q, grip.tuckOpen);
    a.arcH = 0.02f;
    a.arcPeak = 0.40f;
    a.rot.keys.clear();
    a.rot.add(0.0f, from.q);
    a.rot.add(0.85f, g0.q);
    a.fing.keys.clear();
    a.fing.add(0.0f, from.f);
    a.fing.add(0.70f, grip.tuckOpen);
    a.pen.keys.clear();
    a.pen.add(0.0f, from.pen);
    a.pen.add(0.10f, from.pen);
    a.pen.add(0.75f, grip.tucked);
    mo.segs.push_back(a);
    HandSample s = a.sample(tPre);
    // Pinch.
    Segment b = makeSeg(s, tG - tPre, g0.p, vec3(0), g0.q, grip.tuckPinch);
    b.fing.keys.clear();
    b.fing.add(0.0f, s.f);
    b.fing.add(0.35f, s.f);
    b.fing.add(1.0f, grip.tuckPinch);
    b.pen.keys.clear();
    b.pen.add(0.0f, grip.tucked);
    // From the pinch until the page is let go, the pinch point is locked on the corner.
    b.pinLocal = grip.pagePinch;
    b.pinU0 = 0.4f;
    b.pinTo = 1.0f;
    mo.segs.push_back(b);
    // Lift the page past the vertical, following the corner.
    const float Tf = tR - tG;
    auto poseC = [=](float tt) {
        float s1 = pageTurnEase((tG + tt) / T);
        return handAt(s1);
    };
    Segment c;
    c.T = Tf;
    const FingerPose fp = grip.tuckPinch;
    const PenPose tk = grip.tucked;
    c.follow = [poseC, fp, tk, Tf, pinch](float tt) {
        const float hs = 1.0f / 240.0f;
        HandSample r = poseC(tt);
        r.pinW = 1.0f;
        r.pinLocal = pinch;
        vec3 pa = poseC(std::max(0.0f, tt - hs)).p, pb = poseC(std::min(Tf, tt + hs)).p;
        float span = std::min(Tf, tt + hs) - std::max(0.0f, tt - hs);
        if (span > 1e-5f) r.v = (pb - pa) / span;
        r.f = fp;
        r.pen = tk;
        return r;
    };
    mo.segs.push_back(c);
    s = c.sample(Tf);
    // Let go and back to the writing rest, the pen back into the writing grip.
    const HandSample rest = writingRestSample();
    Segment d = makeSeg(s, T - tR, rest.p, vec3(0), rest.q, rest.f);
    d.rot.keys.clear();
    d.rot.add(0.0f, s.q);
    d.rot.add(0.80f, rest.q);
    d.fing.keys.clear();
    d.fing.add(0.0f, s.f);
    d.fing.add(0.18f, grip.tuckOpen);
    d.fing.add(0.45f, grip.tuckOpen);
    d.fing.add(1.0f, rest.f);
    d.pen.keys.clear();
    d.pen.add(0.0f, grip.tucked);
    d.pen.add(0.35f, grip.tucked);
    d.pen.add(0.95f, rest.pen);
    d.pinLocal = grip.pagePinch;
    d.pinFrom = 1.0f;
    d.pinU1 = 0.35f;
    mo.segs.push_back(d);
    h.motion = mo;
    wr.events.push_back({start + tG, EventType::PageGripped, WActGripPage, false});
    wr.events.push_back({start + kTurnLand * T, EventType::PageTurned, WActTurned, false});
}

// =============================================================================================
// Writing task machine
// =============================================================================================
bool Animator::Impl::writingHandFree() const {
    return !wr.running && wr.queue.empty() && !wr.penHeld && time >= wr.suspendUntil - 1e-6f &&
           !(mirrored && running && cur.type == TaskType::Handshake);
}

bool Animator::Impl::nextWriteBoundary(float& t) const {
    if (wr.running) {
        t = wr.start + wr.T;
        return true;
    }
    if (wr.queue.empty()) return false;
    t = std::max(time, wr.suspendUntil);
    return true;
}

void Animator::Impl::stepWriting(std::vector<Event>& ev) {
    if (wr.running) {
        finishWriteTask(ev);
        return;
    }
    WriteTask t = wr.queue.front();
    wr.queue.pop_front();
    startWriteTask(t);
}

void Animator::Impl::startWriteTask(const WriteTask& t) {
    Hand& h = left();
    bakeFollow(h);
    leftChin = 0;
    wr.cur = t;
    wr.running = true;
    wr.start = time;
    wr.T = writeTaskDuration(t);
    wr.events.clear();
    wr.pathStart = wr.pathEnd = -1.0f;
    wr.turnStart = -1.0f;
    wr.path.clear();
    wr.corner = nullptr;
    auto hold = [&]() {
        HandSample s = h.motion.sample(time);
        Motion mo;
        mo.start = time;
        mo.segs.push_back(makeSeg(s, std::max(wr.T, 1e-3f), s.p + s.v * 0.02f, vec3(0), s.q, s.f));
        h.motion = mo;
    };
    switch (t.type) {
        case WriteTaskType::PickPen:
            if (wr.penHeld) hold();
            else planPickPen(t, time, wr.T);
            break;
        case WriteTaskType::PutPen:
            if (!wr.penHeld) hold();
            else planPutPen(t, time, wr.T);
            break;
        case WriteTaskType::Write: planWrite(t, time, wr.T); break;
        case WriteTaskType::TurnPage: planTurnPage(t, time, wr.T); break;
        case WriteTaskType::Wait: hold(); break;
    }
    if (debugLog) LOGI("anim: writing task %d (%.3f s) at t=%.3f", int(t.type), wr.T, time);
}

void Animator::Impl::fireWriteDue(float upTo, std::vector<Event>& ev) {
    for (auto& e : wr.events) {
        if (e.done || e.t > upTo + 1e-6f) continue;
        e.done = true;
        Event out;
        out.type = e.type;
        out.time = e.t;
        switch (e.action) {
            case WActPick:
                wr.penHeld = true;
                wr.penTable = wr.cur.frame;   // a handshake that needs the hand puts it back there
                out.transform = wr.cur.frame;   // it leaves the table exactly from there
                out.position = wr.cur.frame.translation();
                break;
            case WActPut:
                wr.penHeld = false;
                wr.penTable = wr.cur.frame;
                out.transform = wr.cur.frame;
                out.position = wr.cur.frame.translation();
                break;
            case WActDown:
            case WActUp:
            case WActDone:
                out.position = toWorld(penPathPoint(wr.path, e.t - wr.pathStart));
                break;
            case WActGripPage:
            case WActTurned:
                if (wr.corner) out.position = wr.corner(e.action == WActTurned ? 1.0f : 0.0f);
                break;
            default: break;
        }
        ev.push_back(out);
    }
}

void Animator::Impl::finishWriteTask(std::vector<Event>& ev) {
    fireWriteDue(wr.start + wr.T, ev);
    wr.running = false;
    wr.pathStart = wr.pathEnd = -1.0f;
    wr.turnStart = -1.0f;
    wr.corner = nullptr;
    if (wr.queue.empty()) {
        Event e;
        e.type = EventType::WritingQueueEmpty;
        e.time = time;
        ev.push_back(e);
    }
}

// A handshake needs the writing hand (left-handed player): the running writing task stops here.
// The events already due fire first, at their own instants and with their effect (the handshake
// may start mid-frame); the remaining ones fire now (a path is cut where it is, a turning page is
// reported turned), except the pen's own: a pen still on the table stays there, a pen still in the
// hand is laid down by the handshake itself. Queued tasks wait for the end of the handshake.
void Animator::Impl::interruptWriting(std::vector<Event>& ev) {
    if (!wr.running) return;
    fireWriteDue(time, ev);
    for (auto& e : wr.events) {
        if (e.done) continue;
        if (e.action == WActPick || e.action == WActPut) {
            e.done = true;
            continue;
        }
        e.t = time;
    }
    fireWriteDue(time, ev);
    if (debugLog) LOGI("anim: writing task %d interrupted by a handshake at t=%.3f", int(wr.cur.type), time);
    wr.running = false;
    wr.pathStart = wr.pathEnd = -1.0f;
    wr.turnStart = -1.0f;
    wr.corner = nullptr;
}

// The writing hand at work: the body leans and turns a little towards the sheet, and leans further
// when the writing hand would otherwise be out of comfortable reach.
void Animator::Impl::writingSpine(SpineParams& sp, const HandSample& hl) {
    const float w = wr.lean;
    if (w > 1e-3f) {
        sp.flex += 0.045f * w;
        sp.twist += 0.06f * w;
    }
    if (!wr.running && !wr.penHeld && !(mirrored && running && cur.type == TaskType::Handshake)) return;
    Pose tmp;
    const float comfy = 0.84f * (L1 + L2);
    for (int it = 0; it < 3; ++it) {
        float D = length(hl.p - shoulderFor(tmp, Side::Left, sp, hl.p));
        if (D <= comfy) break;
        sp.flex = std::min(0.60f, sp.flex + (D - comfy) * 1.6f);
    }
}

// =============================================================================================
// Public API
// =============================================================================================
character::Side Animator::playHand() const { return impl_ && impl_->mirrored ? Side::Left : Side::Right; }
character::Side Animator::writingHand() const { return impl_ && impl_->mirrored ? Side::Right : Side::Left; }

void Animator::setWritingRest(vec3 worldPos) {
    Impl& I = *impl_;
    if (!I.sk) return;
    I.wr.rest = I.toChar(I.mw(worldPos));
    I.wr.restYaw = I.choosePenYaw(I.wr.rest);
    I.paperY = I.wr.rest.y;
    if (I.wr.penHeld && !I.wr.running && I.wr.queue.empty() && I.time >= I.wr.suspendUntil) {
        Impl::Hand& h = I.left();
        HandSample from = h.motion.sample(I.time);
        HandSample r = I.writingRestSample();
        Motion mo;
        mo.start = I.time;
        Segment sg = I.makeSeg(from, 0.5f, r.p, vec3(0), r.q, r.f);
        sg.arcH = 0.01f;
        sg.pen.keys.clear();
        sg.pen.add(0.0f, from.pen);
        sg.pen.add(1.0f, r.pen);
        mo.segs.push_back(sg);
        h.motion = mo;
    }
}

void Animator::enqueueWriting(const WriteTask& t) {
    Impl& I = *impl_;
    WriteTask c = t;   // into the solver's world
    c.frame = I.mm(t.frame);
    for (PenKey& k : c.path) k.tip = I.mw(k.tip);
    if (t.pageCorner && I.mirrored) {
        auto f = t.pageCorner;
        c.pageCorner = [f](float s) {
            vec3 p = f(s);
            return vec3(-p.x, p.y, p.z);
        };
    }
    I.wr.queue.push_back(c);
}
void Animator::enqueueWriting(const std::vector<WriteTask>& tasks) {
    for (auto& t : tasks) enqueueWriting(t);
}
bool Animator::writingBusy() const { return impl_->wr.running || !impl_->wr.queue.empty(); }
void Animator::clearWritingQueue() { impl_->wr.queue.clear(); }
float Animator::writingRemainingTime() const {
    const Impl& I = *impl_;
    float r = I.wr.running ? std::max(0.0f, I.wr.start + I.wr.T - I.time) : 0.0f;
    for (auto& t : I.wr.queue) r += writeTaskDuration(t);
    return r;
}

float Animator::writingPathTime() const {
    const Impl& I = *impl_;
    if (!I.wr.running || I.wr.cur.type != WriteTaskType::Write || I.wr.pathStart < 0.0f) return -1.0f;
    if (I.time < I.wr.pathStart - 1e-6f || I.time > I.wr.pathEnd + 1e-6f) return -1.0f;
    return clamp(I.time - I.wr.pathStart, 0.0f, I.wr.pathEnd - I.wr.pathStart);
}

float Animator::pageTurnProgress() const {
    const Impl& I = *impl_;
    if (!I.wr.running || I.wr.cur.type != WriteTaskType::TurnPage || I.wr.turnStart < 0.0f) return -1.0f;
    return pageTurnEase(clamp((I.time - I.wr.turnStart) / std::max(1e-4f, I.wr.turnT), 0.0f, 1.0f));
}

bool Animator::penTransform(mat4& out) const {
    const Impl& I = *impl_;
    if (!I.sk || !I.wr.penHeld) return false;
    out = I.mm(I.worldI[HandL] * toMat4(I.evalPen.q, I.evalPen.p));
    return true;
}
bool Animator::holdsPen() const { return impl_->wr.penHeld; }

}  // namespace anim
