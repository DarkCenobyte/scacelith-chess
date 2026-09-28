// Classical order of the walls: wood wainscot with raised panels and pedestals, gilded attic
// bases, fluted limestone pilasters, gilded Corinthian capitals, entablature with dentil cornice.
#include "hall_internal.h"

using namespace m;

namespace hall {
namespace detail {

namespace {

constexpr float PED_HALF = 0.385f;    // pedestal half width
constexpr float PED_PROJ = 0.14f;     // pedestal projection beyond the wainscot
constexpr float FRAME_N = 0.025f;     // wainscot frame plane
constexpr float DOOR_CLEAR = DOOR_WIDTH * 0.5f + 0.26f;  // wainscot stops at the door architrave
const float HW = PILASTER_WIDTH * 0.5f, PP = PILASTER_PROJECTION;

// Wainscot moulding profile (b = distance from the wall, a = height).
Profile wainscotProfile() {
    const float T = DADO_TOP;
    return {{0.045f, 0.0f},   {0.045f, 0.115f}, {0.045f, 0.115f}, {0.04f, 0.125f},  {0.036f, 0.14f},  {0.033f, 0.155f},
            {0.028f, 0.165f}, {0.025f, 0.172f}, {0.025f, 0.172f}, {FRAME_N, T - 0.13f}, {FRAME_N, T - 0.13f},
            {0.031f, T - 0.125f}, {0.034f, T - 0.115f}, {0.031f, T - 0.105f}, {0.031f, T - 0.105f}, {0.038f, T - 0.095f},
            {0.052f, T - 0.078f}, {0.06f, T - 0.06f}, {0.066f, T - 0.045f}, {0.066f, T - 0.02f}, {0.062f, T - 0.008f},
            {0.055f, T}, {0.055f, T}, {0.0f, T}};
}

// Attic base profile relative to the shaft surface (a = absolute height): plinth, lower torus,
// scotia, upper torus, fillet and apophyge.
Profile atticBase() {
    Profile p;
    const float y0 = DADO_TOP;
    p.push_back({0.062f, y0});
    p.push_back({0.062f, y0 + 0.06f});
    p.push_back({0.062f, y0 + 0.06f});
    p.push_back({0.03f, y0 + 0.06f});
    for (int i = 0; i <= 8; ++i) {  // lower torus
        float th = -0.5f * PI + PI * float(i) / 8.0f;
        p.push_back({0.03f + 0.03f * std::cos(th), y0 + 0.09f + 0.03f * std::sin(th)});
    }
    p.push_back({0.024f, y0 + 0.125f});
    p.push_back({0.024f, y0 + 0.135f});
    for (int i = 1; i <= 6; ++i) {  // scotia (concave)
        float f = float(i) / 6.0f;
        p.push_back({lerp(0.024f, 0.018f, f) - 0.012f * std::sin(PI * f), y0 + 0.135f + 0.055f * f});
    }
    p.push_back({0.02f, y0 + 0.195f});
    for (int i = 0; i <= 7; ++i) {  // upper torus
        float th = (-75.0f + 165.0f * float(i) / 7.0f) * DEG;
        p.push_back({0.004f + 0.02f * std::cos(th), y0 + 0.217f + 0.02f * std::sin(th)});
    }
    p.push_back({0.006f, y0 + 0.24f});
    p.push_back({0.006f, y0 + 0.25f});
    p.push_back({0.003f, y0 + 0.27f});
    p.push_back({0.0f, SHAFT_BOTTOM});
    return p;
}

// Three-sided outline path around a pilaster/pedestal (s points outwards).
std::vector<vec3> outline3(const WallDef& w, float u, float hw, float proj, float y = 0.0f) {
    return {w.P(u + hw, y, 0.0f), w.P(u + hw, y, proj), w.P(u - hw, y, proj), w.P(u - hw, y, 0.0f)};
}

void fluteShaft(MeshData& d, const WallDef& w, float u, float y0, float y1) {
    const int NF = 7;
    const float r = 0.03f, pitch = 0.09f, depth = 0.018f;
    const float vb = y0 + 0.14f, vt = y1 - 0.14f;
    // u samples: across each flute (9) plus the fillets / arrises.
    std::vector<float> us = {-HW};
    float first = -pitch * (NF - 1) * 0.5f;
    for (int k = 0; k < NF; ++k) {
        float c = first + pitch * float(k);
        for (int i = 0; i <= 8; ++i) us.push_back(c - r + 2.0f * r * float(i) / 8.0f);
    }
    us.push_back(HW);
    std::vector<float> vs = {y0, vb};
    for (int i = 1; i <= 5; ++i) vs.push_back(vb + r * (1.0f - std::cos(0.5f * PI * float(i) / 5.0f)));
    for (int i = 0; i <= 5; ++i) vs.push_back(vt - r + r * std::sin(0.5f * PI * float(i) / 5.0f));
    vs.push_back(y1);
    auto fn = [&](float x, float y) {
        float dep = 0.0f;
        for (int k = 0; k < NF; ++k) {
            float c = first + pitch * float(k);
            float dx = x - c;
            if (std::fabs(dx) >= r) continue;
            float dy = std::max(0.0f, std::max(y - (vt - r), (vb + r) - y));
            if (y < vb || y > vt) dy = r;
            float q = r * r - dx * dx - dy * dy;
            if (q > 0) dep = std::sqrt(q) / r * depth;
        }
        return w.P(u + x, y, PP - dep);
    };
    surfaceGrid(d, us, vs, fn, false, UVMode::Meters);
    // Returns (sides) of the shaft.
    quadFlat(d, w.P(u + HW, y0, 0), w.P(u + HW, y0, PP), w.P(u + HW, y1, PP), w.P(u + HW, y1, 0), w.U, w.N);
    quadFlat(d, w.P(u - HW, y0, 0), w.P(u - HW, y0, PP), w.P(u - HW, y1, PP), w.P(u - HW, y1, 0), -w.U, w.N);
}

}  // namespace

void buildOrders(Accum& a) {
    MeshData& wood = a[MaterialId::WallPanelWood];
    MeshData& gilt = a[MaterialId::GildedTrim];
    MeshData& stone = a[MaterialId::WallStone];

    // ---- Wainscot: one sweep around the room (pedestal ressauts under every pilaster), open at the door.
    std::vector<vec3> path;
    auto push = [&](vec3 p) {
        if (path.empty() || length2(path.back() - p) > 1e-8f) path.push_back(p);
    };
    auto run = [&](int wid, float uStart, float uEnd) {  // travels -U so that s = N (into the room)
        const WallDef& w = wall(wid);
        push(w.P(uStart, 0, 0));
        std::vector<float> ps = w.pilasters;
        std::sort(ps.begin(), ps.end(), [](float x, float y) { return x > y; });
        for (float up : ps) {
            if (up + PED_HALF >= uStart || up - PED_HALF <= uEnd) continue;
            push(w.P(up + PED_HALF, 0, 0));
            push(w.P(up + PED_HALF, 0, PED_PROJ));
            push(w.P(up - PED_HALF, 0, PED_PROJ));
            push(w.P(up - PED_HALF, 0, 0));
        }
        push(w.P(uEnd, 0, 0));
    };
    run(WALL_NORTH, DOOR_U - DOOR_CLEAR, 0.0f);
    run(WALL_WINDOWS, wall(WALL_WINDOWS).length, 0.0f);
    run(WALL_SOUTH, wall(WALL_SOUTH).length, 0.0f);
    run(WALL_EAST, wall(WALL_EAST).length, 0.0f);
    run(WALL_NORTH, wall(WALL_NORTH).length, DOOR_U + DOOR_CLEAR);
    sweep(wood, wainscotProfile(), path, vec3(0, 1, 0), false, 30.0f, true, true);

    // ---- Raised panels between the pedestals, and on the pedestal fronts.
    const float pv0 = 0.25f, pv1 = DADO_TOP - 0.2f;
    for (int wid = 0; wid < 4; ++wid) {
        const WallDef& w = wall(wid);
        std::vector<std::pair<float, float>> obst;
        for (float up : w.pilasters) obst.push_back({up - PED_HALF, up + PED_HALF});
        if (wid == WALL_NORTH) obst.push_back({DOOR_U - DOOR_CLEAR, DOOR_U + DOOR_CLEAR});
        obst.push_back({-1.0f, 0.0f});
        obst.push_back({w.length, w.length + 1.0f});
        std::sort(obst.begin(), obst.end());
        for (size_t k = 0; k + 1 < obst.size(); ++k) {
            float ua = obst[k].second + 0.11f, ub = obst[k + 1].first - 0.11f;
            if (ub - ua < 0.3f) continue;
            int n = std::max(1, int((ub - ua) / 1.25f + 0.5f));
            float gap = 0.12f, pw = (ub - ua - gap * float(n - 1)) / float(n);
            for (int i = 0; i < n; ++i) {
                float u0 = ua + (pw + gap) * float(i);
                raisedPanel(wood, gilt, w, u0, u0 + pw, pv0, pv1, FRAME_N, 0.045f, 0.012f);
            }
        }
        for (float up : w.pilasters)
            raisedPanel(wood, gilt, w, up - PED_HALF + 0.1f, up + PED_HALF - 0.1f, pv0, pv1, FRAME_N + PED_PROJ, 0.04f, 0.012f);
    }

    // ---- Pilasters: gilded attic base, fluted shaft, astragal, Corinthian capital.
    Profile base = atticBase();
    Profile astragal;
    for (int i = 0; i <= 6; ++i) {
        float th = -0.5f * PI + PI * float(i) / 6.0f;
        astragal.push_back({0.018f * std::cos(th), CAPITAL_BOTTOM - 0.035f + 0.018f * std::sin(th)});
    }
    astragal.push_back({0.0f, CAPITAL_BOTTOM});
    for (int wid = 0; wid < 4; ++wid) {
        const WallDef& w = wall(wid);
        for (float up : w.pilasters) {
            sweep(gilt, base, outline3(w, up, HW, PP), vec3(0, 1, 0), false, 30.0f);
            fluteShaft(stone, w, up, SHAFT_BOTTOM, CAPITAL_BOTTOM - 0.035f);
            sweep(gilt, astragal, outline3(w, up, HW, PP), vec3(0, 1, 0), false, 30.0f);
            // Filler between the astragal and the capital's bell.
            quadFlat(stone, w.P(up - HW, CAPITAL_BOTTOM - 0.035f, PP), w.P(up + HW, CAPITAL_BOTTOM - 0.035f, PP),
                     w.P(up + HW, CAPITAL_BOTTOM, PP), w.P(up - HW, CAPITAL_BOTTOM, PP), w.N, w.U);
            corinthianCapital(gilt, w, up, CAPITAL_BOTTOM, ENTABLATURE_BOTTOM, HW, PP);
        }
    }

    // ---- Entablature: architrave, frieze, dentil cornice with gilded cymatium, around the room.
    std::vector<float> gx, gz;
    ceilingGrid(gx, gz);
    const float X0 = layout::HALL_MIN_X, X1 = layout::HALL_MAX_X, Z0 = layout::HALL_MIN_Z, Z1 = layout::HALL_MAX_Z;
    std::vector<vec3> loop;
    loop.push_back(vec3(X0, 0, Z1));
    for (float x : gx) loop.push_back(vec3(x, 0, Z1));
    loop.push_back(vec3(X1, 0, Z1));
    for (auto it = gz.rbegin(); it != gz.rend(); ++it) loop.push_back(vec3(X1, 0, *it));
    loop.push_back(vec3(X1, 0, Z0));
    for (auto it = gx.rbegin(); it != gx.rend(); ++it) loop.push_back(vec3(*it, 0, Z0));
    loop.push_back(vec3(X0, 0, Z0));
    for (float z : gz) loop.push_back(vec3(X0, 0, z));
    const float E = ENTABLATURE_BOTTOM, H = layout::HALL_HEIGHT, C = CORNICE_PROJECTION;
    Profile ent = {{0.0f, E},          {0.07f, E},         {0.07f, E},         {0.07f, E + 0.08f},  {0.078f, E + 0.083f},
                   {0.085f, E + 0.092f}, {0.085f, E + 0.092f}, {0.085f, E + 0.16f}, {0.093f, E + 0.163f}, {0.1f, E + 0.172f},
                   {0.1f, E + 0.172f}, {0.1f, E + 0.225f}, {0.11f, E + 0.23f}, {0.12f, E + 0.24f},  {0.126f, E + 0.252f},
                   {0.13f, E + 0.27f}, {0.13f, E + 0.27f}, {0.13f, E + 0.28f}, {0.13f, E + 0.28f},  {0.05f, E + 0.28f},
                   {0.05f, E + 0.28f}, {0.05f, 8.42f},     {0.05f, 8.42f},     {0.07f, 8.425f},     {0.088f, 8.438f},
                   {0.1f, 8.455f},     {0.108f, 8.47f},    {0.11f, 8.48f},     {0.11f, 8.48f},      {0.11f, 8.585f},
                   {0.11f, 8.585f},    {0.42f, 8.585f},    {0.42f, 8.585f},    {0.42f, 8.76f},      {0.42f, 8.76f},
                   {0.43f, 8.76f},     {0.43f, 8.76f}};
    sweep(stone, ent, loop, vec3(0, 1, 0), true, 30.0f);
    Profile cyma = {{0.43f, 8.76f},  {0.43f, 8.775f}, {0.43f, 8.775f}, {0.445f, 8.785f}, {0.462f, 8.8f},  {0.476f, 8.82f},
                    {0.486f, 8.845f}, {0.495f, 8.868f}, {0.508f, 8.888f}, {0.52f, 8.908f},  {0.528f, 8.93f}, {C, 8.95f},
                    {C, 8.95f},      {C, H}};
    sweep(gilt, cyma, loop, vec3(0, 1, 0), true, 30.0f);
    // Gilded bead-and-reel under the architrave's top moulding.
    Profile bead;
    for (int i = 0; i <= 6; ++i) {
        float th = -0.5f * PI + PI * float(i) / 6.0f;
        bead.push_back({0.1f + 0.009f * std::cos(th), E + 0.181f + 0.009f * std::sin(th)});
    }
    sweep(gilt, bead, loop, vec3(0, 1, 0), true, 30.0f);

    // Dentils under the corona, and gilded paterae in the frieze above each pilaster.
    for (int wid = 0; wid < 4; ++wid) {
        const WallDef& w = wall(wid);
        const float pitch = 0.08f, dw = 0.048f;
        float usable = w.length - 2.0f * 0.2f;
        int n = int(usable / pitch);
        float start = (w.length - float(n - 1) * pitch) * 0.5f;
        for (int i = 0; i < n; ++i) {
            float uc = start + pitch * float(i);
            vec3 c = w.P(uc, 8.535f, 0.11f + 0.03f);
            box(stone, c, w.U, vec3(0, 1, 0), w.N, vec3(dw * 0.5f, 0.05f, 0.03f), F_PX | F_NX | F_NY | F_PZ);
        }
        for (float up : w.pilasters) rosette(gilt, w.P(up, 8.275f, 0.05f), w.N, 0.1f, 8, 0.04f);
        // Garland-like gilded tablets between the paterae (simple raised plaques).
        std::vector<float> ps = w.pilasters;
        std::sort(ps.begin(), ps.end());
        for (size_t k = 0; k + 1 < ps.size(); ++k) {
            float mid = 0.5f * (ps[k] + ps[k + 1]);
            float half = std::min(1.2f, 0.5f * (ps[k + 1] - ps[k]) - 0.3f);
            if (half < 0.3f) continue;
            raisedPanel(stone, gilt, w, mid - half, mid + half, 8.18f, 8.37f, 0.05f, 0.03f, 0.008f);
        }
    }
}

}  // namespace detail
}  // namespace hall
