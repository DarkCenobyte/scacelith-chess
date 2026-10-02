// Hall decor: the double doors of the -Z wall with their overdoor, the six tapestries, the crystal
// chandelier, paintings and console tables.
#include "hall_internal.h"
#include "../render/renderer.h"

using namespace m;

namespace hall {
namespace detail {

namespace {

Profile circleProfile(float r, int n) {
    Profile p;
    for (int i = 0; i <= n; ++i) {
        float th = TAU * float(i) / float(n);
        p.push_back({r * std::cos(th), r * std::sin(th)});
    }
    return p;
}

// Octahedral crystal bead.
void bead(MeshData& d, vec3 c, float r) {
    vec3 v[6] = {c + vec3(r, 0, 0), c - vec3(r, 0, 0), c + vec3(0, r * 1.3f, 0), c - vec3(0, r * 1.3f, 0), c + vec3(0, 0, r), c - vec3(0, 0, r)};
    int f[8][3] = {{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {0, 4, 3}, {4, 1, 3}, {1, 5, 3}, {5, 0, 3}};
    for (auto& t : f) {
        vec3 n = normalize(cross(v[t[1]] - v[t[0]], v[t[2]] - v[t[0]]));
        if (dot(n, v[t[0]] - c) < 0) n = -n;
        uint32_t a = vtx(d, v[t[0]], n, orthogonal(n), vec2(0, 0));
        uint32_t b = vtx(d, v[t[1]], n, orthogonal(n), vec2(1, 0));
        uint32_t e = vtx(d, v[t[2]], n, orthogonal(n), vec2(0, 1));
        tri(d, a, b, e);
    }
}

// Faceted pendeloque drop hanging with its top at 'top', length len.
void pendeloque(MeshData& d, vec3 top, float len) {
    float w = len * 0.28f;
    Profile pr = {{0.0f, -len}, {w * 0.55f, -len * 0.82f}, {w, -len * 0.55f}, {w * 0.85f, -len * 0.3f}, {w * 0.45f, -len * 0.1f}, {0.0f, 0.0f}};
    MeshData l = prim::lathe(pr, 6);
    l.computeNormals(false);  // faceted
    d.append(l, translate(top));
}

void lathePart(MeshData& d, const Profile& p, int seg, vec3 at) { lathe(d, p, seg, translate(at)); }

}  // namespace

// ------------------------------------------------------------------------------------------------
void buildDoor(Accum& a, MeshData& paintings) {
    const WallDef& w = wall(WALL_NORTH);
    MeshData& wood = a[MaterialId::WallPanelWood];
    MeshData& gilt = a[MaterialId::GildedTrim];
    MeshData& stone = a[MaterialId::WallStone];
    const float hw = DOOR_WIDTH * 0.5f, H = DOOR_HEIGHT, uc = DOOR_U;
    const float lin = 0.02f;             // lining thickness
    const float leafN = -0.12f;          // leaf face plane
    // Reveal linings (jambs and soffit) in front of the leaves.
    quadFlat(wood, w.P(uc - hw + lin, 0, leafN), w.P(uc - hw + lin, 0, 0), w.P(uc - hw + lin, H - lin, 0), w.P(uc - hw + lin, H - lin, leafN), w.U, w.N);
    quadFlat(wood, w.P(uc + hw - lin, 0, leafN), w.P(uc + hw - lin, 0, 0), w.P(uc + hw - lin, H - lin, 0), w.P(uc + hw - lin, H - lin, leafN), -w.U, w.N);
    quadFlat(wood, w.P(uc - hw + lin, H - lin, leafN), w.P(uc + hw - lin, H - lin, leafN), w.P(uc + hw - lin, H - lin, 0), w.P(uc - hw + lin, H - lin, 0),
             vec3(0, -1, 0), w.U);
    // Lining front edges (between the lining and the wall's reveal).
    quadFlat(wood, w.P(uc - hw, 0, 0), w.P(uc - hw + lin, 0, 0), w.P(uc - hw + lin, H - lin, 0), w.P(uc - hw, H, 0), w.N, w.U);
    quadFlat(wood, w.P(uc + hw - lin, 0, 0), w.P(uc + hw, 0, 0), w.P(uc + hw, H, 0), w.P(uc + hw - lin, H - lin, 0), w.N, w.U);
    quadFlat(wood, w.P(uc - hw, H, 0), w.P(uc + hw, H, 0), w.P(uc + hw - lin, H - lin, 0), w.P(uc - hw + lin, H - lin, 0), w.N, w.U);
    // Leaves.
    for (int s = 0; s < 2; ++s) {
        float u0 = s == 0 ? uc - hw + lin : uc, u1 = s == 0 ? uc : uc + hw - lin;
        quadFlat(wood, w.P(u0, 0, leafN), w.P(u1, 0, leafN), w.P(u1, H - lin, leafN), w.P(u0, H - lin, leafN), w.N, w.U);
        const float st = 0.15f;
        float pu0 = u0 + st, pu1 = u1 - st;
        raisedPanel(wood, gilt, w, pu0, pu1, 0.22f, 0.98f, leafN, 0.05f, 0.016f);
        raisedPanel(wood, gilt, w, pu0, pu1, 1.18f, 1.72f, leafN, 0.05f, 0.016f);
        raisedPanel(wood, gilt, w, pu0, pu1, 1.92f, H - 0.24f, leafN, 0.06f, 0.016f);
        // Gilded ornaments: rosettes in the middle panels and a lozenge in the tall panel.
        float pc = 0.5f * (pu0 + pu1);
        rosette(gilt, w.P(pc, 1.45f, leafN + 0.016f), w.N, 0.1f, 8, 0.03f);
        std::vector<vec3> lz = {w.P(pc, 2.35f, leafN + 0.016f), w.P(pc + 0.26f, 3.15f, leafN + 0.016f), w.P(pc, 3.95f, leafN + 0.016f),
                                w.P(pc - 0.26f, 3.15f, leafN + 0.016f)};
        Profile hr;
        for (int i = 0; i <= 6; ++i) {
            float th = PI * float(i) / 6.0f;
            hr.push_back({0.01f * std::cos(th), 0.01f * std::sin(th)});
        }
        sweep(gilt, hr, lz, w.N, true, 20.0f);
        rosette(gilt, w.P(pc, 3.15f, leafN + 0.016f), w.N, 0.07f, 6, 0.03f);
        // Lever handle with an escutcheon near the meeting stiles.
        float hu = s == 0 ? uc - 0.08f : uc + 0.08f;
        box(gilt, w.P(hu, 1.05f, leafN + 0.006f), w.U, vec3(0, 1, 0), w.N, vec3(0.022f, 0.11f, 0.006f));
        vec3 knob = w.P(hu, 1.08f, leafN + 0.05f);
        box(gilt, w.P(hu, 1.08f, leafN + 0.028f), w.U, vec3(0, 1, 0), w.N, vec3(0.008f, 0.008f, 0.022f));
        float dir = s == 0 ? -1.0f : 1.0f;
        box(gilt, knob + w.U * (dir * 0.06f), w.U, vec3(0, 1, 0), w.N, vec3(0.07f, 0.009f, 0.009f));
    }
    // Astragal on the meeting stiles.
    {
        Profile hr;
        for (int i = 0; i <= 6; ++i) {
            float th = PI * float(i) / 6.0f;
            hr.push_back({0.018f * std::cos(th), 0.018f * std::sin(th)});
        }
        sweep(gilt, hr, {w.P(uc, 0, leafN), w.P(uc, H - lin, leafN)}, w.N, false, 30.0f, true, true);
    }
    // Architrave around the opening (s points away from the opening): stone fasciae, gilded cyma.
    std::vector<vec3> path = {w.P(uc - hw, 0, 0), w.P(uc - hw, H, 0), w.P(uc + hw, H, 0), w.P(uc + hw, 0, 0)};
    Profile arStone = {{0.19f, 0.07f}, {0.19f, 0.058f}, {0.19f, 0.058f}, {0.12f, 0.058f}, {0.12f, 0.058f}, {0.12f, 0.046f},
                       {0.12f, 0.046f}, {0.03f, 0.046f}, {0.025f, 0.05f}, {0.012f, 0.053f}, {0.004f, 0.046f}, {0.0f, 0.035f},
                       {0.0f, 0.035f}, {0.0f, 0.0f}};
    Profile arGilt = {{0.24f, 0.0f}, {0.24f, 0.035f}, {0.24f, 0.035f}, {0.233f, 0.05f}, {0.222f, 0.062f}, {0.207f, 0.069f}, {0.19f, 0.07f}};
    sweep(stone, arStone, path, w.N, false, 30.0f);
    sweep(gilt, arGilt, path, w.N, false, 30.0f);
    // Overdoor: frieze block with rosettes and a cornice with a gilded cymatium.
    const float fu0 = uc - hw - 0.24f, fu1 = uc + hw + 0.24f, fy0 = H + 0.0f, fy1 = H + 0.36f;
    box(stone, w.P(uc, 0.5f * (fy0 + fy1), 0.035f), w.U, vec3(0, 1, 0), w.N, vec3(0.5f * (fu1 - fu0), 0.5f * (fy1 - fy0), 0.035f), F_ALL & ~F_NZ);
    for (float du : {-0.9f, 0.0f, 0.9f}) rosette(gilt, w.P(uc + du, 0.5f * (fy0 + fy1), 0.07f), w.N, du == 0.0f ? 0.13f : 0.08f, 8, 0.035f);
    std::vector<vec3> cpath = {w.P(fu1, 0, 0), w.P(fu1, 0, 0.07f), w.P(fu0, 0, 0.07f), w.P(fu0, 0, 0)};
    Profile corn = {{0.0f, fy1},          {0.015f, fy1 + 0.005f}, {0.028f, fy1 + 0.015f}, {0.036f, fy1 + 0.03f}, {0.04f, fy1 + 0.045f},
                    {0.04f, fy1 + 0.045f}, {0.15f, fy1 + 0.045f}, {0.15f, fy1 + 0.045f}, {0.15f, fy1 + 0.13f}, {0.15f, fy1 + 0.13f}};
    Profile cornG = {{0.15f, fy1 + 0.13f}, {0.16f, fy1 + 0.135f}, {0.172f, fy1 + 0.15f}, {0.182f, fy1 + 0.172f}, {0.19f, fy1 + 0.2f},
                     {0.19f, fy1 + 0.2f},  {0.19f, fy1 + 0.215f}, {0.19f, fy1 + 0.215f}, {0.0f, fy1 + 0.215f}};
    sweep(stone, corn, cpath, vec3(0, 1, 0), false, 30.0f, true, true);
    sweep(gilt, cornG, cpath, vec3(0, 1, 0), false, 30.0f, true, true);
    // Sopraporta painting above the cornice.
    framedPainting(a, paintings, w, uc, fy1 + 0.215f + 0.14f + 0.7f, 1.8f, 1.4f, 0.14f);
}

// ------------------------------------------------------------------------------------------------
void buildTapestries(Accum& a, std::vector<ModelPart>& parts) {
    struct T { int wall; float u, width; float color; };
    const T list[] = {
        {WALL_EAST, 11.0f - 5.2f, TAPESTRY_W, 1.0f},  {WALL_EAST, 11.0f, TAPESTRY_W, 0.0f},      {WALL_EAST, 11.0f + 5.2f, TAPESTRY_W, 1.0f},
        {WALL_NORTH, 7.0f - 4.3f, TAPESTRY_NARROW_W, 0.0f}, {WALL_NORTH, 7.0f + 4.3f, TAPESTRY_NARROW_W, 0.0f},
        {WALL_SOUTH, 7.0f, TAPESTRY_W, 1.0f},
    };
    MeshData& gilt = a[MaterialId::GildedTrim];
    int idx = 0;
    for (const T& t : list) {
        const WallDef& w = wall(t.wall);
        const float Wt = t.width, Ht = TAPESTRY_H, yTop = TAPESTRY_TOP_Y;
        Rng rng(uint64_t(100 + idx));
        float p1 = rng.range(0, TAU), p2 = rng.range(0, TAU), p3 = rng.range(0, TAU);
        auto fn = [&](float s, float tt, float back) {
            float u = t.u + (s - 0.5f) * Wt;
            float hang = 1.0f - tt;
            float y = yTop - hang * Ht - 0.02f * std::sin(PI * s) * hang * hang * hang;
            float folds = 0.016f * std::sin(TAU * 3.0f * s + p1) * (0.3f + 0.7f * hang) +
                          0.007f * std::sin(TAU * 6.6f * s + p2) * hang * hang + 0.004f * std::sin(TAU * 11.3f * s + p3) * hang;
            // Anchored at the top (hanging tabs every ~0.4 m make small scallops).
            float top = smoothstep(0.985f, 0.94f, tt);
            float scallop = (1.0f - top) * 0.006f * std::fabs(std::sin(PI * s * Wt / 0.4f));
            float n = 0.045f + folds * top + 0.012f * hang * hang - scallop - back;
            return w.P(u, y, n);
        };
        ModelPart p;
        p.name = "hall_tapestry_" + std::to_string(idx);
        p.material = MaterialId::Tapestry;
        p.flags = render::DRAW_STATIC | render::DRAW_CAST_SHADOW;
        const float seed = float(idx) * 0.137f + 0.21f;
        p.inst[0] = vec4(t.color, seed, std::fmod(seed * 7.31f, 1.0f), 0.0f);  // tapestry.glsl: colour, seeds
        p.inst[1] = vec4(Wt, Ht, 0.0f, 0.0f);                                      // size in m
        surface(p.mesh, 72, 44, [&](float s, float tt) { return fn(s, tt, 0.0f); }, false, UVMode::Param);
        surface(p.mesh, 72, 44, [&](float s, float tt) { return fn(s, tt, 0.008f); }, true, UVMode::Param);
        parts.push_back(std::move(p));
        // Gilded fringe along the bottom edge and tassels at the corners.
        int nStr = int(Wt / 0.013f);
        for (int i = 0; i < nStr; ++i) {
            float s = (float(i) + 0.5f) / float(nStr);
            vec3 b = fn(s, 0.0f, 0.004f);
            float len = 0.075f + 0.01f * std::sin(float(i) * 1.7f);
            box(gilt, b + vec3(0, -len * 0.5f + 0.004f, 0), w.U, vec3(0, 1, 0), w.N, vec3(0.0032f, len * 0.5f, 0.0025f), F_ALL & ~F_PY);
        }
        // Braid over the fringe heading.
        std::vector<vec3> braid;
        for (int i = 0; i <= 48; ++i) braid.push_back(fn(float(i) / 48.0f, 0.004f, -0.004f));
        sweep(gilt, circleProfile(0.007f, 6), braid, vec3(0, 1, 0), false, 60.0f);
        Profile tassel = {{0.0f, -0.26f}, {0.04f, -0.25f}, {0.046f, -0.22f}, {0.04f, -0.14f}, {0.03f, -0.1f},
                          {0.034f, -0.085f}, {0.026f, -0.06f}, {0.014f, -0.03f}, {0.008f, -0.005f}, {0.0f, 0.0f}};
        for (float s : {0.0f, 1.0f}) {
            vec3 b = fn(s, 0.0f, 0.0f) + w.N * 0.02f;
            latheMod(gilt, tassel, 20, translate(b), [](float r, float ang, float y) {
                return y < -0.1f ? r * (1.0f - 0.12f * std::fabs(std::sin(ang * 10.0f))) : r;
            }, 2);
        }
        // Rod with finials and wall brackets.
        float half = Wt * 0.5f + 0.12f, ry = yTop + 0.03f, rn = 0.058f;
        quat q = fromTo(vec3(0, 1, 0), w.U);
        lathe(gilt, {{0.0f, 0.0f}, {0.022f, 0.0f}, {0.022f, 0.0f}, {0.022f, 2.0f * half}, {0.022f, 2.0f * half}, {0.0f, 2.0f * half}}, 16,
              toMat4(q, w.P(t.u - half, ry, rn)));
        Profile fin = {{0.0f, 0.0f}, {0.03f, 0.0f}, {0.03f, 0.0f}, {0.03f, 0.015f}, {0.022f, 0.025f}, {0.036f, 0.05f}, {0.04f, 0.07f},
                       {0.034f, 0.09f}, {0.02f, 0.105f}, {0.012f, 0.115f}, {0.016f, 0.125f}, {0.0f, 0.15f}};
        lathe(gilt, fin, 16, toMat4(q, w.P(t.u + half, ry, rn)));
        lathe(gilt, fin, 16, toMat4(fromTo(vec3(0, 1, 0), -w.U), w.P(t.u - half, ry, rn)));
        for (float s : {-1.0f, 1.0f}) {
            float ub = t.u + s * (half - 0.06f);
            box(gilt, w.P(ub, ry, rn * 0.5f), w.U, vec3(0, 1, 0), w.N, vec3(0.012f, 0.014f, rn * 0.5f));
            rosette(gilt, w.P(ub, ry, 0.0f), w.N, 0.04f, 8, 0.015f);
        }
        ++idx;
    }
}

// ------------------------------------------------------------------------------------------------
// Chandelier arms at the design size: two tiers of n S-scrolls from the hub (hubR, hubY), dipping
// by 'dip', to a bobeche and candle at (cupR, cupY); the arms of a tier start rot / n of a turn
// from +X. Shared by chandelierBody and chandelierCandles.
struct ChandelierTier { int n; float hubR, hubY, cupR, cupY, dip, rot; };
constexpr ChandelierTier kChandelierTiers[2] = {{12, 0.08f, 5.42f, 0.86f, CHANDELIER_Y, 0.14f, 0.0f},  // lower candle ring
                                                {6, 0.06f, 6.2f, 0.5f, 6.36f, 0.08f, 0.5f}};

// Chandelier body at its design size (candle ring radius 0.86 m); scaled up by CHANDELIER_SCALE
// about the candle ring by buildChandelier.
static void chandelierBody(Accum& a) {
    MeshData& brass = a[MaterialId::Brass];
    MeshData& crystal = a[MaterialId::Crystal];
    MeshData& wax = a[MaterialId::CandleWax];
    // Central stem (from the bottom finial to the top hook).
    Profile stem = {{0.0f, 4.62f},  {0.018f, 4.64f}, {0.034f, 4.7f},  {0.036f, 4.76f}, {0.024f, 4.82f}, {0.03f, 4.86f},
                    {0.07f, 4.9f},  {0.15f, 4.98f},  {0.21f, 5.07f},  {0.24f, 5.16f},  {0.245f, 5.2f},  {0.235f, 5.215f},
                    {0.2f, 5.22f},  {0.12f, 5.235f}, {0.06f, 5.26f},  {0.045f, 5.3f},  {0.05f, 5.34f},  {0.08f, 5.38f},
                    {0.085f, 5.41f}, {0.08f, 5.45f},  {0.05f, 5.49f},  {0.035f, 5.56f}, {0.034f, 5.75f}, {0.05f, 5.85f},
                    {0.07f, 5.95f}, {0.075f, 6.05f}, {0.062f, 6.12f}, {0.05f, 6.15f},  {0.055f, 6.18f}, {0.05f, 6.21f},
                    {0.028f, 6.26f}, {0.024f, 6.5f},  {0.045f, 6.58f}, {0.06f, 6.68f},  {0.055f, 6.78f}, {0.035f, 6.85f},
                    {0.022f, 6.95f}, {0.02f, 7.1f},   {0.032f, 7.16f}, {0.02f, 7.2f},   {0.0f, 7.22f}};
    latheMod(brass, stem, 32, mat4(), [](float r, float ang, float y) {
        float g = smoothstep(4.95f, 5.02f, y) * smoothstep(5.17f, 5.1f, y);  // gadrooned bowl
        return r * (1.0f - 0.07f * g * (1.0f - std::fabs(std::cos(ang * 12.0f))));
    }, 2);
    // Crown ring with drops.
    {
        const float yc = 6.9f, R = 0.3f;
        std::vector<vec3> ring;
        for (int k = 0; k < 32; ++k) {
            float th = TAU * float(k) / 32.0f;
            ring.push_back(vec3(R * std::cos(th), yc, R * std::sin(th)));
        }
        sweep(brass, circleProfile(0.01f, 6), ring, vec3(0, 1, 0), true);
        for (int k = 0; k < 4; ++k) {
            float th = TAU * (float(k) + 0.5f) / 4.0f;
            vec3 dir(std::cos(th), 0, std::sin(th));
            sweep(brass, circleProfile(0.008f, 6), {dir * 0.03f + vec3(0, 6.98f, 0), dir * R + vec3(0, yc, 0)}, normalize(cross(dir, vec3(0, 1, 0))),
                  false);
        }
        for (int k = 0; k < 16; ++k) {
            float th = TAU * float(k) / 16.0f;
            vec3 p(R * std::cos(th), yc - 0.012f, R * std::sin(th));
            bead(crystal, p - vec3(0, 0.015f, 0), 0.01f);
            pendeloque(crystal, p - vec3(0, 0.03f, 0), 0.09f);
        }
    }
    // Arms: two tiers of S-scrolls ending in bobeches and candles.
    Profile bob = {{0.0f, -0.03f}, {0.012f, -0.028f}, {0.018f, -0.018f}, {0.03f, -0.006f}, {0.06f, 0.0f}, {0.066f, 0.006f},
                   {0.06f, 0.012f}, {0.024f, 0.012f}, {0.022f, 0.016f}, {0.022f, 0.06f}, {0.026f, 0.064f}, {0.016f, 0.066f}, {0.0f, 0.066f}};
    Profile candle = {{0.0f, 0.0f}, {0.0125f, 0.0f}, {0.0125f, 0.0f}, {0.0125f, 0.15f}, {0.011f, 0.158f}, {0.006f, 0.162f},
                      {0.002f, 0.163f}, {0.001f, 0.172f}, {0.0f, 0.173f}};
    std::vector<vec3> lowCups;
    for (int ti = 0; ti < 2; ++ti) {
        const ChandelierTier& T = kChandelierTiers[ti];
        for (int k = 0; k < T.n; ++k) {
            float th = TAU * (float(k) + T.rot) / float(T.n);
            vec3 dir(std::cos(th), 0, std::sin(th));
            vec3 W = normalize(cross(dir, vec3(0, 1, 0)));
            // Cubic Bezier S-curve in the (radius, height) plane.
            vec2 b0(T.hubR, T.hubY), b1(T.hubR + (T.cupR - T.hubR) * 0.35f, T.hubY - T.dip * 1.6f),
                b2(T.hubR + (T.cupR - T.hubR) * 0.85f, T.hubY - T.dip), b3(T.cupR, T.cupY - 0.035f);
            std::vector<vec3> path;
            for (int i = 0; i <= 28; ++i) {
                float t = float(i) / 28.0f, it = 1.0f - t;
                vec2 q = b0 * (it * it * it) + b1 * (3 * it * it * t) + b2 * (3 * it * t * t) + b3 * (t * t * t);
                path.push_back(dir * q.x + vec3(0, q.y, 0));
            }
            sweep(brass, circleProfile(ti == 0 ? 0.014f : 0.011f, 8), path, W, false, 80.0f);
            // Small scroll curl under the arm near the hub.
            std::vector<vec3> curl;
            for (int i = 0; i <= 16; ++i) {
                float a2 = PI * 1.6f * float(i) / 16.0f;
                float rr = 0.05f * (1.0f - 0.45f * float(i) / 16.0f);
                vec2 q = vec2(T.hubR + 0.12f, T.hubY - 0.07f) + vec2(std::cos(a2 + 1.2f), std::sin(a2 + 1.2f)) * rr;
                curl.push_back(dir * q.x + vec3(0, q.y, 0));
            }
            sweep(brass, circleProfile(0.007f, 6), curl, W, false, 80.0f);
            vec3 cup = dir * T.cupR + vec3(0, T.cupY, 0);
            lathePart(brass, bob, 20, cup);
            lathePart(wax, candle, 14, cup + vec3(0, 0.066f, 0));
            // Drops on the bobeche rim.
            for (int j = 0; j < 4; ++j) {
                float ph = TAU * (float(j) + 0.5f) / 4.0f;
                vec3 rim = cup + vec3(0.055f * std::cos(ph), -0.004f, 0.055f * std::sin(ph));
                bead(crystal, rim - vec3(0, 0.012f, 0), 0.008f);
                pendeloque(crystal, rim - vec3(0, 0.024f, 0), ti == 0 ? 0.075f : 0.06f);
            }
            if (ti == 0) lowCups.push_back(cup);
        }
    }
    // Festoons of beads between the lower cups, and a cascade around the bowl.
    for (size_t k = 0; k < lowCups.size(); ++k) {
        vec3 p0 = lowCups[k] - vec3(0, 0.01f, 0), p1 = lowCups[(k + 1) % lowCups.size()] - vec3(0, 0.01f, 0);
        const int nb = 14;
        for (int i = 1; i < nb; ++i) {
            float t = float(i) / float(nb);
            vec3 p = lerp3(p0, p1, t) - vec3(0, 0.26f * 4.0f * t * (1.0f - t), 0);
            p = p * vec3(0.96f, 1.0f, 0.96f);
            bead(crystal, p, 0.0105f);
        }
        // Long drop hanging at the bottom of the festoon.
        vec3 mid = lerp3(p0, p1, 0.5f) * vec3(0.96f, 1.0f, 0.96f) - vec3(0, 0.26f, 0);
        pendeloque(crystal, mid - vec3(0, 0.012f, 0), 0.11f);
    }
    for (int ring = 0; ring < 2; ++ring) {
        int n = ring == 0 ? 24 : 16;
        float R = ring == 0 ? 0.235f : 0.16f, y = ring == 0 ? 5.19f : 5.0f;
        for (int k = 0; k < n; ++k) {
            float th = TAU * (float(k) + 0.25f * float(ring)) / float(n);
            vec3 p(R * std::cos(th), y, R * std::sin(th));
            for (int c = 0; c < 3; ++c) bead(crystal, p - vec3(0, 0.02f + 0.028f * float(c), 0), 0.009f);
            pendeloque(crystal, p - vec3(0, 0.1f, 0), ring == 0 ? 0.12f : 0.1f);
        }
    }
    // Central bottom drop.
    pendeloque(crystal, vec3(0, 4.62f, 0), 0.16f);
}

static constexpr float CH_DESIGN_TOP = 7.22f;  // top of the stem hook at design size

void buildChandelier(Accum& a) {
    Accum body;
    chandelierBody(body);
    const vec3 pivot(0, CHANDELIER_Y, 0);
    for (MaterialId m : {MaterialId::Brass, MaterialId::Crystal, MaterialId::CandleWax}) {
        MeshData& d = body[m];
        for (Vertex& v : d.vertices) v.pos = pivot + (v.pos - pivot) * CHANDELIER_SCALE;
        a[m].append(d);
    }
    MeshData& brass = a[MaterialId::Brass];
    // Chain from the stem hook up to the ceiling rose.
    {
        float y = CHANDELIER_Y + (CH_DESIGN_TOP - CHANDELIER_Y) * CHANDELIER_SCALE;
        int i = 0;
        const float L = 0.14f, Wd = 0.07f, rr = 0.011f;
        while (y < PLAFOND_TOP - 0.14f) {
            vec3 c(0, y + L * 0.5f - rr, 0);
            vec3 side = (i & 1) ? vec3(1, 0, 0) : vec3(0, 0, 1);
            std::vector<vec3> loop;
            for (int k = 0; k < 16; ++k) {
                float th = TAU * float(k) / 16.0f;
                float hx = (Wd * 0.5f - rr), hy = (L * 0.5f - rr);
                loop.push_back(c + side * (hx * std::cos(th)) + vec3(0, hy * std::sin(th), 0));
            }
            sweep(brass, circleProfile(rr, 6), loop, normalize(cross(side, vec3(0, 1, 0))), true, 80.0f);
            y += L - 2.0f * rr - 0.016f;
            ++i;
        }
    }
}

// ------------------------------------------------------------------------------------------------
void buildProps(Accum& a, MeshData& paintings) {
    MeshData& gilt = a[MaterialId::GildedTrim];
    MeshData& stone = a[MaterialId::WallStone];
    // Console table against a wall at u, with a stone top, gilded fluted legs, an urn on top.
    auto console = [&](const WallDef& w, float u) {
        const float len = 1.5f, dep = 0.46f, topY = 0.88f, n0 = 0.075f;
        const float legH = topY - 0.04f;
        MeshData legs;
        for (float su : {-1.0f, 1.0f})
            for (float nn : {n0 + 0.06f, n0 + dep - 0.06f}) {
                vec3 base = w.P(u + su * (len * 0.5f - 0.07f), 0.0f, nn);
                louisLeg(legs, legs, base, legH, 0.03f, 0.12f, 0.035f, 10);
            }
        gilt.append(legs);
        // Apron with rosettes over the legs and a central cartouche.
        box(gilt, w.P(u, legH - 0.06f, n0 + dep - 0.045f), w.U, vec3(0, 1, 0), w.N, vec3(len * 0.5f - 0.1f, 0.055f, 0.012f));
        for (float su : {-1.0f, 1.0f}) {
            box(gilt, w.P(u + su * (len * 0.5f - 0.07f), legH - 0.06f, n0 + dep * 0.5f), w.U, vec3(0, 1, 0), w.N, vec3(0.012f, 0.055f, dep * 0.5f - 0.1f));
            rosette(gilt, w.P(u + su * (len * 0.5f - 0.07f), legH - 0.06f, n0 + dep - 0.024f), w.N, 0.03f, 8, 0.015f);
        }
        rosette(gilt, w.P(u, legH - 0.06f, n0 + dep - 0.033f), w.N, 0.06f, 10, 0.025f);
        // Stretcher shelf between the legs.
        box(gilt, w.P(u, 0.16f, n0 + dep * 0.5f), w.U, vec3(0, 1, 0), w.N, vec3(len * 0.5f - 0.07f, 0.012f, dep * 0.5f - 0.06f));
        // Top slab with a moulded edge (stone).
        std::vector<vec3> path = {w.P(u + len * 0.5f + 0.03f, 0, n0), w.P(u + len * 0.5f + 0.03f, 0, n0 + dep + 0.03f),
                                  w.P(u - len * 0.5f - 0.03f, 0, n0 + dep + 0.03f), w.P(u - len * 0.5f - 0.03f, 0, n0)};
        Profile edge = {{-0.03f, legH}, {0.0f, legH}, {0.0f, legH}, {0.0f, legH + 0.012f}, {0.01f, legH + 0.018f},
                        {0.016f, legH + 0.028f}, {0.012f, topY - 0.004f}, {0.0f, topY}, {-0.03f, topY}};
        sweep(stone, edge, path, vec3(0, 1, 0), false, 30.0f);
        quadFlat(stone, w.P(u - len * 0.5f, topY, n0), w.P(u + len * 0.5f, topY, n0), w.P(u + len * 0.5f, topY, n0 + dep),
                 w.P(u - len * 0.5f, topY, n0 + dep), vec3(0, 1, 0), w.U);
        quadFlat(stone, w.P(u - len * 0.5f, legH, n0), w.P(u + len * 0.5f, legH, n0), w.P(u + len * 0.5f, legH, n0 + dep),
                 w.P(u - len * 0.5f, legH, n0 + dep), vec3(0, -1, 0), w.U);
        urn(gilt, w.P(u, topY, n0 + dep * 0.5f), 0.5f);
    };
    // Long walls: paintings above consoles in the narrow end bays (both walls, both ends).
    for (int wid : {WALL_WINDOWS, WALL_EAST}) {
        const WallDef& w = wall(wid);
        for (float u : {1.95f, w.length - 1.95f}) {
            framedPainting(a, paintings, w, u, 3.4f, 1.2f, 1.8f, 0.14f);
            console(w, u);
        }
    }
    // +Z wall side bays: large paintings above consoles.
    {
        const WallDef& w = wall(WALL_SOUTH);
        for (float u : {2.7f, 11.3f}) {
            framedPainting(a, paintings, w, u, 4.1f, 2.2f, 2.9f, 0.18f);
            console(w, u);
        }
    }
}

}  // namespace detail

void chandelierCandles(std::vector<m::vec3>& flames) {
    // The arm tiers of chandelierBody (design size), scaled about the candle ring.
    const float flameAbove = 0.066f + 0.19f;  // bobeche + candle, flame centre above the wick
    flames.clear();
    for (const detail::ChandelierTier& T : detail::kChandelierTiers)
        for (int k = 0; k < T.n; ++k) {
            float th = m::TAU * (float(k) + T.rot) / float(T.n);
            m::vec3 p(T.cupR * std::cos(th), T.cupY + flameAbove, T.cupR * std::sin(th));
            m::vec3 pivot(0, CHANDELIER_Y, 0);
            flames.push_back(pivot + (p - pivot) * CHANDELIER_SCALE);
        }
}

}  // namespace hall
