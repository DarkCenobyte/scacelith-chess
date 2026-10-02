// GPU upload and draw submission of the robot parts.
#include "robot.h"

using namespace m;

namespace character {

void GpuRobot::upload(const std::vector<RobotPart>& src) {
    destroy();
    parts.resize(src.size());
    triangleCount = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        Part& p = parts[i];
        p.mesh.upload(src[i].mesh, src[i].name);
        p.bone = src[i].bone;
        p.material = src[i].material;
        p.firstPersonHidden = src[i].firstPersonHidden;
        for (int k = 0; k < 4; ++k) p.inst[k] = src[i].inst[k];
        triangleCount += p.mesh.indexCount / 3;
    }
}

void GpuRobot::destroy() {
    for (auto& p : parts) p.mesh.destroy();
    parts.clear();
    triangleCount = 0;
}

void submitRobot(render::Renderer& r, const GpuRobot& robot, const mat4 boneWorld[BoneCount], bool firstPerson,
                 uint32_t objectIdBase, const mat4* prevBoneWorld, float pupilDilation, float armOpacity, Side armSide,
                 const Material* chestMaterial) {
    const Bone armFirst = sideBone(UpperArmL, armSide), armLast = sideBone(PinkyL3, armSide);
    for (size_t i = 0; i < robot.parts.size(); ++i) {
        const GpuRobot::Part& p = robot.parts[i];
        render::DrawItem d;
        d.mesh = &p.mesh;
        d.material = &materials::get(p.material);
        // The chest shell is the only porcelain part on Spine2 (robot_body.cpp, buildTorso).
        if (chestMaterial && p.bone == Spine2 && p.material == MaterialId::RobotPorcelain) d.material = chestMaterial;
        d.model = boneWorld[p.bone];
        if (prevBoneWorld) {
            d.prevModel = prevBoneWorld[p.bone];
            d.hasPrevModel = true;
        }
        for (int k = 0; k < 4; ++k) d.inst[k] = p.inst[k];
        if (p.material == MaterialId::RobotEyeIris) d.inst[0].x = clamp(pupilDilation, 0.0f, 1.0f);
        d.flags = render::DRAW_CAST_SHADOW;
        if (firstPerson && p.firstPersonHidden) d.flags |= render::DRAW_HIDDEN_MAIN;
        d.objectId = objectIdBase + uint32_t(i);
        if (p.bone >= armFirst && p.bone <= armLast) d.opacity = armOpacity;
        r.submit(d);
    }
}

}  // namespace character
