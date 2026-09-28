// Scacelith glue (not part of upstream Stockfish): CPU check for the embedded build. Compiled with
// the default x86-64 flags (no SSE4.1/POPCNT), so it can run on any CPU.
#include "stockfish_embedded.h"

bool stockfish_embedded_supported() {
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("sse4.1") && __builtin_cpu_supports("popcnt");
#else
    return true;
#endif
}

const char* stockfish_embedded_arch() { return "x86-64-sse41-popcnt"; }
