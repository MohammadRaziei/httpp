"""Every path argument takes a pathlib.Path as well as a str (ADR-0055): the C++ side takes httpp::path, shaped like pathlib."""

import hashlib
import json
import threading
import time
from contextlib import contextmanager
from pathlib import Path

import pytest

import httpp
from httpp import Download, Server
from httpp.utils import sha256_file

BODY = b"pathlib payload"


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


def test_download_takes_a_path_in_the_constructor_and_in_output(tmp_path):
    app = Server()

    @app.get("/f.bin")
    def f():
        return BODY

    with running(app) as url:
        first = tmp_path / "first.bin"
        assert Download(f"{url}/f.bin", first).run().ok
        assert first.read_bytes() == BODY
        second = tmp_path / "second.bin"
        assert Download(f"{url}/f.bin").output(second).run().ok
        assert second.read_bytes() == BODY


def test_sha256_file_takes_a_path(tmp_path):
    f = tmp_path / "x.bin"
    f.write_bytes(BODY)
    assert sha256_file(f) == sha256_file(str(f)) == hashlib.sha256(BODY).hexdigest()


def test_the_file_browser_root_and_file_take_a_path(tmp_path):
    (tmp_path / "a.txt").write_text("bee")
    app = Server()
    httpp.FileBrowser(tmp_path).mount(app, "/files")

    @app.get("/raw")
    def raw():
        return httpp.file(tmp_path / "a.txt")

    with running(app) as url:
        listing = json.loads(httpp.get(f"{url}/files/api/list").text)
        assert [e["name"] for e in listing["entries"]] == ["a.txt"]
        assert httpp.get(f"{url}/raw").text == "bee"


def test_set_log_file_takes_a_path(tmp_path):
    log = tmp_path / "log.txt"
    httpp.set_log_file(log)
    try:
        httpp.set_log_level("debug")
    finally:
        httpp.set_log_file("")
    assert log.exists()


def test_client_ca_file_takes_a_path(tmp_path):
    ca = Path(tmp_path) / "ca.pem"
    httpp.Client("http://127.0.0.1:1", ca_file=ca)
    httpp.AsyncClient("http://127.0.0.1:1", ca_file=ca)
    with pytest.raises(ValueError, match="no_such_cert"):
        Server(openapi_url=None).set_tls(tmp_path / "no_such_cert.pem", tmp_path / "no_such_key.pem")
