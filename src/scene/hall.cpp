// Royal hall generator: assembles the architecture pieces (hall_*.cpp) into per-material parts.
#include "hall.h"
#include "hall_internal.h"
#include "../core/log.h"
#include "../render/renderer.h"
#include <cctype>
#include <chrono>

using namespace m;

namespace hall {
namespace detail {

const WallDef& wall(int id) {
    static const std::vector<WallDef> ws = [] {
        // Pilaster axes: long walls z = +-10.3, +-7.8, +-2.6 (framing the windows / tapestries),
        // end walls x = +-6.3, +-2.3 (framing the doors and the tapestries).
        std::vector<float> longP = {0.7f, 3.2f, 8.4f, 13.6f, 18.8f, 21.3f};
        std::vector<float> endP = {0.7f, 4.7f, 9.3f, 13.3f};
        const float X0 = layout::HALL_MIN_X, X1 = layout::HALL_MAX_X, Z0 = layout::HALL_MIN_Z, Z1 = layout::HALL_MAX_Z;
        std::vector<WallDef> w(4);
        w[WALL_WINDOWS] = {vec3(X0, 0, Z1), vec3(0, 0, -1), vec3(1, 0, 0), Z1 - Z0, longP};
        w[WALL_EAST] = {vec3(X1, 0, Z0), vec3(0, 0, 1), vec3(-1, 0, 0), Z1 - Z0, longP};
        w[WALL_NORTH] = {vec3(X0, 0, Z0), vec3(1, 0, 0), vec3(0, 0, 1), X1 - X0, endP};
        w[WALL_SOUTH] = {vec3(X1, 0, Z1), vec3(-1, 0, 0), vec3(0, 0, -1), X1 - X0, endP};
        return w;
    }();
    return ws[size_t(id)];
}

float windowU(int i) { return layout::HALL_MAX_Z - (float(1 - i) * layout::WINDOW_SPACING); }

vec3 wp(const WallDef& w, vec2 uv, float n) { return w.P(uv.x, uv.y, n); }

void wallBar(MeshData& d, const WallDef& w, vec2 a, vec2 b, float n, float width, float depth, unsigned faces) {
    vec3 pa = w.P(a.x, a.y, n), pb = w.P(b.x, b.y, n);
    vec3 ax = normalize(pb - pa);
    vec3 az = w.N;
    vec3 ay = normalize(cross(az, ax));
    box(d, (pa + pb) * 0.5f, ax, ay, az, vec3(length(pb - pa) * 0.5f, width * 0.5f, depth * 0.5f), faces);
}

}  // namespace detail

Model buildHall(BuildStats* stats) {
    using namespace detail;
    auto t0 = std::chrono::steady_clock::now();
    Accum acc;
    MeshData glass, paintings;
    std::vector<ModelPart> tapestries;
    buildFloor(acc);
    buildShell(acc);
    buildOrders(acc);
    buildCeiling(acc);
    buildWindows(acc, glass);
    buildDoor(acc, paintings);
    buildChandelier(acc);
    buildTapestries(acc, tapestries);
    buildProps(acc, paintings);

    Model model;
    const uint32_t staticFlags = render::DRAW_STATIC | render::DRAW_CAST_SHADOW;
    for (int i = 0; i < int(MaterialId::Count); ++i) {
        MeshData& md = acc.mesh[i];
        if (md.indices.empty()) continue;
        ModelPart p;
        std::string nm = materials::name(MaterialId(i));
        for (auto& c : nm) c = char(std::tolower(static_cast<unsigned char>(c)));
        p.name = "hall_" + nm;
        p.material = MaterialId(i);
        p.flags = MaterialId(i) == MaterialId::Crystal ? uint32_t(render::DRAW_STATIC) : staticFlags;
        p.mesh = std::move(md);
        model.parts.push_back(std::move(p));
    }
    if (!paintings.indices.empty()) {
        ModelPart p;
        p.name = "hall_paintings";
        p.material = MaterialId::CeilingPainted;
        p.flags = staticFlags;
        p.inst[0] = vec4(1, 0, 0, 0);
        p.mesh = std::move(paintings);
        model.parts.push_back(std::move(p));
    }
    for (auto& t : tapestries) model.parts.push_back(std::move(t));
    if (!glass.indices.empty()) {
        ModelPart p;
        p.name = "hall_windowglass";
        p.material = MaterialId::WindowGlass;
        p.flags = render::DRAW_STATIC;  // transparent, never casts shadows
        p.mesh = std::move(glass);
        model.parts.push_back(std::move(p));
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    BuildStats st;
    for (auto& p : model.parts) {
        st.triangles += p.mesh.indices.size() / 3;
        st.vertices += p.mesh.vertices.size();
    }
    st.parts = model.parts.size();
    st.seconds = secs;
    LOGI("hall: %u parts, %u triangles, %u vertices, generated in %.3f s", unsigned(st.parts), unsigned(st.triangles), unsigned(st.vertices),
         secs);
    if (stats) *stats = st;
    return model;
}

}  // namespace hall
