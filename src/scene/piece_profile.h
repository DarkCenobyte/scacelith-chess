// 2D profile authoring for turned (lathe) shapes and swept mouldings, shared by the chess set,
// the board frame and the clock.
//
//   Profile p;                      // points are (x = radius or inward offset, y = height)
//   p.to(0, 0).to(10, 0).arc(...).cubic(...);
//   std::vector<vec2> pts = finishProfile(p, 0.35f, 0.5f);   // round sharp corners, resample
//
// finishProfile() rounds every corner sharper than ~25 degrees with a small fillet (no razor
// edges) and subdivides long segments so smooth normals interpolate well.
#pragma once
#include "../render/mesh.h"
#include <vector>

struct Profile {
    std::vector<m::vec2> pts;
    std::vector<float> radius;  // per point corner radius (-1 = default, 0 = keep sharp)

    Profile& to(float x, float y, float cornerRadius = -1.0f);
    Profile& to(m::vec2 p, float cornerRadius = -1.0f) { return to(p.x, p.y, cornerRadius); }
    // Circle / ellipse arc around c from angle a0 to a1 (degrees, 0 = +x, 90 = +y). The start
    // point is appended too (merged when it coincides with the last point).
    Profile& arc(m::vec2 c, float r, float a0, float a1) { return ellipse(c, m::vec2(r, r), a0, a1); }
    Profile& ellipse(m::vec2 c, m::vec2 r, float a0, float a1);
    // Bezier curves from the last point.
    Profile& cubic(m::vec2 c1, m::vec2 c2, m::vec2 p);
    Profile& quad(m::vec2 c, m::vec2 p);
    // Catmull-Rom spline through the given points (starting from the last point).
    Profile& spline(const std::vector<m::vec2>& through);
    m::vec2 last() const { return pts.empty() ? m::vec2(0, 0) : pts.back(); }
};

// Rounds corners (turn angle > minAngleDeg) with fillets of radius 'fillet' (or the per point
// radius), then subdivides segments longer than maxSeg. Open polyline: end points are kept.
std::vector<m::vec2> finishProfile(const Profile& p, float fillet, float maxSeg, float minAngleDeg = 25.0f);
// Same on a closed outline (every vertex may be rounded).
std::vector<m::vec2> finishClosed(const Profile& p, float fillet, float maxSeg, float minAngleDeg = 25.0f);

// Lathe around +Y from a dense bottom-to-top profile: normals come from the dense curve, then
// the profile is simplified (Douglas-Peucker, tolerance tol) and long segments subdivided to
// maxSeg. u = angle / 2pi (seam at +X), v = y / vScale, tangents around the axis.
MeshData latheProfile(const std::vector<m::vec2>& profile, int segments, float vScale, float tol = 0.02f, float maxSeg = 2.5f);

// Sweeps a moulding profile around a rounded rectangle in the XZ plane (y up).
// profile: (x = inward offset from the outline, y = height), ordered so that the solid is on
// the left when walking the profile with "outward" to the right (e.g. top face from the inside
// to the outer edge, then down the side). Each profile point follows the outline inset by its
// offset (corner radius max(rc - offset, 0)). Straight edges are not subdivided.
MeshData sweepRoundedRect(const std::vector<m::vec2>& profile, float halfX, float halfZ, float cornerRadius, int cornerSegs);

// Rounded prism: a convex polygon in XY (counter-clockwise, sharp corners) whose corners are
// rounded with cornerRadius, extruded along Z over [-halfLen, halfLen] with rounded ends of
// radius endRadius (<= cornerRadius). uv = (z, perimeter distance), tangent = +Z.
MeshData roundedPrism(const std::vector<m::vec2>& convexPoly, float cornerRadius, float halfLen, float endRadius,
                      int cornerSegs, int endSegs, int lengthSegs = 1);

// Flips triangles whose geometric normal disagrees with their vertex normals.
void fixWinding(MeshData& d);
// Sets tangents to the circumferential direction around +Y (lathe-like), orthogonalised.
void setAxialTangents(MeshData& d);
