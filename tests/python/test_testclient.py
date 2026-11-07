"""httpp.testclient.TestClient: a Starlette-shaped client that serves the app on a loopback port of its own (ADR-0042)."""

import json
import urllib.parse

import pytest

import httpp
from httpp import HTTPException, RedirectResponse, Server
from httpp.testclient import TestClient


def make_app():
    app = Server()
    calls = {"startup": 0, "shutdown": 0}

    @app.on_startup
    def up():
        calls["startup"] += 1

    @app.on_shutdown
    def down():
        calls["shutdown"] += 1

    @app.get("/")
    def index():
        return {"hello": "world"}

    @app.get("/items/{item_id}")
    def item(item_id: int, q: str = ""):
        return {"id": item_id, "q": q}

    @app.post("/echo")
    def echo(request: httpp.ServerRequest):
        return {"method": request.method, "type": request.header("content-type"), "body": request.text}

    @app.route("/any", methods=("GET", "PUT", "PATCH", "DELETE", "OPTIONS"))
    def any_method(request: httpp.ServerRequest):
        return {"method": request.method}

    @app.get("/headers")
    def headers(request: httpp.ServerRequest):
        return {"host": request.header("host"), "x": request.header("x-token"), "auth": request.header("authorization"),
                "cookie": request.header("cookie")}

    @app.get("/login")
    def login():
        return "ok", 200, [httpp.set_cookie("sid", "abc123")]

    @app.get("/logout")
    def logout():
        return "bye", 200, [httpp.set_cookie("sid", "x", max_age=0)]

    @app.get("/me")
    def me(request: httpp.ServerRequest):
        return {"sid": request.cookie("sid")}

    @app.get("/old")
    def old():
        return RedirectResponse("/new", status_code=302)

    @app.get("/new")
    def new():
        return {"page": "new"}

    @app.get("/hop1")
    def hop1():
        return RedirectResponse("/hop2", status_code=307)

    @app.get("/hop2")
    def hop2():
        return RedirectResponse("/new", status_code=301)

    @app.get("/loop")
    def loop():
        return RedirectResponse("/loop")

    @app.get("/gone")
    def gone():
        raise HTTPException(404, "no such thing")

    @app.get("/boom")
    def boom():
        raise RuntimeError("kaput")

    @app.get("/async")
    async def a():
        return {"async": True}

    @app.get("/async-boom")
    async def async_boom():
        raise ValueError("async kaput")

    @app.exception_handler(KeyError)
    def key_error(request, exc):
        return httpp.JSONResponse({"handled": "KeyError"}, status_code=418)

    @app.get("/key")
    def key():
        raise KeyError("k")

    return app, calls


@pytest.fixture
def app_and_calls():
    return make_app()


@pytest.fixture
def client(app_and_calls):
    with TestClient(app_and_calls[0]) as c:
        yield c


def test_get_json_text_and_content(client):
    r = client.get("/")
    assert r.status_code == 200
    assert r.json() == {"hello": "world"}
    assert r.text.startswith("{") and r.content == r.text.encode()
    assert r.headers["Content-Type"] == "application/json"
    assert r.headers["content-type"] == "application/json"  # case-insensitive
    assert r.is_success and not r.is_redirect and r.reason_phrase == "OK"
    assert r.url == "http://testserver/"
    assert r.raise_for_status() is r


def test_path_and_query_parameters(client):
    assert client.get("/items/7", params={"q": "a b"}).json() == {"id": 7, "q": "a b"}
    assert client.get("/items/7?q=raw").json() == {"id": 7, "q": "raw"}


def test_every_method_exists(client):
    assert client.put("/any").json() == {"method": "PUT"}
    assert client.patch("/any").json() == {"method": "PATCH"}
    assert client.delete("/any").json() == {"method": "DELETE"}
    assert client.options("/any").json() == {"method": "OPTIONS"}
    r = client.head("/")
    assert r.status_code == 200 and r.content == b""
    assert client.request("GET", "/any").json() == {"method": "GET"}


def test_json_form_and_raw_bodies(client):
    r = client.post("/echo", json={"a": [1, 2]})
    assert r.json()["type"] == "application/json" and json.loads(r.json()["body"]) == {"a": [1, 2]}
    r = client.post("/echo", data={"a": "1", "b": "two words"})
    assert r.json()["type"] == "application/x-www-form-urlencoded"
    assert urllib.parse.parse_qs(r.json()["body"]) == {"a": ["1"], "b": ["two words"]}
    r = client.post("/echo", data=b"raw bytes", headers={"Content-Type": "text/plain"})
    assert r.json() == {"method": "POST", "type": "text/plain", "body": "raw bytes"}
    r = client.post("/echo", content="as text")
    assert r.json()["body"] == "as text"


def test_default_and_per_request_headers_and_host(app_and_calls):
    with TestClient(app_and_calls[0], headers={"X-Token": "client"}) as c:
        assert c.get("/headers").json()["x"] == "client"
        assert c.get("/headers", headers={"x-token": "request"}).json()["x"] == "request"
        assert c.get("/headers").json()["host"] == "testserver"
    with TestClient(app_and_calls[0], base_url="http://example.org:8080") as c:
        assert c.get("/headers").json()["host"] == "example.org:8080"
        assert c.get("/headers").url == "http://example.org:8080/headers"


def test_basic_auth(client):
    assert client.get("/headers", auth=("user", "pw")).json()["auth"] == "Basic dXNlcjpwdw=="


def test_cookies_are_kept_between_requests(client):
    assert client.get("/me").json() == {"sid": ""}
    r = client.get("/login")
    assert r.cookies["sid"] == "abc123"
    assert client.cookies["sid"] == "abc123"
    assert client.get("/me").json() == {"sid": "abc123"}
    client.get("/logout")
    assert "sid" not in client.cookies
    assert client.get("/me").json() == {"sid": ""}


def test_cookies_given_by_the_caller(app_and_calls):
    with TestClient(app_and_calls[0], cookies={"sid": "from-client"}) as c:
        assert c.get("/me").json() == {"sid": "from-client"}
        assert c.get("/me", cookies={"sid": "from-request"}).json() == {"sid": "from-request"}
        assert c.get("/me").json() == {"sid": "from-client"}  # a per-request cookie is not kept


def test_redirects_are_followed_by_default(client):
    r = client.get("/old")
    assert r.status_code == 200 and r.json() == {"page": "new"}
    assert r.url == "http://testserver/new"
    assert [h.status_code for h in r.history] == [302]
    r = client.get("/hop1")
    assert r.json() == {"page": "new"} and [h.status_code for h in r.history] == [307, 301]


def test_redirects_can_be_left_alone(client):
    r = client.get("/old", follow_redirects=False)
    assert r.status_code == 302 and r.headers["location"] == "/new" and r.is_redirect and r.history == []


def test_a_redirect_loop_is_an_error(client):
    with pytest.raises(httpp.TooManyRedirects):
        client.get("/loop")


def test_error_statuses_are_responses(client):
    r = client.get("/gone")
    assert r.status_code == 404 and r.json() == {"detail": "no such thing"}
    assert client.get("/nowhere").status_code == 404
    assert client.post("/").status_code == 405
    with pytest.raises(httpp.HTTPError):
        client.get("/gone").raise_for_status()


def test_async_handlers_work(client):
    assert client.get("/async").json() == {"async": True}


def test_server_exceptions_are_raised_in_the_test(client):
    with pytest.raises(RuntimeError, match="kaput"):
        client.get("/boom")
    assert client.get("/").status_code == 200  # the client is still usable


def test_async_handler_exceptions_are_raised_in_the_test(client):
    with pytest.raises(ValueError, match="async kaput"):
        client.get("/async-boom")
    assert client.get("/async").json() == {"async": True}


def test_server_exceptions_can_become_a_500(app_and_calls):
    with TestClient(app_and_calls[0], raise_server_exceptions=False) as c:
        r = c.get("/boom")
        assert r.status_code == 500


def test_an_exception_with_a_handler_is_not_raised(client):
    r = client.get("/key")
    assert r.status_code == 418 and r.json() == {"handled": "KeyError"}


def test_with_runs_startup_and_shutdown_once(app_and_calls):
    app, calls = app_and_calls
    with TestClient(app) as c:
        assert calls == {"startup": 1, "shutdown": 0}
        c.get("/")
        c.get("/")
        assert calls == {"startup": 1, "shutdown": 0}
    assert calls == {"startup": 1, "shutdown": 1}


def test_without_with_the_server_starts_on_the_first_request_and_close_stops_it(app_and_calls):
    app, calls = app_and_calls
    c = TestClient(app)
    assert calls["startup"] == 0  # nothing runs before the first request
    assert c.get("/").status_code == 200
    assert c.get("/").status_code == 200
    assert calls == {"startup": 1, "shutdown": 0}
    c.close()
    assert calls == {"startup": 1, "shutdown": 1}
    c.close()  # closing twice is harmless
    assert calls["shutdown"] == 1


def test_a_client_can_be_used_again_after_it_was_closed(app_and_calls):
    app, calls = app_and_calls
    c = TestClient(app)
    with c:
        c.get("/")
    with c:
        assert c.get("/").status_code == 200
    assert calls == {"startup": 2, "shutdown": 2}


def test_two_clients_for_two_apps_do_not_mix():
    a, _ = make_app()
    b = Server()

    @b.get("/")
    def other():
        return {"app": "b"}

    with TestClient(a) as ca, TestClient(b) as cb:
        assert ca.get("/").json() == {"hello": "world"}
        assert cb.get("/").json() == {"app": "b"}


def test_the_client_only_takes_a_server(client):
    with pytest.raises(TypeError):
        TestClient(object())
