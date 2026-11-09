"""WebSocket routes on an APIRouter, and dependencies (`Depends`) on WebSocket routes (ADR-0048)."""
import time

import pytest

import httpp
from httpp import APIRouter, Depends, HTTPException, Server
from httpp.testclient import TestClient


def wait_for(cond, seconds=5):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.01)
    return cond()


def echo_room(router, path="/{room}"):
    @router.websocket(path)
    async def room(websocket, room: str):
        await websocket.accept()
        await websocket.send_text(f"room={room}")
        await websocket.send_text(await websocket.receive_text())

    return room


# ---- APIRouter.websocket ----

def test_a_router_websocket_is_served_under_the_router_prefix_and_the_include_prefix():
    chat = APIRouter("/chat")
    echo_room(chat)
    app = Server(openapi_url=None)
    app.include(chat, "/v1")
    with TestClient(app) as c:
        with c.websocket_connect("/v1/chat/lobby") as ws:
            assert ws.receive_text() == "room=lobby"
            ws.send_text("hi")
            assert ws.receive_text() == "hi"
        with pytest.raises(httpp.WebSocketDisconnect):
            with c.websocket_connect("/chat/lobby"):  # not mounted without the include prefix
                pass


def test_nested_routers_join_their_prefixes_for_websockets_and_for_http_routes_together():
    inner = APIRouter("/inner")
    echo_room(inner, "/ws/{room}")

    @inner.get("/ping")
    def ping():
        return "pong"

    outer = APIRouter("/outer")
    outer.include(inner, "/mid")
    app = Server(openapi_url=None)
    app.include(outer)
    with TestClient(app) as c:
        assert c.get("/outer/mid/inner/ping").text == "pong"
        with c.websocket_connect("/outer/mid/inner/ws/a") as ws:
            assert ws.receive_text() == "room=a"


def test_a_router_can_be_included_twice_and_changes_made_after_the_first_include_are_not_seen_by_it():
    r = APIRouter("/r")
    echo_room(r)
    app = Server(openapi_url=None)
    app.include(r, "/one")
    echo_room(r, "/late/{room}")  # added after the first include: the next include sees it, the first did not
    app.include(r, "/two")
    with TestClient(app) as c:
        with c.websocket_connect("/one/r/x") as ws:
            assert ws.receive_text() == "room=x"
        with c.websocket_connect("/two/r/late/y") as ws:
            assert ws.receive_text() == "room=y"
        with pytest.raises(httpp.WebSocketDisconnect):
            with c.websocket_connect("/one/r/late/y"):
                pass


def test_websocket_options_are_checked_when_the_route_is_declared_on_the_router():
    r = APIRouter()
    with pytest.raises(TypeError):
        r.websocket("/ws", bogus=1)
    with pytest.raises(ValueError):
        r.websocket("/ws", ping_interval=-1)
    with pytest.raises(ValueError):
        r.websocket("/ws", max_message=0)


def test_websocket_options_reach_the_server_route():
    r = APIRouter()

    @r.websocket("/small", max_message=8)
    async def small(websocket):
        await websocket.accept()
        try:
            await websocket.receive_text()
        except httpp.WebSocketDisconnect as exc:
            seen.append(exc.code)

    seen = []
    app = Server(openapi_url=None)
    app.include(r)
    with TestClient(app) as c:
        with c.websocket_connect("/small") as ws:
            ws.send_text("x" * 100)  # over max_message: the server closes with 1009
            with pytest.raises(httpp.WebSocketDisconnect) as info:
                ws.receive_text()
            assert info.value.code == 1009
        assert wait_for(lambda: len(seen) == 1)  # the handler is told the connection is gone


def test_a_router_websocket_handler_stays_directly_callable_and_keeps_its_name():
    r = APIRouter()

    @r.websocket("/ws")
    async def handler(websocket):
        await websocket.accept()

    assert handler.__name__ == "handler"
    assert [e.path for e in r.websocket_routes] == ["/ws"]
    assert r.routes == []  # HTTP routes are listed apart


# ---- dependencies on WebSocket routes ----

def user_from_token(request: httpp.ServerRequest):
    token = request.headers.get("x-token")
    if token != "secret":
        raise HTTPException(403, "bad token")
    return {"name": "ann"}


def test_a_dependency_value_reaches_the_handler_and_an_http_exception_in_it_refuses_the_upgrade():
    app = Server(openapi_url=None)

    @app.websocket("/me")
    async def me(websocket, user=Depends(user_from_token)):
        await websocket.accept()
        await websocket.send_json(user)

    with TestClient(app) as c:
        with c.websocket_connect("/me", headers={"X-Token": "secret"}) as ws:
            assert ws.receive_json() == {"name": "ann"}
        with pytest.raises(httpp.WebSocketDisconnect) as info:
            with c.websocket_connect("/me", headers={"X-Token": "wrong"}):
                pass
        assert "403" in info.value.reason


def test_async_and_sync_dependencies_and_sub_dependencies_work_and_are_cached_per_connection():
    calls = []

    def base():
        calls.append("base")
        return 10

    async def doubled(b: int = Depends(base)):
        return b * 2

    def tripled(b: int = Depends(base)):
        return b * 3

    app = Server(openapi_url=None)

    @app.websocket("/sum")
    async def total(websocket, d=Depends(doubled), t=Depends(tripled)):
        await websocket.accept()
        await websocket.send_json(d + t)

    with TestClient(app) as c:
        for _ in range(2):
            with c.websocket_connect("/sum") as ws:
                assert ws.receive_json() == 50
        assert calls == ["base", "base"]  # once per connection, not once per user of it


def test_a_dependency_reads_path_and_query_parameters_and_bad_ones_refuse_the_upgrade_with_422():
    def limit(n: int, scale: int = 1):
        return n * scale

    app = Server(openapi_url=None)

    @app.websocket("/lim/{n}")
    async def lim(websocket, n: int, value=Depends(limit)):
        await websocket.accept()
        await websocket.send_json(value)

    with TestClient(app) as c:
        with c.websocket_connect("/lim/4?scale=3") as ws:
            assert ws.receive_json() == 12
        with pytest.raises(httpp.WebSocketDisconnect) as info:
            with c.websocket_connect("/lim/4?scale=abc"):
                pass
        assert "422" in info.value.reason


def test_a_yield_dependency_is_closed_after_the_handler_ends_also_when_it_raised():
    events = []

    def resource():
        events.append("open")
        try:
            yield "r"
        finally:
            events.append("closed")

    async def aresource():
        events.append("aopen")
        try:
            yield "a"
        finally:
            events.append("aclosed")

    app = Server(openapi_url=None)

    @app.websocket("/ok")
    async def ok(websocket, r=Depends(resource), a=Depends(aresource)):
        await websocket.accept()
        await websocket.send_text(r + a)
        events.append("handler-end")

    @app.websocket("/boom")
    async def boom(websocket, r=Depends(resource)):
        await websocket.accept()
        raise RuntimeError("handler failed")

    with TestClient(app) as c:
        with c.websocket_connect("/ok") as ws:
            assert ws.receive_text() == "ra"
        assert wait_for(lambda: "aclosed" in events and "closed" in events)
        assert events.index("handler-end") < events.index("aclosed") < events.index("closed")  # last opened, first closed
        events.clear()
        with c.websocket_connect("/boom") as ws:
            with pytest.raises(httpp.WebSocketDisconnect) as info:
                ws.receive_text()
            assert info.value.code == 1011
        assert wait_for(lambda: events == ["open", "closed"])


def test_a_dependency_can_ask_for_the_websocket_and_accept_the_connection_itself():
    async def accepted(websocket):
        await websocket.accept()
        return "ready"

    app = Server(openapi_url=None)

    @app.websocket("/dep-accept")
    async def handler(websocket, state=Depends(accepted)):
        await websocket.send_text(state)

    with TestClient(app) as c:
        with c.websocket_connect("/dep-accept") as ws:
            assert ws.receive_text() == "ready"


def test_a_dependency_that_closes_the_websocket_before_the_handler_stops_the_handler_from_running():
    ran = []

    async def gate(websocket, request: httpp.ServerRequest):
        if request.headers.get("x-ok") != "1":
            await websocket.close(1008)
            raise httpp.WebSocketDisconnect(1008)
        await websocket.accept()

    app = Server(openapi_url=None)

    @app.websocket("/gated")
    async def handler(websocket, _=Depends(gate)):
        ran.append(1)
        await websocket.send_text("in")

    with TestClient(app) as c:
        with c.websocket_connect("/gated", headers={"X-Ok": "1"}) as ws:
            assert ws.receive_text() == "in"
        with pytest.raises(httpp.WebSocketDisconnect):
            with c.websocket_connect("/gated") as ws:
                ws.receive_text()
        assert ran == [1]


def test_unsupported_things_on_a_websocket_route_are_still_refused_at_registration():
    app = Server(openapi_url=None)

    with pytest.raises(TypeError, match="response"):
        @app.websocket("/a")
        async def a(websocket, response):
            pass

    with pytest.raises(TypeError, match="async"):
        @app.websocket("/b")
        def b(websocket):
            pass


def test_a_dependency_that_wants_the_websocket_is_refused_on_an_http_route():
    async def needs_ws(websocket):
        return 1

    app = Server(openapi_url=None)
    with pytest.raises(TypeError, match="websocket"):
        @app.get("/h")
        async def h(x=Depends(needs_ws)):
            return "no"


def test_websocket_routes_stay_out_of_the_openapi_document():
    r = APIRouter("/r")
    echo_room(r)

    @r.get("/http")
    def http():
        return "x"

    app = Server()
    app.include(r)
    paths = app.openapi()["paths"]
    assert "/r/http" in paths and not any("{room}" in p for p in paths)
