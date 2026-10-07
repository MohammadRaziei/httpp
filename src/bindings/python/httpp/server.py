"""Decorator-style routes for httpp.Server, in the spirit of Flask/FastAPI.

    from httpp import Server

    app = Server()

    @app.get("/hello/{name}")
    def hello(name):
        return f"hello {name}"

    @app.post("/items")
    def create_item(request):
        return {"got": request.json()}, 201

    app.listen("127.0.0.1", 8000)

A handler may take `request` and/or any of the path parameters by name.
Annotate a path parameter as int/float and it is converted (a bad value
answers 422). Handlers may be `async def`: those run on one shared asyncio
event loop and release their worker thread while they await, so thousands can
be in flight at once. Plain `def` handlers run on the server's thread pool.
A handler may return:  str | bytes | dict | list | None | a generator / async
generator (streamed, chunked) | httpp.file(path),
optionally as (body, status) or (body, status, headers), headers being a dict
or a list of (name, value) pairs (repeat a name for e.g. several Set-Cookie).
Raise nothing special for errors: return ("not found", 404). An uncaught
exception becomes a plain 500 and the traceback goes to stderr, unless an
@app.exception_handler(...) takes it.

Hooks:  @app.before_request   def f(request): None | response (short-circuit)
        @app.after_request    def f(request, response): response.status / .headers
        @app.error_handler(404)       def f(request): ...  (errors the server itself makes)
        @app.exception_handler(Exc)   def f(request, exc): ...  (a route raised)
        @app.on_startup / @app.on_shutdown   run around listen().
Hooks run for every request (static files and unmatched paths too) and may be
`async def`, but they block their worker thread while they await.

Async handlers run on ONE shared event loop thread: a blocking call inside one
blocks every async route. `handler_timeout` (seconds, default 60, None = off)
cancels an async handler that takes too long and answers 504; sync handlers
can't be cancelled, so it doesn't apply to them.

Register routes and hooks BEFORE calling listen().
"""

import asyncio
import contextlib
import inspect
import json
import mimetypes
import os
import re
import threading
import traceback

from .httpp_cy import Server as _Server

_PARAM = re.compile(r"[{<]([^}>]*)[}>]")


class ServerRequest:
    """What a route handler receives as `request`.

    headers: dict, lower-cased names.  query: dict of ?a=1 params (cpp-httplib
    also merges urlencoded form bodies in here).  path_params: dict of str.
    For repeated names the first value wins.  # ponytail: multi-dict if needed
    """

    __slots__ = ("method", "path", "body", "headers", "query", "path_params")

    def __init__(self, method, path, body, headers, query, path_params):
        self.method = method
        self.path = path
        self.body = body  # bytes
        self.headers = _first_wins(headers, str.lower)
        self.query = _first_wins(query)
        self.path_params = dict(path_params)

    @property
    def text(self):
        return self.body.decode("utf-8", errors="replace")

    def json(self):
        return json.loads(self.body)

    @property
    def cookies(self):
        """The Cookie header as a dict (first value wins)."""
        out = {}
        for part in self.headers.get("cookie", "").split(";"):
            name, eq, value = part.strip().partition("=")
            if eq:
                out.setdefault(name, value)
        return out


_TOKEN = re.compile(r"[!#$%&'*+\-.^_`|~0-9A-Za-z]+")
_BAD_VALUE = re.compile(r"[\x00-\x08\x0a-\x1f\x7f]")  # CR, LF, NUL and other controls (tab is fine)


def _check_header(name, value):
    """Header injection guard: a name/value with CR or LF would split the response."""
    if not isinstance(name, str) or not isinstance(value, str):
        raise TypeError(f"header names and values must be str, got {name!r}: {value!r}")
    if not _TOKEN.fullmatch(name):
        raise ValueError(f"invalid header name {name!r}")
    if _BAD_VALUE.search(value):
        raise ValueError(f"invalid character in the value of header {name!r}")


class ResponseHead:
    """What an @after_request hook gets: the status and headers about to be sent.

    `headers` is a list of (name, value) pairs you may edit in place. The body isn't
    passed (it may be huge or streamed) and can't be changed here.
    """

    __slots__ = ("status", "headers")

    def __init__(self, status, headers):
        self.status = status
        self.headers = headers

    def header(self, name):
        """First value of `name` (case-insensitive), or None."""
        low = name.lower()
        return next((v for k, v in self.headers if k.lower() == low), None)

    def add_header(self, name, value):
        _check_header(name, value)
        self.headers.append((name, value))

    def set_header(self, name, value):
        """Replace every `name` header by this one."""
        _check_header(name, value)
        low = name.lower()
        self.headers[:] = [(k, v) for k, v in self.headers if k.lower() != low]
        self.headers.append((name, value))


class _File:
    __slots__ = ("path", "content_type")

    def __init__(self, path, content_type):
        self.path = path
        self.content_type = content_type


def file(path, content_type=None):
    """Answer with a file streamed from disk (known length, never loaded whole into memory).

    Content-Type: `content_type`, else guessed from the extension. A missing or unreadable
    file answers 404. `path` is trusted: NEVER build it from user input (path traversal);
    use Server.serve_directory for that. Can be returned inside (body, status, headers).
    """
    if content_type is not None:
        _check_header("Content-Type", content_type)
    return _File(os.fsdecode(os.fspath(path)), content_type)


def redirect(url, status=302):
    """A redirect response: `return httpp.redirect("/login")`."""
    if not isinstance(status, int) or not 300 <= status < 400:
        raise ValueError("a redirect status must be 3xx")
    _check_header("Location", url)
    return "", status, [("Location", url)]


_COOKIE_VALUE = re.compile(r"[\x21\x23-\x2b\x2d-\x3a\x3c-\x5b\x5d-\x7e]*")  # RFC 6265 cookie-octet
_COOKIE_ATTR = re.compile(r"[^;\x00-\x1f\x7f]*")


def set_cookie(name, value, *, path="/", domain=None, max_age=None, secure=False, http_only=True,
               same_site=None):
    """A Set-Cookie header pair, for the headers of a response (repeat it for several cookies):

        return "ok", 200, [httpp.set_cookie("sid", token), httpp.set_cookie("theme", "dark")]

    `value` must be cookie-safe (no space, quote, comma, semicolon or backslash): percent-encode
    anything else yourself. Defaults match the C++ side: Path=/; HttpOnly.
    """
    if not isinstance(name, str) or not _TOKEN.fullmatch(name):
        raise ValueError(f"invalid cookie name {name!r}")
    if not isinstance(value, str) or not _COOKIE_VALUE.fullmatch(value):
        raise ValueError("invalid cookie value (percent-encode spaces, quotes, commas, semicolons)")
    parts = [f"{name}={value}"]
    for label, attr in (("Path", path), ("Domain", domain)):
        if attr is not None:
            if not _COOKIE_ATTR.fullmatch(attr):
                raise ValueError(f"invalid cookie {label.lower()} {attr!r}")
            parts.append(f"{label}={attr}")
    if max_age is not None:
        parts.append(f"Max-Age={int(max_age)}")
    if secure:
        parts.append("Secure")
    if http_only:
        parts.append("HttpOnly")
    if same_site is not None:
        site = str(same_site).capitalize()
        if site not in ("Lax", "Strict", "None"):
            raise ValueError("same_site must be Lax, Strict or None")
        parts.append(f"SameSite={site}")
    return "Set-Cookie", "; ".join(parts)


def _first_wins(pairs, key=lambda k: k):
    out = {}
    for k, v in pairs:
        out.setdefault(key(k), v)
    return out


def _to_pattern(path):
    """'/users/{id}' or '/users/<id>' -> cpp-httplib's '/users/:id'."""

    def sub(m):
        if not m.group(1).isidentifier():
            # Flask's <int:id> converters etc. would otherwise silently never match.
            raise ValueError(f"unsupported path parameter {m.group(0)!r} in {path!r}")
        return ":" + m.group(1)

    return _PARAM.sub(sub, path)


_END = object()


def _stream(gen):
    """Generator / async generator -> provider(): the next non-empty bytes piece, None at the end.

    Called from whatever thread writes the response. An async generator is pulled on the shared
    event loop and waited for here, so the loop never blocks on a slow client.
    ponytail: a generator that raises mid-body drops the connection (the client sees a truncated
    body, the traceback goes to stderr); a client that hangs up leaves the generator to be
    garbage-collected, not closed on the spot.
    """
    if inspect.isasyncgen(gen):
        loop = _get_loop()

        async def anext_():
            try:
                return await gen.__anext__()
            except StopAsyncIteration:
                return _END

        def pull():
            return asyncio.run_coroutine_threadsafe(anext_(), loop).result()
    else:

        def pull():
            return next(gen, _END)

    def provider():
        while True:
            piece = pull()
            if piece is _END:
                return None
            if isinstance(piece, str):
                piece = piece.encode("utf-8")
            elif not isinstance(piece, (bytes, bytearray, memoryview)):
                raise TypeError(f"a streamed body yields str or bytes, not {type(piece).__name__}")
            if piece:  # an empty piece would only make the server spin
                return bytes(piece)

    return provider


def _to_response(rv, default_status=200):
    status, headers = default_status, []
    if isinstance(rv, tuple):
        rv, *extra = rv
        if len(extra) > 2:
            raise TypeError("a route may return body, (body, status) or (body, status, headers)")
        if extra:
            status = int(extra[0])
        if len(extra) == 2 and extra[1] is not None:
            headers = list(extra[1].items() if hasattr(extra[1], "items") else extra[1])

    if rv is None:
        body, ctype = b"", "text/plain; charset=utf-8"
    elif isinstance(rv, (bytes, bytearray)):
        body, ctype = bytes(rv), "application/octet-stream"
    elif isinstance(rv, str):
        body, ctype = rv.encode("utf-8"), "text/plain; charset=utf-8"
    elif isinstance(rv, (dict, list)):
        body, ctype = json.dumps(rv).encode("utf-8"), "application/json"
    elif isinstance(rv, _File):
        body = rv.path  # a str body = a file to stream (see Server.add_route)
        ctype = rv.content_type or mimetypes.guess_type(rv.path)[0] or "application/octet-stream"
    elif inspect.isgenerator(rv) or inspect.isasyncgen(rv):
        body, ctype = _stream(rv), "text/plain; charset=utf-8"
    else:
        raise TypeError(f"a route can't return {type(rv).__name__}")

    headers = [(k, v) for k, v in headers]
    for k, v in headers:
        _check_header(k, v)
    if not any(k.lower() == "content-type" for k, _ in headers):
        headers.append(("Content-Type", ctype))
    return status, body, headers


def _prepare(fn):
    """Build the kwargs for `fn` from a raw request: path params (converted per
    annotation) plus `request` if asked for. Returns (kwargs, early_response);
    early_response is a ready (status, body, headers) when the request can't
    reach the handler (bad path parameter)."""
    params = inspect.signature(fn).parameters
    takes_all = any(p.kind is p.VAR_KEYWORD for p in params.values())
    convert = {n: p.annotation for n, p in params.items() if p.annotation in (int, float)}

    def prepare(method, path, body, headers, query, path_params):
        req = ServerRequest(method, path, body, headers, query, path_params)
        kwargs = {k: v for k, v in req.path_params.items() if takes_all or k in params}
        try:
            for k, typ in convert.items():
                if k in kwargs:
                    kwargs[k] = typ(kwargs[k])
        except ValueError:
            return None, _to_response(({"error": f"path parameter {k!r} must be {typ.__name__}"}, 422))
        if "request" in params:
            kwargs["request"] = req
        return kwargs, None

    return prepare


def _adapt(fn, app):
    """Sync handler -> the flat blocking signature the compiled core calls."""
    prepare = _prepare(fn)

    def call(*raw):
        kwargs, early = prepare(*raw)
        if early is not None:
            return early
        try:
            return _to_response(fn(**kwargs))
        except Exception as exc:
            handler = app._exception_handler_for(exc)
            if handler is None:
                raise  # the compiled core logs it and answers a plain 500
            return _to_response(_call(handler, ServerRequest(*raw), exc), 500)

    return call


def _call(fn, *args, **kwargs):
    """Call a sync-context hook; if it was `async def`, wait for it on the shared loop.
    Never use this ON the loop thread (it would wait for itself)."""
    rv = fn(*args, **kwargs)
    if inspect.isawaitable(rv):

        async def wait():
            return await rv

        rv = asyncio.run_coroutine_threadsafe(wait(), _get_loop()).result()
    return rv


_loop = None
_loop_lock = threading.Lock()


def _get_loop():
    """The one event loop all async handlers run on (started on first use)."""
    global _loop
    with _loop_lock:
        if _loop is None:
            loop = asyncio.new_event_loop()

            def run():
                asyncio.set_event_loop(loop)
                loop.run_forever()

            threading.Thread(target=run, name="httpp-asyncio", daemon=True).start()
            _loop = loop
        return _loop


def _adapt_async(fn, app):
    """Async handler -> a dispatcher for the compiled core's non-blocking
    routes: it returns at once and the response is sent through `done` later."""
    prepare = _prepare(fn)

    async def call(*raw):
        kwargs, early = prepare(*raw)
        if early is not None:
            return early
        try:
            return _to_response(await fn(**kwargs))
        except Exception as exc:
            handler = app._exception_handler_for(exc)
            if handler is None:
                raise  # run() logs it and answers a plain 500
            rv = handler(ServerRequest(*raw), exc)
            if inspect.isawaitable(rv):
                rv = await rv
            return _to_response(rv, 500)

    def dispatch(method, path, body, headers, query, path_params, done):
        loop = _get_loop()
        raw = (method, path, body, headers, query, path_params)

        async def run():
            result = None
            try:
                task = asyncio.ensure_future(call(*raw))
                timeout = app._handler_timeout
                if timeout:
                    finished, _ = await asyncio.wait({task}, timeout=timeout)
                    if not finished:
                        task.cancel()  # frees the coroutine; its answer would be too late anyway
                        result = (504, b"Gateway Timeout", [])
                if result is None:
                    result = await task
            except BaseException:
                traceback.print_exc()
                result = (500, b"Internal Server Error", [])
            # Writing can block on a slow client; keep that off the event loop.
            await loop.run_in_executor(None, done, *result)

        asyncio.run_coroutine_threadsafe(run(), loop)

    return dispatch


class Server(_Server):
    """httpp.Server plus @route/@get/@post/... decorators, hooks and settings.

    threads / max_threads: worker threads for sync handlers (default: about one per core, at
    least 8; under load up to max_threads, default 4 x threads).  read_timeout / write_timeout /
    keep_alive_timeout: seconds (defaults 5).  keep_alive_max: requests per connection (100).
    max_body: largest request body in bytes, refused above it with a 413 (a client still
    sending a huge body may see the connection closed instead); 0 = no limit.  handler_timeout:
    seconds an async handler may take before it is cancelled and answered with a 504 (default 60,
    None = never).  Everything left as None keeps the C++ default.
    """

    def __init__(self, *, threads=None, max_threads=None, read_timeout=None, write_timeout=None,
                 keep_alive_timeout=None, keep_alive_max=None, max_body=None, handler_timeout=60):
        _Server.__init__(self)
        self._before, self._after, self._startup, self._shutdown = [], [], [], []
        self._error_handlers, self._exception_handlers = {}, {}

        if threads is None:
            if max_threads is not None:
                raise ValueError("max_threads needs threads")
        else:
            self.set_thread_pool(threads, max_threads or 0)
        for value, setter in ((read_timeout, self.set_read_timeout),
                              (write_timeout, self.set_write_timeout),
                              (keep_alive_timeout, self.set_keep_alive_timeout),
                              (keep_alive_max, self.set_keep_alive_max),
                              (max_body, self.set_max_body)):
            if value is not None:
                setter(value)

        if handler_timeout is not None and handler_timeout < 0:
            raise ValueError("handler_timeout can't be negative")
        self._handler_timeout = handler_timeout or None  # 0 and None both mean "never"
        # The C++ timer is only the backstop for the Python one (it can't cancel the coroutine).
        # ponytail: fixed 5 s grace.
        self.set_async_timeout(int(handler_timeout) + 5 if handler_timeout else 0)

    def route(self, path, methods=("GET",)):
        """Flask-style: @app.route("/x", methods=["GET", "POST"])."""
        pattern = _to_pattern(path)

        def register(fn):
            if inspect.iscoroutinefunction(fn):
                adapted, add = _adapt_async(fn, self), self.add_async_route
            else:
                adapted, add = _adapt(fn, self), self.add_route
            for m in methods:
                add(m, pattern, adapted)
            return fn  # unchanged, so it stays directly callable / testable

        return register

    def get(self, path):
        return self.route(path, ("GET",))

    def post(self, path):
        return self.route(path, ("POST",))

    def put(self, path):
        return self.route(path, ("PUT",))

    def patch(self, path):
        return self.route(path, ("PATCH",))

    def delete(self, path):
        return self.route(path, ("DELETE",))

    # --- hooks (the C++ core keeps one hook of each kind; these chain any number of them) ---

    def before_request(self, fn):
        """@app.before_request: fn(request) returns None to go on, or a response (anything a
        route may return) to answer right here without calling the route."""
        if not self._before:
            self.set_before_hook(self._run_before)
        self._before.append((fn, _prepare(fn)))
        return fn

    def after_request(self, fn):
        """@app.after_request: fn(request, response) may edit response.status / .headers
        (a ResponseHead). Hooks run in registration order, also for rejected requests."""
        if not self._after:
            self.set_after_hook(self._run_after)
        self._after.append(fn)
        return fn

    def error_handler(self, *statuses):
        """@app.error_handler(404, 405): fn(request) returns the response for an error the
        SERVER made itself (no route, wrong method, body too large, ...). Errors a route
        returned on purpose, and exceptions (see exception_handler), don't come here."""
        if not statuses or not all(isinstance(s, int) and 400 <= s <= 599 for s in statuses):
            raise ValueError("error_handler(*statuses) takes HTTP error statuses (400-599)")

        def register(fn):
            if not self._error_handlers:
                self.set_error_hook(self._run_error)
            for status in statuses:
                self._error_handlers[status] = (fn, _prepare(fn))
            return fn

        return register

    def exception_handler(self, *types):
        """@app.exception_handler(ValueError): fn(request, exc) answers for a route that raised
        `exc` (the most specific registered class wins). Status defaults to 500."""
        if not types or not all(isinstance(t, type) and issubclass(t, Exception) for t in types):
            raise TypeError("exception_handler(*types) takes Exception subclasses")

        def register(fn):
            for t in types:
                self._exception_handlers[t] = fn
            return fn

        return register

    def on_startup(self, fn):
        """@app.on_startup: run (sync or async) just before the server starts listening."""
        self._startup.append(fn)
        return fn

    def on_shutdown(self, fn):
        """@app.on_shutdown: run (sync or async) after the server stopped, in reverse order."""
        self._shutdown.append(fn)
        return fn

    # ponytail: Python exceptions never reach the C++ exception hook (the adapters catch them), so
    # exception_handler is handled in Python rather than through set_exception_handler.
    def _exception_handler_for(self, exc):
        for cls in type(exc).__mro__:
            handler = self._exception_handlers.get(cls)
            if handler is not None:
                return handler
        return None

    def _run_before(self, *raw):
        for fn, prepare in self._before:
            kwargs, _ = prepare(*raw)
            rv = _call(fn, **kwargs)
            if rv is not None:
                return _to_response(rv)
        return None

    def _run_after(self, method, path, body, headers, query, path_params, status, resp_headers):
        request = ServerRequest(method, path, body, headers, query, path_params)
        head = ResponseHead(status, resp_headers)
        for fn in self._after:
            _call(fn, request, head)
        for k, v in head.headers:  # hooks may have edited the list directly
            _check_header(k, v)
        return int(head.status), list(head.headers)

    def _run_error(self, method, path, body, headers, query, path_params, status):
        entry = self._error_handlers.get(status)
        if entry is None:
            return None
        fn, prepare = entry
        kwargs, _ = prepare(method, path, body, headers, query, path_params)
        rv = _call(fn, **kwargs)
        return None if rv is None else _to_response(rv, status)

    @contextlib.contextmanager
    def _lifecycle(self):
        for fn in self._startup:
            _call(fn)
        try:
            yield
        finally:
            for fn in reversed(self._shutdown):
                try:
                    _call(fn)
                except Exception:
                    traceback.print_exc()  # one failing hook must not stop the others

    def listen_after_bind(self):
        """Block until stop(), running the startup/shutdown hooks around it."""
        with self._lifecycle():
            _Server.listen_after_bind(self)

    def listen(self, host="127.0.0.1", port=8000):
        """Serve until stop() or Ctrl+C. Raises OSError if the port can't be bound.

        The compiled listen() blocks outside the interpreter, so Python never
        sees Ctrl+C while inside it; run it on a thread and keep the main
        thread in Python. # ponytail: 0.2s poll; upgrade path is signal.set_wakeup_fd.
        """
        failure = []

        def serve():
            try:
                _Server.listen(self, host, port)
            except BaseException as e:
                failure.append(e)

        with self._lifecycle():
            th = threading.Thread(target=serve, daemon=True)
            th.start()
            try:
                while th.is_alive():
                    th.join(0.2)
            except KeyboardInterrupt:
                self.stop()
                th.join()
        if failure:
            raise failure[0]
