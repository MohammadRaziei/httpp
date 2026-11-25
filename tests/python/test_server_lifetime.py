"""A stopped server must be freed by reference counting alone (ADR-0112).

The core keeps every handler and hook it is given in app._handlers. A handler or hook that holds the app (a closure over it, a
bound method of it) is then a cycle: app -> _handlers -> handler -> app. Reference counting never frees it. The cycle collector
does, but on free-threaded CPython it runs late, and a run of many tests piled up hundreds of stopped servers with everything they
hold until the 32-bit Windows job ran out of address space. These tests switch the collector off, so they fail on any build."""

import asyncio
import gc
import pathlib
import tempfile
import threading
import time
import weakref
from contextlib import contextmanager

import pytest

from httpp import (APIRouter, BackgroundTasks, BasicAuthMiddleware, CORSMiddleware, Depends, GZipMiddleware, HTTPSRedirectMiddleware,
                   ProxyHeadersMiddleware, RateLimitMiddleware, Request, Server, Swagger, TrustedHostMiddleware, FileBrowser)
from httpp.testclient import TestClient


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


@pytest.fixture(autouse=True)
def collector_off():
    gc.collect()
    gc.disable()
    yield
    gc.enable()
    gc.collect()


def _sync_route(app):
    @app.get("/r")
    def r():
        return "ok"


def _async_route(app):
    @app.get("/r")
    async def r():
        return "ok"


def _router(app):
    sub = APIRouter(prefix="/s")

    @sub.get("/x")
    def x():
        return "x"

    app.include_router(sub)
    _sync_route(app)


def _before_hook(app):
    _sync_route(app)

    @app.before_request
    def hook(request):
        return None


def _after_hook(app):
    _sync_route(app)

    @app.after_request
    def hook(request, response):
        return None


def _error_handler(app):
    _sync_route(app)

    @app.error_handler(404)
    def handler(request):
        return {"error": "none"}, 404


def _exception_handler(app):
    _sync_route(app)

    @app.exception_handler(KeyError)
    def handler(request, exc):
        return {"error": "key"}, 418


def _websocket(app):
    _sync_route(app)

    @app.websocket("/ws")
    async def ws(websocket):
        await websocket.accept()


def _lifecycle(app):
    _sync_route(app)

    @app.on_startup
    def up():
        pass

    @app.on_shutdown
    def down():
        pass


def _dependency(app):
    def dep():
        return 1

    @app.get("/r")
    def r(value=Depends(dep)):
        return "ok"


def _background(app):
    @app.get("/r")
    def r(tasks: BackgroundTasks):
        tasks.add_task(lambda: None)
        return "ok"


def _directory(app):
    _sync_route(app)
    app.serve_directory("/files", tempfile.gettempdir())


def _swagger(app):
    _sync_route(app)
    Swagger("/openapi.json").mount(app, "/docs")


def _file_browser(app):
    _sync_route(app)
    FileBrowser(pathlib.Path(tempfile.gettempdir())).mount(app, "/files")


def _middleware(cls, **options):
    def setup(app):
        _sync_route(app)
        app.add_middleware(cls, **options)

    setup.__name__ = "_" + cls.__name__
    return setup


def _async_hooks(app):
    @app.before_request
    async def before(request):
        await asyncio.sleep(0)

    @app.after_request
    async def after(request, response):
        await asyncio.sleep(0)

    _async_route(app)


def _raising_routes(app):
    @app.exception_handler(KeyError)
    async def key_handler(request, exc):
        return {"error": "key"}, 418

    @app.error_handler(500)
    def server_error(request):
        return {"error": "500"}, 500

    @app.get("/r")
    def r():
        raise KeyError("x")


def _short_circuit(app):
    @app.before_request
    def hook(request):
        return {"down": True}, 503

    _sync_route(app)


# Every way to hand the server something it keeps. The request goes to /r: the paths that run at request time (async hooks, error
# handlers, a hook that answers for the route) must not leave a cycle either.
SETUPS = [_sync_route, _async_route, _router, _before_hook, _after_hook, _error_handler, _exception_handler, _websocket, _lifecycle,
          _dependency, _background, _directory, _async_hooks, _raising_routes, _short_circuit, _swagger, _file_browser,
          _middleware(CORSMiddleware, allow_origins=["*"]), _middleware(TrustedHostMiddleware), _middleware(GZipMiddleware),
          _middleware(HTTPSRedirectMiddleware), _middleware(ProxyHeadersMiddleware),
          _middleware(BasicAuthMiddleware, users={"a": "b"}), _middleware(RateLimitMiddleware, requests_per_second=100.0, burst=10)]


@pytest.mark.parametrize("setup", SETUPS, ids=lambda f: f.__name__.strip("_"))
def test_a_stopped_server_is_freed_without_the_cycle_collector(setup):
    app = Server()
    setup(app)
    with running(app) as url:
        Request(url + "/r", timeout=5).run()  # the answer does not matter: some setups answer 401, 418, 503 on purpose
    alive = weakref.ref(app)
    del app
    assert alive() is None, "the stopped server is still alive: something it registered holds it (a cycle)"


def test_a_server_used_through_the_test_client_is_freed_too():
    app = Server()
    _sync_route(app)
    with TestClient(app) as client:
        assert client.get("/r").status_code == 200
    alive = weakref.ref(app)
    del app, client
    assert alive() is None


def test_a_server_with_an_opened_websocket_is_freed_too():
    app = Server()

    @app.websocket("/ws")
    async def ws(websocket):
        await websocket.accept()
        await websocket.send_text("echo " + await websocket.receive_text())

    with TestClient(app) as client:
        with client.websocket_connect("/ws") as sock:
            sock.send_text("hi")
            assert sock.receive_text() == "echo hi"
    alive = weakref.ref(app)
    del app, ws, client, sock
    assert alive() is None


def test_a_hook_still_works_while_its_server_is_alive():
    app = Server()
    seen = []

    @app.before_request
    def before(request):
        seen.append(request.path)

    @app.after_request
    def after(request, response):
        response.headers.append(("X-After", "1"))

    _sync_route(app)
    with running(app) as url:
        res = Request(url + "/r").run()
    assert (res.status, res.header("X-After"), seen) == (200, "1", ["/r"])
