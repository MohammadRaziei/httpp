"""Ready-made middleware, shaped like Starlette's: Server.add_middleware(CORSMiddleware, allow_origins=["*"]).

The work happens in C++ (include/httpp/middleware.hpp, ADR-0037), with no GIL and no Python call per request.
"""

from __future__ import annotations

from typing import Iterable, Optional, Sequence


def _strings(name: str, value: Optional[Iterable[str]]) -> list:
    if value is None:
        return []
    if isinstance(value, str):
        raise TypeError(f"{name} must be a list of strings, not a single string")
    items = list(value)
    if not all(isinstance(v, str) for v in items):
        raise TypeError(f"{name} must be a list of strings")
    return items


class CORSMiddleware:
    """Cross-Origin Resource Sharing. Answers preflight requests itself and adds Access-Control-* headers to the rest.

    allow_origins: exact origins, or ["*"]. allow_origin_regex: the whole origin must match. allow_methods
    (default ["GET"]) and allow_headers may be ["*"]. With allow_credentials and "*" origins the request's own
    origin is echoed. max_age: seconds a browser may cache a preflight.
    """

    def __init__(self, allow_origins: Sequence[str] = (), allow_methods: Sequence[str] = ("GET",),
                 allow_headers: Sequence[str] = (), allow_credentials: bool = False,
                 allow_origin_regex: Optional[str] = None, expose_headers: Sequence[str] = (), max_age: int = 600):
        if isinstance(max_age, bool) or not isinstance(max_age, int) or max_age < 0:
            raise ValueError("max_age must be an integer >= 0")
        if allow_origin_regex is not None and not isinstance(allow_origin_regex, str):
            raise TypeError("allow_origin_regex must be a string")
        self.allow_origins = _strings("allow_origins", allow_origins)
        self.allow_methods = _strings("allow_methods", allow_methods)
        self.allow_headers = _strings("allow_headers", allow_headers)
        self.expose_headers = _strings("expose_headers", expose_headers)
        self.allow_credentials = bool(allow_credentials)
        self.allow_origin_regex = allow_origin_regex or ""
        self.max_age = max_age

    def install(self, app) -> None:
        app._add_cors(self.allow_origins, self.allow_origin_regex, self.allow_methods, self.allow_headers,
                      self.allow_credentials, self.expose_headers, self.max_age)


class TrustedHostMiddleware:
    """Refuse a request whose Host header is not allowed (400 "Invalid host header").

    allowed_hosts: exact names, "*", or "*.example.com" (subdomains only). The port is ignored. With
    www_redirect, "example.com" is redirected (307) to "www.example.com" when only that one is allowed.
    """

    def __init__(self, allowed_hosts: Sequence[str] = ("*",), www_redirect: bool = True):
        self.allowed_hosts = _strings("allowed_hosts", allowed_hosts)
        if not self.allowed_hosts:
            raise ValueError("allowed_hosts must not be empty")
        self.www_redirect = bool(www_redirect)

    def install(self, app) -> None:
        app._add_trusted_host(self.allowed_hosts, self.www_redirect)


class GZipMiddleware:
    """Gzip text answers of at least minimum_size bytes for clients that accept gzip (Server.set_compression).

    compresslevel is zlib's 1 (fastest, default; Starlette's default is 9) to 9 (smallest).
    """

    def __init__(self, minimum_size: int = 500, compresslevel: int = 1):
        if isinstance(minimum_size, bool) or not isinstance(minimum_size, int) or minimum_size < 1:
            raise ValueError("minimum_size must be an integer >= 1")
        if isinstance(compresslevel, bool) or not isinstance(compresslevel, int) or not 1 <= compresslevel <= 9:
            raise ValueError("compresslevel must be an integer from 1 to 9")
        self.minimum_size = minimum_size
        self.compresslevel = compresslevel

    def install(self, app) -> None:
        app.set_compression(self.minimum_size, self.compresslevel)


class HTTPSRedirectMiddleware:
    """Send requests that did not arrive over TLS to the https:// address of the same URL (307).

    A server serves TLS or plain HTTP on a port, never both, so this is for a plain-HTTP server behind a proxy that terminates TLS and says so
    with "X-Forwarded-Proto: https" (set trust_forwarded_proto=True; leave it off when clients can reach this port directly), or to point
    people at the secure address. Requests on a TLS server are never redirected.
    """

    def __init__(self, trust_forwarded_proto: bool = False):
        self.trust_forwarded_proto = bool(trust_forwarded_proto)

    def install(self, app) -> None:
        app._add_https_redirect(self.trust_forwarded_proto)


class ProxyHeadersMiddleware:
    """Behind a reverse proxy, take the real client from the headers the proxy adds (ADR-0069).

    When the connection comes from one of `trusted_proxies`, request.client_ip becomes the first address of X-Forwarded-For, from the right,
    that is not itself a trusted proxy; X-Forwarded-Proto sets request.secure / scheme; X-Forwarded-Host replaces the Host header only with
    trust_forwarded_host=True. Any other connection is left alone, whatever headers it sends.

    An entry is an IPv4 or IPv6 address, a CIDR range ("10.0.0.0/8") or "*" (every connection: only when the port is reachable through the
    proxy alone). Default: 127.0.0.1 and ::1. add_middleware raises ValueError for anything else. One per server; a second replaces the first.
    """

    def __init__(self, trusted_proxies=("127.0.0.1", "::1"), trust_forwarded_host: bool = False):
        self.trusted_proxies = _strings("trusted_proxies", trusted_proxies)
        self.trust_forwarded_host = bool(trust_forwarded_host)

    def install(self, app) -> None:
        app._add_forwarded_headers(self.trusted_proxies, self.trust_forwarded_host)
