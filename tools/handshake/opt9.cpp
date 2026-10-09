// Scratch: offline fit of the handshake clasp on the exact meshes (IK-parametrized fingers).
//
// Under the seats' C2 symmetry (180 deg about the vertical through the clasp centre C), the partner's
// hand in our hand frame is our own hand rotated by pi about the hand-local vertical v through the
// clasp anchor c. The clasp, the slide-in and the closing of the fingers (both hands together) are
// then a function of a few plan parameters and the finger poses, evaluated on one hand: our mesh
// vertices against the partner's exact SDFs (the other direction is the same by symmetry).
// Fingers: each long finger's pad is solved (solveLongFinger, DIP coupled) onto a target on the
// partner's back of hand; the thumb pad onto a target near the partner's index knuckle (solveThumb).
//
// Usage: opt2 fit <out.txt> [in.txt|-] [seed] [gens] [sigma]  |  opt2 show <in.txt> [full]
#include "geo.h"

enum {
    P_PHI = 0, P_YAW, P_CX, P_CY, P_CZ, P_LIFT,
    P_F = 6,           // 12: long fingers {spread, MCP, PIP} index..pinky (DIP = 0.62 PIP + offset P_DIP)
    P_DIP = 18,        // DIP offset (all fingers)
    P_TH = 19,         // 4: grip thumb {opposition, CMC, MCP, IP}
    P_OPENT = 23,      // 4: open thumb
    P_OX = 27, P_OY = 28,
    P_PUMP = 29,       // pump amplitude
    NP = 30
};
static const char* kNames[NP] = {"phi", "dyaw", "cx", "cy", "cz", "lift", "gI0", "gI1", "gI2", "gM0", "gM1", "gM2", "gR0", "gR1", "gR2", "gP0",
                                 "gP1", "gP2", "dip", "gT0", "gT1", "gT2", "gT3", "oT0", "oT1", "oT2", "oT3", "ox", "oy", "pump"};
struct Bounds { float lo, hi, scale; };
static Bounds kB[NP];
static bool gFixPump = false;
// opt9 additions (weights from the environment, 0 = term off):
//  W_SKIN  pad-to-skin gap of each long finger's distal phalanx (mesh vertices, pad side) over 1.2 mm
//  W_TSKIN the thumb's distal phalanx skin gap over 2.0 mm
//  W_IPT   grip thumb IP towards 0.9 rad (lying along the back, not hooking)
//  W_BALL  exposed part of the dark thumb CMC ball (fraction of its surface outside palm, thenar, partner)
//  W_SIDE  open thumb sticking out of the palm plane (deg over SIDE_MAX)
//  W_LIFT  elbow lift weight (opt8: 12)
//  STRAIN_T soft strain target (opt8: 0.022), STRAIN_H hard (opt8: 0.032)
//  W_CUFF  the grip's vertices in the partner's forearm cuff (depth over 0.2 mm) with that forearm
//          where the pump extremes put it and turned by +-CUFF_ROT rad about the partner's wrist (the
//          two robots' torsos differ: a left-handed partner's forearm turns ~0.12 rad off its hand's
//          mirror image), so the whole motion stays as clear as the static clasp
//  W_SLIDEV slide length over SLIDE_LEN mm (the hand comes in at 1.51 x length / 0.18 s: 82 mm = 0.69 m/s)
static float envf(const char* k, float d) { const char* e = std::getenv(k); return e ? float(std::atof(e)) : d; }
static float W_KNUCKLE, W_PALM, PALM_T, W_CROSS, CROSS_T, PADS_BEHIND, PADS_WRIST, PADS_KNUCKLE, W_TIDX;
static float W_SKIN, W_TSKIN, W_IPT, W_BALL, W_SIDE, SIDE_MAX, W_LIFT, STRAIN_T, STRAIN_H, IP_T, W_RATE, RATE_MAX, SLIDE_MARGIN, W_CUFF, CUFF_ROT, W_SLIDEV, SLIDE_LEN;
static void initWeights() {
    W_RATE = envf("W_RATE", 0.0f);
    W_CUFF = envf("W_CUFF", 0.0f);
    CUFF_ROT = envf("CUFF_ROT", 0.13f);
    W_SLIDEV = envf("W_SLIDEV", 0.0f);
    SLIDE_LEN = envf("SLIDE_LEN", 82.0f);
    W_CROSS = envf("W_CROSS", 0.04f);
    W_PALM = envf("W_PALM", 6.0f);
    W_KNUCKLE = envf("W_KNUCKLE", 3.0f);
    PALM_T = envf("PALM_T", 1.5f);
    CROSS_T = envf("CROSS_T", 50.0f);
    PADS_BEHIND = envf("PADS_BEHIND", 14.0f);
    PADS_WRIST = envf("PADS_WRIST", 20.0f);
    PADS_KNUCKLE = envf("PADS_KNUCKLE", 5.0f);
    W_TIDX = envf("W_TIDX", 2.0f);
    RATE_MAX = envf("RATE_MAX", 2.0f);
    SLIDE_MARGIN = envf("SLIDE_MARGIN", 0.0f);
    W_SKIN = envf("W_SKIN", 0.0f);
    W_TSKIN = envf("W_TSKIN", 0.0f);
    W_IPT = envf("W_IPT", 0.0f);
    IP_T = envf("IP_T", 0.9f);
    W_BALL = envf("W_BALL", 0.0f);
    W_SIDE = envf("W_SIDE", 0.0f);
    SIDE_MAX = envf("SIDE_MAX", 30.0f);
    W_LIFT = envf("W_LIFT", 12.0f);
    STRAIN_T = envf("STRAIN_T", 0.022f);
    STRAIN_H = envf("STRAIN_H", 0.032f);
}
// Thumb out of the palm plane: angle (deg) of the CMC -> tip vector from the hand's YZ plane (+ = palm side).
static float thumbSideways(const Skeleton& sk, const FingerPose& f) {
    vec3 tip = fingerTip(sk, Side::Right, f, Thumb), c = sk.restOffset[ThumbR1];
    vec3 d = tip - c;
    return std::atan2(d.x, std::sqrt(d.y * d.y + d.z * d.z)) / DEG;
}
// Fraction of the dark CMC ball's surface left uncovered (outside the palm, the thenar shell and arm B).
static float ballExposed(const ArmGeo& G, const ArmFrames& FA, const ArmFrames* FB) {
    static std::vector<vec3> dirs;
    if (dirs.empty()) {
        const int n = 160;
        for (int i = 0; i < n; ++i) {
            float y = 1.0f - 2.0f * (i + 0.5f) / n, r = std::sqrt(std::max(0.0f, 1.0f - y * y)), phi = 2.39996323f * i;
            dirs.push_back(vec3(std::cos(phi) * r, y, std::sin(phi) * r));
        }
    }
    const mat4 palmI = inverseAffine(FA.frame[2]);
    // Only the part of the ball facing the back of the hand (-X) or its top (+Z, the thumb side,
    // up in the handshake) counts: what the cameras (first person, from the side) see.
    int out = 0, n = 0;
    for (const vec3& d : dirs) {
        const vec3 nh = transformDir(palmI * FA.frame[3], d);   // the normal in the hand frame
        if (std::max(-nh.x, nh.z) < 0.3f) continue;
        ++n;
        vec3 p = transformPoint(FA.frame[3], d * 0.0096f);   // the ball (r 9.2 mm) at ThumbR1's origin
        if (G.sdf(2, transformPoint(palmI, p)) < 0.0f) continue;
        if (G.sdf(3, d * 0.0096f) < 0.0f) continue;
        if (FB && armDist(G, *FB, p, true) < 0.0f) continue;
        ++out;
    }
    return n ? float(out) / float(n) : 0.0f;
}
static bool gFree[64];
static void initBounds() {
    auto set = [](int i, float lo, float hi, float sc) { kB[i] = {lo, hi, sc}; };
    set(P_PHI, 0.31f, 0.56f, 0.03f);
    set(P_YAW, -0.9f, 0.9f, 0.08f);
    set(P_CX, -0.005f, 0.04f, 0.002f);
    set(P_CY, -0.17f, 0.02f, 0.006f);
    set(P_CZ, -0.06f, 0.11f, 0.006f);
    set(P_LIFT, 0.0f, 0.85f, 0.06f);
    for (int f = 0; f < 4; ++f) {
        set(P_F + f * 3, -0.30f, 0.30f, 0.04f);
        set(P_F + f * 3 + 1, -0.2f, 1.6f, 0.08f);
        set(P_F + f * 3 + 2, 0.0f, 1.9f, 0.08f);
    }
    set(P_DIP, -0.3f, 0.4f, 0.05f);
    const Bounds thumb[4] = {{-0.3f, 1.5f, 0.1f}, {-0.6f, 1.0f, 0.08f}, {-0.3f, 1.3f, 0.1f}, {-0.3f, 1.3f, 0.1f}};
    for (int j = 0; j < 4; ++j) kB[P_OPENT + j] = kB[P_TH + j] = thumb[j];
    kB[P_TH + 1].lo = -0.40f;
    set(P_OX, 0.02f, 0.09f, 0.006f);
    set(P_OY, -0.09f, 0.03f, 0.008f);
    set(P_PUMP, 0.018f, 0.032f, 0.002f);
}

struct Ctx {
    anim::Animator W;
    anim::Animator::Impl* I = nullptr;
    vec3 C;
    float Cy = 0;
    float yaw0 = 0;
    float pumpMinU = 0, pumpMaxU = 0;   // per unit amplitude
    void init() {
        W.init(robotSkeleton(), vec3(0, layout::PLAYER_PELVIS_Y, layout::PLAYER_PELVIS_Z), 1.0f);
        I = W.impl_.get();
        vec3 cW(0, layout::BOARD_TOP_Y + 0.225f, 0);
        Cy = cW.y;
        C = I->toChar(cW);
        vec3 S = I->shoulderRest(Side::Right);
        vec3 dirH = safeNormalize(vec3(C.x - S.x, 0, C.z - S.z), vec3(0, 0, 1));
        yaw0 = std::atan2(dirH.x, dirH.z);
        for (int k = 0; k <= 2000; ++k) {
            float w = k / 2000.0f, e = std::sin(PI * w);
            float o = e * e * std::sin(TAU * 2.0f * w);
            pumpMinU = std::min(pumpMinU, o);
            pumpMaxU = std::max(pumpMaxU, o);
        }
    }
    float solve(vec3 wrist, quat q, float lift, float* clamps = nullptr, mat4* foreRel = nullptr, mat4* upperRel = nullptr, vec3* fpd = nullptr) {
        Pose tmp;
        I->reachShort = I->wristClamp = I->pronClamp = 0;
        SpineParams sp = I->solveSpine(tmp, wrist, 0.0f, 0.0f, 0.0f, 0.0f);
        I->applySpine(tmp, sp);
        I->fkChain(tmp, Pelvis, Spine2);
        I->solveArm(tmp, Side::Right, wrist, q, lift);
        float soft = softWristStrain(I->lastFlex, I->lastDev, I->lastPron);
        float cl = I->wristClamp + I->pronClamp + I->reachShort * 10.0f;
        if (clamps) *clamps = cl;
        if (fpd) *fpd = vec3(I->lastFlex, I->lastDev, I->lastPron);
        if (foreRel || upperRel) {
            mat4 Hi = inverseAffine(I->G[HandR]);
            if (foreRel) *foreRel = Hi * I->G[ForeArmR];
            if (upperRel) *upperRel = Hi * I->G[UpperArmR];
        }
        return cl + 0.5f * soft;
    }
};

struct Report {
    float cost = 0;
    std::map<std::string, float> terms;
    std::vector<std::string> lines;
    bool verbose = false;
    void add(const char* k, float v) {
        terms[k] += v;
        cost += v;
    }
    void say(const char* fmt, ...) {
        if (!verbose) return;
        char buf[640];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        lines.push_back(buf);
    }
};

static const ArmGeo* gGeo = nullptr;
static float sq(float a) { return a * a; }
static float over(float v, float lim) { return v > lim ? v - lim : 0.0f; }
static float under(float v, float lim) { return v < lim ? lim - v : 0.0f; }

// The back skin of the partner's palm at hand-local (y, z): x < 0 where the palm SDF crosses zero.
static float backX(const ArmGeo& G, float y, float z) {
    float lo = -0.030f, hi = 0.0f;   // outside at lo, inside at hi (mid-plane)
    if (G.exact(2, vec3(hi, y, z)) > 0.0f) return -0.0130f;   // off the palm: nominal
    for (int it = 0; it < 18; ++it) {
        float mid = 0.5f * (lo + hi);
        if (G.exact(2, vec3(mid, y, z)) > 0.0f) lo = mid;
        else hi = mid;
    }
    return 0.5f * (lo + hi);
}

// Own fingers against each other (different fingers) and the curled phalanges against the own palm.
static PenResult selfPen(const ArmGeo& G, const ArmFrames& F, bool dec, float margin) {
    PenResult r;
    auto finger = [](int ai) { return ai < 3 ? -1 : (ai - 3) / 3; };
    auto joint = [](int ai) { return (ai - 3) % 3; };
    for (int a = 3; a < kNArm; ++a)
        for (int b = 2; b < kNArm; ++b) {
            if (b == 2 && (joint(a) == 0 || finger(a) == 0)) continue;   // proximal phalanges and the thumb sit in the palm
            if (b >= 3 && finger(a) == finger(b)) continue;
            if (b == 3 && finger(a) == 1 && joint(a) == 0) continue;     // index base beside the thenar
            vec3 ca = transformPoint(F.frame[a], G.bc[a]), cb = transformPoint(F.frame[b], G.bc[b]);
            if (length(ca - cb) > G.br[a] + G.br[b]) continue;
            mat4 M = inverseAffine(F.frame[b]) * F.frame[a];
            for (const vec3& v : dec ? G.dverts[a] : G.verts[a]) {
                float d = G.sdf(b, transformPoint(M, v));
                if (d < margin) r.soft += sq(margin - d);
                if (-d > r.depth) {
                    r.depth = -d;
                    r.aBone = kArmBones[a];
                    r.bBone = kArmBones[b];
                }
            }
        }
    return r;
}

struct Grip {
    FingerPose f;
    float resid[5] = {};
};
// The grip pose from the joint parameters (DIP coupled to PIP).
static Grip gripFor(const ArmGeo&, const float* x, const mat4&) {
    Grip g;
    for (int f = 1; f <= 4; ++f) {
        g.f.v[f][0] = x[P_F + (f - 1) * 3];
        g.f.v[f][1] = x[P_F + (f - 1) * 3 + 1];
        g.f.v[f][2] = x[P_F + (f - 1) * 3 + 2];
        g.f.v[f][3] = std::max(0.0f, kDipCoupling * g.f.v[f][2] + x[P_DIP]);
    }
    for (int j = 0; j < 4; ++j) g.f.v[0][j] = x[P_TH + j];
    return g;
}
static FingerPose openOf(const float* x) {
    FingerPose p = poseShakeOpen();
    for (int j = 0; j < 4; ++j) p.v[0][j] = x[P_OPENT + j];
    return p;
}

static void evaluate(Ctx& cx, const float* xin, Report& R, bool full = false, Grip* gripOut = nullptr) {
    const ArmGeo& G = *gGeo;
    const Skeleton& sk = robotSkeleton();
    float x[NP];
    for (int i = 0; i < NP; ++i) {
        x[i] = clamp(xin[i], kB[i].lo, kB[i].hi);
        float e = (xin[i] - x[i]) / kB[i].scale;
        if (e != 0.0f) R.add("bounds", 50.0f * e * e);
    }
    if (gFixPump) x[P_PUMP] = 0.032f;
    const float phi = x[P_PHI];
    quat q = handRot(Side::Right, cx.yaw0 + x[P_YAW], phi, PI * 0.5f);
    vec3 v = rotate(conjugate(q), vec3(0, 1, 0));
    vec3 c(x[P_CX], x[P_CY], x[P_CZ]);
    vec3 wrist = cx.C - rotate(q, c);
    const float lift = x[P_LIFT], pumpLo = x[P_PUMP] * cx.pumpMinU, pumpHi = x[P_PUMP] * cx.pumpMaxU;
    // --- arm (strain with the elbow lift the segments use; clamps)
    mat4 foreRel, upperRel;
    float clamps = 0, cl2 = 0, cl3 = 0;
    vec3 fpd;
    float sLift = cx.solve(wrist, q, lift, &clamps, &foreRel, &upperRel, &fpd);
    mat4 foreHi, foreLo;
    float sTop = cx.solve(wrist + vec3(0, pumpHi, 0), q, lift, &cl2, &foreHi), sBot = cx.solve(wrist + vec3(0, pumpLo, 0), q, lift, &cl3, &foreLo);
    float sNo = cx.I->armStrainSide(Side::Right, wrist, q);
    vec3 o(x[P_OX], x[P_OY], 0.0f);
    float sPre = cx.solve(wrist - rotate(q, o), q, lift);
    float worstStrain = std::max({sLift, sTop, sBot, sPre});
    R.add("strain", 1e5f * sq(over(worstStrain, STRAIN_T)) + 4e5f * sq(over(worstStrain, STRAIN_H)) + 2e4f * sq(clamps + cl2 + cl3));
    R.say("arm: strain clasp %.4f pump top %.4f bottom %.4f pre %.4f (no lift %.4f) clamps %.4f | flex %.3f dev %.3f pron %.3f", sLift, sTop, sBot, sPre, sNo,
          clamps + cl2 + cl3, fpd.x, fpd.y, fpd.z);
    // --- poses
    ArmFrames FA, FB;
    mat4 T = c2Transform(v, c);
    Grip gp = gripFor(G, x, T);
    if (gripOut) *gripOut = gp;
    const FingerPose fg = gp.f, fo = openOf(x);
    auto partner = [&](vec3 anchor) {
        mat4 Tt = c2Transform(v, anchor);
        for (int i = 0; i < kNArm; ++i) FB.frame[i] = Tt * FA.frame[i];
    };
    const bool dec = !full;
    float worstDepth = 0;
    std::string worstAt;
    auto pen = [&](const char* label, float wt) {
        PenResult r = penetrate(G, FA, FB, dec, 0.0004f, true);
        float dmm = r.depth * 1000.0f;
        R.add("pen", wt * (300.0f * sq(over(dmm, 0.2f)) + 0.03f * r.soft * 1e6f));
        if (dmm > worstDepth) {
            worstDepth = dmm;
            worstAt = label;
        }
        R.say("  pen %-14s %.2f mm (%s in %s) soft %.2f", label, dmm, r.aBone >= 0 ? boneName(Bone(r.aBone)) : "-", r.bBone >= 0 ? boneName(Bone(r.bBone)) : "-",
              r.soft * 1e6f);
    };
    handFrames(sk, fo, FA, foreRel, upperRel);
    for (float s : {2.5f, 2.25f, 2.0f, 1.8f, 1.6f, 1.45f, 1.3f, 1.15f, 1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f, 0.1f, 0.0f}) {
        partner(c + o * s);
        char lab[32];
        std::snprintf(lab, sizeof lab, "slide %.2f", s);
        pen(lab, 1.0f);
        if (SLIDE_MARGIN > 0.0f && s > 0.05f) {   // opt9: keep a margin while sliding (the real path bends a little)
            PenResult r = penetrate(G, FA, FB, dec, SLIDE_MARGIN, true);
            R.add("slidemargin", 0.03f * r.soft * 1e6f);
        }
    }
    {
        // opt9: how far each finger bone turns (hand-relative) between the open hand and the grip: the
        // fingers close in 0.14 s, a bone turning more than ~2.1 rad would go over 1800 deg/s.
        ArmFrames Fo, Fg;
        handFrames(sk, fo, Fo, foreRel, upperRel);
        handFrames(sk, fg, Fg, foreRel, upperRel);
        float worst = 0;
        int wb = -1;
        for (int ai = 3; ai < kNArm; ++ai) {
            quat a = fromMat3(Fo.frame[ai].upper3()), b = fromMat3(Fg.frame[ai].upper3());
            float ang = 2.0f * std::acos(clamp(std::fabs(dot(a, b)), 0.0f, 1.0f));
            if (ang > worst) { worst = ang; wb = kArmBones[ai]; }
            R.add("rate", W_RATE * sq(over(ang, RATE_MAX)));
        }
        R.say("opt9: open -> grip, largest bone turn %.2f rad (%s)", worst, wb >= 0 ? boneName(Bone(wb)) : "-");
    }
    float selfWorst = 0;
    for (float s : {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f}) {
        handFrames(sk, fpLerp(fo, fg, s), FA, foreRel, upperRel);
        partner(c);
        char lab[32];
        std::snprintf(lab, sizeof lab, s < 1.0f ? "close %.2f" : "grip", s);
        pen(lab, s < 1.0f ? 1.0f : 2.0f);
        PenResult sp = selfPen(G, FA, dec, 0.0003f);
        selfWorst = std::max(selfWorst, sp.depth * 1000.0f);
        R.add("self", 100.0f * sq(over(sp.depth * 1000.0f, 0.3f)) + 0.02f * sp.soft * 1e6f);
        if (s == 1.0f)
            R.say("  self (grip) %.2f mm (%s in %s)", sp.depth * 1000.0f, sp.aBone >= 0 ? boneName(Bone(sp.aBone)) : "-", sp.bBone >= 0 ? boneName(Bone(sp.bBone)) : "-");
    }
    R.add("worstpen", 500.0f * sq(over(worstDepth, 0.35f)));
    if (W_CUFF > 0.0f) {
        // (FA holds the grip, FB the partner at the clasp.) Only the partner's forearm is kept: the
        // other parts are moved out of the way.
        const vec3 wB = FB.frame[2].translation(), axX = normalize(transformDir(FB.frame[2], vec3(1, 0, 0))),
                   axZ = normalize(transformDir(FB.frame[2], vec3(0, 0, 1)));
        float worstCuff = 0.0f, cuffCost = 0.0f;
        for (const mat4* fore : {&foreRel, &foreHi, &foreLo})
            for (int k = 0; k < 5; ++k) {
                ArmFrames FC;
                for (int i = 0; i < kNArm; ++i) FC.frame[i] = translate(vec3(0, 10.0f, 0));
                const mat4 turn = k == 0 ? mat4() : toMat4(axisAngle(k < 3 ? axX : axZ, (k & 1) ? CUFF_ROT : -CUFF_ROT));
                FC.frame[1] = translate(wB) * turn * translate(-wB) * T * *fore;
                const PenResult r = penetrate(G, FA, FC, dec, 0.0f, true);
                worstCuff = std::max(worstCuff, r.depth * 1000.0f);
                cuffCost += sq(over(r.depth * 1000.0f, 0.2f));
            }
        R.add("cuff", W_CUFF * cuffCost);
        R.say("opt10: grip in the partner's forearm cuff (pump extremes, turned +-%.2f rad): worst %.2f mm", CUFF_ROT, worstCuff);
    }
    // --- metrics at the grip (FA holds the grip, FB the partner at the clasp)
    float palmGap = 1e9f;
    {
        mat4 M = inverseAffine(FB.frame[2]);
        for (const vec3& p : (full ? G.verts[2] : G.dverts[2])) {
            if (p.x < 0.004f) continue;
            palmGap = std::min(palmGap, G.sdf(2, transformPoint(M, p)));
        }
    }
    R.add("palm", W_PALM * sq(over(palmGap * 1000.0f, PALM_T)));
    const float cross_ = 2.0f * phi / DEG;
    R.add("cross", W_CROSS * sq(cross_ - CROSS_T));
    R.say("palm gap %.2f mm, crossing %.1f deg, self worst %.2f mm", palmGap * 1000.0f, cross_, selfWorst);
    for (int f = 1; f <= 4; ++f) {
        vec3 pad = fingerPad(sk, Side::Right, fg, f);
        vec3 pl = transformPoint(T, pad);
        float knuckle = character::build::mcpLineY(pl.z);
        float d = armDist(G, FB, pad, true) * 1000.0f;
        {
            // On the back: behind B's mid-plane, between its wrist + 20 mm and its knuckles - 5 mm,
            // touching (middle/ring firmly, index/pinky within a few mm).
            const float padW[5] = {0, 1.0f, 2.0f, 2.0f, 1.0f};
            float behind = -pl.x * 1000.0f, fromWrist = -pl.y * 1000.0f, toKnuckles = (pl.y - knuckle) * 1000.0f;
            R.add("pads", 3.0f * sq(under(behind, PADS_BEHIND)) + 200.0f * sq(under(behind, 11.2f)) + 3.0f * sq(under(fromWrist, PADS_WRIST)) + W_KNUCKLE * sq(under(toKnuckles, PADS_KNUCKLE)) +
                              padW[f] * 3.0f * sq(over(d, 1.8f)));
        }
        vec3 mid = phalanxMid(sk, Side::Right, fg, f, 1);
        float dm = armDist(G, FB, mid, true) * 1000.0f;
        R.add("hug", 0.2f * sq(over(dm, 8.0f)));
        R.say("  pad %d: behind %.1f mm, wrist+%.1f, knuckles-%.1f, gap %.2f mm (ik miss %.2f); middle phalanx %.1f mm | %.2f %.2f %.2f %.2f", f, -pl.x * 1000,
              -pl.y * 1000, (pl.y - knuckle) * 1000, d, gp.resid[f] * 1000, dm, fg.v[f][0], fg.v[f][1], fg.v[f][2], fg.v[f][3]);
    }
    {
        vec3 pad = fingerPad(sk, Side::Right, fg, Thumb);
        vec3 pl = transformPoint(T, pad);
        float d = armDist(G, FB, pad, true) * 1000.0f;
        vec3 idx = transformPoint(T, sk.restOffset[IndexR1]);
        float toIdx = length(pad - idx) * 1000.0f;
        R.add("thumb", 3.0f * sq(over(d, 2.5f)) + W_TIDX * sq(over(toIdx, 22.0f)) + 2.0f * sq(over(pl.x * 1000.0f, -6.0f)));
        vec3 a0 = FA.frame[3].translation(), a1 = FA.frame[4].translation();
        float meta = segSegDist(a0, a1, transformPoint(T, a0), transformPoint(T, a1)) * 1000.0f;
        vec3 web = (a1 + sk.restOffset[IndexR1]) * 0.5f;
        float webs = length(web - transformPoint(T, web)) * 1000.0f;
        R.add("thumbs", 5.0f * sq(under(meta, 23.0f)) + 2.0f * sq(over(webs, 22.0f)));
        R.say("  thumb pad: in B (%+.4f %+.4f %+.4f) gap %.2f mm (ik miss %.2f), %.1f mm from B's index knuckle; metacarpals %.1f mm apart, webs %.1f mm | %.2f %.2f %.2f %.2f",
              pl.x, pl.y, pl.z, d, gp.resid[0] * 1000, toIdx, meta, webs, fg.v[0][0], fg.v[0][1], fg.v[0][2], fg.v[0][3]);
    }
    {
        // opt9: skin gaps (mesh vertices on the pad side of the distal 65% of each distal phalanx to
        // the partner's arm), the thumb lying along the back, the CMC ball, the open thumb's angle.
        float skin[5];
        for (int f = 0; f <= 4; ++f) {
            const int ai = 3 + f * 3 + 2;
            const Bone b = Bone(kArmBones[ai]);
            const vec3 ax = normalize(sk.restOffset[b]);
            const vec3 padDir = f == 0 ? normalize(cross(vec3(1, 0, 0), ax)) : vec3(1, 0, 0);
            const float L = sk.boneLength[b];
            float best = 1e9f;
            for (const vec3& p : (full ? G.verts[ai] : G.dverts[ai])) {
                if (dot(p, padDir) < 0.0f || dot(p, ax) < 0.35f * L) continue;
                best = std::min(best, armDist(G, FB, transformPoint(FA.frame[ai], p), false));
            }
            skin[f] = best * 1000.0f;
        }
        float tProx = 1e9f;   // thumb proximal phalanx (ThumbR2) to the partner: lying on its back
        for (const vec3& p : (full ? G.verts[4] : G.dverts[4])) tProx = std::min(tProx, armDist(G, FB, transformPoint(FA.frame[4], p), false));
        tProx *= 1000.0f;
        float sk4 = 0;
        for (int f = 1; f <= 4; ++f) sk4 += sq(over(skin[f], 1.2f));
        R.add("skin", W_SKIN * sk4);
        R.add("tskin", W_TSKIN * (sq(over(skin[0], 2.0f)) + 0.3f * sq(over(tProx, 4.0f))));
        R.add("ipt", W_IPT * sq(fg.v[0][3] - IP_T));
        const float ball = ballExposed(G, FA, &FB);
        R.add("ball", W_BALL * ball);
        const float side = thumbSideways(sk, fo);
        R.add("side", W_SIDE * sq(over(side, SIDE_MAX)));
        R.say("opt9: skin gaps thumb %.2f index %.2f middle %.2f ring %.2f pinky %.2f mm; thumb proximal %.2f mm; IP %.2f; CMC ball exposed %.0f%%; open thumb out of the palm plane %.1f deg (reach %.1f)",
              skin[0], skin[1], skin[2], skin[3], skin[4], tProx, fg.v[0][3], ball * 100.0f, side, thumbSideways(sk, poseShakeReach()));
    }
    float low = 1e9f;
    for (int ai = 2; ai < kNArm; ++ai)
        for (const vec3& p : G.dverts[ai]) low = std::min(low, dot(v, transformPoint(FA.frame[ai], p) - c));
    low = (cx.Cy + low + pumpLo) * 1000.0f;
    R.add("low", 3.0f * sq(under(low, 906.0f)));
    R.add("pump", 0.2f * sq((0.032f - x[P_PUMP]) * 1000.0f));
    R.say("lowest hand point %.1f mm (pump bottom, amplitude %.1f mm), wrist world y %.4f", low, x[P_PUMP] * 1000.0f, cx.Cy - dot(v, c));
    // Natural shapes: no extreme joints.
    float reg = 0;
    for (int f = 1; f <= 4; ++f) reg += 20.0f * (sq(over(fg.v[f][1], 1.35f)) + sq(over(fg.v[f][2], 1.65f)) + sq(under(fg.v[f][1], -0.05f)));
    const float openThumb0[4] = {0.18f, -0.12f, 0.10f, 0.08f};   // the original open thumb
    for (int j = 0; j < 4; ++j) reg += 1.0f * sq(fo.v[0][j] - openThumb0[j]);
    // Thumb CMC within the range the other presets use (beyond it the dark CMC ball shows).
    reg += 400.0f * (sq(under(fg.v[0][1], -0.35f)) + sq(under(fo.v[0][1], -0.40f)));
    R.add("reg", reg);
    R.add("lift", W_LIFT * sq(lift));
    R.add("slide", 0.02f * sq((o.x - 0.045f) * 1000.0f * 0.1f) + 0.02f * sq(o.y * 1000.0f * 0.1f) + W_SLIDEV * sq(over(length(o) * 1000.0f, SLIDE_LEN)));
    R.say("worst penetration %.2f mm (%s)", worstDepth, worstAt.c_str());
}

#include "cma.inc"

static bool loadParams(const char* path, float* x) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char name[64];
    float val;
    int n = 0;
    while (std::fscanf(f, "%63s %f", name, &val) == 2)
        for (int i = 0; i < NP; ++i)
            if (std::string(name) == kNames[i]) {
                x[i] = val;
                ++n;
            }
    std::fclose(f);
    return n > 0;
}
static void saveParams(const char* path, const float* x, float cost) {
    FILE* f = std::fopen(path, "w");
    for (int i = 0; i < NP; ++i) std::fprintf(f, "%s %.5f\n", kNames[i], x[i]);
    std::fprintf(f, "# cost %.4f\n", cost);
    std::fclose(f);
}
static void defaults(float* x) {
    x[P_PHI] = 0.43f;
    x[P_YAW] = 0.2f;
    x[P_CX] = 0.013f;
    x[P_CY] = -0.050f;
    x[P_CZ] = 0.035f;
    x[P_LIFT] = 0.6f;
    const float fing[12] = {-0.05f, 0.55f, 1.35f, 0.0f, 0.60f, 1.40f, 0.03f, 0.70f, 1.40f, 0.08f, 0.85f, 1.35f};
    for (int i = 0; i < 12; ++i) x[P_F + i] = fing[i];
    x[P_DIP] = 0.0f;
    const float th[4] = {1.0f, 0.0f, 0.6f, 0.5f};
    for (int j = 0; j < 4; ++j) x[P_TH + j] = th[j];
    for (int j = 0; j < 4; ++j) x[P_OPENT + j] = poseShakeOpen().v[0][j];
    x[P_OX] = 0.045f;
    x[P_OY] = -0.03f;
    x[P_PUMP] = 0.032f;
}
static void show(Ctx& cx, const float* x, bool full) {
    Report R;
    R.verbose = true;
    Grip g;
    evaluate(cx, x, R, full, &g);
    for (auto& l : R.lines) std::printf("%s\n", l.c_str());
    std::printf("cost %.4f:", R.cost);
    for (auto& kv : R.terms) std::printf(" %s %.3f", kv.first.c_str(), kv.second);
    std::printf("\nparams:");
    for (int i = 0; i < NP; ++i) std::printf(" %s=%.4f", kNames[i], x[i]);
    std::printf("\ngrip pose:");
    for (int f = 0; f < 5; ++f) std::printf(" {%.3f, %.3f, %.3f, %.3f}", g.f.v[f][0], g.f.v[f][1], g.f.v[f][2], g.f.v[f][3]);
    std::printf("\n");
}

int main(int argc, char** argv) {
    initBounds();
    static ArmGeo G;
    G.init();
    gGeo = &G;
    if (std::getenv("FIXPUMP")) gFixPump = true;
    initWeights();
    if (const char* bs = std::getenv("BOUNDS")) {   // "name:lo:hi,name:lo:hi"
        std::string s = bs;
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find(',', p);
            if (e == std::string::npos) e = s.size();
            std::string item = s.substr(p, e - p);
            char nm[64];
            float lo, hi;
            if (std::sscanf(item.c_str(), "%63[^:]:%f:%f", nm, &lo, &hi) == 3)
                for (int i = 0; i < NP; ++i)
                    if (std::string(nm) == kNames[i]) { kB[i].lo = lo; kB[i].hi = hi; std::printf("bounds %s [%.3f, %.3f]\n", nm, lo, hi); }
            p = e + 1;
        }
    }
    std::string mode = argc > 1 ? argv[1] : "show";
    float x0[NP];
    defaults(x0);
    if (mode == "selfbase") {
        const Skeleton& sk = robotSkeleton();
        const FingerPose* poses[] = {&poseShakeOpen(), &poseShakeGrip(), &poseRelaxed(), &poseLooseFist(), &poseTableRest()};
        const char* names[] = {"open", "grip", "relaxed", "fist", "tablerest"};
        for (int i = 0; i < 5; ++i) {
            ArmFrames F;
            handFrames(sk, *poses[i], F, mat4(), mat4());
            G.useGrid = false;
            PenResult r = selfPen(G, F, false, 0.0f);
            std::printf("%-10s self %.2f mm (%s in %s)\n", names[i], r.depth * 1000, r.aBone >= 0 ? boneName(Bone(r.aBone)) : "-", r.bBone >= 0 ? boneName(Bone(r.bBone)) : "-");
        }
        FingerPose z;
        ArmFrames F;
        handFrames(sk, z, F, mat4(), mat4());
        PenResult r = selfPen(G, F, false, 0.0f);
        std::printf("%-10s self %.2f mm (%s in %s)\n", "zero", r.depth * 1000, r.aBone >= 0 ? boneName(Bone(r.aBone)) : "-", r.bBone >= 0 ? boneName(Bone(r.bBone)) : "-");
        return 0;
    }
    if (mode == "map") {
        // ASCII section of the partner's arm in each of our finger planes (spread 0): rows = x (palm
        // normal, towards the partner), columns = along the finger from its knuckle.
        if (argc > 2) loadParams(argv[2], x0);
        Ctx cx;
        cx.init();
        const Skeleton& sk = robotSkeleton();
        quat q = handRot(Side::Right, cx.yaw0 + x0[P_YAW], x0[P_PHI], PI * 0.5f);
        vec3 v = rotate(conjugate(q), vec3(0, 1, 0)), c(x0[P_CX], x0[P_CY], x0[P_CZ]);
        mat4 T = c2Transform(v, c);
        mat4 fore, up;
        cx.solve(cx.C - rotate(q, c), q, x0[P_LIFT], nullptr, &fore, &up);
        ArmFrames FA, FB;
        handFrames(sk, openOf(x0), FA, fore, up);
        for (int i = 0; i < kNArm; ++i) FB.frame[i] = T * FA.frame[i];
        G.useGrid = false;
        for (int f = 1; f <= 4; ++f) {
            vec3 mcp = sk.restOffset[Bone(IndexR1 + (f - 1) * 3)];
            std::printf("finger %d (knuckle z %.3f): x from +0.065 (top) to -0.015; along 0..0.11 m (2 mm/char); B parts: P palm T thenar t thumb f fingers F forearm\n", f, mcp.z);
            for (float xx = 0.065f; xx >= -0.0151f; xx -= 0.0025f) {
                std::printf("%+.4f ", xx);
                for (float a = 0.0f; a <= 0.11f; a += 0.002f) {
                    vec3 p(xx, mcp.y - a, mcp.z);
                    int which = -1;
                    float d = armDist(G, FB, p, false, &which);
                    char ch = '.';
                    if (d < 0) ch = which == HandR ? 'P' : which == ThumbR1 ? 'T' : (which == ThumbR2 || which == ThumbR3) ? 't' : which == ForeArmR ? 'F' : 'f';
                    else if (d < 0.002f) ch = ':';
                    std::printf("%c", ch);
                }
                std::printf("\n");
            }
        }
        return 0;
    }
    if (mode == "proj") {
        // The partner's arm projected onto our palm plane (looking from the partner's side along -X):
        // rows = our z (thumb side up), columns = our y (wrist left, fingertips right), 2.5 mm cells.
        // B parts (first hit along x in [-0.01, 0.07]): P palm T thenar t thumb f fingers F forearm;
        // our knuckles '*', our open fingers '-' (where nothing of B is).
        if (argc > 2) loadParams(argv[2], x0);
        Ctx cx;
        cx.init();
        const Skeleton& sk = robotSkeleton();
        quat q = handRot(Side::Right, cx.yaw0 + x0[P_YAW], x0[P_PHI], PI * 0.5f);
        vec3 v = rotate(conjugate(q), vec3(0, 1, 0)), c(x0[P_CX], x0[P_CY], x0[P_CZ]);
        mat4 T = c2Transform(v, c);
        mat4 fore, up;
        cx.solve(cx.C - rotate(q, c), q, x0[P_LIFT], nullptr, &fore, &up);
        ArmFrames FA, FB;
        handFrames(sk, openOf(x0), FA, fore, up);
        for (int i = 0; i < kNArm; ++i) FB.frame[i] = T * FA.frame[i];
        G.useGrid = true;
        std::printf("columns: our y from +0.030 (left) to -0.200 (right), 2.5 mm; rows: our z from +0.110 down to -0.080\n");
        std::printf("world up in our frame (%.3f %.3f %.3f); anchor (%.3f %.3f %.3f)\n", v.x, v.y, v.z, c.x, c.y, c.z);
        for (float z = 0.110f; z >= -0.0801f; z -= 0.0025f) {
            std::printf("%+.4f ", z);
            for (float y = 0.030f; y >= -0.2001f; y -= 0.0025f) {
                char ch = '.';
                float bestX = 1e9f;
                for (float xx = -0.010f; xx <= 0.07f; xx += 0.0015f) {
                    int which = -1;
                    float d = armDist(G, FB, vec3(xx, y, z), false, &which);
                    if (d < 0) {
                        ch = which == HandR ? 'P' : which == ThumbR1 ? 'T' : (which == ThumbR2 || which == ThumbR3) ? 't' : which == ForeArmR ? 'F' : which == UpperArmR ? 'U' : 'f';
                        bestX = xx;
                        break;
                    }
                }
                bool knuckle = false, finger = false;
                for (int f = 1; f <= 4; ++f) {
                    vec3 m = sk.restOffset[Bone(IndexR1 + (f - 1) * 3)];
                    if (std::fabs(z - m.z) < 0.0013f && std::fabs(y - m.y) < 0.0013f) knuckle = true;
                    if (std::fabs(z - m.z) < 0.0013f && y < m.y && y > m.y - 0.09f) finger = true;
                }
                if (knuckle) ch = '*';
                else if (ch == '.' && finger) ch = '-';
                (void)bestX;
                std::printf("%c", ch);
            }
            std::printf("\n");
        }
        return 0;
    }
    if (mode == "liftscan") {
        if (argc > 2) loadParams(argv[2], x0);
        Ctx cx;
        cx.init();
        quat q = handRot(Side::Right, cx.yaw0 + x0[P_YAW], x0[P_PHI], PI * 0.5f);
        vec3 c(x0[P_CX], x0[P_CY], x0[P_CZ]);
        vec3 wrist = cx.C - rotate(q, c);
        for (float lift = 0.0f; lift <= 0.951f; lift += 0.1f) {
            float cl;
            vec3 fpd;
            float s0 = cx.solve(wrist, q, lift, &cl, nullptr, nullptr, &fpd);
            float st = cx.solve(wrist + vec3(0, x0[P_PUMP] * cx.pumpMaxU, 0), q, lift);
            float sb = cx.solve(wrist + vec3(0, x0[P_PUMP] * cx.pumpMinU, 0), q, lift);
            std::printf("lift %.2f: strain clasp %.3f top %.3f bottom %.3f clamps %.3f | flex %.2f dev %.2f pron %.2f\n", lift, s0, st, sb, cl, fpd.x, fpd.y, fpd.z);
        }
        return 0;
    }
    if (mode == "thumbscan") {
        // Collision-free grip thumbs (CMC >= -0.35) at the fitted clasp, best by the thumb costs.
        if (argc > 2) loadParams(argv[2], x0);
        Ctx cx;
        cx.init();
        const Skeleton& sk = robotSkeleton();
        quat q = handRot(Side::Right, cx.yaw0 + x0[P_YAW], x0[P_PHI], PI * 0.5f);
        vec3 v = rotate(conjugate(q), vec3(0, 1, 0)), c(x0[P_CX], x0[P_CY], x0[P_CZ]);
        mat4 T = c2Transform(v, c);
        mat4 fore, up;
        cx.solve(cx.C - rotate(q, c), q, x0[P_LIFT], nullptr, &fore, &up);
        Grip g0 = gripFor(G, x0, T);
        struct Cand { float score, depth, gap, toIdx, meta, webs, x; float t[4]; };
        std::vector<Cand> cands;
        for (float opp = 0.2f; opp <= 1.51f; opp += 0.1f)
            for (float cmc = -0.35f; cmc <= 0.81f; cmc += 0.1f)
                for (float mcp = -0.2f; mcp <= 1.31f; mcp += 0.15f)
                    for (float ip = -0.2f; ip <= 1.31f; ip += 0.15f) {
                        FingerPose f = g0.f;
                        f.v[0][0] = opp; f.v[0][1] = cmc; f.v[0][2] = mcp; f.v[0][3] = ip;
                        ArmFrames FA, FB;
                        handFrames(sk, f, FA, fore, up);
                        for (int i = 0; i < kNArm; ++i) FB.frame[i] = T * FA.frame[i];
                        // Only the thumb bones of both hands matter here: A's thumb in B, B's thumb in A
                        // (same by symmetry), and the thumb against its own fingers.
                        ArmFrames FAt = FA;
                        PenResult r = penetrate(G, FA, FB, true, 0.0f, true);
                        PenResult sp = selfPen(G, FA, true, 0.0f);
                        float depth = std::max(r.depth, sp.depth);
                        if (depth > 0.0006f) continue;
                        vec3 pad = fingerPad(sk, Side::Right, f, Thumb);
                        vec3 pl = transformPoint(T, pad);
                        float gap = armDist(G, FB, pad, true);
                        vec3 idx = transformPoint(T, sk.restOffset[IndexR1]);
                        float toIdx = length(pad - idx);
                        vec3 a0 = FA.frame[3].translation(), a1 = FA.frame[4].translation();
                        float meta = segSegDist(a0, a1, transformPoint(T, a0), transformPoint(T, a1));
                        vec3 web = (a1 + sk.restOffset[IndexR1]) * 0.5f;
                        float webs = length(web - transformPoint(T, web));
                        float score = sq(over(gap * 1000, 3.0f)) + sq(over(toIdx * 1000, 22.0f)) + sq(over(pl.x * 1000, -6.0f)) +
                                      sq(under(meta * 1000, 23.0f)) + sq(over(webs * 1000, 22.0f));
                        cands.push_back({score, depth, gap, toIdx, meta, webs, pl.x, {opp, cmc, mcp, ip}});
                    }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
        std::printf("%zu collision-free thumbs\n", cands.size());
        for (size_t i = 0; i < std::min<size_t>(15, cands.size()); ++i) {
            const Cand& k = cands[i];
            std::printf("score %.1f: {%.2f %.2f %.2f %.2f} depth %.2f gap %.1f mm toIdx %.1f mm behind %.1f meta %.1f webs %.1f\n", k.score, k.t[0], k.t[1], k.t[2], k.t[3],
                        k.depth * 1000, k.gap * 1000, k.toIdx * 1000, -k.x * 1000, k.meta * 1000, k.webs * 1000);
        }
        return 0;
    }
    if (mode == "heightscan") {
        // Strain against the hand's height (the anchor moved along the hand-local vertical) and the lift.
        if (argc > 2) loadParams(argv[2], x0);
        Ctx cx;
        cx.init();
        quat q = handRot(Side::Right, cx.yaw0 + x0[P_YAW], x0[P_PHI], PI * 0.5f);
        vec3 v = rotate(conjugate(q), vec3(0, 1, 0)), c(x0[P_CX], x0[P_CY], x0[P_CZ]);
        vec3 o(x0[P_OX], x0[P_OY], 0.0f);
        for (float dh = -0.04f; dh <= 0.021f; dh += 0.01f) {
            std::printf("dh %+.2f:", dh);
            for (float lift = 0.5f; lift <= 0.951f; lift += 0.15f) {
                vec3 cc = c - v * dh;   // the wrist dh higher (world)
                vec3 wrist = cx.C - rotate(q, cc);
                float s0 = cx.solve(wrist, q, lift);
                float st = cx.solve(wrist + vec3(0, x0[P_PUMP] * cx.pumpMaxU, 0), q, lift);
                float sp = cx.solve(wrist - rotate(q, o), q, lift);
                std::printf("  L%.2f: %.3f/%.3f/%.3f", lift, s0, st, sp);
            }
            std::printf("\n");
        }
        return 0;
    }
    if (mode == "show") {
        if (argc > 2) loadParams(argv[2], x0);
        Ctx cx;
        cx.init();
        show(cx, x0, argc > 3);
        return 0;
    }
    const char* out = argc > 2 ? argv[2] : "fit.txt";
    if (argc > 3 && std::string(argv[3]) != "-") loadParams(argv[3], x0);
    unsigned seed = argc > 4 ? unsigned(std::atoi(argv[4])) : 1u;
    int gens = argc > 5 ? std::atoi(argv[5]) : 600;
    double sigma0 = argc > 6 ? std::atof(argv[6]) : 1.0;
    const int nThreads = std::getenv("NTHREADS") ? std::atoi(std::getenv("NTHREADS")) : 3;
    std::vector<std::unique_ptr<Ctx>> ctx;
    for (int t = 0; t < nThreads; ++t) {
        ctx.emplace_back(new Ctx);
        ctx.back()->init();
    }
    // FREE="name,name,..." fits only those parameters (the others stay at x0).
    std::vector<int> fi;
    {
        const char* fr = std::getenv("FREE");
        for (int i = 0; i < NP; ++i) {
            bool on = !fr;
            if (fr) {
                std::string s = std::string(",") + fr + ",";
                on = s.find(std::string(",") + kNames[i] + ",") != std::string::npos;
            }
            if (on) fi.push_back(i);
        }
        std::printf("fitting %zu parameters\n", fi.size());
    }
    const int NF = int(fi.size());
    Cma es(NF, sigma0);
    std::mt19937 rng(seed);
    float best[NP];
    std::copy(x0, x0 + NP, best);
    float bestCost;
    {
        Report R;
        evaluate(*ctx[0], x0, R);
        bestCost = R.cost;
        std::printf("start cost %.4f\n", bestCost);
    }
    auto toX = [&](const std::vector<double>& z, float* x) {
        for (int i = 0; i < NP; ++i) x[i] = x0[i];
        for (int k = 0; k < NF; ++k) x[fi[k]] = x0[fi[k]] + float(z[k]) * kB[fi[k]].scale;
    };
    auto t0 = std::chrono::steady_clock::now();
    for (int gen = 0; gen < gens; ++gen) {
        std::vector<std::vector<double>> Z, Y;
        es.sample(rng, Z, Y);
        std::vector<double> cost(es.lambda);
        std::vector<std::thread> th;
        for (int t = 0; t < nThreads; ++t)
            th.emplace_back([&, t] {
                for (int k = t; k < es.lambda; k += nThreads) {
                    std::vector<double> z(NF);
                    for (int i = 0; i < NF; ++i) z[i] = es.m[i] + es.sigma * Y[k][i];
                    float x[NP];
                    toX(z, x);
                    Report R;
                    evaluate(*ctx[t], x, R);
                    cost[k] = R.cost;
                }
            });
        for (auto& x : th) x.join();
        std::vector<int> order(es.lambda);
        for (int k = 0; k < es.lambda; ++k) order[k] = k;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return cost[a] < cost[b]; });
        if (cost[order[0]] < bestCost) {
            bestCost = float(cost[order[0]]);
            std::vector<double> z(NF);
            for (int i = 0; i < NF; ++i) z[i] = es.m[i] + es.sigma * Y[order[0]][i];
            toX(z, best);
            for (int i = 0; i < NP; ++i) best[i] = clamp(best[i], kB[i].lo, kB[i].hi);
            saveParams(out, best, bestCost);
        }
        es.update(Y, Z, order, gen);
        if (gen % 25 == 0 || gen == gens - 1) {
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("gen %d best %.4f gen-best %.4f sigma %.4f (%.0f s)\n", gen, bestCost, cost[order[0]], es.sigma, el);
            std::fflush(stdout);
        }
        if (es.sigma < 1e-4) break;
    }
    show(*ctx[0], best, false);
    return 0;
}
