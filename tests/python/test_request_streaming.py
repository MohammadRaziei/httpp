"""Request streaming, saving to a file, retry and the Response fields that go with them (ADR-0077, ADR-0097, ADR-0102)."""

import asyncio
import os
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Server, Request, Client


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
    calls = {"flaky": 0}

    @app.get("/text")
    def text():
        return "hello world" * 1000

    @app.get("/missing", status_code=404)
    def missing():
        return "nothing here"

    @app.get("/flaky")
    def flaky(response):
        calls["flaky"] += 1
        if calls["flaky"] < 3:
            response.status = 503
            return "busy"
        return "ok"

    app.calls = calls
    return app


def test_on_body_streams_the_body_and_leaves_content_empty():
    with running(make_app()) as url:
        pieces = []
        res = Request(url + "/text").on_body(lambda piece: pieces.append(piece)).run()
        assert res.status == 200 and res.content == b""
        assert b"".join(pieces) == b"hello world" * 1000
        assert all(isinstance(p, bytes) for p in pieces)
        assert res.bytes_received == len(b"".join(pieces))
        assert res.elapsed_ms >= 0


def test_returning_false_from_on_body_cancels_the_transfer():
    with running(make_app()) as url:
        res = Request(url + "/text").on_body(lambda piece: False).run()
        assert res.error == "canceled"


def test_on_head_sees_the_status_and_false_skips_the_body():
    with running(make_app()) as url:
        seen, pieces = [], []
        res = Request(url + "/missing").on_head(lambda head: seen.append(head.status) or False).on_body(pieces.append).run()
        assert seen == [404] and pieces == []
        assert res.status == 404


def test_an_exception_in_a_callback_stops_the_transfer_and_is_raised_by_run():
    with running(make_app()) as url:
        def boom(piece):
            raise RuntimeError("boom")

        with pytest.raises(RuntimeError, match="boom"):
            Request(url + "/text").on_body(boom).run()


def test_an_exception_in_a_callback_is_raised_by_the_awaited_run_async():
    async def go(url):
        def boom(piece):
            raise KeyError("boom")

        with pytest.raises(KeyError):
            await Request(url + "/text").on_body(boom).run_async()
        got = []
        res = await Request(url + "/text").on_body(got.append).run_async()
        assert res.status == 200 and b"".join(got) == b"hello world" * 1000

    with running(make_app()) as url:
        asyncio.run(go(url))


def test_save_to_writes_the_file_and_reports_it(tmp_path):
    with running(make_app()) as url:
        target = tmp_path / "sub" / "out.txt"
        res = Request(url + "/text").save_to(target).create_dirs(True).run()
        assert res.ok and res.saved_as == str(target)
        assert target.read_text() == "hello world" * 1000
        assert res.bytes_received == len("hello world" * 1000)
        again = Request(url + "/text").save_to(target).overwrite(False).run()
        assert again.error == "file" and again.saved_as is None or again.error == "file"
        assert target.read_text() == "hello world" * 1000  # untouched


def test_save_in_names_the_file_from_the_url(tmp_path):
    with running(make_app()) as url:
        res = Request(url + "/text").save_in(tmp_path).run()
        assert res.ok and os.path.basename(res.saved_as) == "text"
        assert (tmp_path / "text").exists()


def test_retry_sends_again_after_a_503_and_on_retry_is_told():
    app = make_app()
    with running(app) as url:
        notes = []
        res = Request(url + "/flaky").retry(3).retry_delay(1).on_retry(lambda last, n, delay: notes.append((last.status, n, delay))).run()
        assert res.status == 200 and res.text == "ok"
        assert app.calls["flaky"] == 3
        assert [n for _, n, _ in notes] == [1, 2] and all(status == 503 for status, _, _ in notes)


def test_retry_delay_takes_fractions_of_a_second():
    # 0.15 s must not be rounded to 0 (the default 1, 2, 4 s back-off) or up to 1
    for make in (lambda url: Request(url).retry(2).retry_delay(0.15),
                 lambda url: Request(url, retry=2, retry_delay=0.15)):
        with running(make_app()) as url:
            start = time.monotonic()
            res = make(url + "/flaky").run()
            took = time.monotonic() - start
        assert res.status == 200
        assert 0.14 < took < 0.9


def test_a_client_takes_a_fraction_of_a_second_as_the_wait_and_checks_it():
    Client("http://127.0.0.1:1", retry=2, retry_delay=0.2)
    with pytest.raises(ValueError):
        Client("http://127.0.0.1:1", retry_delay=-0.5)
    with pytest.raises(ValueError):
        Request("http://127.0.0.1:1/").retry_delay(-0.1)
    with pytest.raises(ValueError):
        Request("http://127.0.0.1:1/").retry_delay(86401)  # more than a day


def test_a_timeout_and_a_max_time_take_a_fraction_of_a_second():
    app = Server()

    @app.get("/slow")
    def slow():
        time.sleep(1.5)
        return "late"

    with running(app) as url:
        for make in (lambda: Request(url + "/slow").timeout(0.3), lambda: Request(url + "/slow").max_time(0.3)):
            start = time.monotonic()
            res = make().run()
            took = time.monotonic() - start
            assert res.failed and res.error == "timeout"
            assert 0.25 < took < 0.9  # not rounded up to a whole second


def test_retry_values_are_checked():
    with pytest.raises(ValueError):
        Request("http://127.0.0.1:1/").retry(-1)
    with pytest.raises(ValueError):
        Request("http://127.0.0.1:1/").retry_delay(-1)
    Client("http://127.0.0.1:1", retry=2, retry_delay=1, retry_connrefused=True)
    with pytest.raises(ValueError):
        Client("http://127.0.0.1:1", retry=-1)


def test_a_clone_keeps_its_callbacks_and_has_its_own_errors():
    with running(make_app()) as url:
        got = []
        base = Request(url + "/text").on_body(got.append)
        copy = base.clone()
        assert copy.run().status == 200
        assert b"".join(got) == b"hello world" * 1000
        assert base.run().status == 200


def test_constructor_keywords_are_the_setters(tmp_path):
    with running(make_app()) as url:
        got = []
        res = Request(url + "/text", on_body=got.append, retry=1, retry_delay=1, max_response_size=10 ** 6).run()
        assert res.status == 200 and b"".join(got) == b"hello world" * 1000
        res = Request(url + "/text", save_to=str(tmp_path / "k.txt")).run()
        assert res.ok and (tmp_path / "k.txt").exists()
