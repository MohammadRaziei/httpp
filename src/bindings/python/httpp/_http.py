"""AsyncClient: the asyncio wrapper over the compiled _AsyncClientCore.

It is a thin layer; every capability here exists in the C++ API too (httpp::async_client). There are no
module-level one-call helpers (httpp.get(...)): the name does not say whether it is a client or a server route,
and Request(url, method="GET", timeout=10).run() is as short (ADR-0092).
"""

from .httpp_python import _AsyncClientCore


class AsyncClient:
    """Many requests to one server at once, from asyncio code.

        async with httpp.AsyncClient("http://localhost:8000", max_connections=16) as c:
            replies = await asyncio.gather(*(c.get(f"/items/{i}") for i in range(100)))

    Requests run on the C++ network thread (non-blocking sockets, Asio + llhttp): no thread per
    request, connections are kept alive and reused, and at most `max_connections` requests are in
    flight (extra ones wait their turn). The C++ counterpart is httpp::async_client.

    `**client_options` are the same as for Client: headers, timeout, max_redirects, connect_timeout, max_time,
    auto_compression, basic_auth, bearer, verify, ca_file, proxy (each the name of a setter).
    """

    def __init__(self, base_url, *, max_connections=8, **client_options):
        if max_connections < 1:
            raise ValueError("max_connections must be at least 1")
        self._core = _AsyncClientCore(base_url, max_connections, **client_options)  # validates the URL now

    def clone(self):
        """A new client to the same base URL with the same settings and max_connections, independent of this
        one and with its own queue: later changes to either do not reach the other. (`b = a` is the same client.)"""
        other = AsyncClient.__new__(AsyncClient)
        other._core = self._core.clone()
        return other

    def make_request(self, path, *, params=None):
        """A Request to `path` that starts with this client's headers and settings as they are now (a copy).
        It runs on its own (run, run_async), not through this client's max_connections."""
        return self._core.make_request(path, params=params)

    async def send(self, method, path, **kwargs):
        return await self._core.send(method, path, **kwargs)

    async def get(self, path, **kwargs):
        return await self.send("GET", path, **kwargs)

    async def head(self, path, **kwargs):
        return await self.send("HEAD", path, **kwargs)

    async def options(self, path, **kwargs):
        return await self.send("OPTIONS", path, **kwargs)

    async def post(self, path, **kwargs):
        return await self.send("POST", path, **kwargs)

    async def put(self, path, **kwargs):
        return await self.send("PUT", path, **kwargs)

    async def patch(self, path, **kwargs):
        return await self.send("PATCH", path, **kwargs)

    async def delete(self, path, **kwargs):
        return await self.send("DELETE", path, **kwargs)

    async def aclose(self):
        pass  # connections belong to the shared network thread's pool and close when idle too long

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        await self.aclose()
