if(DEFINED SKBUILD)
    # ${SKBUILD_SABI_COMPONENT} expands to Development.SABIModule when
    # scikit-build-core targets the Stable ABI (see wheel.py-api in
    # pyproject.toml), and to nothing otherwise.
    find_package(Python REQUIRED COMPONENTS Interpreter Development.Module ${SKBUILD_SABI_COMPONENT})

    find_package(Cython MODULE REQUIRED VERSION 3.0)
    include(UseCython)

    cython_transpile(${CMAKE_CURRENT_SOURCE_DIR}/httpp_cy.pyx
        LANGUAGE CXX
        OUTPUT_VARIABLE httpp_cxx_file)

    if(NOT "${SKBUILD_SABI_VERSION}" STREQUAL "")
        set(USE_SABI USE_SABI ${SKBUILD_SABI_VERSION})
    endif()
    if(SKBUILD_SOABI)
        set(Python_SOABI ${SKBUILD_SOABI})
    endif()

    # Create the Python extension module. httpp_core (mbedtls and the
    # parsers compiled in, hidden visibility) is linked PRIVATE, so they never leak
    # into this .so's public symbols or into the Python package's headers.
    python_add_library(httpp_cy MODULE "${httpp_cxx_file}" WITH_SOABI ${USE_SABI})

    target_link_libraries(httpp_cy PRIVATE httpp::core)
    if(WIN32) # httpp_py_inbox_push (in httpp_cy.pyx) calls send() and WSAGetLastError() itself
        target_link_libraries(httpp_cy PRIVATE ws2_32)
    endif()

    target_compile_definitions(httpp_cy PRIVATE
        VERSION_INFO=${PROJECT_VERSION}
    )

    # libhttpp_core is installed under `httpp/sdk/lib/` (the wheel's SDK folder:
    # `httpp/sdk/{bin,include,lib}`, the shape of an SDK install prefix - see
    # below), not next to the Python module itself. So the loader needs to look
    # in a folder below wherever it is:
    #   - Linux:   $ORIGIN/sdk/lib
    #   - macOS:   @loader_path/sdk/lib (dylib's own id is set to @rpath below)
    #   - Windows: no RPATH concept - handled at import time instead, see
    #              httpp/__init__.py (os.add_dll_directory(get_lib_dir())).
    if(APPLE)
        set(_httpp_rpath "@loader_path/sdk/lib")
    elseif(UNIX)
        set(_httpp_rpath "$ORIGIN/sdk/lib")
    endif()
    if(_httpp_rpath)
        set_target_properties(httpp_cy PROPERTIES
            INSTALL_RPATH "${_httpp_rpath}"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    endif()

    # The request validators (httpp/_validate.py builds them, _validate_cy.pyx runs them). Pure Cython:
    # no link to httpp_core, so no rpath either.
    cython_transpile(${CMAKE_CURRENT_SOURCE_DIR}/_validate_cy.pyx
        LANGUAGE CXX
        OUTPUT_VARIABLE validate_cxx_file)
    python_add_library(_validate_cy MODULE "${validate_cxx_file}" WITH_SOABI ${USE_SABI})

    # Install the compiled extension modules + the pure-Python package files
    install(TARGETS httpp_cy _validate_cy
        DESTINATION httpp
        COMPONENT httpp_wheel
    )
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/httpp/
        DESTINATION httpp
        COMPONENT httpp_wheel
        FILES_MATCHING PATTERN "*.py"
    )
    # The `httpp` command (ADR-0041): a module next to the package, not in it, so starting it does not import the package.
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/httpp_cli.py
        DESTINATION .
        COMPONENT httpp_wheel
    )

    # --- also ship the C++ library inside the wheel ---
    # `pip install httpp` gives you not just the Python module but a usable
    # C++ package too: headers, the compiled core library, and a CMake
    # config, all under the installed `httpp/sdk` folder (the
    # nanobind/pybind11 pattern: httpp.get_cmake_dir() etc., see
    # httpp/__init__.py). A downstream CMakeLists.txt can then do:
    #
    #   execute_process(COMMAND ${Python3_EXECUTABLE} -c
    #       "import httpp; print(httpp.get_cmake_dir())"
    #       OUTPUT_VARIABLE httpp_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    #   find_package(httpp CONFIG REQUIRED)
    #   target_link_libraries(app PRIVATE httpp::httpp)
    #
    # (see examples/find_httpp_via_python/ for a full working example.)
    install(TARGETS httpp_core
        EXPORT httppTargets
        ARCHIVE DESTINATION httpp/sdk/lib COMPONENT httpp_wheel
        LIBRARY DESTINATION httpp/sdk/lib COMPONENT httpp_wheel
        # RUNTIME is the DLL on Windows: next to httpp/sdk/bin/httpp.exe, which has no RPATH and so finds it only there (or on PATH).
        # The import library stays in sdk/lib (ARCHIVE), as in any SDK prefix. Linux and macOS have no RUNTIME artifact here.
        RUNTIME DESTINATION httpp/sdk/bin COMPONENT httpp_wheel
    )
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
        DESTINATION httpp/sdk/include
        COMPONENT httpp_wheel
    )
    install(EXPORT httppTargets
        FILE httppTargets.cmake
        NAMESPACE httpp::
        DESTINATION httpp/sdk/lib/cmake/httpp
        COMPONENT httpp_wheel
    )

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        ${CMAKE_SOURCE_DIR}/cmake/httppConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/httppConfig.cmake
        INSTALL_DESTINATION httpp/sdk/lib/cmake/httpp
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/httppConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/httppConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/httppConfigVersion.cmake
        DESTINATION httpp/sdk/lib/cmake/httpp
        COMPONENT httpp_wheel
    )

    return()
endif()

# Standalone (non-SKBUILD) build: build the extension in-tree via
# ExternalProject so `cmake --build` + `ctest` can exercise it the same
# way `pip install .` would, without re-running the whole top-level
# configure under scikit-build-core.
include(ExternalProject)
ExternalProject_Add(build_python
    SOURCE_DIR ${CMAKE_SOURCE_DIR}
    BINARY_DIR ${CMAKE_BINARY_DIR}/python/build
    INSTALL_DIR ${PYTHON_INSTALL_DIR}

    CMAKE_ARGS
        --no-warn-unused-cli
        -DSKBUILD=2
        -DHTTPP_ASIO_INCLUDE_DIR=${HTTPP_ASIO_INCLUDE_DIR}
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=${PYTHON_INSTALL_DIR}

    INSTALL_COMMAND ${CMAKE_COMMAND} --install . --config Release --component httpp_wheel
    BUILD_ALWAYS 1
)
add_dependencies(build_python httpp_core)
