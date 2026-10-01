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
A handler may return:  str | bytes | dict | list | None,
optionally as (body, status) or (body, status, headers_dict).
Raise nothing special for errors: return ("not found", 404). An uncaught
exception becomes a plain 500 and the traceback goes to stderr.

Register routes BEFORE calling listen().
"""

import asyncio
import inspect
import json
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


def _to_response(rv):
    status, headers = 200, {}
    if isinstance(rv, tuple):
        rv, *extra = rv
        if len(extra) > 2:
            raise TypeError("a route may return body, (body, status) or (body, status, headers)")
        if extra:
            status = int(extra[0])
        if len(extra) == 2:
            headers = dict(extra[1])

    if rv is None:
        body, ctype = b"", "text/plain; charset=utf-8"
    elif isinstance(rv, (bytes, bytearray)):
        body, ctype = bytes(rv), "application/octet-stream"
    elif isinstance(rv, str):
        body, ctype = rv.encode("utf-8"), "text/plain; charset=utf-8"
    elif isinstance(rv, (dict, list)):
        body, ctype = json.dumps(rv).encode("utf-8"), "application/json"
    else:
        raise TypeError(f"a route can't return {type(rv).__name__}")

    if not any(k.lower() == "content-type" for k in headers):
        headers["Content-Type"] = ctype
    return status, body, list(headers.items())


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


def _adapt(fn):
    """Sync handler -> the flat blocking signature the compiled core calls."""
    prepare = _prepare(fn)

    def call(*raw):
        kwargs, early = prepare(*raw)
        return early if early is not None else _to_response(fn(**kwargs))

    return call


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


def _adapt_async(fn):
    """Async handler -> a dispatcher for the compiled core's non-blocking
    routes: it returns at once and the response is sent through `done` later."""
    prepare = _prepare(fn)

    async def call(*raw):
        kwargs, early = prepare(*raw)
        return early if early is not None else _to_response(await fn(**kwargs))

    def dispatch(method, path, body, headers, query, path_params, done):
        loop = _get_loop()

        async def run():
            try:
                status, rbody, rheaders = await call(method, path, body, headers, query, path_params)
            except BaseException:
                traceback.print_exc()
                status, rbody, rheaders = 500, b"Internal Server Error", []
            # Writing can block on a slow client; keep that off the event loop.
            await loop.run_in_executor(None, done, status, rbody, rheaders)

        asyncio.run_coroutine_threadsafe(run(), loop)

    return dispatch


class Server(_Server):
    """httpp.Server plus @route/@get/@post/... decorators."""

    def route(self, path, methods=("GET",)):
        """Flask-style: @app.route("/x", methods=["GET", "POST"])."""
        pattern = _to_pattern(path)

        def register(fn):
            if inspect.iscoroutinefunction(fn):
                adapted, add = _adapt_async(fn), self.add_async_route
            else:
                adapted, add = _adapt(fn), self.add_route
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
