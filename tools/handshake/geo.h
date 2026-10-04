// Scratch (not in the repo): exact meshes and SDFs of the robot's right arm (upper arm, forearm,
// hand, thumb, fingers), SDF grids for speed, and the C2-symmetric handshake clasp geometry.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "math/math.h"
#define private public
#include "anim/animator.cpp"
#include "anim/animator_gesture.cpp"
#include "anim/animator_writing.cpp"
#undef private
#include "character/robot_hand.cpp"

namespace plat { double time() { return 0.0; } }

using namespace m;
using namespace character;
using namespace anim::detail;

#include "handsdf.h"

// The right arm's bones with geometry, in skeleton order.
static const int kArmBones[] = {UpperArmR, ForeArmR, HandR, ThumbR1, ThumbR2, ThumbR3, IndexR1, IndexR2, IndexR3,
                                MiddleR1, MiddleR2, MiddleR3, RingR1, RingR2, RingR3, PinkyR1, PinkyR2, PinkyR3};
constexpr int kNArm = 18;
static int armIndex(int bone) {
    for (int i = 0; i < kNArm; ++i)
        if (kArmBones[i] == bone) return i;
    return -1;
}
static bool isHandBone(int bone) { return bone >= HandR && bone <= PinkyR3; }

struct Grid {
    vec3 lo;
    float h = 0.001f;
    int nx = 0, ny = 0, nz = 0;
    std::vector<float> d;
    float at(int i, int j, int k) const { return d[(size_t(k) * ny + j) * nx + i]; }
    float sample(vec3 p) const {
        vec3 g = (p - lo) / h;
        float out = 0.0f;
        vec3 gc(clamp(g.x, 0.0f, float(nx - 1) - 1e-3f), clamp(g.y, 0.0f, float(ny - 1) - 1e-3f), clamp(g.z, 0.0f, float(nz - 1) - 1e-3f));
        if (gc.x != g.x || gc.y != g.y || gc.z != g.z) out = length((g - gc) * h);
        int i = int(gc.x), j = int(gc.y), k = int(gc.z);
        float fx = gc.x - i, fy = gc.y - j, fz = gc.z - k;
        float c00 = at(i, j, k) * (1 - fx) + at(i + 1, j, k) * fx;
        float c10 = at(i, j + 1, k) * (1 - fx) + at(i + 1, j + 1, k) * fx;
        float c01 = at(i, j, k + 1) * (1 - fx) + at(i + 1, j, k + 1) * fx;
        float c11 = at(i, j + 1, k + 1) * (1 - fx) + at(i + 1, j + 1, k + 1) * fx;
        float c0 = c00 * (1 - fy) + c10 * fy, c1 = c01 * (1 - fy) + c11 * fy;
        return c0 * (1 - fz) + c1 * fz + out;
    }
};

struct ArmGeo {
    probe::HandSdf H;
    std::vector<vec3> verts[kNArm];    // all mesh vertices (bone-local)
    std::vector<vec3> dverts[kNArm];   // decimated
    vec3 bc[kNArm];                    // bounding sphere (bone-local)
    float br[kNArm];
    Grid grid[kNArm];
    bool useGrid = true;
    float exact(int ai, vec3 p) const { return H.bone(kArmBones[ai], p); }
    float sdf(int ai, vec3 p) const {
        if (useGrid) return grid[ai].sample(p);
        return exact(ai, p);
    }

    void build(bool grids, float decim = 0.0015f) {
        build::Sink s;
        build::buildHand(s);
        build::buildArm(s);
        s.run(4);
        for (size_t i = 0; i < s.parts.size(); ++i) {
            if (s.mirrorOf[i] >= 0) continue;
            int ai = armIndex(s.parts[i].bone);
            if (ai < 0) continue;
            for (auto& v : s.parts[i].mesh.vertices) verts[ai].push_back(v.pos);
        }
        for (int ai = 0; ai < kNArm; ++ai) {
            vec3 lo(1e9f), hi(-1e9f);
            for (auto& v : verts[ai]) { lo = min(lo, v); hi = max(hi, v); }
            bc[ai] = (lo + hi) * 0.5f;
            br[ai] = 0;
            for (auto& v : verts[ai]) br[ai] = std::max(br[ai], length(v - bc[ai]));
            // Decimate: one vertex per cell.
            std::map<std::array<int, 3>, int> seen;
            for (auto& v : verts[ai]) {
                std::array<int, 3> key{int(std::floor(v.x / decim)), int(std::floor(v.y / decim)), int(std::floor(v.z / decim))};
                if (seen.emplace(key, 1).second) dverts[ai].push_back(v);
            }
            if (!grids) continue;
            Grid& g = grid[ai];
            g.h = ai <= 1 ? 0.0015f : 0.0006f;   // arm / hand
            const float margin = 0.010f;
            g.lo = lo - vec3(margin);
            vec3 ext = hi - lo + vec3(2 * margin);
            g.nx = int(ext.x / g.h) + 2;
            g.ny = int(ext.y / g.h) + 2;
            g.nz = int(ext.z / g.h) + 2;
            g.d.resize(size_t(g.nx) * g.ny * g.nz);
            const int nThreads = 4;
            std::vector<std::thread> th;
            for (int t = 0; t < nThreads; ++t)
                th.emplace_back([&, t] {
                    for (int k = t; k < g.nz; k += nThreads)
                        for (int j = 0; j < g.ny; ++j)
                            for (int i = 0; i < g.nx; ++i)
                                g.d[(size_t(k) * g.ny + j) * g.nx + i] = exact(ai, g.lo + vec3(float(i), float(j), float(k)) * g.h);
                });
            for (auto& x : th) x.join();
        }
    }
    void save(const char* path) const {
        FILE* f = std::fopen(path, "wb");
        for (int ai = 0; ai < kNArm; ++ai) {
            const Grid& g = grid[ai];
            int n[3] = {g.nx, g.ny, g.nz};
            std::fwrite(n, sizeof n, 1, f);
            std::fwrite(&g.lo, sizeof(vec3), 1, f);
            std::fwrite(&g.h, sizeof(float), 1, f);
            std::fwrite(g.d.data(), sizeof(float), g.d.size(), f);
        }
        std::fclose(f);
    }
    bool load(const char* path) {
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        for (int ai = 0; ai < kNArm; ++ai) {
            Grid& g = grid[ai];
            int n[3];
            if (std::fread(n, sizeof n, 1, f) != 1) { std::fclose(f); return false; }
            g.nx = n[0]; g.ny = n[1]; g.nz = n[2];
            if (std::fread(&g.lo, sizeof(vec3), 1, f) != 1) { std::fclose(f); return false; }
            if (std::fread(&g.h, sizeof(float), 1, f) != 1) { std::fclose(f); return false; }
            g.d.resize(size_t(g.nx) * g.ny * g.nz);
            if (std::fread(g.d.data(), sizeof(float), g.d.size(), f) != g.d.size()) { std::fclose(f); return false; }
        }
        std::fclose(f);
        return true;
    }
    // Meshes (and decimation) always rebuilt; grids loaded from a cache file when present.
    void init(const char* cache = nullptr) {
        // $HS_WORK/grids.bin (tools/handshake/env.sh), else ./grids.bin.
        const char* work = std::getenv("HS_WORK");
        const std::string def = std::string(work ? work : ".") + "/grids.bin";
        if (!cache) cache = def.c_str();
        build(false);
        if (load(cache)) return;
        build(true);
        save(cache);
    }
};

// Bone frames of one arm pose (any space). 'frame[ai]' = the bone's matrix.
struct ArmFrames {
    mat4 frame[kNArm];
};

struct PenResult {
    float depth = 0;      // deepest vertex inside the other arm (m, >= 0)
    float soft = 0;       // sum of squared depths (+ margin) over the vertices
    int nIn = 0;
    int aBone = -1, bBone = -1;
};

// Vertices of arm A (frames FA) inside arm B (frames FB); both in the same space.
// 'onlyHand': A's upper arm skipped (far from everything).
inline PenResult penetrate(const ArmGeo& G, const ArmFrames& FA, const ArmFrames& FB, bool decimated, float margin = 0.0f,
                           bool aSkipUpper = true) {
    PenResult r;
    vec3 cB[kNArm];
    mat4 invB[kNArm];
    for (int b = 0; b < kNArm; ++b) {
        cB[b] = transformPoint(FB.frame[b], G.bc[b]);
        invB[b] = inverseAffine(FB.frame[b]);
    }
    for (int a = aSkipUpper ? 1 : 0; a < kNArm; ++a) {
        vec3 cA = transformPoint(FA.frame[a], G.bc[a]);
        const std::vector<vec3>& V = decimated ? G.dverts[a] : G.verts[a];
        for (int b = 0; b < kNArm; ++b) {
            if (length(cA - cB[b]) > G.br[a] + G.br[b] + 0.002f) continue;
            mat4 M = invB[b] * FA.frame[a];
            // Per-vertex cull against B's bounding sphere.
            vec3 cbInA = transformPoint(inverseAffine(M), G.bc[b]);
            float rb = G.br[b] + margin + 0.0005f;
            for (const vec3& v : V) {
                if (length2(v - cbInA) > rb * rb) continue;
                float d = G.sdf(b, transformPoint(M, v));
                if (d < margin) {
                    float e = margin - d;
                    r.soft += e * e;
                }
                if (d < -0.0002f) ++r.nIn;
                if (-d > r.depth) {
                    r.depth = -d;
                    r.aBone = kArmBones[a];
                    r.bBone = kArmBones[b];
                }
            }
        }
    }
    return r;
}
// Signed distance from point p to arm B (min over its bones; hand bones only when handOnly).
inline float armDist(const ArmGeo& G, const ArmFrames& FB, vec3 p, bool handOnly = false, int* which = nullptr) {
    float best = 1e9f;
    for (int b = handOnly ? 2 : 0; b < kNArm; ++b) {
        vec3 c = transformPoint(FB.frame[b], G.bc[b]);
        float lb = length(p - c) - G.br[b];
        if (lb > best) continue;
        float d = G.sdf(b, transformPoint(inverseAffine(FB.frame[b]), p));
        if (d < best) {
            best = d;
            if (which) *which = kArmBones[b];
        }
    }
    return best;
}

// Hand-relative frames of the fingers for pose fp (right hand convention).
inline void handFrames(const Skeleton& sk, const FingerPose& fp, ArmFrames& F, const mat4& foreRel, const mat4& upperRel) {
    F.frame[0] = upperRel;
    F.frame[1] = foreRel;
    F.frame[2] = mat4();
    for (int f = 0; f < 5; ++f) {
        mat4 fr[3];
        fingerFrames(sk, Side::Right, fp, f, fr);
        for (int j = 0; j < 3; ++j) F.frame[3 + f * 3 + j] = fr[j];
    }
}
inline mat4 frameMul(const mat4& T, const mat4& F) { return T * F; }
// C2 image (the partner) in A's hand frame: rotation by pi about the hand-local vertical v through c.
inline mat4 c2Transform(vec3 v, vec3 c) {
    mat4 R;
    vec3 cols[3];
    for (int i = 0; i < 3; ++i) {
        vec3 e(i == 0, i == 1, i == 2);
        cols[i] = v * (2.0f * dot(v, e)) - e;
    }
    R.c[0] = vec4(cols[0], 0);
    R.c[1] = vec4(cols[1], 0);
    R.c[2] = vec4(cols[2], 0);
    vec3 t = c - transformDir(R, c);
    R.c[3] = vec4(t, 1);
    return R;
}
inline float segSegDist(vec3 p1, vec3 q1, vec3 p2, vec3 q2) {
    vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float s, t;
    float c = dot(d1, r), b = dot(d1, d2), den = a * e - b * b;
    s = den > 1e-12f ? clamp((b * f - c * e) / den, 0.0f, 1.0f) : 0.0f;
    t = (b * s + f) / e;
    if (t < 0) { t = 0; s = clamp(-c / a, 0.0f, 1.0f); }
    else if (t > 1) { t = 1; s = clamp((b - c) / a, 0.0f, 1.0f); }
    return length((p1 + d1 * s) - (p2 + d2 * t));
}
