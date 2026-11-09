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
include "cy/_externs.pxi"
include "cy/_client.pxi"
include "cy/_request.pxi"
include "cy/_async_route.pxi"
include "cy/_server.pxi"
include "cy/_download.pxi"
include "cy/_url_request.pxi"
include "cy/_async_client.pxi"
include "cy/_misc.pxi"
