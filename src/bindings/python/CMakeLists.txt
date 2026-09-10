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

    # Create the Python extension module. httpp_core (httplib.h compiled
    # in, hidden visibility) is linked PRIVATE, so httplib never leaks
    # into this .so's public symbols or into the Python package's headers.
    python_add_library(httpp_cy MODULE "${httpp_cxx_file}" WITH_SOABI ${USE_SABI})

    target_link_libraries(httpp_cy PRIVATE httpp::core)

    target_compile_definitions(httpp_cy PRIVATE
        VERSION_INFO=${PROJECT_VERSION}
    )

    # Install the compiled extension module + the pure-Python package files
    install(TARGETS httpp_cy
        DESTINATION httpp
    )
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/httpp/
        DESTINATION httpp
        FILES_MATCHING PATTERN "*.py"
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
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=${PYTHON_INSTALL_DIR}

    INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install --parallel 8
    BUILD_ALWAYS 1
)
add_dependencies(build_python httpp_core)
