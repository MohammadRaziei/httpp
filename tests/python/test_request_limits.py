"""Request.max_redirects, Request.connect_timeout and Request.max_time (ADR-0079 and 0077)."""
import time

import pytest

import httpp
from httpp import Request, Server

from test_httpp_routes import running


def chain_app():
    app = Server(openapi_url=None)

    @app.get("/a")
    def a():
        return httpp.redirect("/b")

    @app.get("/b")
    def b():
        return httpp.redirect("/c")

    @app.get("/c")
    def c():
        return "end"

    @app.get("/slow")
    def slow():
        time.sleep(1.5)  # the connection is up at once, the answer is late
        return "late"

    return app


def test_max_redirects_bounds_the_hops_that_are_followed():
    with running(chain_app()) as url:
        two = Request(url + "/a").max_redirects(2).run()
        assert two.ok and two.body == "end"
        one = Request(url + "/a").max_redirects(1).run()
        assert one.failed and one.error == "too_many_redirects" and one.status == 0


def test_max_redirects_zero_is_the_default_and_follows_none():
    with running(chain_app()) as url:
        for r in (Request(url + "/a").run(), Request(url + "/a").max_redirects(0).run()):
            assert 300 <= r.status < 400 and not r.failed  # the redirect itself is the answer


def test_max_redirects_minus_one_has_no_limit():
    with running(chain_app()) as url:
        r = Request(url + "/a").max_redirects(-1).run()
        assert r.ok and r.body == "end"


def test_a_negative_max_redirects_below_minus_one_is_a_value_error():
    with pytest.raises(ValueError):
        Request("http://127.0.0.1:1/").max_redirects(-2)


def test_connect_timeout_does_not_limit_the_wait_for_the_answer():
    with running(chain_app()) as url:
        r = Request(url + "/slow").connect_timeout(1).timeout(5).run()
        assert r.ok and r.body == "late"
        r = Request(url + "/slow").connect_timeout(10).timeout(1).run()
        assert r.failed and r.error == "timeout"


def test_connect_timeout_takes_seconds_like_timeout():
    with running(chain_app()) as url:
        assert Request(url + "/c").connect_timeout(0.4).run().ok  # a fraction of a second is kept as it is
    with pytest.raises(ValueError):
        Request("http://127.0.0.1:1/").connect_timeout(-1)


def test_the_request_helper_takes_both_options():
    with running(chain_app()) as url:
        r = httpp.Request(url + "/a", method="GET", max_redirects=1).run()
        assert r.error == "too_many_redirects"
        r = httpp.Request(url + "/a", method="GET", max_redirects=2, connect_timeout=5).run()
        assert r.ok and r.body == "end"
        assert httpp.Request(url + "/a", method="GET", max_redirects=20).run().body == "end"


def test_max_time_ends_a_slow_request_with_a_timeout():
    with running(chain_app()) as url:
        started = time.monotonic()
        r = Request(url + "/slow").timeout(20).max_time(1).run()
        elapsed = time.monotonic() - started
    assert r.failed and r.error == "timeout"
    assert elapsed < 1.45  # the answer would have come after 1.5 s


def test_max_time_zero_is_no_limit_and_negative_is_refused():
    with running(chain_app()) as url:
        r = Request(url + "/c").max_time(0).run()
        assert r.ok and r.body == "end"
    with pytest.raises(ValueError):
        Request("http://localhost/").max_time(-1)


def test_client_takes_max_redirects_like_request():
    with running(chain_app()) as url:
        assert 300 <= httpp.Client(url).get("/a").status < 400  # follows none by default
        assert httpp.Client(url, max_redirects=2).get("/a").text == "end"
        assert httpp.Client(url, max_redirects=-1).get("/a").text == "end"
        assert httpp.Client(url, max_redirects=1).get("/a").error == "too_many_redirects"
        assert httpp.Client(url, max_redirects=20).get("/a").text == "end"
        with pytest.raises(ValueError):
            httpp.Client(url, max_redirects=-2)


def test_client_takes_connect_timeout_max_time_and_auto_compression():
    with running(chain_app()) as url:
        assert httpp.Client(url, connect_timeout=5, max_time=10, auto_compression=False).get("/c").text == "end"
        assert httpp.Client(url, max_time=1).get("/slow").error == "timeout"
        with pytest.raises(ValueError):
            httpp.Client(url, connect_timeout=-1)
        with pytest.raises(ValueError):
            httpp.Client(url, max_time=-1)


def test_download_has_getters_with_the_same_names(tmp_path):
    d = httpp.Download("http://127.0.0.1:1/x", str(tmp_path / "f"))
    assert d.max_redirects() == 20 and d.timeout() == 0 and d.max_wait() == 2.0
    assert d.max_redirects(0) is d and d.max_redirects() == 0
    assert d.max_redirects(-1).max_redirects() == -1
    assert d.timeout(7).timeout() == 7
    assert d.max_wait(0.5).max_wait() == 0.5
    with pytest.raises(ValueError):
        d.max_redirects(-2)
