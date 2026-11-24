"""httpp.proxy_from_environment and httpp.resolve_proxy: the same rules as the C++ functions (tests/cpp/test_proxy.cpp, ADR-0067)."""

import os

import pytest

import httpp

VARIABLES = ("http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "no_proxy", "NO_PROXY")


@pytest.fixture(autouse=True)
def clean_environment(monkeypatch):
    for name in VARIABLES:
        monkeypatch.delenv(name, raising=False)


def test_no_variables_means_direct():
    assert httpp.proxy_from_environment("http://example.com/") is None
    assert httpp.proxy_from_environment("https://example.com/") is None


def test_each_scheme_reads_its_own_variable_and_all_proxy_is_the_fallback(monkeypatch):
    monkeypatch.setenv("http_proxy", "http://127.0.0.1:1111")
    monkeypatch.setenv("HTTPS_PROXY", "127.0.0.1:2222")  # no scheme: an http proxy, as in curl
    monkeypatch.setenv("ALL_PROXY", "socks5h://127.0.0.1:9050")
    assert httpp.proxy_from_environment("http://example.com/") == "http://127.0.0.1:1111"
    assert httpp.proxy_from_environment("https://example.com/") == "http://127.0.0.1:2222"
    monkeypatch.delenv("http_proxy")
    assert httpp.proxy_from_environment("http://example.com/") == "socks5h://127.0.0.1:9050"


@pytest.mark.skipif(os.name == "nt", reason="Windows has one variable for HTTP_PROXY and http_proxy: there is no uppercase spelling to ignore")
def test_uppercase_HTTP_PROXY_is_ignored(monkeypatch):
    monkeypatch.setenv("HTTP_PROXY", "http://127.0.0.1:1111")
    assert httpp.proxy_from_environment("http://example.com/") is None


def test_no_proxy_lists(monkeypatch):
    monkeypatch.setenv("http_proxy", "http://127.0.0.1:1111")
    monkeypatch.setenv("no_proxy", "localhost,127.0.0.0/8,.internal.test")
    assert httpp.proxy_from_environment("http://localhost:8080/") is None
    assert httpp.proxy_from_environment("http://127.0.0.1/") is None
    assert httpp.proxy_from_environment("http://a.internal.test/") is None
    assert httpp.proxy_from_environment("http://parrot.live/") == "http://127.0.0.1:1111"


def test_no_proxy_compares_an_ipv6_literal_without_its_brackets(monkeypatch):
    monkeypatch.setenv("http_proxy", "http://127.0.0.1:1111")
    monkeypatch.setenv("no_proxy", "::1")
    assert httpp.proxy_from_environment("http://[::1]:8080/") is None  # the URL says "[::1]", the entry says "::1"
    assert httpp.proxy_from_environment("http://[2001:db8::1]:8080/") == "http://127.0.0.1:1111"


def test_the_result_keeps_the_login_and_a_socks_address_in_a_form_a_request_reads_back(monkeypatch):
    monkeypatch.setenv("http_proxy", "http://user:pa%20ss@proxy.local:3128")
    assert httpp.proxy_from_environment("http://example.com/") == "http://user:pa%20ss@proxy.local:3128"
    monkeypatch.setenv("https_proxy", "socks5://127.0.0.1:1080")
    found = httpp.proxy_from_environment("https://example.com/")
    assert found == "socks5://127.0.0.1:1080"
    httpp.Client("https://example.com", proxy=found)  # accepted as a proxy


def test_an_unusable_proxy_raises_value_error_naming_it(monkeypatch):
    monkeypatch.setenv("http_proxy", "ftp://127.0.0.1:21")
    with pytest.raises(ValueError, match="ftp://127.0.0.1:21"):
        httpp.proxy_from_environment("http://example.com/")


def test_resolve_proxy_replaces_the_variables_with_what_it_is_given(monkeypatch):
    monkeypatch.setenv("http_proxy", "http://127.0.0.1:1111")
    monkeypatch.setenv("NO_PROXY", "example.com")
    url = "http://example.com/"
    assert httpp.resolve_proxy(url) is None  # the environment: example.com is excluded
    assert httpp.resolve_proxy(url, no_proxy="other.test") == "http://127.0.0.1:1111"
    assert httpp.resolve_proxy(url, no_proxy="") == "http://127.0.0.1:1111"  # nothing is excluded
    assert httpp.resolve_proxy(url, proxy="proxy.local:3128", no_proxy="other.test") == "http://proxy.local:3128"
    assert httpp.resolve_proxy(url, proxy="proxy.local:3128") is None  # the no_proxy variable still applies
    assert httpp.resolve_proxy("http://other.test/", proxy="") is None  # '' means no proxy at all


def test_resolve_proxy_direct_scheme_is_no_proxy_like_an_empty_string():
    # ADR-0106: direct:// (any case) and nothing else after the scheme; the bare word is a host called direct
    assert httpp.resolve_proxy("http://other.test/", proxy="direct://") is None
    assert httpp.resolve_proxy("http://other.test/", proxy="DIRECT://") is None
    assert "direct" in str(httpp.resolve_proxy("http://other.test/", proxy="direct"))
    assert "direct" in str(httpp.resolve_proxy("http://other.test/", proxy="http://direct"))
    with pytest.raises(ValueError):
        httpp.resolve_proxy("http://other.test/", proxy="direct://somehost")
