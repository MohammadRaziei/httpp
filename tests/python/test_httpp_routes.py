"""Tests for decorator-style routes on httpp.Server."""

import json
import threading
import time
from contextlib import contextmanager

import pytest

from httpp import Request, Server


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


def test_get_returns_text():
    app = Server()

    @app.get("/ping")
    def ping():
        return "pong"

    with running(app) as url:
        res = Request(url + "/ping").run()
    assert res.status == 200
    assert res.body == "pong"
    assert res.header("Content-Type").startswith("text/plain")


@pytest.mark.parametrize("pattern", ["/hello/{name}", "/hello/<name>"])
def test_path_params_flask_and_fastapi_style(pattern):
    app = Server()

    @app.get(pattern)
    def hello(name):
        return f"hello {name}"

    with running(app) as url:
        assert Request(url + "/hello/world").run().body == "hello world"


def test_post_json_status_and_headers():
    app = Server()

    @app.post("/items")
    def create(request):
        return {"got": request.json()}, 201, {"X-Made": "yes"}

    with running(app) as url:
        res = Request(url + "/items").method("POST").content_type("application/json").data('{"a": 1}').run()
    assert res.status == 201
    assert res.header("Content-Type") == "application/json"
    assert res.header("X-Made") == "yes"
    assert json.loads(res.body) == {"got": {"a": 1}}


def test_request_exposes_query_and_headers():
    app = Server()

    @app.get("/echo")
    def echo(request):
        return {"q": request.query["a"], "h": request.headers["x-token"], "m": request.method}

    with running(app) as url:
        res = Request(url + "/echo?a=1").header("X-Token", "abc").run()
    assert json.loads(res.body) == {"q": "1", "h": "abc", "m": "GET"}


def test_route_with_several_methods():
    app = Server()

    @app.route("/thing", methods=["GET", "PUT"])
    def thing(request):
        return request.method

    with running(app) as url:
        assert Request(url + "/thing").run().body == "GET"
        assert Request(url + "/thing").method("PUT").data("x").run().body == "PUT"


def test_decorated_function_stays_callable():
    app = Server()

    @app.get("/x/{n}")
    def x(n):
        return n

    assert x("7") == "7"


def test_handler_exception_is_a_generic_500(capfd):
    app = Server()

    @app.get("/boom")
    def boom():
        raise RuntimeError("secret internals")

    with running(app) as url:
        res = Request(url + "/boom").run()
    assert res.status == 500
    assert "secret" not in res.body
    assert "secret internals" in capfd.readouterr().err  # traceback went to stderr


def test_unmatched_route_and_wrong_method_are_not_ok():
    app = Server()

    @app.get("/only-get")
    def only_get():
        return "ok"

    with running(app) as url:
        assert Request(url + "/nope").run().status == 404
        assert not Request(url + "/only-get").method("POST").data("x").run().ok


def test_bad_registrations_fail_loudly():
    app = Server()

    with pytest.raises(ValueError):
        app.get("/u/<int:id>")

    with pytest.raises(ValueError):
        app.route("/m", methods=["BREW"])(lambda: "x")


def test_annotated_path_params_are_converted_and_bad_values_get_422():
    app = Server()

    @app.get("/n/{n}")
    def double(n: int):
        return {"double": n * 2}

    with running(app) as url:
        ok = Request(url + "/n/21").run()
        bad = Request(url + "/n/abc").run()
    assert json.loads(ok.body) == {"double": 42}
    assert bad.status == 422
    assert "must be int" in bad.body


def test_async_handlers_work():
    app = Server()

    @app.get("/a/{x}")
    async def a(x, request):
        import asyncio
        await asyncio.sleep(0.01)
        return {"x": x, "path": request.path}

    with running(app) as url:
        res = Request(url + "/a/1").run()
    assert json.loads(res.body) == {"x": "1", "path": "/a/1"}
