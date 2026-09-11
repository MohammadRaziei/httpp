cmake_minimum_required(VERSION 3.15)
project(httpp_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Ask the *installed* httpp Python package where its CMake config lives
# (the same pattern nanobind/pybind11 use: httpp.get_cmake_dir()) and hand
# that to find_package — no manual -I/-L flags, no hardcoded paths.
execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import httpp; print(httpp.get_cmake_dir())"
    OUTPUT_VARIABLE httpp_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)

list(APPEND CMAKE_PREFIX_PATH "${httpp_DIR}")
find_package(httpp CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE httpp::httpp_core)
