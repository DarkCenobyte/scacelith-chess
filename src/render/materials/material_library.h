// Material library: every material of the game, identified by MaterialId. Geometry generators
// (scene/, character/) tag their parts with a MaterialId; the library (render-materials work
// package) provides the Material (surface shader + params + baked textures).
// Surface shaders live in shaders/materials/ (each .glsl documents its params[] and inst[]);
// procedural textures are baked by compute shaders (shaders/materials/bake/) in init().
//
// Per-object variation goes through DrawItem::inst[] (SurfaceInput.instParams) and objectId:
//   * MarbleWhitePiece / MarbleBlackPiece: inst[0].x = piece seed (unique veins per piece; the
//     veins are volumetric in positionOS, so they stay on the piece when it moves)
//   * BoardSquareLight / BoardSquareDark: one mesh for all squares of a colour; use uv and
//     positionOS to vary each square (square index = floor(uv*8), see scene/board.h); every
//     square is a different slab, hairline joints are drawn at the square edges (55 mm squares)
//   * BoardFrame: positionOS + objectId (Nero Portoro)
//   * FloorMarble / FloorMarbleInlay: world space (positionWS.xz, meters, uv ignored); tile grid
//     in params[4] = (tile size, joint width, grid origin x, grid origin z), default 0.8 m tiles,
//     1.5 mm joints, origin (0,0) (inlay: 0.4 m). Set planarReflector for mirror reflections.
//   * WallStone: world-space ashlar (blocks on the dominant plane of the normal), params[5].xyz =
//     grid origin
//   * TableWood / TableWoodCarved / ChairWood / WallPanelWood / ClockCase: inst[0].xyz = grain
//     (log) axis in object space (0 = material default), inst[0].w = seed (0 = objectId)
//   * Tapestry: inst[0].x = 0 royal blue, 1 royal red; inst[0].y = pattern seed (< 0.5
//     fleur-de-lis trellis, >= 0.5 damask pomegranate); inst[0].z = extra seed; inst[1].xy =
//     hanging size in m (0 = 2.4 x 3.6); uv spans [0,1] over the whole hanging (v up)
//   * CeilingPainted: one coffer per uv unit (fract(uv)); the border is painted in the panel
//   * WindowGlass: inst[0].xy = panes across u / v (dust gathers at each pane bottom; 0 = 1 x 1)
//   * CandleWax: params[3].x = top of the candle (object space y); params[2] = flame glow
//     (rgb nits, falloff m), 0 = unlit
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
