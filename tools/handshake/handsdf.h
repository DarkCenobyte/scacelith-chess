// Exact SDFs of the right hand parts (copied from robot_hand.cpp / robot_body.cpp), bone-local.
// Include AFTER "character/robot_hand.cpp" (uses its anonymous-namespace helpers).
#pragma once
namespace probe {
using namespace m;
using namespace character;
using namespace character::build;
// ---- forearm (copy of robot_body.cpp forearmShell; constants renamed)
const float fGap = 0.0010f, fElbowBall = 0.0255f;
const vec3 fWrist(0, -0.265f, 0);
inline float startCut(float u, float s, float start, float bevel) { return s * std::sin(bevel) - (u - start) * std::cos(bevel); }
float forearm(const vec3& p) {
    float t = clamp(-p.y / 0.265f, 0.0f, 1.0f);
    float sx = lerp(0.97f, 0.90f, t);
    vec3 q(p.x / sx, p.y, p.z);
    float d = sdf::roundCone(q, vec3(0, -0.040f, 0), vec3(0, -0.252f, 0), 0.0330f, 0.0256f) * sx;
    d = sdf::smin(d, sdf::ellipsoid(p - vec3(0.002f, -0.085f, 0.004f), vec3(0.029f, 0.075f, 0.033f)), 0.02f);
    d = sdf::smax(d, (fElbowBall + fGap) - length(p), 0.0025f);
    d = sdf::smax(d, startCut(-p.y, p.z, 0.005f, 42.0f * DEG), 0.003f);
    vec3 w = p - fWrist;
    d = sdf::smin(d, sdf::sphere(w, kWristDome + 0.0045f), 0.010f);
    d = sdf::smax(d, (kWristDome + 0.0005f) - length(w), 0.0020f);
    d = sdf::smax(d, -w.y - 0.0025f, 0.0025f);
    return d;
}
// ---- upper arm (copy of robot_body.cpp upperArmShell) + shoulder ball; forearm + elbow ball
const float fShoulderBall = 0.0300f;
inline float endCut(float v, float s, float lip, float bevel) { return s * std::sin(bevel) - (v + lip) * std::cos(bevel); }
float upperArm(const vec3& p) {
    const vec3 kElbow(0, -0.300f, 0);
    vec3 q(p.x * 1.05f, p.y, p.z);
    float d = sdf::roundCone(q, vec3(0, -0.060f, 0.0f), vec3(0, -0.272f, 0.001f), 0.0370f, 0.0305f);
    d = sdf::smin(d, sdf::ellipsoid(p - vec3(0.001f, -0.165f, 0.009f), vec3(0.029f, 0.080f, 0.029f)), 0.02f);
    d = sdf::smin(d, sdf::ellipsoid(p - vec3(0.000f, -0.130f, -0.010f), vec3(0.030f, 0.085f, 0.030f)), 0.02f);
    float delt = sdf::ellipsoid(p - vec3(-0.005f, -0.020f, 0.0f), vec3(0.047f, 0.066f, 0.049f));
    d = sdf::smin(d, delt, 0.02f);
    float inner = p.x - 0.024f - std::max(0.0f, -p.y - 0.045f) * 0.9f;
    d = sdf::smax(d, inner, 0.010f);
    d = sdf::smax(d, (fShoulderBall + fGap) - length(p), 0.002f);
    vec3 e = p - kElbow;
    d = sdf::smax(d, (fElbowBall + fGap) - length(e), 0.0025f);
    d = sdf::smax(d, endCut(e.y, e.z, 0.003f, 42.0f * DEG), 0.003f);
    return std::min(d, sdf::sphere(p, fShoulderBall));
}
// ---- hand parts (same construction as buildHand)
struct HandSdf {
    Phalanx ph[5][3];
    float halfLen[5][3];
    Phalanx mc;
    vec3 mcp[4];
    float sock[4];
    vec3 cmc;
    float cmcBall = 0.0092f;
    HandSdf() {
        const Skeleton& sk = robotSkeleton();
        const float fscale[4] = {1.0f, 1.03f, 0.97f, 0.86f};
        const float T[3][2] = {{0.0079f, 0.0070f}, {0.0068f, 0.0062f}, {0.0060f, 0.0052f}};
        const float W[3][2] = {{0.0087f, 0.0080f}, {0.0079f, 0.0073f}, {0.0073f, 0.0064f}};
        float mcpBall[4];
        for (int f = 0; f < 4; ++f) {
            Bone b1 = Bone(IndexR1 + f * 3);
            float sc = fscale[f];
            mcpBall[f] = T[0][0] * sc * 0.93f;
            for (int seg = 0; seg < 3; ++seg) {
                Bone b = Bone(b1 + seg);
                Phalanx& p = ph[f + 1][seg];
                p.axis = vec3(0, -1, 0); p.pad = vec3(1, 0, 0); p.width = vec3(0, 0, 1);
                p.len = sk.boneLength[b]; p.tip = seg == 2;
                p.t0 = T[seg][0] * sc; p.t1 = T[seg][1] * sc; p.w0 = W[seg][0] * sc; p.w1 = W[seg][1] * sc;
                p.base0 = seg == 0 ? mcpBall[f] : T[seg][0] * sc * kBase;
                p.base1 = seg < 2 ? T[seg + 1][0] * sc * kBase : 0.0f;
                halfLen[f + 1][seg] = std::min(p.w0, seg > 0 ? W[seg - 1][1] * sc : p.w0) * 0.93f;
            }
        }
        vec3 o2 = sk.restOffset[ThumbR2], o3 = sk.restOffset[ThumbR3];
        vec3 a1 = normalize(o2), a2 = normalize(o3);
        auto padOf = [](vec3 a) { return normalize(vec3(0, -a.z, a.y)); };
        const float TT[2][2] = {{0.0086f, 0.0078f}, {0.0077f, 0.0063f}};
        const float TW[2][2] = {{0.0098f, 0.0092f}, {0.0092f, 0.0080f}};
        const float bases[2] = {TT[0][0] * kBase, TT[1][0] * kBase};
        mc.axis = a1; mc.pad = padOf(a1); mc.width = vec3(1, 0, 0); mc.len = length(o2);
        mc.t0 = 0.0100f; mc.t1 = 0.0084f; mc.w0 = 0.0112f; mc.w1 = 0.0094f; mc.base0 = cmcBall; mc.base1 = bases[0];
        for (int seg = 0; seg < 2; ++seg) {
            Phalanx& p = ph[0][seg + 1];
            p.axis = a2; p.pad = padOf(a2); p.width = vec3(1, 0, 0);
            p.len = seg == 0 ? length(o3) : sk.boneLength[ThumbR3]; p.tip = seg == 1;
            p.t0 = TT[seg][0]; p.t1 = TT[seg][1]; p.w0 = TW[seg][0]; p.w1 = TW[seg][1];
            p.base0 = bases[seg]; p.base1 = seg == 0 ? bases[1] : 0.0f;
            halfLen[0][seg + 1] = std::min(p.w0, seg == 0 ? mc.w1 : TW[0][1]) * 0.93f;
        }
        for (int f = 0; f < 4; ++f) { mcp[f] = sk.restOffset[Bone(IndexR1 + f * 3)]; sock[f] = mcpBall[f] + kGap; }
        cmc = sk.restOffset[ThumbR1];
    }
    float thenar(const vec3& p) const {
        float d = phalanxShell(mc, p);
        vec3 q = local(mc, p);
        float bulge = sdf::ellipsoid(q - vec3(0.0026f, 0.0170f, 0.0040f), vec3(0.0086f, 0.0170f, 0.0092f));
        bulge = sdf::smax(bulge, kShellStart + 0.001f - q.y, 0.003f);
        bulge = sdf::smax(bulge, q.y - (mc.len - 0.0095f), 0.004f);
        d = sdf::smin(d, bulge, 0.006f);
        return std::min(d, sdf::sphere(p, cmcBall));
    }
    float palm(const vec3& p) const {
        float y = p.y;
        float t = smoothstep(-0.012f, -0.070f, y);
        float halfZ = lerp(0.0214f, 0.0358f, t), zc = lerp(0.0f, 0.0022f, t), halfX = lerp(0.0132f, 0.0117f, t);
        float z = p.z - zc;
        float xs = p.x + 0.0010f - 4.5f * z * z;
        float qx = xs / halfX, qz = z / halfZ;
        float n4 = std::sqrt(std::sqrt(qx * qx * qx * qx + qz * qz * qz * qz));
        float d = (n4 - 1.0f) * halfX * 0.9f;
        for (int f = 0; f < 4; ++f) d = sdf::smin(d, sdf::sphere(p - mcp[f] - vec3(-0.0020f, 0.0030f, 0), sock[f] + 0.0010f), 0.005f);
        float hyp = sdf::ellipsoid(p - vec3(0.0078f, -0.045f, -0.021f), vec3(0.0068f, 0.025f, 0.0115f));
        d = sdf::smin(d, hyp, 0.006f);
        float ball = sdf::sphere(p, kWristDome);
        float dome = sdf::smax(ball, std::fabs(p.x + 0.0008f) - 0.0150f, 0.004f);
        d = sdf::smin(d, dome, 0.008f);
        d = sdf::smax(d, sdf::smin(ball, p.y + 0.0125f, 0.009f), 0.0015f);
        float yEnd = mcpLineY(p.z);
        const float sb = std::sin(36.0f * DEG), cb = std::cos(36.0f * DEG);
        d = sdf::smax(d, yEnd - kLipOver - p.y, 0.0010f);
        d = sdf::smax(d, (p.x + 0.004f) * sb - (p.y - yEnd + kLipOver) * cb + 0.0004f, 0.0010f);
        for (int f = 0; f < 4; ++f) d = sdf::smax(d, sock[f] - length(p - mcp[f]), 0.0006f);
        d = sdf::smax(d, cmcBall + kGap - length(p - cmc), 0.0008f);
        return std::min(d, sdf::sphere(p, 0.0148f));   // + wrist mechanism
    }
    // SDF of the part(s) attached to bone b, in its local frame.
    float bone(int b, const vec3& p) const {
        if (b == UpperArmR) return upperArm(p);
        if (b == ForeArmR) return std::min(forearm(p), sdf::sphere(p, fElbowBall));
        if (b == HandR) return palm(p);
        if (b == ThumbR1) return thenar(p);
        int f = (b - ThumbR1) / 3, j = (b - ThumbR1) % 3;
        const Phalanx& P = ph[f][j];
        float d = phalanxShell(P, p);
        float base;
        if (f > 0 && j == 0) base = sdf::sphere(p, P.base0);
        else {
            vec3 hinge = f == 0 ? vec3(1, 0, 0) : vec3(0, 0, 1);
            float a = dot(p, hinge);
            float rho = length(p - hinge * a);
            base = std::max(rho - P.base0, std::fabs(a) - halfLen[f][j]);
        }
        return std::min(d, base);
    }
};
}  // namespace probe
