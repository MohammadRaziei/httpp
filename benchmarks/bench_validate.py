"""Micro-benchmark of httpp's validators: valid input (the common case) and invalid input.

    PYTHONPATH=build-py/python/install python3 benchmarks/bench_validate.py

Prints the median ns per call over 5 rounds. Compare numbers from the same run only (the machine drifts).
"""
import statistics
import sys
import time
import typing

from httpp import _validate as V
from httpp.models import BaseModel, Field


class Address(BaseModel):
    city: str = Field(min_length=1)
    zip: str


class User(BaseModel):
    id: int = Field(ge=1)
    name: str = Field(min_length=1, max_length=50)
    email: typing.Optional[str] = None
    score: float = Field(default=0.0, ge=0)
    tags: typing.List[str] = []
    address: Address


GOOD = {"id": "7", "name": "Ada", "email": "a@b.c", "score": 3, "tags": ["x", "y", "z"],
        "address": {"city": "Frankfurt", "zip": "60311"}}
BAD = {"id": "0", "name": "", "tags": [1, None], "address": {"city": ""}}


def run(label, fn, n):
    samples = []
    for _ in range(5):
        t = time.perf_counter()
        for _ in range(n):
            fn()
        samples.append((time.perf_counter() - t) / n * 1e9)
    print(f"{label:<34}{statistics.median(samples):>10.0f} ns")


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
    errs = []
    int_c = V.compile_type(typing.Annotated[int, Field(ge=1, le=100).constraints])
    int_plain = V.compile_type(int)
    list_int = V.compile_type(typing.List[int])
    opt_union = V.compile_type(typing.Union[int, str, None])
    run("int (plain, str input)", lambda: int_plain("42", (), errs), n * 5)
    run("int ge/le (valid)", lambda: int_c(42, (), errs), n * 5)
    run("List[int] x10 (valid)", lambda: list_int(list(range(10)), (), errs), n)
    run("Union[int,str,None] (valid)", lambda: opt_union("a", (), errs), n * 5)
    run("User model (valid)", lambda: User.model_validate(GOOD), n // 2)

    def bad():
        try:
            User.model_validate(BAD)
        except Exception:
            pass

    run("User model (invalid, 5 errors)", bad, n // 4)


main()
