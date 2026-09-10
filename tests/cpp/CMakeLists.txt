file(GLOB HTTPP_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp
)

add_executable(test_httpp_cpp
    ${HTTPP_TEST_SOURCES}
    ${HTTPP_TEST_SOURCE_DIR}/utest/utest_main.cxx
)

target_include_directories(test_httpp_cpp PRIVATE
    ${HTTPP_TEST_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/third_party/cpp-httplib
)

# httplib.h here is only used to spin up a throwaway local server inside
# test_client.cpp — it never leaks into httpp's public API (see
# include/httpp/client.hpp, which is what's actually under test).
target_link_libraries(test_httpp_cpp PRIVATE httpp::core)

add_test(NAME test_httpp_cpp COMMAND $<TARGET_FILE:test_httpp_cpp>)

add_custom_target(httpp_test_cpp
    COMMAND ${CMAKE_COMMAND} -E echo "-- Running c++ tests..."
    COMMAND $<TARGET_FILE:test_httpp_cpp>
    WORKING_DIRECTORY ${HTTPP_TEST_BINARY_DIR}
)
add_dependencies(httpp_test_cpp test_httpp_cpp)
add_dependencies(httpp_test httpp_test_cpp)
