"""An `async def` handler that cannot suspend runs on the I/O thread, with no hop to the asyncio loop thread.

The rules (ADR-0023): the I/O thread must never wait for the GIL, and the code of a handler must never
run at the same time as other asyncio code. So the inline path is taken only when the loop thread is
asleep in its selector and no `def` handler is running; in every other case the request goes the old
way, through the inbox to the loop thread. Python 3.12+ only (it needs eager tasks).
"""

import asyncio
import dis
import json
import sys
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Client, Server

inline_only = pytest.mark.skipif(sys.version_info < (3, 12), reason="the inline path needs eager tasks (3.12+)")
LOOP = "httpp-asyncio"  # the name of the asyncio loop thread


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
    app = Server(openapi_url=None, io_threads=1, inline_async=True)
    state = {"flag": False, "later": False, "in_section": False}

    @app.get("/who")
    async def who():
        return threading.current_thread().name

    @app.get("/who-await")
    async def who_await():
        await asyncio.sleep(0)
        return threading.current_thread().name

    @app.get("/info")
    async def info():
        loop = asyncio.get_running_loop()
        task = asyncio.current_task()
        return {"loop": loop is sys.modules["httpp.server"]._loop, "task": task is not None}

    @app.get("/spawn")
    async def spawn():
        async def later():
            state["flag"] = True

        asyncio.create_task(later())  # fire and forget: the loop thread must wake up and run it
        return "ok"

    @app.get("/timer")
    async def timer():
        asyncio.get_running_loop().call_later(0.05, state.__setitem__, "later", True)
        return "ok"

    @app.get("/state")
    def read_state():
        return {"flag": state["flag"], "later": state["later"]}

    @app.get("/boom")
    async def boom():
        raise RuntimeError("boom")

    @app.get("/slow")
    async def slow():
        await asyncio.sleep(0)  # now on the loop thread
        state["in_section"] = True
        end = time.perf_counter() + 0.03
        while time.perf_counter() < end:  # a plain-Python section: nothing else may run asyncio code now
            pass
        state["in_section"] = False
        return "ok"

    @app.get("/probe")
    async def probe():
        return "1" if state["in_section"] else "0"

    @app.get("/sleepy")
    async def sleepy():
        await asyncio.sleep(0.05)
        return "awake"

    release, entered = threading.Event(), threading.Event()

    @app.get("/block")
    def block():
        entered.set()
        release.wait(10)
        return "released"

    app.release, app.entered = release, entered
    return app


@inline_only
def test_a_handler_that_cannot_suspend_runs_on_the_io_thread():
    with running(make_app()) as base:
        assert Client(base).get("/who").text != LOOP
        assert Client(base).get("/who-await").text == LOOP  # it can suspend: the loop thread, as before


@inline_only
def test_the_running_loop_and_the_current_task_are_the_servers():
    with running(make_app()) as base:
        assert Client(base).get("/info").json() == {"loop": True, "task": True}


@inline_only
def test_a_task_created_by_an_inline_handler_runs_without_any_other_traffic():
    with running(make_app()) as base:
        assert Client(base).get("/spawn").text == "ok"
        deadline = time.time() + 2
        while time.time() < deadline and not Client(base).get("/state").json()["flag"]:
            time.sleep(0.02)
        assert Client(base).get("/state").json()["flag"]


@inline_only
def test_a_timer_set_by_an_inline_handler_fires_on_time():
    with running(make_app()) as base:
        assert Client(base).get("/timer").text == "ok"
        time.sleep(0.4)  # the loop thread was asleep with no timeout when the timer was set
        assert Client(base).get("/state").json()["later"]


def test_a_failing_handler_answers_500_and_the_server_goes_on():
    with running(make_app()) as base:
        assert Client(base).get("/boom").status == 500
        assert Client(base).get("/who").status == 200  # the gate was given back


@inline_only
@pytest.mark.skipif(hasattr(sys, "_is_gil_enabled") and not sys._is_gil_enabled(), reason="without a GIL the I/O thread has nothing to wait for")
def test_a_running_def_handler_keeps_the_io_thread_away_from_the_gil():
    app = make_app()
    with running(app) as base:
        blocked = threading.Thread(target=lambda: Client(base).get("/block"))
        blocked.start()
        assert app.entered.wait(5)
        try:
            assert Client(base).get("/who").text == LOOP  # a hop, as before
        finally:
            app.release.set()
            blocked.join()
        assert Client(base).get("/who").text != LOOP  # idle again: inline


@inline_only
def test_inline_handlers_never_run_while_the_loop_thread_runs_code():
    with running(make_app()) as base:
        slow = [threading.Thread(target=lambda: Client(base).get("/slow")) for _ in range(8)]
        for t in slow:
            t.start()
        seen = []
        c = Client(base)
        while any(t.is_alive() for t in slow):
            seen.append(c.get("/probe").text)
        for t in slow:
            t.join()
        assert seen and set(seen) == {"0"}


@inline_only
def test_a_flood_of_inline_requests_does_not_starve_the_loop_thread():
    with running(make_app()) as base:
        stop = threading.Event()

        def flood():
            c = Client(base)
            while not stop.is_set():
                c.get("/who")

        threads = [threading.Thread(target=flood) for _ in range(3)]
        for t in threads:
            t.start()
        try:
            started = time.perf_counter()
            for _ in range(10):
                assert Client(base).get("/sleepy").text == "awake"
            assert time.perf_counter() - started < 3
        finally:
            stop.set()
            for t in threads:
                t.join()


def test_which_coroutines_can_suspend():
    can_not = httpp.server._cannot_suspend

    async def plain():
        return 1

    async def awaits():
        await asyncio.sleep(0)

    async def with_async_with(lock):
        async with lock:
            pass

    async def with_async_for(it):
        async for _ in it:
            pass

    async def awaits_in_a_comprehension(items):
        return [await x for x in items]

    class Callable_:
        async def __call__(self):
            return 1

    assert can_not(plain)
    assert not can_not(awaits)
    assert not can_not(with_async_with)
    assert not can_not(with_async_for)
    assert not can_not(awaits_in_a_comprehension)
    assert not can_not(Callable_())  # no code object to look at
    assert not can_not(lambda: 1)  # not a coroutine function at all
