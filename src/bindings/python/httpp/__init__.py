"""httpp: a lightweight, zero-system-dependency HTTP client + server."""

import os as _os
import sys as _sys

__version__ = "0.1.0"

_pkg_dir = _os.path.dirname(__file__)
_lib_dir = _os.path.join(_pkg_dir, "lib")

# On Windows, a .pyd's own directory (and its dependencies' directory) isn't
# automatically searched for dependent DLLs — libhttpp_core.dll lives in
# httpp/lib/, next to but not inside httpp/. Linux/macOS solve the
# equivalent problem at link time via RPATH ($ORIGIN/lib / @loader_path/lib,
# see src/bindings/python/CMakeLists.txt), so this only runs on win32.
if _sys.platform == "win32" and hasattr(_os, "add_dll_directory"):
    _os.add_dll_directory(_lib_dir)

# Import the compiled Cython module
from .httpp_cy import Client, Server, download, DownloadFile, DownloadResult


def get_include_dir():
    """Directory containing httpp.h and httpp/*.hpp, for use in a
    downstream CMakeLists.txt or a plain compiler -I flag."""
    return _os.path.join(_pkg_dir, "include")


def get_lib_dir():
    """Directory containing the compiled httpp C++ library."""
    return _lib_dir


def get_cmake_dir():
    """Directory containing httppConfig.cmake, for
    find_package(httpp CONFIG). See the package README for a full example."""
    return _os.path.join(_pkg_dir, "cmake")


__all__ = [
    "Client", "Server", "download", "DownloadFile", "DownloadResult",
    "get_include_dir", "get_lib_dir", "get_cmake_dir",
]
