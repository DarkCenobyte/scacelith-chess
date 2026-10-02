// Implicit-surface toolkit used to sculpt the robot procedurally at startup.
//
// Shapes are written as signed distance functions (or smooth implicit functions whose zero set is
// the surface: exact distances are not required, only a sane gradient near the surface). The
// porcelain shells go through the volume mesher (Surface Nets + quadric decimation, meshVolume):
//
//   MeshData m = sdf::meshVolume(f, seeds, opts);   // f < 0 inside, seeds = points inside the shape
//
// Simple convex shapes (the hand's ball joints) use the shrink-wrap mesher instead: a capsule grid
// projected onto the zero set, then refined where curvature needs it:
//
//   MeshData m = sdf::meshSegment(f, a, b, opts);   // f < 0 inside, a..b a segment inside the shape
//
// Every ray from the segment (capsule parametrisation) must cross the surface once, i.e. the shape
// has to be star-shaped with respect to the segment. Normals come from the SDF gradient, so shading
// is perfectly smooth even on coarse triangles (and Phong tessellation rounds the silhouettes).
#pragma once
#include "../math/math.h"
#include "../render/mesh.h"
#include <functional>

namespace character {
namespace sdf {

using Fn = std::function<float(const m::vec3&)>;

// ---- primitives (Inigo Quilez' formulas) -----------------------------------------------------
inline float sphere(m::vec3 p, float r) { return m::length(p) - r; }
inline float ellipsoid(m::vec3 p, m::vec3 r) {
    float k0 = m::length(p / r), k1 = m::length(p / (r * r));
    return k1 > 1e-12f ? k0 * (k0 - 1.0f) / k1 : -std::min(r.x, std::min(r.y, r.z));
}
inline float capsule(m::vec3 p, m::vec3 a, m::vec3 b, float r) {
    m::vec3 pa = p - a, ba = b - a;
    float h = m::clamp(m::dot(pa, ba) / std::max(m::dot(ba, ba), 1e-12f), 0.0f, 1.0f);
    return m::length(pa - ba * h) - r;
}
// Cone with rounded ends: sphere r1 at a, sphere r2 at b, tangent cone in between (exact).
inline float roundCone(m::vec3 p, m::vec3 a, m::vec3 b, float r1, float r2) {
    m::vec3 ba = b - a;
    float l2 = m::dot(ba, ba), rr = r1 - r2, a2 = l2 - rr * rr, il2 = 1.0f / l2;
    m::vec3 pa = p - a;
    float y = m::dot(pa, ba), z = y - l2;
    m::vec3 xv = pa * l2 - ba * y;
    float x2 = m::dot(xv, xv), y2 = y * y * l2, z2 = z * z * l2;
    float k = m::sign(rr) * rr * rr * x2;
    if (m::sign(z) * a2 * z2 > k) return std::sqrt(x2 + z2) * il2 - r2;
    if (m::sign(y) * a2 * y2 < k) return std::sqrt(x2 + y2) * il2 - r1;
    return (std::sqrt(x2 * a2 * il2) + y * rr) * il2 - r1;
}
inline float roundBox(m::vec3 p, m::vec3 halfExtent, float r) {
    m::vec3 q = m::abs(p) - halfExtent + m::vec3(r);
    return m::length(m::max(q, m::vec3(0))) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f) - r;
}

// ---- operators -------------------------------------------------------------------------------
// Polynomial smooth min/max, k = blend radius (m).
inline float smin(float a, float b, float k) {
    if (k <= 0.0f) return std::min(a, b);
    float h = std::max(k - std::fabs(a - b), 0.0f) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}
inline float smax(float a, float b, float k) { return -smin(-a, -b, k); }

// Smooth 1D profile through control points (monotone cubic Hermite), clamped at the ends.
struct Profile {
    struct Key { float t, v; };
    std::vector<Key> keys;
    Profile(std::initializer_list<Key> k) : keys(k) {}
    float operator()(float t) const;
};

// ---- meshing ---------------------------------------------------------------------------------
struct MeshOptions {
    float maxEdge = 0.004f;       // split edges longer than this (m)
    float minEdge = 0.00025f;     // never split edges shorter than this
    float maxDeviation = 0.00005f;// split when the estimated chord error (L * angle / 8) exceeds this (m)
    float maxAngleDeg = 24.0f;    // and always when end normals differ more than this (shading)
    int maxIterations = 10;
    int nu = 48, nv = 48;         // base grid (around, along)
    m::vec3 tangentAxis{0, 1, 0}; // tangents follow this direction projected on the surface
    const char* name = "";        // for diagnostics
    float gradientStep = 4e-5f;
};

// Capsule shrink-wrap around segment a..b (a == b gives a sphere-like wrap; 'poleAxis' then
// orients the poles). Returns a closed mesh with smooth SDF normals.
MeshData meshSegment(const Fn& f, m::vec3 a, m::vec3 b, const MeshOptions& o, m::vec3 poleAxis = m::vec3(0, 1, 0));
// General mesher for shapes that are not star-shaped (sockets, cut-outs, palms): surface-following
// Surface Nets on a grid of 'cell' size, then quadric-error decimation down to 'maxError' (m, the
// allowed geometric deviation) with every vertex re-projected on the zero set. 'seeds' are points
// inside the shape (one per disconnected piece at least); only the surface around them is visited.
struct VolumeOptions {
    float cell = 0.0006f;
    float maxError = 0.00004f;
    float maxEdge = 0.004f;
    m::vec3 tangentAxis{0, 1, 0};
    const char* name = "";
    float gradientStep = 4e-5f;
};
MeshData meshVolume(const Fn& f, const std::vector<m::vec3>& seeds, const VolumeOptions& o);
// Adaptive refinement of an existing mesh lying on f's zero set (red/green splits of the marked edges).
void refine(MeshData& mesh, const Fn& f, const MeshOptions& o);
// Recomputes normals from the SDF gradient and tangents from o.tangentAxis.
void surfaceFrames(MeshData& mesh, const Fn& f, const MeshOptions& o);
m::vec3 gradient(const Fn& f, m::vec3 p, float h);
// Projects p onto the zero set with a few Newton steps along the gradient.
m::vec3 project(const Fn& f, m::vec3 p, float h, int iterations = 6);

}  // namespace sdf
}  // namespace character
