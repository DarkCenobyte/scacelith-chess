// Scacelith glue for the embedded Stockfish 16 (GPL-3.0). This is the only header the game sees;
// Stockfish's own headers stay private to the stockfish_embedded library.
//
// Stockfish keeps its whole state in globals (options, thread pool, hash table, network), so only
// one engine session can run at a time in the process. A session is started by calling
// stockfish_embedded_main() on a dedicated thread after std::cin / std::cout have been redirected
// to in-memory stream buffers (see src/ai/uci_host.cpp); it returns after the "quit" command.
// Sessions can be run again sequentially (start, quit, start, ...).
#pragma once

// Returns true when the CPU can run the embedded engine (x86-64 with SSE4.1 and POPCNT).
bool stockfish_embedded_supported();

// Instruction-set target of the embedded build (Stockfish's ARCH name).
const char* stockfish_embedded_arch();

// Stockfish 16's main() without the banner: initialises the engine (UCI options, tables, thread
// pool, embedded NNUE network), runs the UCI loop on std::cin / std::cout until "quit" (or end of
// input) and tears the thread pool down. Never calls exit() on the normal path. Returns 0.
int stockfish_embedded_main();
