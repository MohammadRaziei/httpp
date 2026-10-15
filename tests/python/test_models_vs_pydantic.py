"""httpp.models against Pydantic v2 on the same model definitions (one definition, built with each library)."""

import datetime
import enum
import json
import types
from typing import Dict, List, Literal, Optional, Union

import pytest

pydantic = pytest.importorskip("pydantic")

import httpp.models as ours_lib  # noqa: E402


class Color(enum.Enum):  # shared: two classes with the same members would never compare equal
    red = "red"
    blue = "blue"


def define(lib):
    """The same models, written once, built with `lib`'s BaseModel / Field / validators."""
    BaseModel, Field = lib.BaseModel, lib.Field
    field_validator, model_validator = lib.field_validator, lib.model_validator
    ConfigDict = lib.ConfigDict

    class Address(BaseModel):
        street: str
        zip: str = Field(pattern=r"^\d{5}$")

    class User(BaseModel):
        id: int
        name: str = Field(min_length=2, max_length=10)
        email: Optional[str] = None
        age: int = Field(default=18, ge=0, le=150)
        tags: List[str] = []
        address: Optional[Address] = None
        color: Color = Color.red
        score: float = 0.0
        joined: Optional[datetime.date] = None
        meta: Dict[str, int] = Field(default_factory=dict)
        kind: Literal["a", "b"] = "a"

    class Aliased(BaseModel):
        model_config = ConfigDict(populate_by_name=True)
        user_id: int = Field(alias="userId")
        display_name: str = Field(default="x", alias="displayName")

    class Strict(BaseModel):
        model_config = ConfigDict(extra="forbid")
        a: int

    class Loose(BaseModel):
        model_config = ConfigDict(extra="allow")
        a: int

    class Validated(BaseModel):
        name: str
        qty: int

        @field_validator("name")
        @classmethod
        def upper(cls, v):
            if v == "bad":
                raise ValueError("name is bad")
            return v.upper()

        @field_validator("qty", mode="before")
        @classmethod
        def clean(cls, v):
            return v.replace(",", "") if isinstance(v, str) else v

        @model_validator(mode="after")
        def check(self):
            if self.qty > 100:
                raise ValueError("too many")
            return self

    class Tree(BaseModel):
        value: int
        children: List["Tree"] = []

    class Shape(BaseModel):
        item: Union[int, Address]

    return types.SimpleNamespace(User=User, Address=Address, Aliased=Aliased, Strict=Strict, Loose=Loose,
                                 Validated=Validated, Tree=Tree, Shape=Shape, Color=Color)


MODELS = {name: (define(pydantic), define(ours_lib)) for name in ("x",)}
THEIRS, OURS = MODELS["x"]


def norm_errors(errs):
    out = []
    for e in errs:
        ctx = e.get("ctx") or None
        if ctx and "error" in ctx:
            ctx = {"error": str(ctx["error"])}
        out.append((e["type"], tuple(e["loc"]), e["msg"], repr(e.get("input")), ctx))
    return out


def run(ns, model, data, method="model_validate"):
    cls = getattr(ns, model)
    try:
        obj = getattr(cls, method)(data)
        return "ok", obj
    except Exception as exc:
        if hasattr(exc, "errors"):
            return "err", norm_errors(exc.errors(include_url=False))
        raise


VALID_AND_INVALID = [
    ("User", {"id": 1, "name": "Ann"}),
    ("User", {"id": "7", "name": "Bob", "age": "30", "tags": ["a", "b"], "score": "1.5"}),
    ("User", {"id": 1, "name": "A"}),
    ("User", {"id": "x", "name": "Ann", "age": 200, "tags": "t"}),
    ("User", {}),
    ("User", {"id": 1, "name": "Ann", "address": {"street": "s", "zip": "123"}}),
    ("User", {"id": 1, "name": "Ann", "address": {"street": "s", "zip": "12345"}}),
    ("User", {"id": 1, "name": "Ann", "address": "nope"}),
    ("User", {"id": 1, "name": "Ann", "color": "blue"}),
    ("User", {"id": 1, "name": "Ann", "color": "green"}),
    ("User", {"id": 1, "name": "Ann", "joined": "2024-02-03"}),
    ("User", {"id": 1, "name": "Ann", "meta": {"a": "1"}}),
    ("User", {"id": 1, "name": "Ann", "meta": {"a": "z"}}),
    ("User", {"id": 1, "name": "Ann", "kind": "c"}),
    ("User", {"id": 1, "name": "Ann", "email": None}),
    ("User", {"id": 1, "name": "Ann", "extra": 1}),
    ("User", [1, 2]),
    ("User", None),
    ("Aliased", {"userId": 1}),
    ("Aliased", {"user_id": 1}),
    ("Aliased", {"userId": 1, "displayName": "n"}),
    ("Aliased", {}),
    ("Strict", {"a": 1}),
    ("Strict", {"a": 1, "b": 2}),
    ("Loose", {"a": 1, "b": 2}),
    ("Validated", {"name": "x", "qty": "1,000"}),
    ("Validated", {"name": "bad", "qty": 1}),
    ("Validated", {"name": "x", "qty": 5}),
    ("Validated", {"name": "x", "qty": 500}),
    ("Tree", {"value": 1, "children": [{"value": 2, "children": [{"value": "x"}]}]}),
    ("Shape", {"item": 3}),
    ("Shape", {"item": {"street": "s", "zip": "12345"}}),
    ("Shape", {"item": "q"}),
]


@pytest.mark.parametrize("model,data", VALID_AND_INVALID, ids=lambda x: json.dumps(x, default=str)[:50] if not isinstance(x, str) else x)
def test_validate_gives_the_same_result(model, data):
    a, b = run(THEIRS, model, data), run(OURS, model, data)
    assert a[0] == b[0], f"\n pydantic {a}\n httpp    {b}"
    if a[0] == "err":
        assert a[1] == b[1], f"\n pydantic {a[1]}\n httpp    {b[1]}"
    else:
        assert a[1].model_dump() == b[1].model_dump()
        assert a[1].model_dump(mode="json") == b[1].model_dump(mode="json")
        assert json.loads(a[1].model_dump_json()) == json.loads(b[1].model_dump_json())
        for kw in ({"exclude_none": True}, {"exclude_unset": True}, {"by_alias": True}, {"exclude_defaults": True},
                   {"include": {"id", "a", "name", "user_id"}}, {"exclude": {"id", "a", "name", "user_id"}}):
            assert a[1].model_dump(**kw) == b[1].model_dump(**kw), kw
        assert a[1].model_fields_set == b[1].model_fields_set


def test_constructor_and_equality_and_copy():
    for ns in (THEIRS, OURS):
        u = ns.User(id=1, name="Ann")
        assert u == ns.User(id=1, name="Ann") and u != ns.User(id=2, name="Ann")
        assert u.model_copy(update={"age": 40}).age == 40 and u.age == 18
        assert list(ns.User.model_fields)[:3] == ["id", "name", "email"]
        assert u.tags == [] and ns.User(id=1, name="Ann").tags is not u.tags  # a mutable default is not shared


def test_model_validate_json():
    for ns in (THEIRS, OURS):
        assert ns.User.model_validate_json('{"id": 3, "name": "Zed"}').id == 3
        with pytest.raises(ValueError):
            ns.User.model_validate_json("{bad")


def test_str_of_a_validation_error_looks_like_pydantics():
    errors = []
    for ns in (THEIRS, OURS):
        try:
            ns.User(id="x", name="A")
        except ValueError as exc:
            errors.append(str(exc).splitlines()[0])
    assert errors[0] == errors[1]
