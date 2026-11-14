"""httpp.Directory and httpp.SwaggerUI: the builder classes of ADR-0066 through the Python API (the HTTP behaviour is tested in C++)."""

import threading
import time
import urllib.error
import urllib.request
from contextlib import contextmanager

import pytest

from httpp import Directory, Server, SwaggerUI


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
    assert d.set_index(False).set_show_hidden(False) is d
    d.mount(app, "/site")
    with running(app) as url:
        assert status(url + "/site/") == 404
        assert status(url + "/site/.secret") == 404
        assert status(url + "/site/index.html") == 200


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
    assert d.set_inline_active(True) is d
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


def test_directory_refuses_a_base_without_a_slash(tmp_path):
    with pytest.raises(ValueError):
        Directory(tmp_path).mount(Server(), "site")


def test_swagger_ui_builder_serves_the_page_and_refuses_a_bad_path():
    s = SwaggerUI()
    with pytest.raises(ValueError):
        s.set_docs_path("docs")
    with pytest.raises(ValueError):
        s.set_docs_path("/")
    app = Server()
    assert s.set_docs_path("/api/docs").set_title("T") is s
    s.mount(app)  # the refused values left "/docs" default untouched until the valid one
    with running(app) as url:
        body = urllib.request.urlopen(url + "/api/docs").read().decode()
        assert "swagger-ui" in body and "T - Swagger UI" in body


def test_swagger_ui_keywords():
    app = Server()
    SwaggerUI(docs_path="/d", openapi_url="/o.json", title="K").mount(app)
    with running(app) as url:
        assert "/o.json" in urllib.request.urlopen(url + "/d").read().decode()


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
