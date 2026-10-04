// Capsule model of two clasped right hands (draft of the anim test's check).
#pragma once
namespace shakecheck {
using namespace m;
using namespace character;

// Closest distance between segments [p1,q1] and [p2,q2] (either may be a point).
inline float segSeg(vec3 p1, vec3 q1, vec3 p2, vec3 q2) {
    const vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float s = 0.0f, t = 0.0f;
    if (a < 1e-12f && e < 1e-12f) return length(r);
    if (a < 1e-12f) t = clamp(f / e, 0.0f, 1.0f);
    else {
        const float c = dot(d1, r);
        if (e < 1e-12f) s = clamp(-c / a, 0.0f, 1.0f);
        else {
            const float b = dot(d1, d2), den = a * e - b * b;
            s = den > 1e-12f ? clamp((b * f - c * e) / den, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return length((p1 + d1 * s) - (p2 + d2 * t));
}
struct Capsule {
    vec3 a, b;
    float r;
};
// The phalanges of a right hand as capsules (joint to joint, the last to its tip), world space;
// radii = the porcelain's half thickness (robot_hand.cpp).
inline std::vector<Capsule> phalanges(const Skeleton& sk, const mat4* g) {
    static const float r[5][3] = {{0.0100f, 0.0064f, 0.0050f}, {0.0057f, 0.00475f, 0.00375f}, {0.0059f, 0.00495f, 0.00395f},
                                  {0.0055f, 0.00455f, 0.00365f}, {0.0047f, 0.00385f, 0.00305f}};
    std::vector<Capsule> out;
    for (int f = 0; f < 5; ++f)
        for (int j = 0; j < 3; ++j) {
            const Bone b = Bone(ThumbR1 + f * 3 + j);
            // (the last one ends a radius short of the fingertip, where the porcelain is rounded)
            const vec3 end = j < 2 ? g[b + 1].translation() : transformPoint(g[b], normalize(sk.restOffset[b]) * (sk.boneLength[b] - r[f][j]));
            out.push_back({g[b].translation(), end, r[f][j]});
        }
    return out;
}
// The palm of a right hand: three capsules inscribed in its porcelain (1 mm inside; it is thinner and
// rounded towards both edges), world space.
inline std::vector<Capsule> palm(const mat4& hand) {
    auto cap = [&](float x, float z, float y0, float r) { return Capsule{transformPoint(hand, vec3(x, y0, z)), transformPoint(hand, vec3(x, -0.068f, z)), r}; };
    return {cap(-0.0010f, 0.002f, -0.030f, 0.0105f), cap(0.0005f, 0.020f, -0.045f, 0.0095f), cap(0.0025f, -0.020f, -0.030f, 0.0095f)};
}
struct Margins {
    float palm = 1e9f, cuff = 1e9f, forearm = 1e9f, fingers = 1e9f;   // capsule clearances (m, < 0 = overlap)
    float thumbs = 1e9f;                                             // thumb metacarpal axes apart
    float behind = 1e9f, fromWrist = 1e9f, toKnuckles = 1e9f;        // long finger pads on the back of the other hand
    float cross = 0.0f;                                              // hand axes in the palm plane (rad)
};
// Hand A (globals a) against hand B (globals b), both real right hands.
inline Margins margins(const Skeleton& sk, const mat4* a, const mat4* b) {
    Margins m;
    const std::vector<Capsule> pa = phalanges(sk, a), pb = phalanges(sk, b), palmB = palm(b[HandR]);
    // B's wrist (carpal dome inside the forearm's cuff) and forearm (their cores).
    const vec3 wrist = b[HandR].translation(), elbow = b[ForeArmR].translation();
    const vec3 along = normalize(elbow - wrist);
    for (const Capsule& p : pa) {
        for (const Capsule& q : palmB) {
            const float d = segSeg(p.a, p.b, q.a, q.b) - p.r - q.r;
            if (d < m.palm && std::getenv("CAPDBG")) std::printf("    palm min %.1f mm by capsule %d (r %.1f) vs metacarpal %d\n", d * 1000, int(&p - pa.data()), p.r * 1000, int(&q - palmB.data()));
            m.palm = std::min(m.palm, d);
        }
        m.cuff = std::min(m.cuff, segSeg(p.a, p.b, wrist, wrist) - 0.021f - p.r);
        m.forearm = std::min(m.forearm, segSeg(p.a, p.b, wrist + along * 0.030f, elbow - along * 0.060f) - 0.023f - p.r);
        for (const Capsule& q : pb) m.fingers = std::min(m.fingers, segSeg(p.a, p.b, q.a, q.b) - p.r - q.r);
    }
    m.thumbs = segSeg(a[ThumbR1].translation(), a[ThumbR2].translation(), b[ThumbR1].translation(), b[ThumbR2].translation());
    const mat4 toB = inverseAffine(b[HandR]);
    for (int f = 1; f <= 4; ++f) {
        const Bone b3 = Bone(ThumbR1 + f * 3 + 2);
        const vec3 pad = transformPoint(toB, transformPoint(a[b3], vec3(0.0068f, -0.72f * sk.boneLength[b3], 0.0f)));
        m.behind = std::min(m.behind, -pad.x);
        m.fromWrist = std::min(m.fromWrist, -pad.y);
        m.toKnuckles = std::min(m.toKnuckles, pad.y + 0.086f);
    }
    const vec3 n = normalize(transformDir(a[HandR], vec3(1, 0, 0)));
    vec3 fa = transformDir(a[HandR], vec3(0, -1, 0)), fb = transformDir(b[HandR], vec3(0, -1, 0));
    fa = normalize(fa - n * dot(fa, n));
    fb = normalize(fb - n * dot(fb, n));
    m.cross = std::acos(clamp(std::fabs(dot(fa, fb)), 0.0f, 1.0f));
    return m;
}
}  // namespace shakecheck
