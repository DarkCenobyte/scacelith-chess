# Stockfish 16 (GPL-3.0) compiled into the game as the static library `stockfish_embedded`.
# See README.scacelith.md for provenance and details.
#
#  * All upstream sources except src/main.cpp, unmodified. scacelith/entry.cpp replaces main() with
#    a function that src/ai runs on a thread (UCI over redirected std::cin / std::cout);
#    scacelith/cpu.cpp checks the CPU. The game only sees scacelith/stockfish_embedded.h.
#  * Upstream ARCH=x86-64-sse41-popcnt flags (-O3, no exceptions, like the upstream Makefile).
#  * The default network is embedded with Stockfish's own INCBIN (assembler .incbin), so the engine
#    never reads a file.
cmake_minimum_required(VERSION 3.20)

# Build Stockfish like upstream does: without the game's global definitions (UNICODE,
# WIN32_LEAN_AND_MEAN, NOMINMAX on Windows) inherited from the parent directory.
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")

set(SF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SF_NET ${SF_DIR}/nn-5af11540bbfe.nnue)
set(SF_SOURCES
    benchmark.cpp bitbase.cpp bitboard.cpp endgame.cpp evaluate.cpp material.cpp misc.cpp movegen.cpp
    movepick.cpp pawns.cpp position.cpp psqt.cpp search.cpp thread.cpp timeman.cpp tt.cpp tune.cpp
    uci.cpp ucioption.cpp syzygy/tbprobe.cpp nnue/evaluate_nnue.cpp nnue/features/half_ka_v2_hm.cpp)
list(TRANSFORM SF_SOURCES PREPEND ${SF_DIR}/)

# The engine proper: upstream Makefile flags for ARCH=x86-64-sse41-popcnt
#   -Wall -Wcast-qual -fno-exceptions -std=c++17 -DNDEBUG -O3 -DIS_64BIT -msse -msse3 -mpopcnt
#   -DUSE_POPCNT -DUSE_SSE41 -msse4.1 -DUSE_SSSE3 -mssse3 -DUSE_SSE2 -msse2
# plus -DUSE_PTHREADS, which the Makefile adds on every non-MinGW target (MinGW gets pthread-based
# search threads with 8 MB stacks from thread_win32_osx.h by itself).
add_library(stockfish_engine OBJECT ${SF_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/scacelith/entry.cpp)
target_include_directories(stockfish_engine PRIVATE ${SF_DIR})
target_compile_definitions(stockfish_engine PRIVATE
    NDEBUG IS_64BIT USE_POPCNT USE_SSE41 USE_SSSE3 USE_SSE2 $<$<NOT:$<BOOL:${WIN32}>>:USE_PTHREADS>)
target_compile_options(stockfish_engine PRIVATE
    -O3 -fno-exceptions -Wall -Wcast-qual -msse -msse2 -msse3 -mssse3 -msse4.1 -mpopcnt)
set_target_properties(stockfish_engine PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
# INCBIN's .incbin "nn-5af11540bbfe.nnue" is resolved by the assembler (current directory, then its
# own -I path), and the build runs in the binary directory: point `as` at the network's directory.
set_source_files_properties(${SF_DIR}/evaluate.cpp PROPERTIES
    COMPILE_OPTIONS "-Wa,-I${SF_DIR}" OBJECT_DEPENDS ${SF_NET})

# The CPU check is compiled with the default x86-64 flags so it runs anywhere.
add_library(stockfish_embedded STATIC ${CMAKE_CURRENT_SOURCE_DIR}/scacelith/cpu.cpp $<TARGET_OBJECTS:stockfish_engine>)
target_compile_options(stockfish_embedded PRIVATE -O2 -Wall -Wextra)
target_include_directories(stockfish_embedded PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/scacelith)
find_package(Threads REQUIRED)
target_link_libraries(stockfish_embedded PUBLIC Threads::Threads)
