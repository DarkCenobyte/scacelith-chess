# Stockfish 19 (GPL-3.0) compiled into the game as the static library `stockfish_embedded`.
# See README.scacelith.md for provenance and details.
#
#  * All upstream sources except src/main.cpp, unmodified. scacelith/entry.cpp replaces main() with
#    a function that src/ai runs on a thread (UCI over redirected std::cin / std::cout);
#    scacelith/cpu.cpp checks the CPU. The game only sees scacelith/stockfish_embedded.h.
#  * Instead of patching upstream, headers are force-included into every Stockfish source file:
#    scacelith/local_shm.h keeps the network in process memory (no system-wide shared memory, no
#    files, no sockets), and on Windows scacelith/win_shims.h redirects two Win32 calls (Wine's
#    NUMA stub, the console code page); scacelith/win_shims.cpp holds the redirected NUMA call.
#  * Upstream Makefile flags for ARCH=x86-64-sse41-popcnt with GCC (-O3, no exceptions).
#  * The default network is embedded with Stockfish's own INCBIN (assembler .incbin), so the engine
#    never reads a file.
cmake_minimum_required(VERSION 3.20)

# Build Stockfish like upstream does: without the game's global definitions (UNICODE,
# WIN32_LEAN_AND_MEAN, NOMINMAX on Windows) inherited from the parent directory.
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")

set(SF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SF_GLUE ${CMAKE_CURRENT_SOURCE_DIR}/scacelith)
set(SF_NET ${SF_DIR}/nn-1a298aa575a0.nnue)
set(SF_SOURCES
    attacks.cpp benchmark.cpp bitboard.cpp engine.cpp evaluate.cpp memory.cpp misc.cpp movegen.cpp
    movepick.cpp position.cpp score.cpp search.cpp thread.cpp timeman.cpp tt.cpp tune.cpp uci.cpp
    ucioption.cpp syzygy/tbprobe.cpp nnue/network.cpp nnue/nnue_accumulator.cpp nnue/nnue_misc.cpp
    nnue/features/full_threats.cpp nnue/features/half_ka_v2_hm.cpp nnue/features/pp_3wide.cpp)
list(TRANSFORM SF_SOURCES PREPEND ${SF_DIR}/)

# The engine proper: upstream Makefile flags for ARCH=x86-64-sse41-popcnt with GCC
#   -Wall -Wcast-qual -fno-exceptions -std=c++17 -fno-ipa-cp-clone -fconstexpr-ops-limit=500000000
#   -DNDEBUG -O3 -funroll-loops -DIS_64BIT -msse -msse3 -mpopcnt -DUSE_POPCNT -DUSE_SSE41 -msse4.1
#   -DUSE_SSSE3 -mssse3 -DUSE_SSE2 -msse2
# (search threads are pthreads with 8 MB stacks on Linux and MinGW alike, thread_native.h). The
# forced includes need "SHELL:": target_compile_options() would merge a second bare -include.
add_library(stockfish_engine OBJECT ${SF_SOURCES} ${SF_GLUE}/entry.cpp)
target_include_directories(stockfish_engine PRIVATE ${SF_DIR})
target_compile_definitions(stockfish_engine PRIVATE NDEBUG IS_64BIT USE_POPCNT USE_SSE41 USE_SSSE3 USE_SSE2)
target_compile_options(stockfish_engine PRIVATE
    -O3 -funroll-loops -fno-exceptions -fno-ipa-cp-clone -fconstexpr-ops-limit=500000000 -Wall -Wcast-qual
    -msse -msse2 -msse3 -mssse3 -msse4.1 -mpopcnt "SHELL:-include ${SF_GLUE}/local_shm.h")
if(WIN32)
    target_compile_options(stockfish_engine PRIVATE "SHELL:-include ${SF_GLUE}/win_shims.h")
endif()
set_target_properties(stockfish_engine PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
# INCBIN's .incbin "nn-1a298aa575a0.nnue" is resolved by the assembler (current directory, then its
# own -I path), and the build runs in the binary directory: point `as` at the network's directory.
set_source_files_properties(${SF_DIR}/nnue/network.cpp PROPERTIES
    COMPILE_OPTIONS "-Wa,-I${SF_DIR}" OBJECT_DEPENDS ${SF_NET})

# The CPU check and the Win32 wrapper are compiled with the default x86-64 flags, without the
# forced includes, so they run anywhere.
add_library(stockfish_embedded STATIC ${SF_GLUE}/cpu.cpp $<TARGET_OBJECTS:stockfish_engine>)
target_compile_options(stockfish_embedded PRIVATE -O2 -Wall -Wextra)
target_include_directories(stockfish_embedded PUBLIC ${SF_GLUE})
find_package(Threads REQUIRED)
target_link_libraries(stockfish_embedded PUBLIC Threads::Threads)
if(WIN32)
    target_sources(stockfish_embedded PRIVATE ${SF_GLUE}/win_shims.cpp)
    target_link_libraries(stockfish_embedded PUBLIC shell32)  # CommandLineToArgvW (misc.cpp)
endif()
