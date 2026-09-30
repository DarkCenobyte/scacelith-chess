// AVX-512 kernels (F, BW, VL, DQ and VNNI: Ice Lake, Sapphire Rapids, Zen 4 and later). Compiled
// with those flags plus -mfma (CMakeLists.txt); only called when cpu.cpp has found all of them.
// Rules of kernels_impl.h apply: no standard library, internal linkage only.
#include "kernels.h"
#include <immintrin.h>

#include "kernels_impl.h"

namespace {

struct V512 {
    using R = __m512;
    using I = __m512i;
    using M = __mmask16;
    static constexpr int W = 16;
    static R zero() { return _mm512_setzero_ps(); }
    static R set1(float v) { return _mm512_set1_ps(v); }
    static R loadu(const float* p) { return _mm512_loadu_ps(p); }
    static R load(const float* p) { return _mm512_load_ps(p); }
    static void storeu(float* p, R v) { _mm512_storeu_ps(p, v); }
    static void store(float* p, R v) { _mm512_store_ps(p, v); }
    static float loadScalar(const float* p) {
        float v;
        __builtin_memcpy(&v, p, 4);
        return v;
    }
    static R add(R a, R b) { return _mm512_add_ps(a, b); }
    static R sub(R a, R b) { return _mm512_sub_ps(a, b); }
    static R mul(R a, R b) { return _mm512_mul_ps(a, b); }
    static R div(R a, R b) { return _mm512_div_ps(a, b); }
    static R fmadd(R a, R b, R c) { return _mm512_fmadd_ps(a, b, c); }
    static R max(R a, R b) { return _mm512_max_ps(a, b); }
    static R min(R a, R b) { return _mm512_min_ps(a, b); }
    static float hsum(R a) {
        __m256 h = _mm256_add_ps(_mm512_castps512_ps256(a),
                                 _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(a), 1)));
        __m128 s = _mm_add_ps(_mm256_castps256_ps128(h), _mm256_extractf128_ps(h, 1));
        s = _mm_add_ps(s, _mm_movehl_ps(s, s));
        s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
        return _mm_cvtss_f32(s);
    }
    static I cvtRound(R a) { return _mm512_cvtps_epi32(a); }
    static R cvtI2F(I a) { return _mm512_cvtepi32_ps(a); }
    static R pow2i(I n) {
        return _mm512_castsi512_ps(_mm512_slli_epi32(_mm512_add_epi32(n, _mm512_set1_epi32(127)), 23));
    }
    static R abs(R a) {
        return _mm512_castsi512_ps(_mm512_and_si512(_mm512_castps_si512(a), _mm512_set1_epi32(0x7fffffff)));
    }
    static R copySign(R mag, R sgn) {
        __m512i m = _mm512_set1_epi32(int(0x80000000u));
        return _mm512_castsi512_ps(_mm512_or_si512(_mm512_andnot_si512(m, _mm512_castps_si512(mag)),
                                                   _mm512_and_si512(m, _mm512_castps_si512(sgn))));
    }
    static M cmpGt(R a, R b) { return _mm512_cmp_ps_mask(a, b, _CMP_GT_OQ); }
    static R select(M m, R a, R b) { return _mm512_mask_blend_ps(m, b, a); }
    static R cvtI8(const int8_t* p) {
        return _mm512_cvtepi32_ps(_mm512_cvtepi8_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))));
    }
    static float sqrtScalar(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
    static I izero() { return _mm512_setzero_si512(); }
    static I ibcast32(const int32_t* p) {
        int v;
        __builtin_memcpy(&v, p, 4);
        return _mm512_set1_epi32(v);
    }
    static I iload(const int32_t* p) { return _mm512_load_si512(p); }
    static void istore(int32_t* p, I v) { _mm512_store_si512(p, v); }
    static I iadd(I a, I b) { return _mm512_add_epi32(a, b); }
    static I madd16(I a, I b) { return _mm512_madd_epi16(a, b); }
    static I dpbusd(I acc, I u8, I s8) { return _mm512_dpbusd_epi32(acc, u8, s8); }
    static I iset1(int v) { return _mm512_set1_epi32(v); }
    static I iloadu(const int32_t* p) { return _mm512_loadu_si512(p); }
    static void istoreu(int32_t* p, I v) { _mm512_storeu_si512(p, v); }
    static R cvtU8(const uint8_t* p) {
        return _mm512_cvtepi32_ps(_mm512_cvtepu8_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))));
    }
    static void storeU8(uint8_t* p, I v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm512_cvtepi32_epi8(v)); }
};

tts::kern::Table makeTable() {
    tts::kern::Table t;
    t.name = "avx512";
    t.level = tts::kern::kAvx512;
    t.mr = 12;
    t.nr = 32;
    t.sgemmTile = kSgemmTile<V512, 12, 2>;
    t.packAf32 = kPackAf32<V512>;
    t.packAi8 = kPackAi8<V512>;
    t.packBf32 = kPackBf32<V512, 32>;
    t.imr = 12;
    t.inr = 32;
    t.ik = 4;
    t.packIntA = kPackIntA8;
    t.packIntB = kPackIntB8<V512, 32>;
    t.igemmTile = kIgemmTileVnni<V512, 12, 2>;
    t.quantizeU8 = kQuantizeU8<V512>;
    t.dequantizeU8 = kDequantizeU8<V512>;
    t.requantizeU8 = kRequantizeU8<V512>;
    t.erf = kErf<V512>;
    t.gelu = kGelu<V512>;
    t.exp = kExp<V512>;
    t.tanh = kTanh<V512>;
    t.scaleShift = kScaleShift<V512>;
    t.dwconv = kDwconv<V512>;
    t.layerNormRows = kLayerNormRows<V512>;
    t.softmaxRows = kSoftmaxRows<V512>;
    return t;
}

}  // namespace

namespace tts {
namespace kern {
const Table* tableAvx512() {
    static const Table t = makeTable();
    return &t;
}
}  // namespace kern
}  // namespace tts
