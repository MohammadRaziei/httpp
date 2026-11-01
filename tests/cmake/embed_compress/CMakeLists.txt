# Test project for embed_files(... COMPRESS): configured, built and run by ctest (see tests/CMakeLists.txt) once with
# -DHTTPP_EMBED_COMPRESS=ON and once with OFF. The check itself runs as a post-build step, so a failing check fails the build.
cmake_minimum_required(VERSION 3.21)
project(httpp_embed_compress_test CXX)
set(CMAKE_CXX_STANDARD 17)

list(APPEND CMAKE_MODULE_PATH "${HTTPP_CMAKE_DIR}")
include(EmbedFile)

# A compressible sample, written at configure time so the test needs no data file.
set(SAMPLE "${CMAKE_CURRENT_BINARY_DIR}/sample.txt")
string(REPEAT "httpp embed compress test line, repeated so that gzip has something to do\n" 200 _text)
file(WRITE "${SAMPLE}" "${_text}")

embed_files(
    OUTPUT    ${CMAKE_CURRENT_BINARY_DIR}/generated/data.hpp
    NAMESPACE test_embedded
    COMPRESS
    FILES     sample "${SAMPLE}")

add_executable(check main.cpp)
target_include_directories(check PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(check PRIVATE
    SAMPLE_PATH="${SAMPLE}"
    EXPECT_GZIP=$<BOOL:${HTTPP_EMBED_COMPRESS}>)
add_custom_command(TARGET check POST_BUILD COMMAND check VERBATIM)
