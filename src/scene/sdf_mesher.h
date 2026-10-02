// Signed distance field toolkit used by the procedural chess set (knight head, rook turret,
// bishop mitre, queen coronet, king cross):
//   * small analytic SDF helpers (smooth min/max, ellipsoids, capsules),
//   * Grid2D: a closed 2D polygon baked into an exact signed distance grid (fast lookups for
//     lathe profiles and silhouettes),
//   * meshSurfaceNets(): sparse surface-nets polygonisation of the zero level set with vertices
//     projected onto the true surface and normals from the field gradient (smooth, tessellation
//     friendly).
// Negative = inside. Units are whatever the caller uses (the chess set works in millimetres
// and scales the meshes to meters afterwards).
#pragma once
#include "../render/mesh.h"
#include <functional>
#include <vector>

namespace sdf {

using Field = std::function<float(m::vec3)>;

// ---- analytic helpers ---------------------------------------------------------------------
inline float smin(float a, float b, float k) {
    if (k <= 0.0f) return std::min(a, b);
    float h = std::max(k - std::fabs(a - b), 0.0f) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}
inline float smax(float a, float b, float k) { return -smin(-a, -b, k); }
// Ellipsoid (bound, iq's first-order approximation).
inline float sdEllipsoid(m::vec3 p, m::vec3 c, m::vec3 r) {
    m::vec3 q = p - c;
    float k0 = m::length(q / r), k1 = m::length(q / (r * r));
    return k1 > 1e-12f ? k0 * (k0 - 1.0f) / k1 : -std::min(r.x, std::min(r.y, r.z));
}
// Capsule / round cone between a (radius ra) and b (radius rb).
float sdRoundCone(m::vec3 p, m::vec3 a, m::vec3 b, float ra, float rb);
float sdSegment2(m::vec2 p, m::vec2 a, m::vec2 b);
// Extrusion of a 2D field d2 along an axis coordinate w (|w| <= halfLen) with rounded rims.
inline float extrudeRound(float d2, float w, float halfLen, float round) {
    float qx = d2 + round, qy = std::fabs(w) - halfLen + round;
    return std::min(std::max(qx, qy), 0.0f) + m::length(m::vec2(std::max(qx, 0.0f), std::max(qy, 0.0f))) - round;
}

// ---- baked 2D polygon distance ------------------------------------------------------------
class Grid2D {
public:
    // polygon: closed outline (last point connects to the first), any orientation.
    // cell: grid spacing; band: margin of the grid around the polygon. Distances are exact near
    // the outline and propagated (closest-point dead reckoning) over the whole grid; outside
    // the grid sample() returns a lower bound (band + distance to the grid).
    void build(const std::vector<m::vec2>& polygon, float cell, float band);
    float sample(m::vec2 p) const;  // bilinear, signed (negative inside)
    // Arc-length parameter (along the polygon, from point 0) of the closest boundary point.
    // Useful to lay details along an outline.
    float sampleParam(m::vec2 p) const;

private:
    int w_ = 0, h_ = 0;
    float cell_ = 1, band_ = 1, perimeter_ = 0;
    m::vec2 origin_;
    std::vector<float> d_, t_;
};

// ---- surface nets ---------------------------------------------------------------------------
struct MeshOptions {
    float cell = 0.3f;          // grid spacing
    int block = 4;              // culling block size (cells)
    float lipschitz = 1.2f;     // upper bound of |grad f| (culling safety)
    int projectIterations = 1;  // Newton steps projecting vertices onto the surface
};
struct MeshStats {
    int evaluations = 0;
    int vertices = 0;
    int triangles = 0;
};
// Polygonises { f = 0 } inside box. Output: positions, normals (normalised gradient), uv = 0,
// tangents unset. Counter-clockwise front faces seen from outside (f > 0).
MeshData meshSurfaceNets(const Field& f, const m::AABB& box, const MeshOptions& o, MeshStats* stats = nullptr);

}  // namespace sdf
