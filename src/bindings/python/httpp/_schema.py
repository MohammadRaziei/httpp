"""JSON Schema and OpenAPI 3.1, built from the same type hints the validators use.

    Item.model_json_schema()          # Pydantic v2's shape: {"properties": ..., "required": [...], "title": ..., "type": "object"}
    app.openapi()                     # the document behind /openapi.json

It runs once (the first time the document is asked for), so it is plain Python, not Cython.
ponytail: no discriminated unions, no computed fields, no `mode="serialization"` schemas.
"""

import dataclasses
import datetime
import decimal
import enum
import inspect
import re
import types
import typing
import uuid

from . import _validate as V
from ._params import openapi_path
from .models import to_jsonable

_NoneType = type(None)
_JSON_TYPE = {str: "string", int: "integer", bool: "boolean", float: "number"}


def _title(name):
    return name.replace("_", " ").title()


def _is_union(origin):
    return origin is typing.Union or (hasattr(types, "UnionType") and origin is types.UnionType)


class Defs:
    """The named schemas (models, enums, dataclasses) met while walking, and how to refer to them."""

    def __init__(self, ref_template):
        self.ref_template = ref_template
        self.schemas = {}
        self.hits = {}  # name -> how many times something referred to it
        self._owner = {}

    def name_for(self, cls):
        name = cls.__name__
        if self._owner.get(name, cls) is not cls:  # two classes with one name: qualify by module
            name = f"{cls.__module__.replace('.', '__')}__{cls.__qualname__.replace('.', '__')}"
        return name

    def ref(self, cls, build):
        name = self.name_for(cls)
        self.hits[name] = self.hits.get(name, 0) + 1
        if name not in self._owner:
            self._owner[name] = cls
            self.schemas[name] = None  # placeholder: a model that refers to itself finds the name taken
            self.schemas[name] = build(name)
        return {"$ref": self.ref_template.format(model=name)}


def _apply(schema, c, kind):
    """Constraints (httpp's Field / Annotated metadata) -> JSON Schema keywords."""
    if c is None:
        return schema
    num = (lambda x: float(x)) if schema.get("type") == "number" else (lambda x: x)
    for attr, key in (("ge", "minimum"), ("gt", "exclusiveMinimum"), ("le", "maximum"), ("lt", "exclusiveMaximum"),
                      ("multiple_of", "multipleOf")):
        value = getattr(c, attr)
        if value is not None and kind == "number":
            schema[key] = num(value)
    if kind == "string":
        for attr, key in (("min_length", "minLength"), ("max_length", "maxLength"), ("pattern", "pattern")):
            if getattr(c, attr) is not None:
                schema[key] = getattr(c, attr)
    if kind == "array":
        for attr, key in (("min_length", "minItems"), ("max_length", "maxItems")):
            if getattr(c, attr) is not None:
                schema[key] = getattr(c, attr)
    return schema


def type_schema(tp, defs, c=None):
    """The JSON Schema of type hint `tp`; named types are added to `defs` and referred to with $ref."""
    origin = typing.get_origin(tp)
    args = typing.get_args(tp)

    if origin is typing.Annotated:
        inner, *meta = args
        merged = {}
        for m in meta:
            merged.update(V.constraints_from_metadata(m))
        if c is not None:
            merged.update({n: getattr(c, n) for n in V.Constraints.__slots__ if getattr(c, n) is not None})
        return type_schema(inner, defs, V.Constraints(**merged) if merged else None)

    if _is_union(origin):
        rest = [a for a in args if a is not _NoneType]
        parts = [type_schema(a, defs, c) for a in rest]
        if len(args) != len(rest):
            parts.append({"type": "null"})
        return parts[0] if len(parts) == 1 else {"anyOf": parts}

    if origin is typing.Literal:
        kinds = {_JSON_TYPE.get(type(v)) for v in args}
        out = {"const": args[0]} if len(args) == 1 else {"enum": list(args)}
        if len(kinds) == 1 and None not in kinds:
            out["type"] = kinds.pop()
        return out

    if tp in (typing.Any, object):
        return {}

    if origin in (list, typing.List, set, typing.Set, frozenset, typing.FrozenSet) or tp in (list, set, frozenset):
        out = {"type": "array"}
        if args:
            out["items"] = type_schema(args[0], defs)
        if (origin or tp) in (set, typing.Set, frozenset, typing.FrozenSet):
            out["uniqueItems"] = True
        return _apply(out, c, "array")

    if origin in (tuple, typing.Tuple) or tp is tuple:
        out = {"type": "array"}
        if args and len(args) == 2 and args[1] is Ellipsis:
            out["items"] = type_schema(args[0], defs)
        elif args:
            out.update({"prefixItems": [type_schema(a, defs) for a in args], "minItems": len(args), "maxItems": len(args)})
        return _apply(out, c, "array")

    if origin in (dict, typing.Dict) or tp is dict:
        out = {"type": "object"}
        if len(args) == 2:
            out["additionalProperties"] = type_schema(args[1], defs)
        return out

    if isinstance(tp, type):
        if issubclass(tp, enum.Enum):
            return defs.ref(tp, lambda name: _enum_schema(tp, name))
        if hasattr(tp, "__httpp_model__"):
            return defs.ref(tp, lambda name: _model_schema(tp, name, defs))
        if V._is_pydantic_model(tp):
            return _pydantic_ref(tp, defs)
        if dataclasses.is_dataclass(tp):
            return defs.ref(tp, lambda name: _dataclass_schema(tp, name, defs))

    scalar = _scalar(tp)
    if scalar is None:
        raise TypeError(f"httpp can't describe values of type {tp!r} in a schema")
    kind = scalar.get("type")
    return _apply(dict(scalar), c, "number" if kind in ("integer", "number") else kind)


def _scalar(tp):
    if tp is bool:
        return {"type": "boolean"}
    if tp is int:
        return {"type": "integer"}
    if tp is float:
        return {"type": "number"}
    if tp is str:
        return {"type": "string"}
    if tp is bytes:
        return {"type": "string", "format": "binary"}
    if tp is _NoneType:
        return {"type": "null"}
    if tp is decimal.Decimal:
        return {"anyOf": [{"type": "number"}, {"type": "string"}]}
    if tp is uuid.UUID:
        return {"type": "string", "format": "uuid"}
    if tp is datetime.datetime:
        return {"type": "string", "format": "date-time"}
    if tp is datetime.date:
        return {"type": "string", "format": "date"}
    if tp is datetime.time:
        return {"type": "string", "format": "time"}
    return None


def _enum_schema(cls, name):
    values = [m.value for m in cls]
    out = {"enum": values, "title": name}
    kinds = {_JSON_TYPE.get(type(v)) for v in values}
    if len(kinds) == 1 and None not in kinds:
        out["type"] = kinds.pop()
    if cls.__doc__ and not cls.__doc__.startswith(("An enumeration", "Create a collection")):
        out["description"] = inspect.cleandoc(cls.__doc__)
    return out


def _json_default(value):
    try:
        return str(value) if isinstance(value, decimal.Decimal) else to_jsonable(value)  # Pydantic writes a Decimal as a string
    except TypeError:
        return V.MISSING


def _field_schema(tp, name, info, defs):
    prop = type_schema(tp, defs, info.constraints)
    prop = dict(prop)
    named = "$ref" in prop or (len(prop.get("anyOf", ())) == 2 and any("$ref" in p for p in prop["anyOf"])
                               and {"type": "null"} in prop["anyOf"])
    if info.title or not named:  # a model / enum already carries its own title, like Pydantic
        prop["title"] = info.title or _title(name)
    if info.description:
        prop["description"] = info.description
    if info.examples:
        prop["examples"] = list(info.examples)
    if info.deprecated:
        prop["deprecated"] = True
    if info.default_factory is None and not info.is_required:
        default = _json_default(info.default)
        if default is not V.MISSING:
            prop["default"] = default
    if isinstance(info.json_schema_extra, dict):
        prop.update(info.json_schema_extra)
    return prop


def _model_schema(cls, name, defs):
    schema = cls._build()
    props, required = {}, []
    for f in schema["fields"]:
        if f.info.exclude:
            continue
        key = f.alias or f.name
        props[key] = _field_schema(f.tp, f.name, f.info, defs)
        if f.info.is_required:
            required.append(key)
    out = {"properties": props, "title": name, "type": "object"}
    if required:
        out["required"] = required
    if schema["config"].get("extra") == "forbid":
        out["additionalProperties"] = False
    doc = cls.__dict__.get("__doc__")
    if doc:
        out["description"] = inspect.cleandoc(doc)
    return out


def _dataclass_schema(cls, name, defs):
    hints = typing.get_type_hints(cls, include_extras=True)
    props, required = {}, []
    for f in dataclasses.fields(cls):
        if not f.init:
            continue
        prop = dict(type_schema(hints[f.name], defs))
        prop["title"] = _title(f.name)
        has_default = f.default is not dataclasses.MISSING
        if has_default and _json_default(f.default) is not V.MISSING:
            prop["default"] = _json_default(f.default)
        if not has_default and f.default_factory is dataclasses.MISSING:
            required.append(f.name)
        props[f.name] = prop
    out = {"properties": props, "title": name, "type": "object"}
    if required:
        out["required"] = required
    return out


def _pydantic_ref(cls, defs):
    """A real pydantic model: ask pydantic, and take its definitions into ours."""
    name = defs.name_for(cls)
    if name not in defs._owner:
        defs._owner[name] = cls
        schema = cls.model_json_schema(ref_template=defs.ref_template)
        for k, v in schema.pop("$defs", {}).items():
            defs.schemas.setdefault(k, v)
        defs.schemas[name] = schema
    return {"$ref": defs.ref_template.format(model=name)}


def model_json_schema(cls, by_alias=True, ref_template="#/$defs/{model}"):
    """BaseModel.model_json_schema(): the model's schema, with the models / enums it uses under "$defs"."""
    if not by_alias:
        raise NotImplementedError("model_json_schema(by_alias=False) is not supported")
    defs = Defs(ref_template)
    ref = defs.ref(cls, lambda name: _model_schema(cls, name, defs))
    name = ref["$ref"].rsplit("/", 1)[-1]
    main = defs.schemas.pop(name)
    if defs.hits[name] > 1:  # something inside refers back to it: it must live in $defs and the top is a $ref
        defs.schemas[name] = main
        main = dict(ref)
    if defs.schemas:
        main["$defs"] = defs.schemas
    return main


# ---------------------------------------------------------------------------------------------------
# OpenAPI

_VALIDATION_SCHEMAS = {
    "ValidationError": {
        "properties": {
            "loc": {"items": {"anyOf": [{"type": "string"}, {"type": "integer"}]}, "type": "array", "title": "Location"},
            "msg": {"type": "string", "title": "Message"},
            "type": {"type": "string", "title": "Error Type"},
        },
        "type": "object", "required": ["loc", "msg", "type"], "title": "ValidationError",
    },
    "HTTPValidationError": {
        "properties": {"detail": {"items": {"$ref": "#/components/schemas/ValidationError"}, "type": "array", "title": "Detail"}},
        "type": "object", "title": "HTTPValidationError",
    },
}


def _param_schema(hint, defs, default=V.MISSING):
    schema = {"type": "string"} if hint is None else dict(type_schema(hint, defs))
    if default is not V.MISSING:
        d = _json_default(default)
        if d is not V.MISSING:
            schema["default"] = d
    return schema


def _operation(route, method, defs):
    fn, plan, meta = route.fn, route.plan, route.meta
    name = getattr(fn, "__name__", "handler")
    path = openapi_path(route.path)
    op = {"summary": meta.get("summary") or _title(name),
          "operationId": name + re.sub(r"\W", "_", path) + "_" + method.lower()}
    doc = meta.get("description") or inspect.getdoc(fn)
    if doc:
        op["description"] = doc
    if meta.get("tags"):
        op["tags"] = list(meta["tags"])
    if meta.get("deprecated"):
        op["deprecated"] = True

    params = [{"name": n, "in": "path", "required": True, "schema": _param_schema(h, defs)} for n, h in plan.path.items()]
    for p in plan.query:
        params.append({"name": p.name, "in": "query", "required": p.required, "schema": _param_schema(p.hint, defs, p.default)})
    if params:
        op["parameters"] = params
    if plan.body is not None:
        op["requestBody"] = {"required": plan.body.required,
                             "content": {"application/json": {"schema": _param_schema(plan.body.hint, defs)}}}

    ok = {"description": "Successful Response"}
    response_model = meta.get("response_model")
    ok["content"] = {"application/json": {"schema": type_schema(response_model, defs) if response_model is not None else {}}}
    op["responses"] = {str(meta.get("status_code") or 200): ok}
    if params or plan.body is not None:
        op["responses"]["422"] = {"description": "Validation Error", "content": {
            "application/json": {"schema": {"$ref": "#/components/schemas/HTTPValidationError"}}}}
    return op, params or plan.body is not None


def build_openapi(routes, *, title, version, description=None):
    """The OpenAPI 3.1 document for `routes` (a list of _params.Route)."""
    defs = Defs("#/components/schemas/{model}")
    paths, uses_422 = {}, False
    for route in routes:
        if route.meta.get("include_in_schema") is False:
            continue
        for method in route.methods:
            op, with_422 = _operation(route, method, defs)
            uses_422 = uses_422 or with_422
            paths.setdefault(openapi_path(route.path), {})[method.lower()] = op
    info = {"title": title, "version": version}
    if description:
        info["description"] = description
    doc = {"openapi": "3.1.0", "info": info, "paths": paths}
    schemas = dict(defs.schemas)
    if uses_422:
        schemas.update(_VALIDATION_SCHEMAS)
    if schemas:
        doc["components"] = {"schemas": schemas}
    return doc
