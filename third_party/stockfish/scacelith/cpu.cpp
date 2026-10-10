// Scacelith glue (not part of upstream Stockfish): the dispatcher. Chooses the instruction-set
// variant of the embedded engine this CPU runs best, runs that variant's static initialisers and
// calls its entry point. Compiled once, with the default flags of the target (x86-64, ARMv8-A), so
// it runs on any CPU of the architecture. Each variant is one isolated object
// (cmake/isolate.cmake) that exports only scacelith_sf_main_<tag> and the bounds of its initialiser
// table, sfinit_<tag>_start / _end; on macOS the single variant is not isolated
// (SCACELITH_SF_ISOLATED 0) and the C runtime has run its initialisers at start-up.
#include "stockfish_embedded.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "sf_variants.h"  // generated: SCACELITH_SF_VARIANTS(X), the variants of this build

#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1 << 20)  // as upstream's universal/entry_arm64.cpp (glibc older than 2.26)
#endif
#endif

// What each variant's isolated object exports.
#if SCACELITH_SF_ISOLATED
#define SCACELITH_SF_DECLARE(tag, arch, level)                                                          \
    extern "C" int scacelith_sf_main_##tag();                                                           \
    extern "C" void (*const sfinit_##tag##_start[])();                                                  \
    extern "C" void (*const sfinit_##tag##_end[])();
#else
#define SCACELITH_SF_DECLARE(tag, arch, level) extern "C" int scacelith_sf_main_##tag();
#endif
SCACELITH_SF_VARIANTS(SCACELITH_SF_DECLARE)

namespace {

// Every variant the build knows, from the baseline up, by Stockfish ARCH name; the index is the
// variant's level (SF_ALL_VARIANTS in CMakeLists.txt, same order). A cap at level N allows the
// variants 0..N.
#if defined(__aarch64__) && defined(__APPLE__)
enum Level { APPLE_SILICON, LEVEL_COUNT };
constexpr const char* kLevelArch[LEVEL_COUNT] = {"apple-silicon"};
#elif defined(__aarch64__)
enum Level { ARMV8, ARMV8_DOTPROD, LEVEL_COUNT };
constexpr const char* kLevelArch[LEVEL_COUNT] = {"armv8", "armv8-dotprod"};
#else
enum Level { X86_64, SSE41_POPCNT, AVX2, AVXVNNI, AVX512ICL, LEVEL_COUNT };
constexpr const char* kLevelArch[LEVEL_COUNT] = {"x86-64", "x86-64-sse41-popcnt", "x86-64-avx2", "x86-64-avxvnni",
                                                 "x86-64-avx512icl"};
#endif

constexpr bool sameName(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

#define SCACELITH_SF_CHECK(tag, arch, level)                                                            \
    static_assert(level >= 0 && level < LEVEL_COUNT && sameName(kLevelArch[level], arch),               \
                  "CMakeLists.txt and cpu.cpp disagree on the variant " arch);
SCACELITH_SF_VARIANTS(SCACELITH_SF_CHECK)

struct Variant {
    int level;
    const char* arch;
    int (*main)();
    void (*const* initBegin)();
    void (*const* initEnd)();
};
#if SCACELITH_SF_ISOLATED
#define SCACELITH_SF_ROW(tag, arch, level) {level, arch, &scacelith_sf_main_##tag, sfinit_##tag##_start, sfinit_##tag##_end},
#else
#define SCACELITH_SF_ROW(tag, arch, level) {level, arch, &scacelith_sf_main_##tag, nullptr, nullptr},
#endif
constexpr Variant kVariants[] = {SCACELITH_SF_VARIANTS(SCACELITH_SF_ROW)};
constexpr int kVariantCount = int(sizeof(kVariants) / sizeof(kVariants[0]));

// Whether this CPU can run the code of the variant at `level`: every extension its compiler flags
// enable (CMakeLists.txt, SF_ISA_<arch>).
#if defined(__aarch64__) && defined(__APPLE__)
// Every Apple silicon CPU (M1 and later, ARMv8.4-A and up) has the NEON dot product.
bool cpuRuns(int level) { return level == APPLE_SILICON; }
#elif defined(__aarch64__)
// Linux aarch64: the dot product (ARMv8.2-A, optional before ARMv8.4-A) as the kernel reports it,
// as upstream's universal/entry_arm64.cpp does; the CPUs that have it implement ARMv8.2-A, all of
// -march=armv8.2-a.
bool cpuRuns(int level) {
    switch (level) {
    case ARMV8: return true;
    case ARMV8_DOTPROD: return (getauxval(AT_HWCAP) & HWCAP_ASIMDDP) != 0;
    }
    return false;
}
#else
// x86-64: upstream's dispatcher does not check SSE3 / SSSE3 for sse41-popcnt, BMI1 for avx2
// (-mbmi) nor AVX512DQ / AVX512CD for avx512icl; this one does. The OS support comes with libgcc's
// CPU model: it reports AVX2 only when XCR0 shows that the OS saves the YMM registers, and the
// AVX-512 features only when it also saves the opmask and ZMM registers. Every VEX variant
// requires AVX2 and the EVEX one the AVX-512 features.
#define HAS(feature) __builtin_cpu_supports(feature)
bool cpuRuns(int level) {
    switch (level) {
    case X86_64: return true;
    case SSE41_POPCNT: return HAS("sse3") && HAS("ssse3") && HAS("sse4.1") && HAS("popcnt");
    case AVX2: return cpuRuns(SSE41_POPCNT) && HAS("avx2") && HAS("bmi");
    case AVXVNNI: return cpuRuns(AVX2) && HAS("bmi2") && HAS("avxvnni");
    case AVX512ICL:
        return cpuRuns(AVX2) && HAS("bmi2") && HAS("avx512f") && HAS("avx512bw") && HAS("avx512vl") &&
               HAS("avx512dq") && HAS("avx512cd") && HAS("avx512vnni") && HAS("avx512ifma") &&
               HAS("avx512vbmi") && HAS("avx512vbmi2") && HAS("avx512vpopcntdq") && HAS("avx512bitalg") &&
               HAS("vpclmulqdq") && HAS("gfni") && HAS("vaes");
    }
    return false;
}
#undef HAS
#endif

// The best built variant this CPU runs at or below `maxLevel`, or -1. The levels are not a strict
// chain (a CPU may have AVX-512 but not AVX-VNNI), so each candidate is checked on its own.
int best(int maxLevel) {
#if !defined(__aarch64__)
    __builtin_cpu_init();
#endif
    for (int i = kVariantCount - 1; i >= 0; --i)
        if (kVariants[i].level <= maxLevel && cpuRuns(kVariants[i].level)) return i;
    return -1;
}

std::atomic<int> gCap{LEVEL_COUNT - 1};  // highest level allowed (engine.arch)
std::atomic<int> gChosen{-1};            // index in kVariants of the last stockfish_embedded_supported()
std::once_flag gInitialised[kVariantCount];

// A variant's static initialisers, in the order the C runtime would have run them: .init_array
// (ELF) forwards, .ctors (MinGW's __do_global_ctors) backwards. None without isolation (macOS:
// already run by the C runtime).
void runInitialisers(const Variant& v) {
#if defined(_WIN32)
    for (auto p = v.initEnd; p != v.initBegin;) (*--p)();
#else
    for (auto p = v.initBegin; p != v.initEnd; ++p) (*p)();
#endif
}

}  // namespace

int stockfish_embedded_variant_count() { return kVariantCount; }

const char* stockfish_embedded_variant(int index) {
    return index >= 0 && index < kVariantCount ? kVariants[index].arch : nullptr;
}

bool stockfish_embedded_limit_arch(const char* arch) {
    if (!arch || !*arch || std::strcmp(arch, "auto") == 0) {
        gCap = LEVEL_COUNT - 1;
        return true;
    }
    for (int level = 0; level < LEVEL_COUNT; ++level) {
        if (std::strcmp(arch, kLevelArch[level]) == 0) {
            gCap = level;
            return true;
        }
    }
    return false;
}

bool stockfish_embedded_supported() {
    const int chosen = best(gCap);
    gChosen = chosen;
    if (chosen < 0) return false;
    std::call_once(gInitialised[chosen], [chosen] { runInitialisers(kVariants[chosen]); });
    return true;
}

const char* stockfish_embedded_arch() {
    const int chosen = gChosen;
    return chosen >= 0 ? kVariants[chosen].arch : "none";
}

const char* stockfish_embedded_best_arch() {
    const int i = best(LEVEL_COUNT - 1);
    return i >= 0 ? kVariants[i].arch : "none";
}

int stockfish_embedded_main() {
    const int chosen = gChosen;
    return chosen >= 0 ? kVariants[chosen].main() : 1;
}
