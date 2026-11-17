"""Cookies and Basic credentials in Python go through the C++ core (include/httpp/cookie.hpp): the same rules on both sides."""

import base64

import pytest

import httpp
from httpp import Server
from httpp.testclient import TestClient


def test_set_cookie_defaults_and_every_attribute():
    assert httpp.set_cookie("sid", "abc") == ("Set-Cookie", "sid=abc; Path=/; HttpOnly")
    pair = httpp.set_cookie("sid", "abc", path="/app", domain="example.com", max_age=3600, secure=True, http_only=False, same_site="strict")
    assert pair == ("Set-Cookie", "sid=abc; Path=/app; Domain=example.com; Max-Age=3600; Secure; SameSite=Strict")
    assert httpp.set_cookie("a", "1", path=None, max_age=0, same_site="NONE")[1] == "a=1; Max-Age=0; HttpOnly; SameSite=None"


@pytest.mark.parametrize("name,value", [("", "v"), ("a b", "v"), ("a;b", "v"), ("n", "a b"), ("n", "a;b"), ("n", "a,b"), ("n", 'a"b'),
                                        ("n", "x\r\nY: 1"), ("n", "café"), (1, "v"), ("n", 2)])
def test_set_cookie_refuses_what_cannot_be_sent(name, value):
    with pytest.raises(ValueError):
        httpp.set_cookie(name, value)


def test_set_cookie_refuses_bad_attributes():
    with pytest.raises(ValueError):
        httpp.set_cookie("n", "v", path="/a;b")
    with pytest.raises(ValueError):
        httpp.set_cookie("n", "v", domain="a\r\nb")
    with pytest.raises(ValueError):
        httpp.set_cookie("n", "v", same_site="sometimes")


def test_a_handler_reads_cookies_and_basic_credentials():
    app = Server(openapi_url=None)

    @app.get("/who")
    def who(request):
        return "|".join([request.cookie("a"), request.cookie("missing", "dflt"), str(sorted(request.cookies.items())), str(request.basic_auth())])

    client = TestClient(app)
    token = base64.b64encode("علی:pa:ss".encode()).decode()
    r = client.get("/who", headers={"Cookie": "a=1; b=2 ; a=3; flag; =x", "Authorization": f"basic {token}"})
    assert r.text == "1|dflt|[('a', '1'), ('b', '2')]|('علی', 'pa:ss')"
    for header in ("Bearer abc", "Basic !!!", "Basic " + base64.b64encode(b"nocolon").decode(), ""):
        assert client.get("/who", headers={"Authorization": header}).text.endswith("|None")
