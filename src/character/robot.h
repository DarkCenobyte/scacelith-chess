// Scacelith's player character: an elegant porcelain humanoid robot (1.78 m, slim), generated
// procedurally at startup (SDF sculpting + adaptive meshing, see sdf.h). Both players use the
// exact same model.
//
// The robot is rigid and segmented: every part is attached to one bone of character::Skeleton
// (no skinning). Joints are ball/hinge mechanisms (RobotJoint) hidden between overlapping
// porcelain shells, so the body looks continuous through each bone's range of motion.
//
// Usage:
//   character::setupRobotMaterials();                 // once, after materials::init()
//   std::vector<character::RobotPart> parts = character::buildRobot();
//   character::GpuRobot gpu; gpu.upload(parts);       // share it between both players
//   ...
//   anim.update(dt, events);
//   character::submitRobot(renderer, gpu, anim.globals(), isHumanPlayer, 1000, prevGlobals);
//
// Eyelids: the LidUpper*/LidLower* bones sit at the eyeball centre, axis-aligned with the head in
// the rest pose, and rotate about their local X axis. Identity = relaxed open eye. Closing is a
// POSITIVE rotation for the upper lid and a NEGATIVE one for the lower lid; use lidRotation() to
// stay in sync with the geometry.
#pragma once
#include "skeleton.h"
#include "../render/materials/material_library.h"
#include "../render/mesh.h"
#include "../render/renderer.h"
#include <vector>

namespace character {

struct RobotPart {
    Bone bone;
    MeshData mesh;              // bone-local space, rest pose
    MaterialId material;
    bool firstPersonHidden;     // blocks/clips the first-person camera (head, eyes, lids, neck top)
    m::vec4 inst[4] = {};       // default per-instance parameters (panel seams, material variant)
    const char* name = "";
};

// Builds every part of one robot (right side modelled, left side mirrored). Deterministic.
std::vector<RobotPart> buildRobot();

// Configures the Robot* entries of the material library (surface shaders + parameters).
void setupRobotMaterials();

// ---- Eye / eyelid geometry (eye-bone space: origin = eyeball centre, +Z = gaze) --------------
namespace eye {
constexpr float RADIUS = 0.0115f;          // sclera sphere
constexpr float LIMBUS_RADIUS = 0.0059f;   // visible iris radius
constexpr float CORNEA_RADIUS = 0.0078f;   // cornea curvature radius
constexpr float IRIS_DEPTH = 0.0004f;      // iris plane behind the limbus plane
constexpr float LID_INNER = 0.0133f;       // eyelid shells: inner radius (clears the cornea apex)
constexpr float LID_OUTER = 0.0150f;       // outer radius
constexpr float LID_UPPER_OPEN = 0.22f;    // upper margin plane tilt above the eye axis (rad), open
constexpr float LID_LOWER_OPEN = 0.27f;    // lower margin plane tilt below the axis (rad), open
constexpr float LID_CLOSED = -0.07f;       // both margins meet slightly below the axis when closed
float limbusZ();                           // z of the limbus plane
float corneaCenterZ();
}  // namespace eye

// Rotation of a lid bone for a given closure (0 = relaxed open, 1 = closed) and vertical gaze
// (radians, positive = looking up): lids follow the gaze like human lids do.
m::quat lidRotation(Bone lidBone, float closure, float gazePitch = 0.0f);

// ---- GPU side ----------------------------------------------------------------------------------
struct GpuRobot {
    struct Part {
        Mesh mesh;
        Bone bone = Pelvis;
        MaterialId material = MaterialId::RobotPorcelain;
        bool firstPersonHidden = false;
        m::vec4 inst[4];
    };
    std::vector<Part> parts;
    uint32_t triangleCount = 0;
    void upload(const std::vector<RobotPart>& parts);
    void destroy();
};

// Submits every part with its bone's world matrix. firstPerson = this robot is the viewer: parts
// flagged firstPersonHidden get DRAW_HIDDEN_MAIN (still cast shadows). objectIdBase + part index is
// used as the stable object id. pupilDilation in [0,1] goes to the iris (inst[0].x). armOpacity < 1
// makes the arm on armSide (upper arm, forearm, hand, fingers) see-through in the main view
// (DrawItem::opacity; its shadow stays).
void submitRobot(render::Renderer& r, const GpuRobot& robot, const m::mat4 boneWorld[BoneCount], bool firstPerson,
                 uint32_t objectIdBase, const m::mat4* prevBoneWorld = nullptr, float pupilDilation = 0.35f,
                 float armOpacity = 1.0f, Side armSide = Side::Right);

}  // namespace character
