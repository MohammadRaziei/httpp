"""Handlers get only what they ask for: the request is shared with C++ and read on demand."""

import threading
import time
from contextlib import contextmanager

import pytest

import httpp.server as server_module
from httpp import Request, Server, ServerRequest


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


@pytest.fixture
def built(monkeypatch):
    """Counts the ServerRequest objects built (however they are made)."""
    count = []
    real_init = ServerRequest.__init__
    real_from_raw = ServerRequest._from_raw.__func__

    def init(self, *args, **kwargs):
        count.append("init")
        real_init(self, *args, **kwargs)

    def from_raw(cls, raw):
        count.append("raw")
        return real_from_raw(cls, raw)

    monkeypatch.setattr(ServerRequest, "__init__", init)
    monkeypatch.setattr(ServerRequest, "_from_raw", classmethod(from_raw))
    return count


def test_a_handler_without_request_never_gets_a_ServerRequest_built(built):
    app = Server()

    @app.get("/plain")
    def plain():
        return "p"

    @app.get("/plain-async")
    async def plain_async():
        return "pa"

    @app.get("/item/{item_id}")
    def item(item_id: int):
        return {"id": item_id}

    @app.get("/item-async/{item_id}")
    async def item_async(item_id: int):
        return {"id": item_id}

    with running(app) as url:
        assert Request(url + "/plain").run().body == "p"
        assert Request(url + "/plain-async").run().body == "pa"
        assert Request(url + "/item/7").run().json() == {"id": 7}
        assert Request(url + "/item-async/8").run().json() == {"id": 8}
    assert built == []


def test_a_handler_with_request_gets_exactly_one(built):
    app = Server()

    @app.get("/r")
    def r(request):
        return request.path

    @app.get("/ra")
    async def ra(request):
        return request.path

    with running(app) as url:
        assert Request(url + "/r").run().body == "/r"
        assert Request(url + "/ra").run().body == "/ra"
    assert built == ["raw", "raw"]


@pytest.mark.parametrize("prefix", ["", "/async"])
def test_every_field_of_the_request_is_there(prefix):
    app = Server()
    seen = {}

    def read(request, who):
        seen.update(method=request.method, path=request.path, body=request.body, text=request.text,
                    headers=request.headers, query=request.query, params=request.path_params,
                    cookies=request.cookies, who=who)
        return "ok"

    @app.post("/p/{who}")
    def sync(request, who):
        return read(request, who)

    @app.post("/async/p/{who}")
    async def asynchronous(request, who):
        return read(request, who)

    with running(app) as url:
        res = (Request(url + f"{prefix}/p/caf%C3%A9?a=1&a=2&b=x%20y").method("POST")
               .header("X-Token", "abc").header("Cookie", "s=1; t=2")
               .content_type("text/plain").data("hello").run())
    assert res.status == 200
    assert seen["method"] == "POST"
    assert seen["path"] == f"{prefix}/p/café"
    assert seen["body"] == b"hello" and seen["text"] == "hello"
    assert seen["headers"]["x-token"] == "abc"  # lower-cased names
    assert seen["query"] == {"a": "1", "b": "x y"}  # the first value wins
    assert seen["params"] == {"who": "café"} and seen["who"] == "café"
    assert seen["cookies"] == {"s": "1", "t": "2"}


@pytest.mark.parametrize("prefix", ["", "/async"])
def test_the_request_stays_valid_after_the_handler_returned(prefix):
    app = Server()
    kept = []

    @app.get("/keep/{n}")
    def sync(request, n):
        kept.append(request)
        return "k"

    @app.get("/async/keep/{n}")
    async def asynchronous(request, n):
        kept.append(request)
        return "k"

    with running(app) as url:
        Request(url + f"{prefix}/keep/5?q=1").header("X-A", "b").run()
    request = kept[0]  # read only now, long after the response (and the server) are gone
    assert request.path == f"{prefix}/keep/5"
    assert request.query == {"q": "1"}
    assert request.headers["x-a"] == "b"
    assert request.path_params == {"n": "5"}


@pytest.mark.parametrize("prefix", ["", "/async"])
def test_an_exception_handler_gets_the_whole_request(prefix):
    app = Server()
    seen = {}

    @app.exception_handler(ValueError)
    def handle(request, exc):
        seen.update(path=request.path, h=request.headers.get("x-h"), q=request.query, params=request.path_params)
        return {"error": str(exc)}, 418

    @app.get("/boom/{n}")
    def sync(n):
        raise ValueError("bad " + n)

    @app.get("/async/boom/{n}")
    async def asynchronous(n):  # takes no `request`, yet the exception handler still sees it all
        raise ValueError("bad " + n)

    with running(app) as url:
        res = Request(url + f"{prefix}/boom/3?z=9").header("X-H", "v").run()
    assert res.status == 418 and res.json() == {"error": "bad 3"}
    assert seen == {"path": f"{prefix}/boom/3", "h": "v", "q": {"z": "9"}, "params": {"n": "3"}}


def test_a_path_parameter_named_request_does_not_replace_the_request():
    app = Server()

    @app.get("/x/{request}")
    def handler(request):
        return type(request).__name__

    with running(app) as url:
        assert Request(url + "/x/abc").run().body == "ServerRequest"


def test_kwargs_handlers_get_every_path_parameter():
    app = Server()

    @app.get("/k/{a}/{b}")
    async def handler(**kwargs):
        return kwargs

    with running(app) as url:
        assert Request(url + "/k/1/2").run().json() == {"a": "1", "b": "2"}


def test_a_timed_out_async_handler_is_cancelled_and_answered_504():
    import asyncio

    app = Server(handler_timeout=0.3)
    state = {"cancelled": False}

    @app.get("/slow")
    async def slow():
        try:
            await asyncio.sleep(5)
        except asyncio.CancelledError:
            state["cancelled"] = True
            raise

    with running(app) as url:
        started = time.time()
        res = Request(url + "/slow").run()
        assert res.status == 504
        assert time.time() - started < 3
    assert state["cancelled"]


def test_the_timeout_timer_does_not_fire_for_a_fast_async_handler():
    app = Server(handler_timeout=0.3)

    @app.get("/fast")
    async def fast():
        return "ok"

    with running(app) as url:
        assert Request(url + "/fast").run().body == "ok"
        time.sleep(0.6)  # the cancelled timer must not disturb the next request
        assert Request(url + "/fast").run().body == "ok"
