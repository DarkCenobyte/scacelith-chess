// Material library (render-materials work package): dedicated surface shaders in
// shaders/materials/*.glsl, parameters, and procedural textures baked with compute shaders.
// The meaning of Material::params / DrawItem::inst for each surface file is documented at the
// top of that file; the per-object conventions are summarised in material_library.h.
#include "material_library.h"
#include "material_bake.h"
#include "../../core/log.h"

using namespace m;

namespace materials {
namespace {
Material g_mats[int(MaterialId::Count)];
BakedTextures g_tex;
bool g_initialized = false;
const char* g_names[int(MaterialId::Count)] = {
    "MarbleWhitePiece", "MarbleBlackPiece", "PieceFelt", "BoardSquareLight", "BoardSquareDark", "BoardFrame",
    "TableWood", "TableWoodCarved", "ChairWood", "ChairVelvet", "FloorMarble", "FloorMarbleInlay", "WallStone",
    "WallPanelWood", "GildedTrim", "Tapestry", "CeilingPainted", "WindowGlass", "WindowFrame", "Curtain", "Brass",
    "Crystal", "CandleWax", "ClockCase", "ClockDisplay", "ClockLever", "ClockPanel", "RobotPorcelain", "RobotJoint",
    "RobotEyeSclera", "RobotEyeIris", "RobotEyeCornea", "RobotLid", "ScoresheetPaper", "ScoresheetCard", "PenBody",
    "PenMetal", "Default"};

// sRGB authoring colour (0..255) -> linear.
vec3 srgb(float r, float g, float b) {
    auto f = [](float c) {
        c /= 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return vec3(f(r), f(g), f(b));
}

// Placeholder standard.glsl setup (kept for ids owned by other packages and Default).
void placeholderStandard(MaterialId id, vec3 albedo, float rough, float metal = 0, float clearcoat = 0, float ccRough = 0.05f,
                         float sss = 0) {
    Material& m = g_mats[int(id)];
    m.name = g_names[int(id)];
    m.surface = "shaders/materials/standard.glsl";
    m.params[0] = vec4(albedo, rough);
    m.params[1] = vec4(metal, 0.5f, clearcoat, ccRough);
    m.params[2] = vec4(0, 0, 0, sss);
}

Material& def(MaterialId id, const char* surface, std::initializer_list<const char*> defines = {}) {
    Material& m = g_mats[int(id)];
    m = Material();
    m.name = g_names[int(id)];
    m.surface = surface;
    for (const char* d : defines) m.defines.push_back(d);
    return m;
}

void polishTex(Material& m) { m.textures[7] = g_tex.polish.id; }

// ---------------------------------------------------------------------------------------------
// Marble (shaders/materials/marble.glsl)
void defineMarbles() {
    // White pieces: Statuario / Carrara. Warm white, soft grey clouds, sharp grey veins + capillaries.
    {
        Material& m = def(MaterialId::MarbleWhitePiece, "shaders/materials/marble.glsl", {"MARBLE_PIECE"});
        m.params[0] = vec4(srgb(222, 219, 212), 0.75f);
        m.params[1] = vec4(srgb(178, 180, 184), 0.8f);
        m.params[2] = vec4(srgb(108, 108, 114), 0.9f);
        m.params[3] = vec4(srgb(156, 156, 160), 0.55f);
        m.params[4] = vec4(20.0f, 0.006f, 1.0f, 3.0f);
        m.params[5] = vec4(1.0f, 0.86f, 0.72f, 0.7f);
        m.params[6] = vec4(0.3f, 1.0f, 0.04f, 0.6f);
        m.params[7] = vec4(0.0f, 0.02f, 1.0f, 0.8f);
        m.tessellated = true;
        polishTex(m);
    }
    // Black pieces: Nero Marquina. Deep black, thin white calcite veins and golden fractures.
    {
        Material& m = def(MaterialId::MarbleBlackPiece, "shaders/materials/marble.glsl", {"MARBLE_PIECE"});
        m.params[0] = vec4(srgb(22, 22, 24), 0.35f);
        m.params[1] = vec4(srgb(36, 35, 36), 0.35f);
        m.params[2] = vec4(srgb(206, 202, 194), 0.8f);
        m.params[3] = vec4(srgb(206, 196, 176), 0.75f);
        m.params[4] = vec4(12.0f, 0.0035f, 0.7f, 3.5f);
        m.params[5] = vec4(0.9f, 0.85f, 0.75f, 0.08f);
        m.params[6] = vec4(0.3f, 1.0f, 0.035f, 0.3f);
        m.params[7] = vec4(0.9f, 0.003f, 1.0f, 0.06f);
        m.tessellated = true;
        polishTex(m);
    }
    // Board squares: same stones, each square its own slab.
    {
        Material& m = def(MaterialId::BoardSquareLight, "shaders/materials/marble.glsl", {"MARBLE_BOARD"});
        m.params[0] = vec4(srgb(222, 219, 211), 0.75f);
        m.params[1] = vec4(srgb(180, 182, 186), 0.8f);
        m.params[2] = vec4(srgb(110, 110, 116), 0.9f);
        m.params[3] = vec4(srgb(158, 158, 162), 0.55f);
        m.params[4] = vec4(11.0f, 0.005f, 1.0f, 3.5f);
        m.params[5] = vec4(1.0f, 0.86f, 0.72f, 0.6f);
        m.params[6] = vec4(0.3f, 1.0f, 0.035f, 0.5f);
        m.params[7] = vec4(0.0f, 0.02f, 1.0f, 0.8f);
        polishTex(m);
    }
    {
        Material& m = def(MaterialId::BoardSquareDark, "shaders/materials/marble.glsl", {"MARBLE_BOARD"});
        m.params[0] = vec4(srgb(22, 22, 24), 0.35f);
        m.params[1] = vec4(srgb(36, 35, 36), 0.35f);
        m.params[2] = vec4(srgb(206, 202, 194), 0.8f);
        m.params[3] = vec4(srgb(206, 196, 176), 0.75f);
        m.params[4] = vec4(13.0f, 0.0035f, 0.7f, 3.5f);
        m.params[5] = vec4(0.9f, 0.85f, 0.75f, 0.08f);
        m.params[6] = vec4(0.3f, 1.0f, 0.035f, 0.3f);
        m.params[7] = vec4(0.9f, 0.003f, 1.0f, 0.06f);
        polishTex(m);
    }
    // Board frame: Nero Portoro (black with golden veins) - the royal border.
    {
        Material& m = def(MaterialId::BoardFrame, "shaders/materials/marble.glsl", {"MARBLE_FRAME"});
        m.params[0] = vec4(srgb(20, 18, 17), 0.3f);
        m.params[1] = vec4(srgb(40, 34, 28), 0.5f);
        // Muted ochre veins: real Portoro reads black first, the gold is a discreet accent.
        m.params[2] = vec4(srgb(150, 118, 72), 0.65f);
        m.params[3] = vec4(srgb(118, 100, 80), 0.3f);
        m.params[4] = vec4(9.0f, 0.0042f, 1.3f, 3.0f);
        m.params[5] = vec4(0.9f, 0.8f, 0.6f, 0.05f);
        m.params[6] = vec4(0.3f, 1.0f, 0.035f, 0.15f);
        m.params[7] = vec4(0.3f, 0.003f, 1.0f, 0.35f);
        polishTex(m);
    }
    // Floor: cream Calacatta tiles, 0.8 m, 1.5 mm joints, baked slab.
    {
        Material& m = def(MaterialId::FloorMarble, "shaders/materials/marble.glsl", {"MARBLE_FLOOR"});
        m.params[0] = vec4(srgb(236, 229, 214), 0.45f);
        m.params[1] = vec4(srgb(204, 198, 188), 0.03f);
        m.params[2] = vec4(srgb(128, 120, 108), 0.85f);
        m.params[3] = vec4(srgb(176, 170, 160), 0.35f);
        m.params[4] = vec4(0.8f, 0.0015f, 0.0f, 0.0f);
        m.params[5] = vec4(1.0f, 0.86f, 0.70f, 0.5f);
        m.params[6] = vec4(0.3f, 1.0f, 0.02f, 0.0f);
        m.params[7] = vec4(0.0f, 0.0f, 1.0f, 0.0f);
        m.textures[0] = g_tex.marbleSlab.id;
        polishTex(m);
    }
    // Floor inlay: Verde Alpi (deep green serpentine with a white vein network).
    {
        Material& m = def(MaterialId::FloorMarbleInlay, "shaders/materials/marble.glsl", {"MARBLE_FLOOR"});
        m.params[0] = vec4(srgb(22, 44, 34), 0.5f);
        m.params[1] = vec4(srgb(44, 74, 58), 0.1f);
        m.params[2] = vec4(srgb(170, 186, 172), 0.8f);
        m.params[3] = vec4(srgb(206, 214, 204), 0.9f);
        m.params[4] = vec4(0.4f, 0.0015f, 0.0f, 0.0f);
        m.params[5] = vec4(0.6f, 0.9f, 0.7f, 0.15f);
        m.params[6] = vec4(0.3f, 1.0f, 0.02f, 0.0f);
        m.params[7] = vec4(1.0f, 0.0f, 1.0f, 0.0f);
        m.textures[0] = g_tex.marbleSlab.id;
        polishTex(m);
    }
}


// ---------------------------------------------------------------------------------------------
// Wood (shaders/materials/wood.glsl): European walnut, different finishes.
Material& walnut(MaterialId id, vec3 axis, float rings, float coat, float coatRough, float wax) {
    Material& m = def(id, "shaders/materials/wood.glsl");
    m.params[0] = vec4(srgb(100, 72, 54), 0.18f);
    m.params[1] = vec4(srgb(42, 30, 24), 0.6f);
    m.params[2] = vec4(axis, rings);
    m.params[3] = vec4(0.0f, -0.2f, 0.02f, 0.25f);
    m.params[4] = vec4(0.4f, coat, coatRough, 0.8f);
    m.params[5] = vec4(wax, 0.8f, 0.0f, 0.0f);
    m.params[6] = vec4(1.0f, 0.78f, 0.34f, 0.25f);
    polishTex(m);
    return m;
}

void defineWoods() {
    {
        // Table top: a calm, straight-grained slab (little curl and ribbon under the gloss).
        Material& m = walnut(MaterialId::TableWood, vec3(1, 0, 0), 170.0f, 1.0f, 0.045f, 0.8f);
        m.params[3].w = 0.1f;
        m.params[7].x = 0.2f;
    }
    {
        Material& m = walnut(MaterialId::TableWoodCarved, vec3(0, 1, 0), 220.0f, 0.75f, 0.22f, 0.5f);
        m.params[5].w = 0.6f;
    }
    {
        Material& m = walnut(MaterialId::ChairWood, vec3(0, 1, 0), 240.0f, 0.65f, 0.2f, 0.5f);
        m.params[0] = vec4(srgb(90, 64, 48), 0.2f);
        m.params[1] = vec4(srgb(38, 27, 21), 0.6f);
        m.params[5] = vec4(0.5f, 0.6f, 0.8f, 0.5f);
    }
    {
        Material& m = walnut(MaterialId::WallPanelWood, vec3(0, 1, 0), 200.0f, 0.5f, 0.25f, 0.4f);
        m.params[3] = vec4(0.0f, -0.35f, 0.01f, 0.35f);
    }
    walnut(MaterialId::ClockCase, vec3(1, 0, 0), 260.0f, 0.8f, 0.14f, 0.3f);
}


// ---------------------------------------------------------------------------------------------
// Fabrics
void defineFabrics() {
    {
        Material& m = def(MaterialId::Tapestry, "shaders/materials/tapestry.glsl");
        m.params[0] = vec4(srgb(20, 30, 92), 0.08f);
        m.params[1] = vec4(srgb(118, 14, 22), 0.12f);
        m.params[2] = vec4(0.88f, 0.60f, 0.22f, 0.38f);
        m.params[3] = vec4(srgb(226, 212, 178), 0.8f);
        m.params[4] = vec4(0.34f, 0.26f, 800.0f, 500.0f);
        m.params[5] = vec4(2.4f, 3.6f, 0.7f, 0.0015f);
        m.textures[0] = g_tex.tapestry.id;
    }
    {
        Material& m = def(MaterialId::ChairVelvet, "shaders/materials/cloth.glsl", {"CLOTH_VELVET"});
        m.params[0] = vec4(srgb(92, 9, 16), 0.5f);
        m.params[1] = vec4(0.7f, 0.16f, 0.18f, 0.35f);
        m.params[2] = vec4(8.0f, 0.0f, 0.1f, 0.5f);
        m.params[3] = vec4(0.0f, -1.0f, 0.3f, 0.4f);
    }
    {
        Material& m = def(MaterialId::Curtain, "shaders/materials/cloth.glsl", {"CLOTH_VELVET"});
        m.params[0] = vec4(srgb(98, 10, 18), 0.3f);
        m.params[1] = vec4(0.65f, 0.15f, 0.17f, 0.4f);
        m.params[2] = vec4(3.0f, 0.0015f, 0.35f, 0.4f);
        m.params[3] = vec4(0.0f, -1.0f, 0.0f, 0.35f);
        m.doubleSided = true;
    }
    {
        Material& m = def(MaterialId::PieceFelt, "shaders/materials/cloth.glsl", {"CLOTH_FELT"});
        m.params[0] = vec4(srgb(24, 74, 40), 0.12f);
        m.params[1] = vec4(0.10f, 0.20f, 0.12f, 0.7f);
        m.params[2] = vec4(30.0f, 0.0f, 0.05f, 0.0f);
    }
}


// ---------------------------------------------------------------------------------------------
// Metals, stone, paint, glass, wax, lacquer
void defineHall() {
    {
        Material& m = def(MaterialId::GildedTrim, "shaders/materials/metal.glsl", {"METAL_GILDED"});
        m.params[0] = vec4(1.0f, 0.77f, 0.36f, 0.2f);
        m.params[1] = vec4(srgb(128, 44, 26), 0.9f);
        m.params[2] = vec4(srgb(40, 32, 22), 0.6f);
        m.params[3] = vec4(0.085f, 1.0f, 0.06f, 0.6f);
    }
    {
        Material& m = def(MaterialId::Brass, "shaders/materials/metal.glsl", {"METAL_BRASS"});
        m.params[0] = vec4(0.92f, 0.74f, 0.42f, 0.16f);
        m.params[1] = vec4(srgb(92, 70, 40), 0.35f);
        m.params[2] = vec4(srgb(30, 24, 16), 0.6f);
        m.params[3] = vec4(0.8f, 0.6f, 0.0f, 0.0f);
        polishTex(m);
    }
    {
        Material& m = def(MaterialId::WallStone, "shaders/materials/limestone.glsl");
        m.params[0] = vec4(srgb(206, 194, 170), 0.72f);
        m.params[1] = vec4(srgb(178, 166, 142), 0.55f);
        m.params[2] = vec4(0.92f, 0.46f, 0.004f, 1.0f);
        m.params[3] = vec4(srgb(196, 188, 170), 0.8f);
        m.params[4] = vec4(0.18f, 0.6f, 0.3f, 0.08f);
        m.params[5] = vec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    {
        Material& m = def(MaterialId::CeilingPainted, "shaders/materials/ceiling.glsl");
        m.params[0] = vec4(srgb(226, 214, 186), 0.12f);
        m.params[1] = vec4(srgb(116, 152, 196), 0.13f);
        m.params[2] = vec4(1.0f, 0.77f, 0.36f, 0.28f);
        m.params[3] = vec4(0.6f, 0.8f, 0.5f, 0.75f);
        m.params[4] = vec4(srgb(122, 128, 76), 0.004f);
    }
    {
        Material& m = def(MaterialId::WindowFrame, "shaders/materials/painted.glsl");
        m.params[0] = vec4(srgb(214, 206, 188), 0.35f);
        m.params[1] = vec4(srgb(150, 120, 90), 0.5f);
        m.params[2] = vec4(1.0f, 1.0f, 0.6f, 0.0f);
        m.params[3] = vec4(0.0f, 0.0f, 0.0f, 0.3f);
    }
    {
        Material& m = def(MaterialId::WindowGlass, "shaders/materials/glass.glsl");
        m.params[0] = vec4(0.90f, 0.96f, 0.92f, 0.02f);
        m.params[1] = vec4(1.52f, 0.012f, 6.0f, 0.5f);
        m.params[2] = vec4(1.0f, 1.0f, 1.0f, 0.6f);
        m.transparent = true;
        m.castShadow = false;
        m.doubleSided = true;
        polishTex(m);
    }
    {
        Material& m = def(MaterialId::Crystal, "shaders/materials/glass.glsl", {"GLASS_CRYSTAL"});
        m.params[0] = vec4(1.0f, 1.0f, 1.0f, 0.01f);
        m.params[1] = vec4(1.65f, 0.0f, 0.0f, 0.0f);
        m.transparent = true;
        m.castShadow = false;
        polishTex(m);
    }
    {
        Material& m = def(MaterialId::CandleWax, "shaders/materials/wax.glsl");
        m.params[0] = vec4(srgb(236, 226, 200), 0.4f);
        m.params[1] = vec4(1.0f, 0.72f, 0.42f, 0.85f);
        m.params[2] = vec4(0.0f, 0.0f, 0.0f, 0.02f);
        m.params[3] = vec4(0.25f, 0.8f, 0.3f, 0.0f);
    }
    {
        Material& m = def(MaterialId::ClockLever, "shaders/materials/lacquer.glsl");
        m.params[0] = vec4(srgb(10, 10, 11), 0.35f);
        m.params[1] = vec4(0.0f, 1.0f, 0.05f, 0.0f);
        m.params[2] = vec4(1.0f, 0.0f, 0.0f, 0.6f);
        polishTex(m);
    }
    {
        Material& m = def(MaterialId::ClockPanel, "shaders/materials/lacquer.glsl");
        m.params[0] = vec4(srgb(28, 28, 30), 0.32f);
        m.params[1] = vec4(0.85f, 0.0f, 0.1f, 1.0f);
        m.params[2] = vec4(1.0f, 0.0f, 0.0f, 0.5f);
        polishTex(m);
    }
}

// ---------------------------------------------------------------------------------------------
// Scoresheet pad and ballpoint pen. The paper samples page textures that every game::Scoresheet
// renders for itself (it draws with a copy of this material): the library version gets blank
// 1x1 textures so that it never samples an unbound unit.
GLuint g_blankPage = 0, g_blankEntry = 0;

void defineScoresheet() {
    if (!g_blankPage) {
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &g_blankPage);
        glTextureStorage3D(g_blankPage, 1, GL_RGBA8, 1, 1, 1);
        const uint8_t zero[4] = {0, 0, 0, 0};
        glTextureSubImage3D(g_blankPage, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
        glCreateTextures(GL_TEXTURE_2D, 1, &g_blankEntry);
        glTextureStorage2D(g_blankEntry, 1, GL_RGBA8, 1, 1);
        glTextureSubImage2D(g_blankEntry, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    }
    {
        Material& m = def(MaterialId::ScoresheetPaper, "shaders/materials/paper.glsl");
        m.params[0] = vec4(srgb(241, 238, 229), 0.72f);   // slightly warm white offset paper
        m.params[1] = vec4(srgb(28, 42, 118), 0.38f);     // blue ballpoint ink
        m.params[2] = vec4(srgb(34, 34, 38), 0.6f);       // black offset print
        m.params[3] = vec4(1.0f, 1.0f, 4.0f, 1.0f);
        m.params[4] = vec4(1.0f, 1.0f, 148.0f, 210.0f);
        m.textures[0] = g_blankPage;
        m.textures[1] = g_blankEntry;
    }
    {
        Material& m = def(MaterialId::ScoresheetCard, "shaders/materials/paper.glsl", {"PAPER_CARD"});
        m.params[0] = vec4(srgb(128, 122, 112), 0.88f);   // grey chipboard
        m.params[1] = vec4(srgb(26, 30, 34), 0.72f);      // near-black cloth tape
    }
    {
        // Lacquered barrel: deep blue-black under a thick polished clear coat.
        Material& m = def(MaterialId::PenBody, "shaders/materials/lacquer.glsl");
        m.params[0] = vec4(srgb(14, 18, 36), 0.3f);
        m.params[1] = vec4(0.0f, 1.0f, 0.03f, 0.0f);
        m.params[2] = vec4(0.0f, 1.0f, 0.0f, 0.5f);
        polishTex(m);
    }
    {
        // Chrome plating, a faint polishing grain along the barrel.
        Material& m = def(MaterialId::PenMetal, "shaders/materials/lacquer.glsl");
        m.params[0] = vec4(0.62f, 0.63f, 0.64f, 0.1f);
        m.params[1] = vec4(1.0f, 0.0f, 0.05f, 0.15f);
        m.params[2] = vec4(0.0f, 1.0f, 0.0f, 0.4f);
        polishTex(m);
    }
}

}  // namespace

bool init() {
    if (g_initialized) return true;
    for (int i = 0; i < int(MaterialId::Count); ++i) placeholderStandard(MaterialId(i), vec3(0.5f), 0.5f);
    g_tex = bakeAll();

    defineMarbles();

    defineWoods();
    defineFabrics();
    defineHall();
    defineScoresheet();

    // Owned by other packages: placeholders kept as before.
    placeholderStandard(MaterialId::ClockDisplay, vec3(0.45f, 0.48f, 0.40f), 0.2f, 0, 1, 0.02f);
    placeholderStandard(MaterialId::RobotPorcelain, vec3(0.92f, 0.91f, 0.89f), 0.3f, 0, 1, 0.05f, 0.4f);
    placeholderStandard(MaterialId::RobotJoint, vec3(0.03f), 0.45f, 0.6f);
    placeholderStandard(MaterialId::RobotEyeSclera, vec3(0.85f, 0.82f, 0.80f), 0.1f, 0, 1, 0.02f, 0.5f);
    placeholderStandard(MaterialId::RobotEyeIris, vec3(0.20f, 0.30f, 0.35f), 0.3f, 0, 1, 0.02f);
    placeholderStandard(MaterialId::RobotEyeCornea, vec3(1.0f), 0.02f);
    g_mats[int(MaterialId::RobotEyeCornea)].transparent = true;
    placeholderStandard(MaterialId::RobotLid, vec3(0.92f, 0.91f, 0.89f), 0.3f, 0, 1, 0.05f, 0.4f);
    placeholderStandard(MaterialId::Default, vec3(0.5f), 0.5f);
    g_initialized = true;
    return true;
}

void shutdown() {
    destroy(g_tex);
    if (g_blankPage) glDeleteTextures(1, &g_blankPage);
    if (g_blankEntry) glDeleteTextures(1, &g_blankEntry);
    g_blankPage = g_blankEntry = 0;
    g_initialized = false;
}
const Material& get(MaterialId id) { return g_mats[int(id)]; }
Material& getMutable(MaterialId id) { return g_mats[int(id)]; }
const char* name(MaterialId id) { return g_names[int(id)]; }
}  // namespace materials
