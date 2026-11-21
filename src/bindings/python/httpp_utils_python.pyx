# distutils: language = c++
# cython: language_level=3, freethreading_compatible=True

"""
The log part of the Python bindings (ADR-0086): library logging (httpp.set_log_*) and the bridge for any C++ logger tree
(httpp.bridge_logger, ADR-0011). This module links only libhttpp_utils, so importing it does not load libhttpp (the HTTP stack).
httpp_python.pyx has the rest. Both modules in one process use the one logger registry of libhttpp_utils.
"""

from libcpp.string cimport string
from libcpp cimport bool as cbool

import os


cdef bytes _fs(object p):
    """A path argument (str, bytes or any os.PathLike such as pathlib.Path) as the bytes C++ httpp::path takes (ADR-0054)."""
    return os.fsencode(p)


# ---------------------------------------------------------------------------------------------
# Library logging (httpp/log.hpp). The C++ core logs from its own threads, so the callback form
# takes the GIL; the setters release it, because they wait for the core's log lock, which a core
# thread may hold while it waits for the GIL.
# ---------------------------------------------------------------------------------------------

cdef extern from "httpp/logging.hpp" namespace "httpp":
    # the library's level is httpp::logging::level (ADR-0011); the binding sets it by name, so it needs no copy of the enum
    cbool cpp_set_log_level "httpp::set_log_level"(const string&) except + nogil
    cbool cpp_set_log_file "httpp::set_log_file"(const string&) except + nogil
    cdef enum class log_system:
        native
        syslog
        android
        unified
        debug_string
        event_log
    cbool cpp_set_log_system "httpp::set_log_system"(log_system, const string&) except + nogil

cdef extern from *:
    """
    #include "httpp/logging.hpp"
    typedef void (*httpp_py_log_fn)(void*, int, const std::string&, const std::string&);
    static void httpp_py_set_log_callback(void* ctx, httpp_py_log_fn fn) {
        if (!fn) { httpp::set_log_callback({}); return; }
        httpp::set_log_callback([ctx, fn](httpp::logging::level l, const std::string& where, const std::string& msg) {
            fn(ctx, static_cast<int>(l), where, msg);
        });
    }
    """
    ctypedef void (*log_fn "httpp_py_log_fn")(void*, int, const string&, const string&) noexcept nogil
    void httpp_py_set_log_callback(void*, log_fn) except + nogil

_LOG_LEVELS = ("trace", "debug", "info", "warning", "error", "off")
_LEVEL_NAMES = {5: "trace", 10: "debug", 20: "info", 30: "warning", 40: "error"}  # the numbers of httpp::logging::level the library logs at
_log_callback = None  # keeps the Python callable alive while the core may call it


def set_log_level(level):
    """Set the library's log level: "trace", "debug", "info", "warning" (the default), "error" or "off"."""
    if level not in _LOG_LEVELS:
        raise ValueError("log level must be one of %s, not %r" % (", ".join(_LOG_LEVELS), level))
    cdef string name = level.encode("ascii")
    with nogil:
        cpp_set_log_level(name)


def set_log_file(path):
    """Write the library's log to `path` (truncated) instead of stderr; "" goes back to stderr."""
    cdef string p = _fs(path) if path else b""
    cdef cbool ok
    with nogil:
        ok = cpp_set_log_file(p)
    if not ok:
        raise OSError("cannot open log file %r" % (path,))


_LOG_SYSTEMS = ("native", "syslog", "android", "unified", "debug_string", "event_log")  # the order of httpp::log_system


def set_log_system(which="native", name="httpp"):
    """Send the library's log to the operating system's log instead of stderr: "syslog" (journalctl), "android"
    (logcat), "unified" (Apple's Console), "debug_string" (Windows debugger output), "event_log" (Windows Event
    Viewer), or "native" for the usual one of this platform. `name` is what the system files the lines under
    (syslog ident, unified subsystem, event source). ValueError for an unknown `which`; OSError (the previous output
    is kept) when this platform does not have it or it cannot be opened."""
    if which not in _LOG_SYSTEMS:
        raise ValueError("which must be one of %s, not %r" % (", ".join(_LOG_SYSTEMS), which))
    cdef int idx = _LOG_SYSTEMS.index(which)
    cdef log_system sys = <log_system>idx
    cdef string n = name.encode("utf-8")
    cdef cbool ok
    with nogil:
        ok = cpp_set_log_system(sys, n)
    if not ok:
        raise OSError("the %s log is not available here" % which)


cdef extern from "Python.h":
    int Py_IsInitialized() nogil


cdef void _log_trampoline(void* ctx, int level, const string& where, const string& msg) noexcept nogil:
    # A library thread can log while the interpreter shuts down; taking the GIL then would touch a dead interpreter
    # (Py_IsInitialized turns false early enough in Py_FinalizeEx; the atexit hook in _log.py also unhooks us).
    if not Py_IsInitialized():
        return
    with gil:
        try:
            (<object>ctx)(_LEVEL_NAMES.get(level, "error"), where.decode("utf-8", "replace"), msg.decode("utf-8", "replace"))
        except BaseException:
            pass  # a failing log callback must never take a library thread down


def set_log_callback(callback):
    """Send the library's log lines to `callback(level, where, message)` (strings) instead of stderr.
    It is called from library threads, one call at a time; it must not call into httpp. Pass None to
    go back to stderr. By default the Python package already installs a callback that sends the lines to
    logging.getLogger("httpp") (see httpp/_log.py); calling this replaces that bridge."""
    global _log_callback
    if callback is None:
        with nogil:
            httpp_py_set_log_callback(NULL, NULL)
        _log_callback = None
        return
    if not callable(callback):
        raise TypeError("callback must be callable or None")
    previous = _log_callback
    _log_callback = callback  # reference first, so it outlives the registration below
    with nogil:
        httpp_py_set_log_callback(<void*>callback, <log_fn>_log_trampoline)
    del previous


# ---------------------------------------------------------------------------------------------
# A bridge for any C++ logger tree (ADR-0011). `httpp.bridge_logger("libspeech")` sends every record of the logger
# "libspeech" and of the loggers below it ("libspeech.dsp.stft") to one Python function, which hands it to the Python
# logger of the same name (httpp/_log.py). Unlike set_log_callback it does not touch the "httpp" logger's output and
# it keeps the logger's name.
# ---------------------------------------------------------------------------------------------

cdef extern from *:
    """
    #include <Python.h>
    #include <thread>
    #include <vector>
    #include "httpp/logging.hpp"
    typedef void (*httpp_py_bridge_fn)(void*, int, const std::string&, const std::string&, const std::string&);
    static void httpp_py_bridge_logger(const std::string& name, void* ctx, httpp_py_bridge_fn fn) {
        auto& lg = httpp::logging::get_logger(name);
        lg.set_propagate(false);  // Python's logging does the propagating: a line must not also reach C++ parents
        lg.clear_handlers();
        // serialized = false (ADR-0074): fn takes the GIL, and a Python thread that holds the GIL while it logs
        // through this logger would otherwise wait for the handler's lock that a library thread holds while it waits for the GIL.
        lg.add_handler(httpp::logging::callback_handler([ctx, fn](const httpp::logging::record& r) {
            fn(ctx, static_cast<int>(r.levelno), std::string(r.name), std::string(r.where), std::string(r.message));
        }, false));
    }
    static void httpp_py_unbridge_logger(const std::string& name) {
        auto& lg = httpp::logging::get_logger(name);
        lg.clear_handlers();
        lg.set_propagate(true);
    }
    static void httpp_py_set_logger_level(const std::string& name, int levelno) {
        httpp::logging::get_logger(name).set_level(static_cast<httpp::logging::level>(levelno));
    }
    // For the tests: `threads` library threads and the calling thread each log `count` records. The caller logs
    // while it HOLDS the GIL (a binding without nogil) and releases it only to wait for the threads.
    static void httpp_py_log_from_cpp(const std::string& name, int levelno, const std::string& where,
                                      const std::string& message, int count, int threads) {
        auto& lg = httpp::logging::get_logger(name);
        auto lv = static_cast<httpp::logging::level>(levelno);
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&lg, lv, &where, &message, count] {
                for (int i = 0; i < count; ++i) lg.log_from(lv, where, message);
            });
        for (int i = 0; i < count; ++i) lg.log_from(lv, where, message);
        PyThreadState* ts = PyEval_SaveThread();
        for (auto& th : pool) th.join();
        PyEval_RestoreThread(ts);
    }
    """
    ctypedef void (*bridge_fn "httpp_py_bridge_fn")(void*, int, const string&, const string&, const string&) noexcept nogil
    void httpp_py_bridge_logger(const string&, void*, bridge_fn) except + nogil
    void httpp_py_unbridge_logger(const string&) except + nogil
    void httpp_py_set_logger_level(const string&, int) except + nogil
    void httpp_py_log_from_cpp(const string&, int, const string&, const string&, int, int) except +  # holds the GIL on purpose

_bridged = {}  # logger name -> the Python callable the core calls (kept alive while it is registered)


cdef void _bridge_trampoline(void* ctx, int level, const string& name, const string& where, const string& msg) noexcept nogil:
    # Same rule as _log_trampoline: a library thread that logs while the interpreter shuts down must not take the GIL.
    if not Py_IsInitialized():
        return
    with gil:
        try:
            (<object>ctx)(level, name.decode("utf-8", "replace"), where.decode("utf-8", "replace"), msg.decode("utf-8", "replace"))
        except BaseException:
            pass  # a failing log callback must never take a library thread down


def _bridge_logger(name, callback):
    """Send the records of the C++ logger `name` and of every logger below it to `callback(levelno, logger_name, where,
    message)` (an int and three strings). It is called from library threads, possibly several at once; it must not call
    into httpp. Applications use httpp.bridge_logger (httpp/_log.py), which hands the records to Python's logging."""
    if not callable(callback):
        raise TypeError("callback must be callable")
    cdef string n = name.encode("utf-8")
    previous = _bridged.get(name)
    _bridged[name] = callback  # reference first, so it outlives the registration below
    with nogil:
        httpp_py_bridge_logger(n, <void*>callback, <bridge_fn>_bridge_trampoline)
    del previous


def _unbridge_logger(name):
    """Stop calling the callback of `name`; its records are dropped (the logger keeps no handler)."""
    cdef string n = name.encode("utf-8")
    with nogil:
        httpp_py_unbridge_logger(n)
    _bridged.pop(name, None)


def _set_logger_level(name, int levelno):
    """Set the level of the C++ logger `name` to a Python level number (0 = inherit from the parent)."""
    cdef string n = name.encode("utf-8")
    with nogil:
        httpp_py_set_logger_level(n, levelno)


def _log_from_cpp(name, int levelno, where, message, int count=1, int threads=0):
    """For the tests: log `count` records on the C++ logger `name` from this thread (GIL held) and from `threads` library threads."""
    httpp_py_log_from_cpp(name.encode("utf-8"), levelno, where.encode("utf-8"), message.encode("utf-8"), count, threads)
