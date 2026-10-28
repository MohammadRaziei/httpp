"""Server(io_threads=N): several I/O threads sharing one port. On a free-threaded Python every I/O thread also
gets its own asyncio loop (a loop thread per lane); with a GIL there is one loop, because more would only
fight for it. A connection stays on its I/O thread and so on its loop."""

import asyncio
import sys
import threading
import time
from contextlib import contextmanager

import pytest

from httpp import Client, Server

GIL_FREE = hasattr(sys, "_is_gil_enabled") and not sys._is_gil_enabled()
no_reuseport = pytest.mark.skipif(sys.platform == "win32", reason="several I/O threads need SO_REUSEPORT")


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


def make_app(io_threads):
    app = Server(openapi_url=None, io_threads=io_threads)

    @app.get("/s")
    def sync_route():
        return "s"

    @app.get("/a")
    async def no_await():
        return "a"

    @app.get("/loop")
    async def loop_name():
        await asyncio.sleep(0)
        return threading.current_thread().name

    @app.get("/n/{i}")
    async def twice(i: int):
        await asyncio.sleep(0)
        return str(i * 2)

    return app


def test_io_threads_must_be_at_least_one():
    with pytest.raises(ValueError):
        Server(io_threads=0)


@no_reuseport
def test_every_kind_of_route_answers_correctly_with_several_io_threads():
    with running(make_app(3)) as base:
        results = []

        def user(k):
            c = Client(base)
            for i in range(10):
                results.append((c.get("/s").text, c.get("/a").text, c.get(f"/n/{k * 10 + i}").text, str((k * 10 + i) * 2)))

        users = [threading.Thread(target=user, args=(k,)) for k in range(12)]
        for t in users:
            t.start()
        for t in users:
            t.join()
    assert len(results) == 120
    assert all(s == "s" and a == "a" and got == want for s, a, got, want in results)


@no_reuseport
def test_loops_follow_the_io_threads_only_without_a_gil():
    with running(make_app(4)) as base:
        names = set()
        for _ in range(40):  # a new connection each time: spread over the I/O threads
            names.add(Client(base).get("/loop").text)
    assert all(n.startswith("httpp-asyncio") for n in names)
    if GIL_FREE:
        assert len(names) >= 2
    else:
        assert names == {"httpp-asyncio"}


@no_reuseport
def test_a_connection_stays_on_one_loop():
    with running(make_app(4)) as base:
        c = Client(base)  # keep-alive: one connection
        assert len({c.get("/loop").text for _ in range(15)}) == 1


def test_set_io_threads_validates_and_chains():
    app = Server(openapi_url=None)
    with pytest.raises(ValueError):
        app.set_io_threads(0)
    assert app.set_io_threads(2) is app


@no_reuseport
def test_the_command_line_takes_io_threads(tmp_path):
    import os
    import socket
    import subprocess

    (tmp_path / "tapp.py").write_text("import httpp\napp = httpp.Server()\n\n@app.get('/')\nasync def hi():\n    return 'hi'\n")
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    proc = subprocess.Popen([sys.executable, "-m", "httpp", "run", "tapp:app", "--port", str(port), "--io-threads", "3",
                             "--log-level", "error"], cwd=tmp_path, env={**os.environ, "PYTHONPATH": str(tmp_path)})
    try:
        for _ in range(100):
            try:
                assert Client(f"http://127.0.0.1:{port}").get("/").text == "hi"
                break
            except Exception:
                time.sleep(0.1)
        else:
            raise AssertionError("the server did not answer")
    finally:
        proc.terminate()
        proc.wait(10)


def test_defaults_follow_the_machine(monkeypatch):
    monkeypatch.setattr("os.sched_getaffinity", lambda _pid: set(range(16)), raising=False)
    assert Server(openapi_url=None)._io_threads == (1 if GIL_FREE or sys.platform == "win32" else 4)
    monkeypatch.setattr("os.sched_getaffinity", lambda _pid: {0, 1}, raising=False)
    few = Server(openapi_url=None)
    assert few._io_threads == 1 and few._inline_async is True
    assert Server(openapi_url=None, inline_async=False)._inline_async is False


@pytest.mark.skipif(GIL_FREE, reason="the small starting pool is only for builds with a GIL")
def test_blocking_handlers_still_run_together_with_the_small_default_pool():
    app = Server(openapi_url=None, io_threads=1)

    @app.get("/b")
    def blocks():
        time.sleep(0.2)
        return "x"

    with running(app) as base:
        t0 = time.time()
        users = [threading.Thread(target=lambda: Client(base).get("/b")) for _ in range(24)]
        for t in users:
            t.start()
        for t in users:
            t.join()
        assert time.time() - t0 < 0.6  # serial would be 4.8 s; four workers alone 1.2 s
