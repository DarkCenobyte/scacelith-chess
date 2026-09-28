#include "board.h"
#include "piece_profile.h"
#include "../game/layout.h"

using namespace m;

namespace {

constexpr float GAP = 0.00015f;      // half of the hairline joint between two slabs
constexpr float CHAMFER = 0.00035f;  // 45 degree chamfer on the top arrises
constexpr float JOINT_DEPTH = 0.0015f;  // depth of the joint (base slab top below the surface)

void quad(MeshData& d, vec3 a, vec3 b, vec3 c, vec3 e, vec3 n, vec2 ua, vec2 ub, vec2 uc, vec2 ue) {
    uint32_t base = uint32_t(d.vertices.size());
    vec3 p[4] = {a, b, c, e};
    vec2 u[4] = {ua, ub, uc, ue};
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.pos = p[i];
        v.normal = n;
        v.tangent = vec4(1, 0, 0, 1);
        v.uv = u[i];
        d.vertices.push_back(v);
    }
    // Wind counter-clockwise around n.
    vec3 g = cross(b - a, c - a);
    if (dot(g, n) >= 0) for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) d.indices.push_back(base + k);
    else for (uint32_t k : {0u, 2u, 1u, 0u, 3u, 2u}) d.indices.push_back(base + k);
}

// One inlaid slab: flat top, four chamfers, four sides down to the joint bottom.
void addSquare(MeshData& d, int file, int rank) {
    const float half = layout::BOARD_PLAY_SIZE * 0.5f;
    const float y0 = layout::BOARD_TOP_Y, y1 = y0 - CHAMFER, y2 = y0 - JOINT_DEPTH;
    float x0 = -half + float(file) * layout::SQUARE_SIZE + GAP, x1 = x0 + layout::SQUARE_SIZE - 2 * GAP;
    float z1 = half - float(rank) * layout::SQUARE_SIZE - GAP, z0 = z1 - layout::SQUARE_SIZE + 2 * GAP;  // z0 < z1
    const float eps = 1e-4f;
    float umin = float(file) / 8.0f + eps, umax = float(file + 1) / 8.0f - eps;
    float vmin = float(rank) / 8.0f + eps, vmax = float(rank + 1) / 8.0f - eps;
    auto uvOf = [&](vec3 p) {
        float u = (p.x + half) / layout::BOARD_PLAY_SIZE, v = (half - p.z) / layout::BOARD_PLAY_SIZE;
        return vec2(clamp(u, umin, umax), clamp(v, vmin, vmax));
    };
    auto Q = [&](vec3 a, vec3 b, vec3 c, vec3 e, vec3 n) { quad(d, a, b, c, e, n, uvOf(a), uvOf(b), uvOf(c), uvOf(e)); };
    const float c = CHAMFER;
    // Top (exactly at BOARD_TOP_Y).
    Q(vec3(x0 + c, y0, z1 - c), vec3(x1 - c, y0, z1 - c), vec3(x1 - c, y0, z0 + c), vec3(x0 + c, y0, z0 + c), vec3(0, 1, 0));
    const float s = 0.70710678f;
    // Chamfers.
    Q(vec3(x0, y1, z1), vec3(x1, y1, z1), vec3(x1 - c, y0, z1 - c), vec3(x0 + c, y0, z1 - c), vec3(0, s, s));
    Q(vec3(x1, y1, z0), vec3(x0, y1, z0), vec3(x0 + c, y0, z0 + c), vec3(x1 - c, y0, z0 + c), vec3(0, s, -s));
    Q(vec3(x1, y1, z1), vec3(x1, y1, z0), vec3(x1 - c, y0, z0 + c), vec3(x1 - c, y0, z1 - c), vec3(s, s, 0));
    Q(vec3(x0, y1, z0), vec3(x0, y1, z1), vec3(x0 + c, y0, z1 - c), vec3(x0 + c, y0, z0 + c), vec3(-s, s, 0));
    // Sides (only visible inside the hairline joints).
    Q(vec3(x0, y2, z1), vec3(x1, y2, z1), vec3(x1, y1, z1), vec3(x0, y1, z1), vec3(0, 0, 1));
    Q(vec3(x1, y2, z0), vec3(x0, y2, z0), vec3(x0, y1, z0), vec3(x1, y1, z0), vec3(0, 0, -1));
    Q(vec3(x1, y2, z1), vec3(x1, y2, z0), vec3(x1, y1, z0), vec3(x1, y1, z1), vec3(1, 0, 0));
    Q(vec3(x0, y2, z0), vec3(x0, y2, z1), vec3(x0, y1, z1), vec3(x0, y1, z0), vec3(-1, 0, 0));
}

MeshData buildFrame() {
    // Moulding profile in mm: (inward offset from the outer edge, height relative to the playing
    // surface). From the joint at the play area out over the flat border, a rounded bevel, a
    // small bead, the vertical side and the bottom arris.
    const float B = layout::BOARD_BORDER * 1000.0f, T = layout::BOARD_THICKNESS * 1000.0f;
    const float j = JOINT_DEPTH * 1000.0f, c = CHAMFER * 1000.0f, g = GAP * 1000.0f;
    Profile p;
    p.to(B - g, -j);
    p.to(B - g, -c, 0.0f);
    p.to(B - g - c, 0.0f, 0.0f);
    p.to(6.2f, 0.0f, 1.6f);
    p.to(2.0f, -4.0f, 1.2f);
    p.ellipse(vec2(2.0f, -5.1f), vec2(1.1f, 1.1f), 90, 225);
    p.to(0.0f, -6.6f, 0.8f);
    p.to(0.0f, -T + 1.0f, 0.4f);
    p.to(1.0f, -T, 0.3f);
    p.to(4.0f, -T);
    std::vector<vec2> prof = finishProfile(p, 0.3f, 2.0f);
    // The chamfer next to the squares must stay crisp and planar: finishProfile keeps corners
    // with radius 0 sharp. Convert to meters and sweep.
    for (auto& q : prof) q = q * 0.001f;
    const float half = layout::BOARD_SIZE * 0.5f;
    MeshData d = sweepRoundedRect(prof, half, half, 0.003f, 8);
    d.transform(translate(vec3(0, layout::BOARD_TOP_Y, 0)));
    // Base slab under the squares (seen at the bottom of the joints).
    const float hp = layout::BOARD_PLAY_SIZE * 0.5f + 0.001f;
    quad(d, vec3(-hp, layout::BOARD_TOP_Y - JOINT_DEPTH, hp), vec3(hp, layout::BOARD_TOP_Y - JOINT_DEPTH, hp),
         vec3(hp, layout::BOARD_TOP_Y - JOINT_DEPTH, -hp), vec3(-hp, layout::BOARD_TOP_Y - JOINT_DEPTH, -hp), vec3(0, 1, 0), vec2(0, 0),
         vec2(1, 0), vec2(1, 1), vec2(0, 1));
    return d;
}

}  // namespace

Model buildBoard() {
    Model m;
    ModelPart light, dark, frame;
    light.name = "squares_light";
    light.material = MaterialId::BoardSquareLight;
    dark.name = "squares_dark";
    dark.material = MaterialId::BoardSquareDark;
    for (int r = 0; r < 8; ++r)
        for (int f = 0; f < 8; ++f) addSquare(((f + r) & 1) ? light.mesh : dark.mesh, f, r);
    frame.name = "frame";
    frame.material = MaterialId::BoardFrame;
    frame.mesh = buildFrame();
    m.parts.push_back(std::move(light));
    m.parts.push_back(std::move(dark));
    m.parts.push_back(std::move(frame));
    return m;
}
