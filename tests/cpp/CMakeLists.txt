file(GLOB HTTPP_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp
)

add_executable(test_httpp_cpp
    ${HTTPP_TEST_SOURCES}
    ${HTTPP_TEST_SOURCE_DIR}/utest/utest_main.cxx
)

target_include_directories(test_httpp_cpp PRIVATE
    ${HTTPP_TEST_SOURCE_DIR}
)

# The tests only ever use httpp::client/httpp::server (the public API,
# already compiled into httpp_core) — httplib.h is never included here,
# so no httplib include path or windows.h/winsock macro juggling is needed.
target_link_libraries(test_httpp_cpp PRIVATE httpp::core)

add_test(NAME test_httpp_cpp COMMAND $<TARGET_FILE:test_httpp_cpp>)

add_custom_target(httpp_test_cpp
    COMMAND ${CMAKE_COMMAND} -E echo "-- Running c++ tests..."
    COMMAND $<TARGET_FILE:test_httpp_cpp>
    WORKING_DIRECTORY ${HTTPP_TEST_BINARY_DIR}
)
add_dependencies(httpp_test_cpp test_httpp_cpp)
add_dependencies(httpp_test httpp_test_cpp)
