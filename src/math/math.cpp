#include "math.h"

namespace m {

mat4 inverse(const mat4& mm) {
    const float* m = mm.data();
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    mat4 r;
    if (std::fabs(det) < 1e-30f) return r;
    det = 1.0f / det;
    float* o = &r.c[0].x;
    for (int i = 0; i < 16; ++i) o[i] = inv[i] * det;
    return r;
}

quat fromMat3(const mat3& m) {
    float m00 = m.c[0].x, m11 = m.c[1].y, m22 = m.c[2].z;
    float tr = m00 + m11 + m22;
    quat q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2;
        q.w = 0.25f * s;
        q.x = (m.c[1].z - m.c[2].y) / s;
        q.y = (m.c[2].x - m.c[0].z) / s;
        q.z = (m.c[0].y - m.c[1].x) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        q.w = (m.c[1].z - m.c[2].y) / s;
        q.x = 0.25f * s;
        q.y = (m.c[1].x + m.c[0].y) / s;
        q.z = (m.c[2].x + m.c[0].z) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        q.w = (m.c[2].x - m.c[0].z) / s;
        q.x = (m.c[1].x + m.c[0].y) / s;
        q.y = 0.25f * s;
        q.z = (m.c[2].y + m.c[1].z) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        q.w = (m.c[0].y - m.c[1].x) / s;
        q.x = (m.c[2].x + m.c[0].z) / s;
        q.y = (m.c[2].y + m.c[1].z) / s;
        q.z = 0.25f * s;
    }
    return normalize(q);
}

quat fromTo(vec3 a, vec3 b) {
    float d = dot(a, b);
    if (d < -0.999999f) return axisAngle(orthogonal(a), PI);
    vec3 c = cross(a, b);
    return normalize(quat(c.x, c.y, c.z, 1.0f + d));
}

quat lookRotation(vec3 forward, vec3 up) {
    vec3 f = normalize(forward);
    vec3 r = cross(up, f);
    if (length2(r) < 1e-12f) r = orthogonal(f);
    r = normalize(r);
    vec3 u = cross(f, r);
    return fromMat3(mat3(r, u, f));
}

float rayPlane(const Ray& r, vec3 p, vec3 n) {
    float d = dot(r.d, n);
    if (std::fabs(d) < 1e-8f) return -1.0f;
    return dot(p - r.o, n) / d;
}

float rayAABB(const Ray& r, const AABB& b) {
    float t0 = 0.0f, t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        float inv = 1.0f / (std::fabs(r.d[i]) > 1e-12f ? r.d[i] : 1e-12f);
        float a = (b.lo[i] - r.o[i]) * inv, c = (b.hi[i] - r.o[i]) * inv;
        if (a > c) std::swap(a, c);
        t0 = std::max(t0, a);
        t1 = std::min(t1, c);
        if (t0 > t1) return -1.0f;
    }
    return t0;
}

float raySphere(const Ray& r, vec3 c, float radius) {
    vec3 oc = r.o - c;
    float b = dot(oc, r.d), cc = dot(oc, oc) - radius * radius;
    float h = b * b - cc;
    if (h < 0) return -1.0f;
    h = std::sqrt(h);
    float t = -b - h;
    if (t < 0) t = -b + h;
    return t;
}

float rayCylinderY(const Ray& r, vec3 base, float radius, float height) {
    float best = -1.0f;
    auto consider = [&](float t) { if (t >= 0 && (best < 0 || t < best)) best = t; };
    // side
    float ox = r.o.x - base.x, oz = r.o.z - base.z;
    float a = r.d.x * r.d.x + r.d.z * r.d.z;
    if (a > 1e-12f) {
        float b = ox * r.d.x + oz * r.d.z, c = ox * ox + oz * oz - radius * radius;
        float h = b * b - a * c;
        if (h >= 0) {
            h = std::sqrt(h);
            for (float t : {(-b - h) / a, (-b + h) / a}) {
                float y = r.o.y + r.d.y * t;
                if (y >= base.y && y <= base.y + height) consider(t);
            }
        }
    }
    // caps
    if (std::fabs(r.d.y) > 1e-12f) {
        for (float y : {base.y, base.y + height}) {
            float t = (y - r.o.y) / r.d.y;
            float px = r.o.x + r.d.x * t - base.x, pz = r.o.z + r.d.z * t - base.z;
            if (px * px + pz * pz <= radius * radius) consider(t);
        }
    }
    return best;
}

}  // namespace m
