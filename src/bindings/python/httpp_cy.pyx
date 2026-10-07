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
from libc.stddef cimport size_t
from cpython.ref cimport Py_INCREF, Py_DECREF

import os
import traceback


cdef extern from "httpp/client.hpp" namespace "httpp":
    cdef cppclass response:
        int status
        string body
        vector[pair[string, string]] headers
        string error_message
        string file  # server side: stream the body from this path
        cbool ok()
        const char* error_name()

    cdef cppclass cpp_client "httpp::client":
        cpp_client(string host, int port, cbool use_ssl) except +
        cpp_client(string base_url) except +
        response get(string path) except + nogil
        response send(string method, string path, vector[pair[string, string]] headers,
                      string body, string content_type) except + nogil
        cpp_client& set_header(string name, string value) except +
        cpp_client& set_timeout(long seconds) except +
        cpp_client& set_follow_redirects(cbool enable) except +
        cpp_client& set_basic_auth(string user, string password) except +
        cpp_client& set_bearer_token(string token) except +
        cpp_client& set_proxy(string host, int port) except +
        cpp_client& set_verify(cbool enable) except +
        cpp_client& set_ca_file(string path) except +
        cpp_client& set_keep_alive(cbool enable) except +

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

    cdef cppclass cpp_before_hook "httpp::server::before_hook":
        pass

    cdef cppclass cpp_server "httpp::server":
        cpp_server() except +
        void route(string method, string path, cpp_handler h) except +
        void route_async(string method, string path, cpp_async_handler h) except +
        void serve_directory(string mount_path, string local_dir) except +
        # after_hook / error_hook are the same std::function type as handler, so cpp_handler stands for them.
        cpp_server& before(cpp_before_hook h) except +
        cpp_server& after(cpp_handler h) except +
        cpp_server& set_error_handler(cpp_handler h) except +
        cpp_server& set_thread_pool(size_t base, size_t max) except +
        cpp_server& set_read_timeout(int seconds) except +
        cpp_server& set_write_timeout(int seconds) except +
        cpp_server& set_keep_alive_timeout(int seconds) except +
        cpp_server& set_keep_alive_max(size_t requests) except +
        cpp_server& set_max_body(size_t nbytes) except +
        cpp_server& set_async_timeout(int seconds) except +
        int bind_to_any_port(string host) except +
        cbool listen_after_bind() except + nogil
        cbool listen(string host, int port) except + nogil
        void stop() except +


# Bridge from a plain C function pointer (+ context) to httpp::handler, which
# is a std::function Cython can't build itself.
cdef extern from *:
    """
    #include "httpp/server.hpp"
    #include <memory>
    #include <stdexcept>
    typedef bool (*httpp_py_before_trampoline)(void*, const httpp::request&, httpp::response&);
    static httpp::server::before_hook httpp_py_make_before(httpp_py_before_trampoline fn, void* ctx) {
        return [fn, ctx](const httpp::request& q, httpp::response& r) { return fn(ctx, q, r); };
    }
    // Body pulled piece by piece from Python. `ctx` is an owned reference: the shared_ptr (copied along
    // with the std::function) releases it once the last copy is gone, wherever that happens.
    // next(): 1 = piece in `chunk`, 0 = done, <0 = failed (-> throw -> httpp drops the connection).
    typedef int (*httpp_py_stream_fn)(void*, std::size_t, std::string&);
    typedef void (*httpp_py_release_fn)(void*);
    static void httpp_py_set_stream(httpp::response& res, httpp_py_stream_fn next, httpp_py_release_fn release, void* ctx) {
        std::shared_ptr<void> keep(ctx, release);
        res.stream = [next, keep](std::size_t offset, std::string& chunk) {
            const int r = next(keep.get(), offset, chunk);
            if (r < 0) throw std::runtime_error("python stream failed");
            return r > 0;
        };
    }
    static void httpp_py_reset(httpp::response& res) { res = httpp::response(); }
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
    ctypedef cbool (*before_trampoline_fn "httpp_py_before_trampoline")(void*, const cpp_request&, response&) noexcept nogil
    cpp_before_hook make_before "httpp_py_make_before"(before_trampoline_fn fn, void* ctx)
    ctypedef int (*stream_fn "httpp_py_stream_fn")(void*, size_t, string&) noexcept nogil
    ctypedef void (*release_fn "httpp_py_release_fn")(void*) noexcept nogil
    void reset_response "httpp_py_reset"(response& res)
    void set_stream "httpp_py_set_stream"(response& res, stream_fn next, release_fn release, void* ctx)


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
        cpp_curl_request& param(string name, string value) except +
        cpp_curl_request& json(string document) except +
        cpp_curl_request& form(vector[pair[string, string]] fields) except +
        cpp_curl_request& basic_auth(string user, string password) except +
        cpp_curl_request& bearer(string token) except +
        cpp_curl_request& cookie(string name, string value) except +
        cpp_curl_request& timeout(long seconds) except +
        cpp_curl_request& follow_redirects(cbool enable) except +
        cpp_curl_request& verify(cbool enable) except +
        cpp_curl_request& ca_file(string path) except +
        cpp_curl_request& proxy(string host, int port) except +
        cpp_curl_request& max_response_size(size_t bytes) except +
        response run() except + nogil


import json as _json
from urllib.parse import quote as _quote, urlencode as _urlencode


cdef bytes _to_bytes(object value):
    """str -> UTF-8, bytes-like -> bytes. (Request/response bodies are bytes on the wire.)"""
    if isinstance(value, str):
        return (<str>value).encode("utf-8")
    return bytes(value)


def _encode_query(object params):
    """{'a': 1, 'b': ['x', 'y']} or [('a', 1)] -> 'a=1&b=x&b=y', percent-encoded (spaces as %20)."""
    if hasattr(params, "items"):
        params = params.items()
    pairs = []
    for key, value in params:
        if isinstance(value, (list, tuple)):
            pairs.extend((key, item) for item in value)
        else:
            pairs.append((key, value))
    return _urlencode(pairs, quote_via=_quote)


def _with_params(str path, object params):
    if not params:
        return path
    query = _encode_query(params)
    return path + ("&" if "?" in path else "?") + query


cdef vector[pair[string, string]] _header_pairs(object headers):
    cdef vector[pair[string, string]] out
    if headers:
        items = headers.items() if hasattr(headers, "items") else headers
        for name, value in items:
            out.push_back(pair[string, string]((<str>name).encode("utf-8"), (<str>value).encode("utf-8")))
    return out


cdef class Response:
    """A response to an HTTP request made via Client / Request / the module-level get(), post(), ...

    status, ok, headers, header(name); the body as .content (bytes), .text (str) and .json().
    A request that produced no HTTP response at all (nobody listening, a timeout, a failed TLS
    handshake, ...) has status 0 and .error set to the reason; .raise_for_status() turns that
    (or a 4xx/5xx status) into an exception.
    """
    cdef int _status
    cdef bytes _body
    cdef list _headers  # list of (str, str) tuples, in server order
    cdef str _error
    cdef str _error_message

    def __init__(self, int status, bytes body, list headers=None, str error=None, str error_message=""):
        self._status = status
        self._body = body
        self._headers = headers or []
        self._error = error
        self._error_message = error_message

    @property
    def status(self):
        return self._status

    @property
    def content(self):
        """The body as bytes, exactly as received."""
        return self._body

    @property
    def text(self):
        """The body decoded with the charset from Content-Type (UTF-8 if there is none)."""
        cdef str ctype = self.header("content-type") or ""
        charset = "utf-8"
        for part in ctype.split(";")[1:]:
            name, _, value = part.strip().partition("=")
            if name.lower() == "charset" and value:
                charset = value.strip().strip('"')
        try:
            return self._body.decode(charset, errors="replace")
        except LookupError:
            return self._body.decode("utf-8", errors="replace")

    @property
    def body(self):
        """Same as .text (kept for compatibility); prefer .content for binary data."""
        return self.text

    def json(self):
        """The body parsed as JSON."""
        return _json.loads(self._body)

    @property
    def ok(self):
        return 200 <= self._status < 300

    @property
    def error(self):
        """None, or why no HTTP response arrived: 'connection', 'timeout', 'tls', 'too_many_redirects',
        'too_large', 'invalid_url', 'unsupported_method', 'canceled' or 'other'."""
        return self._error

    @property
    def error_message(self):
        return self._error_message

    @property
    def failed(self):
        """True if there is no HTTP response (see .error). A 404 is a response, not a failure."""
        return self._error is not None

    def raise_for_status(self):
        """Raise httpp.RequestError (or a subclass) if there was no response, httpp.HTTPError for a
        4xx/5xx status; return the response itself otherwise, so calls can be chained."""
        import importlib
        errors = importlib.import_module("httpp.errors")
        if self._error is not None:
            raise errors.request_error(self._error, self._error_message, self)
        if self._status >= 400:
            raise errors.HTTPError(f"HTTP {self._status}", self)
        return self

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

    def __repr__(self):
        if self._error is not None:
            return f"<Response failed: {self._error}: {self._error_message}>"
        return f"<Response [{self._status}]>"


cdef _make_response(response res):
    """Convert a C++ httpp::response into a Python Response, headers and all."""
    cdef list headers = [
        (h.first.decode("utf-8", errors="replace"), h.second.decode("utf-8", errors="replace"))
        for h in res.headers
    ]
    cdef str error = (<bytes>res.error_name()).decode("ascii")
    return Response(res.status, res.body, headers,
                    None if error == "none" else error,
                    res.error_message.decode("utf-8", errors="replace"))


cdef class Client:
    """A small HTTP client session. It keeps its connection alive between calls, so repeated requests
    to one server skip the TCP/TLS handshake. Calls on one Client take turns (it may be shared
    between threads); use several, or AsyncClient, to run requests in parallel.

        c = Client("http://localhost:8000/api", headers={"X-Token": "abc"}, timeout=10)
        c.get("/items", params={"page": 2}).json()
        c.post("/items", json={"name": "apple"}).raise_for_status()

    `Client(host, port, ssl=False)` also works. Network failures do not raise: look at
    response.error, or call response.raise_for_status().
    """
    cdef cpp_client* _cli

    def __init__(self, str host_or_url, port=None, cbool ssl=False, *, headers=None, timeout=None,
                 cbool follow_redirects=False, auth=None, token=None, cbool verify=True, str ca_file=None,
                 proxy=None):
        if port is None:
            self._cli = new cpp_client(host_or_url.encode("utf-8"))
        else:
            self._cli = new cpp_client(host_or_url.encode("utf-8"), <int>port, ssl)
        if headers:
            for name, value in (headers.items() if hasattr(headers, "items") else headers):
                self._cli.set_header((<str>name).encode("utf-8"), (<str>value).encode("utf-8"))
        if timeout is not None:
            self._cli.set_timeout(max(1, int(round(timeout))))
        if follow_redirects:
            self._cli.set_follow_redirects(True)
        if auth is not None:
            self._cli.set_basic_auth((<str>auth[0]).encode("utf-8"), (<str>auth[1]).encode("utf-8"))
        if token is not None:
            self._cli.set_bearer_token((<str>token).encode("utf-8"))
        if not verify:
            self._cli.set_verify(False)
        if ca_file is not None:
            self._cli.set_ca_file(ca_file.encode("utf-8"))
        if proxy is not None:
            self._cli.set_proxy((<str>proxy[0]).encode("utf-8"), <int>proxy[1])

    def __dealloc__(self):
        if self._cli is not NULL:
            del self._cli

    def request(self, str method, str path, *, params=None, headers=None, data=None, json=None, form=None,
                str content_type=None):
        """Send one request. `json` (any JSON-serializable value), `form` (a dict) and `data`
        (str or bytes) set the body; they exclude each other."""
        cdef string m = method.encode("utf-8")
        cdef string p = _with_params(path, params).encode("utf-8")
        cdef string body
        cdef string ctype
        cdef vector[pair[string, string]] hdrs = _header_pairs(headers)
        cdef response res
        if json is not None:
            body = _json.dumps(json).encode("utf-8")
            ctype = b"application/json"
        elif form is not None:
            body = _encode_query(form).encode("utf-8")
            ctype = b"application/x-www-form-urlencoded"
        elif data is not None:
            body = _to_bytes(data)
        if content_type is not None:
            ctype = content_type.encode("utf-8")
        with nogil:  # else a Python-route server in this process can't get the GIL: deadlock
            res = self._cli.send(m, p, hdrs, body, ctype)
        return _make_response(res)

    def get(self, str path, **kwargs):
        return self.request("GET", path, **kwargs)

    def head(self, str path, **kwargs):
        return self.request("HEAD", path, **kwargs)

    def options(self, str path, **kwargs):
        return self.request("OPTIONS", path, **kwargs)

    def post(self, str path, **kwargs):
        return self.request("POST", path, **kwargs)

    def put(self, str path, **kwargs):
        return self.request("PUT", path, **kwargs)

    def patch(self, str path, **kwargs):
        return self.request("PATCH", path, **kwargs)

    def delete(self, str path, **kwargs):
        return self.request("DELETE", path, **kwargs)

    def set_header(self, str name, str value):
        """Send this header with every request from now on."""
        self._cli.set_header(name.encode("utf-8"), value.encode("utf-8"))
        return self

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


cdef void _push_headers(response& res, object headers) except *:
    cdef string k, v
    for name, value in headers:
        k = (<str>name).encode("utf-8")
        v = (<str>value).encode("utf-8")
        res.headers.push_back(pair[string, string](k, v))


cdef int _stream_next(void* ctx, size_t offset, string& chunk) noexcept nogil:
    # The provider returns the next piece (bytes) or None at the end. Runs on whatever thread is
    # writing the response, which Python knows nothing about.
    with gil:
        try:
            piece = (<object>ctx)()
            if piece is None:
                return 0
            chunk = piece  # bytes only; anything else raises TypeError -> -1
        except BaseException:
            traceback.print_exc()
            return -1
        return 1


cdef void _stream_release(void* ctx) noexcept nogil:
    with gil:
        Py_DECREF(<object>ctx)


cdef void _fill_response(response& res, int status, object body, object headers) except *:
    # body: bytes (as is) | str (path of a file to stream, trusted) | callable (stream provider).
    if isinstance(body, bytes):
        res.body = <bytes>body
    elif isinstance(body, str):
        res.file = os.fsencode(body)
    elif callable(body):
        Py_INCREF(body)  # owned by the C++ side now, dropped by _stream_release
        set_stream(res, _stream_next, _stream_release, <void*>body)
    else:
        raise TypeError("response body must be bytes, a file path (str) or a stream provider")
    res.status = status
    _push_headers(res, headers)


cdef void _fail(response& res) noexcept:
    # Drops whatever was half-filled (body, file, stream, headers) and answers a plain 500.
    reset_response(res)
    res.status = 500
    res.body = b"Internal Server Error"


cdef void _run_handler(object fn, const cpp_request& req, response& res) except *:
    # fn(*request_args) -> (status, body, headers)
    status, body, headers = fn(*_request_args(req))
    _fill_response(res, status, body, headers)


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
            _fail(res)


cdef cbool _dispatch_before(void* ctx, const cpp_request& req, response& res) noexcept nogil:
    # hook(*request_args) -> None (go on to the route) | (status, body, headers) (answer it here)
    with gil:
        try:
            out = (<object>ctx)(*_request_args(req))
            if out is None:
                return False
            status, body, headers = out
            _fill_response(res, status, body, headers)
            return True
        except BaseException:
            traceback.print_exc()
            _fail(res)
            return True


cdef void _dispatch_after(void* ctx, const cpp_request& req, response& res) noexcept nogil:
    # hook(*request_args, status, headers) -> None | (status, headers). A failing hook makes it a 500.
    with gil:
        try:
            out = (<object>ctx)(*_request_args(req), res.status, _pairs(res.headers))
            if out is not None:
                status, headers = out
                res.status = status
                res.headers.clear()
                _push_headers(res, headers)
        except BaseException:
            traceback.print_exc()
            res.status = 500


cdef void _dispatch_error(void* ctx, const cpp_request& req, response& res) noexcept nogil:
    # hook(*request_args, status) -> None (keep the plain error) | (status, body, headers)
    with gil:
        try:
            out = (<object>ctx)(*_request_args(req), res.status)
            if out is not None:
                status, body, headers = out
                _fill_response(res, status, body, headers)
        except BaseException:
            traceback.print_exc()


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

    def __call__(self, int status, object body, list headers):
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


cdef int _check_seconds(int seconds) except -1:
    if seconds < 0:
        raise ValueError("a timeout can't be negative")
    return 0


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
        `handler(method, path, body, headers, query, path_params)` returns (status, body, headers):
        body is bytes, or a str = path of a file to stream (trusted, never from user input), or a
        callable returning the next bytes piece / None when done (chunked). headers: list of pairs.
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

    # --- Hooks. Each one replaces the previous (like the C++ setters); httpp/server.py chains several. ---
    def set_before_hook(self, hook):
        """hook(method, path, body, headers, query, path_params) -> None | (status, body, headers):
        a tuple answers the request without calling the route."""
        if not callable(hook):
            raise TypeError("hook must be callable")
        self._handlers.append(hook)
        self._srv.before(make_before(_dispatch_before, <void*>hook))
        return self

    def set_after_hook(self, hook):
        """hook(method, path, body, headers, query, path_params, status, resp_headers) -> None |
        (status, resp_headers). Status and headers only; the body is never passed."""
        if not callable(hook):
            raise TypeError("hook must be callable")
        self._handlers.append(hook)
        self._srv.after(make_handler(_dispatch_after, <void*>hook))
        return self

    def set_error_hook(self, hook):
        """hook(method, path, body, headers, query, path_params, status) -> None | (status, body,
        headers), for 4xx/5xx the server made itself (404, 405, 413, ...)."""
        if not callable(hook):
            raise TypeError("hook must be callable")
        self._handlers.append(hook)
        self._srv.set_error_handler(make_handler(_dispatch_error, <void*>hook))
        return self

    # --- Settings; call them before listening. Each returns self. ---
    def set_thread_pool(self, size_t base, size_t max_threads=0):
        """Worker threads for sync handlers: `base` are kept, up to `max_threads` used under load
        (0 = 4 x base)."""
        if base == 0:
            raise ValueError("the thread pool needs at least 1 thread")
        if 0 < max_threads < base:
            raise ValueError("max_threads can't be smaller than threads")
        self._srv.set_thread_pool(base, max_threads)
        return self

    def set_read_timeout(self, int seconds):
        _check_seconds(seconds)
        self._srv.set_read_timeout(seconds)
        return self

    def set_write_timeout(self, int seconds):
        _check_seconds(seconds)
        self._srv.set_write_timeout(seconds)
        return self

    def set_keep_alive_timeout(self, int seconds):
        _check_seconds(seconds)
        self._srv.set_keep_alive_timeout(seconds)
        return self

    def set_keep_alive_max(self, size_t requests):
        self._srv.set_keep_alive_max(requests)
        return self

    def set_max_body(self, size_t nbytes):
        """Largest request body accepted (413 above it); 0 = no limit."""
        self._srv.set_max_body(nbytes)
        return self

    def set_async_timeout(self, int seconds):
        """add_async_route: answer 504 if `done` isn't called within this; 0 = never."""
        _check_seconds(seconds)
        self._srv.set_async_timeout(seconds)
        return self

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
    """A small, curl-flavored fluent request builder, for one-off requests where a Client session
    is overkill. NOT a libcurl-compatible shim.

        Request(url).method("PUT").header("X-Token", "abc").json({"a": 1}).timeout(10).run()
        await Request(url).params({"q": "x"}).run_async()
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

    def param(self, str name, value):
        """Add one query parameter (percent-encoded)."""
        self._req.param(name.encode("utf-8"), str(value).encode("utf-8"))
        return self

    def params(self, mapping):
        """Add query parameters from a dict or a list of pairs; a list value repeats the name."""
        items = mapping.items() if hasattr(mapping, "items") else mapping
        for name, value in items:
            if isinstance(value, (list, tuple)):
                for item in value:
                    self.param(name, item)
            else:
                self.param(name, value)
        return self

    def data(self, body):
        """The request body, str or bytes."""
        self._req.data(_to_bytes(body))
        return self

    def json(self, value):
        """Send `value` as a JSON body (sets Content-Type)."""
        self._req.json(_json.dumps(value).encode("utf-8"))
        return self

    def form(self, mapping):
        """Send a urlencoded form body from a dict or a list of pairs."""
        cdef vector[pair[string, string]] fields
        for name, value in (mapping.items() if hasattr(mapping, "items") else mapping):
            fields.push_back(pair[string, string](str(name).encode("utf-8"), str(value).encode("utf-8")))
        self._req.form(fields)
        return self

    def content_type(self, str type_):
        self._req.content_type(type_.encode("utf-8"))
        return self

    def basic_auth(self, str user, str password):
        self._req.basic_auth(user.encode("utf-8"), password.encode("utf-8"))
        return self

    def bearer(self, str token):
        self._req.bearer(token.encode("utf-8"))
        return self

    def cookie(self, str name, str value):
        self._req.cookie(name.encode("utf-8"), value.encode("utf-8"))
        return self

    def timeout(self, seconds):
        """Give up after this many seconds (connect, read and write each)."""
        self._req.timeout(max(1, int(round(seconds))))
        return self

    def follow_redirects(self, cbool enable=True):
        self._req.follow_redirects(enable)
        return self

    def verify(self, cbool enable):
        """https: verify the server certificate (default True)."""
        self._req.verify(enable)
        return self

    def ca_file(self, str path):
        """https: trust this CA bundle instead of the system's."""
        self._req.ca_file(path.encode("utf-8"))
        return self

    def proxy(self, str host, int port):
        self._req.proxy(host.encode("utf-8"), port)
        return self

    def max_response_size(self, size_t nbytes):
        self._req.max_response_size(nbytes)
        return self

    def run(self):
        cdef response res
        with nogil:
            res = self._req.run()
        return _make_response(res)

    def run_async(self):
        """Run the request without blocking the event loop; `await` the result. The blocking call
        runs on the loop's default executor (a bounded thread pool), so this scales to tens of
        concurrent requests rather than thousands."""
        import asyncio
        return asyncio.get_running_loop().run_in_executor(None, self.run)
