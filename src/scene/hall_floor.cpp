// Hall floor: polished cream marble field of octagonal tiles with dark cabochons, framed by inlaid
// border bands (layout documented in hall.h). Exactly flat at y = 0, uv = world (x, z).
#include "hall_internal.h"

using namespace m;

namespace hall {
namespace detail {

namespace {

const vec3 UP(0, 1, 0);

void flatPoly(MeshData& d, const std::vector<vec2>& xz) {
    if (xz.size() < 3) return;
    uint32_t base = uint32_t(d.vertices.size());
    for (vec2 p : xz) {
        vtx(d, vec3(p.x, 0.0f, p.y), UP, vec3(1, 0, 0), vec2(p.x, p.y));
        d.vertices.back().tangent.w = -1.0f;  // bitangent = +Z = direction of increasing uv.y
    }
    for (uint32_t i = 1; i + 1 < xz.size(); ++i) tri(d, base, base + i, base + i + 1);
}

// Sutherland-Hodgman clip of a convex polygon against |x| <= hx, |z| <= hz.
std::vector<vec2> clipRect(std::vector<vec2> poly, float hx, float hz) {
    auto clipEdge = [](const std::vector<vec2>& in, int axis, float lim, float sgn) {
        std::vector<vec2> out;
        size_t n = in.size();
        for (size_t i = 0; i < n; ++i) {
            vec2 a = in[i], b = in[(i + 1) % n];
            float da = sgn * a[axis] - lim, db = sgn * b[axis] - lim;  // inside when <= 0
            if (da <= 1e-6f) out.push_back(a);
            if ((da < -1e-6f && db > 1e-6f) || (da > 1e-6f && db < -1e-6f)) {
                float t = da / (da - db);
                out.push_back(a + (b - a) * t);
            }
        }
        return out;
    };
    poly = clipEdge(poly, 0, hx, 1.0f);
    if (poly.size() >= 3) poly = clipEdge(poly, 0, hx, -1.0f);
    if (poly.size() >= 3) poly = clipEdge(poly, 1, hz, 1.0f);
    if (poly.size() >= 3) poly = clipEdge(poly, 1, hz, -1.0f);
    return poly;
}

// Rectangular ring between half extents (ix, iz) (inner) and (ox, oz) (outer), mitred corners,
// each side split at the breakpoints (interior x positions for the +-z sides, z for the +-x sides)
// so neighbouring rings and the field share vertices (no T-junctions).
void ring(MeshData& d, float ix, float iz, float ox, float oz, const std::vector<float>& bx, const std::vector<float>& bz) {
    auto side = [&](bool alongX, float sgn) {
        const std::vector<float>& b = alongX ? bx : bz;
        float ih = alongX ? ix : iz, oh = alongX ? ox : oz;   // half length of the inner / outer edge
        float ic = alongX ? iz : ix, oc = alongX ? oz : ox;   // perpendicular coordinate of the edges
        std::vector<float> inner = {-ih}, outer = {-oh};
        for (float v : b) { inner.push_back(v); outer.push_back(v); }  // b lies strictly inside the field
        inner.push_back(ih);
        outer.push_back(oh);
        auto P = [&](float along, float perp) { return alongX ? vec2(along, sgn * perp) : vec2(sgn * perp, along); };
        for (size_t k = 0; k + 1 < inner.size(); ++k)
            flatPoly(d, {P(inner[k], ic), P(inner[k + 1], ic), P(outer[k + 1], oc), P(outer[k], oc)});
    };
    side(true, 1.0f);
    side(true, -1.0f);
    side(false, 1.0f);
    side(false, -1.0f);
}

}  // namespace

void buildFloor(Accum& a) {
    MeshData& field = a[MaterialId::FloorMarble];
    MeshData& inlay = a[MaterialId::FloorMarbleInlay];
    const float T = FLOOR_TILE, c = FLOOR_CABOCHON;
    const float hx = FLOOR_FIELD_HALF_X, hz = FLOOR_FIELD_HALF_Z;
    // Field: octagonal tiles.
    for (int j = 0; j < FLOOR_TILES_Z; ++j)
        for (int i = 0; i < FLOOR_TILES_X; ++i) {
            float x0 = -hx + T * float(i), x1 = x0 + T, z0 = -hz + T * float(j), z1 = z0 + T;
            flatPoly(field, {{x0 + c, z0}, {x1 - c, z0}, {x1, z0 + c}, {x1, z1 - c}, {x1 - c, z1}, {x0 + c, z1}, {x0, z1 - c}, {x0, z0 + c}});
        }
    // Cabochons at every grid vertex (clipped to the field: half / quarter diamonds on the edge).
    for (int j = 0; j <= FLOOR_TILES_Z; ++j)
        for (int i = 0; i <= FLOOR_TILES_X; ++i) {
            float x = -hx + T * float(i), z = -hz + T * float(j);
            std::vector<vec2> dmd = clipRect({{x - c, z}, {x, z - c}, {x + c, z}, {x, z + c}}, hx, hz);
            flatPoly(inlay, dmd);
        }
    // Border bands. Breakpoints: every field joint +- c along each side.
    std::vector<float> bx, bz;
    for (int i = 0; i <= FLOOR_TILES_X; ++i)
        for (float x : {-hx + T * float(i) - c, -hx + T * float(i) + c})
            if (std::fabs(x) < hx - 1e-4f) bx.push_back(x);
    for (int j = 0; j <= FLOOR_TILES_Z; ++j)
        for (float z : {-hz + T * float(j) - c, -hz + T * float(j) + c})
            if (std::fabs(z) < hz - 1e-4f) bz.push_back(z);
    const float W = layout::HALL_MAX_X, L = layout::HALL_MAX_Z;
    const float d[5] = {FLOOR_BAND3, FLOOR_BAND2, FLOOR_BAND1, FLOOR_BAND0, 0.0f};
    MeshData* mats[4] = {&inlay, &field, &inlay, &field};
    for (int k = 0; k < 4; ++k) ring(*mats[k], W - d[k], L - d[k], W - d[k + 1], L - d[k + 1], bx, bz);
    // Door threshold (extends under the closed leaves).
    float hw = DOOR_WIDTH * 0.5f, z0 = layout::HALL_MIN_Z;
    flatPoly(inlay, {{-hw, z0}, {hw, z0}, {hw, z0 - 0.2f}, {-hw, z0 - 0.2f}});
}

}  // namespace detail
}  // namespace hall
