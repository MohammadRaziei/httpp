# The benchmark suite as a CMake project. One command for the whole comparison:
#
#   cmake -S benchmarks -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-bench --target bench          # about 25 minutes; `--target bench_quick` is about 5
#
# Then send back the file it prints at the end (build-bench/results/*.txt). Targets:
#   bench_env    Python 3.14 (GIL) and 3.14t (free-threaded) virtualenvs with this httpp, uvicorn, websockets..., and the load generator `oha` (needs network)
#   cpp_bench    the httpp C++ server of the suite (HTTP routes and a WebSocket echo)
#   ws_load      the WebSocket load generator (oha does not speak WebSocket)
#   drogon_bench Drogon, fetched and built from source by this file (bench only; see HTTPP_BENCH_DROGON)
#   bench_check  prints what could disturb the numbers (governor, battery, turbo)
#   bench        everything above, then the measurement
#   bench_quick  the same with 3 s, one round, 1 and 4 I/O threads
# Settings are cache variables, e.g.  cmake build-bench -DBENCH_SECONDS=10 -DBENCH_ROUNDS=5  (listed below).
# See docs/adr/0030-benchmarks-built-by-cmake.md.
cmake_minimum_required(VERSION 3.28)
project(httpp_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)  # so that the option() calls of the fetched projects do not clear the settings below
set(BUILD_SHARED_LIBS OFF)  # for the fetched libraries; httpp_core says SHARED itself

# ---- settings ----
set(BENCH_PYS "3.14 3.14t" CACHE STRING "Pythons to compare (space separated; a trailing t is the free-threaded build)")
set(BENCH_SECONDS 5 CACHE STRING "seconds per measurement")
set(BENCH_ROUNDS 3 CACHE STRING "interleaved rounds")
set(BENCH_CONC 64 CACHE STRING "connections of the load generators")
set(BENCH_IO_THREADS "1,2,4,8" CACHE STRING "I/O thread counts")
set(BENCH_ONLY "hello,json,cpu,latency,ws,idle" CACHE STRING "which parts: hello json cpu latency ws (WebSocket echo) idle (memory with many idle WebSockets)")
set(BENCH_WS_SIZE 64 CACHE STRING "bytes per WebSocket message")
set(BENCH_IDLE_CONNS 10000 CACHE STRING "idle WebSocket connections for the memory table (cut to what the open-file limit allows)")
set(BENCH_SERVER_CPUS "" CACHE STRING "e.g. 0-7 (empty: derived from the CPU topology)")
set(BENCH_LOAD_CPUS "" CACHE STRING "e.g. 8-15")
option(HTTPP_BENCH_DROGON "Fetch and build Drogon for the comparison (needs the libuuid headers, uuid-dev; skipped with a note without them)" ON)
set(HTTPP_BENCH_DROGON_TAG "v1.9.14" CACHE STRING "Drogon release to fetch")
set(HTTPP_BENCH_JSONCPP_TAG "1.9.6" CACHE STRING "jsoncpp release to fetch (a Drogon dependency)")

# ---- the library under test ----
set(HTTPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HTTPP_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. httpp EXCLUDE_FROM_ALL)
set(BENCH_HTTPP_INCLUDE ${CMAKE_CURRENT_LIST_DIR}/../include)
if(HTTPP_ASIO_INCLUDE_DIR AND EXISTS "${HTTPP_ASIO_INCLUDE_DIR}/asio.hpp")
    set(BENCH_ASIO_INCLUDE "${HTTPP_ASIO_INCLUDE_DIR}")
else()
    set(BENCH_ASIO_INCLUDE ${CMAKE_CURRENT_LIST_DIR}/../src/third_party/asio/asio/include)
endif()
find_package(Threads REQUIRED)

add_executable(cpp_bench EXCLUDE_FROM_ALL cpp/cpp_bench.cpp)
target_include_directories(cpp_bench PRIVATE ${BENCH_HTTPP_INCLUDE})
target_link_libraries(cpp_bench PRIVATE httpp_core Threads::Threads)

add_executable(ws_load EXCLUDE_FROM_ALL cpp/ws_load.cpp)
target_include_directories(ws_load SYSTEM PRIVATE ${BENCH_ASIO_INCLUDE})
target_compile_definitions(ws_load PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(ws_load PRIVATE Threads::Threads)

# ---- Drogon (built from source, nothing but the libuuid headers from the system) ----
# ponytail: libuuid is the one system package left (Drogon's own requirement). Upgrade path: a small uuid shim library, or Boost.UUID
# (DROGON_USE_BOOST_UUID) fetched the same way.
set(BENCH_HAVE_DROGON OFF)
if(HTTPP_BENCH_DROGON)
    find_path(BENCH_UUID_HEADER uuid/uuid.h)
    if(NOT BENCH_UUID_HEADER)
        message(STATUS "Drogon skipped: uuid/uuid.h not found (install uuid-dev, or pass -DHTTPP_BENCH_DROGON=OFF to silence this)")
    else()
        include(FetchContent)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # trantor and jsoncpp still declare old minimums
        set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
        set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
        set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "" FORCE)
        set(JSONCPP_WITH_CMAKE_PACKAGE ON CACHE BOOL "" FORCE)  # Drogon exports a target that needs jsoncpp_static in an export set
        set(JSONCPP_WITH_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
        set(BUILD_OBJECT_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(jsoncpp
            GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
            GIT_TAG ${HTTPP_BENCH_JSONCPP_TAG}
            GIT_SHALLOW ON
            EXCLUDE_FROM_ALL
            OVERRIDE_FIND_PACKAGE)  # Drogon's find_package(jsoncpp) is answered by this copy
        set(BUILD_CTL OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_ORM OFF CACHE BOOL "" FORCE)
        set(BUILD_BROTLI OFF CACHE BOOL "" FORCE)
        set(BUILD_YAML_CONFIG OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(USE_STATIC_LIBS_ONLY ON CACHE BOOL "" FORCE)  # links jsoncpp_static, and keeps the system OpenSSL/zlib out of it unless they ship .a files
        FetchContent_Declare(drogon
            GIT_REPOSITORY https://github.com/drogonframework/drogon.git
            GIT_TAG ${HTTPP_BENCH_DROGON_TAG}
            GIT_SHALLOW ON
            GIT_SUBMODULES trantor
            EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(jsoncpp drogon)
        add_executable(drogon_bench EXCLUDE_FROM_ALL cpp/drogon_bench.cpp)
        target_link_libraries(drogon_bench PRIVATE drogon)
        set(BENCH_HAVE_DROGON ON)
    endif()
endif()

# ---- the measurement ----
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(BENCH_DIR ${CMAKE_BINARY_DIR})
set(BENCH_OHA ${BENCH_DIR}/oha)
string(REPLACE " " ";" _bench_pys "${BENCH_PYS}")
set(_py_args "")
foreach(v IN LISTS _bench_pys)
    string(REPLACE "." "" _d "${v}")
    list(APPEND _py_args "${v}=${BENCH_DIR}/venvs/py${_d}/bin/python")
endforeach()
string(REPLACE ";" "," BENCH_PY_ARG "${_py_args}")

add_custom_target(bench_env
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/bench_setup.py env --root ${CMAKE_CURRENT_LIST_DIR}/.. --venvs ${BENCH_DIR}/venvs
            --pys "${BENCH_PYS}" --oha ${BENCH_OHA}
    COMMENT "Python virtualenvs and the load generator"
    USES_TERMINAL VERBATIM)

add_custom_target(bench_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/bench_setup.py check
    USES_TERMINAL VERBATIM)

set(_bench_binaries cpp_bench ws_load)
if(BENCH_HAVE_DROGON)
    list(APPEND _bench_binaries drogon_bench)
endif()

function(add_bench_target name seconds rounds io_threads)
    set(_cmd ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/suite.py
        --pys "${BENCH_PY_ARG}" --cpp $<TARGET_FILE:cpp_bench> --ws-load $<TARGET_FILE:ws_load> --oha ${BENCH_OHA}
        --seconds ${seconds} --rounds ${rounds} --conc ${BENCH_CONC} --io-threads ${io_threads} --rate --only ${BENCH_ONLY}
        --ws-size ${BENCH_WS_SIZE} --idle-conns ${BENCH_IDLE_CONNS} --out-dir ${BENCH_DIR}/results)
    if(BENCH_SERVER_CPUS)  # an empty value would be dropped from the command and eat the next option
        list(APPEND _cmd --server-cpus ${BENCH_SERVER_CPUS})
    endif()
    if(BENCH_LOAD_CPUS)
        list(APPEND _cmd --load-cpus ${BENCH_LOAD_CPUS})
    endif()
    if(BENCH_HAVE_DROGON)
        list(APPEND _cmd --drogon $<TARGET_FILE:drogon_bench> --note "drogon ${HTTPP_BENCH_DROGON_TAG}, jsoncpp ${HTTPP_BENCH_JSONCPP_TAG}, static, ${CMAKE_BUILD_TYPE}")
    endif()
    add_custom_target(${name}
        COMMAND ${_cmd}
        USES_TERMINAL VERBATIM)
    add_dependencies(${name} ${_bench_binaries} bench_env bench_check)
endfunction()

add_bench_target(bench ${BENCH_SECONDS} ${BENCH_ROUNDS} "${BENCH_IO_THREADS}")
add_bench_target(bench_quick 3 1 "1,4")
