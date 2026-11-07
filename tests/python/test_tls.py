"""The server over TLS from Python (ADR-0040): set_tls, request.secure, httpp run --ssl-*, the HTTPS redirect."""

import http.client
import os
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import HTTPSRedirectMiddleware, Server

from test_run import child_env, free_port, wait_for

CERTS = os.path.join(os.path.dirname(__file__), "certs")


def _have_certs():
    return os.path.exists(os.path.join(CERTS, "cert.pem"))


@pytest.fixture(scope="module")
def certs(tmp_path_factory):
    """A self-signed certificate for 127.0.0.1 and localhost, made with the openssl command line when it exists."""
    import shutil

    if shutil.which("openssl") is None:
        pytest.skip("needs the openssl command line to make a test certificate")
    d = tmp_path_factory.mktemp("certs")
    cert, key = str(d / "cert.pem"), str(d / "key.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-days", "2",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                    "-keyout", key, "-out", cert], check=True, capture_output=True)
    other = str(d / "other_key.pem")
    subprocess.run(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-out", other],
                   check=True, capture_output=True)
    return cert, key, other


@contextmanager
def running(app):
    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        yield port
    finally:
        app.stop()
        th.join()


def make_app():
    app = Server(openapi_url=None)

    @app.get("/who")
    def who(request):
        return f"{request.scheme} {request.secure}"

    return app


def https_get(port, cert, path="/who", headers=None):
    ctx = ssl.create_default_context(cafile=cert)
    conn = http.client.HTTPSConnection("127.0.0.1", port, context=ctx, timeout=5)
    conn.request("GET", path, headers=headers or {})
    r = conn.getresponse()
    out = (r.status, r.read().decode(), dict((k.lower(), v) for k, v in r.getheaders()))
    conn.close()
    return out


def test_https_with_set_tls_and_a_secure_request(certs):
    cert, key, _ = certs
    app = make_app()
    assert app.set_tls(cert, key) is app
    with running(app) as port:
        status, body, _ = https_get(port, cert)
        assert (status, body) == (200, "https True")


def test_plain_servers_see_http(certs):
    app = make_app()
    with running(app) as port:
        conn = http.client.HTTPConnection("127.0.0.1", port)
        conn.request("GET", "/who")
        assert conn.getresponse().read().decode() == "http False"


def test_set_tls_errors_name_the_file(certs):
    cert, key, other = certs
    app = Server(openapi_url=None)
    with pytest.raises(ValueError, match="no_such_cert"):
        app.set_tls("/no_such_cert.pem", key)
    with pytest.raises(ValueError, match="match"):
        app.set_tls(cert, other)
    with pytest.raises(ValueError):
        app.set_tls(key, key)
    app.set_tls(cert, key)


def test_run_checks_ssl_arguments():
    app = Server(openapi_url=None)
    with pytest.raises(ValueError):
        httpp.run(app, ssl_certfile="a.pem")  # the key is missing
    with pytest.raises(ValueError, match="nothing.pem"):
        httpp.run(app, ssl_certfile="nothing.pem", ssl_keyfile="nothing.pem")
    with pytest.raises(TypeError):
        httpp.run(app, ssl_certfile=1, ssl_keyfile="k.pem")


def test_https_redirect_middleware():
    app = make_app()
    app.add_middleware(HTTPSRedirectMiddleware, trust_forwarded_proto=True)
    with running(app) as port:
        conn = http.client.HTTPConnection("127.0.0.1", port)
        conn.request("GET", "/who?a=1")
        r = conn.getresponse()
        assert r.status == 307 and r.getheader("Location") == f"https://127.0.0.1:{port}/who?a=1"
        conn = http.client.HTTPConnection("127.0.0.1", port)
        conn.request("GET", "/who", headers={"X-Forwarded-Proto": "https"})
        assert conn.getresponse().status == 200


APP = '''
from httpp import Server

app = Server(openapi_url=None)

@app.get("/who")
def who(request):
    return request.scheme
'''


@pytest.mark.skipif(sys.platform == "win32", reason="SIGTERM")
@pytest.mark.parametrize("extra", [[], ["--reload"]], ids=["plain", "reload"])
def test_httpp_run_with_ssl_options(certs, tmp_path, extra):
    cert, key, other = certs
    (tmp_path / "myapp.py").write_text(APP)
    port = free_port()
    proc = subprocess.Popen([sys.executable, "-m", "httpp", "py", "run", "myapp:app", "--port", str(port),
                             "--ssl-certfile", cert, "--ssl-keyfile", key, *extra], cwd=tmp_path, env=child_env())
    try:
        status, body, _ = wait_for(lambda: https_get(port, cert))
        assert (status, body) == (200, "https")
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(20) == 0
    finally:
        if proc.poll() is None:
            proc.kill()


@pytest.mark.skipif(sys.platform == "win32", reason="SIGTERM")
def test_httpp_run_with_a_bad_key_exits_with_a_message(certs, tmp_path):
    cert, key, other = certs
    (tmp_path / "myapp.py").write_text(APP)
    r = subprocess.run([sys.executable, "-m", "httpp", "py", "run", "myapp:app", "--port", str(free_port()),
                        "--ssl-certfile", cert, "--ssl-keyfile", other], cwd=tmp_path, env=child_env(),
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 1 and "match" in r.stderr


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="workers need SO_REUSEPORT")
def test_workers_serve_tls(certs, tmp_path):
    cert, key, _ = certs
    (tmp_path / "myapp.py").write_text(APP)
    port = free_port()
    proc = subprocess.Popen([sys.executable, "-m", "httpp", "py", "run", "myapp:app", "--port", str(port), "--workers", "2",
                             "--ssl-certfile", cert, "--ssl-keyfile", key], cwd=tmp_path, env=child_env())
    try:
        assert wait_for(lambda: https_get(port, cert))[:2] == (200, "https")
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(20) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
