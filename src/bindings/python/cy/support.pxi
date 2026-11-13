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
    cdef cppclass cpp_swagger_ui "httpp::swagger_ui":
        cpp_swagger_ui() except +
        cpp_swagger_ui& set_docs_path(string path) except +
        cpp_swagger_ui& set_openapi_url(string url)
        cpp_swagger_ui& set_title(string title)
        void mount(cpp_server& s) except +


cdef extern from "httpp/file_browser.hpp" namespace "httpp":
    cdef cppclass cpp_file_browser "httpp::file_browser":
        cpp_file_browser(const string& root) except +
        cpp_file_browser& set_title(string title)
        cpp_file_browser& set_accent(string colour) except +
        cpp_file_browser& set_search(bint on)
        cpp_file_browser& set_show_hidden(bint on)
        cpp_file_browser& set_max_entries(size_t count) except +
        void mount(cpp_server& s, const string& base) except +

# The three enum setters take the enum's number: the numbers are checked against the C++ enums right here, so a reordering there fails the build.
cdef extern from *:
    """
    static_assert(static_cast<int>(httpp::file_browser::theme_mode::system) == 0 && static_cast<int>(httpp::file_browser::theme_mode::light) == 1 &&
                  static_cast<int>(httpp::file_browser::theme_mode::dark) == 2, "update the Theme enum in cy/file_browser.pxi");
    static_assert(static_cast<int>(httpp::file_browser::view_mode::list) == 0 && static_cast<int>(httpp::file_browser::view_mode::grid) == 1,
                  "update the View enum in cy/file_browser.pxi");
    static_assert(static_cast<int>(httpp::file_browser::sort_key::name) == 0 && static_cast<int>(httpp::file_browser::sort_key::size) == 1 &&
                  static_cast<int>(httpp::file_browser::sort_key::date) == 2, "update the Sort enum in cy/file_browser.pxi");
    inline void httpp_cy_fb_theme(httpp::file_browser& b, int v) { b.set_theme(static_cast<httpp::file_browser::theme_mode>(v)); }
    inline void httpp_cy_fb_view(httpp::file_browser& b, int v) { b.set_view(static_cast<httpp::file_browser::view_mode>(v)); }
    inline void httpp_cy_fb_sort(httpp::file_browser& b, int v) { b.set_sort(static_cast<httpp::file_browser::sort_key>(v)); }
    """
    void fb_set_theme "httpp_cy_fb_theme"(cpp_file_browser& b, int v)
    void fb_set_view "httpp_cy_fb_view"(cpp_file_browser& b, int v)
    void fb_set_sort "httpp_cy_fb_sort"(cpp_file_browser& b, int v)



cdef extern from "httpp/server.hpp" namespace "httpp":
    cdef cppclass cpp_directory "httpp::directory":
        cpp_directory(const string& root) except +
        cpp_directory& set_index(bint on)
        cpp_directory& set_show_hidden(bint on)
        cpp_directory& set_download_active(bint on)
        void mount(cpp_server& s, const string& base) except +

cdef extern from "httpp/middleware.hpp" namespace "httpp":
    cdef cppclass cpp_cors "httpp::cors":
        cpp_cors() except +
        cpp_cors& set_allow_origins(vector[string] v) except +
        cpp_cors& set_allow_origin_regex(string v) except +
        cpp_cors& set_allow_methods(vector[string] v) except +
        cpp_cors& set_allow_headers(vector[string] v) except +
        cpp_cors& set_allow_credentials(cbool v) except +
        cpp_cors& set_expose_headers(vector[string] v) except +
        cpp_cors& set_max_age(int v) except +
        void install(cpp_server& s) except +
    cdef cppclass cpp_trusted_host "httpp::trusted_host":
        cpp_trusted_host(vector[string] hosts) except +
        cpp_trusted_host& set_www_redirect(cbool v) except +
        void install(cpp_server& s) except +
    cdef cppclass cpp_forwarded_headers "httpp::forwarded_headers":
        cpp_forwarded_headers() except +
        cpp_forwarded_headers& set_trusted_proxies(vector[string] v) except +
        cpp_forwarded_headers& set_trust_forwarded_host(cbool v) except +
        void install(cpp_server& s) except +
    cdef cppclass cpp_https_redirect "httpp::https_redirect":
        cpp_https_redirect() except +
        cpp_https_redirect& set_trust_forwarded_proto(cbool v) except +
        void install(cpp_server& s) except +


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
