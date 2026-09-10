find_package(Python3 COMPONENTS Interpreter REQUIRED)

execute_process(
    COMMAND ${Python3_EXECUTABLE} -m pytest --version
    OUTPUT_QUIET
    ERROR_QUIET
    RESULT_VARIABLE PYTEST_CHECK_RESULT
)

if(NOT PYTEST_CHECK_RESULT EQUAL 0)
    message(WARNING "pytest is not installed. Python tests will be skipped.")
    message(WARNING "Please install it using: pip install pytest")
    return()
endif()

add_test(NAME test_httpp_python
    COMMAND ${Python3_EXECUTABLE} -m pytest ${HTTPP_TEST_SOURCE_DIR}
    WORKING_DIRECTORY ${PYTHON_INSTALL_DIR}
)

add_custom_target(httpp_test_python
    COMMAND ${CMAKE_COMMAND} -E echo "-- Running python tests..."
    COMMAND ${Python3_EXECUTABLE} -m pytest ${HTTPP_TEST_SOURCE_DIR}
    WORKING_DIRECTORY ${PYTHON_INSTALL_DIR}
    USES_TERMINAL
)
add_dependencies(httpp_test_python build_python)
add_dependencies(httpp_test httpp_test_python)
