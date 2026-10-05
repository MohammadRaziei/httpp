"""The client side: explained failures, binary bodies, request options, sessions, async, TLS."""

import asyncio
import json
import shutil
import socket
import subprocess
import sys
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Client, Server


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


def echo_app():
    app = Server()

    @app.route("/echo", methods=["GET", "POST", "PUT", "PATCH", "DELETE"])
    def echo(request):
        return {
            "method": request.method,
            "query": request.query,
            "headers": {k: request.headers.get(k) for k in ("content-type", "authorization", "cookie", "x-default")},
            "body": request.text,
        }

    @app.get("/api/echo")
    def api_echo(request):
        return {"path": request.path, "x-default": request.headers.get("x-default")}

    return app


def test_a_refused_connection_has_an_error_and_raise_for_status_explains_it():
    r = httpp.get("http://127.0.0.1:1/", timeout=3)
    assert r.status == 0 and r.failed and r.error == "connection" and not r.ok
    with pytest.raises(httpp.RequestError) as e:
        r.raise_for_status()
    assert e.value.response is r


def test_a_404_is_a_response_and_raise_for_status_gives_http_error():
    with running(Server()) as url:
        r = httpp.get(url + "/nothing")
    assert r.status == 404 and not r.failed and not r.ok and r.error is None
    with pytest.raises(httpp.HTTPError) as e:
        r.raise_for_status()
    assert e.value.response.status == 404


def test_raise_for_status_returns_the_response_when_all_is_well():
    app = Server()

    @app.get("/ok")
    def ok():
        return {"a": 1}

    with running(app) as url:
        assert httpp.get(url + "/ok").raise_for_status().json() == {"a": 1}


def test_binary_content_text_charset_and_json():
    payload = bytes(range(256)) * 4
    app = Server()

    @app.get("/bin")
    def binary():
        return payload, 200, {"Content-Type": "application/octet-stream"}

    @app.get("/latin")
    def latin():
        return "caf\xe9".encode("latin-1"), 200, {"Content-Type": "text/plain; charset=latin-1"}

    @app.get("/uni")
    def uni():
        return {"w": "سلام"}

    with running(app) as url:
        assert httpp.get(url + "/bin").content == payload  # byte-for-byte, no decoding
        assert httpp.get(url + "/latin").text == "café"  # the charset from Content-Type is honoured
        assert httpp.get(url + "/uni").json() == {"w": "سلام"}


def test_binary_request_body_survives():
    app = Server()

    @app.post("/len")
    def length(request):
        return {"n": len(request.body), "first": request.body[0], "has_nul": b"\x00" in request.body}

    with running(app) as url:
        r = httpp.post(url + "/len", data=b"\x80\x00\xff" * 100).json()
    assert r == {"n": 300, "first": 0x80, "has_nul": True}


def test_params_json_form_auth_token_cookies_headers():
    with running(echo_app()) as url:
        r = httpp.get(url + "/echo?keep=1", params={"q": "a b&c", "tag": ["x", "y"]}).json()
        assert r["query"]["keep"] == "1" and r["query"]["q"] == "a b&c"

        r = httpp.post(url + "/echo", json={"a": [1, "س"]}).json()
        assert r["headers"]["content-type"] == "application/json" and json.loads(r["body"]) == {"a": [1, "س"]}

        r = httpp.post(url + "/echo", form={"k": "v w"}).json()
        assert r["headers"]["content-type"] == "application/x-www-form-urlencoded"

        assert httpp.get(url + "/echo", auth=("user", "pass")).json()["headers"]["authorization"] == "Basic dXNlcjpwYXNz"
        assert httpp.get(url + "/echo", token="tok").json()["headers"]["authorization"] == "Bearer tok"
        assert httpp.get(url + "/echo", cookies={"a": "1", "b": "2"}).json()["headers"]["cookie"] == "a=1; b=2"
        assert httpp.get(url + "/echo", headers={"X-Default": "yes"}).json()["headers"]["x-default"] == "yes"


def test_client_session_base_path_default_headers_and_methods():
    with running(echo_app()) as url:
        c = Client(url + "/api", headers={"X-Default": "yes"})
        assert c.get("/echo").json() == {"path": "/api/echo", "x-default": "yes"}
        d = Client(url)
        for method in ("post", "put", "patch", "delete"):
            assert getattr(d, method)("/echo", data="x").json()["method"] == method.upper()
        assert d.head("/echo").status == 200
        assert d.set_header("X-Default", "later").get("/echo").json()["headers"]["x-default"] == "later"


def test_client_rejects_a_bad_base_url():
    with pytest.raises(ValueError):
        Client("ftp://nope")


def test_redirects_only_when_asked_for():
    app = Server()

    @app.get("/redir")
    def redir():
        return "", 302, {"Location": "/target"}

    @app.get("/target")
    def target():
        return "arrived"

    with running(app) as url:
        raw = httpp.get(url + "/redir")
        assert raw.status == 302 and raw.header("location") == "/target"
        assert httpp.get(url + "/redir", follow_redirects=True).text == "arrived"


def test_a_slow_server_times_out():
    app = Server()

    @app.get("/slow")
    def slow():
        time.sleep(2.5)
        return "late"

    with running(app) as url:
        started = time.time()
        r = httpp.get(url + "/slow", timeout=1)
        assert r.error == "timeout" and time.time() - started < 2.2
        with pytest.raises(httpp.Timeout):
            r.raise_for_status()


class CountingServer:
    """A bare TCP HTTP/1.1 server that counts the connections it accepts."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.accepted = 0
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            self.accepted += 1
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        buf = b""
        try:
            while True:
                while b"\r\n\r\n" not in buf:
                    data = conn.recv(4096)
                    if not data:
                        return
                    buf += data
                buf = buf.split(b"\r\n\r\n", 1)[1]
                conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
        except OSError:
            pass
        finally:
            conn.close()


def test_a_client_session_reuses_its_connection():
    srv = CountingServer()
    c = Client("127.0.0.1", srv.port)
    for _ in range(20):
        assert c.get("/").text == "ok"
    assert srv.accepted == 1  # twenty requests, one TCP connection
    srv.sock.close()


def test_async_client_overlaps_requests_and_honours_its_pool_bound():
    app = Server()
    state = {"now": 0, "peak": 0}

    @app.get("/wait")
    async def wait():
        state["now"] += 1
        state["peak"] = max(state["peak"], state["now"])
        await asyncio.sleep(0.2)
        state["now"] -= 1
        return "ok"

    async def main(url):
        async with httpp.AsyncClient(url, max_connections=8) as c:
            started = time.time()
            replies = await asyncio.gather(*(c.get("/wait") for _ in range(24)))
            return replies, time.time() - started

    with running(app) as url:
        replies, seconds = asyncio.run(main(url))
    assert [r.status for r in replies] == [200] * 24
    assert seconds < 1.5  # 24 x 0.2 s ran in 3 overlapping rounds, not one after another
    assert 2 <= state["peak"] <= 8


def test_request_run_async_is_awaitable():
    app = Server()

    @app.get("/x")
    def x():
        return {"ok": True}

    async def main(url):
        replies = await asyncio.gather(*(httpp.Request(url + "/x").run_async() for _ in range(5)))
        return [r.json() for r in replies]

    with running(app) as url:
        assert asyncio.run(main(url)) == [{"ok": True}] * 5


# ---- TLS, against a real server (the openssl command-line tool) ----------------

needs_openssl = pytest.mark.skipif(
    shutil.which("openssl") is None or sys.platform == "win32",
    reason="needs the openssl command-line tool",
)


@needs_openssl
def test_https_verifies_certificates_and_can_be_configured(tmp_path):
    key, cert = tmp_path / "k.pem", tmp_path / "c.pem"
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(cert),
         "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1", "-days", "1"],
        check=True, capture_output=True,
    )
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    server = subprocess.Popen(
        ["openssl", "s_server", "-accept", str(port), "-cert", str(cert), "-key", str(key), "-www", "-quiet"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        for _ in range(50):  # wait until it listens
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        url = f"https://localhost:{port}/"
        untrusted = httpp.get(url, timeout=5)
        assert untrusted.error == "tls", untrusted  # a self-signed certificate is refused by default
        with pytest.raises(httpp.TLSError):
            untrusted.raise_for_status()
        assert httpp.get(url, verify=False, timeout=5).status == 200  # explicit opt-out
        assert httpp.get(url, ca_file=str(cert), timeout=5).status == 200  # or trust that certificate
        assert Client(f"https://localhost:{port}", ca_file=str(cert), timeout=5).get("/").status == 200
    finally:
        server.kill()
        server.wait()
