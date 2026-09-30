// Scacelith glue for the embedded Stockfish 19 (GPL-3.0). This is the only header the game sees;
// Stockfish's own headers stay private to the stockfish_embedded library.
//
// A session is started by calling stockfish_embedded_main() on a dedicated thread after std::cin /
// std::cout have been redirected to in-memory stream buffers (see src/ai/uci_host.cpp); it returns
// after the "quit" command. The engine's state (options, thread pool, hash table, network) belongs
// to the session and is freed when it ends, so sessions can be run again sequentially (start,
// quit, start, ...), each one loading the network again. Only one session may run at a time: they
// share the process's std::cin / std::cout and a few tables.
#pragma once

// Returns true when the CPU can run the embedded engine (x86-64 with SSE4.1 and POPCNT).
bool stockfish_embedded_supported();

// Instruction-set target of the embedded build (Stockfish's ARCH name).
const char* stockfish_embedded_arch();

// Stockfish 19's main() without the banner: initialises the engine (UCI options, tables, thread
// pool, embedded NNUE network), runs the UCI loop on std::cin / std::cout until "quit" (or end of
// input) and frees it all. Returns 0. Stockfish ends the whole process (std::exit(1)) when a
// "position" command holds an illegal move or FEN, or "go" a malformed number: the client must
// only send what it has checked (src/ai/engine.cpp).
int stockfish_embedded_main();
