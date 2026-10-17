# EmbedFile.cmake
#
# Compiles data files into the binary: at CMake CONFIGURE time (so the header exists right after
# `cmake -B build`) every file is read and written into one generated C++ header, as
#
#     inline constexpr unsigned char <name>[] = "\x2f\x2a..." "\x21\x20...";   // the file's bytes
#     inline constexpr std::size_t   <name>_size = sizeof(<name>) - 1;         // its length
#
# so there is nothing to ship or to find on disk at run time, and `<name>` lives in .rodata (no copy at start-up).
#
# Why it looks like this (the idea comes from liburlparser's EmbedFile.cmake, which embeds the Public Suffix List):
#   * one string literal per chunk, glued by the compiler's adjacent-literal concatenation: a single huge literal
#     chokes MSVC. liburlparser cuts per *source line*; swagger-ui-bundle.js is one minified line of 1.5 MB, so
#     here a chunk is a fixed number of bytes (32), whatever the file looks like;
#   * every byte is written as a \xNN escape: binary-safe (a PNG has NUL bytes, `file(READ)` as text would stop at
#     them), no source-charset or trigraph surprises, no \x-escape swallowing the next hex digit;
#   * byte arrays are the default everywhere (-DHTTPP_EMBED_AS_ARRAY=OFF gives string literals): MSVC refuses long
#     literals and the others warn (-Woverlength-strings) for anything over 64 KB. Same names, same sizes; the cost is
#     compile time: the Swagger UI bundle takes ~4.6 s instead of ~0.9 s to compile with GCC, once per build.
#     ponytail: arrays are tested with GCC and MSVC (CI); upgrade path for much bigger files: .incbin / .o via the
#     linker.
#
# Usage:
#   include(EmbedFile)
#   embed_files(
#       OUTPUT    ${CMAKE_CURRENT_BINARY_DIR}/generated/data.hpp
#       NAMESPACE httpp::embedded
#       FILES     some_name /path/to/file   other_name /path/to/other)

set(_EMBED_FILE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Byte arrays everywhere: one code path on every compiler, and no -Woverlength-strings (the Swagger UI bundle is
# ~1.5 MB; string literals are only guaranteed up to 64 KB, and MSVC refuses long ones).
option(HTTPP_EMBED_AS_ARRAY "Embed data files as byte arrays instead of string literals" ON)

# one file -> the C++ declarations in <out_var>
function(_embed_one NAME PATH OUT_VAR)
    if(NOT EXISTS "${PATH}")
        message(FATAL_ERROR "embed_files: ${PATH} does not exist")
    endif()
    file(SIZE "${PATH}" _size)
    file(READ "${PATH}" _hex HEX)

    if(HTTPP_EMBED_AS_ARRAY)
        set(_chunk 24)
        set(_one "([0-9a-f][0-9a-f])")
    else()
        set(_chunk 32)
    endif()
    # the file is cut into whole chunks plus a shorter tail, each handled on its own (the tail can't be matched
    # by a regex without a chunk-sized repeat, and CMake's regex has no {n})
    math(EXPR _rem "${_size} % ${_chunk}")
    math(EXPR _head_chars "(${_size} - ${_rem}) * 2")
    string(SUBSTRING "${_hex}" 0 ${_head_chars} _head)
    string(SUBSTRING "${_hex}" ${_head_chars} -1 _tail)

    if(HTTPP_EMBED_AS_ARRAY)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _head "${_head}")
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _tail "${_tail}")
        string(REPEAT "0x[0-9a-f][0-9a-f]," ${_chunk} _unit)
        string(REGEX REPLACE "(${_unit})" "    \\1\n" _head "${_head}")
        if(_rem GREATER 0)
            set(_tail "    ${_tail}\n")
        endif()
        set(_decl "inline constexpr unsigned char ${NAME}[] = {\n${_head}${_tail}};\ninline constexpr std::size_t ${NAME}_size = sizeof(${NAME});")
        if(_size EQUAL 0) # a zero-size array is ill-formed (MSVC C2466): one dummy byte, the size stays 0
            set(_decl "inline constexpr unsigned char ${NAME}[1] = {0};\ninline constexpr std::size_t ${NAME}_size = 0;")
        endif()
    else()
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" _head "${_head}")
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" _tail "${_tail}")
        string(REPEAT "\\\\x[0-9a-f][0-9a-f]" ${_chunk} _unit)
        string(REGEX REPLACE "(${_unit})" "    \"\\1\"\n" _head "${_head}")
        if(_rem GREATER 0)
            set(_tail "    \"${_tail}\"\n")
        endif()
        if(_size EQUAL 0)
            set(_head "    \"\"\n")
        endif()
        set(_decl "inline constexpr unsigned char ${NAME}[] =\n${_head}${_tail};\ninline constexpr std::size_t ${NAME}_size = sizeof(${NAME}) - 1;")
    endif()
    set(${OUT_VAR} "${_decl}" PARENT_SCOPE)
endfunction()

function(embed_files)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "OUTPUT;NAMESPACE" "FILES")
    if(NOT ARG_OUTPUT OR NOT ARG_NAMESPACE OR NOT ARG_FILES)
        message(FATAL_ERROR "embed_files: OUTPUT, NAMESPACE and FILES <name> <path> ... are required")
    endif()
    list(LENGTH ARG_FILES _n)
    math(EXPR _odd "${_n} % 2")
    if(_odd)
        message(FATAL_ERROR "embed_files: FILES takes pairs of <name> <path>")
    endif()

    set(CONTENT "")
    set(_i 0)
    while(_i LESS _n)
        math(EXPR _j "${_i} + 1")
        list(GET ARG_FILES ${_i} _name)
        list(GET ARG_FILES ${_j} _path)
        _embed_one("${_name}" "${_path}" _decl)
        string(APPEND CONTENT "${_decl}\n\n")
        # a changed data file re-runs CMake, which regenerates the header
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
        math(EXPR _i "${_i} + 2")
    endwhile()

    set(NAMESPACE "${ARG_NAMESPACE}")
    get_filename_component(_outdir "${ARG_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_outdir}")
    # configure_file only rewrites the header when its content changed, so an unchanged file costs no rebuild
    configure_file("${_EMBED_FILE_MODULE_DIR}/utils/embedded_files.in.hpp" "${ARG_OUTPUT}" @ONLY)
endfunction()
