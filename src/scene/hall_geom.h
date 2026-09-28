// Private procedural-geometry toolkit shared by the hall (hall*.cpp) and furniture generators.
// Not a public module interface: include it only from src/scene/hall*.cpp and furniture.cpp.
//
// Conventions
//  * Every helper appends to a MeshData in the caller's space (usually world space).
//  * Triangles are oriented automatically: tri()/quad() pick the winding whose geometric normal
//    agrees with the vertex normals, so generators only have to get the normals right.
//  * 2D profiles (mouldings, lathes) are polylines (x = horizontal offset b, y = vertical offset a)
//    walked along the exposed surface with the SOLID ON THE LEFT (x to the right, y up). A
//    duplicated point marks a hard crease (like prim::lathe).
#pragma once
#include "../render/materials/material_library.h"
#include "../render/mesh.h"
#include <functional>
#include <vector>

namespace hallgeo {

using m::mat4;
using m::vec2;
using m::vec3;
using m::vec4;
using Profile = std::vector<vec2>;

// ---- Low level -----------------------------------------------------------------------------
uint32_t vtx(MeshData& d, vec3 p, vec3 n, vec3 t, vec2 uv);
void tri(MeshData& d, uint32_t a, uint32_t b, uint32_t c);
void quad(MeshData& d, uint32_t a, uint32_t b, uint32_t c, uint32_t e);  // ring a-b-c-e

// Flat quad a-b-c-e (any winding) facing n. uv = planar projection in meters (axis uAxis).
void quadFlat(MeshData& d, vec3 a, vec3 b, vec3 c, vec3 e, vec3 n, vec3 uAxis);
// Flat convex polygon (fan from the first vertex), facing n.
void polygon(MeshData& d, const std::vector<vec3>& pts, vec3 n, vec3 uAxis);

enum BoxFace : unsigned { F_PX = 1, F_NX = 2, F_PY = 4, F_NY = 8, F_PZ = 16, F_NZ = 32, F_ALL = 63 };
// Axis-aligned box, uv = world meters per face.
void boxAA(MeshData& d, vec3 lo, vec3 hi, unsigned faces = F_ALL);
// Oriented box: centre c, orthonormal axes, half extents h (faces mask uses the local axes).
void box(MeshData& d, vec3 c, vec3 ax, vec3 ay, vec3 az, vec3 h, unsigned faces = F_ALL);
// Box transformed by an arbitrary affine matrix (unit cube [-1,1]^3 scaled by h first).
void boxXf(MeshData& d, const mat4& xf, vec3 h, unsigned faces = F_ALL);

// ---- Parametric surfaces ----------------------------------------------------------------------
using SurfFn = std::function<vec3(float, float)>;
enum class UVMode { Param, Meters };
// Grid over the parameter samples us x vs (increasing, typically in [0,1]). Normals from the grid
// differences, oriented along cross(dP/du, dP/dv) (reversed when flip). closedU welds the seam
// normals (u = first and last sample coincide).
void surfaceGrid(MeshData& d, const std::vector<float>& us, const std::vector<float>& vs, const SurfFn& fn,
                 bool flip = false, UVMode uvMode = UVMode::Meters, vec2 uvScale = vec2(1, 1), bool closedU = false);
void surface(MeshData& d, int nu, int nv, const SurfFn& fn, bool flip = false, UVMode uvMode = UVMode::Meters,
             vec2 uvScale = vec2(1, 1), bool closedU = false);
std::vector<float> linspace(float a, float b, int n);  // n+1 samples
// Same as surface() but the orientation is chosen so that the normals agree with refDir on average.
void surfaceFacing(MeshData& d, int nu, int nv, const SurfFn& fn, vec3 refDir, UVMode uvMode = UVMode::Meters,
                   vec2 uvScale = vec2(1, 1), bool closedU = false);

// ---- Sweeps -----------------------------------------------------------------------------------
// Sweeps a profile along a polyline lying in a plane of normal W. For each path point the profile
// point (b, a) is placed at P + s*b + W*a with s = cross(W, tangent) (mitred at corners). Hard
// corners (> creaseDeg) get split normals. uv = (path length, profile length) in meters.
void sweep(MeshData& d, const Profile& prof, const std::vector<vec3>& path, vec3 W, bool closed,
           float creaseDeg = 30.0f, bool capStart = false, bool capEnd = false);
// Reverses a path (and therefore flips the side s points to).
std::vector<vec3> reversed(const std::vector<vec3>& p);
// The mitred offset curve a sweep would place a profile point (b, 0) on.
std::vector<vec3> offsetPath(const std::vector<vec3>& path, vec3 W, float b, bool closed);
// Direction s of the first path segment (cross(W, tangent)).
vec3 sweepSide(const std::vector<vec3>& path, vec3 W);

// ---- Lathes -----------------------------------------------------------------------------------
// Surface of revolution around the local +Y axis, appended with xf.
void lathe(MeshData& d, const Profile& prof, int seg, const mat4& xf);
// Lathe whose radius is modulated: r' = r * mod(angle, heightFraction) (flutes, gadroons...).
void latheMod(MeshData& d, const Profile& prof, int seg, const mat4& xf,
              const std::function<float(float, float, float)>& radiusFn /* (r, angle, y) -> r' */,
              int subdiv = 1);

// ---- Furniture primitives --------------------------------------------------------------------
// Louis XVI leg: square block on top (blockH tall, blockHalf half width), turned collar, fluted
// tapering shaft, ring and toupie foot. Base (foot tip) at 'base', total height h, radius at the
// top of the shaft rTop. Appended to 'd'; the square block is appended to 'blockMesh'.
void louisLeg(MeshData& d, MeshData& blockMesh, vec3 base, float h, float rTop, float blockH, float blockHalf, int flutes = 10);

// ---- Misc -------------------------------------------------------------------------------------
// Arched opening outline (clockwise seen from the front with u right / v up): starts at
// (uc-w, v0), goes up the left jamb, over the semicircular arch (springing vs) and down the right
// jamb. Returned as (u, v) pairs.
std::vector<vec2> archOutline(float uc, float w, float v0, float vs, int archSeg);

// Per-material accumulation of generated geometry.
struct Accum {
    MeshData mesh[int(MaterialId::Count)];
    MeshData& operator[](MaterialId id) { return mesh[int(id)]; }
};

inline vec3 lerp3(vec3 a, vec3 b, float t) { return a + (b - a) * t; }
inline float sq(float x) { return x * x; }
size_t triangleCount(const MeshData& d);

}  // namespace hallgeo
