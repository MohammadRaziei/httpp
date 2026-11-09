# Response and Client (blocking client), query helpers. Included by httpp_cy.pyx (a textual include: the order of the includes matters).

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
    to one server skip the TCP/TLS handshake. A Client may be shared between threads and its
    calls run side by side; AsyncClient runs many requests without a thread each.

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
