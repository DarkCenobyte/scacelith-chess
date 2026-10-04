// Scratch: runs the real two-robot handshake (anim sources of the worktree) and measures, every
// 1/120 s of the whole task, how deep each robot's right arm mesh vertices (upper arm, forearm,
// palm, thenar, phalanges) go inside the other's right arm (exact SDFs), plus the clasp metrics.
// Usage: pen [rr|rl|lr|rlpen] [exact] [verbose]
#include "geo.h"

struct Cfg {
    bool wLeft = false, bLeft = false, bPen = false, cut = false;
};

// Phase boundaries (PHASES="t1,t2,tc,tp,to,tw", seconds of the default 2.60 s handshake).
static float gPh[6] = {0.60f, 0.78f, 0.92f, 1.90f, 2.08f, 2.18f};
static float cutAt = 1.2f;
static const char* phaseName(float u, float scale) {
    float s = u / scale;
    if (s < 0) return "pre";
    if (s < gPh[0]) return "1approach";
    if (s < gPh[1]) return "2slide-in";
    if (s < gPh[2]) return "3close";
    if (s < gPh[3]) return "4pumps";
    if (s < gPh[4]) return "5open";
    if (s < gPh[5]) return "6withdraw";
    return "7retract";
}
// Skin gap of a bone's mesh (vertices on the side 'side' in bone-local x when side != 0) to arm B.
static float skinGap(const ArmGeo& G, const ArmFrames& FA, int ai, const ArmFrames& FB, float side = 0.0f, int* which = nullptr) {
    float best = 1e9f;
    for (const vec3& v : G.verts[ai]) {
        if (side != 0.0f && v.x * side < 0.0f) continue;
        int w = -1;
        float d = armDist(G, FB, transformPoint(FA.frame[ai], v), false, &w);
        if (d < best) { best = d; if (which) *which = w; }
    }
    return best;
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "rr";
    if (const char* e = std::getenv("PHASES")) std::sscanf(e, "%f,%f,%f,%f,%f,%f", &gPh[0], &gPh[1], &gPh[2], &gPh[3], &gPh[4], &gPh[5]);
    if (const char* e = std::getenv("CUTAT")) cutAt = std::atof(e);
    bool exact = false, verbose = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "exact") exact = true;
        if (std::string(argv[i]) == "verbose") verbose = true;
    }
    Cfg cfg;
    if (mode == "rl") cfg.bLeft = true;
    if (mode == "lr") cfg.wLeft = true;
    if (mode == "rlpen") cfg.bLeft = cfg.bPen = true;
    if (mode == "cutrr") cfg.cut = true;
    if (mode == "cutrl") cfg.cut = cfg.bLeft = true;
    if (mode == "cutlr") cfg.cut = cfg.wLeft = true;
    bool cutDone = false;
    const Skeleton& sk = robotSkeleton();
    static ArmGeo G;
    G.init();
    G.useGrid = !exact;
    anim::Animator W, B;
    W.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, layout::PLAYER_PELVIS_Z), 1.0f, cfg.wLeft ? Side::Left : Side::Right);
    B.init(sk, vec3(0, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z), -1.0f, cfg.bLeft ? Side::Left : Side::Right);
    W.setRestHand(vec3(layout::REST_HAND_X, layout::TABLE_TOP_Y, layout::REST_HAND_Z));
    std::vector<anim::Event> ev;
    const float dt = 1.0f / 120.0f;
    float start = 0.3f;
    if (cfg.bPen) {
        // Black (left-handed) picks its pen up first, then the game ends: PutPen + handshake.
        const mat4 taken = rotateY(3.1415927f) * translate(vec3(-(layout::SCORESHEET_X + layout::SCORESHEET_WIDTH * 0.5f + 0.030f) * -1.0f,
                                                                layout::TABLE_TOP_Y + layout::PEN_RADIUS,
                                                                layout::SCORESHEET_Z - layout::PEN_LENGTH * 0.5f)) *
                           toMat4(fromTo(vec3(0, 1, 0), vec3(0, 0, 1)), vec3(0));
        anim::WriteTask pick;
        pick.type = anim::WriteTaskType::PickPen;
        pick.frame = taken;
        B.enqueueWriting(pick);
        start = 0.8f;
    }
    bool started = false;
    float worst = 0, worstT = 0;
    std::string worstWhat;
    std::map<std::string, float> phaseWorst;
    float minLow = 1e9f, minLowT = 0, minFore = 1e9f, minForeT = 0;
    float maxStrain0 = 0, maxClampC = 0;
    int pops = 0;
    float maxStrain = 0, maxStrainT = 0, maxClamp = 0, maxClampT = 0, maxSpeed = 0, maxSpeedT = 0;
    std::string maxSpeedWhat;
    int maxSpeedBone = 0;
    quat prev[2][BoneCount];
    vec3 prevP[2];
    vec3 prevV[2];
    bool haveP = false;
    float peakIn = 0, peakInT = 0, peakOut = 0, peakOutT = 0, maxAccRel = 0, maxAccRelT = 0;
    std::vector<std::pair<float, float>> prof;
    bool havePrev = false;
    const float T = anim::Timing::Handshake;
    while (W.time() < start + T + 0.1f) {
        if (!started && W.time() >= start - 1e-4f) {
            if (cfg.bPen) {
                anim::WriteTask put;
                put.type = anim::WriteTaskType::PutPen;
                const mat4 taken = rotateY(3.1415927f) * translate(vec3((layout::SCORESHEET_X + layout::SCORESHEET_WIDTH * 0.5f + 0.030f),
                                                                        layout::TABLE_TOP_Y + layout::PEN_RADIUS,
                                                                        layout::SCORESHEET_Z - layout::PEN_LENGTH * 0.5f)) *
                                   toMat4(fromTo(vec3(0, 1, 0), vec3(0, 0, 1)), vec3(0));
                put.frame = translate(vec3(0, 0, -0.03f)) * taken;
                B.enqueueWriting(put);
            }
            anim::Task h;
            h.type = anim::TaskType::Handshake;
            h.partner = &B;
            W.enqueue(h);
            h.partner = &W;
            B.enqueue(h);
            started = true;
        }
        if (cfg.cut && started && !cutDone && W.time() - start >= cutAt - 1e-4f) {   // the online cut (as the cancel test)
            B.cancelTasks();
            anim::Task back;
            back.type = anim::TaskType::Retract;
            B.enqueue(back);
            cutDone = true;
        }
        ev.clear();
        W.update(dt, ev);
        for (auto& e : ev)
            if (verbose && e.type != anim::EventType::TaskStarted) std::printf("  W event %d at %.4f\n", int(e.type), e.time);
        ev.clear();
        B.update(dt, ev);
        for (auto& e : ev)
            if (verbose && e.type != anim::EventType::TaskStarted) std::printf("  B event %d at %.4f\n", int(e.type), e.time);
        const float t = W.time(), u = t - start;
        if (!started) continue;
        ArmFrames FW, FB;
        for (int ai = 0; ai < kNArm; ++ai) {
            FW.frame[ai] = W.globals()[kArmBones[ai]];
            FB.frame[ai] = B.globals()[kArmBones[ai]];
        }
        // Angular speed of every bone (both robots).
        anim::Animator* an[2] = {&W, &B};
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < BoneCount; ++b) {
                if (b == EyeL || b == EyeR || (b >= LidUpperL && b <= LidLowerR)) continue;
                quat q = fromMat3(an[a]->globals()[b].upper3());
                if (havePrev) {
                    float ang = 2.0f * std::acos(clamp(std::fabs(dot(q, prev[a][b])), 0.0f, 1.0f)) / dt / DEG;
                    if (ang > 1800.0f && pops++ < 12) std::printf("  pop %s %s %.0f deg/s at u=%.3f\n", a ? "B" : "W", boneName(Bone(b)), ang, u);
                    if (ang > maxSpeed) {
                        maxSpeed = ang;
                        maxSpeedT = u;
                        maxSpeedBone = b;
                        maxSpeedWhat = a ? "B" : "W";
                    }
                }
                prev[a][b] = q;
            }
        havePrev = true;
        {
            // Hand speed (a palm point) of both robots; the release's acceleration.
            vec3 P[2] = {transformPoint(W.globals()[HandR], vec3(0, -0.05f, 0)), transformPoint(B.globals()[HandR], vec3(0, -0.05f, 0))};
            if (haveP) {
                for (int a = 0; a < 2; ++a) {
                    vec3 v = (P[a] - prevP[a]) / dt;
                    float sp = length(v);
                    if (a == 0) {
                        if (u > gPh[1] - 0.30f && u < gPh[1] + 0.01f && sp > peakIn) { peakIn = sp; peakInT = u; }
                        if (u > gPh[4] - 0.02f && u < T && sp > peakOut) { peakOut = sp; peakOutT = u; }
                        int k = int(std::lround(u * 120));
                        if (k % 3 == 0 && ((u > 0.0f && u < gPh[2]) || (u > gPh[3] && u < T + 0.05f))) prof.push_back({u, sp});
                    }
                    if (cfg.cut && u > cutAt) {
                        static float best[2] = {0, 0};
                        if (sp > best[a]) {
                            best[a] = sp;
                            std::printf("  cut: %s hand speed %.2f m/s at u=%.3f\n", a ? "B" : "W", sp, u);
                        }
                    }
                    if (u > gPh[4] - 0.02f && u < T + 0.05f) {
                        float acc = length(v - prevV[a]) / dt;
                        if (acc > maxAccRel) { maxAccRel = acc; maxAccRelT = u; }
                    }
                    prevV[a] = v;
                }
            }
            prevP[0] = P[0];
            prevP[1] = P[1];
            haveP = true;
        }
        if (u < 0) continue;
        PenResult r1 = penetrate(G, FW, FB, false, 0.0f, false);   // W's vertices in B
        PenResult r2 = penetrate(G, FB, FW, false, 0.0f, false);   // B's vertices in W
        float d = std::max(r1.depth, r2.depth);
        const char* ph = phaseName(u, 1.0f);
        phaseWorst[ph] = std::max(phaseWorst[ph], d);
        char what[128];
        std::snprintf(what, sizeof what, "%s", r1.depth >= r2.depth ? (r1.aBone >= 0 ? (std::string("W.") + boneName(Bone(r1.aBone)) + " in B." + boneName(Bone(r1.bBone))).c_str() : "-")
                                                                    : (std::string("B.") + boneName(Bone(r2.aBone)) + " in W." + boneName(Bone(r2.bBone))).c_str());
        if (d > worst) {
            worst = d;
            worstT = u;
            worstWhat = what;
        }
        if (verbose || (int(std::lround(u / dt)) % 6 == 0))
            if (verbose || d > 0.0005f) std::printf("u=%.3f %-9s depth %5.2f mm  %s (nIn %d/%d)\n", u, ph, d * 1000, what, r1.nIn, r2.nIn);
        // Lowest hand point / forearm over the board (both robots), world.
        for (int a = 0; a < 2; ++a) {
            const ArmFrames& F = a ? FB : FW;
            for (int ai = 1; ai < kNArm; ++ai)
                for (const vec3& v : G.dverts[ai]) {
                    vec3 p = transformPoint(F.frame[ai], v);
                    if (ai >= 2) {
                        if (u > gPh[2] && u < gPh[3] && p.y < minLow) { minLow = p.y; minLowT = u; }
                    } else if (std::fabs(p.x) < 0.22f && std::fabs(p.z) < 0.22f && u > 0.5f && u < 2.3f) {
                        if (p.y < minFore) { minFore = p.y; minForeT = u; }
                    }
                }
        }
        // Strain and clamps of each shaking arm (planned sample; then the real solve).
        for (int a = 0; a < 2; ++a) {
            auto& I = *an[a]->impl_;
            if (!I.shakeHand().motion.segs.empty() && u > 0.0f && u < T) {
                HandSample s = I.shakeHand().motion.sample(I.time);
#ifdef ORIG
                float st = I.armStrainSide(I.shakeSide(), s.p, s.q);
#else
                float st = I.armStrainSide(I.shakeSide(), s.p, s.q, s.elbow);
                float st0 = I.armStrainSide(I.shakeSide(), s.p, s.q);
                if (u > gPh[0] && u < gPh[5]) maxStrain0 = std::max(maxStrain0, st0);
#endif
                if (u > gPh[0] && u < gPh[5] && st > maxStrain) { maxStrain = st; maxStrainT = u; }
                Pose tmp;
                mat4 Wm[BoneCount];
                const Side keep = I.diagSide;
                I.diagSide = I.shakeSide();
                I.evaluate(I.time, tmp, Wm);
                float cl = I.wristClamp + I.pronClamp + I.reachShort;
                if (cl > maxClamp) { maxClamp = cl; maxClampT = u; }
                if (u > gPh[0] && u < gPh[5]) maxClampC = std::max(maxClampC, cl);
                static const int traceRobot = std::getenv("TRACE_ROBOT") ? std::atoi(std::getenv("TRACE_ROBOT")) : -1;
                if ((verbose && a == 0 && int(std::lround(u / dt)) % 12 == 0) || (a == traceRobot && u > 0.25f && u < 0.75f))
                    std::printf("     %s u=%.3f strain %.3f flex %.3f dev %.3f pron %.3f elbow %.2f clamp %.3f\n", a ? "B" : "W", u, st, I.lastFlex, I.lastDev, I.lastPron, s.elbow, cl);
                I.diagSide = keep;
                I.evaluate(I.time, tmp, Wm);
            }
        }
        // Clasp metrics at a few instants.
        for (float at : {0.93f, 1.20f, 1.42f, 1.80f, 1.95f}) {
            if (std::fabs(u - at) > dt * 0.5f) continue;
            std::printf("-- metrics u=%.3f --\n", u);
            for (int a = 0; a < 2; ++a) {
                const mat4* gA = an[a]->globals();
                const mat4* gB = an[1 - a]->globals();
                const ArmFrames& FBo = a ? FW : FB;
                mat4 HB = gB[HandR], HBi = inverseAffine(HB);
                // Crossing angle in the palm plane.
                vec3 nA = normalize(transformDir(gA[HandR], vec3(1, 0, 0)));
                vec3 fa = -transformDir(gA[HandR], vec3(0, 1, 0)), fb = -transformDir(gB[HandR], vec3(0, 1, 0));
                fa = normalize(fa - nA * dot(fa, nA));
                fb = normalize(fb - nA * dot(fb, nA));
                float cross_ = std::acos(clamp(std::fabs(dot(fa, fb)), 0.0f, 1.0f)) / DEG;
                // Palm gap: A's palm vertices to B's palm.
                float palmGap = 1e9f;
                for (const vec3& v : G.verts[2]) palmGap = std::min(palmGap, G.exact(2, transformPoint(HBi * gA[HandR], v)));
                std::printf("  %s: crossing %.1f deg, palm gap %.2f mm, nA.nB %.3f\n", a ? "B" : "W", cross_, palmGap * 1000,
                            dot(nA, normalize(transformDir(gB[HandR], vec3(1, 0, 0)))));
                // Long finger pads in B's hand frame.
                for (int f = 1; f <= 4; ++f) {
                    Bone b3 = Bone(ThumbR1 + f * 3 + 2);
                    vec3 pad = transformPoint(gA[b3], vec3(0, -1, 0) * (sk.boneLength[b3] * 0.72f) + vec3(kPadRadius, 0, 0));
                    vec3 pl = transformPoint(HBi, pad);
                    int which = -1;
                    float dist = armDist(G, FBo, pad, false, &which);
                    float knuckle = character::build::mcpLineY(pl.z);
                    std::printf("    pad %d: in partner hand (%+.4f %+.4f %+.4f) behind mid-plane %.1f mm, along: wrist+%.1f mm, knuckles %.1f mm; gap %.2f mm to %s\n",
                                f, pl.x, pl.y, pl.z, -pl.x * 1000, -pl.y * 1000, (pl.y - knuckle) * 1000, dist * 1000, which >= 0 ? boneName(Bone(which)) : "-");
                }
                {
                    vec3 d3 = normalize(sk.restOffset[ThumbR3]);
                    vec3 pad = transformPoint(gA[ThumbR3], d3 * (sk.boneLength[ThumbR3] * 0.72f) + normalize(cross(vec3(1, 0, 0), d3)) * kPadRadius);
                    vec3 pl = transformPoint(HBi, pad);
                    int which = -1;
                    float dist = armDist(G, FBo, pad, false, &which);
                    float toIdx = length(pad - gB[IndexR1].translation());
                    std::printf("    thumb pad: in partner hand (%+.4f %+.4f %+.4f) gap %.2f mm to %s, %.1f mm from its index knuckle\n", pl.x, pl.y, pl.z,
                                dist * 1000, which >= 0 ? boneName(Bone(which)) : "-", toIdx * 1000);
                }
                {
                    // Skin gaps (mesh vertices to the partner's arm): each distal phalanx's pad side.
                    ArmFrames FAa;
                    for (int ai = 0; ai < kNArm; ++ai) FAa.frame[ai] = gA[kArmBones[ai]];
                    std::printf("    skin gaps (pad side of the distal phalanx):");
                    for (int f = 0; f <= 4; ++f) {
                        int ai = 3 + f * 3 + 2, w = -1;
                        float g = skinGap(G, FAa, ai, FBo, f == 0 ? 0.0f : 1.0f, &w);
                        std::printf(" %s %.2f mm (%s)", f == 0 ? "thumb" : f == 1 ? "index" : f == 2 ? "middle" : f == 3 ? "ring" : "pinky", g * 1000,
                                    w >= 0 ? boneName(Bone(w)) : "-");
                    }
                    std::printf("\n");
                    const HandSample hs = an[a]->impl_->shakeHand().motion.sample(an[a]->impl_->time);
                    std::printf("    thumb joints {%.2f %.2f %.2f %.2f}, elbow %.2f\n", hs.f.v[0][0], hs.f.v[0][1], hs.f.v[0][2], hs.f.v[0][3], hs.elbow);
                }
                float thumbs = segSegDist(gA[ThumbR1].translation(), gA[ThumbR2].translation(), gB[ThumbR1].translation(), gB[ThumbR2].translation());
                vec3 webA = (gA[ThumbR2].translation() + gA[IndexR1].translation()) * 0.5f, webB = (gB[ThumbR2].translation() + gB[IndexR1].translation()) * 0.5f;
                std::printf("    thumb metacarpal axes %.1f mm apart, webs %.1f mm apart\n", thumbs * 1000, length(webA - webB) * 1000);
            }
        }
    }
    for (auto& pr : prof) std::printf("   speed u=%.3f %.2f m/s\n", pr.first, pr.second);
    std::printf("   hand speed: peak into contact %.2f m/s (u=%.3f), peak on the way back %.2f m/s (u=%.3f), max acceleration in the release %.1f m/s2 (u=%.3f)\n",
                peakIn, peakInT, peakOut, peakOutT, maxAccRel, maxAccRelT);
    std::printf("== %s: worst interpenetration %.2f mm at u=%.3f (%s)\n", mode.c_str(), worst * 1000, worstT, worstWhat.c_str());
    for (auto& kv : phaseWorst) std::printf("   phase %-9s worst %.2f mm\n", kv.first.c_str(), kv.second * 1000);
    std::printf("   lowest hand point %.4f m (u=%.3f); forearm over the board lowest %.4f m (u=%.3f)\n", minLow, minLowT, minFore, minForeT);
    std::printf("   clasp strain without the elbow lift max %.4f; pops over 1800 deg/s: %d; IK clamp in contact (t1..tw) %.4f\n", maxStrain0, pops, maxClampC);
    std::printf("   clasp strain max %.4f (u=%.3f); IK clamp max %.4f (u=%.3f); max angular speed %.0f deg/s (%s.%s u=%.3f)\n", maxStrain, maxStrainT,
                maxClamp, maxClampT, maxSpeed, maxSpeedWhat.c_str(), boneName(Bone(maxSpeedBone)), maxSpeedT);
    return 0;
}
