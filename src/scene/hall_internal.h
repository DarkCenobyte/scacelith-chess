// Internal interface between the hall generator files (hall*.cpp). Not for other modules.
#pragma once
#include "hall.h"
#include "hall_geom.h"
#include <unordered_map>

namespace hall {
namespace detail {

using namespace hallgeo;

// One wall of the room seen from inside: u runs to the right along the wall, v = world height,
// n = distance out of the wall into the room. N = cross(U, +Y).
struct WallDef {
    vec3 origin;                   // left end (seen from inside), floor level, on the interior face
    vec3 U, N;
    float length = 0;
    std::vector<float> pilasters;  // u of the pilaster axes
    vec3 P(float u, float v, float n = 0.0f) const { return origin + U * u + vec3(0, v, 0) + N * n; }
    vec3 dir(float du, float dv, float dn) const { return U * du + vec3(0, dv, 0) + N * dn; }
};
enum WallId { WALL_WINDOWS = 0, WALL_EAST = 1, WALL_NORTH = 2, WALL_SOUTH = 3 };  // -X, +X, -Z (door), +Z
const WallDef& wall(int id);
// u of the window axes on the window wall (z = +5.2, 0, -5.2).
float windowU(int i);
constexpr float WIN_HALF = layout::WINDOW_WIDTH * 0.5f;            // frame opening half width
constexpr float WIN_HALF_IN = WIN_HALF + WINDOW_SPLAY;             // at the interior face
constexpr float DOOR_U = 7.0f;                                     // on the -Z wall (x = 0)

// Straight bar (box) between two wall-space points (u, v) at depth n, width w (in the wall plane)
// and depth dd (along N).
void wallBar(MeshData& d, const WallDef& w, vec2 a, vec2 b, float n, float width, float depth, unsigned faces = F_ALL);
// Path helpers (world points) for sweeps on a wall.
vec3 wp(const WallDef& w, vec2 uv, float n = 0.0f);

// Generators (world space). Each appends to the per-material accumulators.
void buildFloor(Accum& a);
void buildShell(Accum& a);            // wall solids, window reveals and sills, roof, exterior terrace
void buildOrders(Accum& a);           // wainscot, pedestals, pilasters, entablature
void buildCeiling(Accum& a);          // coffered ceiling and plafond
// Soffit grid lines of the ceiling (coffer edges), strictly inside the cornice edge; the
// entablature sweep inserts path points at them so the cornice and ceiling share vertices.
void ceilingGrid(std::vector<float>& xs, std::vector<float>& zs);
void buildWindows(Accum& a, MeshData& glass);  // frames, glazing, curtains, rods
void buildDoor(Accum& a, MeshData& paintings);
void buildChandelier(Accum& a);
void buildTapestries(Accum& a, std::vector<ModelPart>& parts);
void buildProps(Accum& a, MeshData& paintings);

// Shared ornament helpers (hall_ornament.cpp).
// Bell samples of corinthianCapital (outline point + relief per surface sample): they depend only
// on the capital's size, so the pilasters of one size share them. Refilled when the size changes.
struct CapitalCache {
    float y0 = 0, y1 = 0, hw = 0, p = 0;
    std::unordered_map<uint64_t, vec2> bell;  // key: bit patterns of the sample (s, t)
};
// Corinthian capital relief wrapped around a pilaster: shaft outline half width hw, projection p.
void corinthianCapital(MeshData& d, const WallDef& w, float u, float y0, float y1, float hw, float p, CapitalCache* cache = nullptr);
// Gilded rosette (flower) facing 'n', centred at c, radius r.
void rosette(MeshData& d, vec3 c, vec3 n, float r, int petals, float depth);
// Picture frame + canvas on a wall: centre (u, v), canvas size (cw, ch), frame width fw.
void framedPainting(Accum& a, MeshData& canvas, const WallDef& w, float u, float v, float cw, float ch, float fw, float n0 = 0.0f);
// Raised panel on a wall (bevelled field + small bead moulding), outline [u0,u1]x[v0,v1] on the
// plane n0.
void raisedPanel(MeshData& wood, MeshData& bead, const WallDef& w, float u0, float u1, float v0, float v1, float n0,
                 float bevel, float raise);
// Classical urn (gilded bronze) standing at base point p, height h.
void urn(MeshData& d, vec3 p, float h);

}  // namespace detail
}  // namespace hall
