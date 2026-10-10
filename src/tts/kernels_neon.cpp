// Baseline kernels of aarch64 (Linux, macOS): the scalar reference table (kernels_scalar.h, as on
// x86-64) and the NEON table. Advanced SIMD is part of every ARMv8-A CPU, so this unit is compiled
// with the project's normal flags like the rest of the program and its table needs no check at run
// time (cpu.cpp). Four floats per register as SSE2, with fused multiply-adds (fmla) as AVX2 and
// AVX-512, and the 16-bit integer GEMM path: the dot-product instructions of ARMv8.2 multiply
// signed by signed or unsigned by unsigned bytes only (u8 x s8 needs the later I8MM), and are
// optional.
//
// Every operation gives the bits of its SSE2 counterpart where the shared kernels rely on them:
// max / min return the second operand when either is NaN (maxps / minps, the scalar table's
// a > b ? a : b), so a NaN never reaches the float-to-int conversions (quantization, exp), which
// round to nearest even like cvtps2dq.
#include "kernels.h"
#include <arm_neon.h>
#include <cmath>
#include <cstring>

#include "kernels_scalar.h"

namespace {

struct VNeon {
    using R = float32x4_t;
    using I = int32x4_t;
    using M = uint32x4_t;
    static constexpr int W = 4;
    static R zero() { return vdupq_n_f32(0.0f); }
    static R set1(float v) { return vdupq_n_f32(v); }
    static R loadu(const float* p) { return vld1q_f32(p); }
    static R load(const float* p) { return vld1q_f32(p); }
    static void storeu(float* p, R v) { vst1q_f32(p, v); }
    static void store(float* p, R v) { vst1q_f32(p, v); }
    static float loadScalar(const float* p) {
        float v;
        std::memcpy(&v, p, 4);
        return v;
    }
    static R add(R a, R b) { return vaddq_f32(a, b); }
    static R sub(R a, R b) { return vsubq_f32(a, b); }
    static R mul(R a, R b) { return vmulq_f32(a, b); }
    static R div(R a, R b) { return vdivq_f32(a, b); }
    static R fmadd(R a, R b, R c) { return vfmaq_f32(c, a, b); }
    static R max(R a, R b) { return vbslq_f32(vcgtq_f32(a, b), a, b); }
    static R min(R a, R b) { return vbslq_f32(vcltq_f32(a, b), a, b); }
    static float hsum(R a) { return vaddvq_f32(a); }
    static I cvtRound(R a) { return vcvtnq_s32_f32(a); }
    static R cvtI2F(I a) { return vcvtq_f32_s32(a); }
    static R pow2i(I n) { return vreinterpretq_f32_s32(vshlq_n_s32(vaddq_s32(n, vdupq_n_s32(127)), 23)); }
    static R abs(R a) { return vabsq_f32(a); }
    static R copySign(R mag, R sgn) { return vbslq_f32(vdupq_n_u32(0x80000000u), sgn, mag); }
    static M cmpGt(R a, R b) { return vcgtq_f32(a, b); }
    static R select(M m, R a, R b) { return vbslq_f32(m, a, b); }
    static R cvtI8(const int8_t* p) {
        int32_t v;
        std::memcpy(&v, p, 4);
        int16x8_t h = vmovl_s8(vreinterpret_s8_s32(vdup_n_s32(v)));
        return vcvtq_f32_s32(vmovl_s16(vget_low_s16(h)));
    }
    static float sqrtScalar(float x) { return vget_lane_f32(vsqrt_f32(vdup_n_f32(x)), 0); }
    static I izero() { return vdupq_n_s32(0); }
    static I ibcast32(const int32_t* p) {
        int32_t v;
        std::memcpy(&v, p, 4);
        return vdupq_n_s32(v);
    }
    static I iload(const int32_t* p) { return vld1q_s32(p); }
    static void istore(int32_t* p, I v) { vst1q_s32(p, v); }
    static I iadd(I a, I b) { return vaddq_s32(a, b); }
    // pmaddwd: the eight int16 products, added in adjacent pairs.
    static I madd16(I a, I b) {
        int16x8_t x = vreinterpretq_s16_s32(a), y = vreinterpretq_s16_s32(b);
        return vpaddq_s32(vmull_s16(vget_low_s16(x), vget_low_s16(y)), vmull_high_s16(x, y));
    }
    static I iset1(int v) { return vdupq_n_s32(v); }
    static I iloadu(const int32_t* p) { return vld1q_s32(p); }
    static void istoreu(int32_t* p, I v) { vst1q_s32(p, v); }
    static R cvtU8(const uint8_t* p) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        uint16x8_t h = vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(v)));
        return vcvtq_f32_u32(vmovl_u16(vget_low_u16(h)));
    }
    // packssdw then packuswb, as SSE2: saturated to 0..255.
    static void storeU8(uint8_t* p, I v) {
        int16x4_t h = vqmovn_s32(v);
        uint8x8_t b = vqmovun_s16(vcombine_s16(h, h));
        uint32_t w = vget_lane_u32(vreinterpret_u32_u8(b), 0);
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
// 6 rows x 16 columns: 24 accumulators, 4 columns of B and the broadcast row of A in AArch64's 32
// vector registers (the 16-bit integer path needs a multiple of 8 columns).
const Table* tableNeon() {
    static const Table t = makeTable<VNeon, 6, 4>("neon", kNeon);
    return &t;
}
}  // namespace kern
}  // namespace tts
