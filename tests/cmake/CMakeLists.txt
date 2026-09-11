set(HTTPP_CMAKE_TEST_EXAMPLE_DIR ${CMAKE_SOURCE_DIR}/examples/find_httpp_via_python)
set(HTTPP_CMAKE_TEST_BUILD_DIR ${CMAKE_BINARY_DIR}/cmake_consumer_test)

add_test(NAME test_httpp_cmake
    COMMAND ${CMAKE_COMMAND}
        -DPYTHON_INSTALL_DIR=${PYTHON_INSTALL_DIR}
        -DEXAMPLE_SOURCE_DIR=${HTTPP_CMAKE_TEST_EXAMPLE_DIR}
        -DEXAMPLE_BINARY_DIR=${HTTPP_CMAKE_TEST_BUILD_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_consumer_test.cmake
)

add_custom_target(httpp_test_cmake
    COMMAND ${CMAKE_COMMAND} -E echo "-- Running cmake consumer test..."
    COMMAND ${CMAKE_COMMAND}
        -DPYTHON_INSTALL_DIR=${PYTHON_INSTALL_DIR}
        -DEXAMPLE_SOURCE_DIR=${HTTPP_CMAKE_TEST_EXAMPLE_DIR}
        -DEXAMPLE_BINARY_DIR=${HTTPP_CMAKE_TEST_BUILD_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_consumer_test.cmake
    USES_TERMINAL
)
add_dependencies(httpp_test_cmake build_python)
add_dependencies(httpp_test httpp_test_cmake)
