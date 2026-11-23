"""Server and client of ours, both on this computer, over every spelling of the loopback address: 127.0.0.1, ::1 and localhost.

localhost resolves to ::1, to 127.0.0.1 or to both (in either order), depending on the machine: a client that tries only the first
address, or a server that listens on only one family, fails there and nowhere else. These tests start our own server on a port the system
chose and ask it with our own client."""

import socket
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Server


def _has_ipv6_loopback():
    if not socket.has_ipv6:
        return False
    try:
        with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as s:
            s.bind(("::1", 0))
        return True
    except OSError:
        return False


needs_ipv6 = pytest.mark.skipif(not _has_ipv6_loopback(), reason="this computer has no IPv6 loopback address (::1)")


def _app():
    app = Server(openapi_url=None)

    @app.get("/ping")
    def ping():
        return "pong"

    @app.post("/echo")
    def echo(request):
        return request.body.decode()

    return app


@contextmanager
def serving(host):
    app = _app()
    port = app.bind_to_any_port(host)
    assert port > 0, f"cannot listen on {host}"
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        yield port
    finally:
        app.stop()
        th.join()


def _get(url):
    return httpp.Request(url, timeout=5, method="GET").run()


def test_ipv4_loopback_answers():
    with serving("127.0.0.1") as port:
        r = _get(f"http://127.0.0.1:{port}/ping")
        assert (r.status, r.text) == (200, "pong")


@needs_ipv6
def test_ipv6_loopback_answers():
    with serving("::1") as port:
        r = _get(f"http://[::1]:{port}/ping")
        assert (r.status, r.text) == (200, "pong")


@needs_ipv6
def test_a_post_body_survives_the_ipv6_loopback():
    with serving("::1") as port:
        body = b"\x00\x01 \xff" * 1000
        r = httpp.Request(f"http://[::1]:{port}/echo", data=body, timeout=5, method="POST").run()
        assert r.status == 200 and r.content == body


@pytest.mark.parametrize("host", ["127.0.0.1", pytest.param("::1", marks=needs_ipv6)])
def test_localhost_reaches_a_server_on_either_family(host):
    """The server listens on one address; `localhost` may resolve to the other one first, and the client must go on to the next."""
    with serving(host) as port:
        r = _get(f"http://localhost:{port}/ping")
        assert (r.status, r.text) == (200, "pong"), r


@pytest.mark.parametrize("host", ["127.0.0.1", pytest.param("::1", marks=needs_ipv6)])
def test_client_keeps_a_connection_alive_on_the_loopback(host):
    url_host = f"[{host}]" if ":" in host else host
    with serving(host) as port:
        client = httpp.Client(f"http://{url_host}:{port}")
        assert [client.get("/ping").status for _ in range(5)] == [200] * 5
