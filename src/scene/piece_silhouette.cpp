#include "piece_silhouette.h"
#include <algorithm>
#include <cmath>

using namespace m;

namespace {

int sectorOf(float x, float z) {
    const float a = std::atan2(z, x);   // -pi..pi
    const int k = int(std::floor((a + PI) / (2.0f * PI) * float(PieceSilhouette::kSectors)));
    return std::clamp(k, 0, PieceSilhouette::kSectors - 1);
}

}  // namespace

PieceSilhouette PieceSilhouette::fromMesh(const MeshData& mesh) {
    PieceSilhouette s;
    float top = 0.0f;
    for (const Vertex& v : mesh.vertices) top = std::max(top, v.pos.y);
    if (top <= 0.0f) return s;
    s.height = top;
    const float slice = top / float(kSlices);
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        vec3 p[3];
        float r = 0.0f, y0 = 1e30f, y1 = -1e30f;
        int sectors[3], n = 0;
        for (int k = 0; k < 3; ++k) {
            p[k] = mesh.vertices[mesh.indices[i + size_t(k)]].pos;
            const float rk = std::sqrt(p[k].x * p[k].x + p[k].z * p[k].z);
            r = std::max(r, rk);
            y0 = std::min(y0, p[k].y);
            y1 = std::max(y1, p[k].y);
            if (rk > 1e-6f) sectors[n++] = sectorOf(p[k].x, p[k].z);   // a corner on the axis has no angle
        }
        if (n == 0) continue;
        // The sectors the triangle spans: the shortest arc around its corners' sectors (the
        // complement of the widest gap between them).
        for (int a = 1; a < n; ++a)   // sort (n <= 3)
            for (int b = a; b > 0 && sectors[b - 1] > sectors[b]; --b) std::swap(sectors[b - 1], sectors[b]);
        int gapAfter = n - 1, gap = sectors[0] + kSectors - sectors[n - 1];
        for (int k = 0; k + 1 < n; ++k)
            if (sectors[k + 1] - sectors[k] > gap) {
                gap = sectors[k + 1] - sectors[k];
                gapAfter = k;
            }
        const int first = sectors[(gapAfter + 1) % n], span = kSectors - gap;
        const int s0 = std::clamp(int(y0 / slice), 0, kSlices - 1), s1 = std::clamp(int(y1 / slice), 0, kSlices - 1);
        for (int sl = s0; sl <= s1; ++sl)
            for (int k = 0; k <= span; ++k) {
                float& cell = s.radius[sl][(first + k) % kSectors];
                cell = std::max(cell, r);
            }
        s.maxRadius = std::max(s.maxRadius, r);
    }
    return s;
}

float PieceSilhouette::intersect(vec3 o, vec3 d, float margin, float maxT) const {
    if (empty()) return -1.0f;
    // The bounding cylinder: radius maxRadius + margin, from the base to the top.
    const float R = maxRadius + margin;
    float t0 = 0.0f, t1 = maxT;
    const float a = d.x * d.x + d.z * d.z, b = o.x * d.x + o.z * d.z, c = o.x * o.x + o.z * o.z - R * R;
    if (a < 1e-12f) {
        if (c > 0.0f) return -1.0f;
    } else {
        const float disc = b * b - a * c;
        if (disc < 0.0f) return -1.0f;
        const float q = std::sqrt(disc);
        t0 = std::max(t0, (-b - q) / a);
        t1 = std::min(t1, (-b + q) / a);
    }
    if (std::fabs(d.y) < 1e-9f) {
        if (o.y < 0.0f || o.y > height) return -1.0f;
    } else {
        float ta = -o.y / d.y, tb = (height - o.y) / d.y;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
    }
    if (t1 < t0) return -1.0f;
    auto inside = [&](float t) {
        const vec3 p = o + d * t;
        if (p.y < 0.0f || p.y > height) return false;
        const float r = std::sqrt(p.x * p.x + p.z * p.z);
        if (r <= margin) return true;
        const int sl = std::clamp(int(p.y / height * float(kSlices)), 0, kSlices - 1);
        return r <= radius[sl][sectorOf(p.x, p.z)] + margin;
    };
    if (inside(t0)) return t0;
    // March through the cylinder (0.4 mm steps: thinner than any part of a piece), then refine.
    constexpr float kStep = 0.0004f;
    float prev = t0;
    for (float t = t0 + kStep;; t += kStep) {
        const bool last = t >= t1;
        if (last) t = t1;
        if (inside(t)) {
            float lo = prev, hi = t;
            for (int k = 0; k < 8; ++k) {
                const float mid = 0.5f * (lo + hi);
                if (inside(mid)) hi = mid;
                else lo = mid;
            }
            return hi;
        }
        if (last) break;
        prev = t;
    }
    return -1.0f;
}
