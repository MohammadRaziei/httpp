import json, datetime, uuid, decimal, enum
from typing import Optional, Union, Literal, List, Dict, Annotated, Any
from pydantic import TypeAdapter, ValidationError, Field, BaseModel

def show(tp, *vals, **kw):
    ta = TypeAdapter(tp)
    for v in vals:
        try:
            r = ta.validate_python(v, **kw)
            print(f"{str(tp):28} {v!r:22} -> OK {r!r}")
        except ValidationError as e:
            errs=[{k:x for k,x in d.items() if k!='url'} for d in e.errors()]
            print(f"{str(tp):28} {v!r:22} -> {json.dumps(errs, default=repr)}")

show(int, 5, "5", " 5 ", "5.0", "abc", 5.0, 5.5, True, None, "1_000", "", [1], b"5")
show(float, 1, "1.5", "abc", True, "nan", "inf", None, "")
show(bool, True, 1, 0, 2, "true", "No", "maybe", "1", None, 1.0)
show(str, "a", 1, None, b"ab", True)
show(Optional[int], None, "3", "x")
show(List[int], [1,"2"], "12", [1,"x","y"], (1,2), {1,2}, None)
show(Dict[str,int], {"a":1}, {"a":"x"}, {1:2}, [])
show(Literal["a","b"], "a", "c", 1)
show(Union[int,str], 5, "5", "x", 5.5, None)
show(Union[int,List[int]], "x", [1])
show(Annotated[int, Field(ge=1, le=10)], 0, 11, 5, "7")
show(Annotated[int, Field(gt=1, lt=10, multiple_of=2)], 1, 10, 3)
show(Annotated[str, Field(min_length=2, max_length=3)], "a", "abcd", "ab")
show(Annotated[str, Field(pattern=r"^\d+$")], "12a")
show(Annotated[List[int], Field(min_length=2, max_length=3)], [1], [1,2,3,4])
show(Annotated[float, Field(ge=0.5)], 0.1)
show(datetime.date, "2024-01-05", "2024-13-01", "x", 5)
show(datetime.datetime, "2024-01-05T10:00:00", "2024-01-05", "x")
show(uuid.UUID, "12345678123456781234567812345678", "x")
show(decimal.Decimal, "1.5", 2, "x")
class Color(enum.Enum):
    red=1; green=2
class S(str, enum.Enum):
    a="a"; b="b"
show(Color, 1, "1", 3, Color.red)
show(S, "a", "z")
class M(BaseModel):
    a: int
    b: Optional[str] = None
    c: List[int] = []
show(M, {"a":1}, {"a":"x","c":["y"]}, {}, [], "x", None, M(a=1))
class N(BaseModel):
    m: M
    items: List[M]
show(N, {"m":{"a":"x"}, "items":[{"a":1},{}]}, {"m":1,"items":"x"})
