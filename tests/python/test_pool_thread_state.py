"""A sync handler's pool thread keeps its Python thread state between requests.

Creating and destroying a thread state for every request is cheap with a GIL but very slow on
free-threaded CPython (about 5x fewer requests per second). threading.local data lives on the
thread state, so it survives from one request to the next only if the state is reused.
"""

import threading
import time

import httpp
from httpp import Client, Server


def test_pool_thread_state_is_reused():
    local = threading.local()
    seen = []
    app = Server()

    @app.get("/count")
    def count():
        local.n = getattr(local, "n", 0) + 1
        seen.append(local.n)
        return "ok"

    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        cli = Client(f"http://127.0.0.1:{port}")
        for _ in range(20):
            assert cli.get("/count").status == 200
    finally:
        app.stop()
        th.join()
    assert max(seen) > 1


def _serve_once(requests):
    """Start a server, send `requests` requests to a sync handler, stop it. Every request runs on a worker of the pool."""
    app = Server()
    app.thread_pool(4, 4)

    @app.get("/ok")
    def ok():
        return "ok"

    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        cli = Client(f"http://127.0.0.1:{port}")
        for _ in range(requests):
            assert cli.get("/ok").status == 200
    finally:
        app.stop()
        th.join()


def test_stopped_servers_give_their_thread_states_back(python_thread_states):
    """ADR-0112: a worker thread keeps one Python thread state while it lives and frees it when it ends.

    Without that, every server a program starts and stops leaves one state per worker behind until the interpreter exits: on
    32-bit free-threaded CPython each one holds tens of MiB of address space."""
    _serve_once(8)  # warm up: first-use states (imports, the client's own threads) are not what is measured
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and threading.active_count() > 1:
        time.sleep(0.01)
    before = python_thread_states()
    for _ in range(10):
        _serve_once(8)
    time.sleep(0.2)
    after = python_thread_states()
    # Measured on free-threaded 3.14.6: no growth with the hook, +10 (one per server) without it. Allow a little for threads of the
    # interpreter itself.
    assert after - before <= 3, f"thread states grew from {before} to {after}"
