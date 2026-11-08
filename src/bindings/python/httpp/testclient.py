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
- `files=` sends multipart/form-data (a bare bytes or str value is a file named after its field). `websocket_connect(url)` opens a
  WebSocket to the app with a small RFC 6455 client of its own (ADR-0046); `receive_text()` and friends raise
  `httpp.WebSocketDisconnect` when the server closes.
- Not there: an in-memory mode without a socket (ADR-0042).
- Cookies are kept by name only (no Domain, Path or Secure matching): there is one host.
"""

import base64
import hashlib
import http
import json as _json
import mimetypes
import os
import socket
import struct
import threading
import time
import urllib.parse

from .errors import HTTPError, TooManyRedirects
from ._http import request as _request
from ._websocket import WebSocketDisconnect

__all__ = ["TestClient", "TestResponse", "WebSocketTestSession"]

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


def _quote_disposition(value):
    """A name or filename inside the quotes of a Content-Disposition: only \" needs an escape (what httpp's parser undoes);
    a line break would end the header, so it becomes a space."""
    return str(value).replace("\r", " ").replace("\n", " ").replace('"', '\\"')


def _file_part(name, value):
    """(filename, bytes, content type) of one `files=` value: bytes or str, a file object, or (filename, content[, content type])."""
    filename = content_type = None
    if isinstance(value, tuple):
        if not 2 <= len(value) <= 3:
            raise ValueError("a file is (filename, content) or (filename, content, content_type)")
        filename, content = value[0], value[1]
        content_type = value[2] if len(value) == 3 else None
    else:
        content = value
    if hasattr(content, "read"):
        if filename is None:
            filename = os.path.basename(getattr(content, "name", "") or "") or None
        content = content.read()
    if isinstance(content, str):
        content = content.encode("utf-8")
    if not isinstance(content, (bytes, bytearray, memoryview)):
        raise TypeError(f"file {name!r}: expected bytes, str or a file object, not {type(content).__name__}")
    filename = str(filename) if filename else str(name)
    return filename, bytes(content), content_type or mimetypes.guess_type(filename)[0] or "application/octet-stream"


def _multipart(data, files):
    """(body, content type) of a multipart/form-data request from the `data` dict and the `files` dict or list of pairs."""
    boundary = "httpp-testclient-" + os.urandom(12).hex()
    out = []
    for name, value in (data or {}).items():
        for item in value if isinstance(value, (list, tuple)) else [value]:
            if isinstance(item, bytes):
                item = item.decode("utf-8")
            out.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{_quote_disposition(name)}"\r\n\r\n{item}\r\n'.encode())
    for name, value in files.items() if hasattr(files, "items") else files:
        filename, content, content_type = _file_part(name, value)
        out.append((f'--{boundary}\r\nContent-Disposition: form-data; name="{_quote_disposition(name)}"; '
                    f'filename="{_quote_disposition(filename)}"\r\nContent-Type: {content_type}\r\n\r\n').encode() + content + b"\r\n")
    out.append(f"--{boundary}--\r\n".encode())
    return b"".join(out), f"multipart/form-data; boundary={boundary}"


_WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _xor_mask(data, key):
    n = len(data)
    if n == 0:
        return b""
    return (int.from_bytes(data, "big") ^ int.from_bytes((key * (n // 4 + 1))[:n], "big")).to_bytes(n, "big")


class WebSocketTestSession:
    """An open WebSocket from `TestClient.websocket_connect`; use it as a context manager.

        with client.websocket_connect("/ws") as ws:
            ws.send_text("hi")
            assert ws.receive_text() == "hi"

    `send_text`, `send_bytes`, `send_json(data, mode="text")`, `receive()` (a dict: `{"type": "websocket.receive", "text": ...}`
    or `"bytes"`, or `{"type": "websocket.close", "code": ..., "reason": ...}`), `receive_text`, `receive_bytes`,
    `receive_json(mode="text")` (these raise `httpp.WebSocketDisconnect` when the server closed), `close(code=1000, reason="")`,
    and `accepted_subprotocol`. A ping from the server is answered with a pong and not shown.
    """

    __test__ = False

    def __init__(self, sock, buffered, accepted_subprotocol):
        self._sock = sock
        self._buf = buffered
        self._closed = None        # (code, reason) once the server's close frame has been read
        self._sent_close = False
        self._dropped = False
        self.accepted_subprotocol = accepted_subprotocol

    # --- frames ---

    def _need(self, n):
        while len(self._buf) < n:
            try:
                chunk = self._sock.recv(65536)
            except socket.timeout:
                raise TimeoutError("no WebSocket message arrived in time") from None
            except OSError:
                chunk = b""
            if not chunk:
                raise EOFError
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def _send_frame(self, opcode, payload=b""):
        key = os.urandom(4)
        n = len(payload)
        if n < 126:
            head = bytes([0x80 | opcode, 0x80 | n])
        elif n < 65536:
            head = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack(">H", n)
        else:
            head = bytes([0x80 | opcode, 0x80 | 127]) + struct.pack(">Q", n)
        try:
            self._sock.sendall(head + key + _xor_mask(payload, key))
        except OSError:
            raise WebSocketDisconnect(*(self._closed or (1006, ""))) from None

    def _frame(self):
        b0, b1 = self._need(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._need(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._need(8))[0]
        key = self._need(4) if b1 & 0x80 else None  # a server never masks; tolerated
        payload = self._need(n)
        return b0 & 0x80 != 0, b0 & 0x0F, _xor_mask(payload, key) if key else payload

    def _message(self):
        """('text', str), ('bytes', bytes) or ('close', code, reason). Ping is answered, pong is skipped."""
        if self._closed is not None:
            return ("close", *self._closed)
        kind, parts = None, []
        while True:
            try:
                fin, opcode, payload = self._frame()
            except EOFError:
                self._closed = (1006, "")
                self._dropped = True
                return ("close", 1006, "")
            if opcode == 9:
                self._send_frame(10, payload)
            elif opcode == 10:
                continue
            elif opcode == 8:
                code = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else 1005
                reason = payload[2:].decode("utf-8", "replace")
                self._closed = (code, reason)
                if not self._sent_close:  # complete the closing handshake
                    self._sent_close = True
                    try:
                        self._send_frame(8, payload[:2])
                    except WebSocketDisconnect:
                        pass
                return ("close", code, reason)
            else:
                if opcode in (1, 2):
                    kind = "text" if opcode == 1 else "bytes"
                parts.append(payload)
                if fin:
                    data = b"".join(parts)
                    return ("text", data.decode("utf-8")) if kind == "text" else ("bytes", data)

    # --- the test's side ---

    def send_text(self, data):
        self._send_frame(1, data.encode("utf-8"))

    def send_bytes(self, data):
        self._send_frame(2, bytes(data))

    def send_json(self, data, mode="text"):
        if mode not in ("text", "binary"):
            raise ValueError("mode must be 'text' or 'binary'")
        text = _json.dumps(data, separators=(",", ":"))
        (self.send_text if mode == "text" else self.send_bytes)(text if mode == "text" else text.encode("utf-8"))

    def receive(self):
        message = self._message()
        if message[0] == "close":
            return {"type": "websocket.close", "code": message[1], "reason": message[2]}
        return {"type": "websocket.receive", message[0]: message[1]}

    def _receive_data(self, kind):
        message = self._message()
        if message[0] == "close":
            raise WebSocketDisconnect(message[1], message[2])
        if message[0] != kind:
            raise TypeError(f"expected a {kind} message, got {message[0]}")
        return message[1]

    def receive_text(self):
        return self._receive_data("text")

    def receive_bytes(self):
        return self._receive_data("bytes")

    def receive_json(self, mode="text"):
        if mode not in ("text", "binary"):
            raise ValueError("mode must be 'text' or 'binary'")
        data = self._receive_data("text" if mode == "text" else "bytes")
        return _json.loads(data)

    def close(self, code=1000, reason=""):
        """Send a close frame, wait a moment for the server's, and drop the connection. Harmless when called again."""
        if self._sock is None:
            return
        try:
            if not self._sent_close and not self._dropped:
                self._sent_close = True
                try:
                    self._send_frame(8, struct.pack(">H", code) + reason.encode("utf-8"))
                except WebSocketDisconnect:
                    pass
                else:
                    self._sock.settimeout(2.0)
                    while self._closed is None:
                        self._message()
        except (TimeoutError, OSError):
            pass
        finally:
            sock, self._sock = self._sock, None
            try:
                sock.close()
            except OSError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


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

    def request(self, method, url, *, params=None, headers=None, data=None, json=None, content=None, files=None, cookies=None,
                auth=None, follow_redirects=None, timeout=None):
        """One request. `url` is a path ("/items") or a URL; `data` is a dict (a form) or bytes/str; `json` any JSON value;
        `content` bytes/str as they are; `files` a dict or a list of (field, file) pairs, sent as multipart/form-data together
        with the fields of a `data` dict; `auth` is a (user, password) pair for Basic."""
        if sum(x is not None for x in (data, json, content)) > 1:
            raise ValueError("pass only one of data, json and content")
        has_files = files is not None and len(files) > 0
        if has_files and (json is not None or content is not None or (data is not None and not isinstance(data, dict))):
            raise ValueError("files go with a data dict only, not with json, content or a raw data body")
        if follow_redirects is None:
            follow_redirects = self.follow_redirects
        self._start()
        if self._failure:
            raise self._failure[0]
        body, body_type = None, None
        if has_files:
            body, body_type = _multipart(data, files)
        elif json is not None:
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

    def websocket_connect(self, url, subprotocols=None, *, headers=None, cookies=None, timeout=None):
        """Open a WebSocket to the app (a path or a URL) and return a `WebSocketTestSession`; use it in a `with`.

        The client's `headers` and cookies go with the handshake, like for a request. A handshake the server refuses
        (any answer but 101) raises `httpp.WebSocketDisconnect`; its `reason` says the HTTP status."""
        self._start()
        if self._failure:
            raise self._failure[0]
        timeout = self.timeout if timeout is None else timeout
        parts = urllib.parse.urlsplit(urllib.parse.urljoin(self.base_url, url))
        target = (parts.path or "/") + ("?" + parts.query if parts.query else "")
        sent = {k.lower(): (k, v) for k, v in self.headers.items()}
        for k, v in (headers.items() if hasattr(headers, "items") else headers or ()):
            sent[k.lower()] = (k, v)
        for k in ("upgrade", "connection", "sec-websocket-key", "sec-websocket-version"):
            sent.pop(k, None)  # the handshake owns these
        key = base64.b64encode(os.urandom(16)).decode()
        lines = [f"GET {target} HTTP/1.1", f"Host: {sent.pop('host', ('Host', parts.netloc))[1]}", "Upgrade: websocket", "Connection: Upgrade",
                 f"Sec-WebSocket-Key: {key}", "Sec-WebSocket-Version: 13"]
        if subprotocols:
            lines.append("Sec-WebSocket-Protocol: " + ", ".join(subprotocols))
        jar = {**self.cookies, **(cookies or {})}
        if jar and "cookie" not in sent:
            lines.append("Cookie: " + "; ".join(f"{k}={v}" for k, v in jar.items()))
        for k, v in sent.values():
            lines.append(f"{k}: {v}")
        for line in lines:
            if "\r" in line or "\n" in line:
                raise ValueError("a header contains a line break")
        sock = socket.create_connection(("127.0.0.1", self._port), timeout)
        try:
            sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1"))
            buf = b""
            while b"\r\n\r\n" not in buf:
                chunk = sock.recv(65536)
                if not chunk:
                    raise WebSocketDisconnect(1006, "the connection ended during the handshake")
                buf += chunk
            head, _, rest = buf.partition(b"\r\n\r\n")
            status_line, *header_lines = head.decode("latin-1").split("\r\n")
            status = int(status_line.split(" ", 2)[1])
            if status != 101:
                raise WebSocketDisconnect(1006, f"the handshake was refused with HTTP {status}")
            answer = {}
            for line in header_lines:
                name, _, value = line.partition(":")
                answer[name.strip().lower()] = value.strip()
            expected = base64.b64encode(hashlib.sha1(key.encode() + _WS_GUID).digest()).decode()
            if answer.get("sec-websocket-accept") != expected:
                raise WebSocketDisconnect(1002, "the server's Sec-WebSocket-Accept is wrong")
        except BaseException:
            sock.close()
            raise
        return WebSocketTestSession(sock, rest, answer.get("sec-websocket-protocol"))
