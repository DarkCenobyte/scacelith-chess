// Lever chess clock geometry. Authored in millimetres (clock-local, see clock_model.h) and
// converted to meters at the end.
#include "clock_model.h"
#include "piece_profile.h"
#include "../core/log.h"
#include <algorithm>
#include <cmath>

using namespace m;

namespace {

// Case cross-section (XY, counter-clockwise): symmetric trapezoid, front face leaning back ~20deg.
constexpr float CASE_Y0 = 2.0f, CASE_Y1 = 47.0f;
constexpr float CASE_X0 = 42.5f, CASE_X1 = 26.0f;  // half depth at the bottom / top
constexpr float CASE_HALF_LEN = 95.0f;
constexpr float LEVER_Y = 51.5f;     // underside of the lever at the pivot
constexpr float LEVER_HALF = 74.0f;
constexpr float LEVER_THICK = 7.0f;
constexpr float BUMPER_Z = 66.0f, BUMPER_TOP = CASE_Y1 + 1.2f;

// Frame of the slanted front face: origin at the middle of the face, U = +Z, V = up the slant,
// N = outward.
struct FaceFrame {
    vec3 o, U, V, N;
    vec3 at(float u, float v, float n) const { return o + U * u + V * v + N * n; }
    // Right-handed placement matrix: local X -> U, Y -> N, Z -> -V.
    mat4 place(float u, float v, float n) const { return mat4(vec4(U, 0), vec4(N, 0), vec4(-V, 0), vec4(at(u, v, n), 1)); }
};

FaceFrame frontFace() {
    FaceFrame f;
    vec2 a(-CASE_X0, CASE_Y0), b(-CASE_X1, CASE_Y1);
    vec2 v = normalize(b - a);
    f.o = vec3((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, 0);
    f.U = vec3(0, 0, 1);
    f.V = vec3(v.x, v.y, 0);
    f.N = vec3(-v.y, v.x, 0);
    return f;
}

constexpr float PANEL_HALF_U = 89.0f, PANEL_HALF_V = 15.5f, PANEL_N0 = -0.6f, PANEL_N1 = 1.3f;
constexpr float LCD_U = 50.0f, LCD_V = 2.5f;

MeshData makeCase() {
    std::vector<vec2> poly = {{-CASE_X0, CASE_Y0}, {CASE_X0, CASE_Y0}, {CASE_X1, CASE_Y1}, {-CASE_X1, CASE_Y1}};
    return roundedPrism(poly, 5.0f, CASE_HALF_LEN, 4.0f, 10, 8, 6);
}

MeshData makePanel(const FaceFrame& f) {
    // Thin rounded slab in (N, V), extruded along U.
    std::vector<vec2> poly = {{PANEL_N0, -PANEL_HALF_V}, {PANEL_N1, -PANEL_HALF_V}, {PANEL_N1, PANEL_HALF_V}, {PANEL_N0, PANEL_HALF_V}};
    MeshData slab = roundedPrism(poly, 0.8f, PANEL_HALF_U, 0.8f, 5, 5, 4);
    // prism x -> N, y -> V, z -> U (right-handed: N x V = -U, so map z to -U).
    mat4 m(vec4(f.N, 0), vec4(f.V, 0), vec4(-f.U, 0), vec4(f.o, 1));
    MeshData out;
    out.append(slab, m);
    return out;
}

MeshData makeBezels(const FaceFrame& f) {
    const float hw = CLOCK_LCD_W * 500.0f, hh = CLOCK_LCD_H * 500.0f, w = 1.6f;
    Profile p;
    p.to(w, 0.12f);
    p.to(w - 0.15f, 0.5f, 0.15f);
    p.to(w - 0.6f, 0.62f, 0.3f);
    p.to(0.35f, 0.55f, 0.3f);
    p.to(0.0f, 0.15f, 0.25f);
    p.to(0.0f, -0.4f);
    std::vector<vec2> prof = finishProfile(p, 0.2f, 1.0f);
    MeshData out;
    for (int s = 0; s < 2; ++s) {
        MeshData ring = sweepRoundedRect(prof, hw + w, hh + w, 2.0f, 6);
        out.append(ring, f.place(s == 0 ? -LCD_U : LCD_U, LCD_V, PANEL_N1));
    }
    return out;
}

MeshData makeDisplays(const FaceFrame& f, vec3 centers[2]) {
    const float hw = CLOCK_LCD_W * 500.0f, hh = CLOCK_LCD_H * 500.0f, n = PANEL_N1 + 0.12f;
    MeshData d;
    for (int s = 0; s < 2; ++s) {
        float cu = s == 0 ? -LCD_U : LCD_U;
        centers[s] = f.at(cu, LCD_V, n);
        uint32_t base = uint32_t(d.vertices.size());
        const float us[4] = {-1, 1, 1, -1}, vs[4] = {-1, -1, 1, 1};
        for (int k = 0; k < 4; ++k) {
            Vertex v;
            v.pos = f.at(cu + us[k] * hw, LCD_V + vs[k] * hh, n);
            v.normal = f.N;
            v.tangent = vec4(f.U, 1);
            v.uv = vec2(float(2 * s) + (us[k] * 0.5f + 0.5f), vs[k] * 0.5f + 0.5f);
            d.vertices.push_back(v);
        }
        for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) d.indices.push_back(base + k);
    }
    fixWinding(d);
    return d;
}

// Push button: bezel ring + domed cap, axis along local +Y.
MeshData button(float diameter) {
    float r = diameter * 0.5f;
    Profile p;
    p.to(0, 0.9f + r * 0.08f + 0.9f);
    p.cubic(vec2(r * 0.55f, 1.8f + r * 0.08f), vec2(r * 0.95f, 1.6f), vec2(r, 1.2f));
    p.to(r, 0.45f, 0.25f);
    p.to(r + 0.35f, 0.45f, 0.0f);
    p.to(r + 0.6f, 0.62f, 0.2f);
    p.to(r + 1.2f, 0.55f, 0.25f);
    p.to(r + 1.45f, 0.1f, 0.2f);
    p.to(r + 1.5f, -0.5f);
    // Lathe expects bottom -> top; our profile is top -> bottom, reverse it.
    std::vector<vec2> prof = finishProfile(p, 0.2f, 0.4f);
    std::reverse(prof.begin(), prof.end());
    return latheProfile(prof, 48, 10.0f);
}

MeshData makeButtons(const FaceFrame& f) {
    MeshData d;
    struct B { float u, v, dia; } bs[] = {{-11.0f, 1.5f, 5.6f}, {0.0f, 1.5f, 7.6f}, {11.0f, 1.5f, 5.6f}, {0.0f, -10.0f, 4.2f}};
    for (auto& b : bs) d.append(button(b.dia), f.place(b.u, b.v, PANEL_N1));
    return d;
}

MeshData makeFeetAndHardware() {
    MeshData d;
    // Rubber feet.
    Profile foot;
    foot.to(0, 0).to(5.0f, 0, 0.8f).to(5.0f, CASE_Y0 + 0.6f, 0.0f).to(0, CASE_Y0 + 0.6f);
    MeshData fm = latheProfile(finishProfile(foot, 0.5f, 1.0f), 32, 5.0f);
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2) d.append(fm, translate(vec3(float(sx) * 31.0f, 0, float(sz) * 80.0f)));
    // Pivot housing under the lever centre.
    std::vector<vec2> hp = {{-9.5f, CASE_Y1 - 1.0f}, {9.5f, CASE_Y1 - 1.0f}, {8.0f, LEVER_Y - 0.3f}, {-8.0f, LEVER_Y - 0.3f}};
    d.append(roundedPrism(hp, 1.2f, 12.0f, 1.2f, 5, 4, 2));
    // Rubber bumpers under the lever ends.
    Profile bump;
    bump.to(0, CASE_Y1 - 0.5f).to(3.6f, CASE_Y1 - 0.5f, 0.0f).to(3.6f, BUMPER_TOP - 0.4f, 0.4f).to(3.0f, BUMPER_TOP, 0.4f).to(0, BUMPER_TOP);
    MeshData bm = latheProfile(finishProfile(bump, 0.3f, 1.0f), 32, 5.0f);
    for (int s = -1; s <= 1; s += 2) d.append(bm, translate(vec3(0, 0, float(s) * BUMPER_Z)));
    return d;
}

MeshData makeLever() {
    const float y = LEVER_Y, t = LEVER_THICK;
    std::vector<vec2> poly = {{-10.0f, y}, {10.0f, y}, {10.0f, y + t - 2.4f}, {6.5f, y + t}, {-6.5f, y + t}, {-10.0f, y + t - 2.4f}};
    return roundedPrism(poly, 1.8f, LEVER_HALF, 1.8f, 6, 6, 12);
}

void toMeters(MeshData& d) { d.transform(scale(vec3(0.001f))); }

}  // namespace

ClockModel buildClock() {
    ClockModel c;
    FaceFrame f = frontFace();
    auto part = [&](Model& m, const char* name, MeshData mesh, MaterialId mat) {
        toMeters(mesh);
        ModelPart p;
        p.name = name;
        p.mesh = std::move(mesh);
        p.material = mat;
        m.parts.push_back(std::move(p));
    };
    part(c.body, "case", makeCase(), MaterialId::ClockCase);
    MeshData panel = makePanel(f);
    panel.append(makeBezels(f));
    panel.append(makeFeetAndHardware());
    part(c.body, "panel", std::move(panel), MaterialId::ClockPanel);
    vec3 centers[2];
    part(c.body, "displays", makeDisplays(f, centers), MaterialId::ClockDisplay);
    c.body.parts.back().inst[0] = clockDisplayState(300000, 300000, 0, -1);
    part(c.body, "buttons", makeButtons(f), MaterialId::ClockLever);
    part(c.lever, "lever", makeLever(), MaterialId::ClockLever);

    c.leverPivot = vec3(0, LEVER_Y, 0) * 0.001f;
    c.leverAxis = vec3(1, 0, 0);
    c.leverMaxAngle = std::asin((LEVER_Y - BUMPER_TOP) / BUMPER_Z);
    for (int i = 0; i < 2; ++i) {
        c.pressPoint[i] = vec3(0, LEVER_Y + LEVER_THICK, (i == 0 ? -1.0f : 1.0f) * 58.0f) * 0.001f;
        c.displayCenter[i] = centers[i] * 0.001f;
    }
    c.displayNormal = f.N;
    size_t tris = 0;
    for (auto& p : c.body.parts) tris += p.mesh.indices.size() / 3;
    for (auto& p : c.lever.parts) tris += p.mesh.indices.size() / 3;
    LOGI("clock: %d triangles, lever max angle %.2f deg", int(tris), double(c.leverMaxAngle / DEG));
    return c;
}
