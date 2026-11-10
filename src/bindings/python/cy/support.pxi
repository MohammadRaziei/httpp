# library logging, Swagger UI, file browser and middleware mounts. Included by httpp_cy.pyx (a textual include: the order of the includes matters).

# ---------------------------------------------------------------------------------------------
# Library logging (httpp/log.hpp). The C++ core logs from its own threads, so the callback form
# takes the GIL; the setters release it, because they wait for the core's log lock, which a core
# thread may hold while it waits for the GIL.
# ---------------------------------------------------------------------------------------------

cdef extern from "httpp/log.hpp" namespace "httpp":
    cdef enum class log_level:
        trace
        debug
        info
        warning
        error
        off
    void cpp_set_log_level "httpp::set_log_level"(log_level) except + nogil
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
    #include "httpp/log.hpp"
    typedef void (*httpp_py_log_fn)(void*, int, const std::string&, const std::string&);
    static void httpp_py_set_log_callback(void* ctx, httpp_py_log_fn fn) {
        if (!fn) { httpp::set_log_callback({}); return; }
        httpp::set_log_callback([ctx, fn](httpp::log_level l, const std::string& where, const std::string& msg) {
            fn(ctx, static_cast<int>(l), where, msg);
        });
    }
    """
    ctypedef void (*log_fn "httpp_py_log_fn")(void*, int, const string&, const string&) noexcept nogil
    void httpp_py_set_log_callback(void*, log_fn) except + nogil

_LOG_LEVELS = ("trace", "debug", "info", "warning", "error", "off")
_log_callback = None  # keeps the Python callable alive while the core may call it


cdef extern from "httpp/swagger_ui.hpp" namespace "httpp":
    cdef cppclass swagger_ui_asset_t "httpp::swagger_ui_asset":
        string_view content_type
        string_view data
        bint gzip
    const swagger_ui_asset_t* find_swagger_ui_asset(string_view name) noexcept
    void mount_swagger_ui(cpp_server& s, string docs_path, string openapi_url, string title) except +


cdef extern from "httpp/file_browser.hpp" namespace "httpp":
    cdef cppclass cpp_file_browser_options "httpp::file_browser_options":
        string title
        string theme
        string accent
        bint search
        bint show_hidden
        size_t max_entries
        string view
        string sort
    void mount_file_browser(cpp_server& s, const string& base, const string& root, const cpp_file_browser_options& options) except +


cdef extern from "httpp/middleware.hpp" namespace "httpp":
    cdef cppclass cpp_cors_options "httpp::cors_options":
        vector[string] allow_origins
        string allow_origin_regex
        vector[string] allow_methods
        vector[string] allow_headers
        cbool allow_credentials
        vector[string] expose_headers
        int max_age
    cdef cppclass cpp_trusted_host_options "httpp::trusted_host_options":
        vector[string] allowed_hosts
        cbool www_redirect
    cdef cppclass cpp_https_redirect_options "httpp::https_redirect_options":
        cbool trust_forwarded_proto
    void add_https_redirect(cpp_server& s, const cpp_https_redirect_options& options) except +
    void add_cors(cpp_server& s, const cpp_cors_options& options) except +
    void add_trusted_host(cpp_server& s, const cpp_trusted_host_options& options) except +


cdef extern from "httpp/server.hpp" namespace "httpp":
    const char* cpp_mime_type_for "httpp::mime_type_for"(const string& path)


def mime_type_for(path):
    """The Content-Type httpp uses for a file by its extension, the same table that mounted directories use (ADR-0047)."""
    cdef string p = os.fsencode(path)
    return cpp_mime_type_for(p).decode("ascii")


def _swagger_ui_asset(str name):
    """(stored bytes, content type, is_gzip) of one Swagger UI file compiled into libhttpp (see include/httpp/swagger_ui.hpp),
    or None. The stored bytes are a gzip stream when is_gzip is true, not the file. They are copied out of the library's
    read-only data each call: cache the result."""
    cdef bytes key = name.encode("utf-8")
    cdef const swagger_ui_asset_t* a = find_swagger_ui_asset(string_view(<const char*>key, len(key)))
    if a == NULL:
        return None
    return (a.data.data()[:a.data.size()],
            a.content_type.data()[:a.content_type.size()].decode("ascii"),
            bool(a.gzip))


def set_log_level(level):
    """Set the library's log level: "trace", "debug", "info", "warning" (the default), "error" or "off"."""
    if level not in _LOG_LEVELS:
        raise ValueError("log level must be one of %s, not %r" % (", ".join(_LOG_LEVELS), level))
    cdef int idx = _LOG_LEVELS.index(level)
    cdef log_level lv = <log_level>idx
    with nogil:
        cpp_set_log_level(lv)


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
            (<object>ctx)(_LOG_LEVELS[level], where.decode("utf-8", "replace"), msg.decode("utf-8", "replace"))
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
