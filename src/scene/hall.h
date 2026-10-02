// The royal hall: procedural palatial architecture generated at startup, in world space.
//
//   Model hall = buildHall();   // ~0.1-0.3 s, see buildHall() for the part list
//
// Room (layout.h): interior faces at x = HALL_MIN_X..HALL_MAX_X (window wall at -X), z = HALL_MIN_Z..
// HALL_MAX_Z, floor y = 0, ceiling soffit y = HALL_HEIGHT. Walls are solid (they cast shadows) so
// the sun only enters through the three arched windows of the -X wall (WALL_THICKNESS deep).
//
// Vertical program of every wall (classical order on pedestals):
//   0 .. DADO_TOP                 wood wainscot (WallPanelWood) with raised panels; pedestals under
//                                 the pilasters; the dado cap is flush with the window sills
//   DADO_TOP .. SHAFT_BOTTOM      gilded attic bases (GildedTrim)
//   SHAFT_BOTTOM .. CAPITAL_BOTTOM  fluted limestone pilaster shafts (WallStone)
//   CAPITAL_BOTTOM .. ENTABLATURE_BOTTOM  gilded Corinthian capitals
//   ENTABLATURE_BOTTOM .. HALL_HEIGHT  architrave, frieze, dentil cornice with gilded cymatium
//   HALL_HEIGHT                   coffered ceiling soffit; coffers recessed up to COFFER_TOP, the
//                                 central painted plafond above the table up to PLAFOND_TOP
//
// Material conventions (for the material library):
//   * FloorMarble / FloorMarbleInlay: exactly flat at y = 0, normal +Y, uv = world (x, z) in meters,
//     tangent +X. See the floor grid below. Part flags: DRAW_STATIC | DRAW_CAST_SHADOW.
//   * WallStone, WallPanelWood, GildedTrim, WindowFrame, Brass, Curtain: uv in meters along the
//     surface (planar faces: world-aligned projection; sweeps: (length along path, length along
//     profile)); positionOS == world position (the hall model is in world space).
//   * CeilingPainted: coffer panels and the plafond face down at y = COFFER_TOP / PLAFOND_TOP with
//     uv = world (x, z) in meters. Paintings (framed canvases on the walls) are a separate part
//     "hall_paintings" (CeilingPainted) with uv in [0,1] over each canvas; its inst[0].x = 1 is
//     reserved for a painting mode that ceiling.glsl does not implement (it ignores instParams).
//   * WindowGlass: two thin faces per window (transparent, no shadow casting); uv = (distance along
//     the wall from the opening's left edge seen from inside, height above the sill) in meters.
//   * Tapestry: one part per tapestry, in tapestry.glsl's layout: inst[0] = (colour 0 blue / 1 red,
//     pattern seed, extra seed, 0), inst[1] = (width m, height m, 0, 0). uv spans [0,1]^2 over the
//     woven area: (0,0) = bottom-left seen from the front, (1,1) = top-right (all hall tapestries
//     are TAPESTRY_W x TAPESTRY_H except the two narrower ones of the -Z wall, TAPESTRY_NARROW_W).
//     The back face (towards the wall) reuses the same uv.
#pragma once
#include "../game/layout.h"
#include "model.h"

namespace hall {

// ---- Walls -----------------------------------------------------------------------------------
constexpr float DADO_TOP = layout::WINDOW_SILL_Y;  // 0.90: wainscot cap = window sill level
constexpr float SHAFT_BOTTOM = 1.20f;
constexpr float CAPITAL_BOTTOM = 7.20f;
constexpr float ENTABLATURE_BOTTOM = 7.85f;
constexpr float CORNICE_PROJECTION = 0.53f;           // cornice top edge distance from the walls
constexpr float PILASTER_WIDTH = 0.65f;
constexpr float PILASTER_PROJECTION = 0.12f;
constexpr float WALL_THICKNESS_OTHER = 0.60f;         // +X and end walls (window wall: layout::WALL_THICKNESS)
constexpr float WINDOW_SPLAY = 0.20f;                 // jamb splay per side at the interior face
constexpr float WINDOW_FRAME_DEPTH = 0.60f;           // frame plane distance from the interior face
constexpr float WINDOW_SPRING_Y = layout::WINDOW_TOP_Y - layout::WINDOW_WIDTH * 0.5f;  // 6.1
// Double doors in the centre of the -Z wall.
constexpr float DOOR_WIDTH = 2.20f;
constexpr float DOOR_HEIGHT = 4.40f;

// ---- Ceiling ---------------------------------------------------------------------------------
constexpr float COFFER_TOP = layout::HALL_HEIGHT + 0.22f;
constexpr float PLAFOND_TOP = layout::HALL_HEIGHT + 0.36f;
constexpr float ROOF_TOP = layout::HALL_HEIGHT + 0.90f;

// ---- Floor -----------------------------------------------------------------------------------
// Field: FLOOR_TILE square tiles, 15 x 25 of them covering |x| <= FLOOR_FIELD_HALF_X,
// |z| <= FLOOR_FIELD_HALF_Z. Tile (i, j) spans x in [-FLOOR_FIELD_HALF_X + i*T, +T],
// z in [-FLOOR_FIELD_HALF_Z + j*T, +T] (T = FLOOR_TILE), so the table centre (0,0) is the centre
// of tile (7, 12). Every tile corner is cut by a right triangle of legs FLOOR_CABOCHON: the four
// cuts around each grid vertex form a dark diamond "cabochon" (FloorMarbleInlay); along the field
// edge they form half diamonds. Joints: every tile edge (field) and every cabochon edge.
constexpr float FLOOR_TILE = 0.80f;
constexpr float FLOOR_FIELD_HALF_X = 6.0f;
constexpr float FLOOR_FIELD_HALF_Z = 10.0f;
constexpr float FLOOR_CABOCHON = 0.12f;
constexpr int FLOOR_TILES_X = 15, FLOOR_TILES_Z = 25;
// Border between the field and the walls, by wall distance d = min(7 - |x|, 11 - |z|):
//   [0, BAND0)        FloorMarble perimeter slabs (joints every FLOOR_BORDER_SLAB along the wall,
//                     mitred at the corners)
//   [BAND0, BAND1)    FloorMarbleInlay outer band
//   [BAND1, BAND2)    FloorMarble inner border (joints every FLOOR_TILE, aligned with the field)
//   [BAND2, BAND3)    FloorMarbleInlay fillet; BAND3 = the field edge
// The door threshold (x in +-DOOR_WIDTH/2, z in [HALL_MIN_Z - 0.2, HALL_MIN_Z], extending under the
// closed leaves at HALL_MIN_Z - 0.12) is FloorMarbleInlay.
constexpr float FLOOR_BAND0 = 0.30f, FLOOR_BAND1 = 0.55f, FLOOR_BAND2 = 0.90f, FLOOR_BAND3 = 1.00f;
constexpr float FLOOR_BORDER_SLAB = 1.20f;

// Field tile containing (x, z), or (-1, -1) outside the field.
inline m::ivec2 floorTile(float x, float z) {
    if (std::fabs(x) >= FLOOR_FIELD_HALF_X || std::fabs(z) >= FLOOR_FIELD_HALF_Z) return {-1, -1};
    return {int((x + FLOOR_FIELD_HALF_X) / FLOOR_TILE), int((z + FLOOR_FIELD_HALF_Z) / FLOOR_TILE)};
}
// Distance to the nearest wall (floor border bands).
inline float floorWallDistance(float x, float z) {
    return std::min(layout::HALL_MAX_X - std::fabs(x), layout::HALL_MAX_Z - std::fabs(z));
}

// ---- Tapestries -------------------------------------------------------------------------------
constexpr float TAPESTRY_W = 3.2f, TAPESTRY_H = 5.0f;
constexpr float TAPESTRY_NARROW_W = 2.8f;
constexpr float TAPESTRY_TOP_Y = 6.70f;

// ---- Chandelier ------------------------------------------------------------------------------
constexpr float CHANDELIER_Y = 5.60f;      // height of the lower candle ring (centre of the body)
constexpr float CHANDELIER_SCALE = 1.4f;   // body: lower ring radius 1.2 m, 4.0 m .. 7.9 m high
// World positions of the chandelier's 18 candle flames (12 lower + 6 upper), e.g. for point lights.
void chandelierCandles(std::vector<m::vec3>& flames);

struct BuildStats {
    size_t triangles = 0, vertices = 0, parts = 0;
    double seconds = 0.0;
};

// Whole room: floor, walls, windows, curtains, tapestries, ceiling, chandelier, doors, props and
// the exterior terrace. Part names start with "hall_".
Model buildHall(BuildStats* stats = nullptr);

// Suggested sun direction (towards the sun) for the hall: low afternoon sun from the -X side so
// the windows cast long patches on the floor in front of and beside the table.
inline m::vec3 recommendedSunDirection() { return m::normalize(m::vec3(-0.70f, 0.52f, 0.49f)); }

}  // namespace hall
