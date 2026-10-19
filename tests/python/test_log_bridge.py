"""The library's C++ log goes through Python's `logging` by default (logger "httpp"), and that logger's level
decides what C++ produces (ADR-0018)."""

import logging
import logging.config
import socket
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Server, _log

LOGGER = logging.getLogger("httpp")


@pytest.fixture(autouse=True)
def fresh_bridge():
    _log.install()
    LOGGER.setLevel(logging.NOTSET)
    LOGGER.disabled = False
    yield
    LOGGER.setLevel(logging.NOTSET)
    LOGGER.disabled = False
    _log.install()


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


def messages(caplog):
    return [r.getMessage() for r in caplog.records if r.name == "httpp"]


def test_cpp_errors_reach_logging_with_no_setup(caplog):
    with socket.socket() as taken:
        taken.bind(("127.0.0.1", 0))
        taken.listen()
        with pytest.raises(Exception):
            Server().listen("127.0.0.1", taken.getsockname()[1])
    records = [r for r in caplog.records if r.name == "httpp"]
    assert any(r.levelno == logging.ERROR and "cannot listen" in r.getMessage() for r in records)


def test_requests_are_not_logged_while_the_logger_is_at_warning(caplog):
    with running(make_app()) as base:
        assert httpp.get(base + "/").status == 200
    assert not [m for m in messages(caplog) if m.startswith("GET ")]


def test_the_logger_level_turns_the_cpp_access_log_on_and_off(caplog):
    LOGGER.setLevel(logging.INFO)
    with running(make_app()) as base:
        assert httpp.get(base + "/?a=1").status == 200
        assert "GET /?a=1 200" in messages(caplog)
        assert any(m.startswith("listening on http://") for m in messages(caplog))
        caplog.clear()
        LOGGER.setLevel(logging.WARNING)
        assert httpp.get(base + "/").status == 200
    assert not messages(caplog)


def test_caplog_set_level_works_too(caplog):
    caplog.set_level(logging.INFO, logger="httpp")
    with running(make_app()) as base:
        assert httpp.get(base + "/x").status == 404
    assert "GET /x 404" in messages(caplog)


def test_dictconfig_controls_it(caplog):
    logging.config.dictConfig({"version": 1, "disable_existing_loggers": False,
                               "loggers": {"httpp": {"level": "INFO"}}})
    with running(make_app()) as base:
        assert httpp.get(base + "/").status == 200
    assert "GET / 200" in messages(caplog)


def test_set_log_level_is_the_logger_level():
    for name, number in (("trace", 5), ("debug", 10), ("info", 20), ("warning", 30), ("error", 40)):
        httpp.set_log_level(name)
        assert LOGGER.level == number
    httpp.set_log_level("off")
    assert LOGGER.level > logging.CRITICAL
    with pytest.raises(ValueError):
        httpp.set_log_level("loud")


@pytest.mark.parametrize("number, cpp", [
    (0, "trace"), (5, "trace"), (10, "debug"), (15, "info"), (20, "info"), (25, "warning"), (30, "warning"),
    (40, "error"), (50, "off"), (60, "off"),
])
def test_python_levels_map_to_the_cpp_level(number, cpp):
    assert _log.cpp_level_name(number) == cpp


def test_an_explicit_callback_replaces_the_bridge(caplog):
    got = []
    httpp.set_log_callback(lambda level, where, msg: got.append((level, msg)))
    LOGGER.setLevel(logging.INFO)
    with running(make_app()) as base:
        assert httpp.get(base + "/").status == 200
    assert ("info", "GET / 200") in got
    assert not messages(caplog)


def test_a_disabled_logger_is_silent(caplog):
    LOGGER.setLevel(logging.INFO)
    LOGGER.disabled = True  # what dictConfig does by default to loggers it does not mention
    with running(make_app()) as base:
        assert httpp.get(base + "/").status == 200
    assert not messages(caplog)
