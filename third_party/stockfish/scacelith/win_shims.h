// Scacelith glue (not part of upstream Stockfish): force-included (-include) before every
// Stockfish source file of the Windows build, after local_shm.h. It redirects two Win32 calls of
// upstream code without editing it:
//
//  * GetNumaProcessorNodeEx (numa.h, NumaConfig::from_system_numa): Wine, and so Proton, only
//    stubs it (it fails with ERROR_CALL_NOT_IMPLEMENTED, Wine 9.0 to 11.0). Upstream then builds an
//    empty system NUMA configuration while it still finds the L3 caches, and
//    NumaConfig::try_get_l3_aware_config() calls nodeByCpu.at() on the empty map:
//    std::out_of_range, i.e. std::terminate() (no exceptions) in Engine's constructor, which ends
//    the game. The wrapper (win_shims.cpp) reports node 0, one node for the whole machine, when
//    the API is not implemented; Windows implements it, so there the wrapper changes nothing.
//  * SetConsoleCP / SetConsoleOutputCP (misc.cpp, set_console_utf8(), at every session start):
//    the engine talks to the game through in-memory streams, not a console, and in a console build
//    (SCACELITH_CONSOLE) these calls would switch the code page of the console the game was
//    started from, which outlives the game. They do nothing here.
//
// A macro only redirects direct calls: if upstream ever reached these functions another way (for
// example through GetProcAddress), the Wine run of the Windows build would crash again, which is
// why every Stockfish update is checked under Wine (README.scacelith.md).
#pragma once
#if defined(_WIN32)

    // As upstream's numa.h and memory.h include it: <windows.h> is included once per translation
    // unit, so the SDK's own declarations must be seen before the macros below rename the calls.
    #if _WIN32_WINNT < 0x0601
        #undef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601
    #endif
    #if !defined(NOMINMAX)
        #define NOMINMAX
    #endif
    #include <windows.h>
    #if defined(small)
        #undef small
    #endif

extern "C" BOOL scacelith_GetNumaProcessorNodeEx(PPROCESSOR_NUMBER processor, PUSHORT nodeNumber);

inline BOOL scacelith_KeepConsoleCodePage(UINT) { return TRUE; }

    #define GetNumaProcessorNodeEx scacelith_GetNumaProcessorNodeEx
    #define SetConsoleCP scacelith_KeepConsoleCodePage
    #define SetConsoleOutputCP scacelith_KeepConsoleCodePage

#endif
