# URL, download(), Request. Included by httpp_cy.pyx (textual include: order matters, see the list there).

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
