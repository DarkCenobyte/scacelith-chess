# Stockfish 19 (GPL-3.0) compiled into the game as the static library `stockfish_embedded`.
# See README.scacelith.md for provenance and details.
#
#  * All upstream sources except src/main.cpp, unmodified, compiled once per instruction-set
#    variant (SCACELITH_SF_VARIANTS). scacelith/entry.cpp replaces main() with a function that
#    src/ai runs on a thread (UCI over redirected std::cin / std::cout); scacelith/cpu.cpp picks
#    the variant the CPU runs best. The game only sees scacelith/stockfish_embedded.h.
#  * Instead of patching upstream, headers are force-included into every Stockfish source file:
#    scacelith/local_shm.h keeps the network in process memory (no system-wide shared memory, no
#    files, no sockets), and on Windows scacelith/win_shims.h redirects two Win32 calls (Wine's
#    NUMA stub, the console code page); scacelith/win_shims.cpp holds the redirected NUMA call.
#  * Upstream Makefile flags for each ARCH with GCC (-O3, no exceptions). Each variant has its
#    namespace renamed (Stockfish_<tag>) and is isolated into one object by cmake/isolate.cmake,
#    so no code of one instruction set can be reached from the game or from another variant.
#  * The default network is embedded once for all variants with Stockfish's own INCBIN (assembler
#    .incbin, scacelith/nnue_incbin.cpp), so the engine never reads a file.
cmake_minimum_required(VERSION 3.21)

# Build Stockfish like upstream does: without the game's global definitions (UNICODE,
# WIN32_LEAN_AND_MEAN, NOMINMAX on Windows) inherited from the parent directory.
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")

# The variants, from the baseline up; scacelith/cpu.cpp knows the requirements of each. Every one
# is a full Stockfish compile: a single one (e.g. x86-64-sse41-popcnt) makes quicker local builds.
set(SF_ALL_VARIANTS x86-64 x86-64-sse41-popcnt x86-64-avx2 x86-64-avxvnni x86-64-avx512icl)
set(SCACELITH_SF_VARIANTS "${SF_ALL_VARIANTS}" CACHE STRING
    "Stockfish instruction-set variants to build (a subset of: ${SF_ALL_VARIANTS})")
if(NOT SCACELITH_SF_VARIANTS)
    message(FATAL_ERROR "SCACELITH_SF_VARIANTS is empty (known variants: ${SF_ALL_VARIANTS})")
endif()
foreach(arch IN LISTS SCACELITH_SF_VARIANTS)
    if(NOT arch IN_LIST SF_ALL_VARIANTS)
        message(FATAL_ERROR "SCACELITH_SF_VARIANTS: unknown variant '${arch}' (known: ${SF_ALL_VARIANTS})")
    endif()
endforeach()

set(SF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SF_GLUE ${CMAKE_CURRENT_SOURCE_DIR}/scacelith)
set(SF_NET ${SF_DIR}/nn-1a298aa575a0.nnue)
set(SF_SOURCES
    attacks.cpp benchmark.cpp bitboard.cpp engine.cpp evaluate.cpp memory.cpp misc.cpp movegen.cpp
    movepick.cpp position.cpp score.cpp search.cpp thread.cpp timeman.cpp tt.cpp tune.cpp uci.cpp
    ucioption.cpp syzygy/tbprobe.cpp nnue/network.cpp nnue/nnue_accumulator.cpp nnue/nnue_misc.cpp
    nnue/features/full_threats.cpp nnue/features/half_ka_v2_hm.cpp nnue/features/pp_3wide.cpp)
list(TRANSFORM SF_SOURCES PREPEND ${SF_DIR}/)
if(WIN32)
    set(SF_FORMAT PE)
else()
    set(SF_FORMAT ELF)
endif()
foreach(tool LINKER OBJCOPY OBJDUMP NM)
    if(NOT CMAKE_${tool})
        message(FATAL_ERROR "Stockfish variants: CMAKE_${tool} not found (binutils)")
    endif()
endforeach()

# Upstream Makefile flags for ARCH=<arch> with GCC:
#   -Wall -Wcast-qual -fno-exceptions -std=c++17 -fno-ipa-cp-clone -fconstexpr-ops-limit=500000000
#   -DNDEBUG -O3 -funroll-loops -DIS_64BIT -DARCH=<arch> and the instruction set below
# (search threads are pthreads with 8 MB stacks on Linux and MinGW alike, thread_native.h).
set(SF_ISA_x86-64 -DUSE_SSE2 -msse -msse2)
set(SF_ISA_x86-64-sse41-popcnt ${SF_ISA_x86-64} -msse3 -mpopcnt -DUSE_POPCNT -DUSE_SSE41 -msse4.1 -DUSE_SSSE3 -mssse3)
set(SF_ISA_x86-64-avx2 ${SF_ISA_x86-64-sse41-popcnt} -DUSE_AVX2 -mavx2 -mbmi)
set(SF_ISA_x86-64-avxvnni ${SF_ISA_x86-64-avx2} -DUSE_PEXT -mbmi2 -DUSE_VNNI -DUSE_AVXVNNI -mavxvnni)
set(SF_ISA_x86-64-avx512icl ${SF_ISA_x86-64-avx2} -DUSE_PEXT -mbmi2 -DUSE_AVX512 -DUSE_VNNI -DUSE_AVX512ICL
    -mavx512f -mavx512cd -mavx512vl -mavx512dq -mavx512bw -mavx512ifma -mavx512vbmi -mavx512vbmi2
    -mavx512vpopcntdq -mavx512bitalg -mavx512vnni -mvpclmulqdq -mgfni -mvaes)
# The highest instruction class each variant may contain, for tools/isa_audit.py: 0 baseline,
# 1 SSE3 to SSE4.2 and POPCNT, 2 VEX (AVX, AVX2, BMI, AVX-VNNI), 3 EVEX (AVX-512).
set(SF_AUDIT_x86-64 0)
set(SF_AUDIT_x86-64-sse41-popcnt 1)
set(SF_AUDIT_x86-64-avx2 2)
set(SF_AUDIT_x86-64-avxvnni 2)
set(SF_AUDIT_x86-64-avx512icl 3)

# One variant: its objects (namespace Stockfish renamed to Stockfish_<tag>, the network only
# declared: UNIVERSAL_BINARY), then one isolated object sf_<tag>.iso.o exporting
# scacelith_sf_main_<tag> and its initialiser table. -fno-gnu-unique (ELF): the guard variables of
# inline statics would otherwise be STB_GNU_UNIQUE symbols, which objcopy cannot make local. The
# forced includes need "SHELL:": target_compile_options() would merge a second bare -include.
set(SF_ISO_OBJECTS "")
set(SF_VARIANT_TABLE "")
set(SF_AUDIT_LEVELS "")
foreach(arch IN LISTS SF_ALL_VARIANTS)
    if(NOT arch IN_LIST SCACELITH_SF_VARIANTS)
        continue()
    endif()
    string(REPLACE "-" "_" tag ${arch})
    add_library(sf_obj_${tag} OBJECT ${SF_SOURCES} ${SF_GLUE}/entry.cpp)
    target_include_directories(sf_obj_${tag} PRIVATE ${SF_DIR})
    target_compile_definitions(sf_obj_${tag} PRIVATE
        Stockfish=Stockfish_${tag} UNIVERSAL_BINARY ARCH=${arch} SCACELITH_SF_TAG=${tag} NDEBUG IS_64BIT)
    target_compile_options(sf_obj_${tag} PRIVATE
        -O3 -funroll-loops -fno-exceptions -fno-ipa-cp-clone -fconstexpr-ops-limit=500000000 -Wall -Wcast-qual
        ${SF_ISA_${arch}} "SHELL:-include ${SF_GLUE}/local_shm.h")
    if(WIN32)
        target_compile_options(sf_obj_${tag} PRIVATE "SHELL:-include ${SF_GLUE}/win_shims.h")
    else()
        target_compile_options(sf_obj_${tag} PRIVATE -fno-gnu-unique)
    endif()
    set_target_properties(sf_obj_${tag} PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)
    set(iso ${CMAKE_CURRENT_BINARY_DIR}/sf_${tag}.iso.o)
    add_custom_command(OUTPUT ${iso}
        COMMAND ${CMAKE_COMMAND} -DTAG=${tag} -DENTRY=scacelith_sf_main_${tag} -DOUT=${iso} -DFORMAT=${SF_FORMAT}
                "-DINPUTS=$<JOIN:$<TARGET_OBJECTS:sf_obj_${tag}>,|>" -DLD=${CMAKE_LINKER}
                -DOBJCOPY=${CMAKE_OBJCOPY} -DOBJDUMP=${CMAKE_OBJDUMP} -DNM=${CMAKE_NM}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/isolate.cmake
        DEPENDS sf_obj_${tag} $<TARGET_OBJECTS:sf_obj_${tag}> ${CMAKE_CURRENT_SOURCE_DIR}/cmake/isolate.cmake
        COMMENT "Isolating Stockfish variant ${arch}" VERBATIM)
    list(APPEND SF_ISO_OBJECTS ${iso})
    list(FIND SF_ALL_VARIANTS ${arch} level)
    string(APPEND SF_VARIANT_TABLE " X(${tag}, \"${arch}\", ${level})")
    list(APPEND SF_AUDIT_LEVELS "\\(sf_${tag}\\.iso\\.o\\)=${SF_AUDIT_${arch}}")
endforeach()
set_source_files_properties(${SF_ISO_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/sf_variants.h CONTENT
"// Generated by third_party/stockfish/CMakeLists.txt: the Stockfish variants of this build, from
// the baseline up, as X(tag, Stockfish ARCH name, index in SF_ALL_VARIANTS).
#define SCACELITH_SF_VARIANTS(X)@SF_VARIANT_TABLE@
" @ONLY)

# The dispatcher, the network and the Win32 wrapper, once for all variants, with the default
# x86-64 flags and without the forced includes, so they run anywhere. INCBIN's .incbin
# "nn-1a298aa575a0.nnue" is resolved by the assembler (current directory, then its own -I path),
# and the build runs in the binary directory: point `as` at the network's directory.
add_library(stockfish_embedded STATIC ${SF_GLUE}/cpu.cpp ${SF_GLUE}/nnue_incbin.cpp ${SF_ISO_OBJECTS})
set_source_files_properties(${SF_GLUE}/nnue_incbin.cpp PROPERTIES
    COMPILE_OPTIONS "-Wa,-I${SF_DIR}" OBJECT_DEPENDS ${SF_NET})
target_compile_options(stockfish_embedded PRIVATE -O2 -Wall -Wextra)
target_include_directories(stockfish_embedded PRIVATE ${SF_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(stockfish_embedded PUBLIC ${SF_GLUE})
find_package(Threads REQUIRED)
target_link_libraries(stockfish_embedded PUBLIC Threads::Threads)
if(WIN32)
    target_sources(stockfish_embedded PRIVATE ${SF_GLUE}/win_shims.cpp)
    target_link_libraries(stockfish_embedded PUBLIC shell32)  # CommandLineToArgvW (misc.cpp)
endif()

# Audit of the final link (tools/isa_audit.py), part of the build when Python is available: a
# small program links the library like the game does (on Windows statically, so libstdc++ is in
# it too) next to ordinary code that uses the same standard library templates, and the build fails
# if its disassembly shows an instruction above baseline outside the variants, one above a
# variant's own level inside it, or any reference into a variant's code other than the
# dispatcher's.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_executable(stockfish_isa_probe ${CMAKE_CURRENT_SOURCE_DIR}/tools/isa_probe.cpp)
    target_link_libraries(stockfish_isa_probe PRIVATE stockfish_embedded)
    target_link_options(stockfish_isa_probe PRIVATE -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/stockfish_isa_probe.map)
    if(WIN32)
        target_link_options(stockfish_isa_probe PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/stockfish_isa_audit.ok)
    add_custom_command(OUTPUT ${stamp}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/isa_audit.py
                $<TARGET_FILE:stockfish_isa_probe> ${CMAKE_CURRENT_BINARY_DIR}/stockfish_isa_probe.map
                ${CMAKE_OBJDUMP} ${SF_AUDIT_LEVELS} --allow "\\(cpu\\.cpp\\.o(bj)?\\)"
                --report ${CMAKE_CURRENT_BINARY_DIR}/stockfish_isa_audit.txt
        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
        DEPENDS stockfish_isa_probe ${CMAKE_CURRENT_SOURCE_DIR}/tools/isa_audit.py
        COMMENT "Auditing the instruction sets of the Stockfish variants" VERBATIM)
    add_custom_target(stockfish_isa_audit ALL DEPENDS ${stamp})
else()
    message(STATUS "Python 3 not found: the Stockfish instruction-set audit is skipped")
endif()
