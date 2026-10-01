# Supertonic 3 INT8 model files for the coach's voice (src/tts), for development builds only. The
# game never ships them (the weights are under OpenRAIL-M): it downloads them itself into
# <application data>/coach/ (src/tts/model_store.h). This step gives developers a copy in
# ${CMAKE_BINARY_DIR}/coach/ for the unit tests and for runs with --coach-dir build/coach: it takes
# the files from a local folder, a local copy of the release archive or a download of it (checked
# against its SHA-256), and verifies every file. Without them the build still succeeds; the TTS
# tests that need the model are skipped. See README.scacelith.md in this folder.
#
#   -DSCACELITH_SUPERTONIC_DIR=/path/to/sherpa-onnx-supertonic-3-tts-int8-2026-05-11   (extracted)
#   -DSCACELITH_SUPERTONIC_ARCHIVE=/path/to/sherpa-onnx-supertonic-3-tts-int8-2026-05-11.tar.bz2
#   -DSCACELITH_SUPERTONIC_DOWNLOAD=OFF   (never download; default ON)
# The first two default to the environment variables of the same names. The release package holds
# the executable only.

set(SUPERTONIC3_NAME sherpa-onnx-supertonic-3-tts-int8-2026-05-11)
set(SUPERTONIC3_URL https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/${SUPERTONIC3_NAME}.tar.bz2)
set(SUPERTONIC3_SHA256 82fa96f91c4ef8abaae3a14a3f4153facf88bed821d1f7331cec2700f432c427)
# The nine files of the model (tts::supertonicManifest(), the list the game downloads) and their
# SHA-256; the runtime reads the last six (tts::Engine::kFiles).
set(SUPERTONIC3_FILES
    LICENSE 0dfe0d0ba84416fe3879d9a34f4909d8d0137c78d1e95834177b0414ac096fa2
    README.md a96c347945f7c8bc1673bea3525b1ac8d36fdde556e1e0a6a186052429caf863
    tts.json 42078d3aef1cd43ab43021f3c54f47d2d75ceb4e75f627f118890128b06a0d09
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
    # The notices the game writes beside the files it downloads (tts::writeFolderNotices): the model
    # licence (Attachment A use restrictions) and the folder README.
    file(COPY_FILE ${ROOT}/assets/licences/Supertonic-3-OpenRAIL-M.txt ${SUPERTONIC3_OUT}/Supertonic-3-OpenRAIL-M.txt
         ONLY_IF_DIFFERENT)
    file(READ ${ROOT}/assets/tts/coach-folder-README.txt readme)
    string(REPLACE "{source}" "This copy was prepared by a development build of Scacelith from ${src}." readme "${readme}")
    file(WRITE ${SUPERTONIC3_OUT}/README.txt.tmp "${readme}")
    file(COPY_FILE ${SUPERTONIC3_OUT}/README.txt.tmp ${SUPERTONIC3_OUT}/README.txt ONLY_IF_DIFFERENT)
    file(REMOVE ${SUPERTONIC3_OUT}/README.txt.tmp)
    set(SUPERTONIC3_AVAILABLE ON PARENT_SCOPE)
endfunction()

supertonic3_prepare()
if(SUPERTONIC3_AVAILABLE)
    message(STATUS "Supertonic 3: model files in ${SUPERTONIC3_OUT}")
else()
    message(WARNING "Supertonic 3: no development copy of the model files; the TTS tests that need them are skipped "
                    "(the game downloads them itself). Set SCACELITH_SUPERTONIC_DIR or SCACELITH_SUPERTONIC_ARCHIVE "
                    "(see third_party/supertonic3/README.scacelith.md).")
endif()
