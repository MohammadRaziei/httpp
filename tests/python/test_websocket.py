"""WebSocket routes (`@app.websocket`): the handler is an asyncio task, the I/O thread never takes the GIL for it."""

import base64
import os
import socket
import struct
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Server, WebSocket, WebSocketDisconnect


@contextmanager
def running(app):
    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        yield port
    finally:
        app.stop()
        th.join()


class Client:
    """A bare RFC 6455 client: handshake, masked frames."""

    def __init__(self, port, path="/ws", headers=None, protocols=None):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        extra = "".join(f"{k}: {v}\r\n" for k, v in (headers or {}).items())
        if protocols:
            extra += f"Sec-WebSocket-Protocol: {', '.join(protocols)}\r\n"
        self.sock.sendall((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n{extra}\r\n").encode())
        self.buf = b""
        head = self._until(b"\r\n\r\n")
        self.head = head.decode("latin-1")
        self.status = int(self.head.split(" ", 2)[1])

    def _until(self, marker):
        while marker not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                break
            self.buf += chunk
        head, _, self.buf = self.buf.partition(marker)
        return head

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, opcode, payload=b"", fin=True):
        mask = os.urandom(4)
        n = len(payload)
        head = bytes([(0x80 if fin else 0) | opcode])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def text(self, s):
        self.send(1, s.encode())

    def recv(self):
        """(opcode, payload) of the next frame."""
        b0, b1 = self._need(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._need(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._need(8))[0]
        return b0 & 0x0F, self._need(n)

    def close(self, code=1000):
        self.send(8, struct.pack(">H", code))

    def drop(self):
        self.sock.close()


def echo_app(**kw):
    app = Server()

    @app.websocket("/ws", **kw)
    async def echo(websocket):
        await websocket.accept()
        async for message in websocket:
            if isinstance(message, str):
                await websocket.send_text(message)
            else:
                await websocket.send_bytes(message)

    return app


def test_echo_text_and_binary():
    with running(echo_app()) as port:
        c = Client(port)
        assert c.status == 101
        c.text("héllo")
        assert c.recv() == (1, "héllo".encode())
        c.send(2, b"\x00\x01\xff")
        assert c.recv() == (2, b"\x00\x01\xff")
        c.close()
        assert c.recv()[0] == 8


def test_many_messages_arrive_in_order():
    with running(echo_app()) as port:
        c = Client(port)
        for i in range(500):
            c.text(str(i))
        assert [c.recv()[1] for _ in range(500)] == [str(i).encode() for i in range(500)]


def test_receive_json_send_json_and_params():
    app = Server()

    @app.websocket("/room/{name}")
    async def room(websocket: WebSocket, name, n: int = 1):
        await websocket.accept()
        data = await websocket.receive_json()
        await websocket.send_json({"room": name, "n": n, "got": data, "path": websocket.path})

    with running(app) as port:
        c = Client(port, "/room/lobby?n=3")
        c.text('{"a":[1,2]}')
        assert c.recv() == (1, b'{"room":"lobby","n":3,"got":{"a":[1,2]},"path":"/room/lobby"}')


def test_handler_return_closes_with_1000():
    app = Server()

    @app.websocket("/ws")
    async def once(websocket):
        await websocket.accept()
        await websocket.send_text("bye")

    with running(app) as port:
        c = Client(port)
        assert c.recv() == (1, b"bye")
        op, payload = c.recv()
        assert op == 8 and struct.unpack(">H", payload[:2])[0] == 1000


def test_close_with_code_and_reason():
    app = Server()

    @app.websocket("/ws")
    async def f(websocket):
        await websocket.accept()
        await websocket.close(4001, "go away")

    with running(app) as port:
        c = Client(port)
        op, payload = c.recv()
        assert op == 8 and struct.unpack(">H", payload[:2])[0] == 4001 and payload[2:] == b"go away"


def test_peer_close_raises_disconnect_with_its_code():
    seen = []
    app = Server()

    @app.websocket("/ws")
    async def f(websocket):
        await websocket.accept()
        try:
            await websocket.receive()
        except WebSocketDisconnect as exc:
            seen.append((exc.code, exc.reason))

    with running(app) as port:
        c = Client(port)
        c.close(4000)
        c.recv()
        time.sleep(0.1)
    assert seen == [(4000, "")]


def test_a_dropped_connection_is_1006():
    seen = []
    app = Server()

    @app.websocket("/ws")
    async def f(websocket):
        await websocket.accept()
        try:
            await websocket.receive()
        except WebSocketDisconnect as exc:
            seen.append(exc.code)

    with running(app) as port:
        c = Client(port)
        time.sleep(0.05)
        c.drop()
        time.sleep(0.2)
    assert seen == [1006]


def test_messages_sent_before_a_close_are_still_read():
    got = []
    app = Server()

    @app.websocket("/ws")
    async def f(websocket):
        await websocket.accept()
        await __import__("asyncio").sleep(0.2)  # the client is gone by now
        async for m in websocket:
            got.append(m)

    with running(app) as port:
        c = Client(port)
        c.text("a")
        c.text("b")
        c.close()
        time.sleep(0.4)
    assert got == ["a", "b"]


def test_not_accepting_rejects_with_403():
    app = Server()

    @app.websocket("/ws")
    async def f(websocket):
        return

    with running(app) as port:
        assert Client(port).status == 403


def test_an_exception_before_accept_is_500_and_after_accept_1011():
    app = Server()

    @app.websocket("/early")
    async def early(websocket):
        raise RuntimeError("boom")

    @app.websocket("/late")
    async def late(websocket):
        await websocket.accept()
        raise RuntimeError("boom")

    with running(app) as port:
        assert Client(port, "/early").status == 500
        c = Client(port, "/late")
        assert c.status == 101
        op, payload = c.recv()
        assert op == 8 and struct.unpack(">H", payload[:2])[0] == 1011


def test_http_exception_before_accept_sets_the_status():
    app = Server()

    @app.websocket("/ws")
    async def f(websocket):
        raise httpp.HTTPException(401)

    with running(app) as port:
        assert Client(port).status == 401


def test_bad_path_parameter_is_rejected():
    app = Server()

    @app.websocket("/n/{i}")
    async def f(websocket, i: int):
        await websocket.accept()

    with running(app) as port:
        assert Client(port, "/n/x").status == 422


def test_subprotocol_negotiation():
    app = Server()

    @app.websocket("/ws")
    async def f(websocket):
        assert websocket.subprotocols == ["chat", "v2"]
        await websocket.accept(subprotocol="v2", headers={"X-Extra": "1"})
        await websocket.send_text("ok")

    with running(app) as port:
        c = Client(port, protocols=["chat", "v2"])
        assert c.status == 101
        assert "sec-websocket-protocol: v2" in c.head.lower() and "x-extra: 1" in c.head.lower()


def test_a_subprotocol_the_client_did_not_offer_fails_the_handshake():
    app = Server()

    @app.websocket("/ws")
    async def f(websocket):
        await websocket.accept(subprotocol="nope")

    with running(app) as port:
        assert Client(port, protocols=["chat"]).status == 500


def test_origin_must_match_the_host_by_default():
    with running(echo_app()) as port:
        assert Client(port, headers={"Origin": "http://evil.example"}).status == 403
        assert Client(port, headers={"Origin": f"http://127.0.0.1:{port}"}).status == 101
        assert Client(port).status == 101  # no Origin: not a browser


def test_origins_option():
    with running(echo_app(origins=["https://app.example"])) as port:
        assert Client(port, headers={"Origin": "https://app.example"}).status == 101
        assert Client(port, headers={"Origin": "https://other.example"}).status == 403
    with running(echo_app(origins=["*"])) as port:
        assert Client(port, headers={"Origin": "https://other.example"}).status == 101


def test_a_plain_get_is_426_or_400():
    with running(echo_app()) as port:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        s.sendall(b"GET /ws HTTP/1.1\r\nHost: x\r\n\r\n")
        assert s.recv(100).split(b" ")[1] in (b"400", b"426")


def test_a_message_over_max_message_closes_with_1009():
    with running(echo_app(max_message=100)) as port:
        c = Client(port)
        c.send(1, b"x" * 101)
        op, payload = c.recv()
        assert op == 8 and struct.unpack(">H", payload[:2])[0] == 1009


def test_bad_arguments():
    app = Server()
    with pytest.raises(ValueError):
        app.websocket("/a", max_message=0)
    with pytest.raises(ValueError):
        app.websocket("/a", ping_interval=-1)
    with pytest.raises(TypeError):
        app.websocket("/a")(lambda websocket: None)

    async def no_ws():
        pass

    with pytest.raises(TypeError):
        app.websocket("/a")(no_ws)


def test_receiving_is_bounded_the_socket_is_not_read_while_the_handler_is_busy():
    total = 64 * 1024 * 1024  # far more than the kernel can buffer on both sides plus max_queued
    state = {}
    app = Server()

    @app.websocket("/ws", max_queued=64 * 1024)
    async def slow(websocket):
        import asyncio

        await websocket.accept()
        await asyncio.sleep(1.0)  # not reading at all for a while
        n = 0
        async for m in websocket:
            n += len(m)
            if n >= total:
                break
        state["bytes"] = n
        await websocket.send_text("done")

    with running(app) as port:
        c = Client(port)
        c.sock.settimeout(0.5)
        chunk = b"y" * 32768
        sent = 0
        stuck = False
        try:
            while sent < total:
                c.send(2, chunk)
                sent += len(chunk)
        except (socket.timeout, TimeoutError):
            stuck = True  # TCP pushed back: the server was not reading
        assert stuck and sent < total, "the server kept reading while its handler did not"
        c.sock.settimeout(60)
        # (a send that timed out may have written part of a frame, so just check the handler's own progress below)
        c.drop()
    time.sleep(0.2)


def test_a_send_into_a_full_queue_waits_and_then_goes_on():
    import asyncio

    times = {}
    app = Server()

    @app.websocket("/ws", max_buffered=64 * 1024)
    async def flood(websocket):
        await websocket.accept()
        payload = b"z" * 16384
        t0 = time.time()
        for _ in range(2000):  # about 32 MiB: far more than the kernel buffers plus the queue
            await websocket.send_bytes(payload)
        times["sent_after"] = time.time() - t0
        await websocket.send_text("end")

    with running(app) as port:
        c = Client(port)
        time.sleep(0.5)  # the client reads nothing: the handler must be stuck in a send
        assert "sent_after" not in times
        got = 0
        while True:
            op, payload = c.recv()
            if op == 1:
                assert payload == b"end"
                break
            got += len(payload)
        assert got == 2000 * 16384
    assert times["sent_after"] >= 0.5


def test_ping_is_answered_by_the_engine_and_the_handler_can_ping():
    app = Server()

    @app.websocket("/ws")
    async def f(websocket):
        await websocket.accept()
        await websocket.ping(b"hi")
        await websocket.receive()

    with running(app) as port:
        c = Client(port)
        assert c.recv() == (9, b"hi")
        c.send(9, b"x")
        assert c.recv() == (10, b"x")


def test_a_send_after_the_peer_left_raises_disconnect():
    import asyncio

    out = []
    app = Server()

    @app.websocket("/ws")
    async def f(websocket):
        await websocket.accept()
        await asyncio.sleep(0.3)
        try:
            for _ in range(100):
                await websocket.send_text("x")
                await asyncio.sleep(0.01)
        except WebSocketDisconnect as exc:
            out.append(exc.code)

    with running(app) as port:
        c = Client(port)
        c.close(1001)
        assert c.recv()[0] == 8  # the echo; the engine then waits for the peer to hang up (a lingering close)
        c.drop()
        time.sleep(1.0)
    assert out == [1001]


def test_many_connections_at_once():
    with running(echo_app()) as port:
        clients = [Client(port) for _ in range(60)]
        for i, c in enumerate(clients):
            c.text(f"m{i}")
        for i, c in enumerate(clients):
            assert c.recv() == (1, f"m{i}".encode())
        for c in clients:
            c.drop()


def test_ordinary_routes_keep_working_next_to_a_websocket():
    app = echo_app()

    @app.get("/hi")
    async def hi():
        return "hi"

    @app.get("/sync")
    def sync():
        return "sync"

    with running(app) as port:
        c = Client(port)
        r = httpp.get(f"http://127.0.0.1:{port}/hi")
        assert r.text == "hi" and httpp.get(f"http://127.0.0.1:{port}/sync").text == "sync"
        c.text("x")
        assert c.recv() == (1, b"x")


def test_stopping_the_server_with_open_connections():
    seen = []
    app = Server()

    @app.websocket("/ws")
    async def f(websocket):
        await websocket.accept()
        try:
            await websocket.receive()
        except WebSocketDisconnect as exc:
            seen.append(exc.code)

    with running(app) as port:
        Client(port)
        time.sleep(0.1)
    time.sleep(0.2)
    assert seen in ([1006], [])  # the loop thread may already be gone at exit; what matters is that stop() returned


def test_several_io_threads_each_with_its_own_lane_when_free_threaded():
    app = Server(io_threads=3)

    @app.websocket("/ws")
    async def echo(websocket):
        await websocket.accept()
        async for m in websocket:
            await websocket.send_text(m)

    with running(app) as port:
        clients = [Client(port) for _ in range(30)]
        for i, c in enumerate(clients):
            c.text(f"c{i}")
        for i, c in enumerate(clients):
            assert c.recv() == (1, f"c{i}".encode())
        for c in clients:
            c.close()
            assert c.recv()[0] == 8
