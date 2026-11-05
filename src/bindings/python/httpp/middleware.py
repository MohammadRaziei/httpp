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
