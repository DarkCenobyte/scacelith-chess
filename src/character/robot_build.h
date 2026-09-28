// Internal helpers shared by the robot geometry builders (robot_*.cpp). Not a public API.
#pragma once
#include "robot.h"
#include "sdf.h"
#include <functional>
#include <string>

namespace character {
namespace build {

// Porcelain per-part parameters (DrawItem::inst, see shaders/materials/robot_porcelain.glsl):
//   inst[0] = (variant, clearcoat scale, roughness offset, seam half-width in m)
//             variant 0 = glazed shell, 1 = soft-touch pad region on the positive side of seam 0
//   inst[1..3] = panel seam planes in bone space (xyz unit normal, w offset: dot(n,p) + w = 0);
//                zero vector = unused.
struct PorcelainLook {
    float variant = 0.0f, coat = 1.0f, roughOffset = 0.0f, seamWidth = 0.00018f;
    m::vec4 seams[3] = {};
};

// Parts are declared with a mesh *job* (a closure returning the mesh); buildRobot() runs all jobs
// in parallel, then fills the mirrored copies.
using MeshJob = std::function<MeshData()>;

struct Sink {
    std::vector<RobotPart> parts;
    std::vector<MeshJob> jobs;      // per part; empty for mirrored copies
    std::vector<int> mirrorOf;      // per part: source part index, or -1
    void add(const char* name, Bone b, MaterialId mat, MeshJob job, bool fpHidden, const m::vec4* inst = nullptr);
    void addPorcelain(const char* name, Bone b, MeshJob job, bool fpHidden, const PorcelainLook& look,
                      MaterialId mat = MaterialId::RobotPorcelain);
    void addJoint(const char* name, Bone b, MeshJob job, bool fpHidden, float variant = 0.0f);
    // Mirrors every part added since 'from' onto the other side (right -> left).
    void mirrorFrom(size_t from);
    // Runs the mesh jobs on 'threads' worker threads and completes the mirrored parts.
    void run(int threads);
};

m::vec4 seamPlane(m::vec3 n, m::vec3 pointOnPlane);

// Opposite-side bone for a right-side bone.
Bone mirrorBone(Bone rightBone);

void buildHand(Sink& s);    // right hand + fingers (+ mirrored left)
void buildArm(Sink& s);     // clavicle, upper arm, forearm (+ mirrored)
void buildTorso(Sink& s);   // pelvis, abdomen, chest, neck
void buildLegs(Sink& s);    // thighs, shins, feet (+ mirrored)
void buildHead(Sink& s);    // head shell, face, eyes, lids

// Bone-local position of a bone's joint relative to another bone (rest pose), e.g. the elbow in
// upper-arm space: restOffset chain difference.
m::vec3 restJoint(Bone b);  // character-space joint position in the rest pose (pelvis at origin)

}  // namespace build
}  // namespace character
