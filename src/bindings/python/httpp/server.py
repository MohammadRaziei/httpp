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
import dataclasses
import functools
import inspect
import json
import mimetypes
import os
import re
import socket
import threading
import traceback

from . import _params, _schema
from . import _validate as V
from .httpp_cy import Server as _Server, _attach_inbox, _drain_inbox
from .models import BaseModel, to_jsonable


_ROUTE_META = frozenset(("summary", "description", "tags", "response_model", "status_code", "deprecated", "include_in_schema"))

class ServerRequest:
    """What a route handler receives as `request`.

    headers: dict, lower-cased names.  query: dict of ?a=1 params (urlencoded form bodies are
    merged in here too).  path_params: dict of str.  For repeated names the first value wins.
    # ponytail: multi-dict if needed

    Inside a route this is a view on the request as C++ holds it: each field is read (and turned
    into a Python object) the first time it is used, so a handler pays only for what it touches.
    The fields are read-only.
    """

    __slots__ = ("_raw", "_method", "_path", "_body", "_headers", "_query", "_path_params")

    def __init__(self, method, path, body, headers, query, path_params):
        self._raw = None
        self._method = method
        self._path = path
        self._body = body  # bytes
        self._headers = _first_wins(headers, str.lower)
        self._query = _first_wins(query)
        self._path_params = dict(path_params)

    @classmethod
    def _from_raw(cls, raw):
        self = cls.__new__(cls)
        self._raw = raw
        self._method = self._path = self._body = self._headers = self._query = self._path_params = None
        return self

    @property
    def method(self):
        if self._method is None:
            self._method = self._raw.method
        return self._method

    @property
    def path(self):
        if self._path is None:
            self._path = self._raw.path
        return self._path

    @property
    def body(self):
        if self._body is None:
            self._body = self._raw.body
        return self._body

    @property
    def headers(self):
        if self._headers is None:
            self._headers = _first_wins(self._raw.headers(), str.lower)
        return self._headers

    @property
    def query(self):
        if self._query is None:
            self._query = _first_wins(self._raw.query())
        return self._query

    @property
    def path_params(self):
        if self._path_params is None:
            self._path_params = self._raw.path_params()
        return self._path_params

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


class _Json:
    """A value already turned into plain JSON data, to be sent as application/json whatever its type."""

    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value


class ResponseValidationError(ValueError):
    """What a handler returned does not fit its response_model (a bug in the handler, so: no handler, a 500)."""

    def __init__(self, errors):
        self._errors = errors
        super().__init__(f"{len(errors)} validation error(s) for the response: {errors}")

    def errors(self):
        return list(self._errors)


def _shape(rv, check):
    """Run a handler's return value through its response_model; (body, status, ...) tuples keep their extras."""
    body, *rest = rv if isinstance(rv, tuple) else (rv,)
    errors = []
    out = check(body, ("response",), errors)
    if out is V.INVALID:
        raise ResponseValidationError(errors)
    return (_Json(to_jsonable(out)), *rest)


def _unprocessable(errors):
    body = json.dumps({"detail": errors}, default=str).encode("utf-8")
    return 422, body, [("Content-Type", "application/json")]


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
    elif isinstance(rv, _Json):
        body, ctype = json.dumps(rv.value).encode("utf-8"), "application/json"
    elif isinstance(rv, BaseModel) or (hasattr(rv, "model_dump") and not isinstance(rv, type)) \
            or (dataclasses.is_dataclass(rv) and not isinstance(rv, type)):
        body, ctype = json.dumps(to_jsonable(rv)).encode("utf-8"), "application/json"
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


_NO_ARGS = ({}, None)


def _prepare(fn, plan=None):
    """Build the kwargs for `fn` from a RawRequest: path params (converted per annotation), the query / body
    parameters of its `plan` (see _params.py; hooks have none) plus `request` if asked for, and nothing else. Returns (kwargs, early_response); early_response is a
    ready (status, body, headers) when the request can't reach the handler (bad path parameter).
    What `fn` asks for is worked out once, here, not per request."""
    params = inspect.signature(fn).parameters
    takes_all = any(p.kind is p.VAR_KEYWORD for p in params.values())
    wants_request = "request" in params
    wanted = tuple(n for n in params if n != "request")
    convert = {n: p.annotation for n, p in params.items() if p.annotation in (int, float)}

    if not (takes_all or wants_request or wanted):
        return lambda raw: _NO_ARGS

    def prepare(raw):
        if takes_all:
            kwargs = raw.path_params()
        elif wanted:
            found = raw.path_params()
            kwargs = {n: found[n] for n in wanted if n in found}
        else:
            kwargs = {}
        try:
            for k, typ in convert.items():
                if k in kwargs:
                    kwargs[k] = typ(kwargs[k])
        except ValueError:
            return None, _to_response(({"error": f"path parameter {k!r} must be {typ.__name__}"}, 422))
        if plan is not None and plan.active:
            errors = plan.run(raw, kwargs)
            if errors:
                return None, _unprocessable(errors)
        if wants_request:
            kwargs["request"] = ServerRequest._from_raw(raw)
        return kwargs, None

    return prepare


class _FlatRaw:
    """The RawRequest interface over the six plain values the C++ hooks (before / error) are given."""

    __slots__ = ("method", "path", "body", "_headers", "_query", "_params")

    def __init__(self, method, path, body, headers, query, path_params):
        self.method, self.path, self.body = method, path, body
        self._headers, self._query, self._params = headers, query, path_params

    def headers(self):
        return self._headers

    def query(self):
        return self._query

    def path_params(self):
        return _first_wins(self._params)


def _prepare_flat(fn):
    """_prepare for the hooks: called with the six values, not with a RawRequest."""
    prepare = _prepare(fn)
    return lambda *flat: prepare(_FlatRaw(*flat))


def _adapt(fn, app, plan=None, status=200, shape=None):
    """Sync handler -> what the compiled core calls on a worker thread: raw request in, (status, body, headers) out."""
    prepare = _prepare(fn, plan)

    if plan is not None and plan.deps:

        def call(raw):
            kwargs, early = prepare(raw)
            if early is not None:
                return early
            try:
                with contextlib.ExitStack() as stack:  # a `yield` dependency is closed after the handler, also when it raised
                    errors = _params.solve_deps(plan, _params.Ctx(raw, stack, ServerRequest._from_raw), kwargs)
                    if errors:
                        return _unprocessable(errors)
                    rv = fn(**kwargs)
                    return _to_response(rv if shape is None else shape(rv), status)
            except Exception as exc:
                handler = app._exception_handler_for(exc)
                if handler is None:
                    raise
                return _to_response(_call(handler, ServerRequest._from_raw(raw), exc), 500)

        return call

    def call(raw):
        kwargs, early = prepare(raw)
        if early is not None:
            return early
        try:
            rv = fn(**kwargs)
            return _to_response(rv if shape is None else shape(rv), status)
        except Exception as exc:
            handler = app._exception_handler_for(exc)
            if handler is None:
                raise  # the compiled core logs it and answers a plain 500
            return _to_response(_call(handler, ServerRequest._from_raw(raw), exc), 500)

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


_defer_listen = False  # True while `httpp run` imports the app: listen()/run() calls in the file are skipped


def _skipped(call, host, port):
    """The warning for a listen()/run() the file made at import time while `httpp run` was importing it."""
    import sys

    sys.stderr.write(
        f"httpp: ignoring app.{call}({host!r}, {port!r}): `httpp run` decides host, port and workers "
        f"(call it under `if __name__ == \"__main__\":` to keep both ways working)\n")
    sys.stderr.flush()
_loop = None
_loop_lock = threading.Lock()
_wake = None  # (reader, writer) socket pair kept alive for the loop's whole life
_tasks = set()  # the loop only holds weak references to tasks; this keeps running handlers alive


def _get_loop():
    """The one event loop all async handlers run on (started on first use).

    Requests for `async def` routes arrive through the C++ inbox: a C++ thread queues the request
    and pokes `_wake`, this loop wakes up and dispatches the whole burst while holding the GIL once.
    """
    global _loop, _wake
    with _loop_lock:
        if _loop is None:
            # A selector loop on every platform: the default Proactor loop on Windows has no add_reader.
            loop = asyncio.SelectorEventLoop()
            reader, writer = socket.socketpair()
            reader.setblocking(False)
            writer.setblocking(False)

            def drain():
                if _drain_inbox():  # a big burst: let the loop breathe, then go on
                    loop.call_soon(drain)

            def on_wake():
                try:
                    reader.recv(4096)  # the byte only means "look at the inbox"
                except BlockingIOError:
                    pass
                drain()

            loop.add_reader(reader.fileno(), on_wake)  # fine before run_forever: the loop isn't running yet
            _wake = (reader, writer)
            _attach_inbox(writer.fileno())

            def run():
                asyncio.set_event_loop(loop)
                loop.run_forever()

            threading.Thread(target=run, name="httpp-asyncio", daemon=True).start()
            loop.call_soon_threadsafe(drain)  # requests queued before the inbox was attached
            _loop = loop
        return _loop


def _adapt_async(fn, app, plan=None, status=200, shape=None):
    """Async handler -> a dispatcher for the compiled core's loop routes: `dispatch(raw, done)` runs on
    the loop thread, returns at once, and the response is sent through `done` when the handler is finished.
    One task per request does everything: the handler, its exception handler and the timeout."""
    prepare = _prepare(fn, plan)
    has_deps = plan is not None and bool(plan.deps)

    async def produce(raw, kwargs):
        async with contextlib.AsyncExitStack() as stack:  # a `yield` dependency is closed after the handler, also when it raised
            errors = await _params.asolve_deps(plan, _params.Ctx(raw, stack, ServerRequest._from_raw), kwargs)
            if errors:
                return _unprocessable(errors)
            rv = await fn(**kwargs)
            return _to_response(rv if shape is None else shape(rv), status)

    async def run(raw, done):
        task = asyncio.current_task()
        loop = asyncio.get_running_loop()
        timeout = app._handler_timeout
        timed_out = False
        timer = None
        if timeout:

            def expire():
                nonlocal timed_out
                timed_out = True
                task.cancel()  # frees the coroutine; its answer would be too late anyway

            timer = loop.call_later(timeout, expire)
        try:
            kwargs, early = prepare(raw)
            if early is not None:
                result = early
            else:
                try:
                    if has_deps:
                        result = await produce(raw, kwargs)
                    else:
                        rv = await fn(**kwargs)
                        result = _to_response(rv if shape is None else shape(rv), status)
                except Exception as exc:
                    handler = app._exception_handler_for(exc)
                    if handler is None:
                        raise  # answered with a plain 500 below
                    rv = handler(ServerRequest._from_raw(raw), exc)
                    if inspect.isawaitable(rv):
                        rv = await rv
                    result = _to_response(rv, 500)
        except asyncio.CancelledError:
            if not timed_out:
                raise  # the loop is shutting down: `done` is dropped and the connection closes unanswered
            result = (504, b"Gateway Timeout", [])
        except BaseException:
            traceback.print_exc()
            result = (500, b"Internal Server Error", [])
        finally:
            if timer is not None:
                timer.cancel()
            _tasks.discard(task)
        # done() only posts the write to the C++ loop (it never waits on the client), so with no
        # hooks it is safe, and cheaper, to call it right here. after/error hooks run inside
        # done() on the calling thread, and an `async def` one would wait on this very loop.
        # ponytail: executor fallback whenever such hooks exist; upgrade path: await the hooks here.
        if app._after or app._error_handlers:
            await loop.run_in_executor(None, done, *result)
        else:
            done(*result)

    def dispatch(raw, done):
        task = asyncio.get_running_loop().create_task(run(raw, done))
        _tasks.add(task)  # the loop only keeps weak references to tasks

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
                 keep_alive_timeout=None, keep_alive_max=None, max_body=None, handler_timeout=60,
                 title="httpp", version="0.1.0", description=None, openapi_url="/openapi.json", docs_url="/docs"):
        _Server.__init__(self)
        self._routes, self._openapi_doc = [], None
        self._title, self._version, self._description = title, version, description
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
        self._has_async = False  # set by the first `async def` route
        # The C++ timer is only the backstop for the Python one (it can't cancel the coroutine).
        # ponytail: fixed 5 s grace.
        self.set_async_timeout(int(handler_timeout) + 5 if handler_timeout else 0)
        if openapi_url is not None:
            self._add_docs(openapi_url, docs_url)

    def route(self, path, methods=("GET",), **meta):
        """Flask-style: @app.route("/x", methods=["GET", "POST"]).

        Also FastAPI's per-route options: summary, description, tags, deprecated, include_in_schema (these
        shape the OpenAPI document), status_code (the status when the handler returns just a body) and
        response_model (a type the returned value is validated against and serialized with)."""
        unknown = set(meta) - _ROUTE_META
        if unknown:
            raise TypeError(f"route() got unexpected options {sorted(unknown)}; known: {sorted(_ROUTE_META)}")
        pattern = _params.to_cpp_pattern(path)

        def register(fn):
            plan = _params.make_plan(fn, path)
            if not inspect.iscoroutinefunction(fn):
                for sub in plan.walk():
                    for _, dep in sub.deps:
                        if dep.kind in ("async", "asyncgen"):
                            raise TypeError(f"{fn.__name__}: a sync route can't use the async dependency {dep.fn.__name__!r}; "
                                            "make the route `async def`")
            status = meta.get("status_code") or 200
            model = meta.get("response_model")
            check = V.compile_type(model) if model is not None else None
            shape = (lambda rv: _shape(rv, check)) if check is not None else None
            if inspect.iscoroutinefunction(fn):
                adapted, add = _adapt_async(fn, self, plan, status, shape), self._add_raw_loop_route
                self._has_async = True
            else:
                adapted, add = _adapt(fn, self, plan, status, shape), self._add_raw_route
            for m in methods:
                add(m, pattern, adapted)
            self._routes.append(_params.Route(path, tuple(methods), fn, plan, meta))
            self._openapi_doc = None
            return fn  # unchanged, so it stays directly callable / testable

        return register

    def get(self, path, **meta):
        return self.route(path, ("GET",), **meta)

    def post(self, path, **meta):
        return self.route(path, ("POST",), **meta)

    def put(self, path, **meta):
        return self.route(path, ("PUT",), **meta)

    def patch(self, path, **meta):
        return self.route(path, ("PATCH",), **meta)

    def delete(self, path, **meta):
        return self.route(path, ("DELETE",), **meta)

    # --- OpenAPI and the Swagger UI page ---

    def openapi(self):
        """The OpenAPI 3.1 document for the routes registered so far (what /openapi.json serves)."""
        if self._openapi_doc is None:
            self._openapi_doc = _schema.build_openapi(self._routes, title=self._title, version=self._version,
                                                      description=self._description)
        return self._openapi_doc

    def _add_docs(self, openapi_url, docs_url):
        @self.get(openapi_url, include_in_schema=False)
        def openapi_json():
            return self.openapi()

        if docs_url is not None:  # Swagger UI is served by C++ from files compiled into libhttpp
            self.mount_swagger_ui(docs_url, openapi_url, self._title)

    # --- hooks (the C++ core keeps one hook of each kind; these chain any number of them) ---

    def before_request(self, fn):
        """@app.before_request: fn(request) returns None to go on, or a response (anything a
        route may return) to answer right here without calling the route."""
        if not self._before:
            self.set_before_hook(self._run_before)
        self._before.append((fn, _prepare_flat(fn)))
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
                self._error_handlers[status] = (fn, _prepare_flat(fn))
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
        if self._has_async:
            _get_loop()  # the inbox must be draining before the first request can arrive
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
        if _defer_listen:  # `httpp run` owns host, port and workers
            return _skipped("listen", host, port)
        failure = []
        done = threading.Event()

        def serve():
            try:
                _Server.listen(self, host, port)
            except BaseException as e:
                failure.append(e)
            finally:
                done.set()

        with self._lifecycle():
            threading.Thread(target=serve, daemon=True).start()
            try:
                while not done.wait(0.2):
                    pass
            except KeyboardInterrupt:
                # stop() does nothing until the port is bound, so repeat it until the server is
                # really done. An Event, not Thread.join(): a KeyboardInterrupt inside join() can
                # make a running thread look dead on some CPython versions.
                while not done.wait(0.05):
                    self.stop()
        if failure:
            raise failure[0]

    def run(self, host="127.0.0.1", port=8000, **options):
        """The usual entry point: `if __name__ == "__main__": app.run()`. Same as httpp.run(app, ...)."""
        if _defer_listen:
            return _skipped("run", host, port)
        from ._run import run

        return run(self, host, port, **options)
