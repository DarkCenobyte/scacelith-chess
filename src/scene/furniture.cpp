// Chess table and players' chairs (Louis XVI): procedural, generated at startup.
#include "furniture.h"
#include "hall_geom.h"
#include "../render/renderer.h"

using namespace m;
using namespace hallgeo;

namespace furniture {

namespace {

const uint32_t STATIC_FLAGS = render::DRAW_STATIC | render::DRAW_CAST_SHADOW;

void addPart(Model& m, const char* name, MeshData&& d, MaterialId mat, uint32_t flags) {
    if (d.indices.empty()) return;
    ModelPart p;
    p.name = name;
    p.material = mat;
    p.flags = flags;
    p.mesh = std::move(d);
    m.parts.push_back(std::move(p));
}

// Closed rounded-rectangle loop in the plane y, traversed so that sweep sides point outwards
// (W = +Y). Half extents hx, hz, corner radius r.
std::vector<vec3> roundedRect(float hx, float hz, float r, float y, int seg) {
    std::vector<vec3> p;
    const vec2 c[4] = {{hx - r, -(hz - r)}, {hx - r, hz - r}, {-(hx - r), hz - r}, {-(hx - r), -(hz - r)}};
    for (int k = 0; k < 4; ++k) {
        float a0 = -0.5f * PI + 0.5f * PI * float(k);
        for (int i = 0; i <= seg; ++i) {
            float a = a0 + 0.5f * PI * float(i) / float(seg);
            p.push_back(vec3(c[k].x + r * std::cos(a), y, c[k].y + r * std::sin(a)));
        }
    }
    return p;
}

// Flat horizontal polygon with uv = world (x, z) (bitangent +Z).
void flatXZ(MeshData& d, const std::vector<vec3>& pts, bool up) {
    uint32_t base = uint32_t(d.vertices.size());
    vec3 c(0);
    for (vec3 p : pts) c += p;
    c /= float(pts.size());
    vec3 n = up ? vec3(0, 1, 0) : vec3(0, -1, 0);
    uint32_t ic = vtx(d, c, n, vec3(1, 0, 0), vec2(c.x, c.z));
    if (up) d.vertices.back().tangent.w = -1.0f;
    for (vec3 p : pts) {
        vtx(d, p, n, vec3(1, 0, 0), vec2(p.x, p.z));
        if (up) d.vertices.back().tangent.w = -1.0f;
    }
    uint32_t n0 = uint32_t(pts.size());
    for (uint32_t i = 0; i < n0; ++i) tri(d, ic, base + 1 + i, base + 1 + (i + 1) % n0);
}

// Carved apron face: flat band with vertical flutes and a bead along the bottom edge.
// The face lies in the plane through 'o' spanned by 'along' (length len) and +Y (height h),
// facing 'out', and stands 2.5 mm (plus the bead) proud of that plane. Its top edge meets the
// underside of the table top; its ends and its bottom are closed down to 1 mm behind the plane.
void carvedApron(MeshData& d, vec3 o, vec3 along, vec3 out, float len, float h) {
    int nu = std::max(8, int(len / 0.004f));
    int nv = 14;
    auto fn = [&](float s, float t) {
        float x = s * len, y = t * h;
        float relief = 0.0f;
        // Bead along the bottom edge.
        float by = 0.012f;
        if (y < 2.0f * by) relief = std::max(relief, 0.006f * std::sqrt(std::max(0.0f, 1.0f - sq((y - by) / by))));
        // Fluted field between the bead and the top fillet, ending in rounded flute heads.
        float fy0 = 2.4f * by, fy1 = h - 0.012f;
        if (y > fy0 && y < fy1 && x > 0.03f && x < len - 0.03f) {
            float pitch = 0.018f;
            float fx = std::fmod(x - 0.03f, pitch) / pitch;  // 0..1
            float groove = std::sin(PI * clamp((fx - 0.25f) / 0.5f, 0.0f, 1.0f));
            float ends = smoothstep(fy0, fy0 + 0.006f, y) * smoothstep(fy1, fy1 - 0.006f, y);
            relief -= 0.0025f * groove * ends;
        }
        return o + along * x + vec3(0, y, 0) + out * (relief + 0.0025f);
    };
    surfaceFacing(d, nu, nv, fn, out, UVMode::Meters);
    // Returns at both ends and along the bottom, built from the grid's own boundary samples so they
    // share its edges exactly. They run 1 mm into the leg block or the backing box behind the face,
    // so the joints overlap: a single sheet would leave its ends and bottom open, and a grazing view
    // past them would look into the apron (whose inner faces are culled) and out under the table.
    const std::vector<float> us = linspace(0, 1, nu), vs = linspace(0, 1, nv);
    auto back = [&](vec3 p) { return p - out * (dot(p - o, out) + 0.001f); };
    for (size_t j = 0; j + 1 < vs.size(); ++j) {
        vec3 a = fn(us.front(), vs[j]), b = fn(us.front(), vs[j + 1]);
        quadFlat(d, a, b, back(b), back(a), -along, out);
        a = fn(us.back(), vs[j]);
        b = fn(us.back(), vs[j + 1]);
        quadFlat(d, a, b, back(b), back(a), along, out);
    }
    for (size_t i = 0; i + 1 < us.size(); ++i) {
        vec3 a = fn(us[i], vs.front()), b = fn(us[i + 1], vs.front());
        quadFlat(d, a, b, back(b), back(a), vec3(0, -1, 0), along);
    }
}

}  // namespace

Model buildTable() {
    MeshData top, carved, gilt;
    const float Y = layout::TABLE_TOP_Y, T = layout::TABLE_TOP_THICKNESS;
    const float hx = layout::TABLE_WIDTH * 0.5f, hz = layout::TABLE_DEPTH * 0.5f, rc = 0.05f;
    // ---- Top: moulded edge sweep + flat top (exactly at TABLE_TOP_Y) + underside. The underside
    // starts less deep inside the outline than the corner radius: at an inset equal to the radius
    // the mitred corner arcs would turn inside out into small bow ties.
    std::vector<vec3> outline = roundedRect(hx, hz, rc, 0.0f, 10);
    const float underInset = 0.045f;
    Profile edge = {{-underInset, Y - T},     {-0.012f, Y - T},         {-0.008f, Y - T + 0.002f}, {-0.004f, Y - T + 0.006f},
                    {-0.001f, Y - T + 0.011f}, {0.0f, Y - T + 0.016f},  {0.0f, Y - 0.018f},         {-0.001f, Y - 0.012f},
                    {-0.003f, Y - 0.007f},     {-0.006f, Y - 0.0032f}, {-0.01f, Y - 0.001f},      {-0.016f, Y},
                    {-0.03f, Y}};
    sweep(top, edge, outline, vec3(0, 1, 0), true, 30.0f);
    std::vector<vec3> inner = offsetPath(outline, vec3(0, 1, 0), -0.03f, true);
    for (auto& p : inner) p.y = Y;
    flatXZ(top, inner, true);
    std::vector<vec3> under = offsetPath(outline, vec3(0, 1, 0), -underInset, true);
    for (auto& p : under) p.y = Y - T;
    flatXZ(carved, under, false);

    // ---- Aprons: long (players') sides shallow for the knees, short sides deeper.
    const float ax = hx - 0.055f, az = furniture::TABLE_APRON_Z, th = 0.022f;
    const float lx = TABLE_LEG_X, lz = TABLE_LEG_Z, bh = 0.032f;
    const float yLong = TABLE_KNEE_APRON_BOTTOM, yShort = Y - T - 0.105f, yTopA = Y - T;
    for (float sz : {-1.0f, 1.0f}) {
        vec3 o(-lx + bh, yLong, sz * az);
        float len = 2.0f * (lx - bh);
        // Box behind the carved face (top against the table, back and bottom).
        boxAA(carved, vec3(-lx + bh, yLong, sz > 0 ? az - th : -az + 0.0005f), vec3(lx - bh, yTopA, sz > 0 ? az - 0.0005f : -az + th),
              F_NY | (sz > 0 ? F_NZ : F_PZ));
        if (sz > 0) carvedApron(carved, o, vec3(1, 0, 0), vec3(0, 0, 1), len, yTopA - yLong);
        else carvedApron(carved, vec3(lx - bh, yLong, -az), vec3(-1, 0, 0), vec3(0, 0, -1), len, yTopA - yLong);
    }
    for (float sx : {-1.0f, 1.0f}) {
        float len = 2.0f * (lz - bh);
        boxAA(carved, vec3(sx > 0 ? ax - th : -ax + 0.0005f, yShort, -lz + bh), vec3(sx > 0 ? ax - 0.0005f : -ax + th, yTopA, lz - bh),
              F_NY | (sx > 0 ? F_NX : F_PX));
        if (sx > 0) carvedApron(carved, vec3(ax, yShort, lz - bh), vec3(0, 0, -1), vec3(1, 0, 0), len, yTopA - yShort);
        else carvedApron(carved, vec3(-ax, yShort, -lz + bh), vec3(0, 0, 1), vec3(-1, 0, 0), len, yTopA - yShort);
        // Gilded central rosette on the short aprons. It sits on the fluted field: a 3 mm collar
        // under its rim (its own lathe, for a hard edge) sinks it below the bottom of the flutes,
        // so no gap opens under the rim where a flute passes.
        vec3 n(sx, 0, 0), t(0, 0, 1), b = cross(t, n);
        const mat4 xf = mat4(mat3(t, n, b), vec3(sx * (ax + 0.0025f), 0.5f * (yShort + yTopA), 0));
        auto lobes = [](float r, float a, float) { return r * (0.72f + 0.28f * std::pow(std::fabs(std::cos(a * 5.0f)), 0.6f)); };
        Profile pr = {{0.035f, 0.0f}, {0.033f, 0.003f}, {0.026f, 0.007f}, {0.018f, 0.008f}, {0.012f, 0.007f}, {0.008f, 0.01f}, {0.0f, 0.012f}};
        latheMod(gilt, pr, 60, xf, lobes, 1);
        latheMod(gilt, {{0.035f, -0.003f}, {0.035f, 0.0f}}, 60, xf, lobes, 1);
    }
    // ---- Legs at the corners: block flush with the aprons, fluted tapering leg, gilded collar rosettes.
    for (float sx : {-1.0f, 1.0f})
        for (float sz : {-1.0f, 1.0f}) {
            vec3 base(sx * lx, 0.0f, sz * lz);
            louisLeg(carved, carved, base, Y - T, 0.034f, Y - T - yShort, bh, 12);
            // Rosettes on the two outer faces of the block.
            float yc = 0.5f * (yShort + yTopA);
            auto rosetteAt = [&](vec3 c, vec3 n) {
                vec3 t = orthogonal(n), b = cross(t, n);
                Profile pr = {{0.018f, 0.0f}, {0.017f, 0.002f}, {0.013f, 0.004f}, {0.009f, 0.0045f}, {0.006f, 0.004f}, {0.004f, 0.006f}, {0.0f, 0.007f}};
                latheMod(gilt, pr, 40, mat4(mat3(t, n, b), c), [](float r, float a, float) {
                    return r * (0.75f + 0.25f * std::pow(std::fabs(std::cos(a * 4.0f)), 0.6f));
                }, 1);
            };
            rosetteAt(base + vec3(sx * bh, yc, 0), vec3(sx, 0, 0));
            rosetteAt(base + vec3(0, yc, sz * bh), vec3(0, 0, sz));
            // Gilded sabot (foot ring).
            Profile sab = {{0.0f, 0.0f}, {0.0152f, 0.0f}, {0.0152f, 0.0f}, {0.0158f, 0.012f}, {0.0152f, 0.024f}, {0.0f, 0.024f}};
            hallgeo::lathe(gilt, sab, 20, translate(base));
        }
    Model m;
    addPart(m, "table_top", std::move(top), MaterialId::TableWood, STATIC_FLAGS);
    addPart(m, "table_carved", std::move(carved), MaterialId::TableWoodCarved, STATIC_FLAGS);
    addPart(m, "table_gilt", std::move(gilt), MaterialId::GildedTrim, STATIC_FLAGS);
    return m;
}

// ------------------------------------------------------------------------------------------------
Model buildChair() {
    MeshData wood, velvet;
    const float S = layout::SEAT_HEIGHT;
    const float railTop = S - 0.05f, railBot = railTop - 0.075f;
    // Seat outline (bowed front), traversed so that sweeps point outwards.
    const float fz = 0.215f, bz = -0.2f, fx = 0.27f, bx = 0.23f;
    std::vector<vec3> seat;
    auto frontZ = [&](float x) { return fz + 0.025f * std::cos(0.5f * PI * x / fx); };
    // Back edge (-z) from -x to +x, right side, bowed front from +x to -x, left side.
    seat.push_back(vec3(-bx, 0, bz));
    seat.push_back(vec3(bx, 0, bz));
    for (int i = 0; i <= 16; ++i) {
        float x = fx - 2.0f * fx * float(i) / 16.0f;
        seat.push_back(vec3(x, 0, frontZ(x)));
    }
    if (dot(sweepSide(seat, vec3(0, 1, 0)), vec3(0, 0, -1)) < 0.0f) seat = reversed(seat);
    // Seat rails with a moulded outer face.
    Profile rail = {{-0.035f, railBot}, {0.0f, railBot}, {0.0f, railBot}, {0.004f, railBot + 0.006f}, {0.006f, railBot + 0.014f},
                    {0.004f, railBot + 0.02f}, {0.0f, railBot + 0.024f}, {0.0f, railBot + 0.03f}, {0.003f, railBot + 0.034f},
                    {0.003f, railTop - 0.012f}, {0.008f, railTop - 0.008f}, {0.008f, railTop - 0.004f}, {0.004f, railTop},
                    {-0.02f, railTop}};
    sweep(wood, rail, seat, vec3(0, 1, 0), true, 25.0f);
    // Cushion: flat-topped pillow over the seat outline, rolled edges down onto the rails.
    {
        auto fn = [&](float s, float t) {
            float x = (s * 2.0f - 1.0f), z = (t * 2.0f - 1.0f);
            float r = std::pow(std::pow(std::fabs(x), 6.0f) + std::pow(std::fabs(z), 6.0f), 1.0f / 6.0f);
            float h = 0.046f * std::pow(std::max(0.0f, 1.0f - std::pow(std::min(r, 1.0f), 10.0f)), 0.35f);
            h += 0.004f * (1.0f - x * x) * (1.0f - z * z);
            float zz = lerp(bz + 0.012f, frontZ(x * fx) - 0.012f, t);
            float halfW = lerp(bx, fx, t) - 0.012f;
            return vec3(x * halfW, railTop + h, zz);
        };
        surfaceFacing(velvet, 40, 40, fn, vec3(0, 1, 0), UVMode::Meters);
        // Gimp braid along the cushion edge.
        std::vector<vec3> edgePath;
        for (int i = 0; i < 64; ++i) {
            float a = TAU * float(i) / 64.0f;
            float x = std::cos(a), z = std::sin(a);
            float k = std::pow(std::pow(std::fabs(x), 6.0f) + std::pow(std::fabs(z), 6.0f), 1.0f / 6.0f);
            x /= k;
            z /= k;
            float t = z * 0.5f + 0.5f;
            float zz = lerp(bz + 0.012f, frontZ(x * fx) - 0.012f, t);
            float halfW = lerp(bx, fx, t) - 0.012f;
            edgePath.push_back(vec3(x * halfW, railTop + 0.004f, zz));
        }
        Profile gimp;
        for (int i = 0; i <= 6; ++i) {
            float th = PI * float(i) / 6.0f;
            gimp.push_back({0.005f * std::cos(th) - 0.004f, 0.005f * std::sin(th)});
        }
        if (dot(sweepSide(edgePath, vec3(0, 1, 0)), edgePath[0] - vec3(0, edgePath[0].y, 0)) < 0.0f) edgePath = reversed(edgePath);
        sweep(wood, gimp, edgePath, vec3(0, 1, 0), true, 60.0f);
    }
    // Legs: fluted, with rosettes on the front blocks.
    const vec3 legs[4] = {vec3(-fx + 0.03f, 0, frontZ(fx) - 0.03f), vec3(fx - 0.03f, 0, frontZ(fx) - 0.03f), vec3(-bx + 0.03f, 0, bz + 0.03f),
                          vec3(bx - 0.03f, 0, bz + 0.03f)};
    for (int i = 0; i < 4; ++i) {
        louisLeg(wood, wood, legs[i], railTop, 0.026f, railTop - railBot, 0.03f, 10);
        if (i < 2) {
            vec3 c = legs[i] + vec3(0, 0.5f * (railTop + railBot), 0.03f);
            vec3 n(0, 0, 1), t = vec3(1, 0, 0), b = cross(t, n);
            Profile pr = {{0.02f, 0.0f}, {0.018f, 0.003f}, {0.013f, 0.005f}, {0.008f, 0.005f}, {0.005f, 0.007f}, {0.0f, 0.008f}};
            latheMod(wood, pr, 40, mat4(mat3(t, n, b), c), [](float r, float a, float) {
                return r * (0.75f + 0.25f * std::pow(std::fabs(std::cos(a * 4.0f)), 0.6f));
            }, 1);
        }
    }
    // Oval (médaillon) back, raked back by 9 degrees.
    const float rake = 9.0f * DEG;
    const vec3 C(0, 0.815f, bz - 0.02f);
    const vec3 upB(0, std::cos(rake), -std::sin(rake));
    const vec3 fwd = normalize(cross(vec3(1, 0, 0), upB));  // back plane normal (+Z-ish)
    const float ea = 0.205f, eb = 0.25f;
    std::vector<vec3> oval;
    for (int i = 0; i < 72; ++i) {
        float t = TAU * float(i) / 72.0f;
        oval.push_back(C + vec3(ea * std::cos(t), 0, 0) + upB * (eb * std::sin(t)));
    }
    if (dot(sweepSide(oval, fwd), oval[0] - C) < 0.0f) oval = reversed(oval);
    Profile frame = {{-0.013f, -0.022f}, {0.032f, -0.022f}, {0.032f, -0.022f}, {0.032f, 0.006f}, {0.028f, 0.016f}, {0.018f, 0.023f},
                     {0.008f, 0.024f}, {0.0f, 0.02f}, {-0.004f, 0.022f}, {-0.009f, 0.018f}, {-0.013f, 0.01f}, {-0.013f, 0.01f},
                     {-0.013f, -0.022f}};
    sweep(wood, frame, oval, fwd, true, 60.0f);
    // Back cushion (front: padded dome, rear: flat panel).
    auto ovalPt = [&](float t, float inset) {
        return C + vec3((ea - inset) * std::cos(t), 0, 0) + upB * ((eb - inset) * std::sin(t));
    };
    surfaceFacing(velvet, 48, 16, [&](float u, float v) {
        float t = TAU * u, rho = v;
        vec3 p = lerp3(C, ovalPt(t, 0.008f), rho);
        return p + fwd * (0.004f + 0.034f * (1.0f - std::pow(rho, 4.0f)));
    }, fwd, UVMode::Meters, vec2(1, 1), true);
    surfaceFacing(velvet, 48, 4, [&](float u, float v) {
        float t = TAU * u;
        return lerp3(C, ovalPt(t, 0.008f), v) - fwd * 0.016f;
    }, -fwd, UVMode::Meters, vec2(1, 1), true);
    // Supports between the seat's back rail and the oval, with turned profile.
    Profile sup = {{0.0f, 0.0f}, {0.022f, 0.0f}, {0.022f, 0.0f}, {0.022f, 0.02f}, {0.016f, 0.035f}, {0.013f, 0.06f}, {0.017f, 0.09f},
                   {0.014f, 0.12f}, {0.017f, 0.14f}, {0.017f, 0.14f}, {0.0f, 0.14f}};
    for (float sx : {-1.0f, 1.0f}) {
        float t = -0.5f * PI + sx * 0.62f;
        vec3 top = ovalPt(t, -0.006f);
        vec3 bot(sx * 0.15f, railTop, bz + 0.012f);
        vec3 axis = top - bot;
        float len = length(axis);
        mat4 xf = toMat4(fromTo(vec3(0, 1, 0), axis / len), bot) * scale(vec3(1, len / 0.14f, 1));
        latheMod(wood, sup, 20, xf, [](float r, float, float) { return r; }, 1);
    }
    // Crest: ribbon-bow rosette with two loops on top of the oval.
    {
        vec3 top = ovalPt(0.5f * PI, -0.03f) + fwd * 0.012f;
        Profile pr = {{0.03f, 0.0f}, {0.027f, 0.006f}, {0.02f, 0.01f}, {0.012f, 0.011f}, {0.006f, 0.014f}, {0.0f, 0.016f}};
        vec3 t = vec3(1, 0, 0), b = cross(t, fwd);
        latheMod(wood, pr, 48, mat4(mat3(t, fwd, b), top), [](float r, float a, float) {
            return r * (0.7f + 0.3f * std::pow(std::fabs(std::cos(a * 3.0f)), 0.5f));
        }, 1);
        Profile cord;
        for (int i = 0; i <= 8; ++i) {
            float th = TAU * float(i) / 8.0f;
            cord.push_back({0.006f * std::cos(th), 0.006f * std::sin(th)});
        }
        for (float sx : {-1.0f, 1.0f}) {
            std::vector<vec3> loop;
            for (int i = 0; i < 20; ++i) {
                float a = TAU * float(i) / 20.0f;
                loop.push_back(top + vec3(sx * (0.055f + 0.035f * std::cos(a)), 0, 0) + upB * (0.018f * std::sin(a) + 0.012f));
            }
            sweep(wood, cord, loop, fwd, true, 60.0f);
        }
    }
    Model m;
    addPart(m, "chair_wood", std::move(wood), MaterialId::ChairWood, STATIC_FLAGS);
    addPart(m, "chair_velvet", std::move(velvet), MaterialId::ChairVelvet, STATIC_FLAGS);
    return m;
}

}  // namespace furniture
