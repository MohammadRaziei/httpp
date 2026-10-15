"""httpp's own validator against Pydantic v2 (the oracle) on the same type hints and inputs.

What has to agree: the converted value, or the error type, loc, msg, input and ctx. Where only the
error *type* is compared (the text comes from Pydantic's Rust parsers and is not worth copying), the
case is in LOOSE. Skipped when pydantic isn't installed.
"""

import datetime
import decimal
import enum
import json
import uuid
from dataclasses import dataclass, field
from typing import Annotated, Any, Dict, FrozenSet, List, Literal, Optional, Set, Tuple, Union

import pytest

pydantic = pytest.importorskip("pydantic")
from pydantic import Field as PField, TypeAdapter, ValidationError  # noqa: E402

from httpp import _validate as V  # noqa: E402

LOOSE = {"date_from_datetime_parsing", "datetime_from_date_parsing", "uuid_parsing", "time_parsing"}


class Color(enum.Enum):
    red = 1
    green = 2


class Name(str, enum.Enum):
    a = "a"
    b = "b"


@dataclass
class Point:
    x: int
    y: int = 0


def norm_errors(errs):
    out = []
    for e in errs:
        item = {"type": e["type"], "loc": tuple(e["loc"])}
        if e["type"] not in LOOSE:
            item["msg"] = e["msg"]
            item["ctx"] = e.get("ctx") or None
        item["input"] = repr(e.get("input"))
        out.append(item)
    return out


def theirs(tp, value):
    try:
        return "ok", TypeAdapter(tp).validate_python(value)
    except ValidationError as exc:
        return "err", norm_errors(exc.errors(include_url=False))


def ours(tp, value):
    errors = []
    result = V.compile_type(tp)(value, (), errors)
    if result is V.INVALID:
        return "err", norm_errors(errors)
    return "ok", result


def same(a, b):
    if a[0] != b[0]:
        return False
    if a[0] == "err":
        return a[1] == b[1]
    x, y = a[1], b[1]
    if isinstance(x, float) and isinstance(y, float) and x != x and y != y:
        return True
    return x == y and type(x) is type(y)


PC = PField  # pydantic Field for the oracle, httpp Field for us (same keyword names)

CASES = [
    (int, [5, "5", " 5 ", "5.0", "abc", 5.0, 5.5, True, None, "1_000", "", [1], b"5", "5.5", "1e3", "-3", "+4", 10**30]),
    (float, [1, "1.5", "abc", True, "nan", "inf", None, "", [1], "1_0", b"2.5", 1e300]),
    (bool, [True, False, 1, 0, 2, "true", "No", "maybe", "1", None, 1.0, 0.0, 0.5, "ON", "f", "", [True], b"yes"]),
    (str, ["a", 1, None, b"ab", True, "", ["a"], 1.5]),
    (bytes, [b"a", "a", 1, None]),
    (Optional[int], [None, "3", "x", 4]),
    (Optional[str], [None, "a", 3]),
    (List[int], [[1, "2"], "12", [1, "x", "y"], (1, 2), {1, 2}, None, [], {"a": 1}, [[1]]]),
    (Set[int], [[1, 1, 2], {1}, "x", ["a"]]),
    (FrozenSet[str], [["a", "a"], 1]),
    (Tuple[int, ...], [[1, 2], (1, "x"), "x", []]),
    (Tuple[int, str], [[1, "a"], [1], [1, "a", 3], ["x", 2], "no"]),
    (Dict[str, int], [{"a": 1}, {"a": "x"}, {1: 2}, [], None, {}, {"a": "1"}]),
    (Dict[str, List[int]], [{"a": [1, "2"]}, {"a": [None]}]),
    (Literal["a", "b"], ["a", "c", 1, None]),
    (Literal[1, 2], [1, 2, 3, "1", True]),
    (Union[int, str], [5, "5", "x", 5.5, None, True, [1]]),
    (Union[str, int], ["5", 5, 5.5]),
    (Union[int, List[int]], ["x", [1], 1, [None]]),
    (Union[int, float], [1, 1.5, "1", "1.5", "x"]),
    (Union[bool, int], [True, 1, "1", 2]),
    (Optional[Union[int, str]], [None, 1, "a", 1.5]),
    (Annotated[int, PC(ge=1, le=10)], [0, 11, 5, "7", 1, 10, "x", 5.5]),
    (Annotated[int, PC(gt=1, lt=10, multiple_of=2)], [1, 10, 3, 4, 2, 0, 12, "6"]),
    (Annotated[float, PC(ge=0.5)], [0.1, 0.5, "0.2", 1]),
    (Annotated[float, PC(gt=0, lt=1)], [0, 1, 0.5, -1.5]),
    (Annotated[str, PC(min_length=2, max_length=3)], ["a", "abcd", "ab", "abc", "", 5]),
    (Annotated[str, PC(min_length=1)], ["", "x"]),
    (Annotated[str, PC(pattern=r"^\d+$")], ["12a", "123", "", 5]),
    (Annotated[List[int], PC(min_length=2, max_length=3)], [[1], [1, 2, 3, 4], [1, 2], [], ["x"]]),
    (Annotated[Set[int], PC(min_length=1)], [[], [1]]),
    (Annotated[Optional[int], PC(ge=3)], [None, 2, 3]),
    (datetime.date, ["2024-01-05", "2024-13-01", "x", 5, "2024-01-05T00:00:00", "2024-01-05T10:00:00", None, datetime.date(2024, 1, 1)]),
    (datetime.datetime, ["2024-01-05T10:00:00", "2024-01-05", "x", None, "2024-01-05T10:00:00Z", "2024-01-05T10:00:00+02:00", 0]),
    (datetime.time, ["10:00:00", "10:00", "x", None]),
    (uuid.UUID, ["12345678123456781234567812345678", "12345678-1234-5678-1234-567812345678", "x", 5, None]),
    (decimal.Decimal, ["1.5", 2, "x", 1.5, None, True]),
    (Color, [1, "1", 3, Color.red, None, 2]),
    (Name, ["a", "z", Name.b, 1]),
    (Point, [{"x": 1}, {"x": "2", "y": "3"}, {"y": 1}, {}, "x", None, Point(1, 2), {"x": "a"}]),
    (Any, [1, "a", None, [1]]),
    (List[Optional[int]], [[1, None, "2"], [None, "x"]]),
    (Optional[List[int]], [None, [1], ["a"], "x"]),
    (List[List[int]], [[[1], [2, "x"]], [1]]),
]


@pytest.mark.parametrize("tp,value", [(tp, v) for tp, values in CASES for v in values],
                         ids=lambda x: repr(x)[:40] if not isinstance(x, type) else x.__name__)
def test_same_result_as_pydantic(tp, value):
    a, b = ours(tp, value), theirs(tp, value)
    assert same(a, b), f"\n type {tp}\n input {value!r}\n httpp    {a}\n pydantic {b}"
