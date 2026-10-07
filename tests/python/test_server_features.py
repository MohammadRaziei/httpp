"""Python parity for the C++ server features: settings, hooks, files, streaming, timeouts."""

import asyncio
import socket
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Request, ResponseHead, Server
from httpp.httpp_cy import Server as RawServer


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


def cookies_of(res):
    return [v for k, v in res.headers if k.lower() == "set-cookie"]


# --- helpers that need no server -------------------------------------------------------------


def test_set_cookie_builds_the_header_and_defaults_to_path_and_httponly():
    assert httpp.set_cookie("a", "1") == ("Set-Cookie", "a=1; Path=/; HttpOnly")
    assert httpp.set_cookie("b", "2", max_age=60, secure=True, http_only=False, same_site="lax",
                            domain="example.com") == (
        "Set-Cookie", "b=2; Path=/; Domain=example.com; Max-Age=60; Secure; SameSite=Lax")


@pytest.mark.parametrize("name, value", [
    ("a b", "1"), ("a;b", "1"), ("", "1"), ("a", "x y"), ("a", "x;y"), ("a", 'x"y'), ("a", "x,y"),
    ("a", "1\r\nSet-Cookie: evil=1"), ("a\r\nX", "1"),
])
def test_set_cookie_refuses_unsafe_names_and_values(name, value):
    with pytest.raises(ValueError):
        httpp.set_cookie(name, value)


def test_set_cookie_refuses_unsafe_attributes():
    with pytest.raises(ValueError):
        httpp.set_cookie("a", "1", path="/x; Secure")
    with pytest.raises(ValueError):
        httpp.set_cookie("a", "1", same_site="sometimes")


def test_redirect_builds_a_3xx_and_refuses_header_injection():
    assert httpp.redirect("/login") == ("", 302, [("Location", "/login")])
    assert httpp.redirect("/new", 301)[1] == 301
    with pytest.raises(ValueError):
        httpp.redirect("/x", 200)
    with pytest.raises(ValueError):
        httpp.redirect("/x\r\nSet-Cookie: evil=1")


def test_response_head_edits_headers_case_insensitively():
    head = ResponseHead(200, [("X-A", "1"), ("x-a", "2"), ("Other", "o")])
    assert head.header("X-A") == "1" and head.header("nope") is None
    head.set_header("X-a", "3")
    assert head.headers == [("Other", "o"), ("X-a", "3")]
    head.add_header("Set-Cookie", "a=1")
    head.add_header("Set-Cookie", "b=2")
    assert [v for k, v in head.headers if k == "Set-Cookie"] == ["a=1", "b=2"]
    with pytest.raises(ValueError):
        head.set_header("X-B", "a\r\nb")
    with pytest.raises(ValueError):
        head.add_header("bad name", "v")


def test_bad_settings_are_refused():
    for kwargs in ({"threads": 0}, {"max_threads": 4}, {"threads": 4, "max_threads": 2},
                   {"read_timeout": -1}, {"write_timeout": -1}, {"keep_alive_timeout": -1},
                   {"handler_timeout": -1}):
        with pytest.raises(ValueError):
            Server(**kwargs)
    with pytest.raises(OverflowError):
        Server(max_body=-1)
    with pytest.raises(ValueError):
        Server().error_handler(200)
    with pytest.raises(ValueError):
        Server().error_handler(lambda request: None)  # used as @app.error_handler without ()
    with pytest.raises(TypeError):
        Server().exception_handler(int)
    with pytest.raises(TypeError):
        Server().exception_handler()


# --- request.cookies, header lists ------------------------------------------------------------


def test_request_cookies_and_repeated_set_cookie_headers():
    app = Server()

    @app.get("/c")
    def c(request):
        return {"cookies": request.cookies}, 200, [httpp.set_cookie("a", "1"), httpp.set_cookie("b", "2")]

    with running(app) as url:
        res = Request(url + "/c").header("Cookie", "x=1; y=2; junk; z=3=4; x=ignored").run()
    assert res.json() == {"cookies": {"x": "1", "y": "2", "z": "3=4"}}
    assert cookies_of(res) == ["a=1; Path=/; HttpOnly", "b=2; Path=/; HttpOnly"]


def test_a_header_with_a_line_break_is_refused_not_sent():
    app = Server()

    @app.get("/inj")
    def inj():
        return "x", 200, {"X-A": "1\r\nX-Evil: yes"}

    with running(app) as url:
        res = Request(url + "/inj").run()
    assert res.status == 500
    assert res.header("X-Evil") is None


# --- hooks -----------------------------------------------------------------------------------


def hook_app():
    app = Server()

    @app.before_request
    def gate(request):
        if request.headers.get("x-key") != "secret" and request.path != "/open":
            return "denied", 401

    @app.after_request
    def stamp(request, response):
        response.add_header("X-After", "yes")

    @app.get("/sync")
    def sync():
        return "s"

    @app.get("/async")
    async def a():
        return "a"

    @app.get("/open")
    def open_():
        return "o"

    return app


def test_before_can_reject_and_after_adds_headers_to_sync_and_async_responses():
    with running(hook_app()) as url:
        denied = Request(url + "/sync").run()
        assert (denied.status, denied.body, denied.header("X-After")) == (401, "denied", "yes")
        # unmatched paths go through both hooks as well
        assert Request(url + "/missing").run().status == 401
        for path in ("/sync", "/async"):
            res = Request(url + path).header("X-Key", "secret").run()
            assert (res.status, res.header("X-After")) == (200, "yes")
        assert Request(url + "/open").run().status == 200


def test_hooks_may_be_async_and_run_in_registration_order():
    app = Server()
    order = []

    @app.before_request
    async def first(request):
        order.append("first")
        await asyncio.sleep(0)

    @app.before_request
    def second(request):
        order.append("second")
        if request.query.get("stop"):
            return httpp.redirect("/elsewhere")

    @app.after_request
    async def one(request, response):
        await asyncio.sleep(0)
        response.set_header("X-Order", "1")

    @app.after_request
    def two(request, response):
        response.set_header("X-Order", response.header("X-Order") + "2")

    @app.get("/r")
    def r():
        return "ok"

    with running(app) as url:
        res = Request(url + "/r").run()
        assert (res.status, res.header("X-Order")) == (200, "12")
        assert order == ["first", "second"]
        res = Request(url + "/r?stop=1").follow_redirects(False).run()
        assert (res.status, res.header("Location")) == (302, "/elsewhere")


def test_before_can_answer_with_any_route_return_value():
    app = Server()

    @app.before_request
    def maint(request):
        if request.path == "/maint":
            return {"down": True}, 503, [("Retry-After", "30")]

    with running(app) as url:
        res = Request(url + "/maint").run()
    assert (res.status, res.json(), res.header("Retry-After")) == (503, {"down": True}, "30")


def test_after_can_change_the_status_and_a_failing_hook_makes_a_500(capsys):
    app = Server()

    @app.after_request
    def teapot(request, response):
        if request.path == "/t":
            response.status = 418
        if request.path == "/boom":
            raise RuntimeError("after-hook failed")

    @app.get("/t")
    def t():
        return "t"

    @app.get("/boom")
    def boom():
        return "fine"

    with running(app) as url:
        assert Request(url + "/t").run().status == 418
        assert Request(url + "/boom").run().status == 500
    assert "after-hook failed" in capsys.readouterr().err


def test_a_header_injected_by_an_after_hook_by_hand_is_refused():
    app = Server()

    @app.after_request
    def evil(request, response):
        response.headers.append(("X-A", "1\r\nX-Evil: yes"))

    @app.get("/x")
    def x():
        return "x"

    with running(app) as url:
        res = Request(url + "/x").run()
    assert res.status == 500 and res.header("X-Evil") is None


def test_a_failing_before_hook_answers_500(capsys):
    app = Server()

    @app.before_request
    def broken(request):
        raise RuntimeError("before-hook failed")

    with running(app) as url:
        assert Request(url + "/anything").run().status == 500
    assert "before-hook failed" in capsys.readouterr().err


def test_error_handler_dresses_server_made_errors_but_not_deliberate_ones():
    app = Server()

    @app.error_handler(404)
    def nf(request):
        return {"error": "not found", "path": request.path}

    @app.get("/mine")
    def mine():
        return "my own 404 body", 404

    with running(app) as url:
        res = Request(url + "/missing").run()
        assert (res.status, res.json()) == (404, {"error": "not found", "path": "/missing"})
        assert res.header("Content-Type") == "application/json"
        assert Request(url + "/mine").run().body == "my own 404 body"


def test_error_handler_can_change_the_status_and_a_broken_one_keeps_the_plain_error(capsys):
    app = Server()
    app.error_handler(404)(lambda: ("gone", 410))
    with running(app) as url:
        res = Request(url + "/x").run()
    assert (res.status, res.body) == (410, "gone")

    app = Server()

    @app.error_handler(404)
    def broken():
        raise RuntimeError("handler broke")

    with running(app) as url:
        assert Request(url + "/x").run().status == 404
    assert "handler broke" in capsys.readouterr().err


class Mine(ValueError):
    pass


def exception_app():
    app = Server()

    @app.exception_handler(ValueError)
    def value_error(request, exc):
        return {"error": str(exc), "path": request.path}, 418

    @app.exception_handler(Mine)
    async def mine(request, exc):
        return "mine", 409

    @app.exception_handler(LookupError)
    def lookup(request, exc):
        return "lost"  # no status given: 500

    @app.exception_handler(ZeroDivisionError)
    def broken(request, exc):
        raise RuntimeError("handler itself broke")

    def raiser(exc):
        def route():
            raise exc

        def aroute():
            raise exc

        return route, aroute

    for name, exc in (("value", ValueError("bad")), ("mine", Mine()), ("lookup", KeyError("k")),
                      ("zero", ZeroDivisionError()), ("plain", RuntimeError("secret detail"))):
        sync_route, async_route = raiser(exc)

        async def a(_r=async_route):
            _r()

        app.get(f"/{name}")(sync_route)
        app.get(f"/a{name}")(a)
    return app


@pytest.mark.parametrize("prefix", ["/", "/a"], ids=["sync", "async"])
def test_exception_handlers_pick_the_most_specific_class(prefix, capsys):
    with running(exception_app()) as url:
        res = Request(url + prefix + "value").run()
        assert (res.status, res.json()) == (418, {"error": "bad", "path": prefix + "value"})
        assert Request(url + prefix + "mine").run().status == 409  # subclass beats ValueError
        res = Request(url + prefix + "lookup").run()
        assert (res.status, res.body) == (500, "lost")
        # a handler that raises, and an exception nobody handles: plain 500, nothing leaked
        for name in ("zero", "plain"):
            res = Request(url + prefix + name).run()
            assert (res.status, res.body) == (500, "Internal Server Error")
    err = capsys.readouterr().err
    assert "secret detail" in err  # logged for the developer ...
    assert "handler itself broke" in err


# --- files -----------------------------------------------------------------------------------


def test_a_file_is_streamed_from_disk_in_sync_and_async_routes(tmp_path):
    content = bytearray(b"q" * 3_000_000)
    content[1_234_567] = ord("X")
    big = tmp_path / "blob.bin"
    big.write_bytes(content)
    doc = tmp_path / "data.json"
    doc.write_text('{"a": 1}')
    app = Server()

    @app.get("/f")
    def f():
        return httpp.file(big)  # a pathlib.Path works too

    @app.get("/af")
    async def af():
        return httpp.file(str(big), "application/x-custom")

    @app.get("/tuple")
    def tup():
        return httpp.file(big), 206, {"X-A": "1"}

    @app.get("/json")
    def js():
        return httpp.file(doc)

    @app.get("/missing")
    def missing():
        return httpp.file(tmp_path / "no-such-file")

    with running(app) as url:
        for route in ("/f", "/af", "/tuple"):
            res = Request(url + route).run()
            assert res.status == (206 if route == "/tuple" else 200)
            assert res.content == bytes(content)
            assert res.header("Content-Length") == str(len(content))
        assert Request(url + "/f").run().header("Content-Type") == "application/octet-stream"
        assert Request(url + "/af").run().header("Content-Type") == "application/x-custom"
        assert Request(url + "/tuple").run().header("X-A") == "1"
        assert Request(url + "/json").run().header("Content-Type") == "application/json"
        assert Request(url + "/missing").run().status == 404


def test_file_refuses_a_content_type_with_a_line_break():
    with pytest.raises(ValueError):
        httpp.file("x.txt", "text/plain\r\nX-Evil: 1")


# --- streaming -------------------------------------------------------------------------------


def stream_app():
    app = Server()

    @app.get("/s")
    def s():
        def gen():
            for i in range(5):
                yield f"piece{i};" if i % 2 == 0 else f"piece{i};".encode()
                yield ""  # empty pieces are skipped

        return gen()

    @app.get("/as")
    async def a_s():
        async def gen():
            for i in range(5):
                await asyncio.sleep(0)
                yield f"piece{i};"

        return gen()

    @app.get("/sa")  # a sync route may hand back an async generator too
    def s_a():
        async def gen():
            yield "x;"
            await asyncio.sleep(0.01)
            yield b"y;"

        return gen()

    @app.get("/sse")
    def sse():
        def gen():
            yield "data: 1\n\n"
            yield "data: 2\n\n"

        return gen(), 207, {"Content-Type": "text/event-stream"}

    @app.get("/empty")
    def empty():
        return (x for x in ())

    @app.get("/boom")
    def boom():
        def gen():
            yield "a;"
            yield "b;"
            raise RuntimeError("stream broke")

        return gen()

    @app.get("/aboom")
    async def aboom():
        async def gen():
            yield "a;"
            raise RuntimeError("async stream broke")

        return gen()

    @app.get("/bad")
    def bad():
        def gen():
            yield "a;"
            yield 5  # not str/bytes

        return gen()

    return app


def test_a_body_can_be_streamed_in_pieces_in_sync_and_async_routes():
    want = "piece0;piece1;piece2;piece3;piece4;"
    with running(stream_app()) as url:
        for route in ("/s", "/as"):
            res = Request(url + route).run()
            assert (res.status, res.body) == (200, want)
            assert res.header("Transfer-Encoding") == "chunked"
            assert res.header("Content-Type").startswith("text/plain")
        assert Request(url + "/sa").run().body == "x;y;"
        res = Request(url + "/sse").run()
        assert (res.status, res.body, res.header("Content-Type")) == (
            207, "data: 1\n\ndata: 2\n\n", "text/event-stream")
        assert Request(url + "/empty").run().body == ""


@pytest.mark.parametrize("route, text", [("/boom", "stream broke"), ("/aboom", "async stream broke"),
                                         ("/bad", "yields str or bytes")])
def test_a_stream_that_fails_midway_drops_the_connection_instead_of_ending_cleanly(route, text, capsys):
    with running(stream_app()) as url:
        res = Request(url + route).timeout(5).run()
        # never a clean, complete-looking answer
        assert res.failed or res.content in (b"", b"a;", b"a;b;")
        assert not (res.ok and res.content == b"a;b;")
        # and the server is still alive for the next request
        assert Request(url + "/s").run().status == 200
    assert text in capsys.readouterr().err


def test_a_streamed_generator_is_released_when_the_response_ends_or_the_client_hangs_up():
    closed = []
    app = Server()

    def tracked(name, endless):
        def gen():
            try:
                yield "a;"
                while endless:
                    yield "x" * 65536
            finally:
                closed.append(name)  # runs when the generator is closed / dropped

        return gen()

    app.get("/done")(lambda: tracked("done", False))
    app.get("/endless")(lambda: tracked("endless", True))

    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        assert Request(f"http://127.0.0.1:{port}/done").run().body == "a;"
        sock = socket.create_connection(("127.0.0.1", port), timeout=3)
        sock.sendall(b"GET /endless HTTP/1.1\r\nHost: x\r\n\r\n")
        sock.recv(100_000)
        sock.close()  # hang up in the middle of an endless body
        deadline = time.monotonic() + 10
        while len(closed) < 2 and time.monotonic() < deadline:
            time.sleep(0.05)
    finally:
        app.stop()
        th.join()
    assert sorted(closed) == ["done", "endless"]


# --- timeouts and cancellation ---------------------------------------------------------------


def test_a_slow_async_handler_is_cancelled_and_answered_with_a_504():
    cancelled = threading.Event()
    app = Server(handler_timeout=0.3)

    @app.get("/slow")
    async def slow():
        try:
            await asyncio.sleep(10)
        except asyncio.CancelledError:
            cancelled.set()
            raise
        return "late"

    @app.get("/fast")
    async def fast():
        await asyncio.sleep(0.05)
        return "ok"

    @app.get("/own")
    async def own():
        # the handler's own timeout is its own error: a 500, not mistaken for ours
        await asyncio.wait_for(asyncio.sleep(10), 0.05)

    with running(app) as url:
        t0 = time.monotonic()
        res = Request(url + "/slow").timeout(10).run()
        took = time.monotonic() - t0
        assert (res.status, res.body) == (504, "Gateway Timeout")
        assert 0.25 <= took < 3
        assert cancelled.wait(2), "the coroutine was not cancelled"
        assert Request(url + "/fast").run().body == "ok"
        assert Request(url + "/own").run().status == 500


def test_handler_timeout_none_lets_an_async_handler_take_as_long_as_it_likes():
    app = Server(handler_timeout=None)

    @app.get("/slow")
    async def slow():
        await asyncio.sleep(0.6)
        return "done"

    with running(app) as url:
        res = Request(url + "/slow").timeout(10).run()
    assert (res.status, res.body) == (200, "done")


def test_the_cpp_timer_is_the_backstop_for_a_dispatcher_that_never_answers():
    app = RawServer()
    app.set_async_timeout(1)
    stash = []  # keep `done` alive: dropping it unanswered would just close the connection
    app.add_async_route("GET", "/never", lambda *args: stash.append(args[-1]))
    with running(app) as url:
        t0 = time.monotonic()
        res = Request(url + "/never").timeout(10).run()
        took = time.monotonic() - t0
        assert res.status == 504
        assert 0.9 <= took < 4
        stash[0](200, b"late", [])  # answering after the 504 is a harmless no-op


# --- settings --------------------------------------------------------------------------------


def test_thread_pool_size_caps_how_many_sync_handlers_run_at_once():
    app = Server(threads=2, max_threads=2)
    lock = threading.Lock()
    now = peak = 0

    @app.get("/w")
    def w():
        nonlocal now, peak
        with lock:
            now += 1
            peak = max(peak, now)
        time.sleep(0.15)
        with lock:
            now -= 1
        return "ok"

    statuses = []
    with running(app) as url:
        threads = [threading.Thread(target=lambda: statuses.append(httpp.get(url + "/w").status))
                   for _ in range(6)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    assert statuses == [200] * 6
    assert peak <= 2


def test_max_body_rejects_larger_requests():
    app = Server(max_body=1000)

    @app.post("/up")
    def up(request):
        return str(len(request.body))

    with running(app) as url:
        small = Request(url + "/up").method("POST").data("a" * 500).run()
        assert (small.status, small.body) == (200, "500")
        big = Request(url + "/up").method("POST").data("a" * 50_000).timeout(5).run()
        assert big.status == 413 or big.failed  # refused either way, never served


def served_before_close(port, limit=6):
    """How many requests one keep-alive connection gets answered."""
    answered = 0
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        for _ in range(limit):
            try:
                sock.sendall(b"GET /k HTTP/1.1\r\nHost: x\r\n\r\n")
                data = b""
                while not data.endswith(b"ok"):
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    data += chunk
            except OSError:
                break
            if not data.endswith(b"ok"):
                break
            answered += 1
    return answered


@pytest.mark.parametrize("kwargs, expected", [({"keep_alive_max": 2}, 2), ({}, 6)])
def test_keep_alive_max_limits_requests_per_connection(kwargs, expected):
    app = Server(**kwargs)

    @app.get("/k")
    def k():
        return "ok"

    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        assert served_before_close(port) == expected
    finally:
        app.stop()
        th.join()


# --- lifecycle -------------------------------------------------------------------------------


def lifecycle_app(events):
    app = Server()

    @app.on_startup
    def a():
        events.append("start-sync")

    @app.on_startup
    async def b():
        events.append("start-async")

    @app.on_shutdown
    def c():
        events.append("stop-1")

    @app.on_shutdown
    async def d():
        events.append("stop-2")

    return app


def test_startup_runs_before_serving_and_shutdown_after_in_reverse_order():
    events = []
    app = lifecycle_app(events)
    with running(app):
        assert events == ["start-sync", "start-async"]
    assert events == ["start-sync", "start-async", "stop-2", "stop-1"]


def test_listen_runs_the_lifecycle_hooks_too():
    events = []
    app = lifecycle_app(events)
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    th = threading.Thread(target=app.listen, args=("127.0.0.1", port))
    th.start()
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.05)
    assert events == ["start-sync", "start-async"]
    app.stop()
    th.join()
    assert events[-2:] == ["stop-2", "stop-1"]


def test_a_failing_startup_hook_stops_everything_and_a_failing_shutdown_hook_does_not(capsys):
    app = Server()
    ran = []

    @app.on_startup
    def boom():
        raise RuntimeError("startup failed")

    @app.on_shutdown
    def never():
        ran.append("never")

    app.bind_to_any_port("127.0.0.1")
    with pytest.raises(RuntimeError, match="startup failed"):
        app.listen_after_bind()
    assert ran == []  # nothing had started, so nothing to shut down

    app = Server()
    events = []
    app.on_shutdown(lambda: events.append("good"))

    @app.on_shutdown
    def bad():
        raise RuntimeError("shutdown failed")

    with running(app):
        pass
    assert events == ["good"]  # `bad` ran first (reverse order) and did not stop `good`
    assert "shutdown failed" in capsys.readouterr().err
