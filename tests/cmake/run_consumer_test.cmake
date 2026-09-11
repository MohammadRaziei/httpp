# End-to-end check of the "get_cmake_dir() -> find_package(httpp)" story:
# 1) configure + build the example consumer project against the *installed*
#    httpp Python package (PYTHON_INSTALL_DIR),
# 2) run the resulting binary,
# 3) check it actually linked against httpp and produced the expected output.
#
# Invoked as: cmake
#   -DPYTHON_INSTALL_DIR=... -DEXAMPLE_SOURCE_DIR=... -DEXAMPLE_BINARY_DIR=...
#   -P run_consumer_test.cmake

foreach(var PYTHON_INSTALL_DIR EXAMPLE_SOURCE_DIR EXAMPLE_BINARY_DIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "run_consumer_test.cmake: ${var} not set")
    endif()
endforeach()

file(REMOVE_RECURSE "${EXAMPLE_BINARY_DIR}")

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${PYTHON_INSTALL_DIR}"
            ${CMAKE_COMMAND} -S "${EXAMPLE_SOURCE_DIR}" -B "${EXAMPLE_BINARY_DIR}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_output
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "consumer project failed to configure:\n${configure_output}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${EXAMPLE_BINARY_DIR}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_output
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "consumer project failed to build:\n${build_output}")
endif()

execute_process(
    COMMAND "${EXAMPLE_BINARY_DIR}/app"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_output
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "consumer app failed to run:\n${run_output}")
endif()

if(NOT run_output MATCHES "httpp found and linked OK")
    message(FATAL_ERROR "unexpected consumer app output:\n${run_output}")
endif()

message(STATUS "OK: ${run_output}")
