"""A small Pydantic-v2-shaped model layer, so FastAPI-style code (request bodies, response models) runs
on httpp without Pydantic installed.

    from httpp.models import BaseModel, Field

    class Item(BaseModel):
        name: str = Field(min_length=1)
        price: float = Field(gt=0)
        tags: list[str] = []

    item = Item(name="tea", price="2.5")          # converts like Pydantic's lax mode
    item.model_dump()                              # {'name': 'tea', 'price': 2.5, 'tags': []}
    Item.model_validate({"name": ""})              # raises ValidationError

Supported: field types and constraints (see _validate.py), defaults and default_factory, aliases,
nested models, Optional/Union/Literal/Enum/containers/dataclasses, @field_validator (before/after),
@model_validator (before/after), model_config (extra, populate_by_name, from_attributes,
str_strip_whitespace), model_dump / model_dump_json / model_validate / model_validate_json /
model_copy / model_json_schema. A real pydantic model also works wherever a model is accepted.

Not supported: validate_assignment, computed fields, private attributes, custom serializers, strict
mode, generic models. Say so in an issue if you need one.
"""

import copy
import dataclasses
import datetime
import decimal
import enum
import json
import sys
import typing
import uuid

from . import _validate as V

__all__ = ["BaseModel", "Field", "FieldInfo", "ValidationError", "ConfigDict", "field_validator", "model_validator",
           "to_jsonable"]

MISSING = V.MISSING


class FieldInfo:
    """What Field() / Query() / Path() / ... remember about one field."""

    __slots__ = ("default", "default_factory", "alias", "title", "description", "examples", "deprecated",
                 "constraints", "exclude", "json_schema_extra", "annotation", "validation_alias")

    def __init__(self, default=MISSING, *, default_factory=None, alias=None, validation_alias=None, title=None,
                 description=None, examples=None, deprecated=None, exclude=False, json_schema_extra=None,
                 gt=None, ge=None, lt=None, le=None, multiple_of=None, min_length=None, max_length=None,
                 pattern=None, regex=None, strip_whitespace=None):
        if default is not MISSING and default_factory is not None:
            raise TypeError("cannot specify both default and default_factory")
        self.default = default
        self.default_factory = default_factory
        self.alias = alias
        self.validation_alias = validation_alias
        self.title = title
        self.description = description
        self.examples = examples
        self.deprecated = deprecated
        self.exclude = exclude
        self.json_schema_extra = json_schema_extra
        self.annotation = None
        self.constraints = V.Constraints(
            gt=gt, ge=ge, lt=lt, le=le, multiple_of=multiple_of, min_length=min_length, max_length=max_length,
            pattern=pattern if pattern is not None else regex, strip_whitespace=strip_whitespace)

    @property
    def is_required(self):
        return self.default is MISSING and self.default_factory is None

    def get_default(self):
        if self.default_factory is not None:
            return self.default_factory()
        return _copy_default(self.default)

    def __repr__(self):
        return f"FieldInfo(default={self.default!r})"


def Field(default=MISSING, **kwargs):
    """Like pydantic.Field: defaults, aliases, constraints (gt ge lt le multiple_of min_length max_length
    pattern) and documentation (title, description, examples)."""
    if default is Ellipsis:
        default = MISSING
    return FieldInfo(default, **kwargs)


def _copy_default(value):
    # Pydantic copies a mutable default for each instance, so `tags: list = []` is not shared
    if isinstance(value, (list, dict, set, bytearray)):
        return copy.deepcopy(value)
    return value


def ConfigDict(**kwargs):
    return dict(kwargs)


# ---------------------------------------------------------------------------------------------------
# errors

class ValidationError(ValueError):
    """Raised when a model can't be built from the data. `errors()` is a list of Pydantic-shaped dicts."""

    def __init__(self, title, errors):
        self.title = title
        self._errors = errors
        super().__init__(str(self))

    def errors(self, *, include_url=False, include_context=True, include_input=True):
        out = []
        for e in self._errors:
            item = {"type": e["type"], "loc": tuple(e["loc"]), "msg": e["msg"]}
            if include_input:
                item["input"] = e.get("input")
            if include_context and e.get("ctx"):
                item["ctx"] = e["ctx"]
            out.append(item)
        return out

    def error_count(self):
        return len(self._errors)

    def json(self, *, indent=None):
        return json.dumps(to_jsonable(self.errors()), indent=indent)

    def __str__(self):
        n = len(self._errors)
        lines = [f"{n} validation error{'s' if n != 1 else ''} for {self.title}"]
        for e in self._errors:
            loc = ".".join(str(p) for p in e["loc"])
            lines.append(loc)
            shown = repr(e.get("input"))
            if len(shown) > 50:
                shown = shown[:47] + "..."
            lines.append(f"  {e['msg']} [type={e['type']}, input_value={shown}, input_type={type(e.get('input')).__name__}]")
        return "\n".join(lines)


# ---------------------------------------------------------------------------------------------------
# validators

def field_validator(*fields, mode="after", check_fields=True):
    """@field_validator("a", "b"): a classmethod `(cls, value)` or `(cls, value, info)` that returns the
    (possibly changed) value or raises ValueError / AssertionError. mode="before" runs before the type check."""
    if mode not in ("before", "after"):
        raise ValueError("field_validator mode must be 'before' or 'after'")
    if not fields or not all(isinstance(f, str) for f in fields):
        raise TypeError("field_validator takes field names")

    def mark(fn):
        fn = fn.__func__ if isinstance(fn, (classmethod, staticmethod)) else fn
        fn.__httpp_validator__ = ("field", fields, mode)
        return classmethod(fn)

    return mark


def model_validator(*, mode="after"):
    """@model_validator(mode="before") on a classmethod `(cls, data)` -> data, or mode="after" on a method
    `(self)` -> self, run once the whole model is built."""
    if mode not in ("before", "after"):
        raise ValueError("model_validator mode must be 'before' or 'after'")

    def mark(fn):
        raw = fn.__func__ if isinstance(fn, (classmethod, staticmethod)) else fn
        raw.__httpp_validator__ = ("model", (), mode)
        return classmethod(raw) if mode == "before" else raw

    return mark


class ValidationInfo:
    __slots__ = ("field_name", "data")

    def __init__(self, field_name, data):
        self.field_name = field_name
        self.data = data


_TAKES_INFO = {}


def _call_validator(fn, owner, value, info):
    key = (fn, owner is not None)
    takes_info = _TAKES_INFO.get(key)
    if takes_info is None:
        import inspect

        try:
            takes_info = len(inspect.signature(fn).parameters) >= (3 if owner is not None else 2)
        except (TypeError, ValueError):
            takes_info = False
        _TAKES_INFO[key] = takes_info
    args = ((owner,) if owner is not None else ()) + (value,) + ((info,) if takes_info else ())
    return fn(*args)


def _run_validator(fn, owner, value, info, loc, errors):
    try:
        return _call_validator(fn, owner, value, info)
    except AssertionError as exc:
        if errors is None:
            return V.INVALID
        msg = str(exc)
        V._fail(errors, "assertion_error", loc, f"Assertion failed, {msg}" if msg else "Assertion failed", value, {"error": exc})
    except ValueError as exc:
        if errors is None:
            return V.INVALID
        if isinstance(exc, ValidationError):
            for e in exc.errors():
                errors.append({**e, "loc": loc + tuple(e["loc"])})
        else:
            V._fail(errors, "value_error", loc, f"Value error, {exc}", value, {"error": exc})
    return V.INVALID


# ---------------------------------------------------------------------------------------------------
# the model

class _Field:
    __slots__ = ("name", "alias", "info", "tp", "validator", "before", "after")


class _ModelMeta(type):
    def __new__(mcls, name, bases, ns, **kwargs):
        cls = super().__new__(mcls, name, bases, ns, **kwargs)
        cls.__httpp_model__ = True
        cls._schema = None
        # A model defined inside a function can refer to names of that function (and to itself) in its
        # hints, which typing.get_type_hints can't see later; keep them, like Pydantic does.
        frame = sys._getframe(1)
        cls._local_names = dict(frame.f_locals) if frame.f_locals is not frame.f_globals else {}
        return cls

    def __repr__(cls):
        return f"<class '{cls.__module__}.{cls.__qualname__}'>"

    @property
    def model_fields(cls):
        """{field name: FieldInfo}, like Pydantic's."""
        return {f.name: f.info for f in cls._build()["fields"]}


class BaseModel(metaclass=_ModelMeta):
    model_config = {}

    # ---- building the schema, once, the first time it is needed (forward references can resolve by then)

    @classmethod
    def _build(cls):
        if cls.__dict__.get("_schema") is not None:
            return cls._schema
        try:
            local = {}
            for klass in reversed(cls.__mro__):
                local.update(getattr(klass, "_local_names", None) or {})
                local[klass.__name__] = klass
            hints = typing.get_type_hints(cls, localns=local, include_extras=True)
        except NameError as exc:
            raise TypeError(f"{cls.__name__}: can't resolve a type hint ({exc}); define the referenced class first") from exc
        config = {}
        for klass in reversed(cls.__mro__):
            config.update(getattr(klass, "model_config", None) or {})

        validators = {"field": [], "model_before": [], "model_after": []}
        for klass in reversed(cls.__mro__):
            for attr, value in vars(klass).items():
                fn = value.__func__ if isinstance(value, (classmethod, staticmethod)) else value
                mark = getattr(fn, "__httpp_validator__", None)
                if mark is None:
                    continue
                kind, fields, mode = mark
                if kind == "field":
                    validators["field"].append((fields, mode, fn))
                elif mode == "before":
                    validators["model_before"].append(fn)
                else:
                    validators["model_after"].append(fn)

        fields = []
        for name, tp in hints.items():
            if name.startswith("_") or name in ("model_config",) or typing.get_origin(tp) is typing.ClassVar:
                continue
            default = MISSING
            for klass in cls.__mro__:
                if name in vars(klass):
                    default = vars(klass)[name]
                    break
            info = default if isinstance(default, FieldInfo) else FieldInfo(default)
            info.annotation = tp
            f = _Field()
            f.name = name
            f.alias = next((a for a in (info.alias, info.validation_alias) if isinstance(a, str)), None)
            f.info = info
            f.tp = tp
            strip = config.get("str_strip_whitespace")
            if strip and tp is str:
                info.constraints.strip_whitespace = True
            f.validator = V.compile_type(tp, info.constraints)
            f.before = [fn for fields_, mode, fn in validators["field"] if mode == "before" and name in fields_]
            f.after = [fn for fields_, mode, fn in validators["field"] if mode == "after" and name in fields_]
            fields.append(f)
        cls._schema = {"fields": fields, "config": config, "model_before": validators["model_before"],
                       "model_after": validators["model_after"]}
        return cls._schema

    @classmethod
    def _validator(cls):
        """The compiled validator for this model (see _validate.compile_type); its schema is built on first use."""
        cached = cls.__dict__.get("_compiled_check")
        if cached is None:
            cached = V.C.ModelV(cls)
            type.__setattr__(cls, "_compiled_check", cached)
        return cached

    # ---- public API

    def __init__(self, **data):
        errors = []
        built = type(self)._validator()(data, (), errors)
        if built is V.INVALID:
            raise ValidationError(type(self).__name__, errors)
        self.__dict__.update(built.__dict__)

    @classmethod
    def model_validate(cls, obj, *, from_attributes=None):
        if from_attributes and not isinstance(obj, (dict, cls)):
            obj = {f.name: getattr(obj, f.name) for f in cls._build()["fields"] if hasattr(obj, f.name)}
        errors = []
        built = cls._validator()(obj, (), errors)
        if built is V.INVALID:
            raise ValidationError(cls.__name__, errors)
        return built

    @classmethod
    def model_validate_json(cls, data):
        try:
            obj = json.loads(data)
        except ValueError as exc:
            raise ValidationError(cls.__name__, [{"type": "json_invalid", "loc": (), "msg": "Invalid JSON: " + str(exc),
                                                  "input": data, "ctx": {"error": str(exc)}}])
        return cls.model_validate(obj)

    @property
    def model_fields_set(self):
        return set(self.__dict__.get("_fields_set", ()))

    def model_dump(self, *, mode="python", include=None, exclude=None, by_alias=False, exclude_unset=False,
                   exclude_defaults=False, exclude_none=False):
        out = {}
        schema = type(self)._build()
        fields_set = self.__dict__.get("_fields_set")
        for f in schema["fields"]:
            name = f.name
            if name not in self.__dict__ or f.info.exclude:
                continue
            if include is not None and name not in include:
                continue
            if exclude is not None and name in exclude:
                continue
            value = self.__dict__[name]
            if exclude_unset and fields_set is not None and name not in fields_set:
                continue
            if exclude_defaults and not f.info.is_required and value == f.info.get_default():
                continue
            if exclude_none and value is None:
                continue
            out[(f.alias if by_alias and f.alias else name)] = _dump(value, mode, by_alias, exclude_none, exclude_unset, exclude_defaults)
        if schema["config"].get("extra") == "allow":
            known = {f.name for f in schema["fields"]}
            for k, v in self.__dict__.items():
                if k in known or k.startswith("_"):
                    continue
                if (include is not None and k not in include) or (exclude is not None and k in exclude):
                    continue
                if exclude_none and v is None:
                    continue
                out[k] = _dump(v, mode, by_alias, exclude_none, exclude_unset, exclude_defaults)
        return out

    def model_dump_json(self, *, indent=None, **kw):
        return json.dumps(self.model_dump(mode="json", **kw), indent=indent, ensure_ascii=False)

    def model_copy(self, *, update=None, deep=False):
        new = copy.deepcopy(self) if deep else copy.copy(self)
        new.__dict__ = dict(self.__dict__)
        if update:
            new.__dict__.update(update)
            new.__dict__["_fields_set"] = set(self.__dict__.get("_fields_set", ())) | set(update)
        return new

    @classmethod
    def model_json_schema(cls, **kwargs):
        raise NotImplementedError("model_json_schema() is not written yet (see handoff.md)")

    # Pydantic v1 names, still common in FastAPI code
    def dict(self, **kw):
        return self.model_dump(**kw)

    def json(self, **kw):
        return self.model_dump_json(**kw)

    @classmethod
    def parse_obj(cls, obj):
        return cls.model_validate(obj)

    @classmethod
    def from_orm(cls, obj):
        return cls.model_validate(obj, from_attributes=True)

    def copy(self, *, update=None, deep=False):
        return self.model_copy(update=update, deep=deep)

    def __iter__(self):
        for name, value in self.__dict__.items():
            if not name.startswith("_"):
                yield name, value

    def __eq__(self, other):
        return type(other) is type(self) and self._public() == other._public()

    def _public(self):
        return {k: v for k, v in self.__dict__.items() if not k.startswith("_")}

    def __repr__(self):
        return f"{type(self).__name__}(" + ", ".join(f"{k}={v!r}" for k, v in self._public().items()) + ")"

    __hash__ = None


# ---------------------------------------------------------------------------------------------------
# serialization

def _dump(value, mode, by_alias, exclude_none, exclude_unset, exclude_defaults):
    if isinstance(value, BaseModel):
        return value.model_dump(mode=mode, by_alias=by_alias, exclude_none=exclude_none, exclude_unset=exclude_unset,
                                exclude_defaults=exclude_defaults)
    if isinstance(value, (list, tuple, set, frozenset)):
        items = [_dump(v, mode, by_alias, exclude_none, exclude_unset, exclude_defaults) for v in value]
        if mode == "json" or isinstance(value, list):
            return items
        try:
            return type(value)(items)
        except TypeError:  # e.g. a set of dumped models: dicts can't be set members
            return items
    if isinstance(value, dict):
        return {(_key(k, mode)): _dump(v, mode, by_alias, exclude_none, exclude_unset, exclude_defaults) for k, v in value.items()}
    if mode == "json":
        return to_jsonable(value)
    if hasattr(value, "model_dump") and callable(value.model_dump):
        return value.model_dump()
    return value


def _key(k, mode):
    if mode == "json" and not isinstance(k, (str, int, float, bool)) and k is not None:
        return str(to_jsonable(k))
    return k


def to_jsonable(value):
    """Anything a route may return -> plain JSON data (what FastAPI's jsonable_encoder does)."""
    if value is None or isinstance(value, (str, int, float, bool)):
        if isinstance(value, float) and (value != value or value in (float("inf"), float("-inf"))):
            return None
        return value
    if isinstance(value, BaseModel):
        return value.model_dump(mode="json")
    if hasattr(value, "model_dump") and callable(value.model_dump):  # a pydantic model
        return value.model_dump(mode="json")
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        return {f.name: to_jsonable(getattr(value, f.name)) for f in dataclasses.fields(value)}
    if isinstance(value, dict):
        return {(k if isinstance(k, (str, int, float, bool)) or k is None else str(to_jsonable(k))): to_jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple, set, frozenset)):
        return [to_jsonable(v) for v in value]
    if isinstance(value, enum.Enum):
        return to_jsonable(value.value)
    if isinstance(value, (datetime.datetime, datetime.date, datetime.time)):
        return value.isoformat()
    if isinstance(value, datetime.timedelta):
        return value.total_seconds()
    if isinstance(value, decimal.Decimal):
        return int(value) if value == value.to_integral_value() else float(value)
    if isinstance(value, (uuid.UUID, bytes)):
        return str(value) if isinstance(value, uuid.UUID) else value.decode("utf-8", errors="replace")
    if isinstance(value, (bytearray, memoryview)):
        return bytes(value).decode("utf-8", errors="replace")
    if hasattr(value, "__fspath__"):
        return str(value)
    raise TypeError(f"Object of type {type(value).__name__} is not JSON serializable")
