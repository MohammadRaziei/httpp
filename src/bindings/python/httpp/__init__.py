"""httpp: a lightweight, zero-system-dependency HTTP client + server."""

import os as _os

__version__ = "0.1.0"

# Import the compiled Cython module
from .httpp_cy import Client, Server


def get_include_dir():
    """Directory containing httpp.h and httpp/*.hpp, for use in a
    downstream CMakeLists.txt or a plain compiler -I flag."""
    return _os.path.join(_os.path.dirname(__file__), "include")


def get_lib_dir():
    """Directory containing the compiled httpp C++ library."""
    return _os.path.join(_os.path.dirname(__file__), "lib")


def get_cmake_dir():
    """Directory containing httppConfig.cmake, for
    find_package(httpp CONFIG). See the package README for a full example."""
    return _os.path.join(_os.path.dirname(__file__), "cmake")


__all__ = ["Client", "Server", "get_include_dir", "get_lib_dir", "get_cmake_dir"]
