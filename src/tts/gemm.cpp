#include "gemm.h"
#include "tensor.h"
#include "threads.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace tts {
namespace {

constexpr int kKc = 512;   // depth of one packed A block (mr x kKc floats stay in L1/L2)

float loadF(const float* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}

void runTasks(ThreadPool* pool, int tasks, const std::function<void(int)>& fn) {
    if (pool && tasks > 1) pool->run(tasks, fn);
    else
        for (int t = 0; t < tasks; ++t) fn(t);
}

int taskCount(ThreadPool* pool, int units) {
    int threads = pool ? pool->size() : 1;
    return std::max(1, std::min(units, threads == 1 ? 1 : threads * 4));
}

}  // namespace

void sgemm(const kern::Table& k, ThreadPool* pool, int M, int N, int K, const GemmA& a, const GemmB& b, float* C,
           ptrdiff_t ldc) {
    if (M <= 0 || N <= 0) return;
    if (K <= 0) {
        for (int i = 0; i < M; ++i) std::memset(C + i * ldc, 0, size_t(N) * 4);
        return;
    }
    const int nr = k.nr, mr = k.mr;
    const int panels = (N + nr - 1) / nr;
    Buffer bbuf(size_t(panels) * size_t(K) * size_t(nr) * 4);
    float* Bp = static_cast<float*>(bbuf.data);
    const int blockCols = b.blocks > 1 ? b.blockCols : N;
    auto packPanel = [&](int p) {
        int j0 = p * nr, cols = std::min(nr, N - j0);
        float* dst = Bp + size_t(p) * size_t(K) * size_t(nr);
        int blk = j0 / blockCols, c0 = j0 % blockCols;
        if (c0 + cols <= blockCols) {
            k.packBf32(b.f32 + blk * b.blockStride + c0, b.ld, K, cols, dst);
            return;
        }
        // The panel straddles two column blocks: gather column by column.
        for (int j = 0; j < nr; ++j) {
            int col = j0 + j;
            const float* src = j < cols ? b.f32 + (col / blockCols) * b.blockStride + col % blockCols : nullptr;
            for (int kk = 0; kk < K; ++kk) dst[kk * nr + j] = src ? loadF(src + kk * b.ld) : 0.0f;
        }
    };
    if (pool && size_t(panels) * size_t(K) * size_t(nr) > 65536) runTasks(pool, panels, packPanel);
    else
        for (int p = 0; p < panels; ++p) packPanel(p);

    const int rowPanels = (M + mr - 1) / mr;
    const int tasks = taskCount(pool, rowPanels);
    runTasks(pool, tasks, [&](int t) {
        int rp0 = int(int64_t(rowPanels) * t / tasks), rp1 = int(int64_t(rowPanels) * (t + 1) / tasks);
        const int kcMax = std::min(K, kKc);
        Buffer abuf(size_t(mr) * size_t(kcMax) * 4);
        float* Ap = static_cast<float*>(abuf.data);
        for (int rp = rp0; rp < rp1; ++rp) {
            int i0 = rp * mr, rows = std::min(mr, M - i0);
            for (int kb = 0; kb < K; kb += kKc) {
                int kc = std::min(kKc, K - kb);
                if (a.i8)
                    k.packAi8(a.i8 + i0 * a.ld + kb, a.ld, rows, kc, a.scale + i0, a.zp ? a.zp + i0 : nullptr, Ap);
                else
                    k.packAf32(a.f32 + i0 * a.ld + kb, a.ld, rows, kc, Ap);
                if (rows < mr) std::memset(Ap + rows * kc, 0, size_t(mr - rows) * size_t(kc) * 4);
                for (int p = 0; p < panels; ++p) {
                    int j0 = p * nr, cols = std::min(nr, N - j0);
                    k.sgemmTile(kc, Ap, Bp + (size_t(p) * size_t(K) + size_t(kb)) * size_t(nr), C + i0 * ldc + j0,
                                ldc, rows, cols, kb > 0);
                }
            }
        }
    });
}

bool igemm(const kern::Table& k, ThreadPool* pool, int M, int N, int K, const uint8_t* A, ptrdiff_t lda,
           bool aUnsigned, int azp, const uint8_t* B, ptrdiff_t ldb, bool bUnsigned, int bzp, int32_t* C,
           ptrdiff_t ldc, const int32_t* signedSums) {
    if (aUnsigned == bUnsigned || (!aUnsigned && azp) || (!bUnsigned && bzp)) return false;
    if (M <= 0 || N <= 0) return true;
    const int ik = k.ik, mr = k.imr, nr = k.inr;
    const int kg = std::max(1, (K + ik - 1) / ik);
    const bool vnni = ik == 4;
    const int panels = (N + nr - 1) / nr;
    const size_t panelBytes = size_t(kg) * size_t(nr) * 4;
    Buffer bbuf(size_t(panels) * panelBytes);
    uint8_t* Bp = static_cast<uint8_t*>(bbuf.data);
    auto packPanel = [&](int p) {
        int j0 = p * nr, cols = std::min(nr, N - j0);
        k.packIntB(B + j0, ldb, K, cols, bUnsigned, vnni ? 0 : bzp, Bp + size_t(p) * panelBytes);
    };
    if (pool && size_t(K) * size_t(N) > 65536) runTasks(pool, panels, packPanel);
    else
        for (int p = 0; p < panels; ++p) packPanel(p);
    // VNNI keeps the bytes as they are: sum (a - azp) b = sum a b - azp sum b, added by the tiles
    // as per-column (A unsigned) or per-row (B unsigned) offsets.
    std::vector<int32_t> colOff, rowOff;
    if (vnni && aUnsigned && azp) {
        colOff.assign(size_t(N), 0);
        if (signedSums) {
            for (int j = 0; j < N; ++j) colOff[size_t(j)] = -azp * signedSums[j];
        } else {
            for (int kk = 0; kk < K; ++kk) {
                const int8_t* row = reinterpret_cast<const int8_t*>(B + kk * ldb);
                for (int j = 0; j < N; ++j) colOff[size_t(j)] += row[j];
            }
            for (int32_t& v : colOff) v *= -azp;
        }
    }
    if (vnni && bUnsigned && bzp) {
        rowOff.assign(size_t(M), 0);
        for (int i = 0; i < M; ++i) {
            int32_t sum = 0;
            if (signedSums) {
                sum = signedSums[i];
            } else {
                const int8_t* row = reinterpret_cast<const int8_t*>(A + i * lda);
                for (int kk = 0; kk < K; ++kk) sum += row[kk];
            }
            rowOff[size_t(i)] = -bzp * sum;
        }
    }
    const int rowPanels = (M + mr - 1) / mr;
    const int tasks = taskCount(pool, rowPanels);
    runTasks(pool, tasks, [&](int t) {
        int rp0 = int(int64_t(rowPanels) * t / tasks), rp1 = int(int64_t(rowPanels) * (t + 1) / tasks);
        Buffer abuf(size_t(mr) * size_t(kg) * 4);
        uint8_t* Ap = static_cast<uint8_t*>(abuf.data);
        for (int rp = rp0; rp < rp1; ++rp) {
            int i0 = rp * mr, rows = std::min(mr, M - i0);
            if (rows < mr) std::memset(Ap, 0, size_t(mr) * size_t(kg) * 4);
            k.packIntA(A + i0 * lda, lda, rows, K, aUnsigned, vnni ? 0 : azp, Ap);
            const int32_t* ro = rowOff.empty() ? nullptr : rowOff.data() + i0;
            for (int p = 0; p < panels; ++p) {
                int j0 = p * nr, cols = std::min(nr, N - j0);
                k.igemmTile(kg, Ap, Bp + size_t(p) * panelBytes, C + i0 * ldc + j0, ldc, rows, cols, aUnsigned, ro,
                            colOff.empty() ? nullptr : colOff.data() + j0);
            }
        }
    });
    return true;
}

}  // namespace tts
