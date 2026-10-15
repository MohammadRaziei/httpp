"""Where each argument of a route handler comes from, worked out once when the route is registered.

The rules are FastAPI's: a name that is in the path is a path parameter; any other argument whose hint is a
scalar (int, str, Optional[int], Literal, Enum, ...) or a list / set / tuple of scalars is a query parameter;
anything else (a model, a dataclass, a dict, a list of models) is part of the JSON request body. `request` is
the request itself. Query(), Path(), Header(), Cookie() and Body() (as a default or inside Annotated) override
the guess and add constraints and documentation; Depends() runs another function first and passes its result.
The same plan feeds the request path (server.py) and the OpenAPI document (_schema.py), so what /docs says is what
the server does.

ponytail: bodies are always JSON (no forms or files), a dependency is a plain function / class (no
`security_scopes`), and a *sync* dependency used by an `async def` route runs on the event-loop thread.
"""

import contextlib
import inspect
import json
import re
import typing

from . import _validate as V
from .models import FieldInfo, _copy_default

# '/users/{id}', '/users/<id>' and cpp-httplib's own '/users/:id'
PATH_PARAM = re.compile(r"[{<]([^}>]*)[}>]|(?<=/):([A-Za-z_]\w*)")


def path_names(path):
    return {m.group(1) or m.group(2) for m in PATH_PARAM.finditer(path)}


def openapi_path(path):
    return PATH_PARAM.sub(lambda m: "{" + (m.group(1) or m.group(2)) + "}", path)


# ---------------------------------------------------------------------------------------------------
# the public markers

class ParamInfo(FieldInfo):
    """What Query() / Path() / Header() / Cookie() / Body() remember: a Field() that also says where it comes from."""

    __slots__ = ("kind", "convert_underscores", "embed")

    def __init__(self, kind, default=V.MISSING, *, convert_underscores=True, embed=False, **kw):
        super().__init__(V.MISSING if default is Ellipsis else default, **kw)
        self.kind = kind
        self.convert_underscores = convert_underscores
        self.embed = embed


def Query(default=V.MISSING, **kw):
    """?name=value. `Query(None)` is optional, `Query(...)` / `Query()` required; constraints like Field()."""
    return ParamInfo("query", default, **kw)


def Path(default=V.MISSING, **kw):
    return ParamInfo("path", default, **kw)


def Header(default=V.MISSING, *, convert_underscores=True, **kw):
    """A request header. `user_agent` is read from `User-Agent` unless convert_underscores=False (or an alias is given)."""
    return ParamInfo("header", default, convert_underscores=convert_underscores, **kw)


def Cookie(default=V.MISSING, **kw):
    return ParamInfo("cookie", default, **kw)


def Body(default=V.MISSING, *, embed=False, **kw):
    """Part of the JSON body. One body parameter is the whole body; several (or embed=True) are read from
    an object by name, like FastAPI."""
    return ParamInfo("body", default, embed=embed, **kw)


class Depends:
    """`def f(db = Depends(get_db))` or `db: Annotated[Db, Depends(get_db)]`: call get_db first (with its own
    parameters resolved the same way) and pass the result. A function that `yield`s is cleaned up after the
    handler returns, also when it raised. The result is shared by everything that needs it in one request unless
    use_cache=False. `Depends()` alone uses the annotation (a class) as the dependency."""

    __slots__ = ("dependency", "use_cache")

    def __init__(self, dependency=None, *, use_cache=True):
        self.dependency = dependency
        self.use_cache = use_cache


# ---------------------------------------------------------------------------------------------------
# the plan

class Param:
    """A query / header / cookie / body parameter."""

    __slots__ = ("name", "kind", "hint", "default", "info", "check", "is_seq", "wire")

    def __init__(self, name, kind, hint, default, info):
        self.name = name
        self.kind = kind
        self.hint = hint
        self.info = info
        self.default = default  # V.MISSING when the caller must supply it
        self.check = V.compile_type(hint, info.constraints if info is not None else None)
        self.is_seq = V.sequence_item_hint(hint) is not None
        wire = (info.alias if info is not None and info.alias else None) or name
        if kind == "header":
            if info is None or (info.convert_underscores and not info.alias):
                wire = wire.replace("_", "-")
            wire = wire.lower()
        self.wire = wire

    @property
    def required(self):
        return self.default is V.MISSING and (self.info is None or self.info.default_factory is None)

    def default_value(self):
        if self.info is not None and self.info.default_factory is not None:
            return self.info.default_factory()
        return _copy_default(self.default)


class Plan:
    __slots__ = ("path", "path_info", "path_checks", "path_from_raw", "params", "body", "embed", "deps", "wants_request", "active")

    def __init__(self):
        self.path = {}         # name -> hint, or None when the handler didn't annotate it
        self.path_info = {}    # name -> Path(...) when given
        self.path_checks = {}  # name -> validator (for a route only what the server doesn't convert itself)
        self.path_from_raw = False  # a dependency reads its path parameters itself
        self.params = []
        self.body = []
        self.embed = False
        self.deps = []         # [(argument name, Dependency)]
        self.wants_request = False
        self.active = False

    def run(self, raw, kwargs):
        """Fill `kwargs` with the converted query / header / cookie / body values (and re-check path values).
        Returns the list of errors, empty when the request is fine."""
        errors = []
        if self.path_from_raw:
            found = raw.path_params()
            for name in self.path:
                if name in found:
                    kwargs[name] = found[name]
        for name, check in self.path_checks.items():
            if name in kwargs:
                value = check(kwargs[name], ("path", name), errors)
                if value is not V.INVALID:
                    kwargs[name] = value
        sources = {}
        for p in self.params:
            src = sources.get(p.kind)
            if src is None:
                src = sources[p.kind] = _source(raw, p.kind)
            pairs, first = src
            if p.is_seq:
                value = [v for k, v in pairs if k == p.wire] or V.MISSING
            else:
                value = first.get(p.wire, V.MISSING)
            if value is V.MISSING:
                if p.required:
                    errors.append({"type": "missing", "loc": (p.kind, p.name if p.kind != "header" else p.wire),
                                   "msg": "Field required", "input": None})
                else:
                    kwargs[p.name] = p.default_value()
                continue
            value = p.check(value, (p.kind, p.name if p.kind != "header" else p.wire), errors)
            if value is not V.INVALID:
                kwargs[p.name] = value
        if self.body:
            self._run_body(raw, kwargs, errors)
        return errors

    def _run_body(self, raw, kwargs, errors):
        text = raw.body
        if not text:
            for p in self.body:
                if p.required:
                    errors.append({"type": "missing", "loc": ("body", p.name) if self.embed else ("body",),
                                   "msg": "Field required", "input": None})
                else:
                    kwargs[p.name] = p.default_value()
            return
        try:
            data = json.loads(text)
        except ValueError as exc:
            errors.append({"type": "json_invalid", "loc": ("body", getattr(exc, "pos", 0)), "msg": "JSON decode error",
                           "input": {}, "ctx": {"error": getattr(exc, "msg", str(exc))}})
            return
        if not self.embed:
            p = self.body[0]
            value = p.check(data, ("body",), errors)
            if value is not V.INVALID:
                kwargs[p.name] = value
            return
        if not isinstance(data, dict):
            errors.append({"type": "model_attributes_type", "loc": ("body",), "input": data,
                           "msg": "Input should be a valid dictionary or object to extract fields from"})
            return
        for p in self.body:
            key = p.info.alias if p.info is not None and p.info.alias else p.name
            if key not in data:
                if p.required:
                    errors.append({"type": "missing", "loc": ("body", key), "msg": "Field required", "input": None})
                else:
                    kwargs[p.name] = p.default_value()
                continue
            value = p.check(data[key], ("body", key), errors)
            if value is not V.INVALID:
                kwargs[p.name] = value

    def walk(self):
        """This plan and the plans of every dependency below it."""
        yield self
        for _, dep in self.deps:
            yield from dep.plan.walk()


def _source(raw, kind):
    if kind == "query":
        pairs = raw.query()
    elif kind == "header":
        pairs = [(k.lower(), v) for k, v in raw.headers()]
    else:
        pairs = _cookies(raw)
    first = {}
    for k, v in pairs:
        first.setdefault(k, v)
    return pairs, first


def _cookies(raw):
    out = []
    for k, v in raw.headers():
        if k.lower() == "cookie":
            for part in v.split(";"):
                name, eq, value = part.strip().partition("=")
                if eq and name:
                    out.append((name, value[1:-1] if len(value) > 1 and value[0] == value[-1] == '"' else value))
    return out


# ---------------------------------------------------------------------------------------------------
# dependencies

class Ctx:
    """What one request shares between its dependencies: a cache, the stack that cleans up `yield` ones, the request."""

    __slots__ = ("raw", "cache", "stack", "_make_request", "_request")

    def __init__(self, raw, stack, make_request):
        self.raw = raw
        self.cache = {}
        self.stack = stack
        self._make_request = make_request
        self._request = None

    def request(self):
        if self._request is None:
            self._request = self._make_request(self.raw)
        return self._request


class Dependency:
    __slots__ = ("fn", "plan", "use_cache", "kind")

    def __init__(self, target, path, use_cache):
        self.fn = target
        self.use_cache = use_cache
        if inspect.isasyncgenfunction(target):
            self.kind = "asyncgen"
        elif inspect.isgeneratorfunction(target):
            self.kind = "gen"
        elif inspect.iscoroutinefunction(target) or (not isinstance(target, type) and inspect.iscoroutinefunction(getattr(target, "__call__", None))):
            self.kind = "async"
        else:
            self.kind = "sync"
        self.plan = make_plan(target, path, top=False)

    def _inputs(self, ctx, errors):
        kw = {}
        errors.extend(self.plan.run(ctx.raw, kw))
        return kw

    def solve(self, ctx, errors):
        """Run the dependency (sync); INVALID, with `errors` extended, when its parameters are bad."""
        if self.use_cache and self.fn in ctx.cache:
            return ctx.cache[self.fn]
        n = len(errors)
        kw = self._inputs(ctx, errors)
        for name, dep in self.plan.deps:
            value = dep.solve(ctx, errors)
            if value is not V.INVALID:
                kw[name] = value
        if len(errors) > n:
            if self.use_cache:
                ctx.cache[self.fn] = V.INVALID  # a second user of it must not report the same errors again
            return V.INVALID
        if self.plan.wants_request:
            kw["request"] = ctx.request()
        if self.kind == "gen":
            value = ctx.stack.enter_context(contextlib.contextmanager(self.fn)(**kw))
        else:
            value = self.fn(**kw)
        if self.use_cache:
            ctx.cache[self.fn] = value
        return value

    async def asolve(self, ctx, errors):
        """The same, for `async def` routes: async dependencies are awaited."""
        if self.use_cache and self.fn in ctx.cache:
            return ctx.cache[self.fn]
        n = len(errors)
        kw = self._inputs(ctx, errors)
        for name, dep in self.plan.deps:
            value = await dep.asolve(ctx, errors)
            if value is not V.INVALID:
                kw[name] = value
        if len(errors) > n:
            if self.use_cache:
                ctx.cache[self.fn] = V.INVALID  # a second user of it must not report the same errors again
            return V.INVALID
        if self.plan.wants_request:
            kw["request"] = ctx.request()
        if self.kind == "asyncgen":
            value = await ctx.stack.enter_async_context(contextlib.asynccontextmanager(self.fn)(**kw))
        elif self.kind == "gen":
            value = ctx.stack.enter_context(contextlib.contextmanager(self.fn)(**kw))
        elif self.kind == "async":
            value = await self.fn(**kw)
        else:
            value = self.fn(**kw)
        if self.use_cache:
            ctx.cache[self.fn] = value
        return value


def solve_deps(plan, ctx, kwargs):
    errors = []
    for name, dep in plan.deps:
        value = dep.solve(ctx, errors)
        if value is not V.INVALID:
            kwargs[name] = value
    return errors


async def asolve_deps(plan, ctx, kwargs):
    errors = []
    for name, dep in plan.deps:
        value = await dep.asolve(ctx, errors)
        if value is not V.INVALID:
            kwargs[name] = value
    return errors


class Route:
    """What the OpenAPI document needs to know about one registered route."""

    __slots__ = ("path", "methods", "fn", "plan", "meta")

    def __init__(self, path, methods, fn, plan, meta):
        self.path, self.methods, self.fn, self.plan, self.meta = path, methods, fn, plan, meta


# ---------------------------------------------------------------------------------------------------
# building a plan

def type_hints(fn):
    target = fn.__init__ if isinstance(fn, type) else fn
    try:
        return typing.get_type_hints(target, include_extras=True)
    except Exception:  # a hint that can't be resolved (a class local to a function, say): use the ones that are real types
        return {k: v for k, v in getattr(target, "__annotations__", {}).items() if not isinstance(v, str)}


def _markers(hint, default):
    """(info, depends, the default value) from a parameter's default and its Annotated metadata."""
    info = dep = None
    if typing.get_origin(hint) is typing.Annotated:
        for m in typing.get_args(hint)[1:]:
            if isinstance(m, Depends):
                dep = m
            elif isinstance(m, ParamInfo):
                info = m
    if isinstance(default, Depends):
        dep, default = default, V.MISSING
    elif isinstance(default, ParamInfo):
        info, default = default, default.default
    elif isinstance(default, FieldInfo):
        raise TypeError("Field() as a parameter default is not supported; use Query() / Body() / ..., or "
                        "Annotated[int, Field(ge=1)] with a plain default")
    elif info is not None and default is V.MISSING:
        default = info.default
    return info, dep, default


def make_plan(fn, path, top=True):
    """The plan for handler `fn` (top=True) or for a dependency of it (top=False)."""
    sig = inspect.signature(fn)
    hints = type_hints(fn)
    names = path_names(path)
    plan = Plan()
    plan.path_from_raw = not top
    for name, p in sig.parameters.items():
        if p.kind in (p.VAR_POSITIONAL, p.VAR_KEYWORD):
            continue
        if name == "request":
            plan.wants_request = True
            continue
        hint = hints.get(name, None if p.annotation is p.empty else p.annotation)
        info, dep, default = _markers(hint, V.MISSING if p.default is p.empty else p.default)
        where = f"{getattr(fn, '__name__', fn)}({name})"

        if dep is not None:
            target = dep.dependency
            if target is None:
                base = typing.get_args(hint)[0] if typing.get_origin(hint) is typing.Annotated else hint
                if not isinstance(base, type):
                    raise TypeError(f"{where}: Depends() without an argument needs a class as the annotation")
                target = base
            plan.deps.append((name, Dependency(target, path, dep.use_cache)))
            continue

        kind = info.kind if info is not None else None
        if name in names:
            if kind not in (None, "path"):
                raise TypeError(f"{where}: {name!r} is in the path, so it can't be a {kind} parameter")
            plan.path[name] = hint
            if info is not None:
                plan.path_info[name] = info
            if not top or info is not None or hint not in (None, int, float, str, typing.Any):
                plan.path_checks[name] = V.compile_type(str if hint is None else hint, info.constraints if info is not None else None)
            continue
        if kind == "path":
            raise TypeError(f"{where}: Path() used but {name!r} is not in the route path {path!r}")
        if hint is None:
            hint = typing.Any  # FastAPI: an unannotated argument is a query parameter
        if kind is None:
            base = typing.get_args(hint)[0] if typing.get_origin(hint) is typing.Annotated else hint
            item = V.sequence_item_hint(base)
            kind = "query" if V.is_scalar_hint(base) or (item is not None and V.is_scalar_hint(item)) else "body"
        param = Param(name, kind, hint, default, info)
        (plan.body if kind == "body" else plan.params).append(param)
    for name in names:
        plan.path.setdefault(name, None)
    plan.embed = len(plan.body) > 1 or any(p.info is not None and p.info.embed for p in plan.body)
    plan.active = bool(plan.path_checks or plan.params or plan.body or plan.path_from_raw)
    return plan
