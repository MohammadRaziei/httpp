"""A client for testing a `httpp.Server` app, shaped like Starlette's `TestClient` (ADR-0042).

    from httpp.testclient import TestClient

    def test_items():
        with TestClient(app) as client:          # startup hooks run here, shutdown hooks at the end
            r = client.post("/items", json={"name": "apple"})
            assert r.status_code == 200 and r.json()["name"] == "apple"

How it works: the app is served by the real engine on a loopback port of its own (127.0.0.1, a free port chosen by the
system) and the requests go through httpp's own client. So routing, middleware, validation, compression and the exception
paths are the production ones; nothing is imitated. The port is an implementation detail: tests give paths ("/items"),
and the `Host` header is the one from `base_url` ("testserver" by default).

Differences from Starlette's client, on purpose or for now:
- The server starts with `with TestClient(app)` or on the first request, and stops at the end of the `with` or on `close()`.
  Startup and shutdown hooks run when the server starts and stops (Starlette without `with` never runs them).
- The scheme of `base_url` only shows in `response.url`; the connection is always plain HTTP, so `request.secure` is false.
- Not there yet: `websocket_connect` (httpp has no WebSocket client), multipart `files=`, and an in-memory mode without a socket.
- Cookies are kept by name only (no Domain, Path or Secure matching): there is one host.
"""

import http
import json as _json
import socket
import threading
import time
import urllib.parse

from .errors import HTTPError, TooManyRedirects
from ._http import request as _request

__all__ = ["TestClient", "TestResponse"]

_REDIRECTS = (301, 302, 303, 307, 308)
_MAX_REDIRECTS = 20


class _Headers:
    """The headers of a response: case-insensitive lookup, every value kept in order."""

    def __init__(self, pairs):
        self._pairs = [(str(k), str(v)) for k, v in pairs]

    def __getitem__(self, name):
        value = self.get(name)
        if value is None:
            raise KeyError(name)
        return value

    def get(self, name, default=None):
        """The first value of the header, or `default`."""
        low = name.lower()
        for k, v in self._pairs:
            if k.lower() == low:
                return v
        return default

    def get_list(self, name):
        """Every value of the header (Set-Cookie comes more than once)."""
        low = name.lower()
        return [v for k, v in self._pairs if k.lower() == low]

    def items(self):
        return list(self._pairs)

    def keys(self):
        return [k for k, _ in self._pairs]

    def values(self):
        return [v for _, v in self._pairs]

    def __contains__(self, name):
        return self.get(name) is not None

    def __iter__(self):
        return iter(self.keys())

    def __len__(self):
        return len(self._pairs)

    def __eq__(self, other):
        if isinstance(other, _Headers):
            other = other._pairs
        return isinstance(other, list) and self._pairs == other

    def __repr__(self):
        return f"Headers({self._pairs!r})"


class Cookies(dict):
    """name -> value. `set` is there for people who come from httpx."""

    def set(self, name, value):
        self[name] = value


def _read_set_cookie(value):
    """(name, value, delete) from one Set-Cookie header value."""
    first, *attributes = [part.strip() for part in value.split(";")]
    name, _, cookie = first.partition("=")
    delete = False
    for attribute in attributes:
        key, _, val = attribute.partition("=")
        key = key.lower()
        if key == "max-age":
            try:
                delete = delete or int(val) <= 0
            except ValueError:
                pass
        elif key == "expires":
            try:
                import email.utils

                when = email.utils.parsedate_to_datetime(val)
                delete = delete or when.timestamp() <= time.time()
            except (TypeError, ValueError):
                pass
    return name.strip(), cookie, delete


class TestResponse:
    """What a request returns: `status_code`, `headers`, `content`, `text`, `json()`, `cookies`, `url`, `history`."""

    __test__ = False

    def __init__(self, raw, url, history=()):
        self._raw = raw
        self.status_code = raw.status
        self.headers = _Headers(raw.headers)
        self.content = raw.content
        self.url = url
        self.history = list(history)
        self.cookies = Cookies()
        for value in self.headers.get_list("set-cookie"):
            name, cookie, delete = _read_set_cookie(value)
            if name and not delete:
                self.cookies[name] = cookie

    @property
    def text(self):
        return self._raw.text

    def json(self):
        return _json.loads(self.content)

    @property
    def reason_phrase(self):
        try:
            return http.HTTPStatus(self.status_code).phrase
        except ValueError:
            return ""

    @property
    def is_success(self):
        return 200 <= self.status_code < 300

    @property
    def is_redirect(self):
        return self.status_code in _REDIRECTS and "location" in self.headers

    @property
    def is_error(self):
        return self.status_code >= 400

    ok = is_success

    def raise_for_status(self):
        """Raise httpp.HTTPError for a 4xx or 5xx status; return the response itself otherwise."""
        if self.status_code >= 400:
            raise HTTPError(f"HTTP {self.status_code} {self.reason_phrase} for {self.url}", self)
        return self

    def __repr__(self):
        return f"<TestResponse [{self.status_code} {self.reason_phrase}]>"


class TestClient:
    """Serve `app` (a httpp.Server) on a loopback port of its own and talk to it.

        client = TestClient(app, base_url="http://testserver", raise_server_exceptions=True, follow_redirects=True,
                            cookies=None, headers=None, timeout=30)

    `raise_server_exceptions`: an exception that a handler raised and no exception handler answered is raised again in the
    test (the server still answers 500); with False the test only sees the 500. `cookies` and `headers` are sent with every
    request; cookies the server sets are kept in `client.cookies` and sent back.
    """

    __test__ = False

    def __init__(self, app, base_url="http://testserver", *, raise_server_exceptions=True, follow_redirects=True,
                 cookies=None, headers=None, timeout=30.0):
        from .server import Server

        if not isinstance(app, Server):
            raise TypeError(f"TestClient takes a httpp.Server, not {type(app).__name__}")
        self.app = app
        self.base_url = base_url if base_url.endswith("/") else base_url + "/"
        self.raise_server_exceptions = raise_server_exceptions
        self.follow_redirects = follow_redirects
        self.timeout = timeout
        self.cookies = Cookies(cookies or {})
        self.headers = dict(headers or {})
        self._lock = threading.RLock()
        self._thread = None
        self._port = None
        self._failure = []

    # --- the server ---

    def _start(self):
        with self._lock:
            if self._thread is not None:
                return
            self._failure = []
            port = self.app.bind_to_any_port("127.0.0.1")

            def serve():
                try:
                    self.app.listen_after_bind()
                except BaseException as e:  # the thread must not die silently: the next request reports it
                    self._failure.append(e)

            self.app._exception_sink = []
            thread = threading.Thread(target=serve, name="httpp-testclient", daemon=True)
            thread.start()
            self._thread, self._port = thread, port
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if self._failure:
                    self._stop()
                    raise self._failure[0]
                try:
                    socket.create_connection(("127.0.0.1", port), 0.2).close()  # listening and past the startup hooks
                    return
                except OSError:
                    time.sleep(0.005)
            self._stop()
            raise TimeoutError("the test server did not start listening within 10 seconds")

    def _stop(self):
        with self._lock:
            thread, self._thread, self._port = self._thread, None, None
            if thread is None:
                return
            self.app.stop()
            thread.join(30)
            self.app._exception_sink = None

    def close(self):
        """Stop the server (the shutdown hooks run). Harmless when it is not running; a later request starts it again."""
        self._stop()

    def __enter__(self):
        self._start()
        return self

    def __exit__(self, *exc):
        self.close()

    # --- requests ---

    def request(self, method, url, *, params=None, headers=None, data=None, json=None, content=None, cookies=None,
                auth=None, follow_redirects=None, timeout=None):
        """One request. `url` is a path ("/items") or a URL; `data` is a dict (a form) or bytes/str; `json` any JSON value;
        `content` bytes/str as they are; `auth` is a (user, password) pair for Basic."""
        if sum(x is not None for x in (data, json, content)) > 1:
            raise ValueError("pass only one of data, json and content")
        if follow_redirects is None:
            follow_redirects = self.follow_redirects
        self._start()
        if self._failure:
            raise self._failure[0]
        body, body_type = None, None
        if json is not None:
            body, body_type = _json.dumps(json, separators=(",", ":")), "application/json"
        elif isinstance(data, dict):
            body, body_type = urllib.parse.urlencode(data, doseq=True), "application/x-www-form-urlencoded"
        elif data is not None or content is not None:
            body = data if data is not None else content
        full = urllib.parse.urljoin(self.base_url, url)
        if params:
            parts = urllib.parse.urlsplit(full)
            query = "&".join(q for q in (parts.query, urllib.parse.urlencode(params, doseq=True)) if q)
            full = urllib.parse.urlunsplit(parts._replace(query=query))
        sent = {k.lower(): (k, v) for k, v in self.headers.items()}
        for k, v in (headers.items() if hasattr(headers, "items") else headers or ()):
            sent[k.lower()] = (k, v)
        if body_type is not None and "content-type" not in sent:
            sent["content-type"] = ("Content-Type", body_type)
        extra_cookies = dict(cookies or {})
        timeout = self.timeout if timeout is None else timeout

        history, hops = [], 0
        method = method.upper()
        while True:
            parts = urllib.parse.urlsplit(full)
            target = parts.path or "/"
            if parts.query:
                target += "?" + parts.query
            out_headers = dict(sent.values())
            if "host" not in sent:
                out_headers["Host"] = parts.netloc
            jar = {**self.cookies, **extra_cookies}
            with self._lock:
                self.app._exception_sink = []
            raw = _request(method, f"http://127.0.0.1:{self._port}{target}", headers=out_headers, data=body, cookies=jar or None,
                           auth=auth, timeout=timeout)
            if raw.failed:
                raw.raise_for_status()  # no HTTP response at all: raises the matching httpp error
            response = TestResponse(raw, full, history)
            if method == "HEAD":
                response.content = b""
            for value in response.headers.get_list("set-cookie"):
                name, cookie, delete = _read_set_cookie(value)
                if delete:
                    self.cookies.pop(name, None)
                elif name:
                    self.cookies[name] = cookie
            self._raise_server_exception()
            if not (follow_redirects and response.is_redirect):
                return response
            hops += 1
            if hops > _MAX_REDIRECTS:
                raise TooManyRedirects(f"more than {_MAX_REDIRECTS} redirects, last to {response.headers['location']}", raw)
            history.append(response)
            full = urllib.parse.urljoin(full, response.headers["location"])
            if response.status_code == 303 and method != "HEAD" or response.status_code in (301, 302) and method == "POST":
                method, body = "GET", None
                sent.pop("content-type", None)
                sent.pop("content-length", None)

    def _raise_server_exception(self):
        sink = self.app._exception_sink
        if not sink:
            return
        first = sink[0]
        sink.clear()
        if self.raise_server_exceptions:
            raise first

    def get(self, url, **kw):
        return self.request("GET", url, **kw)

    def head(self, url, **kw):
        return self.request("HEAD", url, **kw)

    def options(self, url, **kw):
        return self.request("OPTIONS", url, **kw)

    def delete(self, url, **kw):
        return self.request("DELETE", url, **kw)

    def post(self, url, **kw):
        return self.request("POST", url, **kw)

    def put(self, url, **kw):
        return self.request("PUT", url, **kw)

    def patch(self, url, **kw):
        return self.request("PATCH", url, **kw)
