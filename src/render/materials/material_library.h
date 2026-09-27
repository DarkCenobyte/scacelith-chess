// Material library: every material of the game, identified by MaterialId. Geometry generators
// (scene/, character/) tag their parts with a MaterialId; the library (render-materials work
// package) provides the Material (surface shader + params + baked textures).
//
// Per-object variation goes through DrawItem::inst[] (SurfaceInput.instParams) and objectId:
//   * MarbleWhitePiece / MarbleBlackPiece: inst[0].x = piece seed (unique veins per piece)
//   * BoardSquareLight / BoardSquareDark: one mesh for all squares of a colour; use uv and
//     positionOS to vary each square (square index = floor(uv*8), see scene/board.h)
//   * Tapestry: inst[0].x = 0 royal blue, 1 royal red; inst[0].y = pattern seed
//   * ClockDisplay: inst[0] = (white ms, black ms, flags, running side) see scene/clock_model.h
//   * RobotEyeIris: inst[0].x = pupil dilation [0,1]
#pragma once
#include "../material.h"

enum class MaterialId : int {
    // Chess set
    MarbleWhitePiece,     // polished white/cream Carrara-like marble, fine grey veins, SSS
    MarbleBlackPiece,     // polished Nero Marquina: deep black with thin white/gold veins
    PieceFelt,            // green baize under each piece's base
    BoardSquareLight,     // board inlay squares (light marble)
    BoardSquareDark,      // board inlay squares (dark marble)
    BoardFrame,           // board border: darker marble or rosewood frame
    // Furniture
    TableWood,            // waxed, very glossy walnut/mahogany top (clear coat)
    TableWoodCarved,      // legs / apron, satin
    ChairWood,            // carved gilded/dark wood chair frame
    ChairVelvet,          // deep red velvet seat and back (sheen)
    // Hall
    FloorMarble,          // large polished cream-white marble tiles (planar reflections)
    FloorMarbleInlay,     // darker marble borders / cabochons in the floor pattern
    WallStone,            // pale limestone / plaster walls
    WallPanelWood,        // lower wall wainscot
    GildedTrim,           // gold leaf mouldings, frames, capitals
    Tapestry,             // woven royal blue / red tapestries with gold motifs
    CeilingPainted,       // coffered ceiling panels
    WindowGlass,          // slightly imperfect old glass (transparent)
    WindowFrame,          // painted wood / iron mullions
    Curtain,              // heavy velvet drapes beside the windows
    Brass,                // brass fittings, chandelier arms
    Crystal,              // chandelier crystal drops (transparent)
    CandleWax,
    // Clock
    ClockCase,            // walnut wood case, satin lacquer
    ClockDisplay,         // reflective LCD with segment digits (dynamic, inst params)
    ClockLever,           // lever / buttons (black lacquer or brass)
    ClockPanel,           // front panel around the displays (brushed metal / black plastic)
    // Robots
    RobotPorcelain,       // white glazed composite/porcelain shells (clear coat, subtle SSS)
    RobotJoint,           // dark satin metal / rubber joints and gaps
    RobotEyeSclera,       // eye white (wet, slightly veined)
    RobotEyeIris,         // iris with depth (parallax/refraction), pupil
    RobotEyeCornea,       // transparent cornea bulge (specular highlight)
    RobotLid,             // eyelid shells (porcelain, same as body)
    // Generic
    Default,              // standard.glsl, neutral grey
    Count
};

namespace materials {
// Builds every material (bakes procedural textures with compute shaders). GL context required.
bool init();
void shutdown();
const Material& get(MaterialId id);
Material& getMutable(MaterialId id);
const char* name(MaterialId id);
}  // namespace materials
