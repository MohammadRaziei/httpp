"""The library's own logging (httpp.set_log_level / set_log_file / set_log_callback)."""

import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Server


@pytest.fixture(autouse=True)
def restore_logging():
    yield
    httpp.set_log_callback(None)
    httpp.set_log_level("warning")


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

    @app.get("/")
    def index():
        return "ok"

    return app


def test_trace_level_reports_connection_events_to_a_callback():
    seen = []
    httpp.set_log_callback(lambda level, where, msg: seen.append((level, where, msg)))
    httpp.set_log_level("trace")
    with running(make_app()) as base:
        assert httpp.get(base + "/").status == 200
    assert seen, "nothing was logged"
    assert all(isinstance(x, str) for entry in seen for x in entry)
    assert {level for level, _, _ in seen} <= {"trace", "debug", "info", "warning", "error"}
    assert any("connection closed" in msg for _, _, msg in seen)


def test_info_level_logs_one_access_line_per_request():
    seen = []
    httpp.set_log_callback(lambda level, where, msg: seen.append((level, msg)))
    httpp.set_log_level("info")
    with running(make_app()) as base:
        assert httpp.get(base + "/?a=1").status == 200
        assert httpp.get(base + "/missing").status == 404
    assert ("info", "GET /?a=1 200") in seen
    assert ("info", "GET /missing 404") in seen


def test_default_level_is_quiet_for_a_normal_request():
    seen = []
    httpp.set_log_callback(lambda level, where, msg: seen.append(msg))
    with running(make_app()) as base:
        assert httpp.get(base + "/").status == 200
    assert seen == []


def test_off_silences_everything_and_none_restores_stderr():
    seen = []
    httpp.set_log_callback(lambda level, where, msg: seen.append(msg))
    httpp.set_log_level("off")
    with running(make_app()) as base:
        httpp.get(base + "/")
    assert seen == []
    httpp.set_log_callback(None)  # must not raise


def test_a_callback_that_raises_does_not_break_the_server():
    def broken(level, where, msg):
        raise RuntimeError("bad logger")

    httpp.set_log_callback(broken)
    httpp.set_log_level("trace")
    with running(make_app()) as base:
        assert httpp.get(base + "/").status == 200


def test_log_file(tmp_path):
    path = tmp_path / "httpp.log"
    httpp.set_log_file(str(path))
    httpp.set_log_level("trace")
    with running(make_app()) as base:
        httpp.get(base + "/")
    httpp.set_log_file("")  # closes and flushes
    text = path.read_text()
    assert "connection closed" in text and "httpp" in text


def test_bad_arguments_are_rejected():
    with pytest.raises(ValueError):
        httpp.set_log_level("loud")
    with pytest.raises(TypeError):
        httpp.set_log_callback("not callable")
    with pytest.raises(OSError):
        httpp.set_log_file("/no/such/dir/x.log")
