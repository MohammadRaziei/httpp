# Tests of httpp_utils on its own (ADR-0086): a program that links only httpp::utils builds and runs, the library needs nothing from the
# HTTP stack, and one process has one logger registry. None of these targets links httpp::core.

# 1. A consumer of httpp::utils only.
add_executable(utils_consumer utils_consumer.cpp)
target_link_libraries(utils_consumer PRIVATE httpp::utils)
add_test(NAME utils_consumer COMMAND utils_consumer)

# 2. NEEDED entries (readelf -d on Linux, otool -L on macOS): libhttpp_utils must not depend on core, urlparser or mbedtls, and the
#    consumer must not pull libhttpp in. Windows has no such tool in the CI image; its import tables are checked there by the wheel job.
if(NOT WIN32)
    add_test(NAME utils_needed
        COMMAND ${CMAKE_COMMAND}
            -DUTILS_LIB=$<TARGET_FILE:httpp_utils>
            -DCONSUMER=$<TARGET_FILE:utils_consumer>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_needed.cmake)
endif()

# 3. One registry: a shared library that links only httpp::utils logs through get_logger("x.y"); the executable (which also links
#    httpp::utils) installs a handler on "x" and must receive the record. Two copies of the registry would lose it.
add_library(utils_registry_probe SHARED registry_probe.cpp)
target_link_libraries(utils_registry_probe PRIVATE httpp::utils)
target_compile_definitions(utils_registry_probe PRIVATE UTILS_PROBE_BUILDING)
set_target_properties(utils_registry_probe PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

add_executable(utils_one_registry one_registry.cpp)
target_link_libraries(utils_one_registry PRIVATE httpp::utils utils_registry_probe)
add_test(NAME utils_one_registry COMMAND utils_one_registry)


# 4. The inline fast path and the lazy macros (ADR-0090): HTTPP_LOG, HTTPP_LOGF, logger::enabled().
add_executable(utils_fast_path fast_path.cpp)
target_link_libraries(utils_fast_path PRIVATE httpp::utils)
add_test(NAME utils_fast_path COMMAND utils_fast_path)
