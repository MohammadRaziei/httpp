"""One-call helpers (httpp.get(...), httpp.post(...), ...) and AsyncClient.

Both are thin layers over the compiled Client / Request; every capability here exists in the C++
API too (httpp::client::request, httpp::client, httpp::async_client).
"""

import asyncio

from .httpp_cy import Client, Request, _AsyncClientCore


def request(method, url, *, params=None, headers=None, data=None, json=None, form=None,
            content_type=None, timeout=None, follow_redirects=False, auth=None, token=None,
            cookies=None, verify=True, ca_file=None, proxy=None):
    """Make one request and return its Response. See Request for what each option means.

        r = httpp.request("POST", "http://localhost:8000/items", json={"name": "apple"}, timeout=10)
        r.raise_for_status().json()
    """
    req = Request(url).method(method)
    if params:
        req.params(params)
    for name, value in (headers.items() if hasattr(headers, "items") else headers or ()):
        req.header(name, value)
    if json is not None:
        req.json(json)
    elif form is not None:
        req.form(form)
    elif data is not None:
        req.data(data)
    if content_type is not None:
        req.content_type(content_type)
    if timeout is not None:
        req.timeout(timeout)
    if follow_redirects:
        req.follow_redirects()
    if auth is not None:
        req.basic_auth(*auth)
    if token is not None:
        req.bearer(token)
    for name, value in (cookies.items() if hasattr(cookies, "items") else cookies or ()):
        req.cookie(name, value)
    if not verify:
        req.verify(False)
    if ca_file is not None:
        req.ca_file(ca_file)
    if proxy is not None:
        req.proxy(*proxy)
    return req.run()


def get(url, **kwargs):
    return request("GET", url, **kwargs)


def head(url, **kwargs):
    return request("HEAD", url, **kwargs)


def options(url, **kwargs):
    return request("OPTIONS", url, **kwargs)


def post(url, **kwargs):
    return request("POST", url, **kwargs)


def put(url, **kwargs):
    return request("PUT", url, **kwargs)


def patch(url, **kwargs):
    return request("PATCH", url, **kwargs)


def delete(url, **kwargs):
    return request("DELETE", url, **kwargs)


class AsyncClient:
    """Many requests to one server at once, from asyncio code.

        async with httpp.AsyncClient("http://localhost:8000", max_connections=16) as c:
            replies = await asyncio.gather(*(c.get(f"/items/{i}") for i in range(100)))

    Requests run on the C++ network thread (non-blocking sockets, Asio + llhttp): no thread per
    request, connections are kept alive and reused, and at most `max_connections` requests are in
    flight (extra ones wait their turn). The C++ counterpart is httpp::async_client.

    `**client_options` are the same as for Client: headers, timeout, follow_redirects, auth, token,
    verify, ca_file, proxy.
    """

    def __init__(self, base_url, *, max_connections=8, **client_options):
        if max_connections < 1:
            raise ValueError("max_connections must be at least 1")
        self._core = _AsyncClientCore(base_url, max_connections, **client_options)  # validates the URL now

    async def request(self, method, path, **kwargs):
        return await self._core.request(method, path, **kwargs)

    async def get(self, path, **kwargs):
        return await self.request("GET", path, **kwargs)

    async def head(self, path, **kwargs):
        return await self.request("HEAD", path, **kwargs)

    async def options(self, path, **kwargs):
        return await self.request("OPTIONS", path, **kwargs)

    async def post(self, path, **kwargs):
        return await self.request("POST", path, **kwargs)

    async def put(self, path, **kwargs):
        return await self.request("PUT", path, **kwargs)

    async def patch(self, path, **kwargs):
        return await self.request("PATCH", path, **kwargs)

    async def delete(self, path, **kwargs):
        return await self.request("DELETE", path, **kwargs)

    async def aclose(self):
        pass  # connections belong to the shared network thread's pool and close when idle too long

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        await self.aclose()
