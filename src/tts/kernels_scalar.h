// Baseline kernels shared by the baseline unit of each architecture (kernels_sse2.cpp on x86-64,
// kernels_neon.cpp on aarch64): the scalar reference trait (plain C++, one float per "vector";
// its table is the reference of every other level in the tests) and the table of a vector trait
// with the 16-bit integer GEMM path. Only those units include it: they are compiled with the
// project's normal flags and may use the standard library.
#pragma once
#include "kernels.h"
#include <cmath>
#include <cstring>

#include "kernels_impl.h"

namespace {

struct VScalar {
    using R = float;
    using I = int32_t;
    using M = bool;
    static constexpr int W = 1;
    static R zero() { return 0.0f; }
    static R set1(float v) { return v; }
    static R loadu(const float* p) { return loadScalar(p); }
    static R load(const float* p) { return *p; }
    static void storeu(float* p, R v) { std::memcpy(p, &v, 4); }
    static void store(float* p, R v) { *p = v; }
    static float loadScalar(const float* p) {
        float v;
        std::memcpy(&v, p, 4);
        return v;
    }
    static R add(R a, R b) { return a + b; }
    static R sub(R a, R b) { return a - b; }
    static R mul(R a, R b) { return a * b; }
    static R div(R a, R b) { return a / b; }
    static R fmadd(R a, R b, R c) { return a * b + c; }
    static R max(R a, R b) { return a > b ? a : b; }
    static R min(R a, R b) { return a < b ? a : b; }
    static float hsum(R a) { return a; }
    static I cvtRound(R a) { return int32_t(std::nearbyint(a)); }
    static R cvtI2F(I a) { return float(a); }
    static R pow2i(I n) {
        uint32_t bits = uint32_t(n + 127) << 23;
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    static R abs(R a) { return std::fabs(a); }
    static R copySign(R mag, R sgn) { return std::copysign(mag, sgn); }
    static M cmpGt(R a, R b) { return a > b; }
    static R select(M m, R a, R b) { return m ? a : b; }
    static R cvtI8(const int8_t* p) { return float(*p); }
    static float sqrtScalar(float x) { return std::sqrt(x); }
    static I izero() { return 0; }
    static I ibcast32(const int32_t* p) {
        int32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    static I iload(const int32_t* p) { return *p; }
    static void istore(int32_t* p, I v) { *p = v; }
    static I iadd(I a, I b) { return a + b; }
    static I iset1(int v) { return v; }
    static I iloadu(const int32_t* p) {
        int32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    static void istoreu(int32_t* p, I v) { std::memcpy(p, &v, 4); }
    static R cvtU8(const uint8_t* p) { return float(*p); }
    static void storeU8(uint8_t* p, I v) { *p = uint8_t(v); }
    static I madd16(I a, I b) {
        int16_t x[2], y[2];
        std::memcpy(x, &a, 4);
        std::memcpy(y, &b, 4);
        return int32_t(x[0]) * y[0] + int32_t(x[1]) * y[1];
    }
};

template <class V, int MR, int NV>
tts::kern::Table makeTable(const char* name, int level) {
    tts::kern::Table t;
    t.name = name;
    t.level = level;
    t.mr = MR;
    t.nr = NV * V::W;
    t.sgemmTile = kSgemmTile<V, MR, NV>;
    t.packAf32 = kPackAf32<V>;
    t.packAi8 = kPackAi8<V>;
    t.packBf32 = kPackBf32<V, NV * V::W>;
    t.imr = MR;
    t.inr = NV * V::W;
    t.ik = 2;
    t.packIntA = kPackIntA16;
    t.packIntB = kPackIntB16<V, NV * V::W>;
    t.igemmTile = kIgemmTile16<V, MR, NV>;
    t.quantizeU8 = kQuantizeU8<V>;
    t.dequantizeU8 = kDequantizeU8<V>;
    t.erf = kErf<V>;
    t.gelu = kGelu<V>;
    t.exp = kExp<V>;
    t.tanh = kTanh<V>;
    t.dwconv = kDwconv<V>;
    t.layerNormRows = kLayerNormRows<V>;
    t.softmaxRows = kSoftmaxRows<V>;
    return t;
}

}  // namespace
