cmake_minimum_required(VERSION 3.20)
project(Scacelith VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------------------------
# Build configuration
#   Windows (shipping):  cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
#   Linux (tests only):  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   Quicker local builds: add -DSCACELITH_SF_VARIANTS=x86-64-avx2 (or another variant this CPU
#   runs) to compile Stockfish once instead of once per instruction set (third_party/stockfish)
# ---------------------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(SCACELITH_CONSOLE "Windows: build a console-subsystem exe (logs visible)" OFF)
option(SCACELITH_STOCKFISH "Embed the Stockfish engine" ON)
set(SCACELITH_OFFICIAL_SERVER "" CACHE STRING "Official online server: host[:apiPort[:wsPort]] (empty = caissa.scacelith.com:44664, none = no official server)")

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    enable_language(RC)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
include(cmake/embed.cmake)

set(COMMON_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $<$<CXX_COMPILER_ID:GNU>:-Wno-dangling-reference>)
set(OPT_FLAGS $<$<CONFIG:Release>:-O2> $<$<CONFIG:RelWithDebInfo>:-O2 -g>)

# ---------------------------------------------------------------------------------------------
# Embedded data (shaders, fonts, ...) -> generated incbin table
# ---------------------------------------------------------------------------------------------
set(EMBED_CPP ${CMAKE_CURRENT_BINARY_DIR}/embedded_files.cpp)
scacelith_embed_files(${EMBED_CPP} ${ROOT} shaders assets)

# ---------------------------------------------------------------------------------------------
# Stockfish (GPL-3.0), compiled into the executable. See third_party/stockfish/README.scacelith.md
# ---------------------------------------------------------------------------------------------
if(SCACELITH_STOCKFISH AND EXISTS ${ROOT}/third_party/stockfish/CMakeLists.txt)
    add_subdirectory(third_party/stockfish)
endif()

# ---------------------------------------------------------------------------------------------
# Engine-free modules shared by the game and the unit tests
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    ${ROOT}/src/core/*.cpp ${ROOT}/src/math/*.cpp ${ROOT}/src/chess/*.cpp ${ROOT}/src/ai/*.cpp ${ROOT}/src/audio/*.cpp
    ${ROOT}/src/i18n/*.cpp ${ROOT}/src/net/*.cpp ${ROOT}/src/coach/*.cpp ${ROOT}/src/tts/*.cpp)
# Engine-free parts of src/game (Elo, observer camera, camera flights, hot-seat handover, scoresheet
# layout, physical board, the [coach] and [tts] settings): in the core library so the unit tests
# can use them, and left out of the game sources below.
set(GAME_CORE_SOURCES ${ROOT}/src/game/elo.cpp ${ROOT}/src/game/camera_flight.cpp ${ROOT}/src/game/observer_camera.cpp
    ${ROOT}/src/game/hotseat.cpp ${ROOT}/src/game/scoresheet_layout.cpp ${ROOT}/src/game/physical_board.cpp
    ${ROOT}/src/game/settings_coach.cpp)
list(APPEND CORE_SOURCES ${GAME_CORE_SOURCES})
add_library(scacelith_core STATIC ${CORE_SOURCES} ${EMBED_CPP})
target_include_directories(scacelith_core PUBLIC ${ROOT}/src ${ROOT}/third_party)
target_compile_options(scacelith_core PRIVATE ${COMMON_WARNINGS} ${OPT_FLAGS})
if(TARGET stockfish_embedded)
    target_link_libraries(scacelith_core PUBLIC stockfish_embedded)
    target_compile_definitions(scacelith_core PUBLIC SCACELITH_HAS_STOCKFISH=1)
endif()
if(WIN32)
    target_link_libraries(scacelith_core PUBLIC ole32 uuid winmm avrt ksuser)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(scacelith_core PUBLIC Threads::Threads)
endif()
# Text-to-speech (src/tts): the compute kernels are compiled once per instruction set and chosen at
# run time (src/tts/kernels.h); everything else is baseline x86-64. MinGW does not align the stack
# to 32 bytes for AVX spills (GCC bug 54412): the assembler turns aligned vector moves into
# unaligned ones in those units. The model files are prepared by third_party/supertonic3.
set(TTS_AVX2_FLAGS -O3 -mavx2 -mfma)
set(TTS_AVXVNNI_FLAGS ${TTS_AVX2_FLAGS} -mavxvnni)
set(TTS_AVX512_FLAGS ${TTS_AVX2_FLAGS} -mavx512f -mavx512bw -mavx512vl -mavx512dq -mavx512vnni)
if(WIN32)
    foreach(v TTS_AVX2_FLAGS TTS_AVXVNNI_FLAGS TTS_AVX512_FLAGS)
        list(APPEND ${v} -Wa,-muse-unaligned-vector-move)
    endforeach()
endif()
set_source_files_properties(${ROOT}/src/tts/kernels_sse2.cpp PROPERTIES COMPILE_OPTIONS "-O3")
set_source_files_properties(${ROOT}/src/tts/kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "${TTS_AVX2_FLAGS}")
set_source_files_properties(${ROOT}/src/tts/kernels_avxvnni.cpp PROPERTIES COMPILE_OPTIONS "${TTS_AVXVNNI_FLAGS}")
set_source_files_properties(${ROOT}/src/tts/kernels_avx512.cpp PROPERTIES COMPILE_OPTIONS "${TTS_AVX512_FLAGS}")
# Audit of those units (Stockfish's tools/isa_audit.py, part of the build when Python is available):
# a small program links the TTS runtime like the game does, and the build fails if its disassembly
# shows an instruction above baseline outside the kernel units, one above a unit's level inside it,
# or a reference into a unit other than from the dispatcher (src/tts/cpu.cpp).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND EXISTS ${ROOT}/third_party/stockfish/tools/isa_audit.py)
    add_executable(tts_isa_probe ${ROOT}/tools/tts_isa_probe.cpp)
    target_link_libraries(tts_isa_probe PRIVATE scacelith_core)
    target_link_options(tts_isa_probe PRIVATE -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/tts_isa_probe.map)
    if(WIN32)
        target_link_options(tts_isa_probe PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/tts_isa_audit.ok)
    add_custom_command(OUTPUT ${stamp}
        COMMAND ${Python3_EXECUTABLE} ${ROOT}/third_party/stockfish/tools/isa_audit.py
                $<TARGET_FILE:tts_isa_probe> ${CMAKE_CURRENT_BINARY_DIR}/tts_isa_probe.map ${CMAKE_OBJDUMP}
                "\\(kernels_avx2\\.cpp\\.o(bj)?\\)=2" "\\(kernels_avxvnni\\.cpp\\.o(bj)?\\)=2"
                "\\(kernels_avx512\\.cpp\\.o(bj)?\\)=3" --allow "scacelith_core\\.a\\(cpu\\.cpp\\.o(bj)?\\)"
                --report ${CMAKE_CURRENT_BINARY_DIR}/tts_isa_audit.txt
        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
        DEPENDS tts_isa_probe ${ROOT}/third_party/stockfish/tools/isa_audit.py
        COMMENT "Auditing the instruction sets of the TTS kernels" VERBATIM)
    add_custom_target(tts_isa_audit ALL DEPENDS ${stamp})
else()
    message(STATUS "Python 3 not found: the TTS instruction-set audit is skipped")
endif()
include(third_party/supertonic3/supertonic3.cmake)
# Networking (src/net): online client (WinHTTP, DPAPI, the shell for SSO), direct match (Winsock,
# BCrypt ECDH P-256 / AES-GCM / SHA-256, adapter list for iphlpapi) and UPnP on Windows. Linux
# development builds use OpenSSL: required by the direct match; the online client also builds
# without it (online play then reports "unavailable").
set_source_files_properties(${ROOT}/src/net/online_client.cpp PROPERTIES COMPILE_DEFINITIONS
    "SCACELITH_OFFICIAL_SERVER=\"${SCACELITH_OFFICIAL_SERVER}\";SCACELITH_VERSION_STRING=\"${PROJECT_VERSION}\"")
set_source_files_properties(${ROOT}/tests/net_tests.cpp PROPERTIES COMPILE_DEFINITIONS
    "SCACELITH_OFFICIAL_SERVER=\"${SCACELITH_OFFICIAL_SERVER}\"")
if(WIN32)
    target_link_libraries(scacelith_core PUBLIC winhttp crypt32 bcrypt ws2_32 iphlpapi shell32)
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(scacelith_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(scacelith_core PRIVATE SCACELITH_HAS_OPENSSL=1)
endif()
# The real network layer replaces the game's offline stand-in (src/game/online_stub.cpp); the
# in-process fakes stay available with --online-mock.
target_compile_definitions(scacelith_core PUBLIC SCACELITH_NET_REAL=1)

# ---------------------------------------------------------------------------------------------
# Game executable. Every .cpp under src/ (except the core modules above) is compiled directly
# into the exe so static registration (scenes) is never dropped by the linker.
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    ${ROOT}/src/platform/*.cpp ${ROOT}/src/gl/*.cpp ${ROOT}/src/render/*.cpp ${ROOT}/src/scene/*.cpp
    ${ROOT}/src/character/*.cpp ${ROOT}/src/anim/*.cpp ${ROOT}/src/ui/*.cpp ${ROOT}/src/game/*.cpp
    ${ROOT}/src/app/*.cpp)
list(REMOVE_ITEM GAME_SOURCES ${GAME_CORE_SOURCES})
set(GAME_EXTRA)
if(WIN32)
    list(APPEND GAME_EXTRA ${ROOT}/res/scacelith.rc)
endif()
add_executable(scacelith ${ROOT}/src/main.cpp ${GAME_SOURCES} ${GAME_EXTRA})
target_compile_options(scacelith PRIVATE ${COMMON_WARNINGS} ${OPT_FLAGS})
target_link_libraries(scacelith PRIVATE scacelith_core)
if(WIN32)
    target_link_libraries(scacelith PRIVATE opengl32 gdi32 user32 shell32 dwmapi)
    target_link_options(scacelith PRIVATE -static -static-libgcc -static-libstdc++)
    if(NOT SCACELITH_CONSOLE)
        target_link_options(scacelith PRIVATE -mwindows)
    endif()
    set_target_properties(scacelith PROPERTIES OUTPUT_NAME "Scacelith")
else()
    find_package(X11 REQUIRED)
    target_link_libraries(scacelith PRIVATE X11 GL dl)
endif()

# ---------------------------------------------------------------------------------------------
# Unit tests (no GPU): tests/*.cpp linked against the core modules. Run: ./scacelith_tests
# ---------------------------------------------------------------------------------------------
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${ROOT}/tests/*.cpp)
if(TEST_SOURCES)
    add_executable(scacelith_tests ${TEST_SOURCES})
    target_compile_options(scacelith_tests PRIVATE ${COMMON_WARNINGS} ${OPT_FLAGS})
    target_link_libraries(scacelith_tests PRIVATE scacelith_core)
    if(WIN32)
        target_link_options(scacelith_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()
