# Supertonic 3 model files for the coach's voice (src/tts), for development builds only. The game
# never ships them (the weights are under OpenRAIL-M): it downloads them itself into
# <application data>/coach/ (src/tts/model_store.h). This step gives developers a copy in
# ${CMAKE_BINARY_DIR}/coach/ for the unit tests and for runs with --coach-dir build/coach: it takes
# the files from a local copy of the Hugging Face repository or downloads them from it one by one
# (each checked against its SHA-256), and verifies every file. Without them the build still
# succeeds; the TTS tests that need the model are skipped. See README.scacelith.md in this folder.
#
#   -DSCACELITH_SUPERTONIC_DIR=/path/to/supertonic-3   (a copy of the repository: onnx/, voice_styles/;
#                                                       or a folder holding the files themselves)
#   -DSCACELITH_SUPERTONIC_DOWNLOAD=OFF   (never download; default ON)
# The first defaults to the environment variable of the same name. The release package holds the
# executable only.

# Supertone's repository at the revision its Python SDK pins (supertonic 1.3.1), then Supertone's
# archive copy at the revision its GitHub README pins: the same files (tts::supertonicManifest()).
set(SUPERTONIC3_SOURCES
    https://huggingface.co/Supertone/supertonic-3/resolve/724fb5abbf5502583fb520898d45929e62f02c0b
    https://huggingface.co/supertone-oss-archive/supertonic-3/resolve/aafc6e32416a594460b32413efc49d7fe4ce6d46)
# The seven files of the model (the list the game downloads, src/tts/model_store.cpp): the name in
# the model folder, the path in the repository, the SHA-256.
set(SUPERTONIC3_FILES
    tts.json onnx/tts.json 42078d3aef1cd43ab43021f3c54f47d2d75ceb4e75f627f118890128b06a0d09
    unicode_indexer.json onnx/unicode_indexer.json 9bf7346e43883a81f8645c81224f786d43c5b57f3641f6e7671a7d6c493cb24f
    M3.json voice_styles/M3.json ea1ac35ccb91b0d7ecad533a2fbd0eec10c91513d8951e3b25fbba99954e159b
    duration_predictor.onnx onnx/duration_predictor.onnx c3eb91414d5ff8a7a239b7fe9e34e7e2bf8a8140d8375ffb14718b1c639325db
    text_encoder.onnx onnx/text_encoder.onnx c7befd5ea8c3119769e8a6c1486c4edc6a3bc8365c67621c881bbb774b9902ff
    vector_estimator.onnx onnx/vector_estimator.onnx 883ac868ea0275ef0e991524dc64f16b3c0376efd7c320af6b53f5b780d7c61c
    vocoder.onnx onnx/vocoder.onnx 085de76dd8e8d5836d6ca66826601f615939218f90e519f70ee8a36ed2a4c4ba)

set(SCACELITH_SUPERTONIC_DIR "$ENV{SCACELITH_SUPERTONIC_DIR}" CACHE PATH
    "Local copy of the Supertonic 3 repository (empty: download the files into the build folder)")
option(SCACELITH_SUPERTONIC_DOWNLOAD "Download the Supertonic 3 model files when no local copy is given" ON)

set(SUPERTONIC3_OUT ${CMAKE_BINARY_DIR}/coach)
set(SUPERTONIC3_AVAILABLE OFF)

function(supertonic3_prepare)
    set(deps ${CMAKE_BINARY_DIR}/_deps/supertonic-3)
    file(MAKE_DIRECTORY ${SUPERTONIC3_OUT})
    set(src_used "")
    set(rows ${SUPERTONIC3_FILES})
    while(rows)
        list(POP_FRONT rows name remote expected)
        # 1. Where the file comes from: the local copy (repository layout, else flat), else the
        # download cache of the build folder, filled from the first source that answers.
        set(src "")
        if(SCACELITH_SUPERTONIC_DIR)
            if(EXISTS ${SCACELITH_SUPERTONIC_DIR}/${remote})
                set(src ${SCACELITH_SUPERTONIC_DIR}/${remote})
            elseif(EXISTS ${SCACELITH_SUPERTONIC_DIR}/${name})
                set(src ${SCACELITH_SUPERTONIC_DIR}/${name})
            endif()
            set(src_used ${SCACELITH_SUPERTONIC_DIR})
        else()
            set(src ${deps}/${name})
            set(src_used ${deps})
            if(NOT EXISTS ${src} AND SCACELITH_SUPERTONIC_DOWNLOAD)
                file(MAKE_DIRECTORY ${deps})
                foreach(base ${SUPERTONIC3_SOURCES})
                    message(STATUS "Supertonic 3: downloading ${base}/${remote}")
                    # No EXPECTED_HASH: it makes a failed transfer a configure error instead of the
                    # warning below. The digest is checked once the file is complete.
                    file(DOWNLOAD ${base}/${remote} ${src}.part STATUS status TLS_VERIFY ON)
                    list(GET status 0 code)
                    if(code EQUAL 0)
                        file(SHA256 ${src}.part hash)
                        if(hash STREQUAL expected)
                            file(RENAME ${src}.part ${src})
                            break()
                        endif()
                        message(WARNING "Supertonic 3: ${base}/${remote} has SHA-256 ${hash}, expected ${expected}")
                    else()
                        list(GET status 1 reason)
                        message(WARNING "Supertonic 3: ${base}/${remote}: download failed (${reason})")
                    endif()
                    file(REMOVE ${src}.part)
                endforeach()
            endif()
        endif()
        if(NOT src OR NOT EXISTS ${src})
            message(WARNING "Supertonic 3: ${name} (${remote}) is missing")
            return()
        endif()
        # 2. Verify and copy it (again only when the source changed).
        if(NOT EXISTS ${SUPERTONIC3_OUT}/${name} OR ${src} IS_NEWER_THAN ${SUPERTONIC3_OUT}/${name})
            file(SHA256 ${src} hash)
            if(NOT hash STREQUAL expected)
                message(FATAL_ERROR "Supertonic 3: ${src} has SHA-256 ${hash}, expected ${expected}")
            endif()
            file(COPY_FILE ${src} ${SUPERTONIC3_OUT}/${name})
        endif()
    endwhile()
    # The notices the game writes beside the files it downloads (tts::writeFolderNotices): the model
    # licence (Attachment A use restrictions) and the folder README.
    file(COPY_FILE ${ROOT}/assets/licences/Supertonic-3-OpenRAIL-M.txt ${SUPERTONIC3_OUT}/Supertonic-3-OpenRAIL-M.txt
         ONLY_IF_DIFFERENT)
    file(READ ${ROOT}/assets/tts/coach-folder-README.txt readme)
    string(REPLACE "{source}" "This copy was prepared by a development build of Scacelith from ${src_used}." readme "${readme}")
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
                    "(the game downloads them itself). Set SCACELITH_SUPERTONIC_DIR "
                    "(see third_party/supertonic3/README.scacelith.md).")
endif()
