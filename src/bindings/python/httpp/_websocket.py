"""WebSocket for `async def` routes (RFC 6455, see ADR-0026 and ADR-0027). The handler runs as an asyncio task on the loop
thread of its lane; the I/O thread fills a per-connection mailbox in C++ without ever taking the GIL, and wakes the loop only
when the handler is waiting. Receiving is bounded: past `max_queued` bytes the socket is not read until the handler catches up,
and a send into a full queue waits (it never drops and never buffers without limit)."""

import json

_NEW, _ACCEPTING, _OPEN, _CLOSED = range(4)
_TEXT, _BINARY, _PING = 0, 1, 2


class WebSocketDisconnect(Exception):
    """The connection ended (the peer closed it, it dropped, or this side closed it). `code` is the close code: 1005 when the
    close frame had none, 1006 when the connection ended without one."""

    def __init__(self, code=1006, reason=""):
        super().__init__(code, reason)
        self.code, self.reason = code, reason

    def __str__(self):
        return f"WebSocket closed ({self.code})" + (f": {self.reason}" if self.reason else "")


class WebSocket:
    """One WebSocket connection, as the handler of `@app.websocket(path)` sees it. Call `await websocket.accept()` first (a
    handler that returns, or closes, before accepting rejects the upgrade with 403; one that raises gets 500)."""

    def __init__(self, core, request, loop):
        self._core = core
        self._loop = loop
        self._waiters = []
        self._state = _NEW
        self.request = request
        offered = request.headers.get("sec-websocket-protocol", "")
        self.subprotocols = [p.strip() for p in offered.split(",") if p.strip()]  # what the client offers

    # ---- the loop calls this when the handler armed a wake and there is news ----
    def _wake(self):
        waiters, self._waiters = self._waiters, []
        for fut in waiters:
            if not fut.done():
                fut.set_result(None)

    async def _wait(self):
        fut = self._loop.create_future()
        self._waiters.append(fut)
        try:
            await fut
        finally:
            if not fut.done():  # cancelled: do not leave it for the next wake
                fut.cancel()
            try:
                self._waiters.remove(fut)
            except ValueError:
                pass

    def _disconnect(self):
        self._state = _CLOSED
        return WebSocketDisconnect(*self._core.close_info())

    def _need_open(self):
        if self._state == _NEW or self._state == _ACCEPTING:
            raise RuntimeError("accept() the WebSocket first")
        if self._state == _CLOSED:
            raise self._disconnect()

    # ---- state ----
    @property
    def path(self):
        return self.request.path

    @property
    def headers(self):
        return self.request.headers

    @property
    def query_params(self):
        return self.request.query

    @property
    def path_params(self):
        return self.request.path_params

    @property
    def connected(self):
        """True while the connection is open (it turns False as soon as closing starts, from either side)."""
        return self._state == _OPEN and self._core.is_open()

    # ---- handshake ----
    async def accept(self, subprotocol=None, headers=None):
        """Complete the handshake. `subprotocol` must be one of `self.subprotocols`; `headers` are added to the 101 answer.
        Returns when the connection is open; raises WebSocketDisconnect if the client left first."""
        if self._state != _NEW:
            raise RuntimeError("the WebSocket was already accepted or closed")
        out = list(headers.items() if hasattr(headers, "items") else headers or ())
        if subprotocol is not None:
            out.append(("Sec-WebSocket-Protocol", subprotocol))
        self._core.accept(out)  # ValueError for a subprotocol the client did not offer, or a bad header
        self._state = _ACCEPTING
        while True:
            r = self._core.wait_open()
            if r == 1:
                self._state = _OPEN
                return
            if r == -1:
                raise self._disconnect()
            await self._wait()

    # ---- receiving ----
    async def receive(self):
        """The next message: a str for a text message, bytes for a binary one. Raises WebSocketDisconnect when the connection is
        over and every message that arrived before was read."""
        self._need_open_for_receive()
        while True:
            r, message = self._core.pop()
            if r == 1:
                return message
            if r == -1:
                raise self._disconnect()
            await self._wait()

    def _need_open_for_receive(self):
        if self._state == _NEW or self._state == _ACCEPTING:
            raise RuntimeError("accept() the WebSocket first")
        # a closed connection may still hold messages: pop() sorts that out

    async def receive_text(self):
        message = await self.receive()
        if isinstance(message, bytes):
            raise TypeError("expected a text message, got a binary one")
        return message

    async def receive_bytes(self):
        message = await self.receive()
        if isinstance(message, str):
            raise TypeError("expected a binary message, got a text one")
        return message

    async def receive_json(self):
        return json.loads(await self.receive())

    def __aiter__(self):
        return self

    async def __anext__(self):
        try:
            return await self.receive()
        except WebSocketDisconnect:
            raise StopAsyncIteration from None

    # ---- sending ----
    async def _send(self, kind, data):
        self._need_open()
        while True:
            r = self._core.send(kind, data)
            if r == 0:
                return
            if r == 1:  # closing has begun: the close code is final only when the connection has ended
                while not self._core.wait_closed():
                    await self._wait()
                raise self._disconnect()
            if not self._core.arm_drain():  # the queue is full: wait until it has room again
                await self._wait()

    async def send_text(self, text):
        """Queue a text message. If the send queue is full this waits until it has room (backpressure)."""
        await self._send(_TEXT, text)

    async def send_bytes(self, data):
        await self._send(_BINARY, bytes(data))

    async def send_json(self, data):
        await self._send(_TEXT, json.dumps(data, separators=(",", ":"), ensure_ascii=False))

    async def ping(self, payload=b""):
        """A ping frame (at most 125 bytes, else ValueError); the pong is not reported."""
        await self._send(_PING, bytes(payload))

    async def close(self, code=1000, reason=""):
        """Start the closing handshake (returns without waiting for the peer). Before accept() it rejects the upgrade (403)."""
        if self._state == _NEW:
            self._state = _CLOSED
            self._core.reject(403)
        elif self._state in (_ACCEPTING, _OPEN):
            self._core.close(code, reason)  # ValueError for a code that may not be sent
            if self._state == _OPEN and not self._core.is_open():
                self._state = _CLOSED
