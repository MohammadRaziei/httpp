"""Python side of the Basic authentication and rate-limit middleware (include/httpp/middleware.hpp, ADR-0037)."""

import base64
import http.client
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import BasicAuthMiddleware, ProxyHeadersMiddleware, RateLimitMiddleware, Server


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


def call(addr, path="/x", headers=None):
    conn = http.client.HTTPConnection(*addr)
    conn.request("GET", path, headers=headers or {})
    r = conn.getresponse()
    out = (r.status, {k.lower(): v for k, v in r.getheaders()}, r.read().decode())
    conn.close()
    return out


def basic(user, password):
    return {"Authorization": "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()}


def make_app():
    app = Server(openapi_url=None)

    @app.get("/x")
    def x():
        return "hello"

    @app.get("/admin/panel")
    def panel():
        return "panel"

    return app


def test_basic_auth_refuses_without_valid_credentials_and_lets_the_right_user_in():
    app = make_app()
    app.add_middleware(BasicAuthMiddleware, users={"alice": "s3cret", "علی": "رمز"}, realm="Tools", path_prefix="/admin")
    with running(app) as addr:
        status, headers, _ = call(addr, "/admin/panel")
        assert status == 401
        assert headers["www-authenticate"] == 'Basic realm="Tools", charset="UTF-8"'
        assert call(addr, "/admin/panel", basic("alice", "wrong"))[0] == 401
        assert call(addr, "/admin/panel", {"Authorization": "Bearer x"})[0] == 401
        assert call(addr, "/admin/panel", basic("alice", "s3cret"))[0] == 200
        assert call(addr, "/admin/panel", basic("علی", "رمز"))[0] == 200
        assert call(addr, "/x")[0] == 200  # outside the prefix


def test_basic_auth_covers_everything_by_default():
    app = make_app()
    app.add_middleware(BasicAuthMiddleware, users={"u": "p"})
    with running(app) as addr:
        assert call(addr, "/x")[0] == 401
        assert call(addr, "/x", basic("u", "p"))[0] == 200


def test_basic_auth_bad_arguments():
    with pytest.raises(ValueError):
        BasicAuthMiddleware(users={"a:b": "p"})
    with pytest.raises(ValueError):
        BasicAuthMiddleware(users={"": "p"})
    with pytest.raises(TypeError):
        BasicAuthMiddleware(users={"u": 1})
    app = make_app()
    for bad in (dict(users={}), dict(users={"u": "p"}, realm='a"b'), dict(users={"u": "p"}, path_prefix="admin")):
        with pytest.raises(ValueError):
            app.add_middleware(BasicAuthMiddleware, **bad)


def test_rate_limit_answers_429_with_retry_after_and_refills():
    app = make_app()
    app.add_middleware(RateLimitMiddleware, requests_per_second=10, burst=2)
    with running(app) as addr:
        assert call(addr)[0] == 200
        assert call(addr)[0] == 200
        status, headers, body = call(addr)
        assert (status, headers["retry-after"], body) == (429, "1", "Too Many Requests")
        time.sleep(0.25)
        assert call(addr)[0] == 200


def test_rate_limit_tells_clients_apart_by_the_forwarded_address():
    app = make_app()
    app.add_middleware(ProxyHeadersMiddleware, trusted_proxies=["*"])
    app.add_middleware(RateLimitMiddleware, requests_per_second=0.01, burst=1)
    with running(app) as addr:
        assert call(addr, headers={"X-Forwarded-For": "203.0.113.1"})[0] == 200
        assert call(addr, headers={"X-Forwarded-For": "203.0.113.1"})[0] == 429
        assert call(addr, headers={"X-Forwarded-For": "203.0.113.2"})[0] == 200


def test_rate_limit_bad_arguments():
    for bad in (dict(requests_per_second=0), dict(requests_per_second=-1), dict(requests_per_second=float("nan")),
                dict(requests_per_second=float("inf")), dict(burst=0), dict(max_clients=0)):
        with pytest.raises(ValueError):
            RateLimitMiddleware(**bad)
