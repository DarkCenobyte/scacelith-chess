// Hall shell: solid walls (they cast the window shadows), the window wall with its three deep
// arched openings (splayed stone reveals, sills), the roof slab and the exterior terrace with its
// balustrade seen through the windows.
#include "hall_internal.h"

using namespace m;

namespace hall {
namespace detail {

namespace {

constexpr int ARCH_SEG = 28;
const float SPRING = WINDOW_SPRING_Y;
const float SILL = layout::WINDOW_SILL_Y;
const float WT = layout::WALL_THICKNESS;
const float WT2 = WALL_THICKNESS_OTHER;
constexpr float TERRACE_Y = -0.45f;
constexpr float TERRACE_EDGE_X = -15.5f;
constexpr float TERRACE_HALF_Z = 14.0f;

// Planar wall face in the wall plane at depth n, spanning [u0,u1]x[v0,v1], pierced by arched
// openings (centres ucs, half width hw, sill vSill, springing vSpring). Built from vertical strips
// that share vertices with the reveals (no T-junctions).
void faceWithArches(MeshData& d, const WallDef& w, float n, float u0, float u1, float v0, float v1,
                    std::vector<float> ucs, float hw, float vSill, float vSpring) {
    std::sort(ucs.begin(), ucs.end());
    vec3 N = w.N;
    auto q = [&](vec2 a, vec2 b, vec2 c, vec2 e) { quadFlat(d, w.P(a.x, a.y, n), w.P(b.x, b.y, n), w.P(c.x, c.y, n), w.P(e.x, e.y, n), N, w.U); };
    float cur = u0;
    for (float uc : ucs) {
        float a = uc - hw, b = uc + hw;
        // Solid pier left of the opening, split at the sill and springing levels.
        if (a > cur + 1e-4f) {
            float lv[4] = {v0, vSill, vSpring, v1};
            for (int k = 0; k < 3; ++k) q({cur, lv[k]}, {a, lv[k]}, {a, lv[k + 1]}, {cur, lv[k + 1]});
        }
        // Below the sill.
        if (vSill > v0 + 1e-4f) q({a, v0}, {b, v0}, {b, vSill}, {a, vSill});
        // Above the arch.
        std::vector<vec2> ol = archOutline(uc, hw, vSill, vSpring, ARCH_SEG);
        for (size_t k = 1; k + 2 < ol.size(); ++k) q(ol[k], ol[k + 1], {ol[k + 1].x, v1}, {ol[k].x, v1});
        cur = b;
    }
    if (u1 > cur + 1e-4f) {
        float lv[4] = {v0, vSill, vSpring, v1};
        for (int k = 0; k < 3; ++k) q({cur, lv[k]}, {u1, lv[k]}, {u1, lv[k + 1]}, {cur, lv[k + 1]});
    }
}

// Ruled reveal surface between two arched outlines (same parametrisation) at depths nA and nB.
void reveal(MeshData& d, const WallDef& w, float uc, float hwA, float nA, float hwB, float nB, float vSill) {
    std::vector<vec2> oa = archOutline(uc, hwA, vSill, SPRING, ARCH_SEG);
    std::vector<vec2> ob = archOutline(uc, hwB, vSill, SPRING, ARCH_SEG);
    int K = int(oa.size());
    std::vector<float> us(static_cast<size_t>(K));
    for (int k = 0; k < K; ++k) us[size_t(k)] = float(k);
    auto fn = [&](float s, float t) {
        int k = std::min(int(s + 0.5f), K - 1);
        vec3 pa = w.P(oa[size_t(k)].x, oa[size_t(k)].y, nA), pb = w.P(ob[size_t(k)].x, ob[size_t(k)].y, nB);
        return lerp3(pa, pb, t);
    };
    // Normals must face the opening's axis; decide the flip from the first jamb sample.
    MeshData tmp;
    surfaceGrid(tmp, us, {0.0f, 1.0f}, fn, false, UVMode::Meters);
    vec3 toAxis = w.U;  // the left jamb (k = 0) faces +U
    if (dot(tmp.vertices[0].normal, toAxis) < 0.0f) {
        tmp = MeshData();
        surfaceGrid(tmp, us, {0.0f, 1.0f}, fn, true, UVMode::Meters);
    }
    d.append(tmp);
}

// Balustrade along a straight line (world), top of the terrace at TERRACE_Y.
void balustrade(Accum& a, vec3 p0, vec3 p1, float spacing, bool postAtStart, bool postAtEnd) {
    MeshData& st = a[MaterialId::WallStone];
    vec3 dir = p1 - p0;
    float len = length(dir);
    dir = dir / len;
    vec3 side = normalize(cross(vec3(0, 1, 0), dir));
    const float y0 = TERRACE_Y;
    // Plinth and handrail.
    box(st, p0 + dir * (len * 0.5f) + vec3(0, y0 + 0.08f, 0), dir, vec3(0, 1, 0), side, vec3(len * 0.5f, 0.08f, 0.17f));
    // Handrail with a projecting cap.
    box(st, p0 + dir * (len * 0.5f) + vec3(0, y0 + 0.93f, 0), dir, vec3(0, 1, 0), side, vec3(len * 0.5f, 0.07f, 0.16f));
    box(st, p0 + dir * (len * 0.5f) + vec3(0, y0 + 1.02f, 0), dir, vec3(0, 1, 0), side, vec3(len * 0.5f + 0.02f, 0.02f, 0.18f));
    // Balusters (double-bellied), posts every ~2.6 m.
    Profile bal = {{0.0f, 0.0f},   {0.075f, 0.0f}, {0.075f, 0.0f}, {0.075f, 0.04f}, {0.06f, 0.05f},  {0.055f, 0.07f},
                   {0.075f, 0.14f}, {0.085f, 0.22f}, {0.07f, 0.29f}, {0.045f, 0.33f}, {0.035f, 0.36f}, {0.05f, 0.38f},
                   {0.035f, 0.40f}, {0.04f, 0.46f},  {0.05f, 0.52f},  {0.045f, 0.56f}, {0.06f, 0.58f},  {0.075f, 0.6f},
                   {0.075f, 0.6f},  {0.075f, 0.63f}, {0.075f, 0.63f}, {0.0f, 0.63f}};
    int nPosts = std::max(1, int(len / 2.6f + 0.5f));
    float seg = len / float(nPosts);
    for (int p = 0; p <= nPosts; ++p) {
        if ((p == 0 && !postAtStart) || (p == nPosts && !postAtEnd)) continue;
        vec3 c = p0 + dir * (seg * float(p));
        box(st, c + vec3(0, y0 + 0.5f, 0), dir, vec3(0, 1, 0), side, vec3(0.2f, 0.5f, 0.2f));
        box(st, c + vec3(0, y0 + 1.03f, 0), dir, vec3(0, 1, 0), side, vec3(0.25f, 0.03f, 0.25f));
        urn(a[MaterialId::WallStone], c + vec3(0, y0 + 1.06f, 0), 0.55f);
    }
    for (int p = 0; p < nPosts; ++p) {
        vec3 s0 = p0 + dir * (seg * float(p) + 0.2f);
        float span = seg - 0.4f;
        int n = std::max(1, int(span / spacing));
        for (int k = 0; k < n; ++k) {
            vec3 c = s0 + dir * (span * (float(k) + 0.5f) / float(n));
            lathe(st, bal, 10, translate(c + vec3(0, y0 + 0.16f, 0)) * scale(vec3(1.0f, 1.13f, 1.0f)));
        }
    }
}

// Moulded architrave framing an arched opening on the interior face (stone fasciae, gilded
// cymatium), with a carved keystone at the crown that rises to the entablature.
void windowArchitrave(Accum& a, const WallDef& w, float uc) {
    MeshData& stone = a[MaterialId::WallStone];
    MeshData& gilt = a[MaterialId::GildedTrim];
    std::vector<vec3> path;
    for (vec2 p : archOutline(uc, WIN_HALF_IN, SILL, SPRING, ARCH_SEG)) path.push_back(w.P(p.x, p.y, 0.0f));
    // (b = distance from the opening edge, a = projection); s points away from the opening.
    Profile arStone = {{0.16f, 0.06f},  {0.16f, 0.05f},  {0.16f, 0.05f},  {0.1f, 0.05f},   {0.1f, 0.05f},   {0.1f, 0.04f},
                       {0.1f, 0.04f},   {0.025f, 0.04f}, {0.02f, 0.044f}, {0.01f, 0.046f}, {0.003f, 0.04f}, {0.0f, 0.03f},
                       {0.0f, 0.03f},   {0.0f, 0.0f}};
    Profile arGilt = {{0.2f, 0.0f},    {0.2f, 0.03f},   {0.2f, 0.03f},   {0.195f, 0.042f}, {0.186f, 0.052f},
                      {0.174f, 0.058f}, {0.16f, 0.06f}};
    sweep(stone, arStone, path, w.N, false, 30.0f, true, true);
    sweep(gilt, arGilt, path, w.N, false, 30.0f, true, true);

    // Keystone: tapered block, leaning forward, a gilded rosette on its face.
    const float hb = 0.14f, ht = 0.2f, nb = 0.09f, nt = 0.12f;
    // Bottom face meets the intrados at its corners (the centre hangs a few mm into the arch).
    const float vB = SPRING + std::sqrt(WIN_HALF_IN * WIN_HALF_IN - hb * hb) - 0.001f, vT = ENTABLATURE_BOTTOM;
    vec3 fl0 = w.P(uc - hb, vB, nb), fr0 = w.P(uc + hb, vB, nb), fr1 = w.P(uc + ht, vT, nt), fl1 = w.P(uc - ht, vT, nt);
    vec3 bl0 = w.P(uc - hb, vB, 0), br0 = w.P(uc + hb, vB, 0), br1 = w.P(uc + ht, vT, 0), bl1 = w.P(uc - ht, vT, 0);
    auto face = [&](vec3 p0, vec3 p1, vec3 p2, vec3 p3, vec3 out, vec3 uAxis) {
        vec3 n = normalize(cross(p1 - p0, p3 - p0));
        if (dot(n, out) < 0.0f) n = -n;
        quadFlat(stone, p0, p1, p2, p3, n, uAxis);
        return n;
    };
    vec3 fn = face(fl0, fr0, fr1, fl1, w.N, w.U);
    face(bl0, fl0, fl1, bl1, -w.U, w.N);
    face(fr0, br0, br1, fr1, w.U, w.N);
    face(bl0, br0, fr0, fl0, vec3(0, -1, 0), w.U);
    face(fl1, fr1, br1, bl1, vec3(0, 1, 0), w.U);
    rosette(gilt, w.P(uc, 0.5f * (vB + vT), 0.5f * (nb + nt)), fn, 0.075f, 8, 0.025f);
}

}  // namespace

void buildShell(Accum& a) {
    MeshData& stone = a[MaterialId::WallStone];
    const float H = layout::HALL_HEIGHT, top = ROOF_TOP;
    const float X0 = layout::HALL_MIN_X, X1 = layout::HALL_MAX_X, Z0 = layout::HALL_MIN_Z, Z1 = layout::HALL_MAX_Z;
    const WallDef& ww = wall(WALL_WINDOWS);
    std::vector<float> ucs = {windowU(0), windowU(1), windowU(2)};

    // ---- Window wall: interior face, reveals, sills, exterior face.
    faceWithArches(stone, ww, 0.0f, 0.0f, ww.length, 0.0f, H, ucs, WIN_HALF_IN, SILL, SPRING);
    // Exterior face: frame seen from outside (u along +Z starting at the outer corner).
    WallDef ext;
    ext.origin = vec3(X0 - WT, 0, Z0 - WT2);
    ext.U = vec3(0, 0, 1);
    ext.N = vec3(-1, 0, 0);
    ext.length = (Z1 - Z0) + 2 * WT2;
    std::vector<float> ucsExt;
    for (float uc : ucs) ucsExt.push_back((Z1 - uc) - ext.origin.z);  // z of the window -> u
    faceWithArches(stone, ext, 0.0f, 0.0f, ext.length, TERRACE_Y, top, ucsExt, WIN_HALF, SILL - 0.04f, SPRING);
    for (float uc : ucs) {
        // Splayed inner reveal (interior face -> frame plane) and straight outer reveal.
        reveal(stone, ww, uc, WIN_HALF_IN, 0.0f, WIN_HALF, -WINDOW_FRAME_DEPTH, SILL);
        reveal(stone, ww, uc, WIN_HALF, -WINDOW_FRAME_DEPTH, WIN_HALF, -WT, SILL - 0.1f);
        windowArchitrave(a, ww, uc);
        // Interior sill board (flush with the dado cap, runs under the frame).
        vec3 n = vec3(0, 1, 0);
        quadFlat(stone, ww.P(uc - WIN_HALF_IN, SILL, 0.0f), ww.P(uc + WIN_HALF_IN, SILL, 0.0f), ww.P(uc + WIN_HALF, SILL, -WINDOW_FRAME_DEPTH - 0.04f),
                 ww.P(uc - WIN_HALF, SILL, -WINDOW_FRAME_DEPTH - 0.04f), n, ww.U);
        // Exterior sill: sloped slab projecting out of the facade, with ears into the jambs.
        {
            float e = WIN_HALF + 0.12f, n0 = -WINDOW_FRAME_DEPTH - 0.04f, n1 = -WT - 0.07f;
            vec3 t0 = ww.P(uc - e, SILL, n0), t1 = ww.P(uc + e, SILL, n0);
            vec3 t2 = ww.P(uc + e, SILL - 0.035f, n1), t3 = ww.P(uc - e, SILL - 0.035f, n1);
            vec3 b2 = ww.P(uc + e, SILL - 0.11f, n1), b3 = ww.P(uc - e, SILL - 0.11f, n1);
            vec3 b0 = ww.P(uc - e, SILL - 0.11f, -WT + 0.02f), b1 = ww.P(uc + e, SILL - 0.11f, -WT + 0.02f);
            vec3 slope = normalize(cross(t1 - t0, t3 - t0));
            if (slope.y < 0) slope = -slope;
            quadFlat(stone, t0, t1, t2, t3, slope, ww.U);
            quadFlat(stone, t3, t2, b2, b3, -ww.N, ww.U);                // front
            quadFlat(stone, b3, b2, b1, b0, vec3(0, -1, 0), ww.U);         // underside of the nose
            quadFlat(stone, t0, t3, b3, b0, -ww.U, ww.N);                  // ears (ends)
            quadFlat(stone, t1, t2, b2, b1, ww.U, ww.N);
        }
    }
    // Window wall top (under the roof) and ends are hidden; the roof slab closes the box.

    // ---- Other walls: solid boxes (interior faces are the room faces).
    boxAA(stone, vec3(X1, TERRACE_Y, Z0 - WT2), vec3(X1 + WT2, top, Z1 + WT2));
    boxAA(stone, vec3(X0 - WT, TERRACE_Y, Z1), vec3(X1 + WT2, top, Z1 + WT2));
    // -Z wall with the door opening (x in +-DOOR_WIDTH/2, y < DOOR_HEIGHT).
    float dh = DOOR_WIDTH * 0.5f;
    boxAA(stone, vec3(X0 - WT, TERRACE_Y, Z0 - WT2), vec3(-dh, top, Z0));
    boxAA(stone, vec3(dh, TERRACE_Y, Z0 - WT2), vec3(X1 + WT2, top, Z0));
    boxAA(stone, vec3(-dh, DOOR_HEIGHT, Z0 - WT2), vec3(dh, top, Z0));
    // Roof slab (above the coffers).
    boxAA(stone, vec3(X0 - WT, PLAFOND_TOP + 0.04f, Z0 - WT2), vec3(X1 + WT2, top, Z1 + WT2));

    // ---- Exterior terrace (a few steps below the hall floor) and its balustrade.
    boxAA(stone, vec3(TERRACE_EDGE_X, -6.0f, -TERRACE_HALF_Z), vec3(X0 - WT, TERRACE_Y, TERRACE_HALF_Z), F_PY | F_NX | F_PZ | F_NZ);
    // Facade plinth along the terrace.
    boxAA(stone, vec3(X0 - WT - 0.12f, TERRACE_Y, Z0 - WT2 - 0.12f), vec3(X0 - WT, TERRACE_Y + 0.6f, Z1 + WT2 + 0.12f), F_NX | F_PY | F_PZ | F_NZ);
    // Front run owns the corner posts; the returns end in a post against the facade plinth.
    balustrade(a, vec3(TERRACE_EDGE_X + 0.2f, 0, -TERRACE_HALF_Z + 0.2f), vec3(TERRACE_EDGE_X + 0.2f, 0, TERRACE_HALF_Z - 0.2f), 0.3f, true, true);
    balustrade(a, vec3(TERRACE_EDGE_X + 0.2f, 0, TERRACE_HALF_Z - 0.2f), vec3(X0 - WT - 0.4f, 0, TERRACE_HALF_Z - 0.2f), 0.3f, false, true);
    balustrade(a, vec3(TERRACE_EDGE_X + 0.2f, 0, -TERRACE_HALF_Z + 0.2f), vec3(X0 - WT - 0.4f, 0, -TERRACE_HALF_Z + 0.2f), 0.3f, false, true);
}

}  // namespace detail
}  // namespace hall
