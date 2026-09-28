// Right hand (mirrored to the left): palm shell with carpal dome, thenar/thumb, three-segment
// fingers with rounded pads, nail plates (seams) and dark knuckle mechanisms. Seen at 20-40 cm in
// first person, so it gets the finest tessellation of the robot.
//
// Joint scheme (ball-jointed-doll style): every segment has a dark rounded base centred on its
// pivot (ball at MCP/CMC, hinge barrel at PIP/DIP/IP). The parent's porcelain end is a concave
// socket whose lips wrap that base with a hairline gap, and the child's porcelain shell starts
// just past the pivot. At rest only a thin dark seam shows; flexing opens a dark knuckle band on
// the back of the finger, and no rotation ever opens a hole.
//
// Hand frame (HandR, rest pose): origin = wrist centre, fingers along -Y, palm faces +X, thumb
// points forward (+Z), back of the hand faces -X.
#include "robot_build.h"

using namespace m;

namespace character {
namespace build {
namespace {

constexpr float kGap = 0.00040f;        // socket clearance around a dark base
constexpr float kLipOver = 0.0006f;     // parent lips reach this far past the pivot
constexpr float kShellStart = 0.0013f;  // child shell starts this far past the pivot
constexpr float kBase = 0.72f;          // hinge barrel radius / segment half thickness

struct Phalanx {
    vec3 axis, pad, width;         // unit frame: along the bone, flexion (pad) side, hinge axis
    float len;                     // distance to the next joint (or to the tip for distal)
    float t0, t1;                  // half thickness (pad direction) at the proximal / distal end
    float w0, w1;                  // half width (hinge direction)
    bool tip = false;              // distal phalanx: rounded fingertip, no distal joint
    bool ballBase = false;         // proximal joint is a ball (MCP/CMC) instead of a hinge
    float base0 = 0.0f;            // radius of this segment's dark base
    float base1 = 0.0f;            // radius of the next segment's dark base (socket = base1 + gap)
    bool ball1 = false;            // next joint is a ball
    float bevel = 36.0f * DEG;     // palmar bevel (room for flexion)
};

inline vec3 local(const Phalanx& ph, vec3 p) { return vec3(dot(p, ph.pad), dot(p, ph.axis), dot(p, ph.width)); }

float phalanxBody(const Phalanx& ph, float s, float u, float w) {
    float t = clamp(u / ph.len, 0.0f, 1.0f);
    float ht = lerp(ph.t0, ph.t1, t);
    float ws = ph.t0 / ph.w0;
    float padScale = s > 0.0f ? 1.05f : 1.0f;
    vec3 b(s / padScale, u, w * ws);
    float endU = ph.tip ? ph.len - ph.t1 : ph.len + ph.t1;
    float d = sdf::roundCone(b, vec3(0, -ph.t0, 0), vec3(0, endU, 0), ph.t0, ph.t1) * std::min(1.0f, ws);
    // Flat-ish pad and a softly flattened back: reads as a rounded rectangle, not a tube.
    d = sdf::smax(d, s - ht * 0.84f, 0.0045f);
    d = sdf::smax(d, -s - ht * 0.96f, 0.006f);
    return d;
}

float phalanxShell(const Phalanx& ph, vec3 p) {
    vec3 q = local(ph, p);
    float s = q.x, u = q.y, w = q.z;
    float d = phalanxBody(ph, s, u, w);
    const float k = 0.0007f, cb = std::cos(ph.bevel), sb = std::sin(ph.bevel);
    // Proximal end: flat ring just past the pivot, bevelled on the pad side.
    d = sdf::smax(d, kShellStart - u, k);
    d = sdf::smax(d, s * sb - (u - kShellStart) * cb, k);
    if (!ph.tip) {
        // Distal end: concave socket wrapping the next dark base, lips just past the pivot.
        float v = ph.len - u;  // distance before the next pivot
        float rho = ph.ball1 ? length(vec3(s, v, w)) : std::sqrt(s * s + v * v);
        d = sdf::smax(d, ph.base1 + kGap - rho, 0.0006f);
        d = sdf::smax(d, -v - kLipOver, k);
        d = sdf::smax(d, s * sb - (v + kLipOver) * cb + 0.0004f, k);
    }
    return d;
}

sdf::MeshOptions handOptions(vec3 tangentAxis) {
    sdf::MeshOptions o;
    o.maxEdge = 0.0030f;
    o.minEdge = 0.00030f;
    o.maxDeviation = 0.00008f;
    o.maxAngleDeg = 25.0f;
    o.nu = 24;
    o.nv = 18;
    o.capInset = 0.8f;
    o.tangentAxis = tangentAxis;
    return o;
}

sdf::VolumeOptions handVolume(vec3 tangentAxis, const char* name) {
    sdf::VolumeOptions o;
    o.cell = 0.00045f;
    o.maxError = 0.000035f;
    o.maxEdge = 0.0030f;
    o.tangentAxis = tangentAxis;
    o.name = name;
    return o;
}

MeshData meshPhalanx(const Phalanx& ph) {
    return sdf::meshVolume([&](const vec3& p) { return phalanxShell(ph, p); }, {ph.axis * (ph.len * 0.5f)}, handVolume(ph.axis, "phalanx"));
}

// Dark rounded base at the origin of a bone: ball, or hinge barrel along 'hinge'.
MeshData meshBase(vec3 hinge, float r, float halfLen, bool ball) {
    sdf::MeshOptions o = handOptions(hinge);
    o.maxDeviation = 0.00015f;
    o.maxAngleDeg = 30.0f;
    o.minEdge = 0.0005f;
    o.nu = 16;
    o.nv = 8;
    o.capInset = 0.0f;
    o.name = "base";
    if (ball) return sdf::meshSegment([r](const vec3& p) { return sdf::sphere(p, r); }, vec3(0), vec3(0), o, hinge);
    // Hinge barrel: a cylinder with 1.2 mm filleted ends, built analytically as a lathe (exact
    // normals; far fewer triangles than an adaptive mesh of the fillets).
    vec3 ax = normalize(hinge);
    vec3 bu = orthogonal(ax), bv = cross(ax, bu);
    const float e = std::min(0.0012f, std::min(r, halfLen) * 0.5f);
    struct P { float rho, a, nr, na; };
    std::vector<P> prof;
    for (int side = -1; side <= 1; side += 2) {
        std::vector<P> half;
        half.push_back({0.0f, halfLen, 0.0f, 1.0f});
        half.push_back({(r - e) * 0.55f, halfLen, 0.0f, 1.0f});
        for (int k = 0; k <= 4; ++k) {
            float th = 1.5707963f * float(k) / 4.0f;
            half.push_back({r - e + e * std::sin(th), halfLen - e + e * std::cos(th), std::sin(th), std::cos(th)});
        }
        // Profile runs from the -axis pole over the side to the +axis pole.
        if (side < 0) {
            for (const P& q : half) prof.push_back({q.rho, -q.a, q.nr, -q.na});
        } else {
            prof.insert(prof.end(), half.rbegin(), half.rend());
        }
    }
    const int segs = 24;
    MeshData d;
    std::vector<uint32_t> ringStart;
    for (const P& q : prof) {
        ringStart.push_back(uint32_t(d.vertices.size()));
        int n = q.rho <= 0.0f ? 1 : segs;
        for (int j = 0; j < n; ++j) {
            float ph = 6.2831853f * float(j) / float(segs);
            vec3 radial = bu * std::cos(ph) + bv * std::sin(ph);
            Vertex v;
            v.pos = ax * q.a + radial * q.rho;
            v.normal = normalize(ax * q.na + radial * q.nr);
            vec3 t = ax - v.normal * dot(v.normal, ax);
            t = length2(t) > 1e-6f ? normalize(t) : normalize(cross(ax, radial));
            v.tangent = vec4(t, 1.0f);
            v.uv = vec2(float(j) / float(segs), 0.0f);
            d.vertices.push_back(v);
        }
    }
    for (size_t i = 0; i + 1 < prof.size(); ++i) {
        uint32_t r0 = ringStart[i], r1 = ringStart[i + 1];
        bool pole0 = prof[i].rho <= 0.0f, pole1 = prof[i + 1].rho <= 0.0f;
        for (int j = 0; j < segs; ++j) {
            uint32_t j1 = uint32_t((j + 1) % segs);
            if (pole0) d.indices.insert(d.indices.end(), {r0, r1 + uint32_t(j), r1 + j1});
            else if (pole1) d.indices.insert(d.indices.end(), {r0 + uint32_t(j), r1, r0 + j1});
            else d.indices.insert(d.indices.end(), {r0 + uint32_t(j), r1 + uint32_t(j), r1 + j1, r0 + uint32_t(j), r1 + j1, r0 + j1});
        }
    }
    // Outward winding.
    for (size_t i = 0; i + 2 < d.indices.size(); i += 3) {
        const Vertex &a = d.vertices[d.indices[i]], &b = d.vertices[d.indices[i + 1]], &c = d.vertices[d.indices[i + 2]];
        if (dot(cross(b.pos - a.pos, c.pos - a.pos), a.normal + b.normal + c.normal) < 0.0f) std::swap(d.indices[i + 1], d.indices[i + 2]);
    }
    return d;
}

// Metacarpal knuckle line of the right hand (y of the MCP joints as a function of z).
float mcpLineY(float z) { return -0.088f + 6.9f * (z - 0.012f) * (z - 0.012f); }

}  // namespace

void buildHand(Sink& s) {
    const Skeleton& sk = robotSkeleton();
    size_t first = s.parts.size();

    // ---- Fingers ------------------------------------------------------------------------------
    const float fscale[4] = {1.0f, 1.03f, 0.97f, 0.86f};   // index, middle, ring, pinky
    const float T[3][2] = {{0.0079f, 0.0070f}, {0.0068f, 0.0062f}, {0.0060f, 0.0052f}};
    const float W[3][2] = {{0.0087f, 0.0080f}, {0.0079f, 0.0073f}, {0.0073f, 0.0064f}};
    float mcpBall[4];
    for (int f = 0; f < 4; ++f) {
        Bone b1 = Bone(IndexR1 + f * 3);
        float sc = fscale[f];
        mcpBall[f] = T[0][0] * sc * 0.93f;
        for (int seg = 0; seg < 3; ++seg) {
            Bone b = Bone(b1 + seg);
            Phalanx ph;
            ph.axis = vec3(0, -1, 0);
            ph.pad = vec3(1, 0, 0);
            ph.width = vec3(0, 0, 1);
            ph.len = sk.boneLength[b];
            ph.tip = seg == 2;
            ph.ballBase = seg == 0;
            ph.t0 = T[seg][0] * sc;
            ph.t1 = T[seg][1] * sc;
            ph.w0 = W[seg][0] * sc;
            ph.w1 = W[seg][1] * sc;
            ph.base0 = seg == 0 ? mcpBall[f] : T[seg][0] * sc * kBase;
            ph.base1 = seg < 2 ? T[seg + 1][0] * sc * kBase : 0.0f;
            PorcelainLook look;
            look.variant = 1.0f;  // soft-touch pad on the palm side of seam 0
            look.seams[0] = seamPlane(vec3(1, 0, 0), vec3(ph.t0 * 0.50f, 0, 0));
            if (ph.tip) look.seams[1] = seamPlane(vec3(-1, 0, 0), vec3(-ph.t1 * 0.58f, 0, 0));  // nail plate outline
            s.addPorcelain("finger", b, [ph] { return meshPhalanx(ph); }, false, look);
            float halfLen = std::min(ph.w0, seg > 0 ? W[seg - 1][1] * sc : ph.w0) * 0.93f;
            bool ball = seg == 0;
            s.addJoint("knuckle", b, [ph, halfLen, ball] { return meshBase(vec3(0, 0, 1), ph.base0, halfLen, ball); }, false, 0.0f);
        }
    }

    // ---- Thumb --------------------------------------------------------------------------------
    const float cmcBall = 0.0092f;
    {
        vec3 o2 = sk.restOffset[ThumbR2], o3 = sk.restOffset[ThumbR3];
        vec3 a1 = normalize(o2), a2 = normalize(o3);
        auto padOf = [](vec3 a) { return normalize(vec3(0, -a.z, a.y)); };
        const float TT[2][2] = {{0.0086f, 0.0078f}, {0.0077f, 0.0063f}};
        const float TW[2][2] = {{0.0098f, 0.0092f}, {0.0092f, 0.0080f}};
        const float bases[2] = {TT[0][0] * kBase, TT[1][0] * kBase};
        // Metacarpal + thenar eminence.
        Phalanx mc;
        mc.axis = a1;
        mc.pad = padOf(a1);
        mc.width = vec3(1, 0, 0);
        mc.len = length(o2);
        mc.ballBase = true;
        mc.t0 = 0.0100f; mc.t1 = 0.0084f;
        mc.w0 = 0.0112f; mc.w1 = 0.0094f;
        mc.base0 = cmcBall;
        mc.base1 = bases[0];
        auto thenar = [mc](const vec3& p) {
            float d = phalanxShell(mc, p);
            vec3 q = local(mc, p);
            // Muscle mass towards the palm (+X) and the fingers (pad side).
            float bulge = sdf::ellipsoid(q - vec3(0.0026f, 0.0170f, 0.0040f), vec3(0.0086f, 0.0170f, 0.0092f));
            bulge = sdf::smax(bulge, kShellStart + 0.001f - q.y, 0.003f);
            bulge = sdf::smax(bulge, q.y - (mc.len - 0.0095f), 0.004f);
            return sdf::smin(d, bulge, 0.006f);
        };
        PorcelainLook lookMc;
        lookMc.seams[0] = seamPlane(-mc.pad, -mc.pad * 0.0090f);
        s.addPorcelain("thumb", ThumbR1, [thenar, a1, mc] { return sdf::meshVolume(thenar, {a1 * (mc.len * 0.5f)}, handVolume(a1, "thenar")); }, false, lookMc);
        s.addJoint("thumb_cmc", ThumbR1, [cmcBall] { return meshBase(vec3(1, 0, 0), cmcBall, 0.0f, true); }, false);
        for (int seg = 0; seg < 2; ++seg) {
            Phalanx ph;
            ph.axis = a2;
            ph.pad = padOf(a2);
            ph.width = vec3(1, 0, 0);
            ph.len = seg == 0 ? length(o3) : sk.boneLength[ThumbR3];
            ph.tip = seg == 1;
            ph.t0 = TT[seg][0]; ph.t1 = TT[seg][1];
            ph.w0 = TW[seg][0]; ph.w1 = TW[seg][1];
            ph.base0 = bases[seg];
            ph.base1 = seg == 0 ? bases[1] : 0.0f;
            Bone b = seg == 0 ? ThumbR2 : ThumbR3;
            PorcelainLook look;
            look.variant = 1.0f;
            look.seams[0] = seamPlane(ph.pad, ph.pad * (ph.t0 * 0.50f));
            if (ph.tip) look.seams[1] = seamPlane(-ph.pad, -ph.pad * (ph.t1 * 0.58f));
            s.addPorcelain("thumb", b, [ph] { return meshPhalanx(ph); }, false, look);
            float halfLen = std::min(ph.w0, seg == 0 ? mc.w1 : TW[0][1]) * 0.93f;
            s.addJoint("knuckle", b, [ph, halfLen] { return meshBase(vec3(1, 0, 0), ph.base0, halfLen, false); }, false);
        }
    }

    // ---- Palm ---------------------------------------------------------------------------------
    {
        vec3 mcp[4];
        float sock[4];
        for (int f = 0; f < 4; ++f) {
            mcp[f] = sk.restOffset[Bone(IndexR1 + f * 3)];
            sock[f] = mcpBall[f] + kGap;
        }
        vec3 cmc = sk.restOffset[ThumbR1];
        const float wristR = 0.0205f;
        auto palm = [mcp, sock, cmc, cmcBall, wristR](const vec3& p) {
            // Metacarpal slab: superelliptic cross-section, arched back, wider at the knuckles.
            float y = p.y;
            float t = smoothstep(-0.012f, -0.070f, y);  // 0 at the wrist .. 1 at the knuckles
            float halfZ = lerp(0.0214f, 0.0358f, t);
            float zc = lerp(0.0f, 0.0022f, t);
            float halfX = lerp(0.0132f, 0.0117f, t);
            float z = p.z - zc;
            float xs = p.x + 0.0010f - 4.5f * z * z;  // the edges drop towards the palm: convex back
            float qx = xs / halfX, qz = z / halfZ;
            float n4 = std::sqrt(std::sqrt(qx * qx * qx * qx + qz * qz * qz * qz));
            float d = (n4 - 1.0f) * halfX * 0.9f;
            // Subtle knuckles on the back of the hand.
            for (int f = 0; f < 4; ++f) d = sdf::smin(d, sdf::sphere(p - mcp[f] - vec3(-0.0020f, 0.0030f, 0), sock[f] + 0.0010f), 0.005f);
            // Hypothenar pad (pinky side of the palm).
            float hyp = sdf::ellipsoid(p - vec3(0.0078f, -0.045f, -0.021f), vec3(0.0068f, 0.025f, 0.0115f));
            d = sdf::smin(d, hyp, 0.006f);
            // Carpal dome: sphere around the wrist pivot (slides inside the forearm socket).
            float ball = sdf::sphere(p, wristR);
            float dome = sdf::smax(ball, std::fabs(p.x + 0.0008f) - 0.0150f, 0.004f);
            d = sdf::smin(d, dome, 0.008f);
            // Near the wrist everything stays inside the ball, so the hand can bend freely.
            d = sdf::smax(d, sdf::smin(ball, p.y + 0.0125f, 0.009f), 0.0015f);
            // Distal end: concave sockets around each finger's dark ball, lips just past the pivots,
            // palmar lip bevelled for flexion.
            float yEnd = mcpLineY(p.z);
            const float sb = std::sin(36.0f * DEG), cb = std::cos(36.0f * DEG);
            d = sdf::smax(d, yEnd - kLipOver - p.y, 0.0010f);
            d = sdf::smax(d, (p.x + 0.004f) * sb - (p.y - yEnd + kLipOver) * cb + 0.0004f, 0.0010f);
            for (int f = 0; f < 4; ++f) d = sdf::smax(d, sock[f] - length(p - mcp[f]), 0.0006f);
            // Thumb root socket.
            d = sdf::smax(d, cmcBall + kGap - length(p - cmc), 0.0008f);
            return d;
        };
        PorcelainLook look;
        // Back plate outline (the palm side is a soft-touch pad) and a knuckle-plate seam.
        look.variant = 1.0f;
        look.seams[0] = seamPlane(vec3(1, 0, 0), vec3(0.0040f, 0, 0));
        look.seams[1] = seamPlane(vec3(0, -1, 0.10f), vec3(0, -0.066f, 0));
        look.seams[2] = seamPlane(vec3(0, 1, 0), vec3(0, -0.0205f, 0));
        sdf::VolumeOptions o = handVolume(vec3(0, -1, 0), "palm");
        o.cell = 0.0006f;
        o.maxError = 0.00005f;
        s.addPorcelain("palm", HandR, [palm, o] { return sdf::meshVolume(palm, {vec3(0.001f, -0.045f, 0.0f)}, o); }, false, look);
        // Wrist mechanism inside the dome (visible when the wrist bends).
        sdf::MeshOptions oj = handOptions(vec3(0, 1, 0));
        oj.nu = 16;
        oj.nv = 10;
        oj.maxDeviation = 0.00015f;
        oj.maxAngleDeg = 30.0f;
        oj.capInset = 0.0f;
        s.addJoint("wrist", HandR, [oj] { return sdf::meshSegment([](const vec3& p) { return sdf::sphere(p, 0.0148f); }, vec3(0), vec3(0), oj); }, false);
    }
    s.mirrorFrom(first);
}

}  // namespace build
}  // namespace character
