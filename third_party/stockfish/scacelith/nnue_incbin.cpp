// Scacelith glue (not part of upstream Stockfish): the default network, embedded once for every
// instruction-set variant with Stockfish's own INCBIN (assembler .incbin), as upstream's
// nnue/network.cpp does in a single-architecture build. It defines gEmbeddedNNUEData /
// gEmbeddedNNUEEnd / gEmbeddedNNUESize, which each variant's nnue/network.cpp, compiled with
// UNIVERSAL_BINARY, only declares.
#include "evaluate.h"
// On Windows, the read-only data section, as cmake/embed.cmake does: INCBIN's default ".rodata" is
// a writable data section in PE/COFF.
#if defined(_WIN32)
#define INCBIN_OUTPUT_SECTION ".rdata,\"dr\""
#endif
#include "incbin/incbin.h"

INCBIN(EmbeddedNNUE, EvalFileDefaultName);
