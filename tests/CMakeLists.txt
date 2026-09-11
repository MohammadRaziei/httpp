set(HTTPP_TEST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(HTTPP_TEST_BINARY_DIR ${CMAKE_BINARY_DIR}/tests)

add_custom_target(httpp_test
    COMMAND ${CMAKE_COMMAND} -E echo "-- Running All tests..."
)

add_subdirectory(${HTTPP_TEST_SOURCE_DIR}/cpp)

if(TARGET build_python)
    add_subdirectory(${HTTPP_TEST_SOURCE_DIR}/python)
    add_subdirectory(${HTTPP_TEST_SOURCE_DIR}/cmake)
endif()
