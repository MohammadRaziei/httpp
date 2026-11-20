"""Download takes the network settings of Client and Request (ADR-0101): credentials, proxy, TLS, connect_timeout, max_time."""

import threading
import time
from contextlib import contextmanager

import pytest

from httpp import Download, Server


@contextmanager
def echo_server():
    app = Server()

    @app.get("/echo")
    def echo(request):
        return (request.headers.get("authorization") or "") + "|" + (request.headers.get("accept-encoding") or "")

    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        yield f"http://127.0.0.1:{port}"
    finally:
        app.stop()
        th.join()


def test_bearer_and_basic_auth_reach_the_server(tmp_path):
    with echo_server() as url:
        out = tmp_path / "bearer.bin"
        assert Download(url + "/echo", str(out)).enable_progress(False).bearer("tok").run().ok
        assert out.read_text() == "Bearer tok|identity"

        out = tmp_path / "basic.bin"
        assert Download(url + "/echo", str(out)).enable_progress(False).basic_auth("u", "p").run().ok
        assert out.read_text() == "Basic dTpw|identity"


def test_the_settings_chain_and_keep_the_download(tmp_path):
    d = Download("http://127.0.0.1:1/x", str(tmp_path / "x.bin"))
    assert d.connect_timeout(3).max_time(9).verify(True).ca_file("x.pem").timeout(5).max_redirects(2) is d
    assert d.timeout() == 5
    assert d.max_redirects() == 2


def test_a_download_follows_redirects_by_default():
    assert Download("http://127.0.0.1:1/x").max_redirects() == 20


def test_the_rules_are_the_ones_of_request(tmp_path):
    d = Download("http://127.0.0.1:1/x", str(tmp_path / "x.bin"))
    with pytest.raises(ValueError):
        d.max_time(-1)
    with pytest.raises(ValueError):
        d.connect_timeout(-1)
    with pytest.raises(ValueError):
        d.max_redirects(-2)
    with pytest.raises(ValueError):
        d.proxy("host:3128")


def test_auto_compression_is_not_for_a_download():
    d = Download("http://127.0.0.1:1/x")
    assert not hasattr(d, "auto_compression")
