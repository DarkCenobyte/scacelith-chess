// AVX-VNNI kernels (Alder Lake and later, Zen 5): the AVX2 unit rebuilt with vpdpbusd for the
// integer GEMM. Compiled with -mavx2 -mfma -mavxvnni (CMakeLists.txt).
#define TTS_KERNELS_AVXVNNI 1
#include "kernels_avx2.cpp"
