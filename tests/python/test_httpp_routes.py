"""Tests for decorator-style routes on httpp.Server."""

import json
import sys
import threading
import time
from contextlib import contextmanager

import pytest

from httpp import Client, Request, Server


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


def test_listen_on_a_taken_port_raises():
    import socket

    blocker = socket.socket()
    blocker.bind(("127.0.0.1", 0))
    blocker.listen()
    try:
        with pytest.raises(OSError):
            Server().listen("127.0.0.1", blocker.getsockname()[1])
    finally:
        blocker.close()


def test_keyboard_interrupt_stops_listen():
    """Simulates Ctrl+C with interrupt_main (portable; real SIGINT can't be sent
    to a child on Windows). If listen() ever blocks the main thread outside
    Python again, the interrupt is never seen and the watchdog trips the timing
    assert instead of the test hanging."""
    import _thread
    import socket

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    app = Server()
    threading.Timer(0.3, _thread.interrupt_main).start()
    watchdog = threading.Timer(5, app.stop)
    watchdog.start()
    started = time.time()
    try:
        app.listen("127.0.0.1", port)  # must swallow KeyboardInterrupt and return
    finally:
        watchdog.cancel()
    assert time.time() - started < 3


def test_client_can_call_a_python_route_in_the_same_process():
    """Regression: Client.get used to hold the GIL, so the handler could never run."""
    app = Server()

    @app.get("/hi")
    def hi():
        return "hi"

    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    try:
        assert Client("127.0.0.1", port).get("/hi").body == "hi"
        assert Client.fetch(f"http://127.0.0.1:{port}/hi").body == "hi"
    finally:
        app.stop()
        th.join()


def test_second_server_on_a_served_port_fails_instead_of_sharing_it():
    """cpp-httplib's default SO_REUSEPORT let this succeed silently."""
    first = Server()
    port = first.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=first.listen_after_bind)
    th.start()
    try:
        with pytest.raises(OSError):
            Server().listen("127.0.0.1", port)
    finally:
        first.stop()
        th.join()


# ---- real async: awaiting requests must not occupy worker threads -----------


def test_async_handlers_do_not_hold_worker_threads():
    """100 requests that each await 0.5 s finish together in ~0.5 s. With one
    blocked thread per request they would need several rounds (the pool is a
    few dozen threads), i.e. 2 s or more."""
    import asyncio
    from concurrent.futures import ThreadPoolExecutor

    app = Server()

    @app.get("/wait")
    async def wait():
        await asyncio.sleep(0.5)
        return "ok"

    with running(app) as url:
        port = int(url.rsplit(":", 1)[1])
        started = time.time()
        with ThreadPoolExecutor(100) as ex:
            statuses = list(ex.map(lambda _: Client("127.0.0.1", port).get("/wait").status, range(100)))
        elapsed = time.time() - started
    assert statuses == [200] * 100
    assert elapsed < 1.5


def test_async_handlers_share_one_event_loop():
    """Loop-bound resources (aiohttp sessions, DB pools) only work if every
    request runs on the same loop."""
    import asyncio

    app = Server()
    loops = []  # keep references so ids can't be reused

    @app.get("/loop")
    async def loop_():
        loops.append(asyncio.get_running_loop())
        return "x"

    with running(app) as url:
        for _ in range(5):
            Request(url + "/loop").run()
    assert len({id(lp) for lp in loops}) == 1


def test_sync_and_async_routes_coexist_and_async_gets_params():
    app = Server()

    @app.get("/sync/{n}")
    def sync_(n: int):
        return {"sync": n}

    @app.get("/async/{n}")
    async def async_(n: int, request):
        return {"async": n, "path": request.path}, 202, {"X-A": "1"}

    with running(app) as url:
        assert json.loads(Request(url + "/sync/1").run().body) == {"sync": 1}
        res = Request(url + "/async/2").run()
        bad = Request(url + "/async/x").run()
    assert res.status == 202
    assert res.header("X-A") == "1"
    assert json.loads(res.body) == {"async": 2, "path": "/async/2"}
    assert bad.status == 422  # annotation conversion works for async too


def test_async_handler_exception_is_a_generic_500(capfd):
    app = Server()

    @app.get("/boom")
    async def boom():
        raise RuntimeError("secret internals")

    with running(app) as url:
        res = Request(url + "/boom").run()
    assert res.status == 500
    assert "secret" not in res.body
    assert "secret internals" in capfd.readouterr().err


def test_server_can_be_dropped_while_an_async_request_is_pending():
    import asyncio

    app = Server()
    started = threading.Event()

    @app.get("/slow")
    async def slow():
        started.set()
        await asyncio.sleep(0.5)
        return "late"

    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()

    def call():
        try:
            Client("127.0.0.1", port).get("/slow")
        except Exception:
            pass  # the connection is closed under it; that is the point

    client = threading.Thread(target=call)
    client.start()
    assert started.wait(5)
    app.stop()
    th.join()
    del app  # closes the pending connection
    client.join(5)
    assert not client.is_alive()
    time.sleep(0.7)  # the coroutine finishes afterwards and must find nothing to answer


# ---- event-driven connections (all platforms) --------------------------------

def test_idle_connections_do_not_block_other_requests():
    """300 connections that never send a byte must not use up the worker pool."""
    import socket

    try:  # POSIX only; Windows has no per-process fd limit to raise
        import resource

        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, 4096) if hard != resource.RLIM_INFINITY else 4096, hard))
    except ImportError:
        pass

    app = Server()

    @app.get("/k")
    def k():
        return "ok"

    idle = []
    try:
        with running(app) as url:
            port = int(url.rsplit(":", 1)[1])
            idle = [socket.create_connection(("127.0.0.1", port)) for _ in range(300)]
            started = time.time()
            res = Client("127.0.0.1", port).get("/k")
            elapsed = time.time() - started
        assert res.status == 200
        assert elapsed < 1.0
    finally:
        for s in idle:
            s.close()


def test_keep_alive_connection_is_reused_for_sync_and_async_routes():
    import asyncio
    import http.client

    app = Server()

    @app.get("/sync")
    def sync_():
        return "sync"

    @app.get("/async")
    async def async_():
        await asyncio.sleep(0.01)
        return "async"

    with running(app) as url:
        port = int(url.rsplit(":", 1)[1])
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        sock = None
        for path, want in (("/sync", "sync"), ("/async", "async"), ("/sync", "sync"), ("/async", "async")):
            conn.request("GET", path)
            resp = conn.getresponse()
            assert resp.read().decode() == want
            assert conn.sock is not None, "server closed the connection"  # http.client drops it on Connection: close
            sock = sock or conn.sock
            assert conn.sock is sock  # same TCP connection every time
        conn.close()
