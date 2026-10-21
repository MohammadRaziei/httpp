"""HTTPException, the response classes, `status`, and the 422 body of a bad path parameter."""
import json

import pytest

import httpp
from httpp import Request, Server

from test_httpp_routes import running


def get(url, path, method="GET"):
    return Request(url + path).method(method).run()


def test_wrong_method_is_405_with_allow():
    app = Server(openapi_url=None)

    @app.get("/x")
    def x():
        return "ok"

    with running(app) as url:
        r = get(url, "/x", "POST")
        assert r.status == 405
        assert "GET" in r.header("Allow")
        assert get(url, "/nope").status == 404


def test_bad_path_param_422_has_the_fastapi_shape():
    app = Server(openapi_url=None)

    @app.get("/n/{n}")
    def n(n: int):
        return {"n": n}

    @app.get("/f/{x}")
    def f(x: float):
        return {"x": x}

    with running(app) as url:
        bad = get(url, "/n/abc")
        assert bad.status == 422
        assert json.loads(bad.body) == {"detail": [{
            "type": "int_parsing", "loc": ["path", "n"], "input": "abc",
            "msg": "Input should be a valid integer, unable to parse string as an integer"}]}
        assert json.loads(get(url, "/f/zz").body)["detail"][0]["type"] == "float_parsing"
        assert json.loads(get(url, "/n/5").body) == {"n": 5}


def test_http_exception_sync_async_and_dependency():
    app = Server(openapi_url=None)

    def need_auth():
        raise httpp.HTTPException(401, "no token", headers={"WWW-Authenticate": "Bearer"})

    @app.get("/s")
    def s():
        raise httpp.HTTPException(httpp.status.HTTP_404_NOT_FOUND)

    @app.get("/a")
    async def a():
        raise httpp.HTTPException(418, {"why": "teapot"})

    @app.get("/d")
    def d(user=httpp.Depends(need_auth)):
        return "unreachable"

    with running(app) as url:
        r = get(url, "/s")
        assert (r.status, json.loads(r.body)) == (404, {"detail": "Not Found"})
        assert r.header("Content-Type").startswith("application/json")
        r = get(url, "/a")
        assert (r.status, json.loads(r.body)) == (418, {"detail": {"why": "teapot"}})
        r = get(url, "/d")
        assert r.status == 401 and r.header("WWW-Authenticate") == "Bearer"
        assert json.loads(r.body) == {"detail": "no token"}


def test_user_handler_overrides_the_default_http_exception_answer():
    app = Server(openapi_url=None)

    @app.exception_handler(httpp.HTTPException)
    def custom(request, exc):
        return {"custom": exc.status_code}, exc.status_code

    @app.get("/x")
    def x():
        raise httpp.HTTPException(403)

    with running(app) as url:
        r = get(url, "/x")
        assert (r.status, json.loads(r.body)) == (403, {"custom": 403})


def test_http_exception_rejects_a_bad_status():
    for bad in (99, 600, "404", True):
        with pytest.raises(ValueError):
            httpp.HTTPException(bad)


def test_response_classes():
    app = Server(openapi_url=None)

    @app.get("/j")
    def j():
        return httpp.JSONResponse({"a": 1}, status_code=201, headers={"X-One": "1"})

    @app.get("/t")
    def t():
        return httpp.PlainTextResponse("hi")

    @app.get("/h")
    def h():
        return httpp.HTMLResponse("<b>x</b>")

    @app.get("/r")
    def r():
        return httpp.RedirectResponse("/j")

    @app.get("/r2")
    async def r2():
        return httpp.RedirectResponse("/j", status_code=303)

    @app.get("/s")
    def s():
        def gen():
            yield "a"
            yield b"b"
        return httpp.StreamingResponse(gen(), media_type="text/csv")

    @app.get("/m", response_model=dict)
    def m():
        return httpp.JSONResponse({"free": "form"})

    with running(app) as url:
        r = get(url, "/j")
        assert (r.status, json.loads(r.body), r.header("X-One")) == (201, {"a": 1}, "1")
        assert r.header("Content-Type") == "application/json"
        r = get(url, "/t")
        assert r.body == "hi" and r.header("Content-Type") == "text/plain; charset=utf-8"
        assert get(url, "/h").header("Content-Type") == "text/html; charset=utf-8"
        r = Request(url + "/r").follow_redirects(False).run()
        assert (r.status, r.header("Location")) == (307, "/j")
        assert Request(url + "/r2").follow_redirects(False).run().status == 303
        r = get(url, "/s")
        assert r.body == "ab" and r.header("Content-Type").startswith("text/csv")
        assert json.loads(get(url, "/m").body) == {"free": "form"}


def test_file_response(tmp_path):
    f = tmp_path / "a.txt"
    f.write_text("file body")
    app = Server(openapi_url=None)

    @app.get("/f")
    def serve():
        return httpp.FileResponse(str(f))

    with running(app) as url:
        r = get(url, "/f")
        assert r.body == "file body" and r.header("Content-Type").startswith("text/plain")


def test_redirect_status_must_be_3xx():
    with pytest.raises(ValueError):
        httpp.RedirectResponse("/x", status_code=200)


def test_status_constants():
    assert httpp.status.HTTP_200_OK == 200
    assert httpp.status.HTTP_404_NOT_FOUND == 404
    assert httpp.status.HTTP_422_UNPROCESSABLE_ENTITY == 422


def test_http_exception_in_a_before_hook_refuses_the_request():
    app = Server(openapi_url=None)

    @app.before_request
    def guard(request):
        if request.path == "/private":
            raise httpp.HTTPException(401, "login first", headers={"WWW-Authenticate": "Basic"})

    @app.get("/private")
    def private():
        return "secret"

    @app.get("/public")
    def public():
        return "hello"

    with running(app) as url:
        r = get(url, "/private")
        assert (r.status, json.loads(r.body), r.header("WWW-Authenticate")) == (401, {"detail": "login first"}, "Basic")
        assert get(url, "/public").body == "hello"


def test_limit_settings_reach_the_server():
    app = Server(openapi_url=None, header_timeout=2, max_connections=50)

    @app.get("/x")
    def x():
        return "ok"

    with running(app) as url:
        assert get(url, "/x").body == "ok"
    with pytest.raises(Exception):
        Server(openapi_url=None, max_connections=-1)


def test_a_dependency_without_parameters_works_on_a_route_with_path_parameters():
    app = Server(openapi_url=None)

    def plain():
        return 7

    def wants_n(n: int):
        return n * 2

    @app.get("/p/{n}")
    def p(n: int, a=httpp.Depends(plain), b=httpp.Depends(wants_n)):
        return {"n": n, "a": a, "b": b}

    with running(app) as url:
        assert json.loads(get(url, "/p/5").body) == {"n": 5, "a": 7, "b": 10}
