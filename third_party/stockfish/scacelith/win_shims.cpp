// Scacelith glue (not part of upstream Stockfish): the NUMA wrapper that win_shims.h substitutes
// for GetNumaProcessorNodeEx in the Stockfish sources. Compiled without the forced includes, so the
// call below reaches the real Win32 function.
#if defined(_WIN32)
#include <windows.h>

extern "C" BOOL scacelith_GetNumaProcessorNodeEx(PPROCESSOR_NUMBER processor, PUSHORT nodeNumber) {
    if (GetNumaProcessorNodeEx(processor, nodeNumber)) return TRUE;
    if (GetLastError() != ERROR_CALL_NOT_IMPLEMENTED) return FALSE;
    // Wine / Proton: no NUMA information. Every existing processor is on node 0, as on any machine
    // with a single memory node.
    if (processor->Number < GetActiveProcessorCount(processor->Group)) {
        *nodeNumber = 0;
        return TRUE;
    }
    return FALSE;
}
#endif
