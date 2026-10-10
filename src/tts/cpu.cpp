// Run-time choice of the kernel table (baseline code). Mirrors Stockfish's dispatcher
// (third_party/stockfish/scacelith/cpu.cpp): __builtin_cpu_supports also checks that the OS saves
// the AVX / AVX-512 register state (XCR0), and __builtin_cpu_init must run first when this is
// reached during static initialisation. aarch64 has nothing to check: NEON is baseline there.
#include "kernels.h"
#include "core/log.h"
#include <atomic>
#include <cstring>

namespace tts {
namespace kern {
namespace {

std::atomic<int> g_cap{kLevelCount - 1};

const Table* builtTable(int level) {
    switch (level) {
    case kScalar: return tableScalar();
#if defined(__aarch64__)
    case kNeon: return tableNeon();
#else
    case kSse2: return tableSse2();
    case kAvx2: return tableAvx2();
    case kAvxVnni: return tableAvxVnni();
    case kAvx512: return tableAvx512();
#endif
    default: return nullptr;
    }
}

}  // namespace

const char* levelName(int level) {
#if defined(__aarch64__)
    static const char* const names[kLevelCount] = {"scalar", "neon"};
#else
    static const char* const names[kLevelCount] = {"scalar", "sse2", "avx2", "avxvnni", "avx512"};
#endif
    return level >= 0 && level < kLevelCount ? names[level] : "?";
}

bool cpuRuns(int level) {
#if defined(__aarch64__)
    return level == kScalar || level == kNeon;
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    __builtin_cpu_init();
    switch (level) {
    case kScalar:
    case kSse2: return true;
    case kAvx2: return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    case kAvxVnni:
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("avxvnni");
    case kAvx512:
        return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
               __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512dq") &&
               __builtin_cpu_supports("avx512vnni") && __builtin_cpu_supports("fma");
    default: return false;
    }
#else
    return level == kScalar;
#endif
}

const Table* tableFor(int level) { return cpuRuns(level) ? builtTable(level) : nullptr; }

const Table& active() {
    // Levels are not a strict chain (a CPU may have AVX-512 VNNI without AVX-VNNI or the reverse):
    // take the highest one the CPU runs, under the cap.
    int cap = g_cap.load(std::memory_order_relaxed);
    for (int level = cap; level > kScalar; --level)
        if (cpuRuns(level)) return *builtTable(level);
#if defined(__aarch64__)
    return *builtTable(kScalar);
#else
    return *builtTable(kSse2 <= cap ? kSse2 : kScalar);
#endif
}

bool setArchCap(const char* arch) {
    if (!arch || !*arch || std::strcmp(arch, "auto") == 0) {
        g_cap.store(kLevelCount - 1);
        return true;
    }
    for (int level = 0; level < kLevelCount; ++level)
        if (std::strcmp(arch, levelName(level)) == 0) {
            g_cap.store(level);
            return true;
        }
#if defined(__aarch64__)
    LOGW("tts: unknown arch cap '%s' (auto, scalar, neon)", arch);
#else
    LOGW("tts: unknown arch cap '%s' (auto, scalar, sse2, avx2, avxvnni, avx512)", arch);
#endif
    return false;
}

}  // namespace kern
}  // namespace tts
