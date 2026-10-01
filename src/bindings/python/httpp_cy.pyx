# distutils: language = c++
# cython: language_level=3

"""
Cython bindings over the httpp C++ core (include/httpp.h). This is what
replaces libcurl on the Python side, for both making requests (Client) and
serving HTTP (Server) — the `httpp` CLI (`httpp server ...`, analogous to
`python -m http.server`) is a thin wrapper around Server.
"""

from libcpp.string cimport string
from libcpp cimport bool as cbool
from libcpp.vector cimport vector
from libcpp.pair cimport pair

import traceback


cdef extern from "httpp/client.hpp" namespace "httpp":
    cdef cppclass response:
        int status
        string body
        vector[pair[string, string]] headers
        cbool ok()

    cdef cppclass cpp_client "httpp::client":
        cpp_client(string host, int port) except +
        response get(string path) except + nogil

        @staticmethod
        response fetch(string full_url) except + nogil


cdef extern from "httpp/server.hpp" namespace "httpp":
    cdef cppclass cpp_request "httpp::request":
        string method
        string path
        string body
        vector[pair[string, string]] headers
        vector[pair[string, string]] query
        vector[pair[string, string]] path_params

    cdef cppclass cpp_handler "httpp::handler":
        pass

    cdef cppclass cpp_responder "httpp::responder":
        cpp_responder()
        cpp_responder(const cpp_responder&)
        void send(response res) except + nogil

    cdef cppclass cpp_async_handler "httpp::async_handler":
        pass

    cdef cppclass cpp_server "httpp::server":
        cpp_server() except +
        void route(string method, string path, cpp_handler h) except +
        void route_async(string method, string path, cpp_async_handler h) except +
        void serve_directory(string mount_path, string local_dir) except +
        int bind_to_any_port(string host) except +
        cbool listen_after_bind() except + nogil
        cbool listen(string host, int port) except + nogil
        void stop() except +


# Bridge from a plain C function pointer (+ context) to httpp::handler, which
# is a std::function Cython can't build itself.
cdef extern from *:
    """
    #include "httpp/server.hpp"
    typedef void (*httpp_py_trampoline)(void*, const httpp::request&, httpp::response&);
    static httpp::handler httpp_py_make_handler(httpp_py_trampoline fn, void* ctx) {
        return [fn, ctx](const httpp::request& q, httpp::response& r) { fn(ctx, q, r); };
    }
    typedef void (*httpp_py_async_trampoline)(void*, const httpp::request&, const httpp::responder&);
    static httpp::async_handler httpp_py_make_async_handler(httpp_py_async_trampoline fn, void* ctx) {
        return [fn, ctx](const httpp::request& q, httpp::responder r) { fn(ctx, q, r); };
    }
    """
    ctypedef void (*trampoline_fn "httpp_py_trampoline")(void*, const cpp_request&, response&) noexcept nogil
    cpp_handler make_handler "httpp_py_make_handler"(trampoline_fn fn, void* ctx)
    ctypedef void (*async_trampoline_fn "httpp_py_async_trampoline")(void*, const cpp_request&, const cpp_responder&) noexcept nogil
    cpp_async_handler make_async_handler "httpp_py_make_async_handler"(async_trampoline_fn fn, void* ctx)


cdef extern from "httpp/download.hpp" namespace "httpp":
    cdef cppclass download_result:
        cbool ok
        cbool skipped
        int status
        string error

    cdef cppclass cpp_download "httpp::download":
        cpp_download(string url, string dest_path) except +
        cpp_download& output(string dest_path) except +
        cpp_download& enable_progress(cbool enable) except +
        cpp_download& disable_progress() except +
        download_result run() except + nogil


cdef extern from "httpp/client.hpp" namespace "httpp::client":
    cdef cppclass cpp_curl_request "httpp::client::request":
        cpp_curl_request(string url) except +
        cpp_curl_request& method(string m) except +
        cpp_curl_request& header(string name, string value) except +
        cpp_curl_request& data(string body) except +
        cpp_curl_request& content_type(string type) except +
        response run() except + nogil


cdef class Response:
    """A response to an HTTP request made via Client.get() / Request.run()."""
    cdef int _status
    cdef bytes _body
    cdef list _headers  # list of (str, str) tuples, in server order

    def __init__(self, int status, bytes body, list headers=None):
        self._status = status
        self._body = body
        self._headers = headers or []

    @property
    def status(self):
        return self._status

    @property
    def body(self):
        return self._body.decode("utf-8", errors="replace")

    @property
    def ok(self):
        return 200 <= self._status < 300

    @property
    def headers(self):
        """All response headers, as a list of (name, value) tuples."""
        return list(self._headers)

    def header(self, str name):
        """Case-insensitive lookup of the first matching header, or None."""
        lname = name.lower()
        for k, v in self._headers:
            if k.lower() == lname:
                return v
        return None


cdef _make_response(response res):
    """Convert a C++ httpp::response into a Python Response, headers and all."""
    cdef list headers = [
        (h.first.decode("utf-8", errors="replace"), h.second.decode("utf-8", errors="replace"))
        for h in res.headers
    ]
    return Response(res.status, res.body, headers)


cdef class Client:
    """Minimal HTTP client. Replaces reaching for libcurl/requests for
    simple request/response use cases."""
    cdef cpp_client* _cli

    def __init__(self, str host, int port):
        self._cli = new cpp_client(host.encode("utf-8"), port)

    def __dealloc__(self):
        if self._cli is not NULL:
            del self._cli

    def get(self, str path):
        cdef string p = path.encode("utf-8")
        cdef response res
        with nogil:  # else a Python-route server in this process can't get the GIL: deadlock
            res = self._cli.get(p)
        return _make_response(res)

    @staticmethod
    def fetch(str full_url):
        """Parse `full_url` and GET it in one call — no manual URL parsing
        needed on the Python side either (see httpp::client::fetch)."""
        cdef string u = full_url.encode("utf-8")
        cdef response res
        with nogil:
            res = cpp_client.fetch(u)
        return _make_response(res)


cdef list _pairs(const vector[pair[string, string]]& v):
    cdef list out = []
    cdef size_t i
    for i in range(v.size()):
        out.append((v[i].first.decode("utf-8", errors="replace"),
                    v[i].second.decode("utf-8", errors="replace")))
    return out


cdef tuple _request_args(const cpp_request& req):
    # (method, path, body, headers, query, path_params): the shape httpp/server.py expects.
    cdef bytes body_in = req.body
    return (
        req.method.decode("utf-8", errors="replace"),
        req.path.decode("utf-8", errors="replace"),
        body_in,
        _pairs(req.headers), _pairs(req.query), _pairs(req.path_params),
    )


cdef void _fill_response(response& res, int status, bytes body, list headers) except *:
    cdef string k, v
    res.status = status
    res.body = body
    for name, value in headers:
        k = (<str>name).encode("utf-8")
        v = (<str>value).encode("utf-8")
        res.headers.push_back(pair[string, string](k, v))


cdef void _run_handler(object fn, const cpp_request& req, response& res) except *:
    # fn(*request_args) -> (status, body, headers)
    status, body, headers = fn(*_request_args(req))
    _fill_response(res, status, <bytes>body, <list>headers)


cdef void _dispatch(void* ctx, const cpp_request& req, response& res) noexcept nogil:
    # Called from a cpp-httplib worker thread that Python knows nothing about,
    # hence the explicit GIL acquire.
    with gil:
        try:
            _run_handler(<object>ctx, req, res)
        except BaseException:
            # The traceback goes to stderr for the developer; the client only
            # ever sees a generic 500 (no internals leaked).
            traceback.print_exc()
            res.status = 500
            res.body = b"Internal Server Error"
            res.headers.clear()


cdef class _Done:
    """One-shot handle for an async route: calling it sends the response and
    closes the connection. Safe to call from any thread; extra calls are ignored.
    If it is dropped without being called, the connection is closed unanswered."""
    cdef cpp_responder* _r

    def __cinit__(self):
        self._r = NULL

    def __dealloc__(self):
        if self._r != NULL:
            del self._r
            self._r = NULL

    def __call__(self, int status, bytes body, list headers):
        cdef response res
        _fill_response(res, status, body, headers)
        with nogil:
            self._r.send(res)


cdef void _dispatch_async(void* ctx, const cpp_request& req, const cpp_responder& r) noexcept nogil:
    # Runs on a cpp-httplib worker thread, which is released as soon as this
    # returns; the response is sent later through `done`.
    with gil:
        done = _Done.__new__(_Done)
        (<_Done>done)._r = new cpp_responder(r)
        try:
            (<object>ctx)(*_request_args(req), done)
        except BaseException:
            traceback.print_exc()
            try:
                done(500, b"Internal Server Error", [])
            except BaseException:
                pass


cdef class Server:
    """Minimal HTTP server. Replaces spinning up cpp-httplib/libcurl
    yourself — this is also what `httpp server` (the CLI, similar to
    Python's own `python -m http.server`) is built on."""
    cdef cpp_server* _srv
    cdef list _handlers  # keeps route callables alive; C++ only holds a borrowed pointer

    def __init__(self):
        self._srv = new cpp_server()
        self._handlers = []

    def __dealloc__(self):
        if self._srv is not NULL:
            del self._srv

    def serve_directory(self, str mount_path, str local_dir):
        """Serve `local_dir` under `mount_path`, like `python -m http.server`."""
        self._srv.serve_directory(mount_path.encode("utf-8"), local_dir.encode("utf-8"))

    def add_route(self, str method, str path, handler):
        """Low-level: register `handler` for `method` + `path` ("/users/:id").
        Use the decorators on httpp.Server (see httpp/server.py) instead.
        Must be called before listen()."""
        if not callable(handler):
            raise TypeError("handler must be callable")
        self._handlers.append(handler)
        self._srv.route(method.encode("utf-8"), path.encode("utf-8"),
                        make_handler(_dispatch, <void*>handler))

    def add_async_route(self, str method, str path, dispatch):
        """Low-level, like add_route, but non-blocking: `dispatch(method, path,
        body, headers, query, path_params, done)` must return quickly and
        arrange for `done(status, body, headers)` to be called later, from any
        thread. The worker thread is free in the meantime."""
        if not callable(dispatch):
            raise TypeError("dispatch must be callable")
        self._handlers.append(dispatch)
        self._srv.route_async(method.encode("utf-8"), path.encode("utf-8"),
                              make_async_handler(_dispatch_async, <void*>dispatch))

    def bind_to_any_port(self, str host):
        return self._srv.bind_to_any_port(host.encode("utf-8"))

    def listen_after_bind(self):
        cdef cbool ok
        with nogil:
            ok = self._srv.listen_after_bind()
        if not ok:
            raise OSError("httpp: server failed to listen")

    def listen(self, str host, int port):
        cdef string h = host.encode("utf-8")
        cdef cbool ok
        with nogil:
            ok = self._srv.listen(h, port)
        if not ok:
            raise OSError(f"httpp: could not listen on {host}:{port} (port in use, or no permission?)")

    def stop(self):
        self._srv.stop()


cdef class DownloadResult:
    """Result of Download.run()."""
    cdef cbool _ok
    cdef int _status
    cdef bytes _error

    def __init__(self, cbool ok, int status, bytes error):
        self._ok = ok
        self._status = status
        self._error = error

    @property
    def ok(self):
        return self._ok

    @property
    def status(self):
        return self._status

    @property
    def error(self):
        return self._error.decode("utf-8", errors="replace")


cdef class Download:
    """Fluent builder over httpp::download:

        Download(url, path).run()
        Download(url).output(path).enable_progress().run()
    """
    cdef cpp_download* _dl

    def __init__(self, str url, str dest_path=""):
        self._dl = new cpp_download(url.encode("utf-8"), dest_path.encode("utf-8"))

    def __dealloc__(self):
        if self._dl is not NULL:
            del self._dl

    def output(self, str dest_path):
        self._dl.output(dest_path.encode("utf-8"))
        return self

    def enable_progress(self, cbool enable=True):
        self._dl.enable_progress(enable)
        return self

    def disable_progress(self):
        self._dl.disable_progress()
        return self

    def run(self):
        cdef download_result res
        with nogil:
            res = self._dl.run()
        return DownloadResult(res.ok, res.status, res.error)


def download(str url, str dest_path, cbool show_progress=True):
    """Download `url` to `dest_path`, with a tqdm-like terminal progress
    bar unless show_progress=False. Shorthand for
    Download(url, dest_path).enable_progress(show_progress).run()."""
    return Download(url, dest_path).enable_progress(show_progress).run()

cdef class Request:
    """A small, curl-flavored fluent request builder — the common cases
    only (-X method, -H headers, -d data). NOT a libcurl-compatible shim;
    see Client for the plain request API this is built on.

        Request(url).method("PUT").header("X-Token", "abc").data("body").run()
    """
    cdef cpp_curl_request* _req

    def __init__(self, str url):
        self._req = new cpp_curl_request(url.encode("utf-8"))

    def __dealloc__(self):
        if self._req is not NULL:
            del self._req

    def method(self, str m):
        self._req.method(m.encode("utf-8"))
        return self

    def header(self, str name, str value):
        self._req.header(name.encode("utf-8"), value.encode("utf-8"))
        return self

    def data(self, str body):
        self._req.data(body.encode("utf-8"))
        return self

    def content_type(self, str type_):
        self._req.content_type(type_.encode("utf-8"))
        return self

    def run(self):
        cdef response res
        with nogil:
            res = self._req.run()
        return _make_response(res)
