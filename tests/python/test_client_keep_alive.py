"""Client(keep_alive=...) mirrors httpp::client::set_keep_alive (C++ default: on)."""

import threading
import time
from contextlib import contextmanager

import httpp
from httpp import Client, Server


@contextmanager
def serving(seen):
    app = Server()

    @app.before_request
    def record(request):
        seen.append(request.headers.get("connection", ""))

    @app.get("/")
    def index():
        return "ok"

    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        yield f"http://127.0.0.1:{port}"
    finally:
        app.stop()
        th.join()


def test_keep_alive_is_on_by_default():
    seen = []
    with serving(seen) as base:
        assert Client(base).get("/").status == 200
    assert "close" not in [s.lower() for s in seen]


def test_keep_alive_false_asks_the_server_to_close():
    seen = []
    with serving(seen) as base:
        assert Client(base, keep_alive=False).get("/").status == 200
    assert [s.lower() for s in seen] == ["close"]
