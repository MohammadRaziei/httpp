# distutils: language = c++
# cython: language_level=3, freethreading_compatible=True

"""
Cython bindings over the httpp C++ core (include/httpp.h). This is what
replaces libcurl on the Python side, for both making requests (Client) and
serving HTTP (Server) - the `httpp` CLI (`httpp server ...`, analogous to
`python -m http.server`) is a thin wrapper around Server.
"""

from libcpp.string cimport string
from libcpp.string_view cimport string_view
from libcpp cimport bool as cbool
from libcpp.vector cimport vector
from libcpp.pair cimport pair
from libcpp.memory cimport shared_ptr
from cython.operator cimport dereference as deref
from libc.stddef cimport size_t
from cpython.ref cimport Py_INCREF, Py_DECREF

import os
import traceback

# The module is split into textual includes (Cython .pxi files), in this order; one module, one .so.
include "cy/declarations.pxi"
include "cy/client.pxi"
include "cy/request.pxi"
include "cy/async_routes.pxi"
include "cy/server.pxi"
include "cy/transfer.pxi"
include "cy/support.pxi"
