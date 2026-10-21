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
