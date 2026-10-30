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
import contextvars
import dataclasses
import dis
import functools
import inspect
import json
import mimetypes
import os
import re
import selectors
import socket
import sys
import threading
import time
import traceback

from . import _background, _forms, _log, _params, _router, _schema
from . import _validate as V
from .httpp_cy import Server as _Server, _attach_inbox, _drain_inbox, _gate_enter, _gate_leave, _set_lanes, parse_form as _parse_form
from ._websocket import WebSocket, WebSocketDisconnect
from .models import BaseModel, to_jsonable
from .responses import HTTPException, Response as _ResponseBase


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

    __slots__ = ("_raw", "_method", "_path", "_body", "_headers", "_query", "_path_params", "_form")

    def __init__(self, method, path, body, headers, query, path_params):
        self._raw = None
        self._form = None
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
        self._method = self._path = self._body = self._headers = self._query = self._path_params = self._form = None
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

    # The lookups the C++ `httpp::request` has under the same names. The default is "" like in C++ (pass None to tell
    # "absent" from "empty"); a repeated name gives its first value.
    def header(self, name, default=""):
        """A header by name, case-insensitive."""
        return self.headers.get(name.lower(), default)

    def query_param(self, name, default=""):
        return self.query.get(name, default)

    def path_param(self, name, default=""):
        return self.path_params.get(name, default)

    def cookie(self, name, default=""):
        return self.cookies.get(name, default)

    def form(self):
        """A FormData for an urlencoded or multipart body (any other Content-Type gives an empty one), parsed by C++ the
        first time. A malformed body raises HTTPException(400), so an uncaught one answers 400."""
        if self._form is None:
            try:
                if self._raw is not None:
                    parsed = self._raw.form()
                else:
                    parsed = _parse_form(self.body, self.headers.get("content-type", ""))
            except ValueError as exc:
                raise HTTPException(400, f"malformed form data: {exc}") from None
            self._form = _forms.FormData._from_tuple(parsed)
        return self._form

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

    A route handler gets the same object when it declares an argument called `response` (like `request`): it starts
    with `status` None and no headers; what you set is added to the handler's answer (a `status` you set wins over a
    status the handler returned), and `after_send` is a callable the server runs after the answer was written.
    """

    __slots__ = ("status", "headers", "after_send")

    def __init__(self, status, headers):
        self.status = status
        self.headers = headers
        self.after_send = None  # a route handler may set a callable to run after the answer was written

    def header(self, name):
        """First value of `name` (case-insensitive), or None."""
        low = name.lower()
        return next((v for k, v in self.headers if k.lower() == low), None)

    def add_header(self, name, value):
        _check_header(name, value)
        self.headers.append((name, value))

    def set_cookie(self, name, value, **options):
        """Add a Set-Cookie header (the options of httpp.set_cookie)."""
        self.headers.append(set_cookie(name, value, **options))

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
    body, the traceback goes to stderr); a client that hangs up closes a sync generator on the
    spot, an async generator is left to the garbage collector.
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

    if hasattr(gen, "close") and not inspect.isasyncgen(gen):
        provider.close = gen.close  # the release closes it at once: its `finally` runs now, not at GC time
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
    if isinstance(body, _ResponseBase):
        return rv  # a ready-made response is sent as it is
    errors = []
    out = check(body, ("response",), errors)
    if out is V.INVALID:
        raise ResponseValidationError(errors)
    return (_Json(to_jsonable(out)), *rest)


# FastAPI's wording for a path parameter that does not parse (Pydantic v2 lax mode)
_PARSE_ERRORS = {
    int: ("int_parsing", "Input should be a valid integer, unable to parse string as an integer"),
    float: ("float_parsing", "Input should be a valid number, unable to parse string as a number"),
}


def _http_exception_handler(request, exc):
    """The default answer for an HTTPException (override it with @app.exception_handler(HTTPException))."""
    return _Json({"detail": to_jsonable(exc.detail)}), exc.status_code, exc.headers


def _unprocessable(errors):
    body = json.dumps({"detail": errors}, default=str).encode("utf-8")
    return 422, body, [("Content-Type", "application/json")]


def _make_dumps():
    """json.dumps(obj).encode() without the per-call work of json.dumps: it builds a new C encoder every
    call (about 1.6 us of 2.7 us for a small dict). Same output as json.dumps with its defaults.
    ponytail: CPython only (c_make_encoder), json.dumps elsewhere; a circular structure ends in a
    RecursionError instead of json's ValueError (both become a 500)."""
    make = getattr(json.encoder, "c_make_encoder", None)
    if make is None:
        return lambda obj: json.dumps(obj).encode("utf-8")

    def default(o):
        raise TypeError(f"Object of type {type(o).__name__} is not JSON serializable")

    enc = make(None, default, json.encoder.encode_basestring_ascii, None, ": ", ", ", False, False, True)
    return lambda obj: "".join(enc(obj, 0)).encode("utf-8")


_dumps = _make_dumps()


_TEXT = "text/plain; charset=utf-8"
_JSON_TYPE = "application/json"


def _to_response(rv, default_status=200):
    # Fast path for the exact common types: the same answer as the general code below, without its checks
    # (no tuple, no response object, no custom header to validate). Subclasses take the general path.
    t = type(rv)
    if t is str:
        return default_status, rv.encode("utf-8"), [("Content-Type", _TEXT)]
    if t is dict or t is list:
        return default_status, _dumps(rv), [("Content-Type", _JSON_TYPE)]
    if isinstance(rv, _ResponseBase):
        rv = rv._as_return()
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
        body, ctype = _dumps(rv), "application/json"
    elif isinstance(rv, _Json):
        body, ctype = _dumps(rv.value), "application/json"
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


class _AfterHeaders(list):
    """The headers of an answer that also carries a job for C++ to run after sending (read by _fill_response in httpp_cy.pyx)."""

    __slots__ = ("after_send",)


def _merge_out(result, kwargs, plan):
    """Fold the injected `response` and BackgroundTasks of a finished handler into its (status, body, headers)."""
    status, body, headers = result
    resp = kwargs.get("response") if plan.wants_response else None
    tasks = kwargs.get(plan.background_name) if plan.background_name is not None else None
    first = None
    if resp is not None:
        if resp.status is not None:
            status = int(resp.status)
        if resp.headers:
            for k, v in resp.headers:
                _check_header(k, v)
            headers = headers + list(resp.headers)
        first = resp.after_send
        if first is not None and not callable(first):
            raise TypeError("response.after_send must be a callable")
    if first is None and not (tasks is not None and len(tasks)):
        return status, body, headers
    out = _AfterHeaders(headers)
    out.after_send = _background.AfterSend(first, tasks if tasks is not None and len(tasks) else None)
    return status, body, out


def _prepare(fn, plan=None):
    """Build the kwargs for `fn` from a RawRequest: path params (converted per annotation), the query / body
    parameters of its `plan` (see _params.py; hooks have none) plus `request` if asked for, and nothing else. Returns (kwargs, early_response); early_response is a
    ready (status, body, headers) when the request can't reach the handler (bad path parameter).
    What `fn` asks for is worked out once, here, not per request."""
    params = inspect.signature(fn).parameters
    takes_all = any(p.kind is p.VAR_KEYWORD for p in params.values())
    wants_request = "request" in params
    wants_response = plan is not None and plan.wants_response
    background_name = plan.background_name if plan is not None else None
    wanted = tuple(n for n in params if n != "request")
    convert = {n: p.annotation for n, p in params.items() if p.annotation in (int, float)}

    if not (takes_all or wants_request or wanted):
        return lambda raw: _NO_ARGS
    injected = wants_response or background_name is not None

    def prepare(raw):
        if takes_all:
            kwargs = raw.path_params()
        elif wanted:
            found = raw.path_params()
            kwargs = {n: found[n] for n in wanted if n in found}
        else:
            kwargs = {}
        bad = []
        for k, typ in convert.items():
            if k in kwargs:
                try:
                    kwargs[k] = typ(kwargs[k])
                except ValueError:
                    bad.append({"type": _PARSE_ERRORS[typ][0], "loc": ["path", k],
                                "msg": _PARSE_ERRORS[typ][1], "input": kwargs[k]})
        if bad:
            return None, _unprocessable(bad)
        if plan is not None and plan.active:
            errors = plan.run(raw, kwargs)
            if errors:
                return None, _unprocessable(errors)
        if wants_request:
            kwargs["request"] = ServerRequest._from_raw(raw)
        if injected:
            if wants_response:
                kwargs["response"] = ResponseHead(None, [])
            if background_name is not None:
                kwargs[background_name] = _background.BackgroundTasks()
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

    def form(self):
        ctype = next((v for k, v in self._headers if k.lower() == "content-type"), "")
        return _parse_form(self.body, ctype)


def _prepare_flat(fn):
    """_prepare for the hooks: called with the six values, not with a RawRequest."""
    prepare = _prepare(fn)
    return lambda *flat: prepare(_FlatRaw(*flat))


def _adapt(fn, app, plan=None, status=200, shape=None):
    """Sync handler -> what the compiled core calls on a worker thread: raw request in, (status, body, headers) out."""
    prepare = _prepare(fn, plan)
    has_out = plan is not None and (plan.wants_response or plan.background_name is not None)

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
                    result = _to_response(rv if shape is None else shape(rv), status)
                    return _merge_out(result, kwargs, plan) if has_out else result
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
            result = _to_response(rv if shape is None else shape(rv), status)
            return _merge_out(result, kwargs, plan) if has_out else result
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
_loop = None  # lane 0's loop: what code outside the lanes (a sync handler calling an async one) uses
_loop_lock = threading.Lock()
_lanes = []  # one per I/O thread on a free-threaded Python, else just one
_tasks = set()  # the loop only holds weak references to tasks; this keeps running handlers alive
_set_running_loop = asyncio.events._set_running_loop
_EAGER = sys.version_info >= (3, 12)  # asyncio.Task(..., eager_start=True) exists


class _Lane:
    """An asyncio loop thread with its own inbox (C++ side), gate, wake socket and deadlines. An I/O thread feeds one lane."""

    __slots__ = ("index", "loop", "wake", "base_context", "deadlines")

    def __init__(self, index):
        self.index = index
        self.loop = None
        self.wake = None  # (reader, writer) socket pair kept alive for the loop's whole life
        self.base_context = None  # the loop thread's context: a request handled on the I/O thread starts from a copy of it
        self.deadlines = None


class _GatedSelector(selectors.DefaultSelector):
    """The loop's selector. While the loop thread sleeps in it, the lane's gate is open: the I/O thread may then run
    a handler that cannot suspend right on the spot (httpp_cy `_dispatch_hybrid`), which is exactly like running
    it on the loop thread between two iterations: nothing else runs asyncio code at that moment, so the
    "code between two awaits is atomic" rule that asyncio programs rely on still holds."""

    def __init__(self, lane):
        super().__init__()
        self._lane = lane

    def select(self, timeout=None):
        _gate_leave(self._lane)
        try:
            return super().select(timeout)
        finally:
            _gate_enter(self._lane)  # waits (GIL released) while the I/O thread is running a handler


def _make_lane(index):
    lane = _Lane(index)
    # A selector loop on every platform: the default Proactor loop on Windows has no add_reader.
    loop = asyncio.SelectorEventLoop(_GatedSelector(index))
    loop._httpp_lane = lane
    reader, writer = socket.socketpair()
    reader.setblocking(False)
    writer.setblocking(False)

    def drain():
        if _drain_inbox(index):  # a big burst: let the loop breathe, then go on
            loop.call_soon(drain)

    def on_wake():
        try:
            reader.recv(4096)  # the byte only means "look at the inbox"
        except BlockingIOError:
            pass
        drain()

    loop.add_reader(reader.fileno(), on_wake)  # fine before run_forever: the loop isn't running yet
    lane.loop, lane.wake, lane.deadlines = loop, (reader, writer), _Deadlines()
    _attach_inbox(index, writer.fileno())

    def run():
        _gate_enter(index)  # held from now on, except while sleeping in the selector
        lane.base_context = contextvars.copy_context()
        asyncio.set_event_loop(loop)
        loop.run_forever()

    threading.Thread(target=run, name="httpp-asyncio" if index == 0 else f"httpp-asyncio-{index}", daemon=True).start()
    loop.call_soon_threadsafe(drain)  # requests queued before the inbox was attached
    return lane


def _ensure_lanes(n):
    """At least n lanes exist (started on first use). Requests for `async def` routes arrive through the C++ inbox of
    their lane: a C++ thread queues the request and pokes the lane's wake socket, the loop wakes up and dispatches the
    whole burst while holding the GIL once."""
    global _loop
    with _loop_lock:
        while len(_lanes) < max(1, n):
            _lanes.append(_make_lane(len(_lanes)))
        _loop = _lanes[0].loop
        return _lanes


def _get_loop():
    """The first loop (lane 0), started on first use."""
    return _ensure_lanes(1)[0].loop


def _gil_free():
    return hasattr(sys, "_is_gil_enabled") and not sys._is_gil_enabled()


_SUSPENDING = frozenset({"GET_AWAITABLE", "SEND", "YIELD_FROM", "YIELD_VALUE", "GET_AITER", "GET_ANEXT",
                         "BEFORE_ASYNC_WITH", "END_ASYNC_FOR"})


def _cannot_suspend(fn):
    """True for a coroutine function whose own code has no `await`, `async with` or `async for`: it always
    runs to its end in the first step. Anything without a code object (a callable instance, a partial) is
    not trusted."""
    code = getattr(fn, "__code__", None)
    if code is None or not inspect.iscoroutinefunction(fn):
        return False
    return not any(i.opname in _SUSPENDING for i in dis.get_instructions(code))


class _Deadlines:
    """One timer for every in-flight async request. A `call_later` per request (create, push on the heap,
    cancel) cost about a fifth of the request rate. Loop thread only, so no locking.
    ponytail: a sweep walks every in-flight request once per firing (a firing is at the oldest deadline,
    so about once per timeout when requests are quick); upgrade path: a heap if tens of thousands are in flight."""

    def __init__(self):
        self.pending = {}  # task -> deadline on the loop's clock
        self.expired = set()  # tasks cancelled by the sweep, until they have noticed
        self._timer = None
        self._at = None

    def add(self, loop, task, timeout):
        deadline = loop.time() + timeout
        self.pending[task] = deadline
        if self._timer is None or deadline < self._at:
            self._arm(loop, deadline)

    def forget(self, task):
        self.pending.pop(task, None)
        self.expired.discard(task)

    def _arm(self, loop, deadline):
        if self._timer is not None:
            self._timer.cancel()
        self._at = deadline
        self._timer = loop.call_at(deadline, self._sweep, loop)

    def _sweep(self, loop):
        self._timer = None
        now, soonest = loop.time(), None
        for task, deadline in list(self.pending.items()):
            if deadline <= now:
                del self.pending[task]
                self.expired.add(task)
                task.cancel()  # frees the coroutine; its answer would be too late anyway
            elif soonest is None or deadline < soonest:
                soonest = deadline
        if soonest is not None:
            self._arm(loop, soonest)


def _adapt_async(fn, app, plan=None, status=200, shape=None):
    """Async handler -> a dispatcher for the compiled core's loop routes: `dispatch(raw, done)` runs on
    the loop thread, returns at once, and the response is sent through `done` when the handler is finished.
    One task per request does everything: the handler, its exception handler and the timeout."""
    prepare = _prepare(fn, plan)
    has_deps = plan is not None and bool(plan.deps)
    has_out = plan is not None and (plan.wants_response or plan.background_name is not None)

    async def produce(raw, kwargs):
        async with contextlib.AsyncExitStack() as stack:  # a `yield` dependency is closed after the handler, also when it raised
            errors = await _params.asolve_deps(plan, _params.Ctx(raw, stack, ServerRequest._from_raw), kwargs)
            if errors:
                return _unprocessable(errors)
            rv = await fn(**kwargs)
            result = _to_response(rv if shape is None else shape(rv), status)
            return _merge_out(result, kwargs, plan) if has_out else result

    async def run(raw, done):
        task = asyncio.current_task()
        loop = asyncio.get_running_loop()
        deadlines = loop._httpp_lane.deadlines
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
                        if has_out:
                            result = _merge_out(result, kwargs, plan)
                except Exception as exc:
                    handler = app._exception_handler_for(exc)
                    if handler is None:
                        raise  # answered with a plain 500 below
                    rv = handler(ServerRequest._from_raw(raw), exc)
                    if inspect.isawaitable(rv):
                        rv = await rv
                    result = _to_response(rv, 500)
        except asyncio.CancelledError:
            if task not in deadlines.expired:
                raise  # the loop is shutting down: `done` is dropped and the connection closes unanswered
            result = (504, b"Gateway Timeout", [])
        except BaseException:
            traceback.print_exc()
            result = (500, b"Internal Server Error", [])
        finally:
            deadlines.forget(task)
            _tasks.discard(task)
        # done() only posts the write to the C++ loop (it never waits on the client), so with no
        # hooks it is safe, and cheaper, to call it right here. after/error hooks run inside
        # done() on the calling thread, and an `async def` one would wait on this very loop.
        # ponytail: executor fallback whenever such hooks exist; upgrade path: await the hooks here.
        if app._after or app._error_handlers:
            await loop.run_in_executor(None, done, *result)
        else:
            try:
                done(*result)
            except Exception:  # e.g. a bad header: answer something instead of leaving the client to the 504
                traceback.print_exc()
                done(500, b"Internal Server Error", [])

    def dispatch(raw, done, inline=False, lane=0):
        if inline:
            # On an I/O thread, holding its lane's gate: the loop thread is asleep in its selector, so this is
            # the loop's own work done early. The loop must look "running" to the code in the handler; whatever
            # the handler scheduled (a task, a timer) is seen by the loop thread once we wake it.
            ln = _lanes[lane]
            loop = ln.loop
            if loop.get_debug():  # debug mode checks the thread of every call: not for us
                raise RuntimeError("inline handlers need a loop that is not in debug mode")
            ready, scheduled = len(loop._ready), len(loop._scheduled)
            _set_running_loop(loop)
            try:
                task = asyncio.Task(run(raw, done), loop=loop, eager_start=True, context=ln.base_context.copy())
            finally:
                _set_running_loop(None)
            if not task.done():
                _tasks.add(task)
                if app._handler_timeout:
                    ln.deadlines.add(loop, task, app._handler_timeout)
            if not task.done() or len(loop._ready) != ready or len(loop._scheduled) != scheduled:
                ln.wake[1].send(b"\0")  # the loop sleeps with a timeout that does not know about this
            return
        loop = asyncio.get_running_loop()
        if _EAGER:
            # Python 3.12+: the handler runs to its first suspension right here, so a handler that never
            # waits finishes without a trip through the loop's ready queue. A task that is already done
            # is not kept (nothing would ever remove it).
            task = asyncio.Task(run(raw, done), loop=loop, eager_start=True)
            if not task.done():
                _tasks.add(task)  # the loop only keeps weak references to tasks
                if app._handler_timeout:  # a handler that finished at once needs no deadline
                    loop._httpp_lane.deadlines.add(loop, task, app._handler_timeout)
            return
        task = loop.create_task(run(raw, done))
        _tasks.add(task)  # the loop only keeps weak references to tasks
        if app._handler_timeout:
            loop._httpp_lane.deadlines.add(loop, task, app._handler_timeout)

    return dispatch


def _origin_ok(raw, allowed):
    """The cross-site WebSocket hijacking check (browsers send Origin on a WebSocket handshake and let any page open one to
    any host, cookies included). No Origin header: not a browser, allowed. `allowed` None = same origin only (the Origin's
    host is the Host header); "*" in it = everything; otherwise those origins (and the same one)."""
    origin = host = None
    for name, value in raw.headers():
        lower = name.lower()
        if lower == "origin":
            origin = value
        elif lower == "host":
            host = value
    if origin is None:
        return True
    if allowed is not None and ("*" in allowed or origin.rstrip("/").lower() in allowed):
        return True
    netloc = origin.split("://", 1)[-1].rstrip("/")
    return host is not None and netloc.lower() == host.lower()


def _adapt_ws(fn, plan, origins):
    """A WebSocket handler -> `dispatch(raw, core)` for the compiled core: runs on the loop thread, starts one task for the
    connection and returns at once. The task answers the upgrade (accept / reject), runs the handler, and when it ends closes the
    connection (1000, or 1011 after an exception) or, if the handler never accepted, rejects with 403 (500 after an exception)."""
    prepare = _prepare(fn, plan)
    ws_name = plan.websocket_name

    async def run(raw, core):
        task = asyncio.current_task()
        ws = WebSocket(core, ServerRequest._from_raw(raw), asyncio.get_running_loop())
        core.set_waiter(ws)
        failed = False
        try:
            kwargs, early = prepare(raw)
            if early is not None:
                core.reject(early[0], early[1] if isinstance(early[1], bytes) else b"")
            else:
                kwargs[ws_name] = ws
                await fn(**kwargs)
        except WebSocketDisconnect:
            pass
        except HTTPException as exc:
            if core.answered():
                failed = True
                traceback.print_exc()
            else:
                core.reject(exc.status_code, str(exc.detail).encode("utf-8"))
        except asyncio.CancelledError:
            raise
        except BaseException:
            failed = True
            traceback.print_exc()
        finally:
            try:
                if not core.answered():
                    core.reject(500 if failed else 403)
                elif core.is_open():
                    core.close(1011 if failed else 1000)
            finally:
                core.release()
                _tasks.discard(task)

    def dispatch(raw, core):
        if not _origin_ok(raw, origins):
            core.reject(403, b"")
            return
        loop = asyncio.get_running_loop()
        if _EAGER:
            task = asyncio.Task(run(raw, core), loop=loop, eager_start=True)
        else:
            task = loop.create_task(run(raw, core))
        if not task.done():
            _tasks.add(task)

    return dispatch


class Server(_Server):
    """httpp.Server plus @route/@get/@post/... decorators, hooks and settings.

    threads / max_threads: worker threads for sync handlers (default: about one per core, at
    least 8; under load up to max_threads, default 4 x threads).  read_timeout / write_timeout /
    keep_alive_timeout: seconds (defaults 5).  keep_alive_max: requests per connection (100).
    max_body: largest request body in bytes, refused above it with a 413 (a client still
    sending a huge body may see the connection closed instead); 0 = no limit.  header_timeout: seconds the
    whole request head may take to arrive, however slowly it is dripped (default 10, 0 = no limit).
    compression: gzip text answers (text, JSON, XML, JavaScript, SVG) of at least this many bytes for clients that
    send Accept-Encoding: gzip (default: off); compression_level 1 (fastest, default) to 9.
    body_timeout: the same for the request body, counted from the end of the head (default 120, 0 = no limit).
    max_connections: open connections allowed, one more gets a 503 (default: no limit).  handler_timeout:
    seconds an async handler may take before it is cancelled and answered with a 504 (default 60,
    None = never).  io_threads: I/O threads sharing the port (default: min(4, CPUs // 4) with a GIL, 1 on a free-threaded Python or Windows; needs SO_REUSEPORT). On a
    free-threaded Python each also gets its own asyncio loop thread, so `async def` handlers use several cores; with a
    GIL there is still one loop. A connection stays on one thread, and so on one loop.  inline_async: run `async def`
    handlers that cannot suspend on the I/O thread (ADR-0023); default on only with 2 CPUs or fewer.  Everything left as None keeps
    the C++ default.
    """

    def __init__(self, *, threads=None, max_threads=None, read_timeout=None, write_timeout=None,
                 keep_alive_timeout=None, keep_alive_max=None, max_body=None, header_timeout=None,
                 body_timeout=None, compression=None, compression_level=1, max_connections=None, handler_timeout=60,
                 io_threads=None, inline_async=None, title="httpp", version="0.1.0", description=None, openapi_url="/openapi.json", docs_url="/docs"):
        _Server.__init__(self)
        cpus = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else (os.cpu_count() or 1)
        if io_threads is None:
            # Measured (16 threads, ADR-0024): two I/O threads nearly double the hello rate, four add little more.
            # Not with a free-threaded Python: there every I/O thread brings an asyncio loop, which changes what shared
            # state in async handlers must withstand, so that is asked for, not assumed. Windows has no SO_REUSEPORT.
            io_threads = 1 if _gil_free() or sys.platform == "win32" else min(4, max(1, cpus // 4))
        if io_threads < 1:
            raise ValueError("io_threads must be at least 1")
        self._io_threads = 1
        if io_threads > 1:
            self.set_io_threads(io_threads)
        # Running a handler that cannot suspend on the I/O thread saves CPU but puts parsing, the handler and the
        # answer on one thread. With cores to spare the hop to the loop thread is the faster pipeline (measured on 16
        # threads: 126k against 158k req/s), with one or two cores the saved CPU wins (about 30 percent more).
        self._inline_async = (cpus <= 2) if inline_async is None else bool(inline_async)
        self._routes, self._openapi_doc = [], None
        self._title, self._version, self._description = title, version, description
        self._before, self._after, self._startup, self._shutdown = [], [], [], []
        self._error_handlers, self._exception_handlers = {}, {}

        if threads is None:
            if max_threads is not None:
                raise ValueError("max_threads needs threads")
            if not _gil_free():
                # With a GIL, workers only take turns at running Python: on a CPU-bound handler one worker gave 16.7k
                # req/s and p99 8 ms, sixteen 10.6k and 35 ms (ADR-0025). So the pool starts with one thread and grows
                # only when a job has waited 10 ms for a free one (a blocked handler makes that certain, CPU-bound
                # work at saturation rarely waits that long), up to what the C++ default would have been.
                self.set_thread_pool(1, max(8, cpus) * 4, grow_after_ms=10)
        else:
            self.set_thread_pool(threads, max_threads or 0)
        for value, setter in ((read_timeout, self.set_read_timeout),
                              (write_timeout, self.set_write_timeout),
                              (keep_alive_timeout, self.set_keep_alive_timeout),
                              (keep_alive_max, self.set_keep_alive_max),
                              (max_body, self.set_max_body),
                              (header_timeout, self.set_header_timeout),
                              (body_timeout, self.set_body_timeout),
                              (max_connections, self.set_max_connections)):
            if value is not None:
                setter(value)

        if compression is not None:
            self.set_compression(compression, compression_level)

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
                adapted = _adapt_async(fn, self, plan, status, shape)
                # A handler that cannot suspend may run on the I/O thread (see _GatedSelector).
                inline = self._inline_async and _EAGER and not plan.deps and _cannot_suspend(fn)
                add = self._add_raw_hybrid_route if inline else self._add_raw_loop_route
                self._has_async = True
            else:
                adapted, add = _adapt(fn, self, plan, status, shape), self._add_raw_route
            for m in methods:
                add(m, pattern, adapted)
            self._routes.append(_params.Route(path, tuple(methods), fn, plan, meta))
            self._openapi_doc = None
            return fn  # unchanged, so it stays directly callable / testable

        return register

    def websocket(self, path, *, origins=None, max_message=1 << 20, max_buffered=1 << 20, max_queued=1 << 20, ping_interval=0):
        """WebSocket route (RFC 6455), FastAPI / Starlette style:

            @app.websocket("/ws/{room}")
            async def chat(websocket, room):
                await websocket.accept()
                async for text in websocket:
                    await websocket.send_text(text)

        The handler must be `async def` and take the connection as `websocket` (or annotated `WebSocket`); path and query
        parameters work as in `route`. It must `await websocket.accept()` first, else the upgrade is rejected with 403.
        origins: allowed Origin values for browsers (default: the page must come from this server's own host; ["*"] allows
        all). max_message: bytes of one message (more closes with 1009). max_buffered: bytes queued for sending; past it a
        send waits. max_queued: received bytes waiting for the handler; past it the socket is not read (TCP pushes back on the
        peer). ping_interval: seconds of silence before a ping, and again before the connection is closed; 0 = off."""
        for name, value in (("max_message", max_message), ("max_buffered", max_buffered), ("max_queued", max_queued),
                            ("ping_interval", ping_interval)):
            if not isinstance(value, int) or isinstance(value, bool) or value < (0 if name == "ping_interval" else 1):
                raise ValueError(f"{name} must be a {'non-negative' if name == 'ping_interval' else 'positive'} integer")
        allowed = None if origins is None else frozenset(o.rstrip("/").lower() for o in origins)
        pattern = _params.to_cpp_pattern(path)

        def register(fn):
            if not inspect.iscoroutinefunction(fn):
                raise TypeError(f"{fn.__name__}: a WebSocket handler must be `async def`")
            plan = _params.make_plan(fn, path)
            if plan.websocket_name is None:
                raise TypeError(f"{fn.__name__}: a WebSocket handler must take the connection as `websocket` (or annotated WebSocket)")
            if plan.deps or plan.wants_response or plan.background_name is not None:
                raise TypeError(f"{fn.__name__}: dependencies, `response` and background tasks are not supported on a WebSocket route")
            self._add_ws_route(pattern, _adapt_ws(fn, plan, allowed), max_message, max_buffered, ping_interval, max_queued)
            self._has_async = True  # the connection's lane (loop thread + inbox) must be running
            return fn

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

    def include(self, router, prefix=""):
        """Register every route of an APIRouter under prefix + router.prefix (the router is not kept)."""
        _router.include_into(self, router, prefix)
        return self

    include_router = include  # FastAPI's name

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
        return _http_exception_handler if isinstance(exc, HTTPException) else None

    def _run_before(self, *raw):
        for fn, prepare in self._before:
            kwargs, _ = prepare(*raw)
            try:
                rv = _call(fn, **kwargs)
            except HTTPException as exc:  # the usual way to refuse a request in a hook (auth)
                handler = self._exception_handler_for(exc)
                return _to_response(_call(handler, ServerRequest(*raw), exc), 500)
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
            free = _gil_free()
            lanes = self._io_threads if free else 1  # more loops than one only pay without a GIL
            _set_lanes(lanes, free)
            _ensure_lanes(lanes)  # the inboxes must be draining before the first request can arrive
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
        thread in Python. # ponytail: 50 ms poll; upgrade path is signal.set_wakeup_fd.
        """
        if _defer_listen:  # `httpp run` owns host, port and workers
            return _skipped("listen", host, port)
        _log.sync()  # a root-level change the "httpp" logger only inherits is not announced; pick it up here
        failure = []
        finished = []  # a plain list, not a threading.Event: see the note below

        def serve():
            try:
                _Server.listen(self, host, port)
            except BaseException as e:
                failure.append(e)
            finally:
                finished.append(True)

        with self._lifecycle():
            threading.Thread(target=serve, daemon=True).start()
            # ponytail: this thread polls a flag every 50 ms instead of waiting on a threading.Event. A Ctrl+C that lands
            # inside Event.wait() can leave the Event's internal lock taken (or released twice) on free-threaded CPython,
            # and then this thread and the serving thread both block on it for ever. time.sleep holds no lock.
            # Upgrade path: signal.set_wakeup_fd, which wakes at once with no polling.
            try:
                while not finished:
                    time.sleep(0.05)
            except KeyboardInterrupt:
                # stop() does nothing until the port is bound, so repeat it until the server is really done.
                while not finished:
                    self.stop()
                    time.sleep(0.05)
        if failure:
            raise failure[0]

    def set_io_threads(self, n):
        """Same as the constructor's io_threads. Before listening."""
        n = int(n)
        if n < 1:
            raise ValueError("io_threads must be at least 1")
        _Server.set_io_threads(self, n)
        self._io_threads = n
        return self

    def run(self, host="127.0.0.1", port=8000, **options):
        """The usual entry point: `if __name__ == "__main__": app.run()`. Same as httpp.run(app, ...)."""
        if _defer_listen:
            return _skipped("run", host, port)
        from ._run import run

        return run(self, host, port, **options)
