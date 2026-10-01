// AVX2 + FMA kernels (and, in kernels_avxvnni.cpp, the same unit rebuilt with AVX-VNNI for the
// integer GEMM). Compiled with -mavx2 -mfma (CMakeLists.txt); only called when cpu.cpp has found
// both on the CPU. Rules of kernels_impl.h apply: no standard library, internal linkage only.
#include "kernels.h"
#include <immintrin.h>

#include "kernels_impl.h"

namespace {

struct VAvx2 {
    using R = __m256;
    using I = __m256i;
    using M = __m256;
    static constexpr int W = 8;
    static R zero() { return _mm256_setzero_ps(); }
    static R set1(float v) { return _mm256_set1_ps(v); }
    static R loadu(const float* p) { return _mm256_loadu_ps(p); }
    static R load(const float* p) { return _mm256_load_ps(p); }
    static void storeu(float* p, R v) { _mm256_storeu_ps(p, v); }
    static void store(float* p, R v) { _mm256_store_ps(p, v); }
    static float loadScalar(const float* p) {
        float v;
        __builtin_memcpy(&v, p, 4);
        return v;
    }
    static R add(R a, R b) { return _mm256_add_ps(a, b); }
    static R sub(R a, R b) { return _mm256_sub_ps(a, b); }
    static R mul(R a, R b) { return _mm256_mul_ps(a, b); }
    static R div(R a, R b) { return _mm256_div_ps(a, b); }
    static R fmadd(R a, R b, R c) { return _mm256_fmadd_ps(a, b, c); }
    static R max(R a, R b) { return _mm256_max_ps(a, b); }
    static R min(R a, R b) { return _mm256_min_ps(a, b); }
    static float hsum(R a) {
        __m128 s = _mm_add_ps(_mm256_castps256_ps128(a), _mm256_extractf128_ps(a, 1));
        s = _mm_add_ps(s, _mm_movehl_ps(s, s));
        s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
        return _mm_cvtss_f32(s);
    }
    static I cvtRound(R a) { return _mm256_cvtps_epi32(a); }
    static R cvtI2F(I a) { return _mm256_cvtepi32_ps(a); }
    static R pow2i(I n) {
        return _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_add_epi32(n, _mm256_set1_epi32(127)), 23));
    }
    static R signMask() { return _mm256_castsi256_ps(_mm256_set1_epi32(int(0x80000000u))); }
    static R abs(R a) { return _mm256_andnot_ps(signMask(), a); }
    static R copySign(R mag, R sgn) {
        return _mm256_or_ps(_mm256_andnot_ps(signMask(), mag), _mm256_and_ps(signMask(), sgn));
    }
    static M cmpGt(R a, R b) { return _mm256_cmp_ps(a, b, _CMP_GT_OQ); }
    static R select(M m, R a, R b) { return _mm256_blendv_ps(b, a, m); }
    static R cvtI8(const int8_t* p) {
        long long v;
        __builtin_memcpy(&v, p, 8);
        return _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_cvtsi64_si128(v)));
    }
    static float sqrtScalar(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
    static I izero() { return _mm256_setzero_si256(); }
    static I ibcast32(const int32_t* p) {
        int v;
        __builtin_memcpy(&v, p, 4);
        return _mm256_set1_epi32(v);
    }
    static I iload(const int32_t* p) { return _mm256_load_si256(reinterpret_cast<const __m256i*>(p)); }
    static void istore(int32_t* p, I v) { _mm256_store_si256(reinterpret_cast<__m256i*>(p), v); }
    static I iadd(I a, I b) { return _mm256_add_epi32(a, b); }
    static I madd16(I a, I b) { return _mm256_madd_epi16(a, b); }
    static I iset1(int v) { return _mm256_set1_epi32(v); }
    static I iloadu(const int32_t* p) { return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)); }
    static void istoreu(int32_t* p, I v) { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), v); }
    static R cvtU8(const uint8_t* p) {
        long long v;
        __builtin_memcpy(&v, p, 8);
        return _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_cvtsi64_si128(v)));
    }
    static void storeU8(uint8_t* p, I v) {
        __m128i w = _mm_packs_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
        _mm_storel_epi64(reinterpret_cast<__m128i*>(p), _mm_packus_epi16(w, w));
    }
#ifdef TTS_KERNELS_AVXVNNI
    static I dpbusd(I acc, I u8, I s8) { return _mm256_dpbusd_avx_epi32(acc, u8, s8); }
#endif
};

tts::kern::Table makeTable(const char* name, int level) {
    tts::kern::Table t;
    t.name = name;
    t.level = level;
    t.mr = 6;
    t.nr = 16;
    t.sgemmTile = kSgemmTile<VAvx2, 6, 2>;
    t.packAf32 = kPackAf32<VAvx2>;
    t.packAi8 = kPackAi8<VAvx2>;
    t.packBf32 = kPackBf32<VAvx2, 16>;
    t.imr = 6;
    t.inr = 16;
#ifdef TTS_KERNELS_AVXVNNI
    t.ik = 4;
    t.packIntA = kPackIntA8;
    t.packIntB = kPackIntB8<VAvx2, 16>;
    t.igemmTile = kIgemmTileVnni<VAvx2, 6, 2>;
#else
    t.ik = 2;
    t.packIntA = kPackIntA16;
    t.packIntB = kPackIntB16<VAvx2, 16>;
    t.igemmTile = kIgemmTile16<VAvx2, 6, 2>;
#endif
    t.quantizeU8 = kQuantizeU8<VAvx2>;
    t.dequantizeU8 = kDequantizeU8<VAvx2>;
    t.requantizeU8 = kRequantizeU8<VAvx2>;
    t.erf = kErf<VAvx2>;
    t.gelu = kGelu<VAvx2>;
    t.exp = kExp<VAvx2>;
    t.tanh = kTanh<VAvx2>;
    t.scaleShift = kScaleShift<VAvx2>;
    t.dwconv = kDwconv<VAvx2>;
    t.layerNormRows = kLayerNormRows<VAvx2>;
    t.softmaxRows = kSoftmaxRows<VAvx2>;
    return t;
}

}  // namespace

namespace tts {
namespace kern {
#ifdef TTS_KERNELS_AVXVNNI
const Table* tableAvxVnni() {
    static const Table t = makeTable("avxvnni", kAvxVnni);
    return &t;
}
#else
const Table* tableAvx2() {
    static const Table t = makeTable("avx2", kAvx2);
    return &t;
}
#endif
}  // namespace kern
}  // namespace tts
