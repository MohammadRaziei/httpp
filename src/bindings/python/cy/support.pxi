# Swagger UI, file browser and middleware mounts. Included by httpp_python.pyx (a textual include: the order of the includes matters).

cdef extern from "Python.h":
    int Py_IsInitialized() nogil

cdef extern from "httpp/swagger.hpp" namespace "httpp":
    cdef cppclass swagger_ui_asset_t "httpp::swagger_ui_asset":
        string_view content_type
        string_view data
        bint gzip
    const swagger_ui_asset_t* find_swagger_ui_asset(string_view name) noexcept
    cdef cppclass cpp_swagger "httpp::swagger":
        cpp_swagger(string openapi_url) except +
        cpp_swagger& set_title(string title)
        void mount(cpp_server s, const string& base) except +


cdef extern from "httpp/file_browser.hpp" namespace "httpp":
    cdef cppclass cpp_file_browser "httpp::file_browser":
        cpp_file_browser(const string& root) except +
        cpp_file_browser& set_title(string title)
        cpp_file_browser& set_accent(string colour) except +
        cpp_file_browser& set_search(bint on)
        cpp_file_browser& set_show_hidden(bint on)
        cpp_file_browser& set_show_dates(bint on)
        cpp_file_browser& set_max_entries(size_t count) except +
        cpp_file_browser& set_safe_mode(bint on)
        cpp_file_browser& set_zip(bint on)
        cpp_file_browser& set_powered_by(bint on)
        cpp_file_browser& set_max_zip_entries(size_t count) except +
        void mount(cpp_server s, const string& base) except +

# The three enum setters take the enum's number: the numbers are checked against the C++ enums right here, so a reordering there fails the build.
cdef extern from *:
    """
    static_assert(static_cast<int>(httpp::file_browser::theme_mode::system) == 0 && static_cast<int>(httpp::file_browser::theme_mode::light) == 1 &&
                  static_cast<int>(httpp::file_browser::theme_mode::dark) == 2, "update the Theme enum in cy/file_browser.pxi");
    static_assert(static_cast<int>(httpp::file_browser::view_mode::list) == 0 && static_cast<int>(httpp::file_browser::view_mode::grid) == 1,
                  "update the View enum in cy/file_browser.pxi");
    static_assert(static_cast<int>(httpp::file_browser::sort_key::name) == 0 && static_cast<int>(httpp::file_browser::sort_key::size) == 1 &&
                  static_cast<int>(httpp::file_browser::sort_key::date) == 2, "update the Sort enum in cy/file_browser.pxi");
    inline void httpp_python_fb_theme(httpp::file_browser& b, int v) { b.set_theme(static_cast<httpp::file_browser::theme_mode>(v)); }
    inline void httpp_python_fb_view(httpp::file_browser& b, int v) { b.set_view(static_cast<httpp::file_browser::view_mode>(v)); }
    inline void httpp_python_fb_sort(httpp::file_browser& b, int v) { b.set_sort(static_cast<httpp::file_browser::sort_key>(v)); }
    """
    void fb_set_theme "httpp_python_fb_theme"(cpp_file_browser& b, int v)
    void fb_set_view "httpp_python_fb_view"(cpp_file_browser& b, int v)
    void fb_set_sort "httpp_python_fb_sort"(cpp_file_browser& b, int v)



cdef extern from "httpp/server.hpp" namespace "httpp":
    cdef cppclass cpp_directory "httpp::directory":
        cpp_directory(const string& root) except +
        cpp_directory& set_index(bint on)
        cpp_directory& set_show_hidden(bint on)
        cpp_directory& set_show_dates(bint on)
        cpp_directory& set_download_active(bint on)
        cpp_directory& set_inline_active(bint on)
        void mount(cpp_server s, const string& base) except +

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
    cdef cppclass cpp_basic_auth "httpp::basic_auth":
        cpp_basic_auth(string realm) except +
        cpp_basic_auth& set_realm(string v) except +
        cpp_basic_auth& set_users(vector[string] v) except +
        cpp_basic_auth& set_path_prefix(string v) except +
        void install(cpp_server& s) except +
    cdef cppclass cpp_rate_limit "httpp::rate_limit":
        cpp_rate_limit() except +
        cpp_rate_limit& set_rate(double v) except +
        cpp_rate_limit& set_burst(size_t v) except +
        cpp_rate_limit& set_max_clients(size_t v) except +
        void install(cpp_server& s) except +


# basic_auth's password check can be a Python callable. The middleware runs on a server thread, so the call takes the GIL; the callable is kept
# alive by the middleware (released with the GIL when the server goes). An exception in it is printed and becomes a 500 (never a pass).
cdef extern from *:
    """
    #include <Python.h>
    #include <functional>
    #include <memory>
    #include <stdexcept>
    #include <string>
    #include "httpp/middleware.hpp"
    typedef int (*httpp_py_verify_fn)(void*, const std::string&, const std::string&);
    static void httpp_py_set_verifier(httpp::basic_auth& b, void* ctx, httpp_py_verify_fn fn) {
        Py_INCREF(static_cast<PyObject*>(ctx));
        std::shared_ptr<void> hold(ctx, [](void* c) {
            if (!Py_IsInitialized()) return;
            PyGILState_STATE g = PyGILState_Ensure();
            Py_DECREF(static_cast<PyObject*>(c));
            PyGILState_Release(g);
        });
        b.set_verifier([hold, ctx, fn](const std::string& user, const std::string& password) {
            const int r = fn(ctx, user, password);
            if (r < 0) throw std::runtime_error("basic_auth: the verifier raised an exception");
            return r > 0;
        });
    }
    """
    ctypedef int (*verify_fn "httpp_py_verify_fn")(void*, const string&, const string&) noexcept nogil
    void httpp_py_set_verifier(cpp_basic_auth&, void*, verify_fn) except +


cdef int _verify_trampoline(void* ctx, const string& user, const string& password) noexcept nogil:
    if not Py_IsInitialized():
        return -1
    with gil:
        try:
            return 1 if (<object>ctx)(user.decode("utf-8", "surrogateescape"), password.decode("utf-8", "surrogateescape")) else 0
        except BaseException:
            traceback.print_exc()
            return -1


cdef extern from "httpp/server.hpp" namespace "httpp":
    const char* cpp_mime_type_for "httpp::mime_type_for"(const string& path)


def mime_type_for(path):
    """The Content-Type httpp uses for a file by its extension, the same table that mounted directories use (ADR-0047)."""
    cdef string p = os.fsencode(path)
    return cpp_mime_type_for(p).decode("ascii")


def _swagger_ui_asset(str name):
    """(stored bytes, content type, is_gzip) of one Swagger UI file compiled into libhttpp (see include/httpp/swagger.hpp),
    or None. The stored bytes are a gzip stream when is_gzip is true, not the file. They are copied out of the library's
    read-only data each call: cache the result."""
    cdef bytes key = name.encode("utf-8")
    cdef const swagger_ui_asset_t* a = find_swagger_ui_asset(string_view(<const char*>key, len(key)))
    if a == NULL:
        return None
    return (a.data.data()[:a.data.size()],
            a.content_type.data()[:a.content_type.size()].decode("ascii"),
            bool(a.gzip))


def parse_route(str path):
    """The segments of a route path as [(kind, text, type)]: ("lit", "users", "") or ("param", "id", "int").
    type is "str", "int" or "float". Raises ValueError for a malformed path. This is the router's own parser."""
    return [(k.decode(), t.decode(), ty.decode()) for k, t, ty in httpp_py_route_segments(path.encode())]
