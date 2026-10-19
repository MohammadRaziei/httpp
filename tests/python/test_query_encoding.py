"""Client query/form encoding must stay byte for byte what urllib produces (spaces as %20), whichever
implementation does the work (C++ httpp::url::build_query since ADR-0017 rule 1)."""

import random
import string
from urllib.parse import quote, urlencode

import pytest

from httpp import httpp_cy


def reference(params):
    if hasattr(params, "items"):
        params = params.items()
    pairs = []
    for k, v in params:
        if isinstance(v, (list, tuple)):
            pairs.extend((k, i) for i in v)
        else:
            pairs.append((k, v))
    return urlencode(pairs, quote_via=quote)


@pytest.mark.parametrize("params", [
    {"a": 1, "b": ["x", "y"]},
    [("a", 1), ("a", 2)],
    {"q": "hello world", "tag": "a/b&c=d", "u": "é世界"},
    {"flag": True, "none": None, "f": 1.5},
    {"empty": ""},
    {},
    {"k": ("x", "y z")},
])
def test_encode_query_matches_urllib(params):
    assert httpp_cy._encode_query(params) == reference(params)


def test_encode_query_matches_urllib_on_random_text():
    rnd = random.Random(7)
    alphabet = string.printable + "é世界ü€\u00a0"
    for _ in range(3000):
        params = {"".join(rnd.choice(alphabet) for _ in range(rnd.randint(1, 6))):
                  "".join(rnd.choice(alphabet) for _ in range(rnd.randint(0, 12))) for _ in range(rnd.randint(1, 4))}
        assert httpp_cy._encode_query(params) == reference(params), params


def test_bytes_values_keep_working():
    assert httpp_cy._encode_query({"b": b"a b\xff"}) == reference({"b": b"a b\xff"})
