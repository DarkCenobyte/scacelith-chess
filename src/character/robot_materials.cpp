// Robot materials: porcelain shells, joint mechanisms, eyes and eyelids.
#include "robot.h"

using namespace m;

namespace character {

void setupRobotMaterials() {
    // Glazed porcelain / composite shells.
    Material& por = materials::getMutable(MaterialId::RobotPorcelain);
    por = Material();
    por.name = "RobotPorcelain";
    por.surface = "shaders/materials/robot_porcelain.glsl";
    por.params[0] = vec4(0.80f, 0.785f, 0.755f, 0.36f);     // body albedo (warm white), base roughness
    por.params[1] = vec4(1.0f, 0.035f, 0.45f, 0.30f);       // clearcoat, coat roughness, specular, subsurface
    por.params[2] = vec4(1.0f, 0.86f, 0.74f, 0.35f);        // subsurface tint, orange-peel strength
    por.params[3] = vec4(0.70f, 0.69f, 0.67f, 0.62f);       // soft-touch pad albedo, roughness
    por.params[4] = vec4(1.0f, 0.00045f, 0.35f, 0.02f);     // seam darkening, bevel width, smudges, albedo variation
    por.tessellated = true;
    por.tessLevel = 8.0f;
    por.defines = {"MATERIAL_SCREEN_DOOR"};   // see-through playing arm (submitRobot armOpacity)

    // Eyelids: same glaze, a touch warmer and smoother.
    Material& lid = materials::getMutable(MaterialId::RobotLid);
    lid = por;
    lid.name = "RobotLid";
    lid.params[0] = vec4(0.79f, 0.765f, 0.735f, 0.34f);
    lid.params[4].z = 0.0f;
    lid.defines = {"ROBOT_LID"};

    // Joint mechanisms: satin anodised graphite, rubber, polished steel.
    Material& jt = materials::getMutable(MaterialId::RobotJoint);
    jt = Material();
    jt.name = "RobotJoint";
    jt.surface = "shaders/materials/robot_joint.glsl";
    jt.params[0] = vec4(0.055f, 0.058f, 0.066f, 0.34f);
    jt.params[1] = vec4(0.022f, 0.022f, 0.024f, 0.62f);
    jt.params[2] = vec4(0.62f, 0.62f, 0.63f, 0.12f);
    jt.params[3] = vec4(1.0f, 9000.0f, 0, 0);
    jt.tessellated = true;
    jt.tessLevel = 4.0f;
    jt.defines = {"MATERIAL_SCREEN_DOOR"};

    // Eyes: one surface file, three variants.
    const float lz = eye::limbusZ(), cz = eye::corneaCenterZ();
    for (MaterialId id : {MaterialId::RobotEyeSclera, MaterialId::RobotEyeIris, MaterialId::RobotEyeCornea}) {
        Material& e = materials::getMutable(id);
        e = Material();
        e.surface = "shaders/materials/robot_eye.glsl";
        e.params[0] = vec4(eye::RADIUS, eye::CORNEA_RADIUS, cz, eye::LIMBUS_RADIUS);  // geometry
        e.params[1] = vec4(lz - eye::IRIS_DEPTH, 1.376f, 0.0011f, 0.0036f);          // iris plane z, cornea IOR, pupil radius min/max
        e.params[2] = vec4(0.24f, 0.34f, 0.40f, 0.0f);   // iris outer (ciliary) colour: grey-blue
        e.params[3] = vec4(0.46f, 0.30f, 0.12f, 0.0f);   // iris inner (collarette) colour: warm amber
        e.params[4] = vec4(0.82f, 0.79f, 0.76f, 0.0f);   // sclera colour
        e.params[5] = vec4(0.55f, 0.08f, 0.07f, 0.0f);   // vessel colour
    }
    Material& sclera = materials::getMutable(MaterialId::RobotEyeSclera);
    sclera.name = "RobotEyeSclera";
    sclera.defines = {"EYE_SCLERA"};
    Material& iris = materials::getMutable(MaterialId::RobotEyeIris);
    iris.name = "RobotEyeIris";
    iris.defines = {"EYE_IRIS"};
    Material& cornea = materials::getMutable(MaterialId::RobotEyeCornea);
    cornea.name = "RobotEyeCornea";
    cornea.defines = {"EYE_CORNEA"};
    cornea.transparent = true;
    cornea.castShadow = false;
}

}  // namespace character
