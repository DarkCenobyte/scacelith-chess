#include "skeleton.h"

using namespace m;

namespace character {

static const char* kNames[BoneCount] = {
    "Pelvis", "Spine1", "Spine2", "Neck", "Head", "EyeL", "EyeR", "LidUpperL", "LidUpperR", "LidLowerL", "LidLowerR",
    "ClavicleL", "UpperArmL", "ForeArmL", "HandL",
    "ThumbL1", "ThumbL2", "ThumbL3", "IndexL1", "IndexL2", "IndexL3", "MiddleL1", "MiddleL2", "MiddleL3",
    "RingL1", "RingL2", "RingL3", "PinkyL1", "PinkyL2", "PinkyL3",
    "ClavicleR", "UpperArmR", "ForeArmR", "HandR",
    "ThumbR1", "ThumbR2", "ThumbR3", "IndexR1", "IndexR2", "IndexR3", "MiddleR1", "MiddleR2", "MiddleR3",
    "RingR1", "RingR2", "RingR3", "PinkyR1", "PinkyR2", "PinkyR3",
    "ThighL", "ShinL", "FootL", "ThighR", "ShinR", "FootR"};

const char* boneName(Bone b) { return b < BoneCount ? kNames[b] : "?"; }

static Skeleton buildRobotSkeleton() {
    Skeleton s;
    for (int i = 0; i < BoneCount; ++i) { s.parent[i] = -1; s.restOffset[i] = vec3(0); s.boneLength[i] = 0.0f; }
    auto set = [&](Bone b, Bone parent, vec3 off, float len) {
        s.parent[b] = int8_t(parent);
        s.restOffset[b] = off;
        s.boneLength[b] = len;
    };
    s.parent[Pelvis] = -1;
    s.boneLength[Pelvis] = 0.10f;
    // Spine: seated eye height ~0.765 m above the seat (pelvis joint is 0.10 m above the seat),
    // i.e. eyes at ~1.225 m with SEAT_HEIGHT = 0.46; shoulders ~0.56 m above the seat.
    set(Spine1, Pelvis, {0, 0.10f, -0.01f}, 0.10f);
    set(Spine2, Spine1, {0, 0.14f, 0.0f}, 0.25f);
    set(Neck, Spine2, {0, 0.25f, -0.005f}, 0.095f);
    set(Head, Neck, {0, 0.095f, 0.015f}, 0.21f);
    // Eyes: centre of each eyeball, head frame. Interpupillary distance 64 mm.
    set(EyeL, Head, {0.032f, 0.080f, 0.075f}, 0.0115f);
    set(EyeR, Head, {-0.032f, 0.080f, 0.075f}, 0.0115f);
    set(LidUpperL, Head, {0.032f, 0.080f, 0.075f}, 0.013f);
    set(LidUpperR, Head, {-0.032f, 0.080f, 0.075f}, 0.013f);
    set(LidLowerL, Head, {0.032f, 0.080f, 0.075f}, 0.013f);
    set(LidLowerR, Head, {-0.032f, 0.080f, 0.075f}, 0.013f);
    // Arms (left = +X). Shoulder joint 0.19 m from the midline, ~0.59 m above the seat.
    for (int side = 0; side < 2; ++side) {
        float sx = side == 0 ? 1.0f : -1.0f;
        Bone o = side == 0 ? ClavicleL : ClavicleR;
        auto B = [&](Bone left) { return Bone(o + (left - ClavicleL)); };
        set(B(ClavicleL), Spine2, {sx * 0.025f, 0.225f, 0.02f}, 0.16f);
        set(B(UpperArmL), B(ClavicleL), {sx * 0.165f, 0.005f, -0.03f}, 0.300f);
        set(B(ForeArmL), B(UpperArmL), {0, -0.300f, 0}, 0.265f);
        set(B(HandL), B(ForeArmL), {0, -0.265f, 0}, 0.085f);  // wrist -> knuckles (MCP)
        // Finger roots (MCP joints) in the hand frame; palm faces the body (-sx direction).
        const float fz[4] = {0.030f, 0.010f, -0.009f, -0.026f};   // index..pinky spread along Z
        const float fy[4] = {-0.086f, -0.088f, -0.085f, -0.078f};
        const float len[4][3] = {{0.039f, 0.024f, 0.019f}, {0.043f, 0.027f, 0.020f}, {0.040f, 0.026f, 0.019f}, {0.032f, 0.019f, 0.017f}};
        for (int f = 0; f < 4; ++f) {
            Bone f1 = B(Bone(IndexL1 + f * 3));
            set(f1, B(HandL), {sx * 0.004f, fy[f], fz[f]}, len[f][0]);
            set(Bone(f1 + 1), f1, {0, -len[f][0], 0}, len[f][1]);
            set(Bone(f1 + 2), Bone(f1 + 1), {0, -len[f][1], 0}, len[f][2]);
        }
        // Thumb: CMC joint near the wrist, on the palm side, pointing forward/down.
        set(B(ThumbL1), B(HandL), {-sx * 0.012f, -0.022f, 0.026f}, 0.040f);
        set(B(ThumbL2), B(ThumbL1), {0, -0.028f, 0.028f}, 0.032f);
        set(B(ThumbL3), B(ThumbL2), {0, -0.020f, 0.024f}, 0.024f);
    }
    // Legs (seated): thigh forward, shin down, foot forward.
    for (int side = 0; side < 2; ++side) {
        float sx = side == 0 ? 1.0f : -1.0f;
        Bone t = side == 0 ? ThighL : ThighR;
        set(t, Pelvis, {sx * 0.095f, -0.03f, 0.0f}, 0.44f);
        set(Bone(t + 1), t, {0, 0, 0.44f}, 0.44f);
        set(Bone(t + 2), Bone(t + 1), {0, -0.44f, 0}, 0.19f);
    }
    return s;
}

const Skeleton& robotSkeleton() {
    static Skeleton s = buildRobotSkeleton();
    return s;
}

void computeGlobal(const Skeleton& sk, const Pose& pose, mat4 out[BoneCount]) {
    mat4 root = toMat4(pose.rootRotation, pose.rootPosition);
    for (int i = 0; i < BoneCount; ++i) {
        mat4 local = toMat4(pose.local[i], sk.restOffset[i]);
        int p = sk.parent[i];
        out[i] = p < 0 ? root * local : out[p] * local;  // parents always precede children
    }
}

}  // namespace character
