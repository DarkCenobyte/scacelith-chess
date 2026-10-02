// Head: porcelain face mask and skull shell, eyes (sclera, iris under a refracting cornea,
// transparent cornea) and eyelid shells.
//
// Head space: origin = atlas pivot, +Z = face direction, eyes at (+-0.032, 0.080, 0.075).
// The face is a smooth elongated ovoid without nose or ears; each eye sits in a spherical socket
// (radius LID_OUTER + clearance) seen through an almond window in the mask. The lid shells are
// concentric with the eyeball and slide inside that socket, so blinking never opens a gap.
#include "robot_build.h"

using namespace m;

namespace character {

float eye::limbusZ() { return std::sqrt(RADIUS * RADIUS - LIMBUS_RADIUS * LIMBUS_RADIUS); }
float eye::corneaCenterZ() { return limbusZ() - std::sqrt(CORNEA_RADIUS * CORNEA_RADIUS - LIMBUS_RADIUS * LIMBUS_RADIUS); }

quat lidRotation(Bone lid, float closure, float gazePitch) {
    closure = clamp(closure, 0.0f, 1.0f);
    bool upper = lid == LidUpperL || lid == LidUpperR;
    float follow = clamp(gazePitch, -0.5f, 0.5f);
    if (upper) {
        float open = eye::LID_UPPER_OPEN + follow * 0.8f;
        float a = lerp(open, eye::LID_CLOSED, closure);
        return axisAngle(vec3(1, 0, 0), eye::LID_UPPER_OPEN - a);
    }
    float open = -eye::LID_LOWER_OPEN + follow * 0.35f;
    float a = lerp(open, eye::LID_CLOSED, closure);
    return axisAngle(vec3(1, 0, 0), -eye::LID_LOWER_OPEN - a);
}

namespace build {
namespace {

const vec3 kEye = robotSkeleton().restOffset[EyeL];  // right/left eye centre (|x|) in head space
constexpr float kSocket = eye::LID_OUTER + 0.0004f;  // spherical socket the lids slide in

// Cubic norm (superellipsoid with flatter sides than an ellipsoid).
inline float pnorm3(vec3 q) {
    vec3 a = abs(q);
    return std::cbrt(a.x * a.x * a.x + a.y * a.y * a.y + a.z * a.z * a.z);
}

// Almond window around an eye (eye-centred coordinates, x mirrored so +x = lateral).
float eyeWindow(vec3 e) {
    // Slight upward tilt towards the lateral corner, fuller above than below.
    float y = e.y - 0.08f * e.x - 0.0004f;
    float ry = y > 0.0f ? 0.0076f : 0.0068f;
    float rx = 0.0139f;
    float k = length(vec2(e.x / rx, y / ry));
    return (k - 1.0f) * std::min(rx, ry);
}

// Face widths / depth along the height of the face (monotone profiles, head space y).
const sdf::Profile kFaceRx = {{-0.036f, 0.026f}, {-0.016f, 0.036f}, {0.004f, 0.051f}, {0.040f, 0.064f}, {0.080f, 0.069f}};
const sdf::Profile kFaceRz = {{-0.036f, 0.084f}, {0.000f, 0.088f}, {0.060f, 0.088f}};

float headSolid(const vec3& p) {
    float ax = std::fabs(p.x);
    vec3 pm(ax, p.y, p.z);
    // Cranium.
    float d = sdf::ellipsoid(p - vec3(0, 0.100f, -0.006f), vec3(0.0715f, 0.095f, 0.095f));
    // Forehead (fuller, more upright) and the occiput curving into the neck.
    d = sdf::smin(d, sdf::ellipsoid(p - vec3(0, 0.128f, 0.030f), vec3(0.062f, 0.052f, 0.062f)), 0.03f);
    d = sdf::smin(d, sdf::ellipsoid(p - vec3(0, 0.038f, -0.046f), vec3(0.050f, 0.046f, 0.046f)), 0.03f);
    // Face: flat-fronted superellipsoid narrowing to the chin (egg-shaped mask).
    float rx = kFaceRx(p.y), rz = kFaceRz(p.y);
    vec3 fq = (p - vec3(0, 0.050f, 0.010f)) / vec3(rx, 0.086f, rz);
    float face = (pnorm3(fq) - 1.0f) * std::min(rx, rz) * 0.85f;
    // Jaw line: nothing below the line from the chin to the jaw angle and the skull base.
    face = sdf::smax(face, dot(p - vec3(0, -0.034f, 0.060f), normalize(vec3(0, -0.868f, -0.496f))), 0.012f);
    d = sdf::smin(d, face, 0.028f);
    // Soft brow ridge and cheekbones (planes rather than features).
    d = sdf::smin(d, sdf::capsule(pm, vec3(0.009f, 0.100f, 0.092f), vec3(0.050f, 0.099f, 0.077f), 0.0070f), 0.020f);
    d = sdf::smin(d, sdf::ellipsoid(pm - vec3(0.047f, 0.051f, 0.066f), vec3(0.017f, 0.011f, 0.016f)), 0.020f);
    float cheekPlane = dot(pm - vec3(0.058f, 0.040f, 0.058f), normalize(vec3(0.80f, -0.25f, 0.55f)));
    d = sdf::smax(d, cheekPlane, 0.018f);
    // Temples: flatter sides (no ears).
    d = sdf::smax(d, ax - 0.0705f, 0.022f);
    return d;
}

float headShell(const vec3& p) {
    float d = headSolid(p);
    float ax = std::fabs(p.x);
    vec3 pm(ax, p.y, p.z);
    // Orbits: around each eye the mask follows the eyeball (a sphere just outside the lid shells),
    // recessed under the brow and above the cheek like a real eye region.
    vec3 e = pm - kEye;
    {
        float y = e.y - 0.05f * e.x;
        float ry = y > 0.0f ? 0.0135f : 0.0115f;
        float orbitWin = (length(vec2(e.x / 0.0165f, y / ry)) - 1.0f) * ry;
        float orbit = sdf::smax(orbitWin, (kSocket + 0.0009f) - length(e), 0.004f);
        orbit = sdf::smax(orbit, -e.z, 0.002f);
        orbit = sdf::smax(orbit, -(d + 0.0032f), 0.002f);  // never deeper than 3.2 mm below the mask
        d = sdf::smax(d, -orbit, 0.0040f);
    }
    // Eye sockets: a spherical cavity for the eye and lids, opened to the front through an almond
    // window. The lid shells fill the window; the cavity wall shows only as a thin dark gap.
    float tunnel = std::max(eyeWindow(e), -e.z);
    d = sdf::smax(d, -tunnel, 0.0018f);
    // Opening underneath for the neck core.
    vec3 n = p - vec3(0, 0.0f, -0.002f);
    float hole = sdf::smax(length(vec2(n.x * 0.95f, n.z)) - 0.053f, n.y - 0.018f, 0.006f);
    d = sdf::smax(d, -hole, 0.004f);
    return d;
}

// The mask is two panels: the face plate and the skull, split by a plane with a hairline gap.
const vec3 kFaceSplitN = normalize(vec3(0, 0.22f, 1.0f));
const vec3 kFaceSplitP(0, 0.060f, 0.034f);
float facePlate(const vec3& p) { return sdf::smax(headShell(p), 0.00025f - dot(p - kFaceSplitP, kFaceSplitN), 0.0010f); }
float skullShell(const vec3& p) { return sdf::smax(headShell(p), dot(p - kFaceSplitP, kFaceSplitN) + 0.00025f, 0.0010f); }

// Eyelid shell in lid-bone space (open pose): the part of the spherical shell beyond its margin.
float lidShell(const vec3& p, bool upper) {
    float r = length(p);
    float d = std::max(r - eye::LID_OUTER, eye::LID_INNER - r);
    float a = upper ? eye::LID_UPPER_OPEN : -eye::LID_LOWER_OPEN;
    vec3 n = vec3(0, std::cos(a), -std::sin(a));  // margin plane normal (towards the covered side)
    float g = dot(p, n);
    d = sdf::smax(d, upper ? -g : g, 0.0008f);
    // Only the front part of the sphere (the rest stays hidden inside the head).
    d = sdf::smax(d, -(p.z + 0.006f), 0.002f);
    return d;
}

MeshData capMesh(float sphereR, float centerZ, float zMin, int rings, int segs) {
    // Spherical cap (radius sphereR centred on (0,0,centerZ)) for z >= zMin, pole at +Z.
    MeshData d;
    float cosMax = clamp((zMin - centerZ) / sphereR, -1.0f, 1.0f);
    float thetaMax = std::acos(cosMax);
    for (int i = 0; i <= rings; ++i) {
        float t = float(i) / float(rings);
        float th = thetaMax * t;
        int n = i == 0 ? 1 : segs;
        for (int j = 0; j < n; ++j) {
            float ph = 6.2831853f * float(j) / float(segs);
            vec3 dir(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
            Vertex v;
            v.pos = vec3(0, 0, centerZ) + dir * sphereR;
            v.normal = dir;
            vec3 tg = normalize(vec3(-std::sin(ph), std::cos(ph), 0.0f));
            v.tangent = vec4(tg, 1.0f);
            v.uv = vec2(float(j) / float(segs), t);
            d.vertices.push_back(v);
        }
    }
    for (int j = 0; j < segs; ++j) {
        d.indices.push_back(0);
        d.indices.push_back(1 + uint32_t(j));
        d.indices.push_back(1 + uint32_t((j + 1) % segs));
    }
    for (int i = 1; i < rings; ++i) {
        uint32_t r0 = 1 + uint32_t((i - 1) * segs), r1 = r0 + uint32_t(segs);
        for (int j = 0; j < segs; ++j) {
            uint32_t a = r0 + uint32_t(j), b = r0 + uint32_t((j + 1) % segs), c = r1 + uint32_t(j), e = r1 + uint32_t((j + 1) % segs);
            d.indices.insert(d.indices.end(), {a, c, e, a, e, b});
        }
    }
    return d;
}

}  // namespace

void buildHead(Sink& s) {
    // ---- head shell: face plate + skull ---------------------------------------------------------
    {
        PorcelainLook face;
        face.variant = 2.0f;  // the shader draws the closed-mouth line
        s.addPorcelain("face", Head, [] {
            sdf::VolumeOptions o;
            o.cell = 0.00070f;
            o.maxError = 0.00004f;
            o.maxEdge = 0.007f;
            o.tangentAxis = vec3(0, 1, 0);
            o.name = "face";
            return sdf::meshVolume(facePlate, {vec3(0, 0.050f, 0.080f)}, o);
        }, true, face);
        PorcelainLook skull;
        skull.seams[0] = seamPlane(vec3(0, 1.0f, -0.62f), vec3(0, 0.160f, -0.050f));  // crown panel
        s.addPorcelain("skull", Head, [] {
            sdf::VolumeOptions o;
            o.cell = 0.0016f;
            o.maxError = 0.00008f;
            o.maxEdge = 0.012f;
            o.tangentAxis = vec3(0, 1, 0);
            o.name = "skull";
            return sdf::meshVolume(skullShell, {vec3(0, 0.10f, -0.02f)}, o);
        }, true, skull);
    }
    // ---- eyes -----------------------------------------------------------------------------------
    size_t first = s.parts.size();  // eyes and lids are mirrored, the head shell is not
    {
        const float lz = eye::limbusZ(), cz = eye::corneaCenterZ();
        vec4 none[4] = {vec4(0), vec4(0), vec4(0), vec4(0)};
        s.add("eye_sclera", EyeR, MaterialId::RobotEyeSclera, [] { return capMesh(eye::RADIUS, 0.0f, -eye::RADIUS, 18, 40); }, true, none);
        s.add("eye_iris", EyeR, MaterialId::RobotEyeIris, [lz, cz] { return capMesh(eye::CORNEA_RADIUS, cz, lz, 12, 48); }, true, none);
        s.add("eye_cornea", EyeR, MaterialId::RobotEyeCornea,
              [lz, cz] { return capMesh(eye::CORNEA_RADIUS + 0.00004f, cz, lz - 0.00002f, 12, 48); }, true, none);
    }
    // ---- eyelids --------------------------------------------------------------------------------
    for (int k = 0; k < 2; ++k) {
        bool upper = k == 0;
        Bone b = upper ? LidUpperR : LidLowerR;
        float a = upper ? eye::LID_UPPER_OPEN : -eye::LID_LOWER_OPEN;
        vec3 n = vec3(0, std::cos(a), -std::sin(a));
        PorcelainLook look;
        look.seams[0] = vec4(upper ? n : -n, 0.0f);  // margin plane (the shader darkens the margin edge)
        s.addPorcelain(upper ? "lid_upper" : "lid_lower", b, [upper] {
            sdf::VolumeOptions o;
            o.cell = 0.00028f;
            o.maxError = 0.000025f;
            o.maxEdge = 0.004f;
            o.tangentAxis = vec3(1, 0, 0);
            o.name = "lid";
            vec3 seed = upper ? vec3(0, 0.010f, 0.010f) : vec3(0, -0.010f, 0.010f);
            seed = normalize(seed) * (0.5f * (eye::LID_INNER + eye::LID_OUTER));
            return sdf::meshVolume([upper](const vec3& p) { return lidShell(p, upper); }, {seed}, o);
        }, true, look, MaterialId::RobotLid);
    }
    s.mirrorFrom(first);
}

}  // namespace build
}  // namespace character
