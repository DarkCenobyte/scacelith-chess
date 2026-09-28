// Scoresheet pad and ballpoint pen geometry. See scoresheet_model.h for the spaces.
#include "scoresheet_model.h"
#include "../render/renderer.h"
#include <cmath>

using namespace m;
using namespace game::sheet;

namespace {

constexpr float MM = 0.001f;

void quad(MeshData& d, vec3 a, vec3 b, vec3 c, vec3 e, vec3 n, vec2 ua, vec2 ub, vec2 uc, vec2 ue) {
    // a-b-c-e counter-clockwise seen from the side 'n' points to.
    uint32_t base = uint32_t(d.vertices.size());
    vec3 t = normalize(b - a);
    for (auto [p, uv] : {std::pair<vec3, vec2>{a, ua}, {b, ub}, {c, uc}, {e, ue}}) {
        Vertex v;
        v.pos = p;
        v.normal = n;
        v.tangent = vec4(t, 1.0f);
        v.uv = uv;
        d.vertices.push_back(v);
    }
    for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) d.indices.push_back(base + k);
}

ModelPart part(const char* name, MaterialId mat, MeshData mesh, uint32_t flags = render::DRAW_CAST_SHADOW) {
    ModelPart p;
    p.name = name;
    p.material = mat;
    p.mesh = std::move(mesh);
    p.flags = flags;
    return p;
}

// The cloth tape over the bound top edge: a thin C-shaped section (lip over the pages, rounded
// corner, spine down to the table) extruded across the pad, a little wider than the pages.
MeshData bindingTape() {
    MeshData d;
    const float x0 = -(0.5f * PAGE_W + 0.6f) * MM, x1 = -x0;
    const float zl = (HINGE_Y - 0.3f - 0.5f * PAGE_H) * MM;   // lip front edge, just before the hinge line
    const float yT = (PAD_TOP + BINDING_T) * MM, yI = PAD_TOP * MM;
    const float zs = (-0.5f * PAGE_H - BINDING_T) * MM;       // outer face of the spine
    const float r = 1.0f * MM, ri = r - BINDING_T * MM;
    const vec2 C(zs + r, yT - r);                             // (z, y) centre of the corner
    struct PathPt { vec2 o, i, n; };
    std::vector<PathPt> path;
    path.push_back({vec2(zl, yT), vec2(zl, yI), vec2(0, 1)});
    const int arcSeg = 8;
    for (int k = 0; k <= arcSeg; ++k) {
        float a = 0.5f * PI + 0.5f * PI * float(k) / float(arcSeg);
        vec2 n(std::cos(a), std::sin(a));
        path.push_back({C + n * r, C + n * ri, n});
    }
    path.push_back({vec2(zs, 0.0f), vec2(zs + BINDING_T * MM, 0.0f), vec2(-1, 0)});
    // Outer surface.
    for (size_t k = 0; k + 1 < path.size(); ++k) {
        const PathPt &p = path[k], &q = path[k + 1];
        uint32_t base = uint32_t(d.vertices.size());
        for (const PathPt* pp : {&p, &q})
            for (float x : {x0, x1}) {
                Vertex v;
                v.pos = vec3(x, pp->o.y, pp->o.x);
                v.normal = vec3(0.0f, pp->n.y, pp->n.x);
                v.tangent = vec4(1, 0, 0, 1);
                v.uv = vec2(x, pp->o.y);
                d.vertices.push_back(v);
            }
        // (p,x0) (p,x1) (q,x0) (q,x1): CCW from outside is (a, b, c), (b, d, c).
        for (uint32_t i : {0u, 1u, 2u, 1u, 3u, 2u}) d.indices.push_back(base + i);
    }
    // Lip front edge (faces the owner, +Z).
    quad(d, vec3(x0, yI, zl), vec3(x1, yI, zl), vec3(x1, yT, zl), vec3(x0, yT, zl), vec3(0, 0, 1), vec2(0), vec2(0), vec2(0),
         vec2(0));
    // End caps: the C-shaped section at both sides.
    for (int side = 0; side < 2; ++side) {
        float x = side ? x1 : x0;
        vec3 n(side ? 1.0f : -1.0f, 0.0f, 0.0f);
        for (size_t k = 0; k + 1 < path.size(); ++k) {
            vec3 a(x, path[k].o.y, path[k].o.x), b(x, path[k + 1].o.y, path[k + 1].o.x);
            vec3 c(x, path[k + 1].i.y, path[k + 1].i.x), e(x, path[k].i.y, path[k].i.x);
            // Orientation: choose the winding whose normal matches n.
            if (dot(cross(b - a, c - a), n) >= 0.0f) quad(d, a, b, c, e, n, vec2(0), vec2(0), vec2(0), vec2(0));
            else quad(d, a, e, c, b, n, vec2(0), vec2(0), vec2(0), vec2(0));
        }
    }
    return d;
}

}  // namespace

void pageGridSamples(std::vector<float>& xs, std::vector<float>& ys) {
    xs.clear();
    ys.clear();
    const int nx = 13;
    for (int i = 0; i < nx; ++i) xs.push_back(PAGE_W * float(i) / float(nx - 1));
    // A little of the glued strip (it stays flat under the tape), fine rows where the page wraps
    // around the binding, then coarser rows.
    ys.push_back(HINGE_Y - 1.0f);
    const float fineEnd = HINGE_Y + 16.0f;
    const int fine = 46, coarse = 40;
    for (int j = 0; j <= fine; ++j) ys.push_back(HINGE_Y + (fineEnd - HINGE_Y) * float(j) / float(fine));
    for (int j = 1; j <= coarse; ++j) ys.push_back(fineEnd + (PAGE_H - fineEnd) * float(j) / float(coarse));
}

void buildPageMesh(const std::vector<float>& xs, const std::vector<float>& ys, const std::vector<vec3>& grid,
                   MeshData& d) {
    const size_t nx = xs.size(), ny = ys.size();
    d.vertices.resize(2 * nx * ny);
    d.indices.clear();
    d.indices.reserve((nx - 1) * (ny - 1) * 12);
    const float half = 0.04f * MM;  // half the sheet thickness (80 g/m2 paper ~ 0.1 mm)
    auto at = [&](size_t i, size_t j) { return grid[j * nx + i]; };
    for (size_t j = 0; j < ny; ++j)
        for (size_t i = 0; i < nx; ++i) {
            vec3 du = at(std::min(i + 1, nx - 1), j) - at(i > 0 ? i - 1 : 0, j);
            vec3 dv = at(i, std::min(j + 1, ny - 1)) - at(i, j > 0 ? j - 1 : 0);
            vec3 n = normalize(cross(dv, du));
            vec3 t = normalize(du);
            vec3 p = at(i, j);
            vec2 uv(xs[i] / PAGE_W, ys[j] / PAGE_H);
            Vertex& f = d.vertices[j * nx + i];
            f.pos = p + n * half;
            f.normal = n;
            f.tangent = vec4(t, 1.0f);
            f.uv = uv;
            Vertex& b = d.vertices[nx * ny + j * nx + i];
            b.pos = p - n * half;
            b.normal = -n;
            b.tangent = vec4(t, -1.0f);
            b.uv = uv + vec2(2.0f, 0.0f);
        }
    const uint32_t backOff = uint32_t(nx * ny);
    for (size_t j = 0; j + 1 < ny; ++j)
        for (size_t i = 0; i + 1 < nx; ++i) {
            uint32_t a = uint32_t(j * nx + i), b = a + 1, c = a + uint32_t(nx), e = c + 1;
            for (uint32_t k : {a, c, b, b, c, e}) d.indices.push_back(k);
            for (uint32_t k : {a, b, c, b, e, c}) d.indices.push_back(backOff + k);
        }
}

Model buildScoresheetPad() {
    Model m;
    const float hw = 0.5f * PAGE_W * MM, hh = 0.5f * PAGE_H * MM;
    // Back board.
    {
        MeshData b = prim::box(vec3(hw, 0.5f * BOARD_T * MM, hh));
        b.transform(translate(vec3(0.0f, 0.5f * BOARD_T * MM, 0.0f)));
        ModelPart p = part("board", MaterialId::ScoresheetCard, std::move(b));
        m.parts.push_back(std::move(p));
    }
    // Page stack: top face = the page under the top page, sides = sheet edges.
    const float y0 = BOARD_T * MM, yTop = (PAD_TOP - SHEET_T) * MM, yEdge = PAD_TOP * MM;
    {
        MeshData t;
        quad(t, vec3(-hw, yTop, hh), vec3(hw, yTop, hh), vec3(hw, yTop, -hh), vec3(-hw, yTop, -hh), vec3(0, 1, 0),
             vec2(0, 1), vec2(1, 1), vec2(1, 0), vec2(0, 0));
        m.parts.push_back(part("stack_top", MaterialId::ScoresheetPaper, std::move(t)));
    }
    {
        MeshData e;
        auto uv = [](float alongM, float y) { return vec2(4.0f + alongM, y * 1000.0f); };
        const float W = 2.0f * hw, H = 2.0f * hh;
        // Left (-X), right (+X), bottom (+Z, towards the owner), top (-Z, under the tape).
        quad(e, vec3(-hw, y0, -hh), vec3(-hw, y0, hh), vec3(-hw, yEdge, hh), vec3(-hw, yEdge, -hh), vec3(-1, 0, 0),
             uv(0, y0), uv(H, y0), uv(H, yEdge), uv(0, yEdge));
        quad(e, vec3(hw, y0, hh), vec3(hw, y0, -hh), vec3(hw, yEdge, -hh), vec3(hw, yEdge, hh), vec3(1, 0, 0), uv(0, y0),
             uv(H, y0), uv(H, yEdge), uv(0, yEdge));
        quad(e, vec3(-hw, y0, hh), vec3(hw, y0, hh), vec3(hw, yEdge, hh), vec3(-hw, yEdge, hh), vec3(0, 0, 1), uv(0, y0),
             uv(W, y0), uv(W, yEdge), uv(0, yEdge));
        quad(e, vec3(hw, y0, -hh), vec3(-hw, y0, -hh), vec3(-hw, yEdge, -hh), vec3(hw, yEdge, -hh), vec3(0, 0, -1),
             uv(0, y0), uv(W, y0), uv(W, yEdge), uv(0, yEdge));
        m.parts.push_back(part("stack_edges", MaterialId::ScoresheetPaper, std::move(e)));
    }
    {
        ModelPart p = part("tape", MaterialId::ScoresheetCard, bindingTape());
        p.inst[0] = vec4(1, 0, 0, 0);
        m.parts.push_back(std::move(p));
    }
    return m;
}

// ---------------------------------------------------------------------------------------------
// Ballpoint pen (dimensions in mm along the pen axis, from the tip).
Model buildBallpointPen() {
    Model m;
    auto lathe = [](std::vector<vec2> prof, int seg) {
        for (vec2& p : prof) p *= MM;
        return prim::lathe(prof, seg);
    };
    MeshData metal, body;
    // Ball in its socket, the refill's brass-coloured cone (plated here), the chrome grip cone.
    {
        MeshData ball = prim::sphere(0.4f * MM, 16, 8);
        ball.transform(translate(vec3(0.0f, 0.4f * MM, 0.0f)));
        metal.append(ball);
        metal.append(lathe({{0.36f, 0.5f}, {0.42f, 0.58f}, {0.7f, 1.6f}, {1.05f, 3.2f}, {0.0f, 3.25f}}, 24));
        metal.append(lathe({{0.0f, 2.9f},
                            {1.2f, 2.95f},
                            {1.3f, 3.1f},
                            {1.3f, 3.1f},
                            {1.62f, 4.2f},
                            {2.25f, 7.0f},
                            {2.95f, 11.0f},
                            {3.45f, 14.5f},
                            {3.7f, 16.6f},
                            {3.74f, 17.4f},
                            {3.74f, 17.4f},
                            {4.02f, 17.45f},
                            {4.08f, 17.7f},
                            {4.08f, 18.3f},
                            {4.02f, 18.55f},
                            {0.0f, 18.6f}},
                           64));
    }
    // Barrel (slightly swelling towards the middle) and the posted cap over its back end.
    body.append(lathe({{3.95f, 18.5f}, {4.06f, 19.0f}, {4.12f, 40.0f}, {4.15f, 62.0f}, {4.12f, 88.0f}, {4.08f, 93.0f},
                       {0.0f, 93.1f}},
                      64));
    metal.append(lathe({{4.08f, 91.7f}, {4.36f, 91.8f}, {4.5f, 92.1f}, {4.52f, 92.6f}, {4.52f, 94.2f}, {4.46f, 94.6f},
                        {4.3f, 94.7f}},
                       64));
    body.append(lathe({{4.3f, 94.6f}, {4.48f, 94.8f}, {4.5f, 95.4f}, {4.46f, 115.0f}, {4.38f, 133.6f}, {4.2f, 134.1f},
                       {4.0f, 134.2f}},
                      64));
    metal.append(lathe({{3.95f, 134.0f},
                        {4.36f, 134.1f},
                        {4.38f, 134.8f},
                        {4.3f, 136.0f},
                        {4.0f, 137.6f},
                        {3.4f, 139.4f},
                        {2.4f, 140.9f},
                        {1.2f, 141.7f},
                        {0.0f, 142.0f}},
                       64));
    // Clip on +Z: a head clamped under the finial, a spring bar standing off the cap, a ball foot.
    {
        MeshData head = prim::roundedBox(vec3(1.7f, 2.6f, 0.9f) * MM, 0.5f * MM, 3);
        head.transform(translate(vec3(0.0f, 131.2f, 4.9f) * MM));
        metal.append(head);
        const float yTop = 130.0f, yBot = 106.0f;
        MeshData bar = prim::roundedBox(vec3(1.35f, 0.5f * (yTop - yBot), 0.42f) * MM, 0.35f * MM, 3);
        // Standing off 1.2 mm at the head, leaning in towards the foot.
        float lean = std::atan2(0.5f, yTop - yBot);
        bar.transform(translate(vec3(0.0f, 0.5f * (yTop + yBot), 5.55f) * MM) * rotateX(lean));
        metal.append(bar);
        MeshData foot = prim::sphere(0.95f * MM, 16, 10);
        foot.transform(translate(vec3(0.0f, 105.8f, 5.05f) * MM));
        metal.append(foot);
    }
    m.parts.push_back(part("pen_metal", MaterialId::PenMetal, std::move(metal)));
    m.parts.push_back(part("pen_body", MaterialId::PenBody, std::move(body)));
    return m;
}
