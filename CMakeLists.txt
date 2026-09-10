cmake_minimum_required(VERSION 3.21)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(DynamicVersion)

extract_version_string(
    HEADER_FILE ${CMAKE_CURRENT_LIST_DIR}/include/httpp/version.hpp
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

add_library(httpp_core ${HTTPP_CORE_SOURCES})
add_library(httpp::core ALIAS httpp_core)

set_target_properties(httpp_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(httpp_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/cpp-httplib
)

find_package(Threads REQUIRED)
target_link_libraries(httpp_core PUBLIC Threads::Threads)

target_compile_options(httpp_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /EHsc>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

apply_optimization_flags(httpp_core)

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
)
install(DIRECTORY include DESTINATION .)

# Tests
if(HTTPP_BUILD_TESTS)
    enable_testing()
    set(CMAKE_CTEST_ARGUMENTS "--output-on-failure" "-V")
    add_subdirectory(${PROJECT_SOURCE_DIR}/tests)
endif()
