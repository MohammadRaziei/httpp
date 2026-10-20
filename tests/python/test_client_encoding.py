"""The client asks for gzip / deflate and hands out the real body (blocking client, Request and AsyncClient)."""

import asyncio
import gzip
import threading
import time
import zlib
from contextlib import contextmanager

import pytest

import httpp
from httpp import Client, Server

TEXT = ("line of the body\n" * 500).encode()


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


def make_app():
    app = Server()
    seen = {}

    @app.get("/gzip")
    def gz(request):
        seen["accept-encoding"] = request.headers.get("accept-encoding")
        return gzip.compress(TEXT), 200, {"Content-Encoding": "gzip"}

    @app.get("/deflate")
    def df(request):
        return zlib.compress(TEXT), 200, {"Content-Encoding": "deflate"}

    @app.get("/raw-deflate")
    def raw(request):
        c = zlib.compressobj(6, zlib.DEFLATED, -15)
        return c.compress(TEXT) + c.flush(), 200, {"Content-Encoding": "deflate"}

    @app.get("/bad")
    def bad(request):
        return b"\x1f\x8b\x08\x00 this is not gzip", 200, {"Content-Encoding": "gzip"}

    @app.get("/bomb")
    def bomb(request):
        return gzip.compress(bytes(1 << 20)), 200, {"Content-Encoding": "gzip"}

    @app.get("/plain")
    def plain(request):
        return TEXT

    app.seen = seen
    return app


def test_client_session_decodes_gzip_and_deflate():
    app = make_app()
    with running(app) as url:
        c = Client(url)
        for path in ("/gzip", "/deflate", "/raw-deflate", "/plain"):
            r = c.get(path)
            assert r.ok and r.content == TEXT, path
        assert app.seen["accept-encoding"] == "gzip, deflate"


def test_request_decodes_and_a_caller_header_is_sent_as_is():
    app = make_app()
    with running(app) as url:
        assert httpp.Request(url + "/gzip").run().content == TEXT
        assert app.seen["accept-encoding"] == "gzip, deflate"
        httpp.Request(url + "/gzip").header("Accept-Encoding", "identity").run()
        assert app.seen["accept-encoding"] == "identity"


def test_async_request_and_async_client_decode():
    app = make_app()

    async def go(url):
        r = await httpp.Request(url + "/gzip").run_async()
        assert r.content == TEXT
        async with httpp.AsyncClient(url) as c:
            r = await c.get("/deflate")
            assert r.content == TEXT

    with running(app) as url:
        asyncio.run(go(url))


def test_corrupt_data_and_zip_bombs_are_errors():
    app = make_app()
    with running(app) as url:
        r = httpp.Request(url + "/bad").run()
        assert not r.ok and r.status == 0 and r.content == b""
        r = httpp.Request(url + "/bomb").max_response_size(10000).run()
        assert r.status == 413
        assert len(httpp.Request(url + "/bomb").max_response_size(2 << 20).run().content) == 1 << 20
