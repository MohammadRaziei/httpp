"""Request.max_redirects, Request.connect_timeout and Request.max_time (ADR-0079, ADR-0089, ADR-0084)."""
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
        assert Request(url + "/c").connect_timeout(0.4).run().ok  # rounds up to a whole second, never to zero
    with pytest.raises(ValueError):
        Request("http://127.0.0.1:1/").connect_timeout(-1)


def test_the_request_helper_takes_both_options():
    with running(chain_app()) as url:
        r = httpp.request("GET", url + "/a", max_redirects=1)
        assert r.error == "too_many_redirects"
        r = httpp.request("GET", url + "/a", max_redirects=2, connect_timeout=5)
        assert r.ok and r.body == "end"
        assert httpp.request("GET", url + "/a", follow_redirects=True).body == "end"  # the Client spelling: up to 20


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
