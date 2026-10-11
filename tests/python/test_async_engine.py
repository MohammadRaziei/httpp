"""Request.run_async() and AsyncClient run on the C++ network thread: no thread per request."""

import asyncio
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import AsyncClient, Request, Server


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


def slow_app(delay=0.3):
    app = Server()

    @app.get("/slow")
    def slow():
        time.sleep(delay)
        return "done"

    @app.get("/n/{i}")
    def n(i: int):
        return str(i)

    return app


def test_hundreds_in_flight_add_no_python_threads():
    app = slow_app()
    with running(app) as base:

        async def main():
            before = threading.active_count()
            tasks = [asyncio.ensure_future(Request(base + "/slow").run_async()) for _ in range(150)]
            await asyncio.sleep(0.1)  # all of them are in flight now
            during = threading.active_count()
            replies = await asyncio.gather(*tasks)
            return before, during, replies

        before, during, replies = asyncio.run(main())
    assert all(r.status == 200 and r.text == "done" for r in replies)
    # run_in_executor would have started up to ~32 threads here; the engine starts none
    assert during <= before + 1


def test_gather_returns_every_result_in_order():
    app = slow_app(0)
    with running(app) as base:

        async def main():
            return await asyncio.gather(*(Request(f"{base}/n/{i}").run_async() for i in range(300)))

        replies = asyncio.run(main())
    assert [r.text for r in replies] == [str(i) for i in range(300)]


def test_async_client_caps_requests_in_flight():
    app = Server()
    state = {"now": 0, "peak": 0}
    lock = threading.Lock()

    @app.get("/w")
    def w():
        with lock:
            state["now"] += 1
            state["peak"] = max(state["peak"], state["now"])
        time.sleep(0.1)
        with lock:
            state["now"] -= 1
        return "ok"

    with running(app) as base:

        async def main():
            async with AsyncClient(base, max_connections=3) as c:
                return await asyncio.gather(*(c.get("/w") for _ in range(12)))

        replies = asyncio.run(main())
    assert all(r.ok for r in replies)
    assert 1 <= state["peak"] <= 3


def test_async_client_options_and_per_call_arguments():
    app = Server()

    @app.route("/echo", methods=["GET", "POST"])
    def echo2(request):
        return {
            "auth": request.headers.get("authorization", ""),
            "x": request.headers.get("x-default", ""),
            "y": request.headers.get("x-call", ""),
            "method": request.method,
            "q": request.query.get("a", ""),
            "body": request.text,
        }

    with running(app) as base:

        async def main():
            async with AsyncClient(base + "/", headers={"X-Default": "d"}, auth=("u", "p"), timeout=5) as c:
                a = await c.get("/echo", params={"a": "1"}, headers={"X-Call": "c"})
                b = await c.post("/echo", json={"k": 1})
                return a, b

        a, b = asyncio.run(main())
    ja, jb = a.json(), b.json()
    assert ja["auth"] == "Basic dTpw" and ja["x"] == "d" and ja["y"] == "c" and ja["q"] == "1"
    assert jb["method"] == "POST" and jb["body"] == '{"k": 1}'


def test_follow_redirects_and_errors_come_back_as_responses():
    app = Server()

    @app.get("/a")
    def a():
        return httpp.redirect("/b")

    @app.get("/b")
    def b():
        return "arrived"

    with running(app) as base:

        async def main():
            async with AsyncClient(base) as plain, AsyncClient(base, follow_redirects=True) as follow:
                r1 = await plain.get("/a")
                r2 = await follow.get("/a")
            async with AsyncClient("http://127.0.0.1:1", timeout=2) as dead:
                r3 = await dead.get("/")
            return r1, r2, r3

        r1, r2, r3 = asyncio.run(main())
    assert r1.status == 302
    assert r2.status == 200 and r2.text == "arrived"
    assert r3.status == 0 and r3.error == "connection"
    with pytest.raises(httpp.errors.RequestError):
        r3.raise_for_status()


def test_a_bad_base_url_is_rejected_when_the_client_is_created():
    with pytest.raises(ValueError):
        AsyncClient("nonsense")
    with pytest.raises(ValueError):
        AsyncClient("http://localhost:1", max_connections=0)


def test_cancelling_a_task_is_harmless_and_the_late_response_is_ignored():
    app = slow_app(0.3)
    with running(app) as base:

        async def main():
            task = asyncio.ensure_future(Request(base + "/slow").run_async())
            await asyncio.sleep(0.05)
            task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await task
            await asyncio.sleep(0.5)  # the response arrives meanwhile: it must not blow up the loop
            return await Request(base + "/n/7").run_async()

        r = asyncio.run(main())
    assert r.text == "7"


def test_a_response_arriving_after_the_loop_closed_does_not_crash():
    app = slow_app(0.3)
    with running(app) as base:

        async def start_and_leave():
            asyncio.ensure_future(Request(base + "/slow").run_async())
            await asyncio.sleep(0.05)

        asyncio.run(start_and_leave())  # the loop is closed while the request is still in flight
        time.sleep(0.6)  # the callback fires into a closed loop: swallowed

        async def again():
            return await Request(base + "/n/3").run_async()

        assert asyncio.run(again()).text == "3"


def test_the_response_object_is_the_normal_one():
    app = Server()

    @app.get("/j")
    def j():
        return {"a": [1, 2, 3]}

    async def fetch(url):
        return await Request(url).run_async()  # run_async needs the running loop: call it inside a coroutine

    with running(app) as base:
        r = asyncio.run(fetch(base + "/j"))
    assert r.ok and r.json() == {"a": [1, 2, 3]}
    assert r.header("content-type").startswith("application/json")
