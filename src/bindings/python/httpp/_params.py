"""Where each argument of a route handler comes from, worked out once when the route is registered.

The rules are FastAPI's: a name that is in the path is a path parameter; any other argument whose hint is a
scalar (int, str, Optional[int], Literal, Enum, ...) or a list / set / tuple of scalars is a query parameter;
anything else (a model, a dataclass, a dict, a list of models) is the JSON request body. `request` is
the request itself. The same plan feeds the request path (server.py) and the OpenAPI document (_schema.py), so
what /docs says is what the server does.

ponytail: one body parameter (no `embed`), no Query()/Header()/Cookie()/Depends() yet, bodies are always JSON.
"""

import inspect
import json
import re
import typing

from . import _validate as V
from .models import FieldInfo

# '/users/{id}', '/users/<id>' and cpp-httplib's own '/users/:id'
PATH_PARAM = re.compile(r"[{<]([^}>]*)[}>]|(?<=/):([A-Za-z_]\w*)")


def path_names(path):
    return {m.group(1) or m.group(2) for m in PATH_PARAM.finditer(path)}


def openapi_path(path):
    return PATH_PARAM.sub(lambda m: "{" + (m.group(1) or m.group(2)) + "}", path)

_NOT_GIVEN = object()


class Param:
    __slots__ = ("name", "hint", "default", "check", "is_seq")

    def __init__(self, name, hint, default):
        self.name = name
        self.hint = hint
        self.default = default  # V.MISSING when the caller must supply it
        self.check = V.compile_type(hint)
        self.is_seq = V.sequence_item_hint(hint) is not None

    @property
    def required(self):
        return self.default is V.MISSING


class Plan:
    __slots__ = ("path", "path_checks", "query", "body", "active")

    def __init__(self):
        self.path = {}         # name -> hint, or None when the handler didn't annotate it
        self.path_checks = {}  # name -> validator, only for hints beyond the int / float / str the server converts itself
        self.query = []
        self.body = None
        self.active = False

    def run(self, raw, kwargs):
        """Fill `kwargs` with the converted query / body values (and re-check path values that need more than
        int / float). Returns the list of errors, empty when the request is fine."""
        errors = []
        for name, check in self.path_checks.items():
            if name in kwargs:
                value = check(kwargs[name], ("path", name), errors)
                if value is not V.INVALID:
                    kwargs[name] = value
        if self.query:
            pairs = raw.query()
            first = {}
            for k, v in pairs:
                first.setdefault(k, v)
            for p in self.query:
                if p.is_seq:
                    value = [v for k, v in pairs if k == p.name] or V.MISSING
                else:
                    value = first.get(p.name, V.MISSING)
                if value is V.MISSING:
                    if p.required:
                        errors.append({"type": "missing", "loc": ("query", p.name), "msg": "Field required", "input": None})
                    continue
                value = p.check(value, ("query", p.name), errors)
                if value is not V.INVALID:
                    kwargs[p.name] = value
        p = self.body
        if p is not None:
            text = raw.body
            if not text:
                if p.required:
                    errors.append({"type": "missing", "loc": ("body",), "msg": "Field required", "input": None})
            else:
                try:
                    data = json.loads(text)
                except ValueError as exc:
                    errors.append({"type": "json_invalid", "loc": ("body", getattr(exc, "pos", 0)), "msg": "JSON decode error",
                                   "input": {}, "ctx": {"error": getattr(exc, "msg", str(exc))}})
                else:
                    value = p.check(data, ("body",), errors)
                    if value is not V.INVALID:
                        kwargs[p.name] = value
        return errors


class Route:
    """What the OpenAPI document needs to know about one registered route."""

    __slots__ = ("path", "methods", "fn", "plan", "meta")

    def __init__(self, path, methods, fn, plan, meta):
        self.path, self.methods, self.fn, self.plan, self.meta = path, methods, fn, plan, meta


def type_hints(fn):
    try:
        return typing.get_type_hints(fn, include_extras=True)
    except Exception:  # a hint that can't be resolved (a class local to a function, say): use the ones that are real types
        return {k: v for k, v in getattr(fn, "__annotations__", {}).items() if not isinstance(v, str)}


def make_plan(fn, path):
    sig = inspect.signature(fn)
    hints = type_hints(fn)
    path_names_ = path_names(path)
    plan = Plan()
    for name, p in sig.parameters.items():
        if p.kind in (p.VAR_POSITIONAL, p.VAR_KEYWORD) or name == "request":
            continue
        hint = hints.get(name, None if p.annotation is p.empty else p.annotation)
        default = V.MISSING if p.default is p.empty else p.default
        if isinstance(default, FieldInfo):
            raise TypeError(f"{fn.__name__}({name}=Field(...)): Field()/Query() as a parameter default is not supported yet; "
                            "use a plain default and Annotated[int, Field(ge=1)] for constraints")
        if name in path_names_:
            plan.path[name] = hint
            if hint not in (None, int, float, str, typing.Any):
                plan.path_checks[name] = V.compile_type(hint)
            continue
        if hint is None:
            hint = typing.Any  # FastAPI: an unannotated argument is a query parameter
        is_seq = V.sequence_item_hint(hint)
        if V.is_scalar_hint(hint) or (is_seq is not None and V.is_scalar_hint(is_seq)):
            plan.query.append(Param(name, hint, default))
        elif plan.body is None:
            plan.body = Param(name, hint, default)
        else:
            raise TypeError(f"{fn.__name__}: only one request body parameter is supported ({plan.body.name!r} and {name!r}); "
                            "wrap them in one model")
    for name in path_names_:
        plan.path.setdefault(name, None)
    plan.active = bool(plan.path_checks or plan.query or plan.body)
    return plan
