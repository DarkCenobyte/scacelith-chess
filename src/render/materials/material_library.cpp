// Placeholder material library: standard.glsl with plausible parameters for every id.
// The render-materials work package replaces this with dedicated surface shaders and baked
// textures, keeping the same interface.
#include "material_library.h"
#include "../../core/log.h"

using namespace m;

namespace materials {
namespace {
Material g_mats[int(MaterialId::Count)];
const char* g_names[int(MaterialId::Count)] = {
    "MarbleWhitePiece", "MarbleBlackPiece", "PieceFelt", "BoardSquareLight", "BoardSquareDark", "BoardFrame",
    "TableWood", "TableWoodCarved", "ChairWood", "ChairVelvet", "FloorMarble", "FloorMarbleInlay", "WallStone",
    "WallPanelWood", "GildedTrim", "Tapestry", "CeilingPainted", "WindowGlass", "WindowFrame", "Curtain", "Brass",
    "Crystal", "CandleWax", "ClockCase", "ClockDisplay", "ClockLever", "ClockPanel", "RobotPorcelain", "RobotJoint",
    "RobotEyeSclera", "RobotEyeIris", "RobotEyeCornea", "RobotLid", "Default"};

void std(MaterialId id, vec3 albedo, float rough, float metal = 0, float clearcoat = 0, float ccRough = 0.05f, float sss = 0) {
    Material& m = g_mats[int(id)];
    m.name = g_names[int(id)];
    m.surface = "shaders/materials/standard.glsl";
    m.params[0] = vec4(albedo, rough);
    m.params[1] = vec4(metal, 0.5f, clearcoat, ccRough);
    m.params[2] = vec4(0, 0, 0, sss);
}
}  // namespace

bool init() {
    for (int i = 0; i < int(MaterialId::Count); ++i) std(MaterialId(i), vec3(0.5f), 0.5f);
    std(MaterialId::MarbleWhitePiece, vec3(0.86f, 0.84f, 0.80f), 0.2f, 0, 1, 0.04f, 0.6f);
    std(MaterialId::MarbleBlackPiece, vec3(0.025f), 0.2f, 0, 1, 0.04f);
    std(MaterialId::PieceFelt, vec3(0.05f, 0.18f, 0.08f), 0.9f);
    std(MaterialId::BoardSquareLight, vec3(0.85f, 0.83f, 0.78f), 0.2f, 0, 1, 0.03f, 0.5f);
    std(MaterialId::BoardSquareDark, vec3(0.03f), 0.2f, 0, 1, 0.03f);
    std(MaterialId::BoardFrame, vec3(0.12f, 0.05f, 0.03f), 0.3f, 0, 1, 0.05f);
    std(MaterialId::TableWood, vec3(0.20f, 0.08f, 0.04f), 0.35f, 0, 1, 0.04f);
    std(MaterialId::TableWoodCarved, vec3(0.18f, 0.07f, 0.035f), 0.45f, 0, 0.6f, 0.2f);
    std(MaterialId::ChairWood, vec3(0.16f, 0.07f, 0.035f), 0.4f, 0, 0.6f, 0.15f);
    std(MaterialId::ChairVelvet, vec3(0.35f, 0.02f, 0.03f), 0.8f);
    g_mats[int(MaterialId::ChairVelvet)].params[3] = vec4(0.5f, 0.15f, 0.15f, 0.5f);
    std(MaterialId::FloorMarble, vec3(0.86f, 0.82f, 0.75f), 0.15f, 0, 1, 0.02f, 0.3f);
    std(MaterialId::FloorMarbleInlay, vec3(0.35f, 0.20f, 0.15f), 0.15f, 0, 1, 0.02f);
    std(MaterialId::WallStone, vec3(0.72f, 0.68f, 0.60f), 0.8f);
    std(MaterialId::WallPanelWood, vec3(0.18f, 0.08f, 0.04f), 0.5f, 0, 0.5f, 0.2f);
    std(MaterialId::GildedTrim, vec3(1.0f, 0.78f, 0.34f), 0.3f, 1.0f);
    std(MaterialId::Tapestry, vec3(0.05f, 0.08f, 0.35f), 0.9f);
    g_mats[int(MaterialId::Tapestry)].params[3] = vec4(0.2f, 0.2f, 0.4f, 0.6f);
    std(MaterialId::CeilingPainted, vec3(0.75f, 0.70f, 0.60f), 0.7f);
    std(MaterialId::WindowGlass, vec3(0.9f), 0.02f);
    g_mats[int(MaterialId::WindowGlass)].transparent = true;
    std(MaterialId::WindowFrame, vec3(0.25f, 0.22f, 0.20f), 0.5f);
    std(MaterialId::Curtain, vec3(0.30f, 0.02f, 0.03f), 0.8f);
    std(MaterialId::Brass, vec3(0.95f, 0.75f, 0.45f), 0.25f, 1.0f);
    std(MaterialId::Crystal, vec3(0.95f), 0.02f);
    g_mats[int(MaterialId::Crystal)].transparent = true;
    std(MaterialId::CandleWax, vec3(0.9f, 0.88f, 0.8f), 0.4f, 0, 0, 0.05f, 0.8f);
    std(MaterialId::ClockCase, vec3(0.15f, 0.07f, 0.035f), 0.35f, 0, 0.8f, 0.1f);
    std(MaterialId::ClockDisplay, vec3(0.45f, 0.48f, 0.40f), 0.2f, 0, 1, 0.02f);
    std(MaterialId::ClockLever, vec3(0.02f), 0.3f, 0, 1, 0.1f);
    std(MaterialId::ClockPanel, vec3(0.05f), 0.4f, 0.8f);
    std(MaterialId::RobotPorcelain, vec3(0.92f, 0.91f, 0.89f), 0.3f, 0, 1, 0.05f, 0.4f);
    std(MaterialId::RobotJoint, vec3(0.03f), 0.45f, 0.6f);
    std(MaterialId::RobotEyeSclera, vec3(0.85f, 0.82f, 0.80f), 0.1f, 0, 1, 0.02f, 0.5f);
    std(MaterialId::RobotEyeIris, vec3(0.20f, 0.30f, 0.35f), 0.3f, 0, 1, 0.02f);
    std(MaterialId::RobotEyeCornea, vec3(1.0f), 0.02f);
    g_mats[int(MaterialId::RobotEyeCornea)].transparent = true;
    std(MaterialId::RobotLid, vec3(0.92f, 0.91f, 0.89f), 0.3f, 0, 1, 0.05f, 0.4f);
    std(MaterialId::Default, vec3(0.5f), 0.5f);
    return true;
}

void shutdown() {}
const Material& get(MaterialId id) { return g_mats[int(id)]; }
Material& getMutable(MaterialId id) { return g_mats[int(id)]; }
const char* name(MaterialId id) { return g_names[int(id)]; }
}  // namespace materials
