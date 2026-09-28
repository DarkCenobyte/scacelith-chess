// Coffered ceiling: main ribs on the pilaster axes, secondary ribs, 144 stepped coffers with
// gilded mouldings and rosettes, and the coved central plafond above the table (chandelier rose).
#include "hall_internal.h"

using namespace m;

namespace hall {
namespace detail {

namespace {

struct Span {
    float a, b;
    int coffers;  // 0 = plafond (not subdivided)
};
constexpr float BORDER = 0.30f, MAIN_RIB = 0.44f, SEC_RIB = 0.14f;

float edgeX() { return layout::HALL_MAX_X - CORNICE_PROJECTION; }
float edgeZ() { return layout::HALL_MAX_Z - CORNICE_PROJECTION; }

// Compartments along one axis: main ribs centred on the given axes.
std::vector<Span> spans(float edge, const std::vector<float>& ribAxes, const std::vector<int>& coffers) {
    std::vector<Span> r;
    float cur = -edge + BORDER;
    for (size_t i = 0; i <= ribAxes.size(); ++i) {
        float end = i < ribAxes.size() ? ribAxes[i] - MAIN_RIB * 0.5f : edge - BORDER;
        r.push_back({cur, end, coffers[i]});
        if (i < ribAxes.size()) cur = ribAxes[i] + MAIN_RIB * 0.5f;
    }
    return r;
}
std::vector<Span> spansX() { return spans(edgeX(), {-2.3f, 2.3f}, {3, 4, 3}); }
std::vector<Span> spansZ() { return spans(edgeZ(), {-7.8f, -2.6f, 2.6f, 7.8f}, {2, 4, 4, 4, 2}); }

std::vector<std::pair<float, float>> cofferIntervals(const std::vector<Span>& ss) {
    std::vector<std::pair<float, float>> r;
    for (const Span& s : ss) {
        int n = s.coffers;
        float cw = (s.b - s.a - SEC_RIB * float(n - 1)) / float(n);
        for (int k = 0; k < n; ++k) {
            float a = s.a + (cw + SEC_RIB) * float(k);
            r.push_back({a, a + cw});
        }
    }
    return r;
}

bool inPlafond(float x, float z) { return std::fabs(x) < 2.3f - MAIN_RIB * 0.5f && std::fabs(z) < 2.6f - MAIN_RIB * 0.5f; }

// Horizontal rectangle loop at height y with s pointing inwards (for W = +Y).
std::vector<vec3> rectLoop(float x0, float x1, float z0, float z1, float y) {
    return {vec3(x0, y, z1), vec3(x1, y, z1), vec3(x1, y, z0), vec3(x0, y, z0)};
}

void downQuad(MeshData& d, float x0, float x1, float z0, float z1, float y) {
    quadFlat(d, vec3(x0, y, z0), vec3(x1, y, z0), vec3(x1, y, z1), vec3(x0, y, z1), vec3(0, -1, 0), vec3(1, 0, 0));
}

Profile halfRound(float r, float a0 = 0.0f) {
    Profile p;
    for (int i = 0; i <= 6; ++i) {
        float th = PI * float(i) / 6.0f;
        p.push_back({r * std::cos(th), a0 + r * std::sin(th)});
    }
    return p;
}

}  // namespace

void ceilingGrid(std::vector<float>& xs, std::vector<float>& zs) {
    xs.clear();
    zs.clear();
    for (auto& iv : cofferIntervals(spansX())) { xs.push_back(iv.first); xs.push_back(iv.second); }
    for (auto& iv : cofferIntervals(spansZ())) { zs.push_back(iv.first); zs.push_back(iv.second); }
    std::sort(xs.begin(), xs.end());
    std::sort(zs.begin(), zs.end());
}

void buildCeiling(Accum& a) {
    MeshData& stone = a[MaterialId::WallStone];
    MeshData& gilt = a[MaterialId::GildedTrim];
    MeshData& paint = a[MaterialId::CeilingPainted];
    const float H = layout::HALL_HEIGHT;
    const float ex = edgeX(), ez = edgeZ();
    auto cx = cofferIntervals(spansX()), cz = cofferIntervals(spansZ());

    // ---- Soffit (ribs and border band): grid cells not covered by a coffer or the plafond.
    std::vector<float> xs, zs;
    ceilingGrid(xs, zs);
    xs.insert(xs.begin(), -ex);
    xs.push_back(ex);
    zs.insert(zs.begin(), -ez);
    zs.push_back(ez);
    auto inCoffer = [&](float x, float z) {
        if (inPlafond(x, z)) return true;
        bool ix = false, iz = false;
        for (auto& iv : cx) ix = ix || (x > iv.first && x < iv.second);
        for (auto& iv : cz) iz = iz || (z > iv.first && z < iv.second);
        return ix && iz;
    };
    for (size_t j = 0; j + 1 < zs.size(); ++j)
        for (size_t i = 0; i + 1 < xs.size(); ++i) {
            float mx = 0.5f * (xs[i] + xs[i + 1]), mz = 0.5f * (zs[j] + zs[j + 1]);
            if (!inCoffer(mx, mz)) downQuad(stone, xs[i], xs[i + 1], zs[j], zs[j + 1], H);
        }

    // ---- Coffers.
    const float CT = COFFER_TOP - H;  // recess depth
    Profile riser1 = {{0.0f, 0.0f}, {0.0f, 0.07f}};
    Profile ovolo = {{0.0f, 0.07f}, {0.018f, 0.07f}, {0.018f, 0.07f}, {0.03f, 0.074f}, {0.042f, 0.086f},
                     {0.052f, 0.104f}, {0.058f, 0.122f}, {0.06f, 0.13f}, {0.06f, 0.13f}, {0.07f, 0.13f}};
    Profile riser2 = {{0.07f, 0.13f}, {0.07f, CT - 0.02f}};
    Profile beadP = {{0.07f, CT - 0.02f}, {0.08f, CT - 0.019f}, {0.088f, CT - 0.014f}, {0.092f, CT - 0.006f}, {0.095f, CT - 0.002f}, {0.1f, CT}};
    for (auto& ix : cx)
        for (auto& iz : cz) {
            float mx = 0.5f * (ix.first + ix.second), mz = 0.5f * (iz.first + iz.second);
            if (inPlafond(mx, mz)) continue;
            std::vector<vec3> loop = rectLoop(ix.first, ix.second, iz.first, iz.second, H);
            sweep(stone, riser1, loop, vec3(0, 1, 0), true);
            sweep(gilt, ovolo, loop, vec3(0, 1, 0), true);
            sweep(stone, riser2, loop, vec3(0, 1, 0), true);
            sweep(gilt, beadP, loop, vec3(0, 1, 0), true);
            downQuad(paint, ix.first + 0.1f, ix.second - 0.1f, iz.first + 0.1f, iz.second - 0.1f, COFFER_TOP);
            float r = 0.16f * std::min(ix.second - ix.first, iz.second - iz.first);
            rosette(gilt, vec3(mx, COFFER_TOP, mz), vec3(0, -1, 0), r, 8, 0.055f);
        }

    // ---- Gilded beads framing every compartment on the rib soffits.
    for (const Span& sx : spansX())
        for (const Span& sz : spansZ()) {
            const float m = 0.05f;
            std::vector<vec3> loop = rectLoop(sx.a - m, sx.b + m, sz.a - m, sz.b + m, H);
            sweep(gilt, halfRound(0.012f), loop, vec3(0, -1, 0), true);
        }
    {
        std::vector<vec3> loop = rectLoop(-ex + 0.12f, ex - 0.12f, -ez + 0.12f, ez - 0.12f, H);
        sweep(gilt, halfRound(0.015f), loop, vec3(0, -1, 0), true);
    }

    // ---- Central plafond: coved recess with a painted panel and an oval gilded frame.
    {
        float px = 2.3f - MAIN_RIB * 0.5f, pz = 2.6f - MAIN_RIB * 0.5f;
        const float PT = PLAFOND_TOP - H;  // 0.36
        std::vector<vec3> loop = rectLoop(-px, px, -pz, pz, H);
        sweep(stone, {{0.0f, 0.0f}, {0.0f, 0.06f}}, loop, vec3(0, 1, 0), true);
        Profile mould = {{0.0f, 0.06f},  {0.03f, 0.06f},  {0.03f, 0.06f},  {0.045f, 0.066f}, {0.062f, 0.082f}, {0.075f, 0.104f},
                         {0.08f, 0.12f}, {0.08f, 0.12f},  {0.1f, 0.12f},   {0.1f, 0.12f},    {0.1f, 0.14f},    {0.112f, 0.148f},
                         {0.125f, 0.16f}, {0.14f, 0.16f}};
        sweep(gilt, mould, loop, vec3(0, 1, 0), true);
        Profile cove;
        for (int i = 0; i <= 10; ++i) {
            float th = PI - 0.5f * PI * float(i) / 10.0f;
            cove.push_back({0.32f + 0.18f * std::cos(th), 0.16f + 0.18f * std::sin(th)});
        }
        sweep(paint, cove, loop, vec3(0, 1, 0), true);
        sweep(gilt, {{0.32f, 0.34f}, {0.335f, 0.342f}, {0.345f, 0.35f}, {0.35f, PT}}, loop, vec3(0, 1, 0), true);
        downQuad(paint, -px + 0.35f, px - 0.35f, -pz + 0.35f, pz - 0.35f, PLAFOND_TOP);
        // Oval frame.
        std::vector<vec3> oval;
        const int NO = 72;
        float ax = px - 0.62f, az = pz - 0.55f;
        for (int i = 0; i < NO; ++i) {
            float t = TAU * float(i) / float(NO);
            oval.push_back(vec3(ax * std::cos(t), PLAFOND_TOP, az * std::sin(t)));
        }
        Profile ovp = {{0.055f, 0.0f}, {0.05f, 0.016f}, {0.035f, 0.03f}, {0.012f, 0.042f}, {-0.01f, 0.044f}, {-0.03f, 0.036f},
                       {-0.045f, 0.022f}, {-0.052f, 0.01f}, {-0.055f, 0.0f}};
        sweep(gilt, ovp, oval, vec3(0, -1, 0), true);
        // Corner fans (small rosettes) in the spandrels and the chandelier rose.
        for (float sx : {-1.0f, 1.0f})
            for (float sz : {-1.0f, 1.0f}) rosette(gilt, vec3(sx * (px - 0.62f), PLAFOND_TOP, sz * (pz - 0.62f)), vec3(0, -1, 0), 0.13f, 8, 0.05f);
        rosette(gilt, vec3(0, PLAFOND_TOP, 0), vec3(0, -1, 0), 0.45f, 12, 0.12f);
    }
}

}  // namespace detail
}  // namespace hall
