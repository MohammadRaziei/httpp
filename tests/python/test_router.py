"""APIRouter and Server.include: the same rules as httpp::router in C++ (tests/cpp/test_router.cpp)."""

import threading
import time
from contextlib import contextmanager

import pytest

from httpp import APIRouter, Request, Server


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


def fetch(base, path, method="GET"):
    return Request(base + path, method=method).run() if method != "GET" else Request(base + path).run()


def test_a_prefix_goes_in_front_of_every_path_and_slash_is_the_prefix_itself():
    users = APIRouter("/users")

    @users.get("/")
    def listing():
        return "list"

    @users.get("/{user_id}")
    def one(user_id: int):
        return {"id": user_id}

    app = Server(openapi_url=None)
    app.include(users)
    with running(app) as base:
        assert fetch(base, "/users").body == "list"
        assert fetch(base, "/users/7").json() == {"id": 7}
        assert fetch(base, "/7").status == 404


def test_outer_prefix_nesting_and_clean_up_of_slashes():
    users = APIRouter("/users")
    api = APIRouter("api")          # no leading slash is still "/api"

    @users.get("/me")
    def me():
        return "me"

    @api.get("/ping")
    async def ping():
        return "pong"

    api.include(users)
    app = Server(openapi_url=None)
    app.include_router(api, "/v1/")  # FastAPI's name works too
    assert api.prefix == "/api"
    with running(app) as base:
        assert fetch(base, "/v1/api/users/me").body == "me"
        assert fetch(base, "/v1/api/ping").body == "pong"
        assert fetch(base, "/api/ping").status == 404


def test_a_router_can_be_included_twice_and_changed_afterwards():
    r = APIRouter("/x")

    @r.get("/a")
    def a():
        return "a"

    app = Server(openapi_url=None)
    app.include(r, "/one").include(r, "/two")

    @r.get("/late")
    def late():
        return "late"

    with running(app) as base:
        assert fetch(base, "/one/x/a").body == "a"
        assert fetch(base, "/two/x/a").body == "a"
        assert fetch(base, "/one/x/late").status == 404


def test_route_options_and_tags_reach_the_openapi_document():
    r = APIRouter("/items", tags=["items"])

    @r.get("/{item_id}", summary="One item", tags=["extra"])
    def item(item_id: int):
        return {"id": item_id}

    app = Server()
    app.include(r)
    op = app.openapi()["paths"]["/items/{item_id}"]["get"]
    assert op["summary"] == "One item"
    assert op["tags"] == ["items", "extra"]


def test_mistakes_are_reported():
    r = APIRouter()
    with pytest.raises(TypeError):
        r.route("/x", bogus=1)
    with pytest.raises(TypeError):
        r.route("/x", methods="GET")

    @r.route("/x", methods=["FETCH"])
    def x():
        return "x"

    with pytest.raises(ValueError):  # the method is checked when the router is included
        Server(openapi_url=None).include(r)


def test_routes_and_functions_stay_directly_callable():
    r = APIRouter("/p/")

    @r.get("x")
    def x():
        return "x"

    assert x() == "x"
    assert r.prefix == "/p"
    assert [(e.path, e.methods) for e in r.routes] == [("/x", ("GET",))]
