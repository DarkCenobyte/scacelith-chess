// The painted coffers of the hall ceiling (hall::detail::buildCeiling, src/scene/hall_ceiling.cpp).
// ceiling.glsl paints one motif per uv unit (fract(uv)) and varies the brush and the craquelure
// with floor(uv), so each coffer panel must span exactly one uv unit of its own; the central
// plafond keeps its world-metre uv.
#include "test.h"
#include "scene/hall_internal.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace {
bool approx(float a, float b) { return std::fabs(a - b) < 1e-4f; }
// The first vertex of a downward quad at height y (quadFlat appends its 4 corners together).
bool startsDownQuad(const MeshData& d, size_t i, float y) {
    if (i + 4 > d.vertices.size()) return false;
    for (size_t k = i; k < i + 4; ++k)
        if (!approx(d.vertices[k].pos.y, y) || d.vertices[k].normal.y > -0.999f) return false;
    return true;
}
}  // namespace

// Each of the 144 coffer panels: uv = (column, row) + position in the panel, u along +X and v
// along +Z, and no two coffers share a cell. (The panels used to get world (x, z) in metres, so
// the motif drifted off the gilded rosette and the border crossed the panels.)
TEST(hall_ceiling_coffer_uv_per_panel) {
    hall::detail::Accum acc;
    hall::detail::buildCeiling(acc);
    const MeshData& paint = acc[MaterialId::CeilingPainted];
    std::set<std::pair<int, int>> cells;
    int panels = 0, misaligned = 0;
    for (size_t i = 0; i < paint.vertices.size(); ++i) {
        if (!startsDownQuad(paint, i, hall::COFFER_TOP)) continue;
        float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f, u0 = 1e9f, v0 = 1e9f;
        for (size_t k = i; k < i + 4; ++k) {
            const Vertex& v = paint.vertices[k];
            x0 = std::min(x0, v.pos.x), x1 = std::max(x1, v.pos.x);
            z0 = std::min(z0, v.pos.z), z1 = std::max(z1, v.pos.z);
            u0 = std::min(u0, v.uv.x), v0 = std::min(v0, v.uv.y);
        }
        const int kx = int(std::lround(u0)), kz = int(std::lround(v0));
        bool ok = approx(u0, float(kx)) && approx(v0, float(kz));
        for (size_t k = i; k < i + 4; ++k) {
            const Vertex& v = paint.vertices[k];
            ok = ok && approx(v.uv.x, float(kx) + (v.pos.x - x0) / (x1 - x0)) &&
                 approx(v.uv.y, float(kz) + (v.pos.z - z0) / (z1 - z0));
        }
        misaligned += ok ? 0 : 1;
        cells.insert({kx, kz});
        ++panels;
        i += 3;
    }
    CHECK_EQ(panels, 144);
    CHECK_EQ(misaligned, 0);
    CHECK_EQ(cells.size(), size_t(144));
}

// The plafond panel above the table keeps uv = world (x, z) in metres.
TEST(hall_ceiling_plafond_uv_world) {
    hall::detail::Accum acc;
    hall::detail::buildCeiling(acc);
    const MeshData& paint = acc[MaterialId::CeilingPainted];
    int panels = 0;
    for (size_t i = 0; i < paint.vertices.size(); ++i) {
        if (!startsDownQuad(paint, i, hall::PLAFOND_TOP)) continue;
        for (size_t k = i; k < i + 4; ++k) {
            const Vertex& v = paint.vertices[k];
            CHECK(approx(v.uv.x, v.pos.x) && approx(v.uv.y, v.pos.z));
        }
        ++panels;
        i += 3;
    }
    CHECK_EQ(panels, 1);
}
