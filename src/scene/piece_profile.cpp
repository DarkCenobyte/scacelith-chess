#include "piece_profile.h"
#include <algorithm>
#include <cmath>

using namespace m;

// ---------------------------------------------------------------------------------------------
Profile& Profile::to(float x, float y, float r) {
    vec2 p(x, y);
    if (!pts.empty() && length(p - pts.back()) < 1e-6f) {
        if (r >= 0.0f) radius.back() = r;
        return *this;
    }
    pts.push_back(p);
    radius.push_back(r);
    return *this;
}

Profile& Profile::ellipse(vec2 c, vec2 r, float a0, float a1) {
    int n = std::max(2, int(std::ceil(std::fabs(a1 - a0) / 4.0f)));
    for (int i = 0; i <= n; ++i) {
        float a = lerp(a0, a1, float(i) / float(n)) * DEG;
        to(c.x + r.x * std::cos(a), c.y + r.y * std::sin(a), (i == 0 || i == n) ? -1.0f : 0.0f);
    }
    return *this;
}

Profile& Profile::cubic(vec2 c1, vec2 c2, vec2 p) {
    vec2 p0 = last();
    float len = length(c1 - p0) + length(c2 - c1) + length(p - c2);
    int n = std::max(12, int(std::ceil(len / 0.35f)));
    for (int i = 1; i <= n; ++i) {
        float t = float(i) / float(n), u = 1.0f - t;
        vec2 q = p0 * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + p * (t * t * t);
        to(q.x, q.y, i == n ? -1.0f : 0.0f);
    }
    return *this;
}

Profile& Profile::quad(vec2 c, vec2 p) {
    vec2 p0 = last();
    return cubic(p0 + (c - p0) * (2.0f / 3.0f), p + (c - p) * (2.0f / 3.0f), p);
}

Profile& Profile::spline(const std::vector<vec2>& through) {
    if (through.empty()) return *this;
    std::vector<vec2> k;
    k.push_back(last());
    for (auto& p : through) k.push_back(p);
    size_t n = k.size();
    auto at = [&](int i) {
        if (i < 0) return k[0] * 2.0f - k[1];
        if (i >= int(n)) return k[n - 1] * 2.0f - k[n - 2];
        return k[size_t(i)];
    };
    for (int s = 0; s + 1 < int(n); ++s) {
        vec2 p0 = at(s - 1), p1 = at(s), p2 = at(s + 1), p3 = at(s + 2);
        int steps = std::max(6, int(std::ceil(length(p2 - p1) / 0.3f)));
        for (int i = 1; i <= steps; ++i) {
            float t = float(i) / float(steps), t2 = t * t, t3 = t2 * t;
            vec2 q = (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
            to(q.x, q.y, 0.0f);
        }
    }
    radius.back() = -1.0f;
    return *this;
}

Profile& Profile::scaled(float sx, float sy) {
    for (auto& p : pts) p = vec2(p.x * sx, p.y * sy);
    for (auto& r : radius)
        if (r > 0) r *= std::min(sx, sy);
    return *this;
}

// ---------------------------------------------------------------------------------------------
static std::vector<vec2> roundOpen(const std::vector<vec2>& pts, const std::vector<float>& rad, float fillet, float maxSeg,
                                   float minAngleDeg) {
    size_t n = pts.size();
    if (n < 3) return pts;
    std::vector<float> S(n, 0.0f);
    for (size_t i = 1; i < n; ++i) S[i] = S[i - 1] + length(pts[i] - pts[i - 1]);
    auto pointAt = [&](float s) {
        s = clamp(s, 0.0f, S.back());
        size_t i = size_t(std::upper_bound(S.begin(), S.end(), s) - S.begin());
        if (i == 0) return pts[0];
        if (i >= n) return pts.back();
        float seg = S[i] - S[i - 1];
        float t = seg > 1e-12f ? (s - S[i - 1]) / seg : 0.0f;
        return lerp(pts[i - 1], pts[i], t);
    };
    struct Corner { size_t i; float r, theta; };
    std::vector<Corner> corners;
    float minA = minAngleDeg * DEG;
    for (size_t i = 1; i + 1 < n; ++i) {
        float r = rad[i] >= 0.0f ? rad[i] : fillet;
        if (r <= 0.0f) continue;
        vec2 d1 = normalize(pts[i] - pts[i - 1]), d2 = normalize(pts[i + 1] - pts[i]);
        float th = std::acos(clamp(dot(d1, d2), -1.0f, 1.0f));
        if (th > minA) corners.push_back({i, r, th});
    }
    std::vector<vec2> out;
    auto push = [&](vec2 p) {
        if (out.empty() || length(p - out.back()) > 1e-6f) out.push_back(p);
    };
    float cur = 0.0f;
    size_t j = 0;
    push(pts[0]);
    for (size_t c = 0; c < corners.size(); ++c) {
        const Corner& k = corners[c];
        float sPrev = c > 0 ? S[corners[c - 1].i] : S[0];
        float sNext = c + 1 < corners.size() ? S[corners[c + 1].i] : S.back();
        float fPrev = c > 0 ? 0.5f : 0.9f, fNext = c + 1 < corners.size() ? 0.5f : 0.9f;
        float t = k.r * std::tan(std::min(k.theta, 170.0f * DEG) * 0.5f);
        float tb = std::min(t, (S[k.i] - sPrev) * fPrev), tf = std::min(t, (sNext - S[k.i]) * fNext);
        float s0 = S[k.i] - tb, s1 = S[k.i] + tf;
        while (j < n && S[j] <= cur + 1e-7f) ++j;
        while (j < n && S[j] < s0 - 1e-7f) push(pts[j++]);
        vec2 A = pointAt(s0), B = pointAt(s1), P = pts[k.i];
        int steps = std::max(3, int(std::ceil(k.theta / (7.0f * DEG))));
        for (int q = 0; q <= steps; ++q) {
            float u = float(q) / float(steps), v = 1.0f - u;
            push(A * (v * v) + P * (2 * u * v) + B * (u * u));
        }
        cur = s1;
    }
    while (j < n && S[j] <= cur + 1e-7f) ++j;
    while (j < n) push(pts[j++]);
    // Resample long segments.
    std::vector<vec2> res;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0) {
            float l = length(out[i] - out[i - 1]);
            int k = int(std::ceil(l / maxSeg));
            for (int q = 1; q < k; ++q) res.push_back(lerp(out[i - 1], out[i], float(q) / float(k)));
        }
        res.push_back(out[i]);
    }
    return res;
}

static void mergeDup(const Profile& p, std::vector<vec2>& pts, std::vector<float>& rad) {
    for (size_t i = 0; i < p.pts.size(); ++i) {
        if (!pts.empty() && length(p.pts[i] - pts.back()) < 1e-6f) continue;
        pts.push_back(p.pts[i]);
        rad.push_back(i < p.radius.size() ? p.radius[i] : -1.0f);
    }
}

std::vector<vec2> finishProfile(const Profile& p, float fillet, float maxSeg, float minAngleDeg) {
    std::vector<vec2> pts;
    std::vector<float> rad;
    mergeDup(p, pts, rad);
    return roundOpen(pts, rad, fillet, maxSeg, minAngleDeg);
}

std::vector<vec2> finishClosed(const Profile& p, float fillet, float maxSeg, float minAngleDeg) {
    std::vector<vec2> pts;
    std::vector<float> rad;
    mergeDup(p, pts, rad);
    if (pts.size() > 2 && length(pts.front() - pts.back()) < 1e-6f) { pts.pop_back(); rad.pop_back(); }
    if (pts.size() < 3) return pts;
    // Start in the middle of the longest segment so every vertex is interior.
    size_t best = 0;
    float bl = -1;
    for (size_t i = 0; i < pts.size(); ++i) {
        float l = length(pts[(i + 1) % pts.size()] - pts[i]);
        if (l > bl) { bl = l; best = i; }
    }
    std::vector<vec2> op;
    std::vector<float> orad;
    vec2 mid = (pts[best] + pts[(best + 1) % pts.size()]) * 0.5f;
    op.push_back(mid);
    orad.push_back(0.0f);
    for (size_t k = 1; k <= pts.size(); ++k) {
        size_t i = (best + k) % pts.size();
        op.push_back(pts[i]);
        orad.push_back(rad[i]);
    }
    op.push_back(mid);
    orad.push_back(0.0f);
    std::vector<vec2> r = roundOpen(op, orad, fillet, maxSeg, minAngleDeg);
    if (r.size() > 1) r.pop_back();
    return r;
}

// ---------------------------------------------------------------------------------------------
// Douglas-Peucker on an open polyline: keeps points deviating more than tol.
static void dpSimplify(const std::vector<vec2>& p, size_t a, size_t b, float tol, std::vector<char>& keep) {
    if (b <= a + 1) return;
    vec2 d = p[b] - p[a];
    float l = length(d);
    float best = -1;
    size_t bi = a;
    for (size_t i = a + 1; i < b; ++i) {
        float dist = l > 1e-9f ? std::fabs((p[i].x - p[a].x) * d.y - (p[i].y - p[a].y) * d.x) / l : length(p[i] - p[a]);
        if (dist > best) { best = dist; bi = i; }
    }
    if (best > tol) {
        keep[bi] = 1;
        dpSimplify(p, a, bi, tol, keep);
        dpSimplify(p, bi, b, tol, keep);
    }
}

MeshData latheProfile(const std::vector<vec2>& dense, int segments, float vScale, float tol, float maxSeg) {
    MeshData d;
    size_t n = dense.size();
    if (n < 2) return d;
    // Normals and arc length on the dense curve (outward for a bottom-to-top profile).
    std::vector<float> S(n, 0.0f);
    for (size_t i = 1; i < n; ++i) S[i] = S[i - 1] + length(dense[i] - dense[i - 1]);
    std::vector<vec2> N(n);
    auto segN = [&](size_t a, size_t b) {
        vec2 t = dense[b] - dense[a];
        vec2 q(t.y, -t.x);
        float l = length(q);
        return l > 1e-12f ? q / l : vec2(0, 0);
    };
    for (size_t i = 0; i < n; ++i) {
        vec2 s(0, 0);
        if (i > 0) s += segN(i - 1, i);
        if (i + 1 < n) s += segN(i, i + 1);
        N[i] = length(s) > 1e-9f ? normalize(s) : vec2(0, 1);
    }
    auto normalAt = [&](float s) {
        size_t i = size_t(std::upper_bound(S.begin(), S.end(), s) - S.begin());
        if (i == 0) return N[0];
        if (i >= n) return N[n - 1];
        float seg = S[i] - S[i - 1];
        float t = seg > 1e-12f ? (s - S[i - 1]) / seg : 0.0f;
        return normalize(lerp(N[i - 1], N[i], t));
    };
    std::vector<char> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    if (tol > 0.0f) dpSimplify(dense, 0, n - 1, tol, keep);
    else std::fill(keep.begin(), keep.end(), 1);
    std::vector<vec2> P, PN;
    size_t prev = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!keep[i]) continue;
        if (i > 0) {
            // Subdivide long spans only where the normal turns along them (flat or conical
            // spans with matching end normals stay one segment).
            float l = length(dense[i] - dense[prev]);
            bool turns = dot(N[i], N[prev]) < 0.9986f;  // ~3 degrees
            int k = maxSeg > 0.0f && turns ? int(std::ceil(l / maxSeg)) : 1;
            for (int q = 1; q < k; ++q) {
                float t = float(q) / float(k);
                P.push_back(lerp(dense[prev], dense[i], t));
                PN.push_back(normalAt(lerp(S[prev], S[i], t)));
            }
        }
        P.push_back(dense[i]);
        PN.push_back(N[i]);
        prev = i;
    }
    size_t np = P.size();
    for (size_t i = 0; i < np; ++i)
        for (int s = 0; s <= segments; ++s) {
            float u = float(s) / float(segments), a = u * TAU;
            float c = std::cos(a), sn = std::sin(a);
            Vertex v;
            v.pos = vec3(P[i].x * c, P[i].y, -P[i].x * sn);
            v.normal = normalize(vec3(PN[i].x * c, PN[i].y, -PN[i].x * sn));
            v.tangent = vec4(-sn, 0, -c, 1);
            v.uv = vec2(u, P[i].y / vScale);
            d.vertices.push_back(v);
        }
    for (size_t i = 0; i + 1 < np; ++i) {
        bool axis0 = P[i].x < 1e-6f, axis1 = P[i + 1].x < 1e-6f;
        for (int s = 0; s < segments; ++s) {
            uint32_t a = uint32_t(i * size_t(segments + 1) + size_t(s)), b = a + 1, c = a + uint32_t(segments + 1), e = c + 1;
            if (!axis0) for (uint32_t k : {a, b, e}) d.indices.push_back(k);
            if (!axis1) for (uint32_t k : {a, e, c}) d.indices.push_back(k);
        }
    }
    return d;
}

static std::vector<vec2> profileNormals(const std::vector<vec2>& prof) {
    // Profile given as (inward offset, height): convert to (outward, up) normals.
    size_t n = prof.size();
    std::vector<vec2> nrm(n, vec2(0, 1));
    auto segN = [&](size_t a, size_t b) {
        vec2 t(-(prof[b].x - prof[a].x), prof[b].y - prof[a].y);
        vec2 q(-t.y, t.x);
        float l = length(q);
        return l > 1e-12f ? q / l : vec2(0, 0);
    };
    for (size_t i = 0; i < n; ++i) {
        vec2 s(0, 0);
        if (i > 0) s += segN(i - 1, i);
        if (i + 1 < n) s += segN(i, i + 1);
        nrm[i] = length(s) > 1e-9f ? normalize(s) : vec2(0, 1);
    }
    return nrm;
}

MeshData sweepRoundedRect(const std::vector<vec2>& prof, float hx, float hz, float rc, int cs) {
    MeshData d;
    std::vector<vec2> pn = profileNormals(prof);
    std::vector<float> parc(prof.size(), 0.0f);
    for (size_t i = 1; i < prof.size(); ++i) parc[i] = parc[i - 1] + length(prof[i] - prof[i - 1]);
    const float sx[4] = {1, -1, -1, 1}, sz[4] = {1, 1, -1, -1};
    struct OS { vec2 c, s; float a; };
    std::vector<OS> outline;
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i <= cs; ++i) {
            float a = (90.0f * float(k) + 90.0f * float(i) / float(cs)) * DEG;
            outline.push_back({vec2(sx[k] * (hx - rc), sz[k] * (hz - rc)), vec2(sx[k], sz[k]), a});
        }
    size_t no = outline.size(), np = prof.size();
    float u = 0.0f;
    vec3 prevP0;
    for (size_t s = 0; s <= no; ++s) {
        const OS& o = outline[s % no];
        vec2 dir(std::cos(o.a), std::sin(o.a));
        vec3 N(dir.x, 0, dir.y), T(-dir.y, 0, dir.x);
        for (size_t i = 0; i < np; ++i) {
            float r = rc - prof[i].x;
            vec2 q = r >= 0.0f ? o.c + dir * r : o.c + o.s * r;
            Vertex v;
            v.pos = vec3(q.x, prof[i].y, q.y);
            if (i == 0) {
                if (s > 0) u += length(v.pos - prevP0);
                prevP0 = v.pos;
            }
            v.normal = normalize(N * pn[i].x + vec3(0, pn[i].y, 0));
            v.tangent = vec4(T, 1);
            v.uv = vec2(u, parc[i]);
            d.vertices.push_back(v);
        }
    }
    for (size_t s = 0; s < no; ++s)
        for (size_t i = 0; i + 1 < np; ++i) {
            uint32_t a = uint32_t(s * np + i), b = a + 1, c = uint32_t((s + 1) * np + i), e = c + 1;
            for (uint32_t k : {a, c, e, a, e, b}) d.indices.push_back(k);
        }
    fixWinding(d);
    return d;
}

MeshData roundedPrism(const std::vector<vec2>& poly, float rc, float L, float re, int cs, int es, int ls) {
    MeshData d;
    size_t n = poly.size();
    struct CS { vec2 c; float a; };
    std::vector<CS> ring;
    vec2 centroid(0, 0);
    for (auto& p : poly) centroid += p;
    centroid /= float(n);
    for (size_t i = 0; i < n; ++i) {
        vec2 p = poly[i], pp = poly[(i + n - 1) % n], pn = poly[(i + 1) % n];
        vec2 e0 = normalize(p - pp), e1 = normalize(pn - p);
        vec2 n0(e0.y, -e0.x), n1(e1.y, -e1.x);
        vec2 c = p - (n0 + n1) * (rc / (1.0f + dot(n0, n1)));
        float a0 = std::atan2(n0.y, n0.x), a1 = std::atan2(n1.y, n1.x);
        while (a1 < a0) a1 += TAU;
        for (int k = 0; k <= cs; ++k) ring.push_back({c, lerp(a0, a1, float(k) / float(cs))});
    }
    size_t nr = ring.size();
    // Perimeter parameter at zero inset.
    std::vector<float> per(nr + 1, 0.0f);
    for (size_t i = 1; i <= nr; ++i) {
        const CS &a = ring[i - 1], &b = ring[i % nr];
        vec2 pa = a.c + vec2(std::cos(a.a), std::sin(a.a)) * rc, pb = b.c + vec2(std::cos(b.a), std::sin(b.a)) * rc;
        per[i] = per[i - 1] + length(pb - pa);
    }
    struct Sec { float z, inset, nz, nr; };
    std::vector<Sec> secs;
    for (int j = 0; j <= es; ++j) {
        float ph = (90.0f - 90.0f * float(j) / float(es)) * DEG;
        secs.push_back({-(L - re) - re * std::sin(ph), re * (1 - std::cos(ph)), -std::sin(ph), std::cos(ph)});
    }
    for (int j = 1; j < ls; ++j) secs.push_back({lerp(-(L - re), L - re, float(j) / float(ls)), 0, 0, 1});
    for (int j = 0; j <= es; ++j) {
        float ph = (90.0f * float(j) / float(es)) * DEG;
        secs.push_back({(L - re) + re * std::sin(ph), re * (1 - std::cos(ph)), std::sin(ph), std::cos(ph)});
    }
    for (auto& s : secs)
        for (size_t i = 0; i <= nr; ++i) {
            const CS& c = ring[i % nr];
            vec2 dir(std::cos(c.a), std::sin(c.a));
            vec2 q = c.c + dir * (rc - s.inset);
            Vertex v;
            v.pos = vec3(q.x, q.y, s.z);
            v.normal = normalize(vec3(dir.x * s.nr, dir.y * s.nr, s.nz));
            v.tangent = vec4(0, 0, 1, 1);
            v.uv = vec2(s.z, per[i]);
            d.vertices.push_back(v);
        }
    size_t rs = nr + 1;
    for (size_t j = 0; j + 1 < secs.size(); ++j)
        for (size_t i = 0; i < nr; ++i) {
            uint32_t a = uint32_t(j * rs + i), b = a + 1, c = uint32_t((j + 1) * rs + i), e = c + 1;
            for (uint32_t k : {a, b, e, a, e, c}) d.indices.push_back(k);
        }
    // Caps (fans).
    for (int side = 0; side < 2; ++side) {
        size_t ringStart = side == 0 ? 0 : (secs.size() - 1) * rs;
        Vertex c;
        c.pos = vec3(centroid.x, centroid.y, side == 0 ? -L : L);
        c.normal = vec3(0, 0, side == 0 ? -1.0f : 1.0f);
        c.tangent = vec4(1, 0, 0, 1);
        c.uv = vec2(c.pos.z, 0);
        uint32_t ci = uint32_t(d.vertices.size());
        d.vertices.push_back(c);
        for (size_t i = 0; i < nr; ++i) {
            uint32_t a = uint32_t(ringStart + i), b = a + 1;
            for (uint32_t k : {ci, a, b}) d.indices.push_back(k);
        }
    }
    fixWinding(d);
    // Tangent along Z, orthogonalised (normal may be parallel on the caps).
    for (auto& v : d.vertices) {
        vec3 t = vec3(0, 0, 1) - v.normal * v.normal.z;
        if (length2(t) < 1e-6f) t = vec3(1, 0, 0) - v.normal * v.normal.x;
        v.tangent = vec4(normalize(t), 1);
    }
    return d;
}

void fixWinding(MeshData& d) {
    for (size_t i = 0; i + 2 < d.indices.size(); i += 3) {
        const Vertex &a = d.vertices[d.indices[i]], &b = d.vertices[d.indices[i + 1]], &c = d.vertices[d.indices[i + 2]];
        vec3 g = cross(b.pos - a.pos, c.pos - a.pos);
        if (length2(g) < 1e-20f) continue;
        if (dot(g, a.normal + b.normal + c.normal) < 0.0f) std::swap(d.indices[i + 1], d.indices[i + 2]);
    }
}

void setAxialTangents(MeshData& d) {
    for (auto& v : d.vertices) {
        vec3 t(v.pos.z, 0, -v.pos.x);
        if (length2(t) < 1e-14f) t = vec3(1, 0, 0);
        t = normalize(t);
        vec3 o = t - v.normal * dot(v.normal, t);
        if (length2(o) < 1e-8f) o = orthogonal(v.normal);
        v.tangent = vec4(normalize(o), 1);
    }
}
