cmake_minimum_required(VERSION 3.21)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(DynamicVersion)

extract_version_string(
    HEADER_FILE ${CMAKE_CURRENT_LIST_DIR}/include/httpp.h
    VERSION_PREFIX HTTPP_
    OUTPUT_VAR PROJECT_VERSION
)

project(httpp LANGUAGES CXX VERSION ${PROJECT_VERSION})
message(STATUS "Project: ${PROJECT_NAME}@v${PROJECT_VERSION}")
include(Startup)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(Optimize)

# Project options
option(HTTPP_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(HTTPP_BUILD_PYTHON "Build Python bindings" ${PROJECT_IS_TOP_LEVEL})

set(HTTPP_SOURCE_DIR ${PROJECT_SOURCE_DIR})

# --- httpp C++ core ---
# httplib.h is vendored at src/third_party/cpp-httplib (a real committed
# file, no system package). mbedtls is a git submodule at
# src/third_party/mbedtls (also built from source, no system package).
# Both are included only from src/core/*.cpp, never from a public
# include/httpp/*.hpp header, and are compiled with hidden visibility so
# they never leak into the httpp .so/.a's public symbols.
file(GLOB HTTPP_CORE_SOURCES ${PROJECT_SOURCE_DIR}/src/core/*.cpp)

# Swagger UI (vendored in src/third_party/swagger-ui) is compiled into the library: at CONFIGURE time its three
# files become a generated header (cmake/EmbedFile.cmake), so /docs needs no data files at run time. They are stored gzip-compressed
# (COMPRESS, ADR-0031; -DHTTPP_EMBED_COMPRESS=OFF stores them as they are).
include(EmbedFile)
set(HTTPP_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(HTTPP_SWAGGER_UI_DIR ${PROJECT_SOURCE_DIR}/src/third_party/swagger-ui)
embed_files(
    OUTPUT    ${HTTPP_GENERATED_DIR}/httpp_swagger_ui_data.hpp
    NAMESPACE httpp::embedded
    COMPRESS
    FILES
        swagger_ui_bundle_js    ${HTTPP_SWAGGER_UI_DIR}/swagger-ui-bundle.js
        swagger_ui_css          ${HTTPP_SWAGGER_UI_DIR}/swagger-ui.css
        swagger_ui_icon_svg     ${HTTPP_SWAGGER_UI_DIR}/swagger-api-icon.svg
)

# The file browser page (src/assets/file_browser.html) is embedded the same way (ADR-0032).
embed_files(
    OUTPUT    ${HTTPP_GENERATED_DIR}/httpp_file_browser_data.hpp
    NAMESPACE httpp::embedded
    COMPRESS
    FILES
        file_browser_html       ${PROJECT_SOURCE_DIR}/src/assets/file_browser.html
)

# liburlparser (git submodule, built from source - no system package) does
# the real URL parsing (scheme/host/port/path, IPv4/IPv6, PSL-aware). It is
# linked PRIVATE into httpp_core, same as cpp-httplib: consumers of
# <httpp/url.hpp> see only httpp::url, never urlparser::url directly.
set(BUILD_PYTHON OFF CACHE BOOL "" FORCE)
# Needed so liburlparser's static lib can be linked into httpp's shared
# lib / the Cython extension module (which are themselves PIC).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(${PROJECT_SOURCE_DIR}/src/third_party/liburlparser)

# mbedtls (TLS support for HTTPS, via cpp-httplib's CPPHTTPLIB_MBEDTLS_SUPPORT).
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
# mbedtls's codegen'd sources (library/error.c,
# tf-psa-crypto/core/psa_crypto_driver_wrappers_no_static.c, etc.) must
# exist on disk before add_subdirectory() below runs. mbedtls generates
# them itself via its own GEN_FILES option on Linux/macOS, but defaults
# GEN_FILES OFF on any Windows host (assuming a release tarball with these
# files already bundled). We build mbedtls from a git submodule, which
# never has them on any platform, so we generate them ourselves here,
# unconditionally, using mbedtls's own official scripts - the approach
# mbedtls's maintainers recommend: https://github.com/Mbed-TLS/mbedtls/issues/10678.
# Requires Python 3 and Perl on PATH (both are already required to build
# mbedtls from a git checkout, per its own documentation).
set(_httpp_mbedtls_dir ${PROJECT_SOURCE_DIR}/src/third_party/mbedtls)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND ${Python3_EXECUTABLE} framework/scripts/make_generated_files.py
    WORKING_DIRECTORY ${_httpp_mbedtls_dir}/tf-psa-crypto
    RESULT_VARIABLE _httpp_mbedtls_gen_result
)
if(NOT _httpp_mbedtls_gen_result EQUAL 0)
    message(FATAL_ERROR "Failed to generate tf-psa-crypto sources (exit ${_httpp_mbedtls_gen_result})")
endif()
execute_process(
    COMMAND ${Python3_EXECUTABLE} scripts/make_generated_files.py
    WORKING_DIRECTORY ${_httpp_mbedtls_dir}
    RESULT_VARIABLE _httpp_mbedtls_gen_result
)
if(NOT _httpp_mbedtls_gen_result EQUAL 0)
    message(FATAL_ERROR "Failed to generate mbedtls sources (exit ${_httpp_mbedtls_gen_result})")
endif()
# mbedtls's own install() rules are unconditional unless this is forced ON
# (its "am I a subproject" auto-detection doesn't reliably suppress them);
# httpp links it in statically and never re-exports its headers/libs, so
# nothing from mbedtls itself belongs in the installed wheel.
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
# Thread safety of Mbed TLS: its process-wide state (PSA random generator, key slots) is only locked when the
# threading layer is enabled, and httpp does TLS on many threads. See cmake/httpp_mbedtls_config.h.
option(HTTPP_MBEDTLS_THREADING_ALT "Use the alt threading layer (always on for Windows) instead of pthread" OFF)
if(HTTPP_MBEDTLS_THREADING_ALT)
    add_compile_definitions(HTTPP_MBEDTLS_THREADING_ALT)
endif()
set(TF_PSA_CRYPTO_USER_CONFIG_FILE ${PROJECT_SOURCE_DIR}/cmake/httpp_mbedtls_config.h CACHE FILEPATH "" FORCE)
find_package(Threads REQUIRED)
set(LINK_WITH_PTHREAD ON CACHE BOOL "" FORCE)
# threading_alt.h (the alt layer's types) must be found by every Mbed TLS source (it builds several object libraries
# that do not see each other's include paths) and by everything that includes its headers: a directory-level include
# path, inherited by the subdirectories added below, reaches all of them.
if(WIN32 OR HTTPP_MBEDTLS_THREADING_ALT)
    include_directories(${PROJECT_SOURCE_DIR}/src/core/mbedtls_alt)
endif()
add_subdirectory(${PROJECT_SOURCE_DIR}/src/third_party/mbedtls)

# llhttp (vendored release C sources, unmodified, no Node needed): the HTTP/1.x parser of the async
# server and client. Own C static target so the generated code is built without
# httpp's -Wall -Wextra -Wpedantic; see src/third_party/llhttp/VENDORED.md.
enable_language(C)
add_library(httpp_llhttp STATIC
    ${PROJECT_SOURCE_DIR}/src/third_party/llhttp/src/llhttp.c
    ${PROJECT_SOURCE_DIR}/src/third_party/llhttp/src/api.c
    ${PROJECT_SOURCE_DIR}/src/third_party/llhttp/src/http.c)
target_include_directories(httpp_llhttp PUBLIC
    ${PROJECT_SOURCE_DIR}/src/third_party/llhttp/include)
set_target_properties(httpp_llhttp PROPERTIES
    LINKER_LANGUAGE C POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_compile_options(httpp_llhttp PRIVATE
    $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)

add_library(httpp_core SHARED ${HTTPP_CORE_SOURCES})
add_library(httpp::core ALIAS httpp_core)

# Windows/MSVC dllexport-vs-dllimport: HTTPP_API (include/httpp/export.hpp)
# resolves to __declspec(dllexport) only when HTTPP_BUILDING_DLL is defined.
# It must be defined while compiling httpp_core's OWN translation units
# (client.cpp/server.cpp/url.cpp) so the public classes are actually
# exported from httpp_core.dll - and must NOT be defined for anyone
# consuming the already-built DLL (they need dllimport instead), hence
# PRIVATE, not PUBLIC/INTERFACE.
target_compile_definitions(httpp_core PRIVATE HTTPP_BUILDING_DLL)

set_target_properties(httpp_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    MACOSX_RPATH ON
    INSTALL_NAME_DIR "@rpath"
)

target_include_directories(httpp_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:httpp/sdk/include>
)
# SYSTEM: these are vendored/third-party headers we don't control (cpp-httplib,
# indicators, etc.). Marking them SYSTEM tells the compiler their own internal
# warnings (e.g. indicators.hpp's narrowing-conversion C4244/C4267 on MSVC)
# aren't ours to fix and shouldn't surface under our own -Wall/-Wextra/-W4
# below - without patching vendored code to silence its own warnings.
target_include_directories(httpp_core PRIVATE ${HTTPP_GENERATED_DIR})
target_include_directories(httpp_core SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
)

find_package(Threads REQUIRED)
target_link_libraries(httpp_core PUBLIC Threads::Threads)
target_link_libraries(httpp_core PRIVATE url::base httpp_llhttp)

# HTTPS support: mbedtls is linked PRIVATE and CPPHTTPLIB_MBEDTLS_SUPPORT is
# only ever defined in src/core/internal/httplib_common.hpp (never in a
# public header), so consumers of httpp never need mbedtls on their own
# include/link path - see that file for the shared httplib.h include.
target_link_libraries(httpp_core PRIVATE mbedtls mbedx509)
target_compile_definitions(httpp_core PRIVATE CPPHTTPLIB_MBEDTLS_SUPPORT)
# Asio (standalone, header-only, vendored) runs the async engines: the server's
# connection loop and the non-blocking client (epoll on Linux, kqueue on macOS,
# IOCP on Windows). It is only ever included from src/core/async_server.cpp and
# src/core/async_http.cpp, never together with httplib.h.
# Where Asio comes from, in order:
#   1. -DHTTPP_ASIO_INCLUDE_DIR=<directory containing asio.hpp>  (offline builds, packagers)
#   2. src/third_party/asio, if it holds a copy: a git submodule (<dir>/asio/include)
#      or a plain copy (<dir>/include)
#   3. downloaded at configure time, pinned by version and SHA-256 (about 3 MB)
set(HTTPP_ASIO_VERSION 1.34.2)
set(HTTPP_ASIO_SHA256 f3bac015305fbb700545bd2959fbc52d75a1ec2e05f9c7f695801273ceb78cf5)
set(HTTPP_ASIO_INCLUDE_DIR "" CACHE PATH "Directory containing asio.hpp (empty: auto-detect, else download)")
if(NOT HTTPP_ASIO_INCLUDE_DIR)
    foreach(_candidate
            ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/asio/asio/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/asio/include)
        if(EXISTS "${_candidate}/asio.hpp")
            set(HTTPP_ASIO_INCLUDE_DIR "${_candidate}" CACHE PATH "" FORCE)
            break()
        endif()
    endforeach()
endif()
if(NOT HTTPP_ASIO_INCLUDE_DIR)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW) # extracted files get the time of extraction
    endif()
    string(REPLACE "." "-" _asio_tag "asio-${HTTPP_ASIO_VERSION}")
    FetchContent_Declare(httpp_asio
        URL https://codeload.github.com/chriskohlhoff/asio/tar.gz/refs/tags/${_asio_tag}
        URL_HASH SHA256=${HTTPP_ASIO_SHA256})
    FetchContent_MakeAvailable(httpp_asio) # Asio has no CMakeLists.txt: this only downloads and extracts
    set(HTTPP_ASIO_INCLUDE_DIR "${httpp_asio_SOURCE_DIR}/asio/include" CACHE PATH "" FORCE)
endif()
message(STATUS "Asio ${HTTPP_ASIO_VERSION}: ${HTTPP_ASIO_INCLUDE_DIR}")

target_include_directories(httpp_core SYSTEM PRIVATE ${HTTPP_ASIO_INCLUDE_DIR})
target_compile_definitions(httpp_core PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
if(WIN32)
    # Asio wants the target Windows version spelled out (Windows 10 and later).
    target_compile_definitions(httpp_core PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(httpp_core PRIVATE ws2_32 mswsock crypt32)
endif()
if(MSVC)
    # Asio's templates produce very large object files.
    set_source_files_properties(${PROJECT_SOURCE_DIR}/src/core/async_http.cpp ${PROJECT_SOURCE_DIR}/src/core/async_server.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()

# macOS: cpp-httplib (src/third_party/cpp-httplib/httplib.h) reads the
# system trust store via the Keychain APIs (CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN),
# which pulls in CoreFoundation/Security symbols (e.g. SecTrustSettingsCopyCertificates).
# These aren't linked by default and must be added explicitly, or the final
# link of libhttpp_core.dylib fails with "symbol(s) not found for architecture".
if(APPLE)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
    find_library(SECURITY_LIBRARY Security REQUIRED)
    target_link_libraries(httpp_core PRIVATE ${COREFOUNDATION_LIBRARY} ${SECURITY_LIBRARY})
endif()

target_compile_options(httpp_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /EHsc>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

apply_optimization_flags(httpp_core)

# --- the command line: `httpp` (ADR-0041) ---
# Native commands (server, browse, download, curl) are this executable, started by httpp_cli.py (next to the package) in the wheel; run on its
# own it hands `httpp py ...` to `python -m httpp`. CLI11 is one vendored header (src/third_party/CLI11, unmodified). std::filesystem is switched off in it
# because the macOS 10.13 target does not have it.
option(HTTPP_BUILD_CLI "Build the httpp command line" ON)
if(HTTPP_BUILD_CLI)
    add_executable(httpp_cli ${PROJECT_SOURCE_DIR}/src/cli/httpp_cli.cpp)
    set_target_properties(httpp_cli PROPERTIES
        OUTPUT_NAME httpp
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    target_link_libraries(httpp_cli PRIVATE httpp_core)
    target_include_directories(httpp_cli SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/src/third_party/CLI11)
    target_compile_definitions(httpp_cli PRIVATE CLI11_HAS_FILESYSTEM=0)
    if(WIN32)
        target_compile_definitions(httpp_cli PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    target_compile_options(httpp_cli PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /EHsc>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
    )
    apply_optimization_flags(httpp_cli)

    # The executable sits in a bin/ folder next to lib/ (<prefix>/bin and <prefix>/lib for an SDK install, httpp/sdk/bin and
    # httpp/sdk/lib inside the wheel), so one relative RPATH finds libhttpp_core in both.
    if(APPLE)
        # @loader_path, not @executable_path: for the executable itself they are the same folder at run time, but delocate
        # (cibuildwheel's repair step) resolves @loader_path and fails with "@rpath/libhttpp_core.dylib not found" on @executable_path.
        set(_httpp_cli_origin "@loader_path")
    else()
        set(_httpp_cli_origin "$ORIGIN")
    endif()
    set_target_properties(httpp_cli PROPERTIES INSTALL_RPATH "${_httpp_cli_origin}/../lib")
    if(DEFINED SKBUILD)
        # Inside the package, not in the scripts directory: httpp_cli.py (the `httpp` command, next to the package) finds it and runs it.
        install(TARGETS httpp_cli RUNTIME DESTINATION httpp/sdk/bin COMPONENT httpp_wheel)
    endif()
endif()

if(HTTPP_BUILD_PYTHON OR DEFINED SKBUILD)
    set(PYTHON_INSTALL_DIR ${CMAKE_BINARY_DIR}/python/install)
    add_subdirectory(${PROJECT_SOURCE_DIR}/src/bindings/python)
    if(DEFINED SKBUILD)
        return()
    endif()
endif()

# Installation
install(TARGETS httpp_core
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin  # the DLL on Windows, next to bin/httpp.exe
)
install(DIRECTORY include DESTINATION .)
if(HTTPP_BUILD_CLI)
    install(TARGETS httpp_cli RUNTIME DESTINATION bin)
endif()

# Tests
if(HTTPP_BUILD_TESTS)
    enable_testing()
    set(CMAKE_CTEST_ARGUMENTS "--output-on-failure" "-V")
    add_subdirectory(${PROJECT_SOURCE_DIR}/tests)
endif()
