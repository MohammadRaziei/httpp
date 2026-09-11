"""Tests for the httpp Python bindings (Client + Server)."""

import time
import threading


def test_server_serves_a_directory(tmp_path):
    from httpp.httpp_cy import Server, Client

    (tmp_path / "index.html").write_text("hello from httpp")

    srv = Server()
    srv.serve_directory("/", str(tmp_path))
    port = srv.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=srv.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        cli = Client("127.0.0.1", port)
        res = cli.get("/index.html")
        assert res.status == 200
        assert res.body == "hello from httpp"
    finally:
        srv.stop()
        th.join()


def test_client_returns_404_for_missing_path():
    from httpp.httpp_cy import Server, Client

    srv = Server()
    port = srv.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=srv.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        cli = Client("127.0.0.1", port)
        res = cli.get("/nope")
        assert res.status == 404
        assert not res.ok
    finally:
        srv.stop()
        th.join()
