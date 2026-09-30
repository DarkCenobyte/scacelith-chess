# Supertonic 3 INT8 model files for the coach's voice (src/tts). The weights are under the
# OpenRAIL-M licence and are not in git: this step takes them from a local folder, a local copy of
# the release archive or a download of it (checked against its SHA-256), verifies every file and
# copies them to ${CMAKE_BINARY_DIR}/coach/, the folder the game reads beside its executable.
# Without them the build still succeeds and the coach speaks with subtitles only.
# See README.scacelith.md in this folder.
#
#   -DSCACELITH_SUPERTONIC_DIR=/path/to/sherpa-onnx-supertonic-3-tts-int8-2026-05-11   (extracted)
#   -DSCACELITH_SUPERTONIC_ARCHIVE=/path/to/sherpa-onnx-supertonic-3-tts-int8-2026-05-11.tar.bz2
#   -DSCACELITH_SUPERTONIC_DOWNLOAD=OFF   (never download; default ON)
#   -DSCACELITH_TTS_EMBED=ON              (embed the files in the executables instead; default OFF)
# The first two default to the environment variables of the same names.

set(SUPERTONIC3_NAME sherpa-onnx-supertonic-3-tts-int8-2026-05-11)
set(SUPERTONIC3_URL https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/${SUPERTONIC3_NAME}.tar.bz2)
set(SUPERTONIC3_SHA256 82fa96f91c4ef8abaae3a14a3f4153facf88bed821d1f7331cec2700f432c427)
# The files the runtime reads (the order of tts::Engine::kFiles) and their SHA-256.
set(SUPERTONIC3_FILES
    duration_predictor.int8.onnx c3eb91414d5ff8a7a239b7fe9e34e7e2bf8a8140d8375ffb14718b1c639325db
    text_encoder.int8.onnx c7befd5ea8c3119769e8a6c1486c4edc6a3bc8365c67621c881bbb774b9902ff
    vector_estimator.int8.onnx 20cd86fa5c6effedfda0e7cffe5b0569ca401c440a0c3a1d72bf39286c0db3fd
    vocoder.int8.onnx e923d60f53f95eb1ce235f1dc33ec56d9c057823c96fa6f8acf98f32b0da6152
    unicode_indexer.bin 8402ca48e5189a8950138580b0fff64db6f072f24ac07cd54ba8b2fbb9883b30
    voice.bin 67d5209b0ee8ce6c74105ffbe12fe6a7628aea3b4ba2fcb308a4a67938a93ce8)

set(SCACELITH_SUPERTONIC_DIR "$ENV{SCACELITH_SUPERTONIC_DIR}" CACHE PATH
    "Folder of the extracted ${SUPERTONIC3_NAME} release (empty: use the archive)")
set(SCACELITH_SUPERTONIC_ARCHIVE "$ENV{SCACELITH_SUPERTONIC_ARCHIVE}" CACHE FILEPATH
    "Local copy of ${SUPERTONIC3_NAME}.tar.bz2 (empty: download it into the build folder)")
option(SCACELITH_SUPERTONIC_DOWNLOAD "Download the Supertonic 3 model archive when no local copy is given" ON)
option(SCACELITH_TTS_EMBED "Embed the Supertonic 3 model files in the executables instead of the coach/ folder" OFF)

set(SUPERTONIC3_OUT ${CMAKE_BINARY_DIR}/coach)
set(SUPERTONIC3_AVAILABLE OFF)

function(supertonic3_prepare)
    # 1. Where the release files come from.
    set(src "")
    if(SCACELITH_SUPERTONIC_DIR)
        set(src ${SCACELITH_SUPERTONIC_DIR})
    else()
        set(deps ${CMAKE_BINARY_DIR}/_deps)
        set(archive ${SCACELITH_SUPERTONIC_ARCHIVE})
        if(NOT archive)
            set(archive ${deps}/${SUPERTONIC3_NAME}.tar.bz2)
            if(NOT EXISTS ${archive} AND SCACELITH_SUPERTONIC_DOWNLOAD)
                message(STATUS "Supertonic 3: downloading ${SUPERTONIC3_URL}")
                file(DOWNLOAD ${SUPERTONIC3_URL} ${archive}.part STATUS status
                     EXPECTED_HASH SHA256=${SUPERTONIC3_SHA256} TLS_VERIFY ON)
                list(GET status 0 code)
                if(code EQUAL 0)
                    file(RENAME ${archive}.part ${archive})
                else()
                    file(REMOVE ${archive}.part)
                    list(GET status 1 reason)
                    message(WARNING "Supertonic 3: download failed (${reason})")
                endif()
            endif()
        endif()
        if(NOT EXISTS ${archive})
            return()
        endif()
        set(extracted ${deps}/${SUPERTONIC3_NAME})
        set(stamp ${deps}/${SUPERTONIC3_NAME}.extracted)
        if(NOT EXISTS ${stamp} OR ${archive} IS_NEWER_THAN ${stamp})
            file(SHA256 ${archive} hash)
            if(NOT hash STREQUAL SUPERTONIC3_SHA256)
                message(FATAL_ERROR "Supertonic 3: ${archive} has SHA-256 ${hash}, expected ${SUPERTONIC3_SHA256}")
            endif()
            file(REMOVE_RECURSE ${extracted})
            file(ARCHIVE_EXTRACT INPUT ${archive} DESTINATION ${deps})
            file(TOUCH ${stamp})
        endif()
        set(src ${extracted})
    endif()

    # 2. Verify and copy the files the runtime needs (again only when the source changed).
    file(MAKE_DIRECTORY ${SUPERTONIC3_OUT})
    set(pairs ${SUPERTONIC3_FILES})
    while(pairs)
        list(POP_FRONT pairs name expected)
        if(NOT EXISTS ${src}/${name})
            message(WARNING "Supertonic 3: ${src}/${name} is missing")
            return()
        endif()
        if(NOT EXISTS ${SUPERTONIC3_OUT}/${name} OR ${src}/${name} IS_NEWER_THAN ${SUPERTONIC3_OUT}/${name})
            file(SHA256 ${src}/${name} hash)
            if(NOT hash STREQUAL expected)
                message(FATAL_ERROR "Supertonic 3: ${src}/${name} has SHA-256 ${hash}, expected ${expected}")
            endif()
            file(COPY_FILE ${src}/${name} ${SUPERTONIC3_OUT}/${name})
        endif()
    endwhile()
    # The model licence travels with the weights (Attachment A use restrictions).
    file(COPY_FILE ${ROOT}/assets/licences/Supertonic-3-OpenRAIL-M.txt ${SUPERTONIC3_OUT}/Supertonic-3-OpenRAIL-M.txt
         ONLY_IF_DIFFERENT)
    file(COPY_FILE ${CMAKE_CURRENT_LIST_DIR}/coach-folder-README.txt ${SUPERTONIC3_OUT}/README.txt ONLY_IF_DIFFERENT)
    set(SUPERTONIC3_AVAILABLE ON PARENT_SCOPE)
endfunction()

supertonic3_prepare()
if(SUPERTONIC3_AVAILABLE)
    message(STATUS "Supertonic 3: model files in ${SUPERTONIC3_OUT}")
else()
    message(WARNING "Supertonic 3: model files not available; the coach will run with subtitles only. "
                    "Set SCACELITH_SUPERTONIC_DIR or SCACELITH_SUPERTONIC_ARCHIVE (see third_party/supertonic3/README.scacelith.md).")
endif()

# Optional embedding (the files are still copied to coach/, which takes precedence at run time).
if(SCACELITH_TTS_EMBED)
    if(NOT SUPERTONIC3_AVAILABLE)
        message(FATAL_ERROR "SCACELITH_TTS_EMBED needs the Supertonic 3 model files")
    endif()
    set(asm "")
    set(table "")
    set(idx 0)
    set(pairs ${SUPERTONIC3_FILES})
    set(deps_files "")
    while(pairs)
        list(POP_FRONT pairs name expected)
        set(f ${SUPERTONIC3_OUT}/${name})
        list(APPEND deps_files ${f})
        set(sym "scacelith_tts_${idx}")
        string(APPEND asm "    \".balign 64\\n\"\n    \".globl ${sym}\\n\"\n    \"${sym}:\\n\"\n    \".incbin \\\"${f}\\\"\\n\"\n    \".globl ${sym}_end\\n\"\n    \"${sym}_end:\\n\"\n")
        string(APPEND decl "extern \"C\" const unsigned char ${sym}[];\nextern \"C\" const unsigned char ${sym}_end[];\n")
        string(APPEND table "    {${sym}, ${sym}_end},\n")
        math(EXPR idx "${idx} + 1")
    endwhile()
    set(content "// Generated by third_party/supertonic3/supertonic3.cmake -- do not edit.\n#include <cstddef>\n#include <cstdint>\n\n")
    string(APPEND content "#if defined(_WIN32)\n#define TTS_SECTION \".section .rdata,\\\"dr\\\"\\n\"\n#else\n#define TTS_SECTION \".section .rodata\\n\"\n#endif\n")
    string(APPEND content "__asm__(\n    TTS_SECTION\n${asm}    \".text\\n\");\n\n${decl}\n")
    string(APPEND content "namespace tts {\nnamespace {\nstruct Range { const unsigned char* begin; const unsigned char* end; };\nconst Range kFiles[] = {\n${table}};\n}  // namespace\n\n")
    string(APPEND content "bool embeddedModelFile(int index, const uint8_t** data, size_t* size) {\n    if (index < 0 || index >= ${idx}) return false;\n    *data = kFiles[index].begin;\n    *size = size_t(kFiles[index].end - kFiles[index].begin);\n    return true;\n}\n}  // namespace tts\n")
    set(out_cpp ${CMAKE_BINARY_DIR}/tts_embedded.cpp)
    file(WRITE ${out_cpp}.tmp "${content}")
    file(COPY_FILE ${out_cpp}.tmp ${out_cpp} ONLY_IF_DIFFERENT)
    set_source_files_properties(${out_cpp} PROPERTIES OBJECT_DEPENDS "${deps_files}")
    target_sources(scacelith_core PRIVATE ${out_cpp})
    set_property(SOURCE ${ROOT}/src/tts/tts.cpp APPEND PROPERTY COMPILE_DEFINITIONS SCACELITH_TTS_EMBEDDED=1)
    message(STATUS "Supertonic 3: model files embedded in the executables")
endif()
