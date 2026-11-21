"""httpp.Directory and httpp.Swagger: the builder classes of ADR-0066 through the Python API (the HTTP behaviour is tested in C++)."""

import threading
import time
import urllib.error
import urllib.request
from contextlib import contextmanager

import pytest

from httpp import Directory, FileBrowser, Server, Swagger


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


def status(url):
    try:
        return urllib.request.urlopen(url).status
    except urllib.error.HTTPError as e:
        return e.code


def test_directory_settings_chain_and_change_the_answer(tmp_path):
    (tmp_path / "index.html").write_text("<h1>home</h1>")
    (tmp_path / ".secret").write_text("x")
    app = Server()
    d = Directory(tmp_path)
    assert d.index(False).show_hidden(False) is d
    d.mount(app, "/site")
    with running(app) as url:
        assert status(url + "/site/") == 404
        assert status(url + "/site/.secret") == 404
        assert status(url + "/site/index.html") == 200


def _get(url, **request_headers):
    """(status, headers, body) of a GET with the given request headers; a 304 or 206 is an answer, not an error."""
    req = urllib.request.Request(url, headers=request_headers)
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()


def test_directory_show_dates_false_sends_no_validator_and_never_answers_304(tmp_path):
    (tmp_path / "a.txt").write_text("hello world")
    app = Server()
    d = Directory(tmp_path)
    assert d.show_dates(False) is d
    d.mount(app, "/off")
    Directory(tmp_path).mount(app, "/on")
    Directory(tmp_path, show_dates=False).mount(app, "/kw")
    with running(app) as url:
        code, on, _ = _get(url + "/on/a.txt")
        assert code == 200 and on["ETag"] and on["Last-Modified"]
        assert _get(url + "/on/a.txt", **{"If-None-Match": on["ETag"]})[0] == 304
        for base in ("/off", "/kw"):
            code, off, body = _get(url + base + "/a.txt")
            assert code == 200 and body == b"hello world"
            assert off["ETag"] is None and off["Last-Modified"] is None and off["Cache-Control"] == "no-cache"
            assert _get(url + base + "/a.txt", **{"If-None-Match": "*"})[0] == 200
            assert _get(url + base + "/a.txt", **{"If-Modified-Since": "Wed, 01 Jan 2031 00:00:00 GMT"})[0] == 200
            assert _get(url + base + "/a.txt", Range="bytes=0-4")[0::2] == (206, b"hello")
            assert _get(url + base + "/a.txt", Range="bytes=0-4", **{"If-Range": '"x"'})[0::2] == (200, b"hello world")  # nothing to compare it with: the whole file


def test_directory_keywords_and_defaults(tmp_path):
    (tmp_path / "index.html").write_text("<h1>home</h1>")
    (tmp_path / ".dot").write_text("x")
    app = Server()
    Directory(tmp_path).mount(app, "/a")
    Directory(tmp_path, index=False, show_hidden=False).mount(app, "/b")
    with running(app) as url:
        assert status(url + "/a/") == 200 and status(url + "/a/.dot") == 200
        assert status(url + "/b/") == 404 and status(url + "/b/.dot") == 404


def test_directory_inline_active_chains_and_shows_html_and_svg_only_behind_a_sandbox(tmp_path):
    (tmp_path / "p.html").write_text("<p>x</p>")
    (tmp_path / "x.xml").write_text("<a/>")
    app = Server()
    d = Directory(tmp_path, download_active=True)
    assert d.inline_active(True) is d
    d.mount(app, "/on")
    Directory(tmp_path, download_active=True).mount(app, "/off")
    Directory(tmp_path, download_active=True, inline_active=True).mount(app, "/kw")
    with running(app) as url:
        for base in ("/on", "/kw"):
            with urllib.request.urlopen(url + base + "/p.html") as r:
                assert r.headers.get("Content-Disposition") is None
                assert "sandbox;" in r.headers["Content-Security-Policy"]
            with urllib.request.urlopen(url + base + "/x.xml") as r:
                assert "attachment" in r.headers["Content-Disposition"]
        with urllib.request.urlopen(url + "/off/p.html") as r:
            assert "attachment" in r.headers["Content-Disposition"]


def test_mount_returns_the_server_so_a_chain_can_go_on(tmp_path):
    app = Server()
    assert Directory(tmp_path).mount(app, "/a") is app
    assert FileBrowser(tmp_path).mount(app, "/files") is app
    assert Swagger("/openapi.json").mount(app, "/docs") is app
    # one chain, from a temporary: this is what the C++ `mount(httpp::server(), ...)` does
    chained = Directory(tmp_path).show_hidden(False).mount(Server(), "/").tls(*_cert_and_key(tmp_path))
    assert isinstance(chained, Server)


def _cert_and_key(folder):
    """A throwaway self-signed pair, so that `.tls(...)` can end the chain above."""
    import shutil
    import subprocess
    if shutil.which("openssl") is None:
        pytest.skip("openssl is needed to make a certificate")
    cert, key = folder / "c.pem", folder / "k.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(cert),
                    "-days", "1", "-subj", "/CN=localhost"], check=True, capture_output=True)
    return str(cert), str(key)


def test_directory_refuses_a_base_without_a_slash(tmp_path):
    with pytest.raises(ValueError):
        Directory(tmp_path).mount(Server(), "site")


def test_swagger_builder_serves_the_page_and_refuses_a_bad_base():
    s = Swagger("/openapi.json")
    app = Server()
    for bad in ("docs", "/", ""):
        with pytest.raises(ValueError):
            s.mount(app, bad)
    assert s.title("T") is s
    s.mount(app, "/api/docs")  # the refused bases mounted nothing
    with running(app) as url:
        body = urllib.request.urlopen(url + "/api/docs").read().decode()
        assert "swagger-ui" in body and "T - Swagger UI" in body
        assert "/api/docs/swagger-ui.css" in body


def test_swagger_needs_an_openapi_url_and_the_base_defaults_to_docs():
    with pytest.raises(TypeError):
        Swagger()
    with pytest.raises(ValueError):
        Swagger("")
    app = Server(docs_url=None)  # a Server serves its own /docs by default
    Swagger("/o.json", title="K").mount(app)
    with running(app) as url:
        body = urllib.request.urlopen(url + "/docs").read().decode()
        assert "/o.json" in body and "K - Swagger UI" in body


def test_one_swagger_object_mounts_on_two_bases():
    app = Server()
    s = Swagger("/o.json")
    s.mount(app, "/a")
    s.mount(app, "/b/ui")
    with running(app) as url:
        assert "/b/ui/swagger-ui.css" in urllib.request.urlopen(url + "/b/ui").read().decode()


def test_proxy_is_one_url_in_every_client(tmp_path):
    from httpp import AsyncClient, Client, Request
    for cls in (Client, AsyncClient):
        cls("http://127.0.0.1:1", proxy="http://user:pass@127.0.0.1:3128")
        with pytest.raises(TypeError):
            cls("http://127.0.0.1:1", proxy=("127.0.0.1", 3128))
        cls("http://127.0.0.1:1", proxy="socks5h://u:p@127.0.0.1:1080")
        cls("http://127.0.0.1:1", proxy="socks5://127.0.0.1")
        for bad in ("127.0.0.1:3128", "https://127.0.0.1:3128", "socks4://h:1", "socks5://"):
            with pytest.raises(ValueError):
                cls("http://127.0.0.1:1", proxy=bad)
    Request("http://127.0.0.1:1").proxy("http://127.0.0.1:3128")
    with pytest.raises(ValueError):
        Request("http://127.0.0.1:1").proxy("nonsense")


def test_url_exposes_the_decoded_user_and_password():
    from httpp import URL
    u = URL("http://us%40er:p%3Ass@h.example:81/x")
    assert (u.username, u.password, u.host, u.port) == ("us@er", "p:ss", "h.example", 81)
    assert URL("http://h.example/").username == ""


def test_a_socks_url_is_a_proxy_address_not_a_place_to_fetch_from():
    from httpp import URL, Client
    u = URL("socks5h://u:p@127.0.0.1")
    assert u.valid and not u.is_http and u.port == 1080 and u.username == "u"
    assert URL("https://h/").is_http and not URL("ftp://h/").valid
    with pytest.raises(ValueError):
        Client("socks5://127.0.0.1:1080")


def test_proxy_headers_middleware_gives_the_real_client_and_scheme():
    import socket
    from httpp import ProxyHeadersMiddleware

    app = Server()

    @app.get("/who")
    def who(request):
        return f"{request.client_ip}|{request.scheme}"

    app.add_middleware(ProxyHeadersMiddleware, trusted_proxies=["127.0.0.1", "10.0.0.0/8"])
    with running(app) as url:
        port = int(url.rsplit(":", 1)[1])

        def get(extra):
            with socket.create_connection(("127.0.0.1", port)) as s:
                s.sendall(f"GET /who HTTP/1.1\r\nHost: x\r\n{extra}Connection: close\r\n\r\n".encode())
                data = b""
                while chunk := s.recv(4096):
                    data += chunk
            return data.split(b"\r\n\r\n", 1)[1].decode()

        assert get("") == "127.0.0.1|http"
        assert get("X-Forwarded-For: 1.1.1.1, 203.0.113.9, 10.0.0.2\r\nX-Forwarded-Proto: https\r\n") == "203.0.113.9|https"


def test_proxy_headers_middleware_refuses_bad_entries():
    from httpp import ProxyHeadersMiddleware

    with pytest.raises(ValueError):
        Server().add_middleware(ProxyHeadersMiddleware, trusted_proxies=["localhost"])
    with pytest.raises(TypeError):
        ProxyHeadersMiddleware(trusted_proxies=123)
