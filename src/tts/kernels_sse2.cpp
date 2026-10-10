// Baseline kernels of x86-64: the scalar reference table (plain C++, one float per "vector"; used
// by the tests as the reference of every other level; kernels_scalar.h) and the SSE2 table (every
// x86-64 CPU). Compiled with the project's normal flags.
#include "kernels.h"
#include <cmath>
#include <cstring>
#include <emmintrin.h>

#include "kernels_scalar.h"

namespace {

struct VSse2 {
    using R = __m128;
    using I = __m128i;
    using M = __m128;
    static constexpr int W = 4;
    static R zero() { return _mm_setzero_ps(); }
    static R set1(float v) { return _mm_set1_ps(v); }
    static R loadu(const float* p) { return _mm_loadu_ps(p); }
    static R load(const float* p) { return _mm_load_ps(p); }
    static void storeu(float* p, R v) { _mm_storeu_ps(p, v); }
    static void store(float* p, R v) { _mm_store_ps(p, v); }
    static float loadScalar(const float* p) {
        float v;
        std::memcpy(&v, p, 4);
        return v;
    }
    static R add(R a, R b) { return _mm_add_ps(a, b); }
    static R sub(R a, R b) { return _mm_sub_ps(a, b); }
    static R mul(R a, R b) { return _mm_mul_ps(a, b); }
    static R div(R a, R b) { return _mm_div_ps(a, b); }
    static R fmadd(R a, R b, R c) { return _mm_add_ps(_mm_mul_ps(a, b), c); }
    static R max(R a, R b) { return _mm_max_ps(a, b); }
    static R min(R a, R b) { return _mm_min_ps(a, b); }
    static float hsum(R a) {
        R s = _mm_add_ps(a, _mm_movehl_ps(a, a));
        s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
        return _mm_cvtss_f32(s);
    }
    static I cvtRound(R a) { return _mm_cvtps_epi32(a); }
    static R cvtI2F(I a) { return _mm_cvtepi32_ps(a); }
    static R pow2i(I n) { return _mm_castsi128_ps(_mm_slli_epi32(_mm_add_epi32(n, _mm_set1_epi32(127)), 23)); }
    static R signMask() { return _mm_castsi128_ps(_mm_set1_epi32(int(0x80000000u))); }
    static R abs(R a) { return _mm_andnot_ps(signMask(), a); }
    static R copySign(R mag, R sgn) { return _mm_or_ps(_mm_andnot_ps(signMask(), mag), _mm_and_ps(signMask(), sgn)); }
    static M cmpGt(R a, R b) { return _mm_cmpgt_ps(a, b); }
    static R select(M m, R a, R b) { return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b)); }
    static R cvtI8(const int8_t* p) {
        int32_t v;
        std::memcpy(&v, p, 4);
        __m128i b = _mm_cvtsi32_si128(v);
        b = _mm_unpacklo_epi8(b, b);
        b = _mm_unpacklo_epi16(b, b);
        return _mm_cvtepi32_ps(_mm_srai_epi32(b, 24));
    }
    static float sqrtScalar(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
    static I izero() { return _mm_setzero_si128(); }
    static I ibcast32(const int32_t* p) {
        int32_t v;
        std::memcpy(&v, p, 4);
        return _mm_set1_epi32(v);
    }
    static I iload(const int32_t* p) { return _mm_load_si128(reinterpret_cast<const __m128i*>(p)); }
    static void istore(int32_t* p, I v) { _mm_store_si128(reinterpret_cast<__m128i*>(p), v); }
    static I iadd(I a, I b) { return _mm_add_epi32(a, b); }
    static I madd16(I a, I b) { return _mm_madd_epi16(a, b); }
    static I iset1(int v) { return _mm_set1_epi32(v); }
    static I iloadu(const int32_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }
    static void istoreu(int32_t* p, I v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v); }
    static R cvtU8(const uint8_t* p) {
        int32_t v;
        std::memcpy(&v, p, 4);
        __m128i z = _mm_setzero_si128();
        return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(v), z), z));
    }
    static void storeU8(uint8_t* p, I v) {
        __m128i b = _mm_packus_epi16(_mm_packs_epi32(v, v), _mm_setzero_si128());
        int32_t w = _mm_cvtsi128_si32(b);
        std::memcpy(p, &w, 4);
    }
};

}  // namespace

namespace tts {
namespace kern {
const Table* tableScalar() {
    static const Table t = makeTable<VScalar, 4, 4>("scalar", kScalar);
    return &t;
}
const Table* tableSse2() {
    static const Table t = makeTable<VSse2, 6, 2>("sse2", kSse2);
    return &t;
}
}  // namespace kern
}  // namespace tts
