cmake_minimum_required(VERSION 3.21)
project(${SKBUILD_PROJECT_NAME})
# Set Python module name (should match the .pyx filename)
string(REPLACE "-" "_" MODULE_NAME ${SKBUILD_PROJECT_NAME})

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
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
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
)

# Install the Python module
install(TARGETS ${MODULE_NAME}
    DESTINATION ${SKBUILD_PROJECT_NAME}
)