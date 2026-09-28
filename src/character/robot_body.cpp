// Arms, torso and legs of the robot (right side modelled in its bone spaces, left side mirrored).
//
// Joint scheme: every articulation is a dark ball (RobotJoint) centred on the child bone's pivot.
// Both porcelain shells end in concave sockets around it with a small gap; the flexion side of
// each rim is bevelled so hinge-like joints (elbow, knee, ankle) can fold without the shells
// meeting. Hips are the exception: the pelvis carries a convex porcelain hip cover and the thigh a
// concave cup around it (ball-jointed doll style), so the hip never opens in any direction.
#include "robot_build.h"

using namespace m;

namespace character {
namespace build {
namespace {

constexpr float kGap = 0.0010f;  // clearance between a shell and the ball it wraps

// Joint radii shared by the parts that meet at a joint.
constexpr float kShoulderBall = 0.0300f;
constexpr float kElbowBall = 0.0255f;
constexpr float kWristDome = 0.0205f;  // carpal dome of the palm (robot_hand.cpp)
constexpr float kHipCover = 0.0600f;   // convex hip cover of the pelvis around the hip pivot
constexpr float kKneeBall = 0.0340f;
constexpr float kKneeCap = kKneeBall + kGap + 0.0100f;  // outer radius of the knee cap shell
constexpr float kAnkleBall = 0.0255f;
constexpr float kNeckR = 0.0440f;

sdf::VolumeOptions vol(const char* name, vec3 axis, float cell = 0.0015f, float err = 0.00010f, float maxEdge = 0.010f) {
    sdf::VolumeOptions o;
    o.cell = cell;
    o.maxError = err;
    o.maxEdge = maxEdge;
    o.tangentAxis = axis;
    o.name = name;
    return o;
}

MeshJob ballJob(float r, vec3 c = vec3(0)) {
    return [=] {
        MeshData m = prim::sphere(r, 28, 14);
        m.transform(translate(c));
        return m;
    };
}

// Bevelled rim helpers (see robot_hand.cpp for the same construction on the fingers).
//  'u' = coordinate along the bone measured from a pivot, 's' = coordinate towards the flexion side.
//  shellStart: keep u > start, bevelled so the flexion side starts later.
inline float startCut(float u, float s, float start, float bevel) { return s * std::sin(bevel) - (u - start) * std::cos(bevel); }
//  shellEnd: keep v > -lip (v = distance before the next pivot), flexion side ends earlier.
inline float endCut(float v, float s, float lip, float bevel) { return s * std::sin(bevel) - (v + lip) * std::cos(bevel); }

// ---- arm -----------------------------------------------------------------------------------------
float upperArmShell(const vec3& p) {
    const vec3 elbow(0, -0.300f, 0);
    vec3 q(p.x * 1.05f, p.y, p.z);
    float d = sdf::roundCone(q, vec3(0, -0.060f, 0.0f), vec3(0, -0.272f, 0.001f), 0.0370f, 0.0305f);
    // Biceps (front) and triceps (back) swell a little.
    d = sdf::smin(d, sdf::ellipsoid(p - vec3(0.001f, -0.165f, 0.009f), vec3(0.029f, 0.080f, 0.029f)), 0.02f);
    d = sdf::smin(d, sdf::ellipsoid(p - vec3(0.000f, -0.130f, -0.010f), vec3(0.030f, 0.085f, 0.030f)), 0.02f);
    // Deltoid cap over the shoulder ball.
    float delt = sdf::ellipsoid(p - vec3(-0.005f, -0.020f, 0.0f), vec3(0.047f, 0.066f, 0.049f));
    d = sdf::smin(d, delt, 0.02f);
    // The body side of the cap stays clear of the chest; the ball shows in the armpit.
    float inner = p.x - 0.024f - std::max(0.0f, -p.y - 0.045f) * 0.9f;
    d = sdf::smax(d, inner, 0.010f);
    d = sdf::smax(d, (kShoulderBall + kGap) - length(p), 0.002f);
    // Elbow: socket around the ball, lips just past the pivot, front (flexion side) bevelled.
    vec3 e = p - elbow;
    d = sdf::smax(d, (kElbowBall + kGap) - length(e), 0.0025f);
    d = sdf::smax(d, endCut(e.y, e.z, 0.003f, 42.0f * DEG), 0.003f);
    return d;
}

float forearmShell(const vec3& p) {
    const vec3 wrist(0, -0.265f, 0);
    // Flatter towards the wrist (thin palm-to-back, wide thumb-to-pinky).
    float t = clamp(-p.y / 0.265f, 0.0f, 1.0f);
    float sx = lerp(0.97f, 0.90f, t);
    vec3 q(p.x / sx, p.y, p.z);
    float d = sdf::roundCone(q, vec3(0, -0.040f, 0), vec3(0, -0.252f, 0), 0.0330f, 0.0256f) * sx;
    // Flexor / brachioradialis mass near the elbow.
    d = sdf::smin(d, sdf::ellipsoid(p - vec3(0.002f, -0.085f, 0.004f), vec3(0.029f, 0.075f, 0.033f)), 0.02f);
    // Elbow end: socket, shell starts past the pivot, front bevelled.
    d = sdf::smax(d, (kElbowBall + kGap) - length(p), 0.0025f);
    d = sdf::smax(d, startCut(-p.y, p.z, 0.005f, 42.0f * DEG), 0.003f);
    // Wrist end: socket wrapping the carpal dome of the hand, lips just past the pivot.
    vec3 w = p - wrist;
    d = sdf::smin(d, sdf::sphere(w, kWristDome + 0.0045f), 0.010f);  // cuff: the wall never gets thin
    d = sdf::smax(d, (kWristDome + 0.0005f) - length(w), 0.0020f);
    d = sdf::smax(d, -w.y - 0.0025f, 0.0025f);
    return d;
}

// ---- legs ----------------------------------------------------------------------------------------
float thighShell(const vec3& p) {
    const vec3 knee(0, 0, 0.44f);
    vec3 q(p.x, p.y * 1.10f, p.z);
    float d = sdf::roundCone(q, vec3(0, 0.004f, 0.030f), vec3(0, 0.002f, 0.418f), 0.0720f, 0.0520f);
    // Quadriceps swell on top, flatter inner thigh, flattened where it rests on the seat.
    d = sdf::smin(d, sdf::ellipsoid(p - vec3(-0.004f, 0.018f, 0.20f), vec3(0.052f, 0.040f, 0.15f)), 0.03f);
    d = sdf::smax(d, p.x - 0.058f, 0.03f);
    d = sdf::smax(d, -(p.y + 0.066f), 0.03f);
    // Hip: concave cup around the pelvis' hip cover.
    d = sdf::smax(d, (kHipCover + kGap) - length(p), 0.003f);
    // Knee: a sleeve over the shin's knee cap, lips a little past the pivot, underside bevelled.
    vec3 k = p - knee;
    d = sdf::smin(d, sdf::sphere(k, kKneeCap + kGap + 0.0050f), 0.016f);  // cuff: the wall never gets thin
    d = sdf::smax(d, (kKneeCap + kGap) - length(k), 0.0025f);
    d = sdf::smax(d, endCut(-k.z, -k.y, 0.004f, 40.0f * DEG), 0.0035f);
    return d;
}

float shinShell(const vec3& p) {
    const vec3 ankle(0, -0.44f, 0);
    vec3 q(p.x * 1.10f, p.y, p.z);
    float d = sdf::roundCone(q, vec3(0, -0.050f, 0.004f), vec3(0, -0.405f, 0.0f), 0.0440f, 0.0290f);
    // Calf at the back, subtle tibia crest at the front.
    d = sdf::smin(d, sdf::ellipsoid(p - vec3(0.002f, -0.150f, -0.016f), vec3(0.040f, 0.105f, 0.045f)), 0.03f);
    d = sdf::smin(d, sdf::capsule(p, vec3(0, -0.07f, 0.030f), vec3(0, -0.36f, 0.018f), 0.012f), 0.025f);
    // Knee: the tube starts below the pivot (back bevelled for flexion); a knee cap shell over the
    // front of the ball slides under the thigh's sleeve. The sides of the ball stay visible.
    d = sdf::smax(d, startCut(-p.y, -p.z, 0.006f, 35.0f * DEG), 0.003f);
    float r = length(p);
    float cap = std::max(r - kKneeCap, (kKneeBall + kGap) - r);
    cap = sdf::smax(cap, -p.z - 0.010f, 0.003f);
    cap = sdf::smax(cap, std::fabs(p.x) - 0.029f, 0.004f);
    d = sdf::smin(d, cap, 0.006f);
    d = sdf::smax(d, (kKneeBall + kGap) - r, 0.0015f);
    // Ankle: socket, lips just past the pivot, front and back bevelled (foot flexes both ways).
    vec3 a = p - ankle;
    d = sdf::smin(d, sdf::sphere(a, kAnkleBall + kGap + 0.0045f), 0.012f);  // cuff: the wall never gets thin
    d = sdf::smax(d, (kAnkleBall + kGap) - length(a), 0.0025f);
    d = sdf::smax(d, endCut(a.y, std::fabs(a.z), 0.002f, 30.0f * DEG), 0.0035f);
    return d;
}

float footShell(const vec3& p) {
    // Sole on the floor (y = -0.09 in foot space), heel behind the ankle, toes forward.
    float heel = sdf::ellipsoid(p - vec3(0, -0.058f, -0.018f), vec3(0.031f, 0.036f, 0.045f));
    vec3 q(p.x * 0.80f, p.y, p.z);
    float mid = sdf::roundCone(q, vec3(0, -0.050f, 0.000f), vec3(0.002f, -0.070f, 0.150f), 0.030f, 0.020f);
    float toes = sdf::ellipsoid(p - vec3(0.004f, -0.071f, 0.146f), vec3(0.043f, 0.021f, 0.050f));
    float d = sdf::smin(sdf::smin(heel, mid, 0.03f), toes, 0.03f);
    d = sdf::smax(d, -(p.y + 0.090f), 0.004f);
    // Ankle: the top of the foot wraps the ball from below, front and back bevelled.
    d = sdf::smax(d, (kAnkleBall + kGap) - length(p), 0.0025f);
    d = sdf::smax(d, startCut(-p.y, std::fabs(p.z), 0.004f, 30.0f * DEG), 0.003f);
    return d;
}

// ---- torso ---------------------------------------------------------------------------------------
float pelvisShell(const vec3& p) {
    float d = sdf::roundBox(p - vec3(0, -0.028f, -0.018f), vec3(0.125f, 0.068f, 0.086f), 0.055f);
    float ax = std::fabs(p.x);
    vec3 pm(ax, p.y, p.z);
    // Buttocks and hip covers (convex spheres the thigh cups turn around).
    d = sdf::smin(d, sdf::ellipsoid(pm - vec3(0.060f, -0.050f, -0.050f), vec3(0.068f, 0.050f, 0.062f)), 0.03f);
    const vec3 hip(0.095f, -0.030f, 0.0f);
    d = sdf::smin(d, sdf::sphere(pm - hip, kHipCover), 0.02f);
    // Seat contact is flat.
    d = sdf::smax(d, -(p.y + 0.098f), 0.02f);  // 2 mm above the seat top: no coplanar contact
    // Waist: taper towards the core, open on top.
    d = sdf::smax(d, p.y - 0.080f, 0.012f);
    // Room for the thighs: remove the thigh tube beyond the hip cover.
    vec3 h = pm - hip;
    float tube = std::max(length(vec2(h.x, h.y * 1.1f)) - 0.078f, -h.z);
    float thighRoom = sdf::smax(tube, (kHipCover + kGap) - length(h), 0.004f);
    d = sdf::smax(d, -thighRoom, 0.004f);
    return d;
}

float waistCore(const vec3& p) {
    // Dark flexible core between pelvis and chest: elliptic bellows.
    float rx = 0.102f, rz = 0.075f;
    float e = length(vec2(p.x / rx, (p.z + 0.004f) / rz));
    float d = (e - 1.0f) * rz;
    d += 0.0016f * (0.5f + 0.5f * std::cos(p.y * 6.2832f / 0.016f));
    d = sdf::smax(d, std::fabs(p.y - 0.060f) - 0.115f, 0.01f);
    return d;
}

float abdomenRing(const vec3& p) {
    // Porcelain ring around the waist core (Spine1), with dark gaps above and below.
    vec3 c = p - vec3(0, 0.035f, -0.004f);
    float outer = (length(vec2(c.x / 0.114f, c.z / 0.087f)) - 1.0f) * 0.087f;
    outer = sdf::smin(outer, sdf::ellipsoid(c - vec3(0, 0, 0.004f), vec3(0.112f, 0.10f, 0.094f)), 0.01f);
    float inner = (length(vec2(c.x / 0.103f, c.z / 0.077f)) - 1.0f) * 0.077f;
    float d = sdf::smax(outer, -inner, 0.002f);
    d = sdf::smax(d, std::fabs(c.y) - 0.038f, 0.006f);
    return d;
}

float chestShell(const vec3& p) {
    float ax = std::fabs(p.x);
    vec3 pm(ax, p.y, p.z);
    float d = sdf::ellipsoid(p - vec3(0, 0.105f, 0.004f), vec3(0.132f, 0.150f, 0.100f));
    // Ribcage base: broad elliptic section down to the waist.
    float base = (length(vec2(p.x / 0.114f, (p.z - 0.002f) / 0.088f)) - 1.0f) * 0.088f;
    base = sdf::smax(base, std::fabs(p.y - 0.040f) - 0.080f, 0.030f);
    d = sdf::smin(d, base, 0.03f);
    // Shoulder girdle, pectoral plates, trapezius slope, scapulae.
    d = sdf::smin(d, sdf::ellipsoid(p - vec3(0, 0.183f, -0.006f), vec3(0.150f, 0.066f, 0.088f)), 0.04f);
    d = sdf::smin(d, sdf::ellipsoid(pm - vec3(0.056f, 0.160f, 0.050f), vec3(0.058f, 0.046f, 0.042f)), 0.03f);
    d = sdf::smin(d, sdf::capsule(pm, vec3(0.036f, 0.252f, -0.014f), vec3(0.145f, 0.226f, -0.012f), 0.028f), 0.03f);
    d = sdf::smin(d, sdf::ellipsoid(pm - vec3(0.068f, 0.150f, -0.058f), vec3(0.058f, 0.072f, 0.040f)), 0.03f);
    // Lower edge: costal arch notch at the front, open underneath over the waist core.
    d = sdf::smax(d, -(p.y + 0.038f), 0.008f);
    d = sdf::smax(d, -sdf::ellipsoid(p - vec3(0, -0.045f, 0.095f), vec3(0.050f, 0.040f, 0.060f)), 0.012f);
    // Shoulder sockets (ball + clearance for the deltoid cap turning with the arm).
    const vec3 sh(0.190f, 0.230f, -0.010f);
    d = sdf::smax(d, (kShoulderBall + kGap) - length(pm - sh), 0.003f);
    float deltRoom = sdf::smax(length(pm - sh) - 0.055f, 0.150f - ax, 0.01f);
    d = sdf::smax(d, -deltRoom, 0.006f);
    // Collar opening around the neck core (wider at the top so the head can nod).
    vec3 n = p - vec3(0, 0.250f, -0.005f);
    float coneR = kNeckR + 0.004f + std::max(0.0f, n.y) * 0.35f;
    float hole = sdf::smax(length(vec2(n.x, n.z)) - coneR, -0.030f - n.y, 0.004f);
    d = sdf::smax(d, -hole, 0.004f);
    return d;
}

float neckCore(const vec3& p) {
    vec3 q(p.x / 1.06f, p.y, p.z);
    float d = sdf::roundCone(q, vec3(0, -0.040f, -0.004f), vec3(0, 0.118f, 0.012f), kNeckR, 0.039f);
    d += 0.0012f * (0.5f + 0.5f * std::cos(p.y * 6.2832f / 0.013f));
    return d;
}

float neckTendons(const vec3& p) {
    vec3 pm(std::fabs(p.x), p.y, p.z);
    float d = sdf::capsule(pm, vec3(0.028f, 0.092f, 0.016f), vec3(0.014f, -0.020f, 0.044f), 0.0060f);
    d = std::min(d, sdf::capsule(pm, vec3(0.026f, 0.092f, -0.028f), vec3(0.028f, -0.022f, -0.032f), 0.0060f));
    return d;
}

}  // namespace

void buildArm(Sink& s) {
    size_t first = s.parts.size();
    {
        PorcelainLook look;
        // Deltoid cap panel line and a seam along the back of the arm.
        look.seams[0] = seamPlane(vec3(0, 1, 0.25f), vec3(0, -0.075f, 0));
        s.addPorcelain("upperarm", UpperArmR, [] {
            return sdf::meshVolume(upperArmShell, {vec3(0, -0.15f, 0)}, vol("upperarm", vec3(0, -1, 0), 0.0018f));
        }, false, look);
        s.addJoint("shoulder", UpperArmR, ballJob(kShoulderBall), false);
    }
    {
        PorcelainLook look;
        look.seams[0] = seamPlane(vec3(0, 1, 0), vec3(0, -0.200f, 0));
        s.addPorcelain("forearm", ForeArmR, [] {
            return sdf::meshVolume(forearmShell, {vec3(0, -0.13f, 0)}, vol("forearm", vec3(0, -1, 0), 0.0016f));
        }, false, look);
        s.addJoint("elbow", ForeArmR, ballJob(kElbowBall), false);
    }
    s.mirrorFrom(first);
}

void buildLegs(Sink& s) {
    size_t first = s.parts.size();
    {
        PorcelainLook look;
        look.seams[0] = seamPlane(vec3(1, 0, 0), vec3(0.030f, 0, 0));  // inner thigh panel
        s.addPorcelain("thigh", ThighR, [] {
            return sdf::meshVolume(thighShell, {vec3(0, 0, 0.22f)}, vol("thigh", vec3(0, 0, 1), 0.0022f, 0.00015f, 0.016f));
        }, false, look);
        s.addJoint("knee", ShinR, ballJob(kKneeBall), false);
    }
    {
        PorcelainLook look;
        look.seams[0] = seamPlane(vec3(0, 0, -1), vec3(0, 0, 0.012f));
        s.addPorcelain("shin", ShinR, [] {
            return sdf::meshVolume(shinShell, {vec3(0, -0.2f, 0)}, vol("shin", vec3(0, -1, 0), 0.0020f, 0.00015f, 0.016f));
        }, false, look);
        s.addJoint("ankle", FootR, ballJob(kAnkleBall), false);
    }
    {
        PorcelainLook look;
        look.seams[0] = seamPlane(vec3(0, 0, 1), vec3(0, 0, 0.118f));  // toe cap
        look.seams[1] = seamPlane(vec3(0, -1, 0), vec3(0, -0.083f, 0));  // sole line
        s.addPorcelain("foot", FootR, [] {
            return sdf::meshVolume(footShell, {vec3(0, -0.06f, 0.05f)}, vol("foot", vec3(0, 0, 1), 0.0016f, 0.00012f, 0.014f));
        }, false, look);
    }
    s.mirrorFrom(first);
}

void buildTorso(Sink& s) {
    {
        PorcelainLook look;
        look.seams[0] = seamPlane(vec3(0, 1, -0.4f), vec3(0, 0.02f, 0.06f));
        s.addPorcelain("pelvis", Pelvis, [] {
            return sdf::meshVolume(pelvisShell, {vec3(0, -0.03f, -0.02f)}, vol("pelvis", vec3(0, 1, 0), 0.0024f, 0.00016f, 0.020f));
        }, false, look);
    }
    s.addJoint("waist", Spine1, [] {
        return sdf::meshVolume(waistCore, {vec3(0, 0.06f, 0)}, vol("waist", vec3(0, 1, 0), 0.0020f, 0.00020f, 0.020f));
    }, false, 1.0f);
    {
        PorcelainLook look;
        look.seams[0] = seamPlane(vec3(1, 0, 0), vec3(0, 0, 0));
        s.addPorcelain("abdomen", Spine1, [] {
            return sdf::meshVolume(abdomenRing, {vec3(0, 0.035f, 0.083f)}, vol("abdomen", vec3(0, 1, 0), 0.0016f, 0.00012f, 0.016f));
        }, false, look);
    }
    {
        PorcelainLook look;
        look.seams[0] = seamPlane(vec3(1, 0, 0), vec3(0, 0, 0));             // sternum line
        look.seams[1] = seamPlane(vec3(0, -1, 0.35f), vec3(0, 0.115f, 0.09f));  // under the pectoral plates
        look.seams[2] = seamPlane(vec3(0, 0, -1), vec3(0, 0, -0.035f));       // front / back shells
        s.addPorcelain("chest", Spine2, [] {
            return sdf::meshVolume(chestShell, {vec3(0, 0.12f, 0)}, vol("chest", vec3(0, 1, 0), 0.0022f, 0.00014f, 0.020f));
        }, false, look);
    }
    // Neck: dark core with tendon cords; the upper part is hidden in first person. The halves
    // meet with rounded ends at the bottom of a ridge groove, where the joint reads as ring segments.
    auto neckLow = [](const vec3& p) {
        // Inner spindle bridging the gap, so the gap is dark rather than see-through.
        float spindle = sdf::capsule(p, vec3(0, 0.028f, 0.004f), vec3(0, 0.036f, 0.005f), 0.030f);
        return std::min(sdf::smax(neckCore(p), p.y - 0.0384f, 0.0030f), spindle);
    };
    auto neckTop = [](const vec3& p) { return sdf::smax(neckCore(p), 0.0396f - p.y, 0.0030f); };
    s.addJoint("neck", Neck, [neckLow] {
        return sdf::meshVolume(neckLow, {vec3(0, 0.0f, 0)}, vol("neck", vec3(0, 1, 0), 0.0016f, 0.00012f, 0.012f));
    }, false, 1.0f);
    s.addJoint("neck_top", Neck, [neckTop] {
        return sdf::meshVolume(neckTop, {vec3(0, 0.07f, 0)}, vol("neck_top", vec3(0, 1, 0), 0.0016f, 0.00012f, 0.012f));
    }, true, 1.0f);
    s.addJoint("neck_tendon", Neck, [] {
        std::vector<vec3> seeds = {vec3(0.02f, 0.04f, 0.032f), vec3(-0.02f, 0.04f, 0.032f), vec3(0.029f, 0.035f, -0.033f),
                                   vec3(-0.029f, 0.035f, -0.033f)};
        return sdf::meshVolume(neckTendons, seeds, vol("neck_tendon", vec3(0, 1, 0), 0.0012f, 0.00016f, 0.012f));
    }, true, 0.0f);
}

}  // namespace build
}  // namespace character
