// Windows of the -X wall: moulded frames, mullion and transom, two glazed casements with small
// panes, sunburst fanlight, thin glass, and heavy velvet curtains tied back on gilded rods.
#include "hall_internal.h"

using namespace m;

namespace hall {
namespace detail {

namespace {

const float SILL = layout::WINDOW_SILL_Y;
const float SPRING = WINDOW_SPRING_Y;
const float FN = -WINDOW_FRAME_DEPTH;  // frame plane (n)

Profile rectProfile(float hb, float ha) {  // closed rectangle centred on the path
    return {{hb, -ha}, {hb, ha}, {hb, ha}, {-hb, ha}, {-hb, ha}, {-hb, -ha}, {-hb, -ha}, {hb, -ha}};
}

void windowFrame(Accum& a, MeshData& glass, const WallDef& w, float uc) {
    MeshData& fr = a[MaterialId::WindowFrame];
    const float OW = WIN_HALF;       // frame outer edge (against the reveal)
    const float FW = 0.10f;          // outer frame width
    const float IW = OW - FW;        // clear half width inside the frame
    // Outer frame: closed sweep with s pointing into the opening.
    std::vector<vec2> ol = archOutline(uc, OW, SILL, SPRING, 32);
    std::vector<vec3> path;
    for (auto it = ol.rbegin(); it != ol.rend(); ++it) path.push_back(wp(w, *it, FN));
    Profile outer = {{0.0f, -0.045f}, {FW, -0.045f}, {FW, -0.045f}, {FW, 0.018f}, {FW - 0.008f, 0.03f}, {FW - 0.022f, 0.04f},
                     {FW - 0.035f, 0.045f}, {FW - 0.035f, 0.045f}, {0.0f, 0.045f}};
    sweep(fr, outer, path, w.N, true, 30.0f);
    const float vb = SILL + FW, vt = SPRING;  // casement zone
    // Transom and mullion.
    wallBar(fr, w, {uc - IW, vt}, {uc + IW, vt}, FN, 0.09f, 0.085f);
    wallBar(fr, w, {uc, vb}, {uc, vt - 0.045f}, FN, 0.085f, 0.08f);
    // Casements: sash frames and glazing bars (3 x 9 panes each).
    for (int side = 0; side < 2; ++side) {
        float u0 = side == 0 ? uc - IW : uc + 0.0425f, u1 = side == 0 ? uc - 0.0425f : uc + IW;
        float v0 = vb, v1 = vt - 0.045f;
        const float S = 0.05f;
        wallBar(fr, w, {u0 + S * 0.5f, v0}, {u0 + S * 0.5f, v1}, FN, S, 0.06f);
        wallBar(fr, w, {u1 - S * 0.5f, v0}, {u1 - S * 0.5f, v1}, FN, S, 0.06f);
        wallBar(fr, w, {u0 + S, v0 + S * 0.6f}, {u1 - S, v0 + S * 0.6f}, FN, S * 1.2f, 0.06f);
        wallBar(fr, w, {u0 + S, v1 - S * 0.5f}, {u1 - S, v1 - S * 0.5f}, FN, S, 0.06f);
        float gu0 = u0 + S, gu1 = u1 - S, gv0 = v0 + S * 1.2f, gv1 = v1 - S;
        for (int c = 1; c < 3; ++c) {
            float uu = lerp(gu0, gu1, float(c) / 3.0f);
            wallBar(fr, w, {uu, gv0}, {uu, gv1}, FN, 0.022f, 0.035f);
        }
        for (int r = 1; r < 9; ++r) {
            float vv = lerp(gv0, gv1, float(r) / 9.0f);
            wallBar(fr, w, {gu0, vv}, {gu1, vv}, FN, 0.022f, 0.035f, F_ALL & ~(F_PX | F_NX));
        }
    }
    // Fanlight: two concentric arcs and radial bars.
    float cy = vt + 0.045f;
    for (float R : {0.42f, 0.84f}) {
        std::vector<vec3> arc;
        for (int i = 0; i <= 24; ++i) {
            float th = PI * float(i) / 24.0f;
            arc.push_back(wp(w, {uc + R * std::cos(th), cy + R * std::sin(th)}, FN));
        }
        sweep(fr, rectProfile(0.012f, 0.018f), arc, w.N, false, 30.0f);
    }
    for (int k = 1; k < 6; ++k) {
        float th = PI * float(k) / 6.0f;
        vec2 dir(std::cos(th), std::sin(th));
        vec2 p0 = vec2(uc, cy) + dir * 0.0f, p1 = vec2(uc, cy) + dir * (IW + 0.01f);
        wallBar(fr, w, p0, p1, FN, 0.024f, 0.036f);
    }
    // Hub of the sunburst.
    std::vector<vec3> hub;
    for (int i = 0; i <= 12; ++i) {
        float th = PI * float(i) / 12.0f;
        hub.push_back(wp(w, {uc + 0.12f * std::cos(th), cy + 0.12f * std::sin(th)}, FN));
    }
    sweep(fr, rectProfile(0.014f, 0.02f), hub, w.N, false, 30.0f);

    // Glass: two thin faces over the whole opening (uv in meters from the opening's corner).
    for (int face = 0; face < 2; ++face) {
        float n = FN + (face == 0 ? 0.002f : -0.002f);
        vec3 nn = face == 0 ? w.N : -w.N;
        uint32_t base = uint32_t(glass.vertices.size());
        for (vec2 p : ol) vtx(glass, wp(w, p, n), nn, w.U, vec2(p.x - (uc - OW), p.y - SILL));
        for (uint32_t i = 1; i + 1 < ol.size(); ++i) tri(glass, base, base + i, base + i + 1);
    }
}

// One curtain panel (side = -1 left of the window seen from inside, +1 right).
void curtain(Accum& a, const WallDef& w, float uc, float side, uint32_t seed) {
    MeshData& cl = a[MaterialId::Curtain];
    const float yTop = 7.67f, yT = 1.25f;
    const float W0 = 1.28f;
    const int K = 7;
    Rng rng(seed);
    float ph0 = rng.range(0.0f, TAU), ph1 = rng.range(0.0f, TAU);
    auto eA = [&](float y) {
        if (y >= yT) {
            float t = (y - yT) / (yTop - yT);
            return 1.66f - 0.36f * (1.0f - std::pow(1.0f - t, 2.2f));
        }
        float t = y / yT;
        return 1.66f - 0.11f * std::pow(1.0f - t, 1.5f);
    };
    auto eB = [&](float y) {
        if (y >= yT) return 1.90f + 0.05f * std::sqrt((y - yT) / (yTop - yT));
        return 1.90f + 0.08f * std::pow(1.0f - y / yT, 1.2f);
    };
    auto fn = [&](float s, float t, float back) {
        float y = t * yTop;
        float ea = eA(y), eb = eB(y);
        float wd = eb - ea;
        float e = side > 0 ? lerp(ea, eb, s) : lerp(eb, ea, s);
        float ratio = W0 / wd;
        float A = std::min(0.075f, wd / (TAU * float(K)) * std::sqrt(std::max(0.0f, 2.0f * (ratio * ratio - 1.0f))));
        float phase = TAU * float(K) * s + ph0 + 0.5f * std::sin(y * 0.7f + ph1) + 0.3f * std::sin(y * 2.1f);
        float fold = A * std::sin(phase);
        // Sharper, deeper folds in the gathered part near the tie-back.
        float gather = std::exp(-sq((y - yT) / 0.6f));
        fold *= 1.0f + 0.3f * gather;
        float n = 0.20f + fold + 0.045f * std::exp(-sq((y - yT - 0.28f) / 0.22f)) - 0.02f * gather;
        if (y < 0.15f) n += 0.05f * sq(1.0f - y / 0.15f);
        n -= back;
        return w.P(uc + side * e, y, n);
    };
    auto front = [&](float s, float t) { return fn(s, t, 0.0f); };
    auto backF = [&](float s, float t) { return fn(s, t, 0.006f); };
    // 71 uniform samples along the drape height.
    std::vector<float> ts = linspace(0.0f, 1.0f, 70);
    std::vector<float> ss = linspace(0.0f, 1.0f, K * 8);
    surfaceGrid(cl, ss, ts, front, false, UVMode::Meters);
    surfaceGrid(cl, ss, ts, backF, true, UVMode::Meters);
    // Tie-back: gilded cord loop around the bundle, tassel, wall rosette.
    MeshData& gilt = a[MaterialId::GildedTrim];
    float ec = 0.5f * (eA(yT) + eB(yT));
    float ra = 0.5f * (eB(yT) - eA(yT)) + 0.09f;
    std::vector<vec3> loop;
    for (int i = 0; i < 24; ++i) {
        float th = TAU * float(i) / 24.0f;
        loop.push_back(w.P(uc + side * (ec + ra * std::cos(th)), yT, 0.2f + 0.1f * std::sin(th)));
    }
    Profile cord;
    for (int i = 0; i <= 8; ++i) {
        float th = TAU * float(i) / 8.0f;
        cord.push_back({0.013f * std::cos(th), 0.013f * std::sin(th)});
    }
    sweep(gilt, cord, loop, vec3(0, 1, 0), true);
    vec3 tp = w.P(uc + side * (ec + ra * 0.8f), yT - 0.02f, 0.28f);
    Profile tassel = {{0.0f, -0.32f}, {0.045f, -0.31f}, {0.05f, -0.28f}, {0.042f, -0.18f}, {0.032f, -0.12f}, {0.03f, -0.11f},
                      {0.036f, -0.1f}, {0.03f, -0.07f}, {0.018f, -0.04f}, {0.012f, -0.01f}, {0.0f, 0.0f}};
    latheMod(gilt, tassel, 24, translate(tp), [](float r, float ang, float y) {
        float skirt = y < -0.12f ? 1.0f - 0.12f * std::fabs(std::sin(ang * 12.0f)) : 1.0f;
        return r * skirt;
    }, 2);
    rosette(gilt, w.P(uc + side * (eB(yT) + 0.02f), yT, 0.0f), w.N, 0.05f, 8, 0.03f);
}

void curtainRod(Accum& a, const WallDef& w, float uc) {
    MeshData& gilt = a[MaterialId::GildedTrim];
    const float y = 7.72f, n = 0.2f, half = 1.96f;
    // Rod along U: lathe around +Y then rotated so +Y -> U.
    vec3 U = w.U;
    quat q = fromTo(vec3(0, 1, 0), U);
    vec3 p0 = w.P(uc - half, y, n);
    lathe(gilt, {{0.0f, 0.0f}, {0.026f, 0.0f}, {0.026f, 0.0f}, {0.026f, 2.0f * half}, {0.026f, 2.0f * half}, {0.0f, 2.0f * half}}, 20,
          toMat4(q, p0));
    // Finials (ball + spike) at both ends.
    Profile fin = {{0.0f, 0.0f}, {0.04f, 0.0f}, {0.04f, 0.0f}, {0.04f, 0.02f}, {0.03f, 0.03f}, {0.045f, 0.06f}, {0.05f, 0.08f},
                   {0.045f, 0.1f}, {0.03f, 0.12f}, {0.018f, 0.13f}, {0.024f, 0.14f}, {0.01f, 0.17f}, {0.0f, 0.19f}};
    lathe(gilt, fin, 20, toMat4(q, w.P(uc + half, y, n)));
    lathe(gilt, fin, 20, toMat4(fromTo(vec3(0, 1, 0), -U), w.P(uc - half, y, n)));
    // Brackets into the wall.
    for (float s : {-1.0f, 1.0f}) {
        float ub = uc + s * (half - 0.15f);
        box(gilt, w.P(ub, y, n * 0.5f), w.U, vec3(0, 1, 0), w.N, vec3(0.018f, 0.018f, n * 0.5f));
        rosette(gilt, w.P(ub, y, 0.0f), w.N, 0.05f, 8, 0.02f);
    }
    // Rings.
    Profile ring;
    for (int i = 0; i <= 8; ++i) {
        float th = TAU * float(i) / 8.0f;
        ring.push_back({0.006f * std::cos(th), 0.006f * std::sin(th)});
    }
    for (float s : {-1.0f, 1.0f})
        for (int k = 0; k < 8; ++k) {
            float e = lerp(1.32f, 1.9f, float(k) / 7.0f);
            vec3 c = w.P(uc + s * e, y, n);
            std::vector<vec3> loop;
            for (int i = 0; i < 12; ++i) {
                float th = TAU * float(i) / 12.0f;
                loop.push_back(c + vec3(0, 0.036f * std::cos(th), 0) + w.N * (0.036f * std::sin(th)));
            }
            sweep(gilt, ring, loop, w.U, true);
        }
}

}  // namespace

void buildWindows(Accum& a, MeshData& glass) {
    const WallDef& w = wall(WALL_WINDOWS);
    for (int i = 0; i < layout::WINDOW_COUNT; ++i) {
        float uc = windowU(i);
        windowFrame(a, glass, w, uc);
        curtain(a, w, uc, -1.0f, 11u + uint32_t(i) * 7u);
        curtain(a, w, uc, 1.0f, 12u + uint32_t(i) * 7u);
        curtainRod(a, w, uc);
    }
}

}  // namespace detail
}  // namespace hall
