// GEMM drivers (baseline code): packing, cache blocking and threading around the micro-kernels
// of the active kernel table.
#pragma once
#include "kernels.h"
#include <cstddef>
#include <cstdint>

namespace tts {

class ThreadPool;

// Left operand, M x K, row-major: float (any alignment) or int8 dequantised per row as
// (q - zp[r]) * scale[r] (the weights of a DequantizeLinear -> Conv / MatMul).
struct GemmA {
    const float* f32 = nullptr;
    const int8_t* i8 = nullptr;
    const float* scale = nullptr;      // per row (i8)
    const int32_t* zp = nullptr;       // per row (i8), may be null
    ptrdiff_t ld = 0;
};

// Right operand, K x N float, row-major (any alignment). Its columns may come in 'blocks' of
// 'blockCols' columns each, block b starting at f32 + b * blockStride (several batch items of a
// 1x1 convolution multiplied in one GEMM): N = blocks * blockCols.
struct GemmB {
    const float* f32 = nullptr;
    ptrdiff_t ld = 0;
    int blocks = 1;
    int blockCols = 0;
    ptrdiff_t blockStride = 0;
};

// C (M x N, row stride ldc) = A * B. With cBlockStride > 0 the columns of C come in the blocks of
// B instead: column c of block j is at C + j * cBlockStride + c (a 1x1 convolution of several batch
// items writes each item's [Cout x L] output in place).
void sgemm(const kern::Table& k, ThreadPool* pool, int M, int N, int K, const GemmA& a, const GemmB& b, float* C,
           ptrdiff_t ldc, ptrdiff_t cBlockStride = 0);

// Integer GEMM: C (int32, M x N) = sum_k (A[m,k] - azp) * (B[k,n] - bzp), exact. A and B are 8-bit
// row-major; exactly one of them is unsigned and only the unsigned one may have a zero point.
// 'signedSums' (optional) holds the sums over k of the signed operand (per row of A when A is the
// signed one, per column of B otherwise), precomputed for constant weights.
bool igemm(const kern::Table& k, ThreadPool* pool, int M, int N, int K, const uint8_t* A, ptrdiff_t lda,
           bool aUnsigned, int azp, const uint8_t* B, ptrdiff_t ldb, bool bUnsigned, int bzp, int32_t* C,
           ptrdiff_t ldc, const int32_t* signedSums = nullptr);

}  // namespace tts
