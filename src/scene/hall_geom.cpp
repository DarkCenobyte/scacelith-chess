// Procedural geometry toolkit for the hall and furniture (see hall_geom.h).
#include "hall_geom.h"

using namespace m;

namespace hallgeo {

uint32_t vtx(MeshData& d, vec3 p, vec3 n, vec3 t, vec2 uv) {
    Vertex v;
    v.pos = p;
    v.normal = n;
    v.tangent = vec4(t, 1.0f);
    v.uv = uv;
    d.vertices.push_back(v);
    return uint32_t(d.vertices.size() - 1);
}

void tri(MeshData& d, uint32_t a, uint32_t b, uint32_t c) {
    const Vertex &A = d.vertices[a], &B = d.vertices[b], &C = d.vertices[c];
    vec3 fn = cross(B.pos - A.pos, C.pos - A.pos);
    if (length2(fn) < 1e-18f) return;  // degenerate
    vec3 vn = A.normal + B.normal + C.normal;
    if (dot(fn, vn) < 0.0f) std::swap(b, c);
    d.indices.push_back(a);
    d.indices.push_back(b);
    d.indices.push_back(c);
}

void quad(MeshData& d, uint32_t a, uint32_t b, uint32_t c, uint32_t e) {
    // Split along the shorter diagonal for better shading of non-planar quads.
    const vec3 &A = d.vertices[a].pos, &B = d.vertices[b].pos, &C = d.vertices[c].pos, &E = d.vertices[e].pos;
    if (length2(C - A) <= length2(E - B)) {
        tri(d, a, b, c);
        tri(d, a, c, e);
    } else {
        tri(d, a, b, e);
        tri(d, b, c, e);
    }
}

void quadFlat(MeshData& d, vec3 a, vec3 b, vec3 c, vec3 e, vec3 n, vec3 uAxis) {
    vec3 vAxis = cross(n, uAxis);
    auto uv = [&](vec3 p) { return vec2(dot(p, uAxis), dot(p, vAxis)); };
    uint32_t ia = vtx(d, a, n, uAxis, uv(a)), ib = vtx(d, b, n, uAxis, uv(b));
    uint32_t ic = vtx(d, c, n, uAxis, uv(c)), ie = vtx(d, e, n, uAxis, uv(e));
    tri(d, ia, ib, ic);
    tri(d, ia, ic, ie);
}

void box(MeshData& d, vec3 c, vec3 ax, vec3 ay, vec3 az, vec3 h, unsigned faces) {
    struct F { unsigned bit; vec3 n, u, v; float hn, hu, hv; };
    const F fs[6] = {{F_PX, ax, az, ay, h.x, h.z, h.y},  {F_NX, -ax, az, ay, h.x, h.z, h.y},
                     {F_PY, ay, ax, az, h.y, h.x, h.z},  {F_NY, -ay, ax, az, h.y, h.x, h.z},
                     {F_PZ, az, ax, ay, h.z, h.x, h.y},  {F_NZ, -az, ax, ay, h.z, h.x, h.y}};
    for (const F& f : fs) {
        if (!(faces & f.bit)) continue;
        vec3 o = c + f.n * f.hn;
        vec3 a = o - f.u * f.hu - f.v * f.hv, b = o + f.u * f.hu - f.v * f.hv;
        vec3 cc = o + f.u * f.hu + f.v * f.hv, e = o - f.u * f.hu + f.v * f.hv;
        quadFlat(d, a, b, cc, e, f.n, f.u);
    }
}

void boxAA(MeshData& d, vec3 lo, vec3 hi, unsigned faces) {
    box(d, (lo + hi) * 0.5f, vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1), (hi - lo) * 0.5f, faces);
}

std::vector<float> linspace(float a, float b, int n) {
    std::vector<float> r(size_t(n) + 1);
    for (int i = 0; i <= n; ++i) r[size_t(i)] = a + (b - a) * float(i) / float(n);
    return r;
}

void surfaceGrid(MeshData& d, const std::vector<float>& us, const std::vector<float>& vs, const SurfFn& fn, bool flip,
                 UVMode uvMode, vec2 uvScale, bool closedU) {
    int nu = int(us.size()), nv = int(vs.size());
    if (nu < 2 || nv < 2) return;
    std::vector<vec3> P(size_t(nu) * size_t(nv));
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) P[size_t(j * nu + i)] = fn(us[size_t(i)], vs[size_t(j)]);
    auto at = [&](int i, int j) -> const vec3& { return P[size_t(j * nu + i)]; };
    // Lengths for metric uvs.
    std::vector<float> lu(static_cast<size_t>(nu), 0.0f), lv(static_cast<size_t>(nv), 0.0f);
    if (uvMode == UVMode::Meters) {
        int jm = nv / 2, im = nu / 2;
        for (int i = 1; i < nu; ++i) lu[size_t(i)] = lu[size_t(i - 1)] + length(at(i, jm) - at(i - 1, jm));
        for (int j = 1; j < nv; ++j) lv[size_t(j)] = lv[size_t(j - 1)] + length(at(im, j) - at(im, j - 1));
    }
    uint32_t base = uint32_t(d.vertices.size());
    float umin = us.front(), umax = us.back(), vmin = vs.front(), vmax = vs.back();
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            int i0 = i - 1, i1 = i + 1;
            if (closedU) {
                if (i0 < 0) i0 = nu - 2;
                if (i1 > nu - 1) i1 = 1;
            } else {
                i0 = std::max(i0, 0);
                i1 = std::min(i1, nu - 1);
            }
            vec3 du = at(i1, j) - at(i0, j);
            vec3 dv = at(i, std::min(j + 1, nv - 1)) - at(i, std::max(j - 1, 0));
            vec3 n = cross(du, dv);
            if (length2(n) < 1e-20f) {
                // Degenerate (pole / collapsed row): sample the function slightly inside.
                float e = 1e-3f;
                float u = clamp(us[size_t(i)], umin + e, umax - e), v = clamp(vs[size_t(j)], vmin + e, vmax - e);
                du = fn(u + e * 0.5f, v) - fn(u - e * 0.5f, v);
                dv = fn(u, v + e * 0.5f) - fn(u, v - e * 0.5f);
                n = cross(du, dv);
                if (length2(n) < 1e-24f) n = vec3(0, 1, 0);
            }
            n = normalize(n);
            if (flip) n = -n;
            vec3 t = length2(du) > 1e-20f ? normalize(du) : orthogonal(n);
            vec2 uv = uvMode == UVMode::Param ? vec2(us[size_t(i)] * uvScale.x, vs[size_t(j)] * uvScale.y)
                                              : vec2(lu[size_t(i)] * uvScale.x, lv[size_t(j)] * uvScale.y);
            vtx(d, at(i, j), n, t, uv);
            if (flip) d.vertices.back().tangent.w = -1.0f;  // bitangent follows +v
        }
    for (int j = 0; j + 1 < nv; ++j)
        for (int i = 0; i + 1 < nu; ++i) {
            uint32_t a = base + uint32_t(j * nu + i), b = a + 1, c = a + uint32_t(nu) + 1, e = a + uint32_t(nu);
            quad(d, a, b, c, e);
        }
}

void surface(MeshData& d, int nu, int nv, const SurfFn& fn, bool flip, UVMode uvMode, vec2 uvScale, bool closedU) {
    surfaceGrid(d, linspace(0, 1, nu), linspace(0, 1, nv), fn, flip, uvMode, uvScale, closedU);
}

void surfaceFacing(MeshData& d, int nu, int nv, const SurfFn& fn, vec3 refDir, UVMode uvMode, vec2 uvScale, bool closedU) {
    MeshData t;
    surface(t, nu, nv, fn, false, uvMode, uvScale, closedU);
    vec3 acc(0);
    for (auto& v : t.vertices) acc += v.normal;
    if (dot(acc, refDir) < 0.0f) {
        t = MeshData();
        surface(t, nu, nv, fn, true, uvMode, uvScale, closedU);
    }
    d.append(t);
}

// 2D profile normals (air side), duplicate points = creases.
static std::vector<vec2> profileNormals(const Profile& p) {
    int np = int(p.size());
    auto segN = [&](int a, int b) {
        vec2 t = p[size_t(b)] - p[size_t(a)];
        vec2 n{t.y, -t.x};
        float l = length(n);
        return l > 1e-9f ? n / l : vec2(0, 0);
    };
    std::vector<vec2> nrm(static_cast<size_t>(np));
    for (int i = 0; i < np; ++i) {
        vec2 n(0, 0);
        bool dupPrev = i > 0 && length(p[size_t(i)] - p[size_t(i - 1)]) < 1e-7f;
        bool dupNext = i < np - 1 && length(p[size_t(i + 1)] - p[size_t(i)]) < 1e-7f;
        if (i > 0 && !dupPrev) n += segN(i - 1, i);
        if (i < np - 1 && !dupNext) n += segN(i, i + 1);
        if (length(n) < 1e-9f) {
            if (dupPrev && i < np - 1) n = segN(i, i + 1);
            else if (dupNext && i > 0) n = segN(i - 1, i);
        }
        nrm[size_t(i)] = length(n) > 1e-9f ? normalize(n) : vec2(0, 1);
    }
    return nrm;
}

void sweep(MeshData& d, const Profile& prof, const std::vector<vec3>& path, vec3 W, bool closed, float creaseDeg,
           bool capStart, bool capEnd) {
    int np = int(path.size()), nprof = int(prof.size());
    if (np < 2 || nprof < 2) return;
    W = normalize(W);
    int nseg = closed ? np : np - 1;
    std::vector<vec3> t(static_cast<size_t>(nseg)), s(static_cast<size_t>(nseg));
    for (int k = 0; k < nseg; ++k) {
        vec3 dir = path[size_t((k + 1) % np)] - path[size_t(k)];
        dir -= W * dot(dir, W);
        t[size_t(k)] = normalize(dir);
        s[size_t(k)] = normalize(cross(W, t[size_t(k)]));
    }
    std::vector<vec2> n2 = profileNormals(prof);
    std::vector<float> plen(static_cast<size_t>(nprof), 0.0f);
    for (int j = 1; j < nprof; ++j) plen[size_t(j)] = plen[size_t(j - 1)] + length(prof[size_t(j)] - prof[size_t(j - 1)]);
    float cosCrease = std::cos(creaseDeg * DEG);
    std::vector<uint32_t> ringIn(static_cast<size_t>(np)), ringOut(static_cast<size_t>(np));
    float pathLen = 0.0f;
    auto emitRing = [&](vec3 P, vec3 mitre, vec3 sn, vec3 tan, float u) {
        uint32_t base = uint32_t(d.vertices.size());
        for (int j = 0; j < nprof; ++j) {
            vec2 q = prof[size_t(j)];
            vec3 pos = P + mitre * q.x + W * q.y;
            vec3 nn = normalize(sn * n2[size_t(j)].x + W * n2[size_t(j)].y);
            vec2 tp = j + 1 < nprof ? prof[size_t(j + 1)] - prof[size_t(j)] : prof[size_t(j)] - prof[size_t(j - 1)];
            vec3 t3 = sn * tp.x + W * tp.y;
            vtx(d, pos, nn, tan, vec2(u, plen[size_t(j)]));
            if (dot(cross(nn, tan), t3) < 0.0f) d.vertices.back().tangent.w = -1.0f;
        }
        return base;
    };
    for (int i = 0; i < np; ++i) {
        if (i > 0) pathLen += length(path[size_t(i)] - path[size_t(i - 1)]);
        bool hasIn = closed || i > 0, hasOut = closed || i < np - 1;
        int kIn = (i - 1 + nseg) % nseg, kOut = i % nseg;
        vec3 s0 = hasIn ? s[size_t(kIn)] : s[size_t(kOut)];
        vec3 s1 = hasOut ? s[size_t(kOut)] : s[size_t(kIn)];
        vec3 t0 = hasIn ? t[size_t(kIn)] : t[size_t(kOut)];
        vec3 t1 = hasOut ? t[size_t(kOut)] : t[size_t(kIn)];
        float den = 1.0f + dot(s0, s1);
        vec3 mitre = den > 1e-3f ? (s0 + s1) / den : s0;
        bool sharp = dot(t0, t1) < cosCrease;
        if (sharp) {
            ringIn[size_t(i)] = emitRing(path[size_t(i)], mitre, s0, t0, pathLen);
            ringOut[size_t(i)] = emitRing(path[size_t(i)], mitre, s1, t1, pathLen);
        } else {
            ringIn[size_t(i)] = ringOut[size_t(i)] = emitRing(path[size_t(i)], mitre, normalize(s0 + s1), normalize(t0 + t1), pathLen);
        }
    }
    for (int k = 0; k < nseg; ++k) {
        uint32_t ra = ringOut[size_t(k)], rb = ringIn[size_t((k + 1) % np)];
        for (int j = 0; j + 1 < nprof; ++j) {
            if (length(prof[size_t(j + 1)] - prof[size_t(j)]) < 1e-7f) continue;
            quad(d, ra + uint32_t(j), rb + uint32_t(j), rb + uint32_t(j + 1), ra + uint32_t(j + 1));
        }
    }
    if (!closed) {
        auto cap = [&](uint32_t ring, vec3 n) {
            std::vector<vec3> pts;
            for (int j = 0; j < nprof; ++j) {
                vec3 p = d.vertices[ring + uint32_t(j)].pos;
                if (pts.empty() || length2(p - pts.back()) > 1e-14f) pts.push_back(p);
            }
            if (pts.size() < 3) return;
            vec3 c(0);
            for (vec3 p : pts) c += p;
            c /= float(pts.size());
            vec3 ua = orthogonal(n);
            vec3 va = cross(n, ua);
            uint32_t ic = vtx(d, c, n, ua, vec2(dot(c, ua), dot(c, va)));
            uint32_t b0 = uint32_t(d.vertices.size());
            for (vec3 p : pts) vtx(d, p, n, ua, vec2(dot(p, ua), dot(p, va)));
            for (uint32_t j = 0; j + 1 < pts.size(); ++j) tri(d, ic, b0 + j, b0 + j + 1);
            tri(d, ic, b0 + uint32_t(pts.size()) - 1, b0);
        };
        if (capStart) cap(ringOut[0], -t[0]);
        if (capEnd) cap(ringIn[size_t(np - 1)], t[size_t(nseg - 1)]);
    }
}

std::vector<vec3> reversed(const std::vector<vec3>& p) { return std::vector<vec3>(p.rbegin(), p.rend()); }

std::vector<vec3> offsetPath(const std::vector<vec3>& path, vec3 W, float b, bool closed) {
    int np = int(path.size());
    std::vector<vec3> r;
    if (np < 2) return path;
    W = normalize(W);
    int nseg = closed ? np : np - 1;
    std::vector<vec3> s(static_cast<size_t>(nseg));
    for (int k = 0; k < nseg; ++k) {
        vec3 dir = path[size_t((k + 1) % np)] - path[size_t(k)];
        dir -= W * dot(dir, W);
        s[size_t(k)] = normalize(cross(W, normalize(dir)));
    }
    for (int i = 0; i < np; ++i) {
        bool hasIn = closed || i > 0, hasOut = closed || i < np - 1;
        vec3 s0 = hasIn ? s[size_t((i - 1 + nseg) % nseg)] : s[size_t(i % nseg)];
        vec3 s1 = hasOut ? s[size_t(i % nseg)] : s[size_t((i - 1 + nseg) % nseg)];
        float den = 1.0f + dot(s0, s1);
        vec3 mitre = den > 1e-3f ? (s0 + s1) / den : s0;
        r.push_back(path[size_t(i)] + mitre * b);
    }
    return r;
}

vec3 sweepSide(const std::vector<vec3>& path, vec3 W) {
    vec3 dir = path[1] - path[0];
    W = normalize(W);
    dir -= W * dot(dir, W);
    return normalize(cross(W, normalize(dir)));
}

void lathe(MeshData& d, const Profile& prof, int seg, const mat4& xf) {
    MeshData l = prim::lathe(prof, seg);
    d.append(l, xf);
}

void latheMod(MeshData& d, const Profile& prof, int seg, const mat4& xf, const std::function<float(float, float, float)>& radiusFn,
              int subdiv) {
    int np = int(prof.size());
    if (np < 2) return;
    // Parameter along the profile: point index (with subdivisions between points).
    std::vector<float> vs;
    for (int i = 0; i < np - 1; ++i)
        for (int k = 0; k < subdiv; ++k) vs.push_back(float(i) + float(k) / float(subdiv));
    vs.push_back(float(np - 1));
    bool mirror = determinant(xf.upper3()) < 0.0f;
    auto fn = [&](float u, float v) {
        int i = std::min(int(v), np - 2);
        float f = v - float(i);
        vec2 p = lerp(prof[size_t(i)], prof[size_t(i + 1)], f);
        float a = u * TAU;
        float r = radiusFn(p.x, a, p.y);
        return transformPoint(xf, vec3(r * std::cos(a), p.y, -r * std::sin(a)));
    };
    surfaceGrid(d, linspace(0, 1, seg), vs, fn, mirror, UVMode::Meters, vec2(1, 1), true);
}

void louisLeg(MeshData& d, MeshData& blockMesh, vec3 base, float h, float rTop, float blockH, float blockHalf, int flutes) {
    // Block at the top (joins the apron / seat rail).
    boxAA(blockMesh, base + vec3(-blockHalf, h - blockH, -blockHalf), base + vec3(blockHalf, h, blockHalf), F_ALL & ~F_PY);
    // Turned part below the block, normalised by the turned height.
    float T = h - blockH;
    float r = rTop;
    Profile pr = {{0.0f, 0.0f},          {0.42f * r, 0.0f},      {0.55f * r, 0.012f * T}, {0.72f * r, 0.035f * T},
                  {0.62f * r, 0.06f * T}, {0.5f * r, 0.075f * T}, {0.62f * r, 0.09f * T},  {0.66f * r, 0.105f * T},
                  {0.56f * r, 0.12f * T}, {0.6f * r, 0.14f * T},  {0.62f * r, 0.16f * T},  {0.7f * r, 0.84f * T},
                  {0.8f * r, 0.86f * T},  {0.75f * r, 0.88f * T}, {0.9f * r, 0.9f * T},    {1.0f * r, 0.93f * T},
                  {0.92f * r, 0.955f * T}, {1.05f * r, 0.97f * T}, {1.1f * r, 0.99f * T},  {1.1f * r, T},
                  {0.0f, T}};
    // Flutes on the tapering shaft (between 0.18 T and 0.82 T).
    float nf = float(flutes);
    latheMod(d, pr, flutes * 6, translate(base), [nf, T](float rr, float a, float y) {
        float w = smoothstep(0.16f * T, 0.2f * T, y) * smoothstep(0.84f * T, 0.8f * T, y);
        float f = std::pow(std::fabs(std::sin(a * nf * 0.5f)), 3.0f);
        return rr * (1.0f - 0.09f * w * (1.0f - f));
    }, 2);
}

std::vector<vec2> archOutline(float uc, float w, float v0, float vs, int archSeg) {
    std::vector<vec2> r;
    r.push_back({uc - w, v0});
    r.push_back({uc - w, vs});
    for (int k = 1; k < archSeg; ++k) {
        float th = PI - PI * float(k) / float(archSeg);
        r.push_back({uc + w * std::cos(th), vs + w * std::sin(th)});
    }
    r.push_back({uc + w, vs});
    r.push_back({uc + w, v0});
    return r;
}

}  // namespace hallgeo
