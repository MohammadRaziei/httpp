"""Python parity for the C++ middleware (ADR-0037): CORS, trusted host, gzip, ordering with the app's hooks."""

import http.client
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import CORSMiddleware, GZipMiddleware, Server, TrustedHostMiddleware


@contextmanager
def running(app):
    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        yield "127.0.0.1", port
    finally:
        app.stop()
        th.join()


def call(addr, method="GET", path="/x", headers=None):
    conn = http.client.HTTPConnection(*addr)
    conn.request(method, path, headers=headers or {})
    r = conn.getresponse()
    body = r.read().decode()
    out = (r.status, {k.lower(): v for k, v in r.getheaders()}, body)
    conn.close()
    return out


def make_app(**kw):
    app = Server(openapi_url=None)

    @app.get("/x")
    def x():
        return "hello"

    return app


def test_exports_and_option_validation():
    assert httpp.middleware.CORSMiddleware is CORSMiddleware
    with pytest.raises(TypeError):
        make_app().add_middleware(object)
    with pytest.raises(TypeError):
        make_app().add_middleware(CORSMiddleware, allow_origins="https://a.example")  # a string, not a list
    with pytest.raises(ValueError):
        make_app().add_middleware(CORSMiddleware, max_age=-1)
    with pytest.raises(ValueError):
        make_app().add_middleware(CORSMiddleware, allow_origin_regex="(")
    with pytest.raises(ValueError):
        make_app().add_middleware(TrustedHostMiddleware, allowed_hosts=[])
    with pytest.raises(ValueError):
        make_app().add_middleware(TrustedHostMiddleware, allowed_hosts=["*example.com"])
    with pytest.raises(ValueError):
        make_app().add_middleware(GZipMiddleware, compresslevel=10)
    app = make_app()
    assert app.add_middleware(CORSMiddleware, allow_origins=["*"]) is app


def test_cors_preflight_simple_request_and_refusals():
    app = make_app()
    app.add_middleware(CORSMiddleware, allow_origins=["https://a.example"], allow_methods=["GET", "POST"],
                       allow_headers=["X-Token"], expose_headers=["X-Total"])
    with running(app) as addr:
        status, h, _ = call(addr, "OPTIONS", "/nowhere", {"Origin": "https://a.example",
                                                           "Access-Control-Request-Method": "POST",
                                                           "Access-Control-Request-Headers": "x-token"})
        assert status == 200
        assert h["access-control-allow-origin"] == "https://a.example"
        assert "POST" in h["access-control-allow-methods"] and "X-Token" in h["access-control-allow-headers"]
        assert h["access-control-max-age"] == "600" and "Origin" in h["vary"]

        status, _, body = call(addr, "OPTIONS", "/x", {"Origin": "https://evil.example", "Access-Control-Request-Method": "GET"})
        assert status == 400 and "Disallowed CORS origin" in body
        status, _, body = call(addr, "OPTIONS", "/x", {"Origin": "https://a.example", "Access-Control-Request-Method": "DELETE"})
        assert status == 400 and "Disallowed CORS method" in body

        status, h, _ = call(addr, "GET", "/x", {"Origin": "https://a.example"})
        assert status == 200 and h["access-control-allow-origin"] == "https://a.example"
        assert h["access-control-expose-headers"] == "X-Total"
        status, h, _ = call(addr, "GET", "/x", {"Origin": "https://evil.example"})
        assert status == 200 and "access-control-allow-origin" not in h
        status, h, _ = call(addr, "GET", "/missing", {"Origin": "https://a.example"})
        assert status == 404 and h["access-control-allow-origin"] == "https://a.example"
        assert "access-control-allow-origin" not in call(addr)[1]


def test_cors_wildcard_credentials_and_regex():
    star = make_app().add_middleware(CORSMiddleware, allow_origins=["*"])
    with running(star) as addr:
        assert call(addr, headers={"Origin": "https://any.example"})[1]["access-control-allow-origin"] == "*"
    cred = make_app().add_middleware(CORSMiddleware, allow_origins=["*"], allow_credentials=True)
    with running(cred) as addr:
        h = call(addr, headers={"Origin": "https://any.example"})[1]
        assert h["access-control-allow-origin"] == "https://any.example" and h["access-control-allow-credentials"] == "true"
    rx = make_app().add_middleware(CORSMiddleware, allow_origin_regex=r"https://.*\.example\.com")
    with running(rx) as addr:
        assert call(addr, headers={"Origin": "https://app.example.com"})[1]["access-control-allow-origin"] == "https://app.example.com"
        assert "access-control-allow-origin" not in call(addr, headers={"Origin": "https://app.example.com.evil.io"})[1]


def test_trusted_host():
    app = make_app().add_middleware(TrustedHostMiddleware, allowed_hosts=["example.com", "*.api.test"], www_redirect=False)
    with running(app) as addr:
        assert call(addr, headers={"Host": "example.com:8000"})[0] == 200
        assert call(addr, headers={"Host": "v1.api.test"})[0] == 200
        status, _, body = call(addr, headers={"Host": "evil.com"})
        assert status == 400 and "Invalid host header" in body
        assert call(addr, headers={"Host": "api.test"})[0] == 400
    www = make_app().add_middleware(TrustedHostMiddleware, allowed_hosts=["www.example.com"])
    with running(www) as addr:
        status, h, _ = call(addr, path="/x?a=1", headers={"Host": "example.com"})
        assert status == 307 and h["location"] == "http://www.example.com/x?a=1"


def test_gzip_middleware_is_set_compression():
    import gzip
    big = "The quick brown fox jumps over the lazy dog. " * 100
    app = Server(openapi_url=None)

    @app.get("/big")
    def big_text():
        return big

    app.add_middleware(GZipMiddleware, minimum_size=1000)
    with running(app) as addr:
        status, h, _ = call(addr, path="/big", headers={"Accept-Encoding": "identity"})
        assert "content-encoding" not in h
        conn = http.client.HTTPConnection(*addr)
        conn.request("GET", "/big", headers={"Accept-Encoding": "gzip"})
        r = conn.getresponse()
        body = r.read()
        assert r.getheader("Content-Encoding") == "gzip" and big in gzip.decompress(body).decode()
        conn.close()


def test_middleware_composes_with_the_apps_hooks():
    app = make_app()
    seen = []

    @app.before_request
    def app_before(request):
        seen.append("before")

    @app.after_request
    def app_after(request, response):
        response.add_header("X-App", "1")

    app.add_middleware(CORSMiddleware, allow_origins=["https://a.example"])
    app.add_middleware(TrustedHostMiddleware, allowed_hosts=["127.0.0.1"], www_redirect=False)
    with running(app) as addr:
        status, h, _ = call(addr, headers={"Origin": "https://a.example"})
        assert status == 200 and h["access-control-allow-origin"] == "https://a.example" and h["x-app"] == "1"
        assert seen == ["before"]
        # a refused host never reaches the app's before hook, but the CORS after hook still decorates the 400
        status, h, _ = call(addr, headers={"Origin": "https://a.example", "Host": "evil.com"})
        assert status == 400 and h["access-control-allow-origin"] == "https://a.example"
        assert seen == ["before"]
