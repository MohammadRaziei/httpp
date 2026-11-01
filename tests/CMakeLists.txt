set(HTTPP_TEST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(HTTPP_TEST_BINARY_DIR ${CMAKE_BINARY_DIR}/tests)

add_custom_target(httpp_test
    COMMAND ${CMAKE_COMMAND} -E echo "-- Running All tests..."
)

add_subdirectory(${HTTPP_TEST_SOURCE_DIR}/cpp)

# embed_files(... COMPRESS), compressed and not (see cmake/EmbedFile.cmake). Needs no Python.
foreach(mode ON OFF)
    add_test(NAME embed_compress_${mode}
        COMMAND ${CMAKE_CTEST_COMMAND} --build-and-test
            ${HTTPP_TEST_SOURCE_DIR}/cmake/embed_compress ${CMAKE_BINARY_DIR}/embed_compress_${mode}
            --build-generator ${CMAKE_GENERATOR}
            --build-options -DHTTPP_EMBED_COMPRESS=${mode} -DHTTPP_CMAKE_DIR=${PROJECT_SOURCE_DIR}/cmake)
endforeach()

if(TARGET build_python)
    add_subdirectory(${HTTPP_TEST_SOURCE_DIR}/python)
    add_subdirectory(${HTTPP_TEST_SOURCE_DIR}/cmake)
endif()
