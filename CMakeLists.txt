cmake_minimum_required(VERSION 3.21)
if(NOT DEFINED SKBUILD_PROJECT_NAME)
    # Standalone `cmake -S . -B build` (not via `pip install`/scikit-build-core)
    set(SKBUILD_PROJECT_NAME httpp)
endif()
project(${SKBUILD_PROJECT_NAME} LANGUAGES CXX)
# Set Python module name (should match the .pyx filename)
string(REPLACE "-" "_" MODULE_NAME ${SKBUILD_PROJECT_NAME})

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set build type to Release for better performance
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Optimize for performance in Release mode
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DNDEBUG")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# --- httpp C++ core (used by the Cython module AND covered by C++ tests) ---
# httplib.h is vendored at src/third_party/cpp-httplib (a real file, no
# system package). It is only ever included from src/core/*.cpp, never from
# a public include/httpp/*.hpp header, and stays hidden inside the compiled
# core (see include/httpp/export.hpp / CXX_VISIBILITY_PRESET below).
add_library(httpp_core STATIC
    src/core/client.cpp
)
add_library(httpp::core ALIAS httpp_core)

set_target_properties(httpp_core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(httpp_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/third_party/cpp-httplib
)

find_package(Threads REQUIRED)
target_link_libraries(httpp_core PUBLIC Threads::Threads)

# Only build/run the C++ test suite for a standalone `cmake -S . -B build`
# checkout. `pip install .` (scikit-build-core) defines SKBUILD and should
# not need Threads-linked test binaries or ctest.
if(DEFINED SKBUILD)
    option(HTTPP_BUILD_TESTS "Build httpp C++ tests" OFF)
else()
    option(HTTPP_BUILD_TESTS "Build httpp C++ tests" ON)
endif()
if(HTTPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Find Python and Cython
find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(Cython MODULE REQUIRED VERSION 3.0)
include(UseCython)

# Configure Cython module
cython_transpile(src/${MODULE_NAME}/${MODULE_NAME}.pyx LANGUAGE CXX OUTPUT_VARIABLE source_file)

# Create Python module
python_add_library(${MODULE_NAME} MODULE "${source_file}" WITH_SOABI)

# Set properties for the module
set_target_properties(${MODULE_NAME} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(${MODULE_NAME} PRIVATE httpp::core)

# Install the Python module
install(TARGETS ${MODULE_NAME}
    DESTINATION ${SKBUILD_PROJECT_NAME}
)
