"""Query() / Path() / Header() / Cookie() / Body() and Depends(), at runtime and in the OpenAPI document."""

import json
import threading
import time
from contextlib import contextmanager
from typing import Annotated, List, Optional

import pytest

import httpp
from httpp import Body, Cookie, Depends, Header, Path, Query, Server
from httpp.models import BaseModel, Field


@contextmanager
def running(app):
    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        yield f"http://127.0.0.1:{port}"
    finally:
        app.stop()
        th.join()


def get(url, headers=None):
    return httpp.get(url, headers=headers or {})


def detail(r):
    return [(e["type"], e["loc"]) for e in json.loads(r.text)["detail"]]


# --- Query / Path / Header / Cookie -----------------------------------------------------------------

def make_app():
    app = Server()

    @app.get("/q")
    def q(term: str = Query(min_length=3), n: int = Query(5, ge=1, le=10, description="how many"),
          tags: List[str] = Query(default_factory=list), opt: Optional[int] = Query(None)):
        return {"term": term, "n": n, "tags": tags, "opt": opt}

    @app.get("/ann")
    def ann(n: Annotated[int, Query(ge=1)] = 3, m: Annotated[int, Field(le=9)] = 1):
        return [n, m]

    @app.get("/h")
    def h(user_agent: str = Header(), x_token: Optional[str] = Header(None), raw_name: str = Header("d", convert_underscores=False),
          tokens: List[str] = Header(default_factory=list)):
        return {"ua": user_agent, "tok": x_token, "raw": raw_name, "tokens": tokens}

    @app.get("/c")
    def c(session: str = Cookie(), theme: str = Cookie("light")):
        return {"session": session, "theme": theme}

    @app.get("/p/{item_id}")
    def p(item_id: int = Path(ge=1, le=100, description="the id")):
        return str(item_id)

    @app.get("/alias")
    def alias(item_q: str = Query(alias="item-query")):
        return item_q

    return app


def test_query_constraints_defaults_and_lists():
    with running(make_app()) as url:
        assert json.loads(get(f"{url}/q?term=abc").text) == {"term": "abc", "n": 5, "tags": [], "opt": None}
        assert json.loads(get(f"{url}/q?term=abcd&n=7&tags=a&tags=b&opt=3").text) == {"term": "abcd", "n": 7, "tags": ["a", "b"], "opt": 3}
        r = get(f"{url}/q?term=ab&n=11")
        assert r.status == 422 and detail(r) == [("string_too_short", ["query", "term"]), ("less_than_equal", ["query", "n"])]
        assert detail(get(f"{url}/q")) == [("missing", ["query", "term"])]


def test_annotated_markers():
    with running(make_app()) as url:
        assert json.loads(get(f"{url}/ann").text) == [3, 1]
        assert detail(get(f"{url}/ann?n=0&m=10")) == [("greater_than_equal", ["query", "n"]), ("less_than_equal", ["query", "m"])]


def test_headers_use_hyphens_and_ignore_case():
    with running(make_app()) as url:
        r = get(f"{url}/h", {"User-Agent": "tester", "x-TOKEN": "t", "raw_name": "r", "tokens": "a"})
        assert json.loads(r.text) == {"ua": "tester", "tok": "t", "raw": "r", "tokens": ["a"]}
        r = get(f"{url}/h", {"User-Agent": "tester"})
        assert json.loads(r.text) == {"ua": "tester", "tok": None, "raw": "d", "tokens": []}


def test_missing_required_header_is_reported_by_its_header_name():
    app = Server()

    @app.get("/h")
    def h(x_api_key: str = Header()):
        return x_api_key

    with running(app) as url:
        assert detail(get(f"{url}/h")) == [("missing", ["header", "x-api-key"])]


def test_cookies():
    with running(make_app()) as url:
        r = get(f"{url}/c", {"Cookie": 'session=abc; theme="dark"; other=1'})
        assert json.loads(r.text) == {"session": "abc", "theme": "dark"}
        assert detail(get(f"{url}/c")) == [("missing", ["cookie", "session"])]


def test_path_constraints():
    with running(make_app()) as url:
        assert get(f"{url}/p/7").text == "7"
        r = get(f"{url}/p/0")
        assert r.status == 422 and detail(r) == [("greater_than_equal", ["path", "item_id"])]


def test_query_alias():
    with running(make_app()) as url:
        assert get(f"{url}/alias?item-query=x").text == "x"


def test_mutable_defaults_are_not_shared_between_requests():
    app = Server()

    @app.get("/m")
    def m(tags: List[str] = Query([])):
        tags.append("x")
        return tags

    with running(app) as url:
        assert json.loads(get(f"{url}/m").text) == ["x"] and json.loads(get(f"{url}/m").text) == ["x"]


def test_wrong_marker_for_where_the_name_is():
    app = Server()
    with pytest.raises(TypeError, match="is in the path"):
        @app.get("/a/{x}")
        def a(x: int = Query()):
            return ""
    with pytest.raises(TypeError, match="not in the route path"):
        @app.get("/b")
        def b(x: int = Path()):
            return ""


def test_openapi_describes_every_marker():
    doc = make_app().openapi()
    q = {p["name"]: p for p in doc["paths"]["/q"]["get"]["parameters"]}
    assert q["term"] == {"name": "term", "in": "query", "required": True, "schema": {"type": "string", "minLength": 3}}
    assert q["n"]["schema"] == {"type": "integer", "minimum": 1, "maximum": 10, "default": 5} and q["n"]["description"] == "how many"
    assert q["tags"]["schema"]["items"] == {"type": "string"}
    h = {p["name"]: p for p in doc["paths"]["/h"]["get"]["parameters"]}
    assert set(h) == {"user-agent", "x-token", "raw_name", "tokens"} and h["user-agent"]["in"] == "header" and h["user-agent"]["required"]
    assert {p["name"] for p in doc["paths"]["/c"]["get"]["parameters"]} == {"session", "theme"}
    assert [p["in"] for p in doc["paths"]["/c"]["get"]["parameters"]] == ["cookie", "cookie"]
    pp = doc["paths"]["/p/{item_id}"]["get"]["parameters"][0]
    assert pp["in"] == "path" and pp["schema"] == {"type": "integer", "minimum": 1, "maximum": 100} and pp["description"] == "the id"
    assert doc["paths"]["/alias"]["get"]["parameters"][0]["name"] == "item-query"


def test_openapi_with_markers_is_valid():
    validator = pytest.importorskip("openapi_spec_validator")
    validator.validate(make_app().openapi())


# --- Body ---------------------------------------------------------------------------------------------

class Item(BaseModel):
    name: str


def test_several_body_parameters_are_read_by_name():
    app = Server()

    @app.post("/two")
    def two(item: Item, importance: int = Body(), note: Optional[str] = Body(None)):
        return {"item": item.name, "importance": importance, "note": note}

    with running(app) as url:
        r = httpp.post(f"{url}/two", json={"item": {"name": "a"}, "importance": "3"})
        assert json.loads(r.text) == {"item": "a", "importance": 3, "note": None}
        r = httpp.post(f"{url}/two", json={"item": {}, "note": 1})
        assert detail(r) == [("missing", ["body", "item", "name"]), ("missing", ["body", "importance"]), ("string_type", ["body", "note"])]
        assert detail(httpp.post(f"{url}/two", json=[1]))[0][0] == "model_attributes_type"
    schema = app.openapi()["paths"]["/two"]["post"]["requestBody"]["content"]["application/json"]["schema"]
    assert schema["type"] == "object" and schema["required"] == ["item", "importance"]
    assert schema["properties"]["item"] == {"$ref": "#/components/schemas/Item", "title": "Item"}


def test_embed_and_a_scalar_body():
    app = Server()

    @app.post("/e")
    def e(item: Item = Body(embed=True)):
        return item.name

    @app.post("/s")
    def s(count: int = Body()):
        return str(count + 1)

    with running(app) as url:
        assert httpp.post(f"{url}/e", json={"item": {"name": "z"}}).text == "z"
        assert httpp.post(f"{url}/s", json=4).text == "5"


# --- Depends -----------------------------------------------------------------------------------------

def test_dependencies_with_parameters_sub_dependencies_and_cache():
    app = Server()
    calls = []

    def common(q: Optional[str] = None, limit: int = Query(10, le=50)):
        calls.append("common")
        return {"q": q, "limit": limit}

    def user(token: str = Header(), c: dict = Depends(common)):
        calls.append("user")
        return {"user": token, "c": c}

    @app.get("/d")
    def d(u: dict = Depends(user), c: dict = Depends(common)):
        return {"u": u, "c": c}

    with running(app) as url:
        r = get(f"{url}/d?q=x&limit=7", {"token": "t"})
        assert json.loads(r.text) == {"u": {"user": "t", "c": {"q": "x", "limit": 7}}, "c": {"q": "x", "limit": 7}}
        assert calls == ["common", "user"]  # `common` ran once for the whole request
        r = get(f"{url}/d?limit=99")  # errors from the route's dependencies are all reported
        assert sorted(detail(r)) == sorted([("less_than_equal", ["query", "limit"]), ("missing", ["header", "token"])])  # `common` once


def test_use_cache_false_runs_it_again():
    app = Server()
    n = []

    def counter():
        n.append(1)
        return len(n)

    @app.get("/c")
    def c(a: int = Depends(counter), b: int = Depends(counter, use_cache=False), c: int = Depends(counter)):
        return [a, b, c]

    with running(app) as url:
        assert json.loads(get(f"{url}/c").text) == [1, 2, 1]


def test_annotated_depends_and_class_dependencies():
    app = Server()

    class Pager:
        def __init__(self, page: int = 1, size: int = Query(20, le=100)):
            self.page, self.size = page, size

    @app.get("/pg")
    def pg(p: Annotated[Pager, Depends()], q: Annotated[Pager, Depends(Pager)]):
        return [p.page, p.size, q.page]

    with running(app) as url:
        assert json.loads(get(f"{url}/pg?page=3&size=5").text) == [3, 5, 3]
        assert detail(get(f"{url}/pg?size=500"))[0] == ("less_than_equal", ["query", "size"])


def test_dependency_can_ask_for_the_request_and_for_path_params():
    app = Server()

    def who(request, item_id: int):
        return f"{request.method}:{item_id}"

    @app.get("/w/{item_id}")
    def w(x: str = Depends(who)):
        return x

    with running(app) as url:
        assert get(f"{url}/w/5").text == "GET:5"
        assert detail(get(f"{url}/w/nope"))[0] == ("int_parsing", ["path", "item_id"]) or get(f"{url}/w/nope").status == 422


def test_yield_dependency_is_cleaned_up_after_the_handler_even_on_error():
    app = Server()
    events = []

    def resource():
        events.append("open")
        try:
            yield "r"
        finally:
            events.append("close")

    @app.get("/ok")
    def ok(r: str = Depends(resource)):
        events.append("handler")
        return r

    @app.get("/boom")
    def boom(r: str = Depends(resource)):
        raise RuntimeError("x")

    with running(app) as url:
        assert get(f"{url}/ok").text == "r"
        assert events == ["open", "handler", "close"]
        events.clear()
        assert get(f"{url}/boom").status == 500
        assert events == ["open", "close"]


def test_exception_in_dependency_goes_to_the_exception_handler():
    app = Server()

    class Denied(Exception):
        pass

    @app.exception_handler(Denied)
    def denied(request, exc):
        return {"error": "denied"}, 403

    def guard(token: Optional[str] = Header(None)):
        if token != "ok":
            raise Denied()

    @app.get("/g")
    def g(_: None = Depends(guard)):
        return "in"

    with running(app) as url:
        assert get(f"{url}/g").status == 403
        assert get(f"{url}/g", {"token": "ok"}).text == "in"


def test_async_route_with_async_and_yield_dependencies():
    import asyncio

    app = Server()
    events = []

    async def fetch(n: int = 1):
        await asyncio.sleep(0.001)
        return n * 2

    async def conn():
        events.append("open")
        try:
            yield "c"
        finally:
            events.append("close")

    def plain():
        return "p"

    @app.get("/a")
    async def a(x: int = Depends(fetch), c: str = Depends(conn), p: str = Depends(plain)):
        return {"x": x, "c": c, "p": p}

    with running(app) as url:
        assert json.loads(get(f"{url}/a?n=4").text) == {"x": 8, "c": "c", "p": "p"}
        assert events == ["open", "close"]
        assert detail(get(f"{url}/a?n=z")) == [("int_parsing", ["query", "n"])]


def test_sync_route_cannot_use_an_async_dependency():
    app = Server()

    async def dep():
        return 1

    with pytest.raises(TypeError, match="async dependency"):
        @app.get("/x")
        def x(v: int = Depends(dep)):
            return ""


def test_openapi_includes_the_parameters_of_dependencies():
    app = Server()

    def common(q: Optional[str] = None, token: str = Header()):
        return q

    @app.get("/d/{item_id}")
    def d(item_id: int, c: Optional[str] = Depends(common), extra: int = 0):
        return ""

    params = {(p["in"], p["name"]) for p in app.openapi()["paths"]["/d/{item_id}"]["get"]["parameters"]}
    assert params == {("path", "item_id"), ("query", "q"), ("header", "token"), ("query", "extra")}
    validator = pytest.importorskip("openapi_spec_validator")
    validator.validate(app.openapi())
