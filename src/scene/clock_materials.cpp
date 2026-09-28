// ClockDisplay material: reflective seven-segment LCD drawn procedurally by
// shaders/materials/clock_display.glsl from DrawItem::inst[0] (see clock_model.h).
#include "clock_model.h"
#include "../render/materials/material_library.h"

using namespace m;

void setupClockMaterials() {
    Material& mat = materials::getMutable(MaterialId::ClockDisplay);
    mat.name = "ClockDisplay";
    mat.surface = "shaders/materials/clock_display.glsl";
    mat.displacement.clear();
    mat.defines.clear();
    mat.transparent = false;
    mat.doubleSided = false;
    mat.tessellated = false;
    mat.castShadow = true;
    mat.planarReflector = -1;
    for (auto& p : mat.params) p = vec4(0);
    for (auto& t : mat.textures) t = 0;
    // x = window aspect (W/H), y = italic slant, z = reflector depth behind the segments
    // (fraction of the window height), w = ghost segment strength.
    mat.params[0] = vec4(CLOCK_LCD_W / CLOCK_LCD_H, 0.09f, 0.045f, 0.045f);
    // Background (polariser + transflective reflector): pale grey-green, fairly diffuse.
    mat.params[1] = vec4(0.22f, 0.255f, 0.19f, 0.5f);
    // Segments: near black, slightly blue; w = roughness.
    mat.params[2] = vec4(0.010f, 0.012f, 0.016f, 0.6f);
    // x = window clear coat (anti-glare plastic), y = its roughness, z = segment shadow strength,
    // w = contrast at grazing angles.
    mat.params[3] = vec4(0.6f, 0.09f, 0.38f, 0.35f);
}
