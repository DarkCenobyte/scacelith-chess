// Scacelith math library: vectors, matrices, quaternions, geometric helpers.
//
// Conventions (shared by every module, do not change):
//  * Units are meters, seconds, radians.
//  * Right-handed world, +Y up. The board is centred on the XZ origin (see game/layout.h).
//  * Matrices are column-major (m[column][row]) exactly as GLSL expects, so a mat4 can be
//    uploaded with glUniformMatrix4fv(..., GL_FALSE, &m.c[0].x) or copied into a std140 block.
//  * v' = M * v (column vectors). Transform composition: parent * child.
//  * Projection uses reverse-Z with a [0,1] clip range (glClipControl(GL_LOWER_LEFT,
//    GL_ZERO_TO_ONE)): depth 1 at the near plane, 0 at infinity. Depth test is GL_GREATER,
//    depth clears to 0.
#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <type_traits>

namespace m {

constexpr float PI = 3.14159265358979323846f;
constexpr float TAU = 6.28318530717958647692f;
constexpr float DEG = PI / 180.0f;

inline float clamp(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
inline float saturate(float x) { return clamp(x, 0.0f, 1.0f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstep(float a, float b, float x) { float t = saturate((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
inline float smootherstep(float t) { t = saturate(t); return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); } // minimum-jerk profile
inline float sign(float x) { return x < 0.0f ? -1.0f : 1.0f; }
inline float fract(float x) { return x - std::floor(x); }

struct vec2 {
    float x = 0, y = 0;
    vec2() = default;
    constexpr vec2(float x_, float y_) : x(x_), y(y_) {}
    explicit constexpr vec2(float s) : x(s), y(s) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};
struct vec3 {
    float x = 0, y = 0, z = 0;
    vec3() = default;
    constexpr vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit constexpr vec3(float s) : x(s), y(s), z(s) {}
    constexpr vec3(vec2 v, float z_) : x(v.x), y(v.y), z(z_) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
    vec2 xy() const { return {x, y}; }
    vec2 xz() const { return {x, z}; }
};
struct vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    vec4() = default;
    constexpr vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    explicit constexpr vec4(float s) : x(s), y(s), z(s), w(s) {}
    constexpr vec4(vec3 v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
    vec3 xyz() const { return {x, y, z}; }
};
// operator[] indexes the components from &x, and mat4::data() and the GL uploads read a matrix as
// one float array: these layouts are relied upon.
static_assert(sizeof(vec2) == 2 * sizeof(float) && std::is_standard_layout<vec2>::value, "vec2: 2 packed floats");
static_assert(sizeof(vec3) == 3 * sizeof(float) && std::is_standard_layout<vec3>::value, "vec3: 3 packed floats");
static_assert(sizeof(vec4) == 4 * sizeof(float) && std::is_standard_layout<vec4>::value, "vec4: 4 packed floats");
struct ivec2 { int x = 0, y = 0; ivec2() = default; constexpr ivec2(int a, int b) : x(a), y(b) {} };

#define M_VEC_OPS(T, N)                                                                          \
    inline T operator+(T a, T b) { T r; for (int i = 0; i < N; ++i) r[i] = a[i] + b[i]; return r; } \
    inline T operator-(T a, T b) { T r; for (int i = 0; i < N; ++i) r[i] = a[i] - b[i]; return r; } \
    inline T operator*(T a, T b) { T r; for (int i = 0; i < N; ++i) r[i] = a[i] * b[i]; return r; } \
    inline T operator/(T a, T b) { T r; for (int i = 0; i < N; ++i) r[i] = a[i] / b[i]; return r; } \
    inline T operator*(T a, float s) { T r; for (int i = 0; i < N; ++i) r[i] = a[i] * s; return r; } \
    inline T operator*(float s, T a) { return a * s; }                                           \
    inline T operator/(T a, float s) { return a * (1.0f / s); }                                  \
    inline T operator-(T a) { T r; for (int i = 0; i < N; ++i) r[i] = -a[i]; return r; }        \
    inline T& operator+=(T& a, T b) { a = a + b; return a; }                                     \
    inline T& operator-=(T& a, T b) { a = a - b; return a; }                                     \
    inline T& operator*=(T& a, T b) { a = a * b; return a; }                                     \
    inline T& operator*=(T& a, float s) { a = a * s; return a; }                                 \
    inline T& operator/=(T& a, float s) { a = a * (1.0f / s); return a; }                        \
    inline float dot(T a, T b) { float s = 0; for (int i = 0; i < N; ++i) s += a[i] * b[i]; return s; } \
    inline float length(T a) { return std::sqrt(dot(a, a)); }                                    \
    inline float length2(T a) { return dot(a, a); }                                              \
    inline T normalize(T a) { float l = length(a); return l > 1e-20f ? a / l : a; }              \
    inline T lerp(T a, T b, float t) { return a + (b - a) * t; }                                 \
    inline T min(T a, T b) { T r; for (int i = 0; i < N; ++i) r[i] = std::min(a[i], b[i]); return r; } \
    inline T max(T a, T b) { T r; for (int i = 0; i < N; ++i) r[i] = std::max(a[i], b[i]); return r; } \
    inline T abs(T a) { T r; for (int i = 0; i < N; ++i) r[i] = std::fabs(a[i]); return r; }     \
    inline T clamp(T a, float lo, float hi) { T r; for (int i = 0; i < N; ++i) r[i] = clamp(a[i], lo, hi); return r; } \
    inline bool operator==(T a, T b) { for (int i = 0; i < N; ++i) if (a[i] != b[i]) return false; return true; } \
    inline bool operator!=(T a, T b) { return !(a == b); }
M_VEC_OPS(vec2, 2)
M_VEC_OPS(vec3, 3)
M_VEC_OPS(vec4, 4)
#undef M_VEC_OPS

inline vec3 cross(vec3 a, vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float distance(vec3 a, vec3 b) { return length(a - b); }
inline vec3 reflect(vec3 i, vec3 n) { return i - n * (2.0f * dot(n, i)); }
// Any unit vector orthogonal to n.
inline vec3 orthogonal(vec3 n) {
    vec3 a = std::fabs(n.x) < 0.9f ? vec3(1, 0, 0) : vec3(0, 1, 0);
    return normalize(cross(n, a));
}

// ---------------------------------------------------------------------------------------------
struct mat3 {
    vec3 c[3];  // columns
    mat3() : c{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}} {}
    mat3(vec3 c0, vec3 c1, vec3 c2) : c{c0, c1, c2} {}
    vec3& operator[](int i) { return c[i]; }
    const vec3& operator[](int i) const { return c[i]; }
};
static_assert(sizeof(mat3) == 9 * sizeof(float) && std::is_standard_layout<mat3>::value, "mat3: 9 packed floats");
inline vec3 operator*(const mat3& m, vec3 v) { return m.c[0] * v.x + m.c[1] * v.y + m.c[2] * v.z; }
inline mat3 operator*(const mat3& a, const mat3& b) { return {a * b.c[0], a * b.c[1], a * b.c[2]}; }
inline mat3 transpose(const mat3& m) {
    return {{m.c[0].x, m.c[1].x, m.c[2].x}, {m.c[0].y, m.c[1].y, m.c[2].y}, {m.c[0].z, m.c[1].z, m.c[2].z}};
}
inline float determinant(const mat3& m) { return dot(m.c[0], cross(m.c[1], m.c[2])); }
inline mat3 inverse(const mat3& m) {
    vec3 r0 = cross(m.c[1], m.c[2]), r1 = cross(m.c[2], m.c[0]), r2 = cross(m.c[0], m.c[1]);
    float inv = 1.0f / dot(m.c[0], r0);
    return transpose(mat3(r0 * inv, r1 * inv, r2 * inv));
}

struct mat4 {
    vec4 c[4];  // columns
    mat4() : c{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}} {}
    mat4(vec4 c0, vec4 c1, vec4 c2, vec4 c3) : c{c0, c1, c2, c3} {}
    explicit mat4(const mat3& m, vec3 t = vec3(0)) : c{{m.c[0], 0}, {m.c[1], 0}, {m.c[2], 0}, {t, 1}} {}
    vec4& operator[](int i) { return c[i]; }
    const vec4& operator[](int i) const { return c[i]; }
    const float* data() const { return &c[0].x; }
    mat3 upper3() const { return {c[0].xyz(), c[1].xyz(), c[2].xyz()}; }
    vec3 translation() const { return c[3].xyz(); }
};
static_assert(sizeof(mat4) == 16 * sizeof(float) && std::is_standard_layout<mat4>::value, "mat4: 16 packed floats");
inline vec4 operator*(const mat4& m, vec4 v) { return m.c[0] * v.x + m.c[1] * v.y + m.c[2] * v.z + m.c[3] * v.w; }
inline mat4 operator*(const mat4& a, const mat4& b) { return {a * b.c[0], a * b.c[1], a * b.c[2], a * b.c[3]}; }
inline vec3 transformPoint(const mat4& m, vec3 p) { return (m * vec4(p, 1.0f)).xyz(); }
inline vec3 transformDir(const mat4& m, vec3 d) { return (m * vec4(d, 0.0f)).xyz(); }
inline vec3 projectPoint(const mat4& m, vec3 p) { vec4 r = m * vec4(p, 1.0f); return r.xyz() / r.w; }
inline mat4 transpose(const mat4& m) {
    mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.c[i][j] = m.c[j][i];
    return r;
}
mat4 inverse(const mat4& m);  // general inverse (math.cpp)
// Fast inverse for rigid/affine matrices (rotation+scale+translation, no projection).
inline mat4 inverseAffine(const mat4& m) {
    mat3 ri = inverse(m.upper3());
    return mat4(ri, -(ri * m.translation()));
}
inline mat3 normalMatrix(const mat4& m) { return transpose(inverse(m.upper3())); }

inline mat4 translate(vec3 t) { mat4 r; r.c[3] = vec4(t, 1); return r; }
inline mat4 scale(vec3 s) { mat4 r; r.c[0].x = s.x; r.c[1].y = s.y; r.c[2].z = s.z; return r; }
inline mat4 rotateAxis(vec3 axis, float angle) {
    axis = normalize(axis);
    float c = std::cos(angle), s = std::sin(angle), t = 1 - c;
    float x = axis.x, y = axis.y, z = axis.z;
    return mat4({t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0},
                {t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0},
                {t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0}, {0, 0, 0, 1});
}
inline mat4 rotateX(float a) { return rotateAxis({1, 0, 0}, a); }
inline mat4 rotateY(float a) { return rotateAxis({0, 1, 0}, a); }
inline mat4 rotateZ(float a) { return rotateAxis({0, 0, 1}, a); }
// Camera-to-world style basis looking from eye towards target. The returned matrix is the VIEW
// matrix (world -> view), view space looks down -Z.
inline mat4 lookAt(vec3 eye, vec3 target, vec3 up) {
    vec3 f = normalize(target - eye);
    vec3 s = normalize(cross(f, up));
    vec3 u = cross(s, f);
    return mat4({s.x, u.x, -f.x, 0}, {s.y, u.y, -f.y, 0}, {s.z, u.z, -f.z, 0},
                {-dot(s, eye), -dot(u, eye), dot(f, eye), 1});
}
// Reverse-Z infinite perspective, clip depth in [0,1] (1 = near plane). fovY in radians.
inline mat4 perspectiveReverseZ(float fovY, float aspect, float zNear) {
    float f = 1.0f / std::tan(fovY * 0.5f);
    return mat4({f / aspect, 0, 0, 0}, {0, f, 0, 0}, {0, 0, 0, -1}, {0, 0, zNear, 0});
}
// Reverse-Z finite perspective (for probes / reflections that want a far plane).
inline mat4 perspectiveReverseZ(float fovY, float aspect, float zNear, float zFar) {
    float f = 1.0f / std::tan(fovY * 0.5f);
    float a = zNear / (zFar - zNear);
    return mat4({f / aspect, 0, 0, 0}, {0, f, 0, 0}, {0, 0, a, -1}, {0, 0, zFar * a, 0});
}
// Orthographic with [0,1] clip depth, reverse-Z (near -> 1, far -> 0). View space looks down -Z.
inline mat4 orthoReverseZ(float l, float r, float b, float t, float zNear, float zFar) {
    return mat4({2 / (r - l), 0, 0, 0}, {0, 2 / (t - b), 0, 0}, {0, 0, 1 / (zFar - zNear), 0},
                {-(r + l) / (r - l), -(t + b) / (t - b), zFar / (zFar - zNear), 1});
}
// Orthographic with standard [0,1] depth (near -> 0, far -> 1). Used for shadow maps.
inline mat4 ortho01(float l, float r, float b, float t, float zNear, float zFar) {
    return mat4({2 / (r - l), 0, 0, 0}, {0, 2 / (t - b), 0, 0}, {0, 0, -1 / (zFar - zNear), 0},
                {-(r + l) / (r - l), -(t + b) / (t - b), -zNear / (zFar - zNear), 1});
}

// ---------------------------------------------------------------------------------------------
struct quat {
    float x = 0, y = 0, z = 0, w = 1;
    quat() = default;
    constexpr quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    vec3 v() const { return {x, y, z}; }
};
inline quat operator*(quat a, quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline float dot(quat a, quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline quat normalize(quat q) {
    float l = std::sqrt(dot(q, q));
    return l > 0 ? quat(q.x / l, q.y / l, q.z / l, q.w / l) : quat();
}
inline quat conjugate(quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline quat axisAngle(vec3 axis, float angle) {
    axis = normalize(axis);
    float s = std::sin(angle * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)};
}
inline vec3 rotate(quat q, vec3 v) {
    vec3 u = q.v();
    vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
inline quat nlerp(quat a, quat b, float t) {
    if (dot(a, b) < 0) b = {-b.x, -b.y, -b.z, -b.w};
    return normalize(quat(lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t)));
}
inline quat slerp(quat a, quat b, float t) {
    float d = dot(a, b);
    if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f) return nlerp(a, b, t);
    float th = std::acos(clamp(d, -1.0f, 1.0f));
    float s = std::sin(th);
    float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}
inline mat3 toMat3(quat q) {
    float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    float xx = q.x * x2, xy = q.x * y2, xz = q.x * z2, yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
    float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    return {{1 - (yy + zz), xy + wz, xz - wy}, {xy - wz, 1 - (xx + zz), yz + wx}, {xz + wy, yz - wx, 1 - (xx + yy)}};
}
inline mat4 toMat4(quat q, vec3 t = vec3(0)) { return mat4(toMat3(q), t); }
quat fromMat3(const mat3& m);  // math.cpp
// Shortest rotation taking unit vector a onto unit vector b.
quat fromTo(vec3 a, vec3 b);   // math.cpp
// Rotation whose local +Z maps to 'forward' and local +Y is as close as possible to 'up'.
quat lookRotation(vec3 forward, vec3 up);  // math.cpp

// ---------------------------------------------------------------------------------------------
struct AABB {
    vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    void add(vec3 p) { lo = min(lo, p); hi = max(hi, p); }
    void add(const AABB& b) { lo = min(lo, b.lo); hi = max(hi, b.hi); }
    bool valid() const { return lo.x <= hi.x; }
    vec3 center() const { return (lo + hi) * 0.5f; }
    vec3 extent() const { return (hi - lo) * 0.5f; }
};
AABB transformAABB(const AABB& b, const mat4& m);  // math.cpp

struct Ray { vec3 o, d; };  // d normalized
// Returns t >= 0 of the hit or a negative value when missed.
float rayPlane(const Ray& r, vec3 planePoint, vec3 planeNormal);
float rayAABB(const Ray& r, const AABB& b);
float raySphere(const Ray& r, vec3 c, float radius);
// Finite vertical (Y-axis) cylinder: base centre, radius, height.
float rayCylinderY(const Ray& r, vec3 base, float radius, float height);

// Small, fast, deterministic RNG (PCG32).
struct Rng {
    uint64_t state = 0x853c49e6748fea9bULL, inc = 0xda3e39cb94b95bdbULL;
    explicit Rng(uint64_t seed = 1) { seedWith(seed); }
    void seedWith(uint64_t seed) { state = 0; inc = (seed << 1u) | 1u; next(); state += seed; next(); }
    uint32_t next() {
        uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        uint32_t xs = uint32_t(((old >> 18u) ^ old) >> 27u), rot = uint32_t(old >> 59u);
        return (xs >> rot) | (xs << ((32 - rot) & 31));
    }
    float uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }  // [0,1)
    float range(float a, float b) { return a + (b - a) * uniform(); }
    int rangeInt(int a, int bInclusive) { return a + int(next() % uint32_t(bInclusive - a + 1)); }
};

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

}  // namespace m
