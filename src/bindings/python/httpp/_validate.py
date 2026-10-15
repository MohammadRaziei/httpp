"""Turns a type hint into a validator, once, so checking a request costs a few C calls, not a type walk.

    check = compile_type(List[int])
    errors = []
    value = check(["1", "2"], ("query", "ids"), errors)   # -> [1, 2]; failures are appended to `errors`

This module only walks the hints (at registration time). The validators it returns are compiled objects from
`_validate_cy.pyx` (see its docstring for the fast pass / error pass). A validator returns the converted value,
or INVALID after appending errors shaped like Pydantic v2's: {"type", "loc", "msg", "input", ("ctx")}. The
coercion rules are Pydantic's "lax" mode, which is what FastAPI uses: "5" is an int, 1 is a bool, a list accepts a
tuple or a set, and so on. Differences from Pydantic are listed in tests/python/test_validate_vs_pydantic.py,
which compares both on a large corpus.

ponytail: no strict mode, no custom serializers, no generics beyond the standard containers.
"""

import dataclasses
import datetime
import decimal
import enum
import types
import typing
import uuid

from . import _validate_cy as C
from ._validate_cy import INVALID, MISSING, _fail  # noqa: F401  (re-exported: models.py and the tests use them)

_NoneType = type(None)


class Constraints:
    """What Field(...) can ask of a value. Only the ones set are checked."""

    __slots__ = ("gt", "ge", "lt", "le", "multiple_of", "min_length", "max_length", "pattern", "strip_whitespace")

    def __init__(self, **kw):
        for name in self.__slots__:
            setattr(self, name, kw.get(name))

    def any(self):
        return any(getattr(self, n) is not None for n in self.__slots__)


_SCALARS = {
    int: C.IntV(), float: C.FloatV(), bool: C.BoolV(), str: C.StrV(), bytes: C.BytesV(), _NoneType: C.NoneV(),
    typing.Any: C.AnyV(), object: C.AnyV(), decimal.Decimal: C.DecimalV(), uuid.UUID: C.UuidV(),
    datetime.datetime: C.DateTimeV(), datetime.date: C.DateV(), datetime.time: C.TimeV(),
}
_ANY = _SCALARS[typing.Any]


def _repr_choices(values):
    parts = [repr(v) for v in values]
    return parts[0] if len(parts) == 1 else ", ".join(parts[:-1]) + " or " + parts[-1]


def _label(tp):
    origin = typing.get_origin(tp)
    if origin is None:
        return getattr(tp, "__name__", None) or str(tp)
    if origin is typing.Literal:
        return "literal[" + ",".join(repr(a) for a in typing.get_args(tp)) + "]"
    name = getattr(origin, "__name__", str(origin))
    args = typing.get_args(tp)
    return f"{name}[{','.join(_label(a) for a in args)}]" if args else name


def _strict_spec(tp):
    """What UnionV needs to ask "would v already be a tp, without any conversion?" (Pydantic's smart-union first
    pass): (0,) never, (1, T) exactly T, (2,) None, (3, values) one of the literal values, (4, T) an instance of T."""
    origin = typing.get_origin(tp) or tp
    if origin in (int, float, bool, str):
        return (1, origin)
    if origin is _NoneType:
        return (2,)
    if origin is typing.Literal:
        return (3, typing.get_args(tp))
    if isinstance(origin, type):
        return (4, origin)
    return (0,)


def _is_pydantic_model(tp):
    return isinstance(tp, type) and hasattr(tp, "model_validate") and hasattr(tp, "model_fields") and not hasattr(tp, "__httpp_model__")


# ---------------------------------------------------------------------------------------------------

def constraints_from_metadata(m):
    """The constraints carried by one Annotated[...] item: httpp's Field(), pydantic's Field(), or an
    annotated_types object such as Ge(1) / MinLen(2). Anything else carries none."""
    own = getattr(m, "constraints", None)
    if isinstance(own, Constraints):
        return {n: getattr(own, n) for n in Constraints.__slots__ if getattr(own, n) is not None}
    found = {}
    for item in getattr(m, "metadata", None) or (m,):  # pydantic's FieldInfo keeps them in .metadata
        for name in ("ge", "gt", "le", "lt", "multiple_of", "min_length", "max_length", "pattern", "strip_whitespace"):
            value = getattr(item, name, None)
            if value is not None and not callable(value) and not isinstance(item, (str, bytes, type)):
                found[name] = value
    return found


def _wrap_collection(inner, c, label):
    return C.CollectionConstraintV(inner, c.min_length, c.max_length, label) if c.min_length is not None or c.max_length is not None else inner


def compile_type(tp, constraints=None, _stack=()):
    """The validator for the type hint `tp` (see the module docstring)."""
    c = constraints if constraints is not None and constraints.any() else None

    if isinstance(tp, str) or isinstance(tp, typing.ForwardRef):
        raise TypeError(f"unresolved forward reference {tp!r}")

    origin = typing.get_origin(tp)
    args = typing.get_args(tp)

    if origin is typing.Annotated:
        # Annotated[T, Field(...), ...]: collect the constraints, then validate T
        inner, *meta = args
        merged = {}
        for m in meta:
            merged.update(constraints_from_metadata(m))
        if c is not None:
            merged.update({n: getattr(c, n) for n in Constraints.__slots__ if getattr(c, n) is not None})
        return compile_type(inner, Constraints(**merged) if merged else None, _stack)

    if origin is typing.Union or (hasattr(types, "UnionType") and origin is types.UnionType):
        rest = [a for a in args if a is not _NoneType]
        nullable = len(rest) != len(args)
        if len(rest) == 1:
            inner = compile_type(rest[0], c, _stack)
        else:
            inner = C.UnionV([(_label(a), _strict_spec(a), compile_type(a, c, _stack)) for a in rest])
        return C.NullableV(inner) if nullable else inner

    if origin is typing.Literal:
        return C.LiteralV(args, _repr_choices(args))

    if tp in (typing.Any, object):
        return _ANY

    for kind, (names, label, py_label) in enumerate((((list, typing.List), "list", "List"),
                                                     ((set, typing.Set), "set", "Set"),
                                                     ((frozenset, typing.FrozenSet), "frozenset", "Frozenset"))):
        if origin in names or tp is names[0]:
            item = compile_type(args[0], None, _stack) if args else _ANY
            v = C.ListV(item, kind, label)
            return _wrap_collection(v, c, py_label) if c else v
    if origin in (tuple, typing.Tuple) or tp is tuple:
        if not args:
            return C.TupleV(None, _ANY)
        if len(args) == 2 and args[1] is Ellipsis:
            v = C.TupleV(None, compile_type(args[0], None, _stack))
        else:
            v = C.TupleV([compile_type(a, None, _stack) for a in args], None)
        return _wrap_collection(v, c, "Tuple") if c else v
    if origin in (dict, typing.Dict) or tp is dict:
        if args:
            return C.DictV(compile_type(args[0], None, _stack), compile_type(args[1], None, _stack))
        return C.DictV(_ANY, _ANY)

    if isinstance(tp, type):
        if issubclass(tp, enum.Enum):
            return C.EnumV(tp, _repr_choices([m.value for m in tp]))
        if hasattr(tp, "__httpp_model__"):
            return tp._validator()  # our BaseModel: compiled lazily, so models may refer to each other
        if _is_pydantic_model(tp):
            return C.PydanticV(tp)
        if dataclasses.is_dataclass(tp):
            return C.DataclassV(tp, compile_type)

    base = _SCALARS.get(tp)
    if base is None:
        raise TypeError(f"httpp can't validate values of type {tp!r}")
    if c is not None:
        if tp in (int, float, decimal.Decimal) and (c.gt is not None or c.ge is not None or c.lt is not None
                                                     or c.le is not None or c.multiple_of is not None):
            return C.NumericV(base, c.gt, c.ge, c.lt, c.le, c.multiple_of, tp is float)
        if tp is str and (c.min_length is not None or c.max_length is not None or c.pattern is not None or c.strip_whitespace):
            return C.StrConstraintV(base, c.min_length, c.max_length, c.strip_whitespace, c.pattern)
    return base


def is_scalar_hint(tp):
    """True for a hint that arrives as one string in a query / header / cookie / path (int, str, Optional[int],
    Literal, Enum, ...), False for containers and models, which are read from a body or a repeated key."""
    origin = typing.get_origin(tp)
    if origin is typing.Annotated:
        return is_scalar_hint(typing.get_args(tp)[0])
    if origin is typing.Union or (hasattr(types, "UnionType") and origin is types.UnionType):
        return all(is_scalar_hint(a) for a in typing.get_args(tp) if a is not _NoneType)
    if origin is typing.Literal:
        return True
    if origin is not None:
        return False
    if tp in (typing.Any, object):
        return True
    if tp in (list, set, frozenset, tuple, dict):
        return False
    if isinstance(tp, type):
        if issubclass(tp, enum.Enum):
            return True
        if hasattr(tp, "__httpp_model__") or _is_pydantic_model(tp) or dataclasses.is_dataclass(tp):
            return False
    return tp in _SCALARS


def sequence_item_hint(tp):
    """For List[X] / Set[X] / Tuple[X, ...] (also inside Optional / Annotated): X; else None."""
    origin = typing.get_origin(tp)
    if origin is typing.Annotated:
        return sequence_item_hint(typing.get_args(tp)[0])
    if origin is typing.Union or (hasattr(types, "UnionType") and origin is types.UnionType):
        rest = [a for a in typing.get_args(tp) if a is not _NoneType]
        return sequence_item_hint(rest[0]) if len(rest) == 1 else None
    if origin in (list, set, frozenset, typing.List, typing.Set, typing.FrozenSet):
        args = typing.get_args(tp)
        return args[0] if args else typing.Any
    if origin in (tuple, typing.Tuple):
        args = typing.get_args(tp)
        if len(args) == 2 and args[1] is Ellipsis:
            return args[0]
    if tp in (list, set, frozenset):
        return typing.Any
    return None
