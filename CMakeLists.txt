# harmonyrecomp - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(harmonyrecomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(HARMONYRECOMP_SOURCES
    src/main.cpp
    src/graphics_menu.cpp
    src/crash_log.cpp
    src/dlc.cpp
    src/game_fixes.cpp
    src/netplay/gamesock.cpp
    src/netplay/hooks.cpp
    src/netplay/lobby.cpp
    src/netplay/netplay.cpp
    src/netplay/sessions.cpp
    src/netplay/udp.cpp
)

if(WIN32)
    add_executable(harmonyrecomp WIN32 ${HARMONYRECOMP_SOURCES})
else()
    add_executable(harmonyrecomp ${HARMONYRECOMP_SOURCES})
endif()

rexglue_setup_target(harmonyrecomp)

# Netplay lobby service (src/netplay/udp.cpp): Winsock and adapter enumeration.
if(WIN32)
    target_link_libraries(harmonyrecomp PRIVATE ws2_32 iphlpapi)
    # Symbol map, to resolve the offsets in logs/crash_*.txt.
    target_link_options(harmonyrecomp PRIVATE "LINKER:/MAP:harmonyrecomp.map")
endif()

# Move the main module's indirect-call table out of the way of the player DLLs.
# By default it sits right after the image (0x82960000) and is ~13 MB, which
# would cover 0x83000000-0x835F0000 where the game loads dllAlucard.dll and the
# other characters. 0x8C000000-0x8CD5E990 is free in the 0x80000000 guest heap.
# Needs the table_base SDK change applied by scripts/build.py.
target_compile_definitions(harmonyrecomp PRIVATE REX_FUNC_TABLE_BASE=0x8C000000ull)

# Give every DLL module its own indirect-call table as well. By default a DLL's
# table sits right after its image, which overlaps the next module's fixed load
# address (Alucard 0x83000000, Soma 0x83090000, ...). The tables are packed one
# after another from 0x8D000000, each (code size + thunk reserve) * 2 bytes,
# the size the runtime allocates. Everything must stay below 0x90000000, the
# end of the guest heap the tables live in. With all DLC (about 110 modules)
# this needs roughly 10 MB.
# Needs SDK change 5 from scripts/build.py (ReXModule_TableBase).
set(_harmony_dll_targets_file "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/dll_targets.cmake")
math(EXPR _harmony_table_next "0x8D000000")  # decimal, for if() below
if(EXISTS "${_harmony_dll_targets_file}")
    file(STRINGS "${_harmony_dll_targets_file}" _harmony_dll_lines
         REGEX "^    add_library\\(|^    include\\(")
    set(_harmony_dll_dir "")
    foreach(_harmony_line ${_harmony_dll_lines})
        if(_harmony_line MATCHES "^    include\\(([^)]+)/sources\\.cmake\\)")
            set(_harmony_dll_dir "${CMAKE_MATCH_1}")
        elseif(_harmony_line MATCHES "^    add_library\\(([^ ]+) SHARED")
            set(_harmony_lib "${CMAKE_MATCH_1}")
            if(TARGET ${_harmony_lib} AND _harmony_dll_dir)
                file(GLOB _harmony_init_h "${CMAKE_CURRENT_SOURCE_DIR}/${_harmony_dll_dir}/*_init.h")
                set(_harmony_code_size "")
                if(_harmony_init_h)
                    list(GET _harmony_init_h 0 _harmony_init_h)
                    file(STRINGS "${_harmony_init_h}" _harmony_size_line
                         REGEX "#define REX_CODE_SIZE ")
                    if(_harmony_size_line MATCHES "REX_CODE_SIZE (0x[0-9A-Fa-f]+|[0-9]+)")
                        set(_harmony_code_size "${CMAKE_MATCH_1}")
                    endif()
                endif()
                if(NOT _harmony_code_size)
                    message(FATAL_ERROR "No REX_CODE_SIZE in ${_harmony_dll_dir}; run codegen again")
                endif()
                # Table size rounded up to the 64 KB allocation granularity.
                math(EXPR _harmony_table_size
                     "((${_harmony_code_size} + 0x10000) * 2 + 0xFFFF) / 0x10000 * 0x10000")
                math(EXPR _harmony_table "${_harmony_table_next}" OUTPUT_FORMAT HEXADECIMAL)
                target_compile_definitions(${_harmony_lib} PRIVATE
                    REX_FUNC_TABLE_BASE=${_harmony_table}ull)
                math(EXPR _harmony_table_next "${_harmony_table_next} + ${_harmony_table_size}")
            endif()
            set(_harmony_dll_dir "")
        endif()
    endforeach()
endif()
math(EXPR _harmony_table_limit "0x90000000")
if(_harmony_table_next GREATER _harmony_table_limit)
    math(EXPR _harmony_table_end "${_harmony_table_next}" OUTPUT_FORMAT HEXADECIMAL)
    message(FATAL_ERROR "DLL function tables end at ${_harmony_table_end}, past 0x90000000")
endif()
