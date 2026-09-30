# Scacelith build script (not part of upstream Stockfish), run as
#   cmake -DTAG=<variant tag> -DENTRY=<entry symbol> -DOUT=<file.o> -DINPUTS=<obj|obj|...>
#         -DFORMAT=ELF|PE -DLD=... -DOBJCOPY=... -DOBJDUMP=... -DNM=... -P isolate.cmake
#
# Turns the object files of one instruction-set variant of Stockfish into ONE relocatable object
# whose only global symbols are ENTRY, sfinit_<TAG>_start and sfinit_<TAG>_end. Every variant
# compiles the same inline functions and C++ standard library templates (std::vector<...>,
# std::string, ...) with its own instruction set. Left alone, those copies are weak / COMDAT
# symbols that the linker merges program-wide: the game, or the baseline variant, could then call
# an AVX2 copy on a CPU without AVX2 and crash. Isolated:
#   1. partial link (ld -r), which resolves the variant's COMDAT groups inside the variant;
#   2. no COMDAT section left (ELF: --force-group-allocation; PE: the LINK_ONCE flag cleared on
#      every .text$ / .rdata$ / .data$ / .bss$ / .xdata$ / .pdata$ section, because COFF merges
#      COMDATs by name even when their symbol is local), so the final link cannot merge the
#      variant's copies with anyone else's;
#   3. the static initialisers (.init_array / .ctors) moved to the section sfinit_<TAG>, between
#      the symbols sfinit_<TAG>_start and sfinit_<TAG>_end: the C runtime no longer runs them at
#      start-up (they may use the variant's instructions), the dispatcher (scacelith/cpu.cpp)
#      runs those of the variant it selects;
#   4. every other defined symbol made local. Undefined references (libstdc++, the C runtime,
#      pthreads, Win32) stay external and bind to the single copies in the executable, so the
#      engine still uses the game's std::cin / std::cout.
# Then the result is verified, and the build fails if anything could leak or go missing: exactly
# those three globals, bounds that span the whole initialiser table, no symbol both defined and
# undefined, no COMDAT left, and no section this scheme does not handle (initialiser priorities,
# destructor tables, thread-local storage).
cmake_minimum_required(VERSION 3.20)
string(REPLACE "|" ";" INPUTS "${INPUTS}")

# A failure removes the output, so that the next build runs the script again.
function(fail message)
    file(REMOVE ${OUT})
    message(FATAL_ERROR "isolate(${TAG}): ${message}")
endfunction()

function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        fail("'${ARGV}' failed: ${err}")
    endif()
    set(RUN_OUT "${out}" PARENT_SCOPE)
endfunction()

# 1-2. Partial link.
set(linked "${OUT}.r.o")
set(renamed "${OUT}.s.o")
if(FORMAT STREQUAL "ELF")
    run(${LD} -r --force-group-allocation -o ${linked} ${INPUTS})
else()
    # One ld -r leaves, for each COMDAT whose duplicate it dropped, an undefined symbol next to
    # the kept definition; they would only be matched in the final link, after step 4 made the
    # definition invisible. A second ld -r over the single object merges them.
    run(${LD} -r -o ${linked}.0 ${INPUTS})
    run(${LD} -r -o ${linked} ${linked}.0)
    file(REMOVE ${linked}.0)
endif()

# 3. Find the initialiser table (the section index may have 3 digits).
run(${OBJDUMP} -h ${linked})
string(REPLACE "\n" ";" lines "${RUN_OUT}")
set(initSection "")
foreach(line IN LISTS lines)
    if(line MATCHES "^ *[0-9]+ ([^ ]+) +([0-9a-f]+) ")
        set(name "${CMAKE_MATCH_1}")
        set(size "${CMAKE_MATCH_2}")  # before the next MATCHES overwrites CMAKE_MATCH_<n>
        if(name MATCHES "^\\.(init_array|ctors)$")
            if(NOT initSection STREQUAL "")
                fail("several initialiser sections")
            endif()
            set(initSection "${name}")
            set(initSize "0x${size}")
        elseif(name MATCHES "^\\.(init_array\\.|ctors\\.|fini_array|dtors|tbss|tdata|tls)")
            fail("unsupported section ${name} (initialiser priorities, destructor tables or TLS)")
        endif()
    endif()
endforeach()
if(initSection STREQUAL "")
    fail("no static initialisers found (Stockfish has some: the object list is wrong)")
endif()

# The section stays writable data: read-only would ask the linker for text relocations.
set(args --rename-section ${initSection}=sfinit_${TAG},contents,alloc,load,data
         --add-symbol sfinit_${TAG}_start=sfinit_${TAG}:0,global
         --add-symbol sfinit_${TAG}_end=sfinit_${TAG}:${initSize},global)
if(FORMAT STREQUAL "PE")
    list(APPEND args
         --set-section-flags ".text$*=contents,alloc,load,readonly,code"
         --set-section-flags ".rdata$*=contents,alloc,load,readonly,data"
         --set-section-flags ".xdata$*=contents,alloc,load,readonly,data"
         --set-section-flags ".pdata$*=contents,alloc,load,readonly,data"
         --set-section-flags ".data$*=contents,alloc,load,data"
         --set-section-flags ".bss$*=alloc")
endif()
run(${OBJCOPY} ${args} ${linked} ${renamed})

# 4. Localise everything else.
run(${OBJCOPY} --keep-global-symbol=${ENTRY} --keep-global-symbol=sfinit_${TAG}_start
               --keep-global-symbol=sfinit_${TAG}_end ${renamed} ${OUT})
file(REMOVE ${linked} ${renamed})

# Verification: exactly the three globals.
run(${NM} --defined-only -g ${OUT})
string(REGEX MATCHALL "[^\n]+" symbols "${RUN_OUT}")
set(names "")
foreach(symbol IN LISTS symbols)
    string(REGEX REPLACE "^.* " "" name "${symbol}")
    list(APPEND names "${name}")
endforeach()
list(SORT names)
set(expected ${ENTRY} sfinit_${TAG}_end sfinit_${TAG}_start)
list(SORT expected)
if(NOT names STREQUAL expected)
    fail("unexpected global symbols: ${names}")
endif()
# The bounds span the whole initialiser table (the dispatcher runs what lies between them).
foreach(symbol IN LISTS symbols)
    if(symbol MATCHES "^([0-9a-f]+) [A-Za-z] sfinit_${TAG}_(start|end)$")
        set(bound_${CMAKE_MATCH_2} "0x${CMAKE_MATCH_1}")
    endif()
endforeach()
math(EXPR tableSize "${bound_end} - ${bound_start}")
math(EXPR expectedSize "${initSize}")
if(tableSize EQUAL 0 OR NOT tableSize EQUAL expectedSize)
    fail("the initialiser table bounds ${bound_start}..${bound_end} do not span its ${initSize} bytes")
endif()
# No symbol both undefined and defined: it would stay unresolved, or bind to another copy.
run(${NM} ${OUT})
string(REGEX MATCHALL "[^\n]+" symbols "${RUN_OUT}")
set(undefined "")
set(defined "")
foreach(symbol IN LISTS symbols)
    if(symbol MATCHES "^ +U (.+)$")
        list(APPEND undefined "${CMAKE_MATCH_1}")
    elseif(symbol MATCHES "^[0-9a-f]+ [A-Za-z] (.+)$")
        list(APPEND defined "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES undefined)
list(REMOVE_DUPLICATES defined)
set(both ${undefined} ${defined})
list(LENGTH both total)
list(REMOVE_DUPLICATES both)
list(LENGTH both distinct)
if(NOT total EQUAL distinct)
    math(EXPR n "${total} - ${distinct}")
    fail("${n} symbols are both undefined and defined")
endif()
# No COMDAT left.
run(${OBJDUMP} -h ${OUT})
if(RUN_OUT MATCHES "LINK_ONCE|GROUP")
    fail("COMDAT sections left")
endif()
