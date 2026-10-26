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
    cdef cppclass cpp_upload_file "httpp::upload_file":
        string field
        string filename
        string content_type
        string data

    cdef cppclass cpp_form_data "httpp::form_data":
        vector[pair[string, string]] fields
        vector[cpp_upload_file] files

    cdef cppclass cpp_request "httpp::request":
        string method
        string path
        string body
        vector[pair[string, string]] headers
        vector[pair[string, string]] query
        vector[pair[string, string]] path_params
        cpp_form_data form() except +  # std::invalid_argument -> ValueError

    cdef cppclass cpp_handler "httpp::handler":
        pass

    cdef cppclass cpp_responder "httpp::responder":
        cpp_responder()
        cpp_responder(const cpp_responder&)
        void send(response res) except + nogil

    cdef cppclass cpp_async_handler "httpp::async_handler":
        pass

    cdef cppclass cpp_shared_handler "httpp::shared_handler":
        pass

    cdef cppclass cpp_io_handler "httpp::io_handler":
        pass

    cdef cppclass cpp_before_hook "httpp::server::before_hook":
        pass

    cdef cppclass cpp_server "httpp::server":
        cpp_server() except +
        void route(string method, string path, cpp_handler h) except +
        void route_async(string method, string path, cpp_async_handler h) except +
        void route_shared(string method, string path, cpp_shared_handler h) except +
        void route_io(string method, string path, cpp_io_handler h) except +
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
        cpp_server& set_header_timeout(int seconds) except +
        cpp_server& set_body_timeout(int seconds) except +
        cpp_server& set_compression(size_t min_size, int level) except +
        cpp_server& set_max_connections(size_t n) except +
        cpp_server& set_async_timeout(int seconds) except +
        cpp_server& set_reuse_port(cbool on) except +
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
    // The job to run after the answer was written (response::after_send). `ctx` is an owned reference, released
    // when the last copy of the std::function is gone, wherever that happens.
    typedef void (*httpp_py_run_fn)(void*);
    static void httpp_py_set_after_send(httpp::response& res, httpp_py_run_fn run, httpp_py_release_fn release, void* ctx) {
        std::shared_ptr<void> keep(ctx, release);
        res.after_send = [run, keep]() { run(keep.get()); };
    }
    static void httpp_py_reset(httpp::response& res) { res = httpp::response(); }
    // Pool threads are not Python threads. PyGILState_Ensure/Release would create a thread state on
    // every request and destroy it again (the counter drops to 0), which is very slow on free-threaded
    // CPython. One Ensure per thread that is never released keeps the state alive, so the later
    // `with gil` blocks only attach and detach it.
    // ponytail: the state of an exited pool thread is freed at interpreter shutdown, not earlier.
    static void httpp_py_pin_thread() {
        static thread_local bool pinned = false;
        if (pinned) return;
        pinned = true;
        PyGILState_Ensure();
        PyEval_SaveThread();
    }
    typedef void (*httpp_py_trampoline)(void*, const httpp::request&, httpp::response&);
    static httpp::handler httpp_py_make_handler(httpp_py_trampoline fn, void* ctx) {
        return [fn, ctx](const httpp::request& q, httpp::response& r) { fn(ctx, q, r); };
    }
    typedef void (*httpp_py_async_trampoline)(void*, const httpp::request&, const httpp::responder&);
    static httpp::async_handler httpp_py_make_async_handler(httpp_py_async_trampoline fn, void* ctx) {
        return [fn, ctx](const httpp::request& q, httpp::responder r) { fn(ctx, q, r); };
    }
    typedef void (*httpp_py_shared_trampoline)(void*, std::shared_ptr<const httpp::request>, httpp::response&);
    static httpp::shared_handler httpp_py_make_shared_handler(httpp_py_shared_trampoline fn, void* ctx) {
        return [fn, ctx](std::shared_ptr<const httpp::request> q, httpp::response& r) { fn(ctx, std::move(q), r); };
    }
    typedef void (*httpp_py_io_trampoline)(void*, std::shared_ptr<const httpp::request>, const httpp::responder&);
    static httpp::io_handler httpp_py_make_io_handler(httpp_py_io_trampoline fn, void* ctx) {
        return [fn, ctx](std::shared_ptr<const httpp::request> q, httpp::responder r) { fn(ctx, std::move(q), r); };
    }
    """
    void httpp_py_pin_thread() noexcept nogil
    ctypedef void (*trampoline_fn "httpp_py_trampoline")(void*, const cpp_request&, response&) noexcept nogil
    cpp_handler make_handler "httpp_py_make_handler"(trampoline_fn fn, void* ctx)
    ctypedef void (*async_trampoline_fn "httpp_py_async_trampoline")(void*, const cpp_request&, const cpp_responder&) noexcept nogil
    cpp_async_handler make_async_handler "httpp_py_make_async_handler"(async_trampoline_fn fn, void* ctx)
    ctypedef void (*shared_trampoline_fn "httpp_py_shared_trampoline")(void*, shared_ptr[const cpp_request], response&) noexcept nogil
    cpp_shared_handler make_shared_handler "httpp_py_make_shared_handler"(shared_trampoline_fn fn, void* ctx)
    ctypedef void (*io_trampoline_fn "httpp_py_io_trampoline")(void*, shared_ptr[const cpp_request], const cpp_responder&) noexcept nogil
    cpp_io_handler make_io_handler "httpp_py_make_io_handler"(io_trampoline_fn fn, void* ctx)
    ctypedef cbool (*before_trampoline_fn "httpp_py_before_trampoline")(void*, const cpp_request&, response&) noexcept nogil
    cpp_before_hook make_before "httpp_py_make_before"(before_trampoline_fn fn, void* ctx)
    ctypedef int (*stream_fn "httpp_py_stream_fn")(void*, size_t, string&) noexcept nogil
    ctypedef void (*release_fn "httpp_py_release_fn")(void*) noexcept nogil
    ctypedef void (*run_fn "httpp_py_run_fn")(void*) noexcept nogil
    void set_after_send "httpp_py_set_after_send"(response& res, run_fn run, release_fn release, void* ctx)


# The "gate" and the busy count. They let an `async def` handler that cannot suspend run on the I/O thread
# (no hop to the asyncio loop thread) without breaking two rules:
#  1. The I/O thread never waits for the GIL. So it only takes the inline path while no `def` handler, hook
#     or stream callback is running or waiting for the GIL on another C++ thread (`busy` is zero). Python
#     threads that httpp does not know about (user threads, executors) can still hold the GIL; that wait is
#     bounded by the interpreter's switch interval, and is the one case this does not cover.
#  2. Code that runs on an asyncio loop never runs at the same time as other code of that loop (asyncio code
#     between two awaits is atomic, and user code relies on it). The loop thread owns the gate while it runs
#     callbacks and gives it up only while it sleeps in its selector (see httpp/server.py `_GatedSelector`).
#     The I/O thread may use the loop only while it holds the gate. `loop_waits` keeps the I/O thread from
#     re-taking the gate for ever while the loop thread wants it back.
cdef extern from *:
    """
    #include <atomic>
    #include <mutex>
    struct httpp_py_gate {
        std::mutex m;
        std::atomic<bool> loop_waits{false};
        std::atomic<int> busy{0};
    };
    // Leaked on purpose: a late call during interpreter exit must still find it alive.
    static httpp_py_gate& httpp_py_gate_get() { static httpp_py_gate* g = new httpp_py_gate; return *g; }
    static void httpp_py_busy_enter() { ++httpp_py_gate_get().busy; }
    static void httpp_py_busy_exit() { --httpp_py_gate_get().busy; }
    static bool httpp_py_gate_try() {
        auto& g = httpp_py_gate_get();
        if (g.loop_waits.load() || g.busy.load() != 0) return false;
        return g.m.try_lock();
    }
    static void httpp_py_gate_unlock() { httpp_py_gate_get().m.unlock(); }
    static void httpp_py_gate_loop_enter() {
        auto& g = httpp_py_gate_get();
        g.loop_waits = true;
        g.m.lock();
        g.loop_waits = false;
    }
    static void httpp_py_gate_loop_leave() { httpp_py_gate_get().m.unlock(); }
    """
    void busy_enter "httpp_py_busy_enter"() noexcept nogil
    void busy_exit "httpp_py_busy_exit"() noexcept nogil
    bint gate_try "httpp_py_gate_try"() noexcept nogil
    void gate_unlock "httpp_py_gate_unlock"() noexcept nogil
    void gate_loop_enter "httpp_py_gate_loop_enter"() noexcept nogil
    void gate_loop_leave "httpp_py_gate_loop_leave"() noexcept nogil
    void reset_response "httpp_py_reset"(response& res)
    void set_stream "httpp_py_set_stream"(response& res, stream_fn next, release_fn release, void* ctx)


cdef extern from "httpp/download.hpp" namespace "httpp":
    cdef cppclass download_result:
        cbool ok
        cbool skipped
        int status
        string error
        string sha256

    cdef cppclass cpp_milliseconds "std::chrono::milliseconds":
        cpp_milliseconds(long long)

    cdef cppclass cpp_download "httpp::download":
        cpp_download(string url, string dest_path) except +
        cpp_download& output(string dest_path) except +
        cpp_download& enable_progress(cbool enable) except +
        cpp_download& disable_progress() except +
        cpp_download& follow_redirects(cbool enable) except +
        cpp_download& resume(cbool enable) except +
        cpp_download& force(cbool enable) except +
        cpp_download& sha256(string hex) except +
        cpp_download& sha256_url(string checksum_url) except +
        cpp_download& set_timeout(long seconds) except +
        cpp_download& set_max_wait(cpp_milliseconds max_wait) except +
        download_result run() except + nogil


cdef extern from "httpp/sha256.hpp" namespace "httpp":
    string cpp_sha256_hex "httpp::sha256_hex"(const void* data, size_t size) except + nogil
    string cpp_sha256_file "httpp::sha256_file"(string path) except + nogil
    string cpp_parse_sha256_checksum "httpp::parse_sha256_checksum"(string text, string file_name) except +


cdef extern from "httpp/progress.hpp" namespace "httpp::progress":
    cdef cppclass cpp_bar "httpp::progress::bar":
        cpp_bar(size_t total, string description) except +
        void update(size_t n) except +
        void set_progress(size_t current) except +
        void finish() except +
        size_t total()
        size_t current()
        cbool is_finished()


cdef extern from "httpp/url.hpp" namespace "httpp":
    cdef cppclass cpp_url "httpp::url":
        @staticmethod
        cpp_url parse(const string& raw) except +
        @staticmethod
        string encode_component(const string& text) except +
        @staticmethod
        string decode_component(const string& text) except +
        @staticmethod
        string build_query(const vector[pair[string, string]]& params) except +
        cbool valid()
        const string& scheme()
        const string& host()
        int port()
        const string& path()
        const string& query()


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


cdef bytes _to_bytes(object value):
    """str -> UTF-8, bytes-like -> bytes. (Request/response bodies are bytes on the wire.)"""
    if isinstance(value, str):
        return (<str>value).encode("utf-8")
    return bytes(value)


cdef bytes _query_part(object value):
    # what urllib.parse.urlencode did: str() of anything that is not already text or bytes
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, bytes):
        return value
    return str(value).encode("utf-8")


def _encode_query(object params):
    """{'a': 1, 'b': ['x', 'y']} or [('a', 1)] -> 'a=1&b=x&b=y', percent-encoded (spaces as %20).
    Done by the C++ httpp::url::build_query: same output as urllib.parse.urlencode(quote_via=quote), about 4x faster."""
    cdef vector[pair[string, string]] out
    if hasattr(params, "items"):
        params = params.items()
    for key, value in params:
        if isinstance(value, (list, tuple)):
            for item in value:
                out.push_back(pair[string, string](_query_part(key), _query_part(item)))
        else:
            out.push_back(pair[string, string](_query_part(key), _query_part(value)))
    return cpp_url.build_query(out).decode("ascii")


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


ctypedef fused _cpp_http_client:
    cpp_client
    cpp_async_client


cdef void _apply_client_options(_cpp_http_client* cli, headers, timeout, cbool follow_redirects, auth, token,
                                cbool verify, ca_file, proxy) except *:
    """The options httpp.Client and httpp.AsyncClient share (the C++ client and async_client have the same setters)."""
    if headers:
        for name, value in (headers.items() if hasattr(headers, "items") else headers):
            cli.set_header((<str>name).encode("utf-8"), (<str>value).encode("utf-8"))
    if timeout is not None:
        cli.set_timeout(max(1, int(round(timeout))))
    if follow_redirects:
        cli.set_follow_redirects(True)
    if auth is not None:
        cli.set_basic_auth((<str>auth[0]).encode("utf-8"), (<str>auth[1]).encode("utf-8"))
    if token is not None:
        cli.set_bearer_token((<str>token).encode("utf-8"))
    if not verify:
        cli.set_verify(False)
    if ca_file is not None:
        cli.set_ca_file((<str>ca_file).encode("utf-8"))
    if proxy is not None:
        cli.set_proxy((<str>proxy[0]).encode("utf-8"), <int>proxy[1])


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
                 proxy=None, cbool keep_alive=True):
        if port is None:
            self._cli = new cpp_client(host_or_url.encode("utf-8"))
        else:
            self._cli = new cpp_client(host_or_url.encode("utf-8"), <int>port, ssl)
        _apply_client_options(self._cli, headers, timeout, follow_redirects, auth, token, verify, ca_file, proxy)
        if not keep_alive:
            self._cli.set_keep_alive(False)

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
        """Parse `full_url` and GET it in one call - no manual URL parsing
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


cdef class RawRequest:
    """A request the way C++ holds it, read on demand. Nothing is turned into Python objects until it is
    asked for, so a handler that wants one path parameter pays for one string, not for the whole request.
    It shares the C++ request (no copy) and keeps it alive for as long as it exists."""
    cdef shared_ptr[const cpp_request] _req

    @property
    def method(self):
        return self._req.get().method.decode("utf-8", errors="replace")

    @property
    def path(self):
        return self._req.get().path.decode("utf-8", errors="replace")

    @property
    def body(self):
        cdef bytes b = self._req.get().body
        return b

    def headers(self):
        """[(name, value), ...] in wire order."""
        return _pairs(self._req.get().headers)

    def query(self):
        return _pairs(self._req.get().query)

    def path_params(self):
        """{name: value}; for a repeated name the first one wins."""
        cdef const cpp_request* r = self._req.get()
        cdef dict out = {}
        cdef size_t i
        for i in range(r.path_params.size()):
            name = r.path_params[i].first.decode("utf-8", errors="replace")
            if name not in out:
                out[name] = r.path_params[i].second.decode("utf-8", errors="replace")
        return out

    def form(self):
        """(fields, files) of a urlencoded or multipart body, parsed by C++ on the spot: fields is [(name, value), ...],
        files is [(field, filename, content_type, data_bytes), ...]. ValueError for a malformed multipart body."""
        return _form_tuple(deref(self._req.get()).form())

    def _args(self):
        """The old flat shape (method, path, body, headers, query, path_params-as-pairs)."""
        return _request_args(deref(self._req.get()))


cdef tuple _form_tuple(cpp_form_data form):
    cdef list fields = []
    cdef list files = []
    cdef size_t i
    cdef bytes data
    for i in range(form.fields.size()):
        fields.append((form.fields[i].first.decode("utf-8", errors="replace"),
                       form.fields[i].second.decode("utf-8", errors="replace")))
    for i in range(form.files.size()):
        data = form.files[i].data
        files.append((form.files[i].field.decode("utf-8", errors="replace"),
                      form.files[i].filename.decode("utf-8", errors="replace"),
                      form.files[i].content_type.decode("utf-8", errors="replace"), data))
    return fields, files


def parse_form(bytes body, str content_type):
    """The same parser for a body and Content-Type that are not a C++ request (a hook's request): (fields, files)."""
    cdef cpp_request req
    req.body = body
    req.headers.push_back(pair[string, string](b"Content-Type", content_type.encode("utf-8")))
    return _form_tuple(req.form())


cdef RawRequest _new_raw(shared_ptr[const cpp_request] req):
    cdef RawRequest raw = RawRequest.__new__(RawRequest)
    raw._req = req
    return raw


cdef void _push_headers(response& res, object headers) except *:
    cdef string k, v
    for name, value in headers:
        k = (<str>name).encode("utf-8")
        v = (<str>value).encode("utf-8")
        res.headers.push_back(pair[string, string](k, v))


cdef int _stream_next(void* ctx, size_t offset, string& chunk) noexcept nogil:
    # The provider returns the next piece (bytes) or None at the end. Runs on whatever thread is
    # writing the response, which Python knows nothing about.
    busy_enter()  # see the gate comment above
    try:
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
    finally:
        busy_exit()


cdef void _stream_release(void* ctx) noexcept nogil:
    # The provider's `finally` (an open file, a connection) must run now. On free-threaded CPython the
    # object itself is freed only when the thread that created it next runs Python (its refcounts
    # are merged then), and that thread may sit idle in the pool until the next request.
    busy_enter()  # see the gate comment above
    try:
        with gil:
            provider = <object>ctx
            close = getattr(provider, "close", None)
            if close is not None:
                try:
                    close()
                except BaseException:
                    traceback.print_exc()
            provider = None
            Py_DECREF(<object>ctx)
    finally:
        busy_exit()


cdef void _after_send_run(void* ctx) noexcept nogil:
    # Runs on a pool thread, after the answer was written.
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
        with gil:
            try:
                (<object>ctx)()
            except BaseException:
                traceback.print_exc()
    finally:
        busy_exit()


cdef void _after_send_release(void* ctx) noexcept nogil:
    busy_enter()  # see the gate comment above
    try:
        with gil:
            Py_DECREF(<object>ctx)
    finally:
        busy_exit()


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
    if type(headers) is not list:  # a list subclass (httpp.server._AfterHeaders) carries the after_send job
        after = getattr(headers, "after_send", None)
        if after is not None:
            Py_INCREF(after)  # owned by the C++ side now, dropped by _after_send_release
            set_after_send(res, _after_send_run, _after_send_release, <void*>after)


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
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
        with gil:
            try:
                _run_handler(<object>ctx, req, res)
            except BaseException:
                # The traceback goes to stderr for the developer; the client only
                # ever sees a generic 500 (no internals leaked).
                traceback.print_exc()
                _fail(res)
    finally:
        busy_exit()


cdef void _dispatch_raw(void* ctx, shared_ptr[const cpp_request] req, response& res) noexcept nogil:
    # Like _dispatch, but the handler gets a RawRequest (shared with C++, built on demand).
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
        with gil:
            try:
                status, body, headers = (<object>ctx)(_new_raw(req))
                _fill_response(res, status, body, headers)
            except BaseException:
                traceback.print_exc()
                _fail(res)
    finally:
        busy_exit()


cdef cbool _dispatch_before(void* ctx, const cpp_request& req, response& res) noexcept nogil:
    # hook(*request_args) -> None (go on to the route) | (status, body, headers) (answer it here)
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
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
    finally:
        busy_exit()


cdef void _dispatch_after(void* ctx, const cpp_request& req, response& res) noexcept nogil:
    # hook(*request_args, status, headers) -> None | (status, headers). A failing hook makes it a 500.
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
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
    finally:
        busy_exit()


cdef void _dispatch_error(void* ctx, const cpp_request& req, response& res) noexcept nogil:
    # hook(*request_args, status) -> None (keep the plain error) | (status, body, headers)
    busy_enter()  # see the gate comment above
    try:
        httpp_py_pin_thread()
        with gil:
            try:
                out = (<object>ctx)(*_request_args(req), res.status)
                if out is not None:
                    status, body, headers = out
                    _fill_response(res, status, body, headers)
            except BaseException:
                traceback.print_exc()
    finally:
        busy_exit()


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

    def __call__(self, int status, object body, object headers):  # a list, or a list subclass carrying after_send
        cdef response res
        _fill_response(res, status, body, headers)
        with nogil:
            self._r.send(res)


cdef void _start_dispatch(void* ctx, const cpp_request& req, const cpp_responder& r) noexcept:
    # GIL held. Hands the request to the route's `dispatch`, which must return quickly; the
    # response is sent later through `done`. Never raises: a failing dispatch answers 500.
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


cdef void _dispatch_async(void* ctx, const cpp_request& req, const cpp_responder& r) noexcept nogil:
    # Runs on a pool thread that Python knows nothing about, hence the explicit GIL acquire.
    busy_enter()  # see the gate comment above
    try:
        with gil:
            _start_dispatch(ctx, req, r)
    finally:
        busy_exit()


# The "loop inbox": requests for `async def` routes go from the C++ side to the asyncio loop thread
# WITHOUT the C++ thread ever touching the GIL. The C++ thread queues the request and writes one byte
# to a socket the loop is watching; the loop thread wakes up, takes the GIL once and drains the whole
# burst (see httpp/server.py `_get_loop`). A wake byte is only written when the queue goes from
# empty to non-empty, so a burst costs one wake-up, not one per request.
cdef extern from *:
    """
    #include <atomic>
    #include <cerrno>
    #include <deque>
    #include <mutex>
    #include <vector>
    #ifdef _WIN32
    #include <winsock2.h>
    #else
    #include <sys/socket.h>
    #endif
    struct httpp_py_item { void* ctx; std::shared_ptr<const httpp::request> req; httpp::responder rsp; };
    struct httpp_py_inbox {
        std::mutex mu;
        std::deque<httpp_py_item> q;
        bool armed = false;               // a wake byte is on its way (or the loop is draining)
        std::atomic<long long> wake_fd{-1};
    };
    // Leaked on purpose: a late push during interpreter exit must still find it alive.
    static httpp_py_inbox& httpp_py_box() { static httpp_py_inbox* b = new httpp_py_inbox; return *b; }
    static void httpp_py_inbox_attach(int fd) { httpp_py_box().wake_fd = fd; }
    static void httpp_py_inbox_push(void* ctx, std::shared_ptr<const httpp::request> q, const httpp::responder& r) {
        auto& b = httpp_py_box();
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(b.mu);
            b.q.push_back(httpp_py_item{ctx, std::move(q), r});
            if (!b.armed) b.armed = wake = true;
        }
        const long long fd = b.wake_fd.load();
        if (!wake || fd < 0) return; // not attached yet: the attaching side drains once itself
        const char one = 1;
        const long long n = ::send(static_cast<decltype(::socket(0, 0, 0))>(fd), &one, 1, 0);
        // A full socket buffer (EAGAIN) means wake bytes are already waiting; any other failure
        // must not leave the inbox "armed" forever.
        if (n < 0) {
    #ifdef _WIN32
            const bool full = WSAGetLastError() == WSAEWOULDBLOCK;
    #else
            const bool full = errno == EAGAIN || errno == EWOULDBLOCK;
    #endif
            if (!full) { std::lock_guard<std::mutex> lock(b.mu); b.armed = false; }
        }
    }
    // Moves up to `max` queued items into `out` under ONE lock. Returns 0 (and disarms, atomically with the
    // emptiness check) when nothing is left. A lock per item cost about 2.5 us per request under load.
    static std::size_t httpp_py_inbox_pop_many(std::vector<httpp_py_item>& out, std::size_t max) {
        auto& b = httpp_py_box();
        std::lock_guard<std::mutex> lock(b.mu);
        if (b.q.empty()) { b.armed = false; return 0; }
        std::size_t n = 0;
        while (n < max && !b.q.empty()) { out.push_back(std::move(b.q.front())); b.q.pop_front(); ++n; }
        return n;
    }
    """
    cdef cppclass inbox_item "httpp_py_item":
        void* ctx
        shared_ptr[const cpp_request] req
        cpp_responder rsp
        inbox_item()
    void inbox_attach "httpp_py_inbox_attach"(int fd) nogil
    void inbox_push "httpp_py_inbox_push"(void* ctx, shared_ptr[const cpp_request] req, const cpp_responder& r) nogil
    size_t inbox_pop_many "httpp_py_inbox_pop_many"(vector[inbox_item]& out, size_t maxn) nogil


cdef void _dispatch_inbox(void* ctx, shared_ptr[const cpp_request] req, const cpp_responder& r) noexcept nogil:
    # Runs on the I/O thread (route_io): queue the request for the asyncio loop. No GIL, no copy.
    inbox_push(ctx, req, r)


cdef void _start_raw(void* ctx, shared_ptr[const cpp_request] req, const cpp_responder& r, bint inline=False) noexcept:
    # GIL held, on the loop thread (or, for inline=True, on the I/O thread holding the gate). Hands the request
    # to the route's `dispatch(raw, done)` (a hybrid route's takes a third argument, `True` when inline),
    # which must return quickly; the response is sent later through `done`. Never raises: a failing
    # dispatch answers 500.
    done = _Done.__new__(_Done)
    (<_Done>done)._r = new cpp_responder(r)
    try:
        if inline:
            (<object>ctx)(_new_raw(req), done, True)
        else:
            (<object>ctx)(_new_raw(req), done)
    except BaseException:
        traceback.print_exc()
        try:
            done(500, b"Internal Server Error", [])
        except BaseException:
            pass


cdef void _dispatch_hybrid(void* ctx, shared_ptr[const cpp_request] req, const cpp_responder& r) noexcept nogil:
    # Runs on the I/O thread (route_io). The inline path only when the loop thread is asleep and nothing
    # else is running Python (see the gate comment); anything else is queued for the loop thread.
    if not gate_try():
        inbox_push(ctx, req, r)
        return
    httpp_py_pin_thread()
    with gil:
        _start_raw(ctx, req, r, True)
    gate_unlock()


def _gate_enter():
    """Loop thread: take the gate (waits, without the GIL, while an inline handler runs on the I/O thread)."""
    with nogil:
        gate_loop_enter()


def _gate_leave():
    """Loop thread: give the gate up, just before sleeping in the selector."""
    gate_loop_leave()


def _attach_inbox(int wake_fd):
    """Tell the inbox which socket to poke when requests arrive (the loop watches its other end)."""
    inbox_attach(wake_fd)


def _drain_inbox(int budget=256):
    """Loop thread, GIL held: dispatch up to `budget` queued requests. True if some may remain, so
    the caller should call again after letting other callbacks run (a huge burst must not starve them).
    The batch is taken WITH the GIL held on purpose: the mutex is only ever held for a moment and never
    by anyone waiting for the GIL, and letting go of the GIL per request would invite a thread switch."""
    cdef vector[inbox_item] batch
    cdef size_t left = <size_t>budget
    cdef size_t n, i
    while left > 0:
        n = inbox_pop_many(batch, left)  # 0 means empty, and the inbox is disarmed
        if n == 0:
            return False
        for i in range(n):
            _start_raw(batch[i].ctx, batch[i].req, batch[i].rsp)
        batch.clear()
        left -= n
    return True


cdef int _check_seconds(int seconds) except -1:
    if seconds < 0:
        raise ValueError("a timeout can't be negative")
    return 0


cdef class Server:
    """Minimal HTTP server. Replaces spinning up cpp-httplib/libcurl
    yourself - this is also what `httpp server` (the CLI, similar to
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

    def mount_swagger_ui(self, str docs_path, str openapi_url, str title):
        """Serve Swagger UI at `docs_path` from the files compiled into libhttpp, entirely in C++ (no GIL)."""
        mount_swagger_ui(self._srv[0], docs_path.encode("utf-8"), openapi_url.encode("utf-8"), title.encode("utf-8"))

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

    def add_loop_route(self, str method, str path, dispatch):
        """Like add_async_route, but `dispatch` runs on the asyncio loop thread that drains the inbox
        (see httpp/server.py), not on a pool thread, and the C++ side never takes the GIL for it. Needs
        _attach_inbox() and a loop calling _drain_inbox(); httpp.Server arranges both when it listens."""
        if not callable(dispatch):
            raise TypeError("dispatch must be callable")
        self._add_raw_loop_route(method, path, lambda raw, done: dispatch(*raw._args(), done))

    def _add_raw_route(self, str method, str path, fn):
        """What httpp.Server uses for `def` routes: `fn(raw)` -> (status, body, headers), where `raw` is a
        RawRequest (built on demand). Runs on a worker thread, with the GIL."""
        if not callable(fn):
            raise TypeError("handler must be callable")
        self._handlers.append(fn)
        self._srv.route_shared(method.encode("utf-8"), path.encode("utf-8"),
                               make_shared_handler(_dispatch_raw, <void*>fn))

    def _add_raw_loop_route(self, str method, str path, dispatch):
        """What httpp.Server uses for `async def` routes: `dispatch(raw, done)` runs on the asyncio loop
        thread, fed through the inbox straight from the I/O thread (one thread hop, no GIL on the C++
        side), and must return quickly; `done(status, body, headers)` sends the answer, from any thread."""
        if not callable(dispatch):
            raise TypeError("dispatch must be callable")
        self._handlers.append(dispatch)
        self._srv.route_io(method.encode("utf-8"), path.encode("utf-8"),
                           make_io_handler(_dispatch_inbox, <void*>dispatch))

    def _add_raw_hybrid_route(self, str method, str path, dispatch):
        """Like _add_raw_loop_route, for a handler that cannot suspend: `dispatch(raw, done, inline=False)`
        runs on the I/O thread itself (inline=True) when the loop thread sleeps and no `def` handler runs,
        and on the loop thread otherwise. Needs the loop to use httpp.server's _GatedSelector."""
        if not callable(dispatch):
            raise TypeError("dispatch must be callable")
        self._handlers.append(dispatch)
        self._srv.route_io(method.encode("utf-8"), path.encode("utf-8"),
                           make_io_handler(_dispatch_hybrid, <void*>dispatch))

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

    def set_header_timeout(self, int seconds):
        """Seconds the whole request head may take to arrive (slowloris guard); 0 = no limit."""
        self._srv.set_header_timeout(seconds)
        return self

    def set_body_timeout(self, int seconds):
        """Seconds the whole request body may take to arrive after its head (slow-drip guard); 0 = no limit."""
        self._srv.set_body_timeout(seconds)
        return self

    def set_compression(self, size_t min_size, int level=1):
        """Gzip text answers of at least min_size bytes for clients that accept it (level 1 fastest .. 9 smallest);
        min_size 0 turns it off. Files, streams and already encoded bodies are never touched."""
        self._srv.set_compression(min_size, level)
        return self

    def set_max_connections(self, size_t n):
        """Open connections allowed at once (one more gets a 503 and is closed); 0 = no limit."""
        self._srv.set_max_connections(n)
        return self

    def set_async_timeout(self, int seconds):
        """add_async_route: answer 504 if `done` isn't called within this; 0 = never."""
        _check_seconds(seconds)
        self._srv.set_async_timeout(seconds)
        return self

    def set_reuse_port(self, cbool on):
        """Several processes may listen on one port, the kernel balancing between them (Linux only;
        listen() then fails elsewhere). Used by `run(workers=N)`."""
        self._srv.set_reuse_port(on)
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
    """Result of Download.run(): ok, status (0 if no response), error (text, empty when ok),
    skipped (True when resume() found the complete file and made no transfer; ok is True too),
    sha256 (the digest the file was checked against; empty unless sha256()/sha256_url() was used)."""
    cdef cbool _ok
    cdef cbool _skipped
    cdef int _status
    cdef bytes _error
    cdef bytes _sha256

    def __init__(self, cbool ok, int status, bytes error, cbool skipped=False, bytes sha256=b""):
        self._ok = ok
        self._status = status
        self._error = error
        self._skipped = skipped
        self._sha256 = sha256

    @property
    def sha256(self):
        return self._sha256.decode("ascii")

    @property
    def skipped(self):
        return self._skipped

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
        Download(url, path).resume().run()      # wget -c: skip a complete file, continue a .part
        await Download(url, path).run_async()   # doesn't block the event loop

    follow_redirects (on by default; off = a 3xx is the final answer), resume (off by default;
    needs a server that answers HEAD and Range, else it quietly degrades to a fresh download),
    force (with resume: ignore existing files and start from byte 0).
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

    def follow_redirects(self, cbool enable=True):
        self._dl.follow_redirects(enable)
        return self

    def resume(self, cbool enable=True):
        self._dl.resume(enable)
        return self

    def force(self, cbool enable=True):
        self._dl.force(enable)
        return self

    def sha256(self, str hex):
        """Verify the finished file against this SHA-256 (64 hex digits, any case); on a mismatch the
        file is deleted and the result is not ok."""
        self._dl.sha256(hex.encode("utf-8"))
        return self

    def sha256_url(self, str checksum_url=""):
        """Read the expected SHA-256 from a checksum file (as `sha256sum` writes it) fetched before the
        download; empty means `<download url>.sha256`. Overrides sha256()."""
        self._dl.sha256_url(checksum_url.encode("utf-8"))
        return self

    def set_timeout(self, long seconds):
        """Seconds for connecting, reading and writing; 0 or less keeps the defaults."""
        self._dl.set_timeout(seconds)
        return self

    def set_max_wait(self, double seconds):
        """Seconds a lock that is not being updated is waited for before it is taken over (default 2);
        values <= 0 are ignored. (C++ takes milliseconds; Python uses seconds, like set_timeout.)"""
        if not seconds < 1e9:  # also catches nan and inf
            raise ValueError("max_wait is out of range")
        self._dl.set_max_wait(cpp_milliseconds(<long long>(seconds * 1000.0)))
        return self

    def run(self):
        cdef download_result res
        with nogil:
            res = self._dl.run()
        return DownloadResult(res.ok, res.status, res.error, res.skipped, res.sha256)

    def run_async(self):
        """Run the download without blocking the event loop; `await` the DownloadResult. Like
        Request.run_async, the blocking call runs on the loop's default thread-pool executor."""
        import asyncio
        return asyncio.get_running_loop().run_in_executor(None, self.run)


def sha256_hex(data):
    """SHA-256 of a bytes-like object as 64 lowercase hex characters (C++: httpp::sha256_hex).
    Text must be encoded first (a str raises TypeError)."""
    # ponytail: no typed memoryview here. Cython's memoryview needs Py_buffer, which the stable ABI
    # only has from Python 3.11, and the abi3 wheel targets 3.9 (pyproject.toml, py-api = "cp39").
    # bytes is read in place; bytearray / memoryview / array.array are copied once. Upgrade path:
    # raise py-api to cp311 and go back to `const unsigned char[::1] data not None` (no copy).
    cdef bytes raw
    if type(data) is bytes:
        raw = data
    else:
        raw = memoryview(data).tobytes()  # TypeError for str, int, list and other non-buffers
    cdef size_t n = len(raw)
    cdef const char* p = raw
    cdef string res
    with nogil:
        res = cpp_sha256_hex(p, n)
    return res.decode("ascii")


def sha256_file(path):
    """SHA-256 of a whole file, read in chunks. RuntimeError if it cannot be opened or read
    (C++: httpp::sha256_file)."""
    import os
    cdef string p = os.fspath(path).encode("utf-8")
    cdef string res
    with nogil:
        res = cpp_sha256_file(p)
    return res.decode("ascii")


def parse_sha256_checksum(str text, str file_name):
    """Digest (lowercase) from the text of a checksum file: one line `<64 hex>` optionally followed by
    `*`/spaces and a file name that must equal file_name. RuntimeError for anything else
    (C++: httpp::parse_sha256_checksum)."""
    return cpp_parse_sha256_checksum(text.encode("utf-8"), file_name.encode("utf-8")).decode("ascii")


cdef class Bar:
    """tqdm-like terminal progress bar with a known total, drawn on stderr (so piping a program's
    output stays clean). Also available as httpp.progress.bar:

        bar = httpp.progress.bar(100, "working")
        bar.update()            # advance by 1 (or update(n))
        bar.set_progress(50)    # absolute position, clamped to total
        bar.finish()
    """
    cdef cpp_bar* _bar

    def __init__(self, size_t total, str description=""):
        self._bar = new cpp_bar(total, description.encode("utf-8"))

    def __dealloc__(self):
        if self._bar is not NULL:
            del self._bar

    def update(self, size_t n=1):
        self._bar.update(n)

    def set_progress(self, size_t current):
        self._bar.set_progress(current)

    def finish(self):
        self._bar.finish()

    @property
    def total(self):
        return self._bar.total()

    @property
    def current(self):
        return self._bar.current()

    @property
    def finished(self):
        return self._bar.is_finished()


cdef bytes _query_text(object value):
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, bytes):
        return value
    if isinstance(value, (int, float)):
        return str(value).encode("utf-8")
    raise TypeError(f"a query name or value must be str, bytes, int or float, not {type(value).__name__}")


cdef class URL:
    """httpp's view of an http(s) URL (the same parser the client uses), plus the percent-encoding
    helpers:

        u = httpp.URL.parse("https://example.com:8443/a/b?x=1")
        u.valid, u.scheme, u.host, u.port, u.path, u.query
        httpp.URL.encode_component("a b/é")        # 'a%20b%2F%C3%A9'
        httpp.URL.decode_component("a%20b+c")      # 'a b+c'  ("+" stays "+")
        httpp.URL.build_query({"q": "x y", "n": 2})  # 'q=x%20y&n=2'

    Parsing never raises: check `valid`.
    """
    cdef readonly bint valid
    cdef readonly str scheme
    cdef readonly str host
    cdef readonly int port
    cdef readonly str path
    cdef readonly str query

    def __init__(self, str raw):
        cdef cpp_url u = cpp_url.parse(raw.encode("utf-8"))
        self.valid = u.valid()
        self.scheme = u.scheme().decode("utf-8", "replace")
        self.host = u.host().decode("utf-8", "replace")
        self.port = u.port()
        self.path = u.path().decode("utf-8", "replace")
        self.query = u.query().decode("utf-8", "replace")

    @staticmethod
    def parse(str raw):
        return URL(raw)

    @staticmethod
    def encode_component(str text):
        """Percent-encode one component: all but letters, digits and "-._~" becomes %XX."""
        return cpp_url.encode_component(text.encode("utf-8")).decode("ascii")

    @staticmethod
    def decode_component(str text):
        """%XX -> byte, read as UTF-8 (a byte that isn't valid UTF-8 becomes U+FFFD)."""
        return cpp_url.decode_component(text.encode("utf-8")).decode("utf-8", "replace")

    @staticmethod
    def build_query(params):
        """dict or iterable of (name, value) -> 'a=1&b=x%20y', each part encoded."""
        cdef vector[pair[string, string]] pairs
        for name, value in (params.items() if hasattr(params, "items") else params):
            pairs.push_back(pair[string, string](_query_text(name), _query_text(value)))
        return cpp_url.build_query(pairs).decode("ascii")

    def __repr__(self):
        if not self.valid:
            return "<URL invalid>"
        return f"<URL {self.scheme}://{self.host}:{self.port}{self.path}{'?' + self.query if self.query else ''}>"


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
        """Run the request without blocking the event loop; `await` the result. It runs on the C++
        network thread (non-blocking sockets, no thread per request), so thousands can be in flight."""
        import asyncio
        loop = asyncio.get_running_loop()
        fut = loop.create_future()
        holder = (loop, fut)
        Py_INCREF(holder)  # owned by the C++ callback until _done_trampoline releases it
        try:
            with nogil:
                httpp_py_run_request(self._req[0], <void*>holder, <done_fn>_done_trampoline)
        except BaseException:
            Py_DECREF(holder)
            raise
        return fut


# ---------------------------------------------------------------------------------------------
# Truly asynchronous requests. Request.run_async() and AsyncClient hand the request to the C++ network
# thread (Asio + llhttp, no thread per request); when the response is ready that thread takes the GIL
# just long enough to schedule the result on the asyncio loop that asked for it.
# ---------------------------------------------------------------------------------------------

cdef extern from "httpp/client.hpp" namespace "httpp":
    cdef cppclass cpp_async_client "httpp::async_client":
        cpp_async_client(string base_url, size_t max_connections) except +
        cpp_async_client& set_header(string name, string value) except +
        cpp_async_client& set_timeout(long seconds) except +
        cpp_async_client& set_follow_redirects(cbool enable) except +
        cpp_async_client& set_basic_auth(string user, string password) except +
        cpp_async_client& set_bearer_token(string token) except +
        cpp_async_client& set_verify(cbool enable) except +
        cpp_async_client& set_ca_file(string path) except +
        cpp_async_client& set_proxy(string host, int port) except +

cdef extern from *:
    """
    #include <functional>
    #include "httpp/client.hpp"
    typedef void (*httpp_py_done_fn)(void*, httpp::response*);
    static void httpp_py_run_request(const httpp::client::request& r, void* ctx, httpp_py_done_fn fn) {
        r.run_async([ctx, fn](httpp::response res) { fn(ctx, &res); });
    }
    static void httpp_py_client_send(httpp::async_client& c, const std::string& method, const std::string& path,
                                     const std::vector<std::pair<std::string, std::string>>& headers,
                                     const std::string& body, const std::string& ctype, void* ctx, httpp_py_done_fn fn) {
        c.send(method, path, headers, body, ctype, [ctx, fn](httpp::response res) { fn(ctx, &res); });
    }
    """
    ctypedef void (*done_fn "httpp_py_done_fn")(void*, response*) noexcept nogil
    void httpp_py_run_request(const cpp_curl_request&, void*, done_fn) except + nogil
    void httpp_py_client_send(cpp_async_client&, const string&, const string&, const vector[pair[string, string]]&,
                              const string&, const string&, void*, done_fn) except + nogil


def _resolve(fut, value):
    # runs on the event loop thread; the awaiting task may have been cancelled meanwhile
    if not fut.done():
        fut.set_result(value)


cdef void _done_trampoline(void* ctx, response* res) noexcept nogil:
    with gil:
        holder = <object>ctx
        Py_DECREF(holder)  # balances the Py_INCREF made when the request was started
        try:
            loop, fut = holder
            result = _make_response(res[0])
            loop.call_soon_threadsafe(_resolve, fut, result)
        except BaseException:
            pass  # e.g. the loop was closed: nobody is waiting any more


cdef class _AsyncClientCore:
    """The C++ httpp::async_client: a base URL, shared defaults and at most `max_connections`
    requests in flight, on the non-blocking network thread. Used by httpp.AsyncClient."""
    cdef cpp_async_client* _cli

    def __cinit__(self):
        self._cli = NULL

    def __init__(self, str base_url, size_t max_connections=8, *, headers=None, timeout=None,
                 cbool follow_redirects=False, auth=None, token=None, cbool verify=True, str ca_file=None,
                 proxy=None):
        self._cli = new cpp_async_client(base_url.encode("utf-8"), max_connections)
        _apply_client_options(self._cli, headers, timeout, follow_redirects, auth, token, verify, ca_file, proxy)

    def __dealloc__(self):
        if self._cli is not NULL:
            del self._cli

    def request(self, str method, str path, *, params=None, headers=None, data=None, json=None, form=None,
                str content_type=None):
        """Start one request and return an asyncio Future for its Response (await it)."""
        import asyncio
        cdef string m = method.encode("utf-8")
        cdef string p = _with_params(path, params).encode("utf-8")
        cdef string body
        cdef string ctype
        cdef vector[pair[string, string]] hdrs = _header_pairs(headers)
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
        loop = asyncio.get_running_loop()
        fut = loop.create_future()
        holder = (loop, fut)
        Py_INCREF(holder)  # owned by the C++ callback until _done_trampoline releases it
        try:
            with nogil:
                httpp_py_client_send(self._cli[0], m, p, hdrs, body, ctype, <void*>holder, <done_fn>_done_trampoline)
        except BaseException:
            Py_DECREF(holder)
            raise
        return fut


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
    const swagger_ui_asset_t* find_swagger_ui_asset(string_view name) noexcept
    void mount_swagger_ui(cpp_server& s, string docs_path, string openapi_url, string title) except +


def _swagger_ui_asset(str name):
    """(bytes, content type) of one Swagger UI file compiled into libhttpp (see include/httpp/swagger_ui.hpp), or None.
    The bytes are copied out of the library's read-only data each call: cache the result."""
    cdef bytes key = name.encode("utf-8")
    cdef const swagger_ui_asset_t* a = find_swagger_ui_asset(string_view(<const char*>key, len(key)))
    if a == NULL:
        return None
    return (a.data.data()[:a.data.size()],
            a.content_type.data()[:a.content_type.size()].decode("ascii"))


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
    cdef string p = os.fsencode(path) if path else b""
    cdef cbool ok
    with nogil:
        ok = cpp_set_log_file(p)
    if not ok:
        raise OSError("cannot open log file %r" % (path,))


cdef void _log_trampoline(void* ctx, int level, const string& where, const string& msg) noexcept nogil:
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
