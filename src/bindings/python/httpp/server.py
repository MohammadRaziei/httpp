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
answers 422). Handlers may be `async def`. It may return:  str | bytes | dict | list | None,
optionally as (body, status) or (body, status, headers_dict).
Raise nothing special for errors: return ("not found", 404). An uncaught
exception becomes a plain 500 and the traceback goes to stderr.

Register routes BEFORE calling listen().
"""

import asyncio
import inspect
import json
import re

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


def _adapt(fn):
    """Wrap a user function into the flat signature the compiled core calls."""
    is_async = inspect.iscoroutinefunction(fn)
    params = inspect.signature(fn).parameters
    takes_all = any(p.kind is p.VAR_KEYWORD for p in params.values())
    convert = {n: p.annotation for n, p in params.items() if p.annotation in (int, float)}

    def call(method, path, body, headers, query, path_params):
        req = ServerRequest(method, path, body, headers, query, path_params)
        kwargs = {k: v for k, v in req.path_params.items() if takes_all or k in params}
        try:
            for k, typ in convert.items():
                if k in kwargs:
                    kwargs[k] = typ(kwargs[k])
        except ValueError:
            return _to_response(({"error": f"path parameter {k!r} must be {typ.__name__}"}, 422))
        if "request" in params:
            kwargs["request"] = req
        rv = fn(**kwargs)
        if is_async:
            # ponytail: a fresh event loop per request (worker threads have none).
            # Upgrade path: one long-lived loop thread + run_coroutine_threadsafe.
            rv = asyncio.run(rv)
        return _to_response(rv)

    return call


class Server(_Server):
    """httpp.Server plus @route/@get/@post/... decorators."""

    def route(self, path, methods=("GET",)):
        """Flask-style: @app.route("/x", methods=["GET", "POST"])."""
        pattern = _to_pattern(path)

        def register(fn):
            adapted = _adapt(fn)
            for m in methods:
                self.add_route(m, pattern, adapted)
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
