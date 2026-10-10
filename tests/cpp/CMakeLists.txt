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

# test_event_loop.cpp drives the internal event loop directly. That class is not
# exported from the shared library (hidden visibility), so compile it in.
target_sources(test_httpp_cpp PRIVATE ${PROJECT_SOURCE_DIR}/src/core/event_loop.cpp ${PROJECT_SOURCE_DIR}/src/core/async_http.cpp ${PROJECT_SOURCE_DIR}/src/core/tls_session.cpp ${PROJECT_SOURCE_DIR}/src/core/async_server.cpp)
target_link_libraries(test_httpp_cpp PRIVATE httpp_llhttp mbedtls mbedx509)
if(WIN32)
    target_link_libraries(test_httpp_cpp PRIVATE crypt32)
endif()
target_include_directories(test_httpp_cpp PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
target_include_directories(test_httpp_cpp SYSTEM PRIVATE ${HTTPP_ASIO_INCLUDE_DIR})
target_compile_definitions(test_httpp_cpp PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
if(WIN32)
    # utest.h includes <Windows.h>; keep it from dragging in the legacy <winsock.h>, which conflicts
    # with <winsock2.h> (used by test_event_loop.cpp and by Asio).
    target_compile_definitions(test_httpp_cpp PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(test_httpp_cpp PRIVATE ws2_32 mswsock)
endif()
if(MSVC)
    set_source_files_properties(${PROJECT_SOURCE_DIR}/src/core/event_loop.cpp ${PROJECT_SOURCE_DIR}/src/core/async_http.cpp ${PROJECT_SOURCE_DIR}/src/core/async_server.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
endif()

add_test(NAME test_httpp_cpp COMMAND $<TARGET_FILE:test_httpp_cpp>)

add_custom_target(httpp_test_cpp
    COMMAND ${CMAKE_COMMAND} -E echo "-- Running c++ tests..."
    COMMAND $<TARGET_FILE:test_httpp_cpp>
    WORKING_DIRECTORY ${HTTPP_TEST_BINARY_DIR}
)
add_dependencies(httpp_test_cpp test_httpp_cpp)
add_dependencies(httpp_test httpp_test_cpp)
