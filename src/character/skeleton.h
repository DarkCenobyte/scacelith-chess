// Character skeleton shared by the robot model (character/) and the animation system (anim/).
//
// Conventions (do not change without updating both sides):
//  * Character space: +Y up, +Z forward (the way the character faces), +X = the character's LEFT.
//    (Right-handed: right = forward x up = -X.)
//  * Every bone has a local frame whose origin is the joint pivot. In the REST pose all local
//    rotations are identity, i.e. every bone frame is axis-aligned with character space.
//  * Rest pose = seated upright: thighs horizontal pointing +Z, shins vertical down, feet flat
//    pointing +Z; arms hanging straight down (-Y) at the sides, palms facing the thighs
//    (right palm faces +X, left palm faces -X), thumbs pointing forward (+Z); fingers straight
//    down; head looking forward (+Z); eyes looking +Z.
//  * Bone "direction": for limbs the child joint lies along the bone's local -Y (arms, fingers,
//    shins) or +Z (thighs, feet); for the spine/neck/head along +Y.
//  * Finger flexion (curling toward the palm) is a rotation about the bone's local Z axis:
//    positive angle curls the RIGHT hand's fingers toward +X (palm side) and the LEFT hand's toward
//    -X; use fingerCurlAxis(side) to stay sign-correct. Thumb flexion uses its own local X axis.
//  * The root (Pelvis) is placed in the world by the owner (chair position); poses store local
//    rotations only (translations come from Skeleton::restOffset).
#pragma once
#include "../math/math.h"
#include <cstdint>

namespace character {

enum Bone : uint8_t {
    Pelvis, Spine1, Spine2, Neck, Head, EyeL, EyeR, LidUpperL, LidUpperR, LidLowerL, LidLowerR,
    ClavicleL, UpperArmL, ForeArmL, HandL,
    ThumbL1, ThumbL2, ThumbL3, IndexL1, IndexL2, IndexL3, MiddleL1, MiddleL2, MiddleL3,
    RingL1, RingL2, RingL3, PinkyL1, PinkyL2, PinkyL3,
    ClavicleR, UpperArmR, ForeArmR, HandR,
    ThumbR1, ThumbR2, ThumbR3, IndexR1, IndexR2, IndexR3, MiddleR1, MiddleR2, MiddleR3,
    RingR1, RingR2, RingR3, PinkyR1, PinkyR2, PinkyR3,
    ThighL, ShinL, FootL, ThighR, ShinR, FootR,
    BoneCount
};

enum class Side : uint8_t { Left, Right };

const char* boneName(Bone b);

struct Skeleton {
    int8_t parent[BoneCount];          // -1 for Pelvis
    m::vec3 restOffset[BoneCount];     // joint position in the parent's frame (rest pose)
    float boneLength[BoneCount];       // distance to the "end" of the bone (for IK / tips)
};

// The robot's proportions (adult, ~1.78 m standing). Defined in character/skeleton.cpp.
const Skeleton& robotSkeleton();

struct Pose {
    m::quat local[BoneCount];          // local rotation relative to rest (identity = rest)
    m::vec3 rootPosition;              // pelvis position in world space
    m::quat rootRotation;              // character space -> world
    Pose() : rootPosition(0, 0, 0) {}
};

// Global (world) bone matrices: world = root * (rest translation * local rotation) chain.
void computeGlobal(const Skeleton& sk, const Pose& pose, m::mat4 out[BoneCount]);

// Helpers for mirrored limb code.
inline Bone sideBone(Bone leftBone, Side s) {
    // Left arm/hand block is ClavicleL..PinkyL3, right block ClavicleR..PinkyR3 (same order).
    if (s == Side::Left) return leftBone;
    if (leftBone >= ClavicleL && leftBone <= PinkyL3) return Bone(leftBone + (ClavicleR - ClavicleL));
    if (leftBone >= ThighL && leftBone <= FootL) return Bone(leftBone + (ThighR - ThighL));
    if (leftBone == EyeL) return EyeR;
    if (leftBone == LidUpperL) return LidUpperR;
    if (leftBone == LidLowerL) return LidLowerR;
    return leftBone;
}
inline m::vec3 fingerCurlAxis(Side s) { return s == Side::Right ? m::vec3(0, 0, 1) : m::vec3(0, 0, -1); }

}  // namespace character
