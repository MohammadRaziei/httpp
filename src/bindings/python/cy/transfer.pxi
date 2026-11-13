# outgoing transfers: Download, SHA-256 helpers, Bar, URL, Request, and the asynchronous request core. Included by httpp_cy.pyx (a textual include: the order of the includes matters).

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

    def __init__(self, str url, dest_path=""):
        self._dl = new cpp_download(url.encode("utf-8"), _fs(dest_path))

    def __dealloc__(self):
        if self._dl is not NULL:
            del self._dl

    def output(self, dest_path):
        self._dl.output(_fs(dest_path))
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
    cdef string p = _fs(path)
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
    cdef readonly bint is_http
    cdef readonly str username
    cdef readonly str password
    cdef readonly int port
    cdef readonly str path
    cdef readonly str query

    def __init__(self, str raw):
        cdef cpp_url u = cpp_url.parse(raw.encode("utf-8"))
        self.valid = u.valid()
        self.is_http = u.is_http()
        self.scheme = u.scheme().decode("utf-8", "replace")
        self.host = u.host().decode("utf-8", "replace")
        self.username = u.username().decode("utf-8", "replace")
        self.password = u.password().decode("utf-8", "replace")
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

    def ca_file(self, path):
        """https: trust this CA bundle instead of the system's."""
        self._req.ca_file(_fs(path))
        return self

    def proxy(self, str proxy_url):
        """Send the request through an HTTP proxy: "http://[user[:pass]@]host[:port]". ValueError for anything else."""
        self._req.proxy(proxy_url.encode("utf-8"))
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
        cpp_async_client& set_proxy(string proxy_url) except +

cdef extern from *:
    """
    #include <functional>
    #include "httpp/client.hpp"
    typedef void (*httpp_py_done_fn)(void*, httpp::response*);
    static void httpp_py_run_request(const httpp::request& r, void* ctx, httpp_py_done_fn fn) {
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
    # Runs on the C++ network thread, which is not a Python thread. Without the pin, `with gil` creates a
    # thread state on every call and destroys it again; on free-threaded CPython that costs hundreds of
    # microseconds per request (3.14t AsyncClient fell from ~34k to ~2.4k req/s).
    httpp_py_pin_thread()
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
                 cbool follow_redirects=False, auth=None, token=None, cbool verify=True, ca_file=None,
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
