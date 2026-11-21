# outgoing transfers: Download, SHA-256 helpers, Bar, URL, Request, and the asynchronous request core. Included by httpp_python.pyx (a textual include: the order of the includes matters).

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

    max_redirects (20 by default; 0 = a 3xx is the final answer, -1 = no limit), resume (off by default;
    needs a server that answers HEAD and Range, else it quietly degrades to a fresh download),
    force (with resume: ignore existing files and start from byte 0).
    """
    cdef cpp_download* _dl

    def __init__(self, str url, dest_path="", *, enable_progress=None, resume=None, force=None, sha256=None,
                 sha256_url=None, max_wait=None, timeout=None, max_redirects=None, connect_timeout=None,
                 max_time=None, basic_auth=None, bearer=None, verify=None, ca_file=None, proxy=None, headers=None,
                 retry=None, retry_delay=None, retry_connrefused=None):
        """Every keyword is the name of a setter and does what the setter does (basic_auth takes (user, password);
        headers is a dict, one header() call per item);
        Download(url, path, resume=True, timeout=30) is Download(url, path).resume().timeout(30)."""
        self._dl = new cpp_download(url.encode("utf-8"), _fs(dest_path))
        if enable_progress is not None:
            self.enable_progress(enable_progress)
        if resume is not None:
            self.resume(resume)
        if force is not None:
            self.force(force)
        if sha256 is not None:
            self.sha256(sha256)
        if sha256_url is not None:
            self.sha256_url(sha256_url)
        if max_wait is not None:
            self.max_wait(max_wait)
        if timeout is not None:
            self.timeout(timeout)
        if max_redirects is not None:
            self.max_redirects(max_redirects)
        if connect_timeout is not None:
            self.connect_timeout(connect_timeout)
        if max_time is not None:
            self.max_time(max_time)
        if basic_auth is not None:
            self.basic_auth(*basic_auth)
        if bearer is not None:
            self.bearer(bearer)
        if verify is not None:
            self.verify(verify)
        if ca_file is not None:
            self.ca_file(ca_file)
        if proxy is not None:
            self.proxy(proxy)
        if retry is not None:
            self.retry(retry)
        if retry_delay is not None:
            self.retry_delay(retry_delay)
        if retry_connrefused is not None:
            self.retry_connrefused(retry_connrefused)
        for name, value in (headers.items() if hasattr(headers, "items") else headers or ()):
            self.header(name, value)

    def __dealloc__(self):
        if self._dl is not NULL:
            del self._dl

    def retry(self, int times):
        """Send again up to this many more times after a try that may pass (a timeout, a refused-by-load status such as 503,
        a body cut off in the middle); with resume() a retry continues the .part file. 0 (default) never sends again."""
        if times < 0:
            raise ValueError("retry must not be negative")
        self._dl.retry(times)
        return self

    def retry_delay(self, seconds):
        """The wait in seconds before each new try; 0 (default) waits 1, 2, 4, ... seconds, 600 at most."""
        if seconds < 0:
            raise ValueError("retry_delay must not be negative")
        self._dl.retry_delay(int(round(seconds)))
        return self

    def retry_connrefused(self, cbool enable=True):
        """Also send again after a refused connection or a failed lookup (off by default)."""
        self._dl.retry_connrefused(enable)
        return self

    def header(self, str name, str value):
        """Send this header with every step (the size probe, the file, a checksum file on the same host and port);
        the same name again replaces it. ValueError for Range and Accept-Encoding: the download sets those."""
        self._dl.header(name.encode("utf-8"), value.encode("utf-8"))
        return self

    def output(self, dest_path):
        self._dl.output(_fs(dest_path))
        return self

    def enable_progress(self, cbool enable=True):
        self._dl.enable_progress(enable)
        return self

    def disable_progress(self):
        self._dl.disable_progress()
        return self

    def max_redirects(self, count=None):
        """How many redirects to follow (20 by default; 0 follows none and a 3xx is the final answer, -1 has
        no limit; ValueError for any other negative number). With no argument it returns the value."""
        if count is None:
            return self._dl.max_redirects()
        self._dl.max_redirects(<int>count)
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

    def timeout(self, seconds=None):
        """Seconds for connecting, reading and writing; 0 or less keeps the defaults. With no argument it
        returns the value."""
        if seconds is None:
            return self._dl.timeout()
        self._dl.timeout(<long>seconds)
        return self

    def connect_timeout(self, seconds):
        """Limit, in seconds, for resolving the host, connecting and the TLS handshake. timeout() still
        limits each write and read; without this one it covers the connect steps too. ValueError for a
        negative number."""
        if seconds < 0:
            raise ValueError("connect_timeout must not be negative")
        self._dl.connect_timeout(max(1, int(round(seconds))))
        return self

    def max_time(self, seconds):
        """Limit, in seconds, for the whole download: redirects and the body included. When it runs out the
        download ends with an error, even while the server is still sending. 0 (default) sets no such
        limit; timeout() is separate and limits each step. ValueError for a negative number."""
        if seconds < 0:
            raise ValueError("max_time must not be negative")
        self._dl.max_time(0 if seconds == 0 else max(1, int(round(seconds))))
        return self

    def basic_auth(self, str user, str password):
        """Send these credentials to the URL (and to a checksum file on the same host and port)."""
        self._dl.basic_auth(user.encode("utf-8"), password.encode("utf-8"))
        return self

    def bearer(self, str token):
        """Send "Authorization: Bearer <token>" to the URL (and to a checksum file on the same host and port)."""
        self._dl.bearer(token.encode("utf-8"))
        return self

    def proxy(self, str proxy_url):
        """Download through an HTTP proxy: "http://[user[:pass]@]host[:port]". ValueError for anything else."""
        self._dl.proxy(proxy_url.encode("utf-8"))
        return self

    def verify(self, cbool enable):
        """https: verify the server certificate (default True)."""
        self._dl.verify(enable)
        return self

    def ca_file(self, path):
        """https: trust this CA bundle instead of the system's."""
        self._dl.ca_file(_fs(path))
        return self

    def max_wait(self, seconds=None):
        """Seconds a lock that is not being updated is waited for before it is taken over (default 2);
        values <= 0 are ignored. (C++ takes milliseconds; Python uses seconds, like timeout.) With no
        argument it returns the value in seconds."""
        if seconds is None:
            return self._dl.max_wait().count() / 1000.0
        if not seconds < 1e9:  # also catches nan and inf
            raise ValueError("max_wait is out of range")
        self._dl.max_wait(cpp_milliseconds(<long long>(<double>seconds * 1000.0)))
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
        return self._bar.finished()


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
        cdef cpp_url u = cpp_parse_url(raw.encode("utf-8"))
        self.valid = cpp_url_valid(u)
        self.is_http = cpp_url_is_http(u)
        self.scheme = cpp_url_scheme(u).decode("utf-8", "replace")
        self.host = cpp_url_host(u).decode("utf-8", "replace")
        self.username = cpp_url_username(u).decode("utf-8", "replace")
        self.password = cpp_url_password(u).decode("utf-8", "replace")
        self.port = cpp_url_port(u)
        self.path = cpp_url_path(u).decode("utf-8", "replace")
        self.query = cpp_url_query(u).decode("utf-8", "replace")

    @staticmethod
    def parse(str raw):
        return URL(raw)

    @staticmethod
    def encode_component(str text):
        """Percent-encode one component: all but letters, digits and "-._~" becomes %XX."""
        return cpp_encode_component(text.encode("utf-8")).decode("ascii")

    @staticmethod
    def decode_component(str text):
        """%XX -> byte, read as UTF-8 (a byte that isn't valid UTF-8 becomes U+FFFD)."""
        return cpp_decode_component(text.encode("utf-8")).decode("utf-8", "replace")

    @staticmethod
    def build_query(params):
        """dict or iterable of (name, value) -> 'a=1&b=x%20y', each part encoded."""
        cdef vector[pair[string, string]] pairs
        for name, value in (params.items() if hasattr(params, "items") else params):
            pairs.push_back(pair[string, string](_query_text(name), _query_text(value)))
        return cpp_build_query(pairs).decode("ascii")

    @staticmethod
    def fetch_public_suffix_list(str source="", long timeout=30):
        """Download the Public Suffix List (default: the address liburlparser names, publicsuffix.org) with
        httpp's own client and load it, so domain/suffix splitting knows multi-part suffixes like co.uk.
        Raises RuntimeError on a failed request or a body that is not a list; the old list stays then."""
        cdef string src = source.encode("utf-8")
        cdef long t = timeout
        with nogil:
            cpp_fetch_public_suffix_list(src, t)

    def __repr__(self):
        if not self.valid:
            return "<URL invalid>"
        return f"<URL {self.scheme}://{self.host}:{self.port}{self.path}{'?' + self.query if self.query else ''}>"


def download(str url, str dest_path, cbool show_progress=True):
    """Download `url` to `dest_path`, with a tqdm-like terminal progress
    bar unless show_progress=False. Shorthand for
    Download(url, dest_path).enable_progress(show_progress).run()."""
    return Download(url, dest_path).enable_progress(show_progress).run()

cdef extern from *:
    """
    #include "httpp/client.hpp"
    static httpp::request* httpp_py_clone_request(const httpp::request& r) { return new httpp::request(r.clone()); }
    """
    cpp_curl_request* py_clone_request "httpp_py_clone_request"(const cpp_curl_request& r) except +


cdef extern from *:
    """
    #include <cstddef>
    #include <functional>
    #include <string_view>
    #include "httpp/client.hpp"
    typedef bool (*httpp_py_body_cb)(void*, const char*, std::size_t);
    typedef bool (*httpp_py_head_cb)(void*, const httpp::response&);
    typedef void (*httpp_py_retry_cb)(void*, const httpp::response&, int, long);
    static void httpp_py_on_body(httpp::request& r, void* ctx, httpp_py_body_cb fn) {
        r.on_body([ctx, fn](std::string_view piece) { return fn(ctx, piece.data(), piece.size()); });
    }
    static void httpp_py_on_head(httpp::request& r, void* ctx, httpp_py_head_cb fn) {
        r.on_head([ctx, fn](const httpp::response& head) { return fn(ctx, head); });
    }
    static void httpp_py_on_retry(httpp::request& r, void* ctx, httpp_py_retry_cb fn) {
        r.on_retry([ctx, fn](const httpp::response& last, int number, long delay) { fn(ctx, last, number, delay); });
    }
    """
    ctypedef bint (*body_cb_fn "httpp_py_body_cb")(void*, const char*, size_t) noexcept nogil
    ctypedef bint (*head_cb_fn "httpp_py_head_cb")(void*, const response&) noexcept nogil
    ctypedef void (*retry_cb_fn "httpp_py_retry_cb")(void*, const response&, int, long) noexcept nogil
    void py_on_body "httpp_py_on_body"(cpp_curl_request& r, void* ctx, body_cb_fn fn) except +
    void py_on_head "httpp_py_on_head"(cpp_curl_request& r, void* ctx, head_cb_fn fn) except +
    void py_on_retry "httpp_py_on_retry"(cpp_curl_request& r, void* ctx, retry_cb_fn fn) except +


cdef class _Callback:
    """A Python callable that a C++ request calls from a network thread (on_body, on_head, on_retry). An exception it raises
    is kept in `errors` (a list shared with the Request) and re-raised by run() / the awaited run_async(); the transfer is
    stopped (on_body, on_head) or goes on (on_retry)."""
    cdef object fn
    cdef list errors

    def __init__(self, fn, list errors):
        self.fn = fn
        self.errors = errors


cdef bint _body_trampoline(void* ctx, const char* data, size_t n) noexcept nogil:
    httpp_py_pin_thread()
    with gil:
        cb = <_Callback>ctx
        try:
            return cb.fn(data[:n]) is not False
        except BaseException as e:
            cb.errors.append(e)
            return False


cdef bint _head_trampoline(void* ctx, const response& head) noexcept nogil:
    httpp_py_pin_thread()
    with gil:
        cb = <_Callback>ctx
        try:
            return cb.fn(_make_response(head)) is not False
        except BaseException as e:
            cb.errors.append(e)
            return False


cdef void _retry_trampoline(void* ctx, const response& last, int number, long delay) noexcept nogil:
    httpp_py_pin_thread()
    with gil:
        cb = <_Callback>ctx
        try:
            cb.fn(_make_response(last), number, delay)
        except BaseException as e:
            cb.errors.append(e)


cdef class Request:
    """A small, curl-flavored fluent request builder, for one-off requests where a Client session
    is overkill. NOT a libcurl-compatible shim.

        Request(url).method("PUT").header("X-Token", "abc").json({"a": 1}).timeout(10).run()
        await Request(url).params({"q": "x"}).run_async()
    """
    cdef cpp_curl_request* _req
    cdef _Callback _on_body, _on_head, _on_retry
    cdef list _errors

    def __cinit__(self):
        self._errors = []

    def __init__(self, str url, *, method=None, params=None, headers=None, data=None, json=None, form=None,
                 content_type=None, cookies=None, timeout=None, max_redirects=None, auto_compression=None,
                 connect_timeout=None, max_time=None, basic_auth=None, bearer=None, verify=None, ca_file=None,
                 proxy=None, max_response_size=None, retry=None, retry_delay=None, retry_connrefused=None,
                 save_to=None, save_in=None, create_dirs=None, overwrite=None, include_head=None,
                 on_body=None, on_head=None, on_retry=None):
        """Every keyword is the name of a setter and does what the setter does, so Request(url, method="PUT",
        timeout=10) is Request(url).method("PUT").timeout(10). Three are plural because the setter is called once
        per item: headers, params, cookies (a dict each). basic_auth takes (user, password). json, form and data
        set the body and exclude each other."""
        self._req = new cpp_curl_request(url.encode("utf-8"))
        if method is not None:
            self.method(method)
        if params:
            self.params(params)
        for name, value in (headers.items() if hasattr(headers, "items") else headers or ()):
            self.header(name, value)
        if (json is not None) + (form is not None) + (data is not None) > 1:
            raise ValueError("json, form and data set the body: give only one of them")
        if json is not None:
            self.json(json)
        elif form is not None:
            self.form(form)
        elif data is not None:
            self.data(data)
        if content_type is not None:
            self.content_type(content_type)
        for name, value in (cookies.items() if hasattr(cookies, "items") else cookies or ()):
            self.cookie(name, value)
        if timeout is not None:
            self.timeout(timeout)
        if max_redirects is not None:
            self.max_redirects(max_redirects)
        if auto_compression is not None:
            self.auto_compression(auto_compression)
        if connect_timeout is not None:
            self.connect_timeout(connect_timeout)
        if max_time is not None:
            self.max_time(max_time)
        if basic_auth is not None:
            self.basic_auth(*basic_auth)
        if bearer is not None:
            self.bearer(bearer)
        if verify is not None:
            self.verify(verify)
        if ca_file is not None:
            self.ca_file(ca_file)
        if proxy is not None:
            self.proxy(proxy)
        if max_response_size is not None:
            self.max_response_size(max_response_size)
        if retry is not None:
            self.retry(retry)
        if retry_delay is not None:
            self.retry_delay(retry_delay)
        if retry_connrefused is not None:
            self.retry_connrefused(retry_connrefused)
        if save_to is not None:
            self.save_to(save_to)
        if save_in is not None:
            self.save_in(save_in)
        if create_dirs is not None:
            self.create_dirs(create_dirs)
        if overwrite is not None:
            self.overwrite(overwrite)
        if include_head is not None:
            self.include_head(include_head)
        if on_body is not None:
            self.on_body(on_body)
        if on_head is not None:
            self.on_head(on_head)
        if on_retry is not None:
            self.on_retry(on_retry)

    def __dealloc__(self):
        if self._req is not NULL:
            del self._req

    def clone(self):
        """An independent copy of this request as it is now (ADR-0093): a change to either afterwards does not
        reach the other, so a prepared request is a template."""
        cdef Request other = Request.__new__(Request)
        other._req = py_clone_request(self._req[0])
        other._errors = []
        # the copied C++ callbacks point at this object's _Callback holders: give the copy its own
        if self._on_body is not None:
            other.on_body(self._on_body.fn)
        if self._on_head is not None:
            other.on_head(self._on_head.fn)
        if self._on_retry is not None:
            other.on_retry(self._on_retry.fn)
        return other

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

    def max_redirects(self, int count):
        """How many redirects to follow: 0 (the default) follows none and a 3xx is the answer, n follows
        up to n hops (one more fails with error "too_many_redirects"), -1 follows without a limit.
        ValueError for any other negative number."""
        self._req.max_redirects(count)
        return self

    def auto_compression(self, cbool enable=True):
        """On (default): ask for gzip / deflate and return the decoded body. Off: send no Accept-Encoding
        (unless header() sets one) and return a coded body exactly as the server sent it."""
        self._req.auto_compression(enable)
        return self

    def connect_timeout(self, seconds):
        """Limit, in seconds, for resolving the host, connecting and the TLS handshake. timeout() still
        limits each write and read; without this one it covers the connect steps too."""
        if seconds < 0:
            raise ValueError("connect_timeout must not be negative")
        self._req.connect_timeout(max(1, int(round(seconds))))
        return self

    def max_time(self, seconds):
        """Limit, in seconds, for the whole request: redirects and a streamed body included. When it runs
        out the request ends with error "timeout", even while the server is still sending. 0 (default) sets
        no such limit; timeout() is separate and limits each step. ValueError for a negative number."""
        if seconds < 0:
            raise ValueError("max_time must not be negative")
        self._req.max_time(0 if seconds == 0 else max(1, int(round(seconds))))
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

    def retry(self, int times):
        """Send the request again up to this many more times after a try that may pass: a timeout, a status 408, 429, 500,
        502, 503 or 504 (and a failed connection with retry_connrefused()). Any other answer is final. 0 (default) never
        sends again. ValueError for a negative number."""
        if times < 0:
            raise ValueError("retry must not be negative")
        self._req.retry(times)
        return self

    def retry_delay(self, seconds):
        """The wait in seconds before each new try; 0 (default) waits 1, 2, 4, ... seconds, 600 at most."""
        if seconds < 0:
            raise ValueError("retry_delay must not be negative")
        self._req.retry_delay(int(round(seconds)))
        return self

    def retry_connrefused(self, cbool enable=True):
        """Also send again after a refused connection or a failed lookup (off by default)."""
        self._req.retry_connrefused(enable)
        return self

    def save_to(self, path):
        """Write the body of the final response to this file as it arrives, instead of into .content (like curl -o). The
        file is opened when the final head is accepted, so a redirect, a retried answer or a request that fails leaves nothing
        behind. A file that cannot be made or written ends the request with error "file"; Response.saved_as names it."""
        self._req.save_to(_fs(path))
        return self

    def save_in(self, folder):
        """Like save_to, in this folder, with the name the answer chooses (Content-Disposition, else the end of the URL)."""
        self._req.save_in(_fs(folder))
        return self

    def create_dirs(self, cbool enable=True):
        """With save_to / save_in: make the missing folders of the path (default off)."""
        self._req.create_dirs(enable)
        return self

    def overwrite(self, cbool enable=True):
        """With save_to / save_in: False makes a file that is already there an error (default True)."""
        self._req.overwrite(enable)
        return self

    def include_head(self, cbool enable=True):
        """With save_to / save_in: write the status line and headers before the body, like curl -i (default off)."""
        self._req.include_head(enable)
        return self

    def on_body(self, fn):
        """Stream the body of the final response: fn(piece: bytes) is called for each piece as it arrives, in order, and
        .content stays empty. Return False to stop (the response then has error "canceled"). It runs on a network thread
        (with the GIL): a blocking write is fine, a blocking httpp request inside it is not. An exception it raises stops
        the transfer and is raised by run() (or by the await of run_async())."""
        if not callable(fn):
            raise TypeError("on_body takes a callable")
        self._on_body = _Callback(fn, self._errors)
        py_on_body(self._req[0], <void*>self._on_body, <body_cb_fn>_body_trampoline)
        return self

    def on_head(self, fn):
        """With on_body: fn(response) is called once with the status and headers of the final response (empty body) before
        the first piece. Return False to skip the body (it is read and thrown away; run() still returns the response)."""
        if not callable(fn):
            raise TypeError("on_head takes a callable")
        self._on_head = _Callback(fn, self._errors)
        py_on_head(self._req[0], <void*>self._on_head, <head_cb_fn>_head_trampoline)
        return self

    def on_retry(self, fn):
        """With retry: fn(response, retry_number, delay_seconds) is called before each new try with the response of the try that
        failed, the number of the retry about to start (1, 2, ...) and the seconds it waits first."""
        if not callable(fn):
            raise TypeError("on_retry takes a callable")
        self._on_retry = _Callback(fn, self._errors)
        py_on_retry(self._req[0], <void*>self._on_retry, <retry_cb_fn>_retry_trampoline)
        return self

    def max_response_size(self, size_t nbytes):
        self._req.max_response_size(nbytes)
        return self

    def run(self):
        cdef response res
        with nogil:
            res = self._req.run()
        if self._errors:  # a callback raised: the transfer was stopped, and that is the error to report
            raise self._errors.pop(0)
        return _make_response(res)

    def run_async(self):
        """Run the request without blocking the event loop; `await` the result. It runs on the C++
        network thread (non-blocking sockets, no thread per request), so thousands can be in flight."""
        import asyncio
        loop = asyncio.get_running_loop()
        fut = loop.create_future()
        # the callbacks are kept alive by the holder until the request is done, even if this Request is dropped meanwhile
        holder = (loop, fut, self._errors, (self._on_body, self._on_head, self._on_retry))
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
        cpp_async_client& header(string name, string value) except +
        cpp_async_client& timeout(long seconds) except +
        cpp_async_client& max_redirects(int count) except +
        cpp_async_client& connect_timeout(long seconds) except +
        cpp_async_client& max_time(long seconds) except +
        cpp_async_client& retry(int times) except +
        cpp_async_client& retry_delay(long seconds) except +
        cpp_async_client& retry_connrefused(cbool enable) except +
        cpp_async_client& auto_compression(cbool enable) except +
        cpp_async_client& basic_auth(string user, string password) except +
        cpp_async_client& bearer(string token) except +
        cpp_async_client& verify(cbool enable) except +
        cpp_async_client& ca_file(string path) except +
        cpp_async_client& proxy(string proxy_url) except +

cdef extern from *:
    """
    #include <string>
    #include "httpp/client.hpp"
    static httpp::async_client* httpp_py_clone_async(const httpp::async_client& c) { return new httpp::async_client(c.clone()); }
    static httpp::request* httpp_py_async_make_request(const httpp::async_client& c, const std::string& path) {
        return new httpp::request(c.make_request(path));
    }
    """
    cpp_async_client* py_clone_async "httpp_py_clone_async"(const cpp_async_client& c) except +
    cpp_curl_request* py_async_make_request "httpp_py_async_make_request"(const cpp_async_client& c, const string& path) except +

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


def _fail_future(fut, exc):
    # runs on the event loop thread: a callback of the request raised
    if not fut.done():
        fut.set_exception(exc)


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
            loop, fut, errors = holder[0], holder[1], holder[2]
            if errors:
                loop.call_soon_threadsafe(_fail_future, fut, errors.pop(0))
            else:
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
                 max_redirects=None, connect_timeout=None, max_time=None,
                 cbool auto_compression=True, basic_auth=None, bearer=None, cbool verify=True, ca_file=None,
                 proxy=None, retry=None, retry_delay=None, retry_connrefused=None):
        self._cli = new cpp_async_client(base_url.encode("utf-8"), max_connections)
        _apply_client_options(self._cli, headers, timeout, max_redirects, connect_timeout, max_time,
                              auto_compression, basic_auth, bearer, verify, ca_file, proxy,
                              retry, retry_delay, retry_connrefused)

    def __dealloc__(self):
        if self._cli is not NULL:
            del self._cli

    def clone(self):
        cdef _AsyncClientCore other = _AsyncClientCore.__new__(_AsyncClientCore)
        other._cli = py_clone_async(self._cli[0])
        return other

    def make_request(self, str path, *, params=None):
        cdef Request req = Request.__new__(Request)
        req._req = py_async_make_request(self._cli[0], _with_params(path, params).encode("utf-8"))
        return req

    def send(self, str method, str path, *, params=None, headers=None, data=None, json=None, form=None,
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
        holder = (loop, fut, None)
        Py_INCREF(holder)  # owned by the C++ callback until _done_trampoline releases it
        try:
            with nogil:
                httpp_py_client_send(self._cli[0], m, p, hdrs, body, ctype, <void*>holder, <done_fn>_done_trampoline)
        except BaseException:
            Py_DECREF(holder)
            raise
        return fut
