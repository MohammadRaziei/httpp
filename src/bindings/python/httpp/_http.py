"""One-call helpers (httpp.get(...), httpp.post(...), ...) and AsyncClient.

Both are thin layers over the compiled Client / Request; every capability here exists in the C++
API too (httpp::client::request, httpp::client, httpp::async_client).
"""

import asyncio

from .httpp_cy import Client, Request


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

    It keeps a pool of keep-alive Clients (at most `max_connections`; extra requests wait their
    turn). Each request runs its blocking call on the event loop's default thread pool, so the
    loop is never blocked, but concurrency is bounded by that pool and by max_connections: this
    is not non-blocking socket I/O. The C++ counterpart is httpp::async_client.
    """

    def __init__(self, base_url, *, max_connections=8, **client_options):
        if max_connections < 1:
            raise ValueError("max_connections must be at least 1")
        self._base_url = base_url
        self._options = client_options
        self._max = max_connections
        self._free = [Client(base_url, **client_options)]  # validates the URL now
        self._created = 1
        self._slots = None  # created on first use, inside the running loop

    async def _acquire(self):
        if self._slots is None:
            self._slots = asyncio.Semaphore(self._max)
        await self._slots.acquire()
        if self._free:
            return self._free.pop()
        self._created += 1
        return Client(self._base_url, **self._options)

    def _release(self, client):
        self._free.append(client)
        self._slots.release()

    async def request(self, method, path, **kwargs):
        client = await self._acquire()
        try:
            loop = asyncio.get_running_loop()
            return await loop.run_in_executor(None, lambda: client.request(method, path, **kwargs))
        finally:
            self._release(client)

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
        self._free.clear()

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        await self.aclose()
