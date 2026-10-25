"""The injected `response` and BackgroundTasks: Python's side of C++ `response&` and `response::after_send`."""

import threading
import time
from contextlib import contextmanager

import pytest

from httpp import BackgroundTasks, Client, Server


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


def get(base, path, method="GET"):
    c = Client(base)
    return c.post(path, data="") if method == "POST" else c.get(path)


def test_the_injected_response_adds_status_headers_and_cookies_to_the_answer():
    app = Server(openapi_url=None)

    @app.get("/sync")
    def sync(response):
        response.status = 201
        response.add_header("X-One", "1")
        response.set_cookie("sid", "abc")
        return {"ok": True}

    @app.get("/async")
    async def a(response):
        response.add_header("X-Two", "2")
        return "hello"

    @app.get("/plain")
    def plain(response):
        return "nothing set"  # status stays 200, no extra headers

    with running(app) as base:
        r = get(base, "/sync")
        assert r.status == 201 and r.json() == {"ok": True}
        assert r.header("X-One") == "1"
        assert "sid=abc" in r.header("Set-Cookie")
        r = get(base, "/async")
        assert r.status == 200 and r.body == "hello" and r.header("X-Two") == "2"
        assert get(base, "/plain").status == 200


def test_response_status_wins_over_a_status_the_handler_returned():
    app = Server(openapi_url=None)

    @app.get("/x")
    def x(response):
        response.status = 202
        return "body", 400

    with running(app) as base:
        assert get(base, "/x").status == 202


def test_a_bad_header_on_the_injected_response_is_refused():
    app = Server(openapi_url=None)

    @app.get("/x")
    def x(response):
        response.headers.append(("X-Bad", "a\r\nb"))
        return "x"

    with running(app) as base:
        assert get(base, "/x").status == 500  # never sent


def test_background_tasks_run_after_the_answer_and_in_order():
    app = Server(openapi_url=None)
    seen, done = [], threading.Event()

    def slow(tag):
        time.sleep(0.3)
        seen.append(tag)

    async def last(tag):
        seen.append(tag)
        done.set()

    @app.get("/sync")
    def sync(tasks: BackgroundTasks):
        tasks.add_task(slow, "one")
        tasks.add_task(last, "two")  # a coroutine function is fine
        return "queued"

    with running(app) as base:
        r = get(base, "/sync")
        assert r.status == 200 and r.body == "queued"
        assert seen == []  # the client did not wait for them
        assert done.wait(3)
        assert seen == ["one", "two"]


def test_background_tasks_in_an_async_handler_and_a_failing_task_does_not_stop_the_next():
    app = Server(openapi_url=None)
    seen, done = [], threading.Event()

    def boom():
        raise RuntimeError("boom")

    def ok():
        seen.append("ok")
        done.set()

    @app.post("/a")
    async def a(background: BackgroundTasks):
        background.add_task(boom)
        background.add_task(ok)
        return {"ok": 1}

    with running(app) as base:
        assert get(base, "/a", "POST").json() == {"ok": 1}
        assert done.wait(3) and seen == ["ok"]


def test_nothing_runs_when_the_handler_raises():
    app = Server(openapi_url=None)
    ran = []

    @app.get("/x")
    def x(tasks: BackgroundTasks):
        tasks.add_task(ran.append, 1)
        raise ValueError("no")

    with running(app) as base:
        assert get(base, "/x").status == 500
        time.sleep(0.2)
    assert ran == []


def test_response_after_send_is_the_one_job_tasks_are_built_on():
    app = Server(openapi_url=None)
    done = threading.Event()

    @app.get("/x")
    def x(response):
        response.after_send = done.set
        return "x"

    with running(app) as base:
        assert get(base, "/x").body == "x"
        assert done.wait(3)


def test_these_arguments_are_not_query_parameters_in_the_openapi_document():
    app = Server()

    @app.get("/x")
    def x(response, tasks: BackgroundTasks, q: int = 1):
        return "x"

    params = app.openapi()["paths"]["/x"]["get"].get("parameters", [])
    assert [p["name"] for p in params] == ["q"]


def test_add_task_needs_a_callable():
    with pytest.raises(TypeError):
        BackgroundTasks().add_task("not callable")
