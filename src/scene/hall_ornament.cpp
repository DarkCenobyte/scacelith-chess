// Shared ornaments of the hall: Corinthian capital relief, rosettes, picture frames, raised
// panels, urns.
#include "hall_internal.h"
#include <cstring>

using namespace m;

namespace hall {
namespace detail {

namespace {

float sstep(float a, float b, float x) { return smoothstep(a, b, x); }

// One acanthus leaf as a height field. (x, v) relative to the leaf base centre, w = half width,
// ht = height, proj = tip projection. Returns the height (0 outside).
float acanthus(float x, float v, float w, float ht, float proj) {
    float lv = v / ht;
    if (lv <= 0.0f || lv >= 1.0f) return 0.0f;
    float lx = x / w;
    // Outline: broad lobed blade with a pointed, slightly drooping tip.
    float shape = std::pow(std::sin(PI * std::pow(lv, 0.75f)), 0.7f) * (1.0f - 0.25f * lv);
    float lobes = 1.0f + 0.16f * std::sin(lv * 4.5f * TAU + 0.6f) - 0.08f * std::fabs(std::sin(lv * 9.0f * TAU));
    float half = shape * lobes;
    float ax = std::fabs(lx);
    if (ax >= half) return 0.0f;
    float edge = sstep(half, half * 0.8f, ax);
    // Body: bends outwards towards the top; the tip curls forward.
    float body = 0.25f + 0.75f * std::pow(lv, 1.6f);
    float curl = sstep(0.75f, 0.95f, lv) * 0.25f;
    // Midrib, radiating grooves between the lobes, slight cupping of the blade.
    float rib = 0.18f * std::exp(-lx * lx * 60.0f);
    float groove = 0.12f * std::cos(std::atan2(lx * 1.3f, lv + 0.15f) * 9.0f) * lv;
    float cup = -0.12f * (1.0f - ax / std::max(half, 1e-3f)) * (1.0f - lv);
    return proj * edge * (body + curl + rib + groove + cup);
}

// Spiral volute seen face on: centre (0,0), radius R, projection pj.
float volute(float x, float y, float R, float pj) {
    float r = std::sqrt(x * x + y * y);
    if (r >= R) return 0.0f;
    float th = std::atan2(y, x);
    float turns = 2.6f;
    float phase = (r / R) * turns + th / TAU;
    float band = std::fabs(fract(phase) - 0.5f) * 2.0f;  // 0 on the spiral ridge line
    float ridge = 1.0f - sstep(0.15f, 0.55f, band);
    float falloff = sstep(R, R * 0.8f, r);
    float eye = sstep(R * 0.22f, 0.0f, r);
    return pj * falloff * (0.55f + 0.35f * ridge + 0.3f * eye) * (0.8f + 0.2f * (1.0f - r / R));
}

}  // namespace

void corinthianCapital(MeshData& d, const WallDef& w, float u, float y0, float y1, float hw, float p, CapitalCache* cache) {
    const float abacusH = 0.085f;
    const float bellTop = y1 - abacusH;
    const float H = bellTop - y0;
    const float flare = 0.055f, rc = 0.03f;
    auto dims = [&](float t, float& hwT, float& pT) {
        float f = std::pow(t, 1.4f) * flare;
        hwT = hw + f;
        pT = p + f;
    };
    // Outline position / normal for an unrolled coordinate X at height fraction t.
    auto outline = [&](float X, float t, vec2& pos, vec2& nrm, float& Xc) {
        float hwT, pT;
        dims(t, hwT, pT);
        float Lf = hwT - rc, La = 0.5f * PI * rc, Ls = pT - rc;
        Xc = Lf + 0.5f * La;
        float s = std::fabs(X), sg = X < 0 ? -1.0f : 1.0f;
        if (s <= Lf) {
            pos = vec2(sg * s, pT);
            nrm = vec2(0, 1);
        } else if (s <= Lf + La) {
            float a = (s - Lf) / rc;  // angle from the front
            vec2 c(sg * Lf, pT - rc);
            nrm = vec2(sg * std::sin(a), std::cos(a));
            pos = c + nrm * rc;
        } else {
            float e = std::min(s - Lf - La, Ls);
            pos = vec2(sg * hwT, pT - rc - e);
            nrm = vec2(sg, 0);
        }
    };
    auto halfLen = [&](float t) {
        float hwT, pT;
        dims(t, hwT, pT);
        return (hwT - rc) + 0.5f * PI * rc + (pT - rc);
    };
    auto relief = [&](float X, float v, float Xc, float sideEnd) {
        float h = 0.0f;
        // Lower tier: three leaves (front centre + the two corners).
        float lw = 0.15f, lh = 0.46f * H;
        h = std::max(h, acanthus(X, v, lw, lh, 0.055f));
        h = std::max(h, acanthus(X - Xc, v, lw, lh, 0.06f));
        h = std::max(h, acanthus(X + Xc, v, lw, lh, 0.06f));
        // Upper tier: two leaves between, rising higher.
        float uw = 0.14f, uh = 0.70f * H, ub = 0.18f * H;
        h = std::max(h, acanthus(X - Xc * 0.5f, v - ub, uw, uh, 0.05f) * 0.95f);
        h = std::max(h, acanthus(X + Xc * 0.5f, v - ub, uw, uh, 0.05f) * 0.95f);
        // Corner volutes under the abacus corners and small inner helices at the centre.
        float vy = v - 0.80f * H;
        h = std::max(h, volute(X - Xc, vy, 0.075f, 0.07f));
        h = std::max(h, volute(X + Xc, vy, 0.075f, 0.07f));
        float hy = v - 0.84f * H;
        h = std::max(h, volute(X - 0.075f, hy, 0.045f, 0.035f));
        h = std::max(h, volute(X + 0.075f, hy, 0.045f, 0.035f));
        // Stems (caulicoli) rising from the upper leaves to the volutes.
        for (float sg : {-1.0f, 1.0f}) {
            float cx = sg * Xc * 0.72f, cy = 0.62f * H;
            float dx = X - cx - sg * (v - cy) * 0.35f;
            if (v > cy && v < 0.8f * H) h = std::max(h, 0.03f * sstep(0.02f, 0.0f, std::fabs(dx)));
        }
        // Fade out where the capital meets the wall.
        float toWall = sideEnd - std::fabs(X);
        return h * sstep(0.0f, 0.03f, toWall);
    };
    const int NS = 76, NV = 46;
    auto bell = [&](float s, float t) {
        float hl = halfLen(t);
        float X = (s * 2.0f - 1.0f) * hl;
        vec2 pos, nrm;
        float Xc;
        outline(X, t, pos, nrm, Xc);
        float h = relief(X, t * H, Xc, hl);
        return pos + nrm * h;
    };
    if (cache && (cache->y0 != y0 || cache->y1 != y1 || cache->hw != hw || cache->p != p)) {
        cache->bell.clear();
        cache->y0 = y0; cache->y1 = y1; cache->hw = hw; cache->p = p;
    }
    auto fn = [&](float s, float tv) {
        float t = tv;
        vec2 q;
        if (cache) {
            uint32_t bs, bt;
            std::memcpy(&bs, &s, sizeof(bs));
            std::memcpy(&bt, &tv, sizeof(bt));
            auto [it, inserted] = cache->bell.try_emplace((uint64_t(bs) << 32) | bt);
            if (inserted) it->second = bell(s, t);
            q = it->second;
        } else {
            q = bell(s, t);
        }
        return w.P(u + q.x, y0 + t * H, q.y);
    };
    surface(d, NS, NV, fn, false, UVMode::Meters);
    // Abacus: moulded slab with a concave face, then a fleuron at the centre.
    {
        float hwT, pT;
        dims(1.0f, hwT, pT);
        std::vector<vec3> path = {w.P(u + hwT, 0, 0), w.P(u + hwT, 0, pT), w.P(u - hwT, 0, pT), w.P(u - hwT, 0, 0)};
        Profile pr = {{0.0f, bellTop}, {0.012f, bellTop + 0.004f}, {0.02f, bellTop + 0.015f}, {0.022f, bellTop + 0.03f},
                      {0.022f, bellTop + 0.03f}, {0.03f, bellTop + 0.035f}, {0.03f, bellTop + 0.035f}, {0.03f, bellTop + 0.06f},
                      {0.045f, bellTop + 0.07f}, {0.055f, bellTop + 0.08f}, {0.055f, y1}, {0.055f, y1}, {0.0f, y1}};
        sweep(d, pr, path, vec3(0, 1, 0), false, 30.0f);
        rosette(d, w.P(u, bellTop + 0.04f, pT + 0.03f), w.N, 0.045f, 6, 0.03f);
    }
}

void rosette(MeshData& d, vec3 c, vec3 n, float r, int petals, float depth) {
    n = normalize(n);
    vec3 t = orthogonal(n);
    vec3 b = cross(t, n);
    mat4 xf(mat3(t, n, b), c);
    Profile pr = {{r, 0.0f},           {r * 0.97f, depth * 0.2f}, {r * 0.85f, depth * 0.45f}, {r * 0.65f, depth * 0.55f},
                  {r * 0.5f, depth * 0.5f}, {r * 0.38f, depth * 0.62f}, {r * 0.25f, depth * 0.85f}, {r * 0.12f, depth},
                  {0.0f, depth * 1.02f}};
    float pf = float(petals);
    latheMod(d, pr, petals * 6, xf, [pf](float rr, float a, float) {
        float lobe = std::pow(std::fabs(std::cos(a * pf * 0.5f)), 0.6f);
        return rr * (0.72f + 0.28f * lobe);
    }, 1);
}

void framedPainting(Accum& a, MeshData& canvas, const WallDef& w, float u, float v, float cw, float ch, float fw, float n0) {
    float u0 = u - cw * 0.5f - fw, u1 = u + cw * 0.5f + fw, v0 = v - ch * 0.5f - fw, v1 = v + ch * 0.5f + fw;
    std::vector<vec3> path = {w.P(u0, v0, n0), w.P(u1, v0, n0), w.P(u1, v1, n0), w.P(u0, v1, n0)};
    float f = fw;
    Profile pr = {{f, 0.0f},           {f, 0.012f},           {f, 0.012f},          {f - 0.012f, 0.02f},  {f * 0.8f, 0.032f},
                  {f * 0.72f, 0.03f},  {f * 0.62f, 0.042f},   {f * 0.5f, 0.058f},   {f * 0.38f, 0.068f},  {f * 0.26f, 0.07f},
                  {f * 0.16f, 0.064f}, {f * 0.1f, 0.055f},    {f * 0.06f, 0.058f},  {0.02f, 0.052f},      {0.0f, 0.04f},
                  {0.0f, 0.04f},       {0.0f, 0.0f}};
    sweep(a[MaterialId::GildedTrim], pr, path, w.N, true, 30.0f);
    // Canvas, slightly recessed inside the sight edge; uv [0,1].
    float cu0 = u - cw * 0.5f, cu1 = u + cw * 0.5f, cv0 = v - ch * 0.5f, cv1 = v + ch * 0.5f;
    float cn = n0 + 0.008f;
    uint32_t i0 = vtx(canvas, w.P(cu0, cv0, cn), w.N, w.U, vec2(0, 0));
    uint32_t i1 = vtx(canvas, w.P(cu1, cv0, cn), w.N, w.U, vec2(1, 0));
    uint32_t i2 = vtx(canvas, w.P(cu1, cv1, cn), w.N, w.U, vec2(1, 1));
    uint32_t i3 = vtx(canvas, w.P(cu0, cv1, cn), w.N, w.U, vec2(0, 1));
    quad(canvas, i0, i1, i2, i3);
}

void raisedPanel(MeshData& wood, MeshData& bead, const WallDef& w, float u0, float u1, float v0, float v1, float n0, float bevel,
                 float raise) {
    vec3 o[4] = {w.P(u0, v0, n0), w.P(u1, v0, n0), w.P(u1, v1, n0), w.P(u0, v1, n0)};
    vec3 in[4] = {w.P(u0 + bevel, v0 + bevel, n0 + raise), w.P(u1 - bevel, v0 + bevel, n0 + raise), w.P(u1 - bevel, v1 - bevel, n0 + raise),
                  w.P(u0 + bevel, v1 - bevel, n0 + raise)};
    quadFlat(wood, in[0], in[1], in[2], in[3], w.N, w.U);
    for (int k = 0; k < 4; ++k) {
        int k1 = (k + 1) % 4;
        vec3 nn = normalize(cross(o[k1] - o[k], in[k] - o[k]));
        if (dot(nn, w.N) < 0) nn = -nn;
        quadFlat(wood, o[k], o[k1], in[k1], in[k], nn, normalize(o[k1] - o[k]));
    }
    // Small half-round bead framing the panel.
    Profile pr;
    const float r = 0.007f;
    for (int i = 0; i <= 6; ++i) {
        float th = PI * float(i) / 6.0f;
        pr.push_back({r * std::cos(th) - r, r * std::sin(th)});
    }
    std::vector<vec3> path = {w.P(u0, v0, n0), w.P(u1, v0, n0), w.P(u1, v1, n0), w.P(u0, v1, n0)};
    sweep(bead, pr, path, w.N, true, 30.0f);
}

void urn(MeshData& d, vec3 p, float h) {
    Profile pr = {{0.0f, 0.0f},   {0.17f, 0.0f},  {0.17f, 0.0f},  {0.17f, 0.045f}, {0.14f, 0.06f},  {0.1f, 0.09f},
                  {0.075f, 0.15f}, {0.1f, 0.19f},  {0.18f, 0.25f}, {0.24f, 0.35f},  {0.26f, 0.46f},  {0.24f, 0.56f},
                  {0.18f, 0.64f}, {0.13f, 0.68f}, {0.16f, 0.7f},  {0.17f, 0.73f},  {0.13f, 0.76f},  {0.1f, 0.81f},
                  {0.06f, 0.87f}, {0.035f, 0.9f}, {0.055f, 0.94f}, {0.03f, 0.99f},  {0.0f, 1.0f}};
    for (auto& q : pr) q = q * h;
    // Gadroons on the lower belly of the body.
    latheMod(d, pr, 32, translate(p), [h](float r, float a, float y) {
        float w = smoothstep(0.17f * h, 0.24f * h, y) * smoothstep(0.44f * h, 0.36f * h, y);
        return r * (1.0f - 0.06f * w * (1.0f - std::fabs(std::cos(a * 8.0f))));
    }, 1);
}

}  // namespace detail
}  // namespace hall
