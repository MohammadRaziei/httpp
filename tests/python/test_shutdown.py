"""Graceful shutdown (ADR-0038): Server.shutdown(), Ctrl+C / SIGTERM, httpp run --shutdown-timeout."""

import http.client
import os
import signal
import socket
import subprocess
import sys
import threading
import time

import pytest

import httpp
from httpp import Server

from test_run import child_env, free_port, wait_for

SLOW_APP = '''
import os, time
from httpp import Server

app = Server(openapi_url=None)

@app.on_shutdown
def bye():
    open(os.environ["BYE_DIR"] + "/bye", "w").write("ran")

@app.get("/ping")
def ping():
    return "pong"

@app.get("/slow")
def slow():
    time.sleep(float(os.environ.get("SLOW_SECONDS", "0.6")))
    return "finished"
'''


def test_shutdown_lets_an_in_flight_request_finish_and_then_returns():
    app = Server(openapi_url=None)
    started = threading.Event()

    @app.get("/slow")
    def slow():
        started.set()
        time.sleep(0.4)
        return "done"

    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    result = {}

    def client():
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        conn.request("GET", "/slow")
        r = conn.getresponse()
        result["status"], result["body"], result["conn"] = r.status, r.read(), r.getheader("Connection")

    c = threading.Thread(target=client)
    c.start()
    assert started.wait(2)
    app.shutdown(5)
    c.join(5)
    th.join(5)
    assert not th.is_alive() and not c.is_alive()
    assert result == {"status": 200, "body": b"done", "conn": "close"}


def test_shutdown_checks_its_argument():
    app = Server(openapi_url=None)
    for bad in (-1, float("nan")):
        with pytest.raises(ValueError):
            app.shutdown(bad)
    with pytest.raises(TypeError):
        app.shutdown("soon")
    app.shutdown(0)  # not listening: nothing happens


def test_run_validates_shutdown_timeout():
    app = Server(openapi_url=None)
    for bad in (-1, float("inf")):
        with pytest.raises(ValueError):
            httpp.run(app, shutdown_timeout=bad)
    with pytest.raises(TypeError):
        httpp.run(app, shutdown_timeout="10")


def _start(app_dir, *args, slow="0.6"):
    (app_dir / "myapp.py").write_text(SLOW_APP)
    env = child_env(BYE_DIR=str(app_dir), SLOW_SECONDS=slow)
    return subprocess.Popen([sys.executable, "-m", "httpp", "py", "run", "myapp:app", *args], cwd=app_dir, env=env)


def _get_raw(port, path, timeout=10):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
    return s


def _read_all(s):
    data = b""
    try:
        while chunk := s.recv(4096):
            data += chunk
    except OSError:
        pass
    return data


@pytest.mark.skipif(sys.platform == "win32", reason="SIGTERM")
def test_sigterm_lets_an_in_flight_request_finish(tmp_path):
    port = free_port()
    proc = _start(tmp_path, "--port", str(port))
    try:
        wait_for(lambda: socket.create_connection(("127.0.0.1", port), timeout=1).close())
        s = _get_raw(port, "/slow")
        time.sleep(0.2)  # the handler is running now
        proc.send_signal(signal.SIGTERM)
        data = _read_all(s)
        assert b"200 OK" in data and b"finished" in data and b"Connection: close" in data
        assert proc.wait(15) == 0
        assert (tmp_path / "bye").read_text() == "ran"
        with pytest.raises(OSError):  # nobody listens any more
            socket.create_connection(("127.0.0.1", port), timeout=1)
    finally:
        if proc.poll() is None:
            proc.kill()


@pytest.mark.skipif(sys.platform == "win32", reason="SIGTERM")
def test_a_second_ctrl_c_style_signal_is_not_needed_the_timeout_cuts_a_stuck_request(tmp_path):
    port = free_port()
    proc = _start(tmp_path, "--port", str(port), "--shutdown-timeout", "0.3", slow="3")
    try:
        wait_for(lambda: socket.create_connection(("127.0.0.1", port), timeout=1).close())
        s = _get_raw(port, "/slow")
        time.sleep(0.2)
        t0 = time.monotonic()
        proc.send_signal(signal.SIGTERM)
        data = _read_all(s)
        assert b"200 OK" not in data  # cut after 0.3 s, long before the handler's 3 s
        assert time.monotonic() - t0 < 2.0
        assert proc.wait(15) == 0
    finally:
        if proc.poll() is None:
            proc.kill()


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="workers need SO_REUSEPORT")
def test_workers_drain_on_sigterm_and_the_supervisor_waits_for_them(tmp_path):
    port = free_port()
    proc = _start(tmp_path, "--port", str(port), "--workers", "2")
    try:
        wait_for(lambda: socket.create_connection(("127.0.0.1", port), timeout=1).close())
        time.sleep(0.5)  # both workers are up
        s = _get_raw(port, "/slow")
        time.sleep(0.2)
        proc.send_signal(signal.SIGTERM)
        data = _read_all(s)
        assert b"200 OK" in data and b"finished" in data
        assert proc.wait(20) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
