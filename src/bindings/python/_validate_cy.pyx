# cython: language_level=3, freethreading_compatible=True
"""The validators that run on every request, compiled. `httpp/_validate.py` walks the type hints once and builds
these objects; nothing here looks at a type hint.

Every validator is a cdef class with one C-level method, `_validate(v, loc, errors)`, which returns the converted
value or INVALID. It runs in two modes:

  * errors is None  ("fast pass"): no location tuples, no messages, no error dicts are made; the first failure
    returns INVALID at once. This is what a valid request pays for.
  * errors is a list ("error pass"): failures are appended as Pydantic-shaped dicts and the walk goes on, so one
    422 can carry every error.

Calling a validator from Python (`check(value, loc, errors)`) runs the fast pass and, only if it failed, the
error pass. Because of that a user-written @field_validator can run twice on an invalid input (once per pass);
it must not have side effects. ponytail: one code path with a flag, not two copies of every validator.
"""

import dataclasses
import datetime
import decimal
import importlib
import math
import re
import typing
import uuid

INVALID = object()  # what a validator returns after it reported (or, in the fast pass, hit) an error
MISSING = object()  # "no default"

cdef object _NoneType = type(None)
cdef object _dict_keys = type({}.keys())
cdef object _dict_values = type({}.values())
cdef object _Decimal = decimal.Decimal
cdef object _UUID = uuid.UUID
cdef object _datetime = datetime.datetime
cdef object _date = datetime.date
cdef object _time = datetime.time
cdef object _models_module = None


cdef inline object fail(list errors, str type_, tuple loc, str msg, object value, object ctx=None):
    if errors is None:
        return INVALID
    cdef dict err = {"type": type_, "loc": loc, "msg": msg, "input": value}
    if ctx:
        err["ctx"] = ctx
    errors.append(err)
    return INVALID


def _fail(errors, type_, loc, msg, value, ctx=None):
    """Python-level twin of `fail`, for models.py."""
    return fail(errors, type_, loc, msg, value, ctx)


cdef object _models():
    global _models_module
    if _models_module is None:
        _models_module = importlib.import_module("httpp.models")
    return _models_module


cdef class Validator:
    cdef object _validate(self, object v, tuple loc, list errors):
        raise NotImplementedError

    def __call__(self, object v, tuple loc, list errors):
        cdef object r = self._validate(v, None, None)
        if r is INVALID and errors is not None:
            r = self._validate(v, loc, errors)
        return r


# ---------------------------------------------------------------------------------------------------
# scalars

cdef object _TRUE = frozenset(("1", "on", "t", "true", "y", "yes"))
cdef object _FALSE = frozenset(("0", "off", "f", "false", "n", "no"))


cdef object _text(object v):
    """str for a str or bytes input, else None."""
    if isinstance(v, str):
        return v
    if isinstance(v, (bytes, bytearray)):
        try:
            return bytes(v).decode("utf-8")
        except UnicodeDecodeError:
            return None
    return None


cdef class AnyV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        return v


cdef class IntV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object t = type(v)
        cdef object s, f
        if t is int:
            return v
        if t is bool:
            return int(v)
        if t is float:
            if math.isfinite(v) and v == int(v):
                return int(v)
            return fail(errors, "int_from_float", loc, "Input should be a valid integer, got a number with a fractional part", v)
        s = _text(v)
        if s is not None:
            s = s.strip()
            try:
                return int(s)
            except ValueError:
                try:
                    f = float(s)
                    if math.isfinite(f) and f == int(f) and "e" not in s.lower() and "_" not in s:
                        return int(f)
                except (ValueError, OverflowError):
                    pass
            return fail(errors, "int_parsing", loc, "Input should be a valid integer, unable to parse string as an integer", v)
        if isinstance(v, _Decimal) and v == v.to_integral_value():
            return int(v)
        return fail(errors, "int_type", loc, "Input should be a valid integer", v)


cdef class FloatV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object t = type(v)
        cdef object s
        if t is float:
            return v
        if t is int or t is bool:
            return float(v)
        s = _text(v)
        if s is not None:
            try:
                return float(s.strip())
            except ValueError:
                return fail(errors, "float_parsing", loc, "Input should be a valid number, unable to parse string as a number", v)
        if isinstance(v, _Decimal):
            return float(v)
        return fail(errors, "float_type", loc, "Input should be a valid number", v)


cdef class BoolV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object t = type(v)
        cdef object s, low
        if t is bool:
            return v
        if t is int or t is float:
            if v == 1:
                return True
            if v == 0:
                return False
            if t is float:
                return fail(errors, "bool_type", loc, "Input should be a valid boolean", v)
            return fail(errors, "bool_parsing", loc, "Input should be a valid boolean, unable to interpret input", v)
        s = _text(v)
        if s is not None:
            low = s.strip().lower()
            if low in _TRUE:
                return True
            if low in _FALSE:
                return False
            return fail(errors, "bool_parsing", loc, "Input should be a valid boolean, unable to interpret input", v)
        return fail(errors, "bool_type", loc, "Input should be a valid boolean", v)


cdef class StrV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object s
        if type(v) is str:
            return v
        s = _text(v)
        if s is not None:
            return s
        return fail(errors, "string_type", loc, "Input should be a valid string", v)


cdef class BytesV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        if isinstance(v, (bytes, bytearray)):
            return bytes(v)
        if isinstance(v, str):
            return v.encode("utf-8")
        return fail(errors, "bytes_type", loc, "Input should be a valid bytes", v)


cdef class NoneV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        if v is None:
            return None
        return fail(errors, "none_required", loc, "Input should be None", v)


cdef class DecimalV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object d
        if isinstance(v, _Decimal):
            return v
        if isinstance(v, bool):
            return fail(errors, "decimal_type", loc, "Decimal input should be an integer, float, string or Decimal object", v)
        if isinstance(v, (int, float, str)):
            try:
                d = _Decimal(str(v).strip())
                if d.is_finite() or isinstance(v, str):
                    return d
            except decimal.InvalidOperation:
                pass
            return fail(errors, "decimal_parsing", loc, "Input should be a valid decimal", v)
        return fail(errors, "decimal_type", loc, "Decimal input should be an integer, float, string or Decimal object", v)


cdef class UuidV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object s
        if isinstance(v, _UUID):
            return v
        s = _text(v)
        if s is not None:
            try:
                return _UUID(s)
            except ValueError:
                return fail(errors, "uuid_parsing", loc, "Input should be a valid UUID, invalid character or length", v,
                            {"error": "invalid character or length"})
        return fail(errors, "uuid_type", loc, "UUID input should be a string, bytes or UUID object", v)


cdef object _date_error(list errors, str kind, tuple loc, object v, str why):
    if errors is None:
        return INVALID
    if kind == "date":
        return fail(errors, "date_from_datetime_parsing", loc, f"Input should be a valid date or datetime, {why}", v, {"error": why})
    return fail(errors, "datetime_from_date_parsing", loc, f"Input should be a valid datetime or date, {why}", v, {"error": why})


cdef tuple _parse_iso(str s):
    """datetime.fromisoformat, tolerant of a trailing Z. Returns (datetime, None) or (None, reason)."""
    s = s.strip()
    if len(s) < 4:
        return None, "input is too short"
    try:
        return _datetime.fromisoformat(s[:-1] + "+00:00" if s[-1:] in "zZ" else s), None
    except ValueError as exc:
        text = str(exc)
        if "month must be in 1..12" in text:
            return None, "month value is outside expected range of 1-12"
        if "day is out of range" in text:
            return None, "day value is outside expected range"
        if "hour must be" in text:
            return None, "hour value is outside expected range of 0-23"
        return None, "invalid character in year"


cdef class DateTimeV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object s, parsed, why
        if isinstance(v, _datetime):
            return v
        if isinstance(v, _date):
            return _datetime(v.year, v.month, v.day)
        s = _text(v)
        if s is not None:
            parsed, why = _parse_iso(s)
            if parsed is None:
                return _date_error(errors, "datetime", loc, v, why)
            return parsed
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            return _datetime.fromtimestamp(v, datetime.timezone.utc)
        return fail(errors, "datetime_type", loc, "Input should be a valid datetime", v)


cdef class DateV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object s, parsed, why
        if isinstance(v, _datetime):
            if v.time() == _time():
                return v.date()
            return fail(errors, "date_from_datetime_inexact", loc, "Datetimes provided to dates should have zero time - e.g. be exact dates", v)
        if isinstance(v, _date):
            return v
        s = _text(v)
        if s is not None:
            parsed, why = _parse_iso(s)
            if parsed is None:
                return _date_error(errors, "date", loc, v, why)
            if parsed.time() != _time():
                return fail(errors, "date_from_datetime_inexact", loc, "Datetimes provided to dates should have zero time - e.g. be exact dates", v)
            return parsed.date()
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            return fail(errors, "date_from_datetime_inexact", loc, "Datetimes provided to dates should have zero time - e.g. be exact dates", v)
        return fail(errors, "date_type", loc, "Input should be a valid date", v)


cdef class TimeV(Validator):
    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object s
        if isinstance(v, _time):
            return v
        s = _text(v)
        if s is not None:
            try:
                return _time.fromisoformat(s.strip())
            except ValueError:
                return fail(errors, "time_parsing", loc, "Input should be in a valid time format, invalid character in hour", v,
                            {"error": "invalid character in hour"})
        return fail(errors, "time_type", loc, "Input should be a valid time", v)


# ---------------------------------------------------------------------------------------------------
# constraints

cdef str _plural(object n, str word):
    return f"{n} {word}" if n == 1 else f"{n} {word}s"


# kind -> (error type, message). The order the checks run in is the order Pydantic reports them in.
cdef dict _BOUND = {
    "multiple_of": "Input should be a multiple of {}",
    "less_than_equal": "Input should be less than or equal to {}",
    "less_than": "Input should be less than {}",
    "greater_than_equal": "Input should be greater than or equal to {}",
    "greater_than": "Input should be greater than {}",
}
cdef dict _BOUND_CTX = {"multiple_of": "multiple_of", "less_than_equal": "le", "less_than": "lt",
                        "greater_than_equal": "ge", "greater_than": "gt"}


cdef object _bound_error(list errors, tuple loc, str kind, object bound, object orig, bint as_float):
    if errors is None:
        return INVALID
    return fail(errors, kind, loc, _BOUND[kind].format(bound), orig,
                {_BOUND_CTX[kind]: float(bound) if as_float else bound})  # Pydantic stores a float field's bounds as floats


cdef class NumericV(Validator):
    cdef Validator inner
    cdef object gt, ge, lt, le, mult
    cdef bint as_float

    def __init__(self, Validator inner, gt, ge, lt, le, multiple_of, bint as_float):
        self.inner = inner
        self.gt, self.ge, self.lt, self.le, self.mult = gt, ge, lt, le, multiple_of
        self.as_float = as_float

    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object orig = v
        v = self.inner._validate(v, loc, errors)
        if v is INVALID:
            return v
        if self.mult is not None and v % self.mult != 0:
            return _bound_error(errors, loc, "multiple_of", self.mult, orig, self.as_float)
        if self.le is not None and not v <= self.le:
            return _bound_error(errors, loc, "less_than_equal", self.le, orig, self.as_float)
        if self.lt is not None and not v < self.lt:
            return _bound_error(errors, loc, "less_than", self.lt, orig, self.as_float)
        if self.ge is not None and not v >= self.ge:
            return _bound_error(errors, loc, "greater_than_equal", self.ge, orig, self.as_float)
        if self.gt is not None and not v > self.gt:
            return _bound_error(errors, loc, "greater_than", self.gt, orig, self.as_float)
        return v


cdef class StrConstraintV(Validator):
    cdef Validator inner
    cdef object lo, hi, pattern, pattern_text
    cdef bint strip

    def __init__(self, Validator inner, lo, hi, strip, pattern_text):
        self.inner = inner
        self.lo, self.hi, self.strip = lo, hi, bool(strip)
        self.pattern_text = pattern_text
        self.pattern = re.compile(pattern_text) if pattern_text is not None else None

    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object orig = v
        v = self.inner._validate(v, loc, errors)
        if v is INVALID:
            return v
        if self.strip:
            v = v.strip()
        if self.lo is not None and len(v) < self.lo:
            if errors is None:
                return INVALID
            return fail(errors, "string_too_short", loc, f"String should have at least {_plural(self.lo, 'character')}", orig,
                        {"min_length": self.lo})
        if self.hi is not None and len(v) > self.hi:
            if errors is None:
                return INVALID
            return fail(errors, "string_too_long", loc, f"String should have at most {_plural(self.hi, 'character')}", orig,
                        {"max_length": self.hi})
        if self.pattern is not None and self.pattern.search(v) is None:
            if errors is None:
                return INVALID
            return fail(errors, "string_pattern_mismatch", loc, f"String should match pattern '{self.pattern_text}'", orig,
                        {"pattern": self.pattern_text})
        return v


cdef class CollectionConstraintV(Validator):
    cdef Validator inner
    cdef object lo, hi
    cdef str label

    def __init__(self, Validator inner, lo, hi, str label):
        self.inner = inner
        self.lo, self.hi, self.label = lo, hi, label

    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object out = self.inner._validate(v, loc, errors)
        cdef Py_ssize_t n
        if out is INVALID:
            return out
        n = len(out)
        if self.lo is not None and n < self.lo:
            if errors is None:
                return INVALID
            return fail(errors, "too_short", loc,
                        f"{self.label} should have at least {_plural(self.lo, 'item')} after validation, not {n}", v,
                        {"field_type": self.label, "min_length": self.lo, "actual_length": n})
        if self.hi is not None and n > self.hi:
            if errors is None:
                return INVALID
            return fail(errors, "too_long", loc,
                        f"{self.label} should have at most {_plural(self.hi, 'item')} after validation, not {n}", v,
                        {"field_type": self.label, "max_length": self.hi, "actual_length": n})
        return out


# ---------------------------------------------------------------------------------------------------
# containers, unions, literals, enums

cdef class ListV(Validator):
    """list / set / frozenset of `item`. kind: 0 list, 1 set, 2 frozenset."""
    cdef Validator item
    cdef int kind
    cdef str label, type_name

    def __init__(self, Validator item, int kind, str label):
        self.item = item
        self.kind = kind
        self.label = label
        self.type_name = ("frozen_set" if label == "frozenset" else label) + "_type"

    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object t = type(v)
        cdef list out
        cdef object x, r
        cdef bint bad = False
        cdef Py_ssize_t i = 0
        if not (t is list or t is tuple or t is set or t is frozenset or t is _dict_keys or t is _dict_values):
            return fail(errors, self.type_name, loc, f"Input should be a valid {self.label}", v)
        out = []
        if errors is None:
            for x in v:
                r = self.item._validate(x, None, None)
                if r is INVALID:
                    return INVALID
                out.append(r)
        else:
            for x in v:
                r = self.item._validate(x, loc + (i,), errors)
                if r is INVALID:
                    bad = True
                else:
                    out.append(r)
                i += 1
            if bad:
                return INVALID
        if self.kind == 0:
            return out
        if self.kind == 1:
            return set(out)
        return frozenset(out)


cdef class TupleV(Validator):
    """Tuple[X, ...] when `variadic` is given, else a fixed Tuple[A, B, ...] of `items`."""
    cdef list items
    cdef Validator variadic

    def __init__(self, items, variadic):
        self.items = list(items) if items is not None else None
        self.variadic = variadic

    cdef object _validate(self, object v, tuple loc, list errors):
        cdef list out = []
        cdef object x, r
        cdef Validator check_i
        cdef bint bad = False
        cdef Py_ssize_t i, n
        if not isinstance(v, (list, tuple)):
            return fail(errors, "tuple_type", loc, "Input should be a valid tuple", v)
        if self.variadic is not None:
            i = 0
            for x in v:
                r = self.variadic._validate(x, None if errors is None else loc + (i,), errors)
                if r is INVALID:
                    if errors is None:
                        return INVALID
                    bad = True
                out.append(r)
                i += 1
            return INVALID if bad else tuple(out)
        n = len(self.items)
        if len(v) > n:
            if errors is None:
                return INVALID
            return fail(errors, "too_long", loc, f"Tuple should have at most {n} items after validation, not {len(v)}", v,
                        {"field_type": "Tuple", "max_length": n, "actual_length": len(v)})
        for i in range(n):
            check_i = self.items[i]
            if i >= len(v):
                if errors is None:
                    return INVALID
                fail(errors, "missing", loc + (i,), "Field required", v)  # Pydantic reports each missing item
                bad = True
                continue
            r = check_i._validate(v[i], None if errors is None else loc + (i,), errors)
            if r is INVALID:
                if errors is None:
                    return INVALID
                bad = True
            out.append(r)
        return INVALID if bad else tuple(out)


cdef class DictV(Validator):
    cdef Validator key, value

    def __init__(self, Validator key, Validator value):
        self.key = key
        self.value = value

    cdef object _validate(self, object v, tuple loc, list errors):
        cdef dict out = {}
        cdef object k, x, k2, x2
        cdef bint bad = False
        if not isinstance(v, dict):
            return fail(errors, "dict_type", loc, "Input should be a valid dictionary", v)
        for k, x in v.items():
            if errors is None:
                k2 = self.key._validate(k, None, None)
                if k2 is INVALID:
                    return INVALID
                x2 = self.value._validate(x, None, None)
                if x2 is INVALID:
                    return INVALID
                out[k2] = x2
            else:
                k2 = self.key._validate(k, loc + (k, "[key]"), errors)
                x2 = self.value._validate(x, loc + (k,), errors)
                if k2 is INVALID or x2 is INVALID:
                    bad = True
                else:
                    out[k2] = x2
        return INVALID if bad else out


cdef class LiteralV(Validator):
    cdef tuple values
    cdef str expected

    def __init__(self, values, str expected):
        self.values = tuple(values)
        self.expected = expected

    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object want
        for want in self.values:
            if v == want and isinstance(v, str) == isinstance(want, str):  # True matches 1, like Pydantic
                return want
        return fail(errors, "literal_error", loc, f"Input should be {self.expected}", v, {"expected": self.expected})


cdef class EnumV(Validator):
    cdef object cls
    cdef str expected

    def __init__(self, cls, str expected):
        self.cls = cls
        self.expected = expected

    cdef object _validate(self, object v, tuple loc, list errors):
        if isinstance(v, self.cls):
            return v
        try:
            return self.cls(v)
        except (ValueError, TypeError):
            return fail(errors, "enum", loc, f"Input should be {self.expected}", v, {"expected": self.expected})


cdef bint _strict_ok(tuple spec, object v):
    """Would `v` already be the type `spec` describes, without any conversion? (Pydantic's smart-union first pass.)
    spec comes from _validate._strict_spec: (0,) never, (1, T) exactly T, (2,) None, (3, values) a literal,
    (4, T) an instance of T."""
    cdef int mode = spec[0]
    cdef object a
    if mode == 1:
        return type(v) is spec[1]
    if mode == 2:
        return v is None
    if mode == 3:
        for a in spec[1]:
            if v == a and type(v) is type(a):
                return True
        return False
    if mode == 4:
        return isinstance(v, spec[1])
    return False


cdef class UnionV(Validator):
    """members: [(label, strict spec, validator)]. Smart mode: a value that already is one of the types wins,
    then each member is tried in order; if none fits, every member's errors are reported."""
    cdef list members

    def __init__(self, members):
        self.members = list(members)

    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object label, spec
        cdef Validator vf
        cdef object r
        cdef list collected, sub
        for label, spec, vf in self.members:
            if _strict_ok(spec, v):
                if errors is None:
                    r = vf._validate(v, None, None)
                else:
                    r = vf._validate(v, loc + (label,), [])
                if r is not INVALID:
                    return r
        if errors is None:
            for label, spec, vf in self.members:
                r = vf._validate(v, None, None)
                if r is not INVALID:
                    return r
            return INVALID
        collected = []
        for label, spec, vf in self.members:
            sub = []
            r = vf._validate(v, loc + (label,), sub)
            if r is not INVALID:
                return r
            collected.extend(sub)
        errors.extend(collected)
        return INVALID


cdef class NullableV(Validator):
    cdef Validator inner

    def __init__(self, Validator inner):
        self.inner = inner

    cdef object _validate(self, object v, tuple loc, list errors):
        if v is None:
            return None
        return self.inner._validate(v, loc, errors)


# ---------------------------------------------------------------------------------------------------
# dataclasses, real pydantic models

cdef class DataclassV(Validator):
    cdef object cls
    cdef object compile_type
    cdef list fields  # [(name, validator, required)], built on first use so classes may refer to each other

    def __init__(self, cls, compile_type):
        self.cls = cls
        self.compile_type = compile_type
        self.fields = None

    cdef object _validate(self, object v, tuple loc, list errors):
        cdef dict kwargs = {}
        cdef bint bad = False
        cdef object name, required
        cdef Validator vf
        cdef object r, hints, f
        cdef list built
        if isinstance(v, self.cls):
            return v
        if not isinstance(v, dict):
            return fail(errors, "dataclass_type", loc, "Input should be a dictionary or an instance of " + self.cls.__name__, v,
                        {"class_name": self.cls.__name__})
        if self.fields is None:
            hints = typing.get_type_hints(self.cls)
            built = []
            for f in dataclasses.fields(self.cls):
                if f.init:
                    built.append((f.name, self.compile_type(hints[f.name]),
                                  MISSING if f.default is dataclasses.MISSING and f.default_factory is dataclasses.MISSING else None))
            self.fields = built
        for name, vf, required in self.fields:
            if name in v:
                r = vf._validate(v[name], None if errors is None else loc + (name,), errors)
                if r is INVALID:
                    if errors is None:
                        return INVALID
                    bad = True
                else:
                    kwargs[name] = r
            elif required is MISSING:
                if errors is None:
                    return INVALID
                fail(errors, "missing", loc + (name,), "Field required", v)
                bad = True
        return INVALID if bad else self.cls(**kwargs)


cdef class PydanticV(Validator):
    """A real pydantic model (if the user has pydantic installed and uses it): let pydantic validate, and
    translate its errors into ours so the 422 looks the same."""
    cdef object cls

    def __init__(self, cls):
        self.cls = cls

    cdef object _validate(self, object v, tuple loc, list errors):
        cdef dict err
        try:
            return self.cls.model_validate(v)
        except Exception as exc:  # pydantic.ValidationError, without importing pydantic
            if not hasattr(exc, "errors"):
                raise
            if errors is None:
                return INVALID
            for e in exc.errors(include_url=False):
                err = {"type": e["type"], "loc": loc + tuple(e["loc"]), "msg": e["msg"], "input": e.get("input")}
                if e.get("ctx"):
                    err["ctx"] = e["ctx"]
                errors.append(err)
            return INVALID


# ---------------------------------------------------------------------------------------------------
# httpp's own models (models.py builds the schema; this is the loop that runs for every request)

cdef class FieldSpec:
    cdef object name, loc_name, info, default
    cdef tuple keys
    cdef Validator validator
    cdef list before, after
    cdef bint required, simple_default, hooked


cdef class ModelV(Validator):
    cdef object cls
    cdef list fields
    cdef list model_before, model_after
    cdef object extra
    cdef bint from_attributes, ready
    cdef object known

    def __init__(self, cls):
        self.cls = cls
        self.ready = False

    cdef int _ensure(self) except -1:
        # built on first use, so models may refer to each other (and to themselves) in their hints
        cdef dict schema = self.cls._build()
        cdef dict config = schema["config"]
        cdef list specs = []
        cdef set known = set()
        cdef FieldSpec spec
        cdef object f, alias, by_name
        by_name = config.get("populate_by_name") or config.get("validate_by_name")
        for f in schema["fields"]:
            spec = FieldSpec()
            alias = f.alias
            spec.name = f.name
            spec.loc_name = alias or f.name
            spec.keys = (alias, f.name) if alias is not None and by_name else (alias,) if alias is not None else (f.name,)
            known.update(spec.keys)
            spec.info = f.info
            spec.required = f.info.is_required
            spec.default = f.info.default
            spec.simple_default = f.info.default_factory is None and not isinstance(f.info.default, (list, dict, set, bytearray))
            spec.validator = f.validator
            spec.before = list(f.before)
            spec.after = list(f.after)
            spec.hooked = bool(spec.before or spec.after)
            specs.append(spec)
        self.model_before = list(schema["model_before"])
        self.model_after = list(schema["model_after"])
        self.extra = config.get("extra", "ignore")
        self.from_attributes = bool(config.get("from_attributes"))
        self.known = frozenset(known)
        self.fields = specs
        self.ready = True  # last, so a concurrent caller never sees half of it
        return 0

    cdef object _model_type_error(self, list errors, tuple loc, object v):
        return fail(errors, "model_type", loc, f"Input should be a valid dictionary or instance of {self.cls.__name__}", v,
                    {"class_name": self.cls.__name__})

    cdef object _validate(self, object v, tuple loc, list errors):
        cdef object cls = self.cls
        cdef dict out = {}
        cdef set provided = set()
        cdef bint bad = False
        cdef FieldSpec f
        cdef object data, key, k, value, info, fn, floc, obj, models
        if isinstance(v, cls):
            return v
        if not self.ready:
            self._ensure()
        if not isinstance(v, dict):
            if self.from_attributes and v is not None and not isinstance(v, (str, bytes, int, float, list, tuple)):
                data = {}
                for f in self.fields:
                    if hasattr(v, f.name):
                        data[f.name] = getattr(v, f.name)
                v = data
            else:
                return self._model_type_error(errors, loc, v)
        data = v
        if self.model_before:
            models = _models()
            for fn in self.model_before:
                data = models._call_validator(fn, cls, data, None)
                if not isinstance(data, dict):
                    return self._model_type_error(errors, loc, data)
        for f in self.fields:
            key = None
            for k in f.keys:
                if k in data:
                    key = k
                    break
            if key is None:
                if f.required:
                    if errors is None:
                        return INVALID
                    fail(errors, "missing", loc + (f.loc_name,), "Field required", data)
                    bad = True
                elif f.simple_default:
                    out[f.name] = f.default
                else:
                    out[f.name] = f.info.get_default()
                continue
            value = data[key]
            floc = None if errors is None else loc + (key,)
            if f.hooked:
                models = _models()
                info = models.ValidationInfo(f.name, out)
                for fn in f.before:
                    value = models._run_validator(fn, cls, value, info, floc, errors)
                    if value is INVALID:
                        break
                if value is not INVALID:
                    value = f.validator._validate(value, floc, errors)
                if value is not INVALID:
                    for fn in f.after:
                        value = models._run_validator(fn, cls, value, info, floc, errors)
                        if value is INVALID:
                            break
            else:
                value = f.validator._validate(value, floc, errors)
            if value is INVALID:
                if errors is None:
                    return INVALID
                bad = True
            else:
                out[f.name] = value
                provided.add(f.name)
        if self.extra != "ignore":
            for k in data:
                if k not in self.known:
                    if self.extra == "forbid":
                        if errors is None:
                            return INVALID
                        fail(errors, "extra_forbidden", loc + (k,), "Extra inputs are not permitted", data[k])
                        bad = True
                    elif self.extra == "allow":
                        out[k] = data[k]
                        provided.add(k)
        if bad:
            return INVALID
        obj = cls.__new__(cls)
        obj.__dict__.update(out)
        object.__setattr__(obj, "_fields_set", provided)
        for fn in self.model_after:
            try:
                obj = fn(obj)
            except AssertionError as exc:
                if errors is None:
                    return INVALID
                return fail(errors, "assertion_error", loc, f"Assertion failed, {exc}" if str(exc) else "Assertion failed", data, {"error": exc})
            except ValueError as exc:
                if errors is None:
                    return INVALID
                return fail(errors, "value_error", loc, f"Value error, {exc}", data, {"error": exc})
        return obj
