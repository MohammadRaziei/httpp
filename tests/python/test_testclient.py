"""httpp.testclient.TestClient: a Starlette-shaped client that serves the app on a loopback port of its own (ADR-0042)."""

import json
import urllib.parse

import pytest

import httpp
from httpp import HTTPException, RedirectResponse, Server
from httpp.testclient import TestClient


def make_app():
    app = Server()
    calls = {"startup": 0, "shutdown": 0}

    @app.on_startup
    def up():
        calls["startup"] += 1

    @app.on_shutdown
    def down():
        calls["shutdown"] += 1

    @app.get("/")
    def index():
        return {"hello": "world"}

    @app.get("/items/{item_id}")
    def item(item_id: int, q: str = ""):
        return {"id": item_id, "q": q}

    @app.post("/echo")
    def echo(request: httpp.ServerRequest):
        return {"method": request.method, "type": request.header("content-type"), "body": request.text}

    @app.route("/any", methods=("GET", "PUT", "PATCH", "DELETE", "OPTIONS"))
    def any_method(request: httpp.ServerRequest):
        return {"method": request.method}

    @app.get("/headers")
    def headers(request: httpp.ServerRequest):
        return {"host": request.header("host"), "x": request.header("x-token"), "auth": request.header("authorization"),
                "cookie": request.header("cookie")}

    @app.get("/login")
    def login():
        return "ok", 200, [httpp.set_cookie("sid", "abc123")]

    @app.get("/logout")
    def logout():
        return "bye", 200, [httpp.set_cookie("sid", "x", max_age=0)]

    @app.get("/me")
    def me(request: httpp.ServerRequest):
        return {"sid": request.cookie("sid")}

    @app.get("/old")
    def old():
        return RedirectResponse("/new", status_code=302)

    @app.get("/new")
    def new():
        return {"page": "new"}

    @app.get("/hop1")
    def hop1():
        return RedirectResponse("/hop2", status_code=307)

    @app.get("/hop2")
    def hop2():
        return RedirectResponse("/new", status_code=301)

    @app.get("/loop")
    def loop():
        return RedirectResponse("/loop")

    @app.get("/gone")
    def gone():
        raise HTTPException(404, "no such thing")

    @app.get("/boom")
    def boom():
        raise RuntimeError("kaput")

    @app.get("/async")
    async def a():
        return {"async": True}

    @app.get("/async-boom")
    async def async_boom():
        raise ValueError("async kaput")

    @app.exception_handler(KeyError)
    def key_error(request, exc):
        return httpp.JSONResponse({"handled": "KeyError"}, status_code=418)

    @app.get("/key")
    def key():
        raise KeyError("k")

    return app, calls


@pytest.fixture
def app_and_calls():
    return make_app()


@pytest.fixture
def client(app_and_calls):
    with TestClient(app_and_calls[0]) as c:
        yield c


def test_get_json_text_and_content(client):
    r = client.get("/")
    assert r.status_code == 200
    assert r.json() == {"hello": "world"}
    assert r.text.startswith("{") and r.content == r.text.encode()
    assert r.headers["Content-Type"] == "application/json"
    assert r.headers["content-type"] == "application/json"  # case-insensitive
    assert r.is_success and not r.is_redirect and r.reason_phrase == "OK"
    assert r.url == "http://testserver/"
    assert r.raise_for_status() is r


def test_path_and_query_parameters(client):
    assert client.get("/items/7", params={"q": "a b"}).json() == {"id": 7, "q": "a b"}
    assert client.get("/items/7?q=raw").json() == {"id": 7, "q": "raw"}


def test_every_method_exists(client):
    assert client.put("/any").json() == {"method": "PUT"}
    assert client.patch("/any").json() == {"method": "PATCH"}
    assert client.delete("/any").json() == {"method": "DELETE"}
    assert client.options("/any").json() == {"method": "OPTIONS"}
    r = client.head("/")
    assert r.status_code == 200 and r.content == b""
    assert client.request("GET", "/any").json() == {"method": "GET"}


def test_json_form_and_raw_bodies(client):
    r = client.post("/echo", json={"a": [1, 2]})
    assert r.json()["type"] == "application/json" and json.loads(r.json()["body"]) == {"a": [1, 2]}
    r = client.post("/echo", data={"a": "1", "b": "two words"})
    assert r.json()["type"] == "application/x-www-form-urlencoded"
    assert urllib.parse.parse_qs(r.json()["body"]) == {"a": ["1"], "b": ["two words"]}
    r = client.post("/echo", data=b"raw bytes", headers={"Content-Type": "text/plain"})
    assert r.json() == {"method": "POST", "type": "text/plain", "body": "raw bytes"}
    r = client.post("/echo", content="as text")
    assert r.json()["body"] == "as text"


def test_default_and_per_request_headers_and_host(app_and_calls):
    with TestClient(app_and_calls[0], headers={"X-Token": "client"}) as c:
        assert c.get("/headers").json()["x"] == "client"
        assert c.get("/headers", headers={"x-token": "request"}).json()["x"] == "request"
        assert c.get("/headers").json()["host"] == "testserver"
    with TestClient(app_and_calls[0], base_url="http://example.org:8080") as c:
        assert c.get("/headers").json()["host"] == "example.org:8080"
        assert c.get("/headers").url == "http://example.org:8080/headers"


def test_basic_auth(client):
    assert client.get("/headers", auth=("user", "pw")).json()["auth"] == "Basic dXNlcjpwdw=="


def test_cookies_are_kept_between_requests(client):
    assert client.get("/me").json() == {"sid": ""}
    r = client.get("/login")
    assert r.cookies["sid"] == "abc123"
    assert client.cookies["sid"] == "abc123"
    assert client.get("/me").json() == {"sid": "abc123"}
    client.get("/logout")
    assert "sid" not in client.cookies
    assert client.get("/me").json() == {"sid": ""}


def test_cookies_given_by_the_caller(app_and_calls):
    with TestClient(app_and_calls[0], cookies={"sid": "from-client"}) as c:
        assert c.get("/me").json() == {"sid": "from-client"}
        assert c.get("/me", cookies={"sid": "from-request"}).json() == {"sid": "from-request"}
        assert c.get("/me").json() == {"sid": "from-client"}  # a per-request cookie is not kept


def test_redirects_are_followed_by_default(client):
    r = client.get("/old")
    assert r.status_code == 200 and r.json() == {"page": "new"}
    assert r.url == "http://testserver/new"
    assert [h.status_code for h in r.history] == [302]
    r = client.get("/hop1")
    assert r.json() == {"page": "new"} and [h.status_code for h in r.history] == [307, 301]


def test_redirects_can_be_left_alone(client):
    r = client.get("/old", follow_redirects=False)
    assert r.status_code == 302 and r.headers["location"] == "/new" and r.is_redirect and r.history == []


def test_a_redirect_loop_is_an_error(client):
    with pytest.raises(httpp.TooManyRedirects):
        client.get("/loop")


def test_error_statuses_are_responses(client):
    r = client.get("/gone")
    assert r.status_code == 404 and r.json() == {"detail": "no such thing"}
    assert client.get("/nowhere").status_code == 404
    assert client.post("/").status_code == 405
    with pytest.raises(httpp.HTTPError):
        client.get("/gone").raise_for_status()


def test_async_handlers_work(client):
    assert client.get("/async").json() == {"async": True}


def test_server_exceptions_are_raised_in_the_test(client):
    with pytest.raises(RuntimeError, match="kaput"):
        client.get("/boom")
    assert client.get("/").status_code == 200  # the client is still usable


def test_async_handler_exceptions_are_raised_in_the_test(client):
    with pytest.raises(ValueError, match="async kaput"):
        client.get("/async-boom")
    assert client.get("/async").json() == {"async": True}


def test_server_exceptions_can_become_a_500(app_and_calls):
    with TestClient(app_and_calls[0], raise_server_exceptions=False) as c:
        r = c.get("/boom")
        assert r.status_code == 500


def test_an_exception_with_a_handler_is_not_raised(client):
    r = client.get("/key")
    assert r.status_code == 418 and r.json() == {"handled": "KeyError"}


def test_with_runs_startup_and_shutdown_once(app_and_calls):
    app, calls = app_and_calls
    with TestClient(app) as c:
        assert calls == {"startup": 1, "shutdown": 0}
        c.get("/")
        c.get("/")
        assert calls == {"startup": 1, "shutdown": 0}
    assert calls == {"startup": 1, "shutdown": 1}


def test_without_with_the_server_starts_on_the_first_request_and_close_stops_it(app_and_calls):
    app, calls = app_and_calls
    c = TestClient(app)
    assert calls["startup"] == 0  # nothing runs before the first request
    assert c.get("/").status_code == 200
    assert c.get("/").status_code == 200
    assert calls == {"startup": 1, "shutdown": 0}
    c.close()
    assert calls == {"startup": 1, "shutdown": 1}
    c.close()  # closing twice is harmless
    assert calls["shutdown"] == 1


def test_a_client_can_be_used_again_after_it_was_closed(app_and_calls):
    app, calls = app_and_calls
    c = TestClient(app)
    with c:
        c.get("/")
    with c:
        assert c.get("/").status_code == 200
    assert calls == {"startup": 2, "shutdown": 2}


def test_two_clients_for_two_apps_do_not_mix():
    a, _ = make_app()
    b = Server()

    @b.get("/")
    def other():
        return {"app": "b"}

    with TestClient(a) as ca, TestClient(b) as cb:
        assert ca.get("/").json() == {"hello": "world"}
        assert cb.get("/").json() == {"app": "b"}


def test_the_client_only_takes_a_server(client):
    with pytest.raises(TypeError):
        TestClient(object())


# ---- multipart files= (ADR-0042 addendum, ADR-0046) ----

def upload_app():
    app = Server(openapi_url=None)

    @app.post("/up")
    def up(request: httpp.ServerRequest):
        form = request.form()
        return {
            "fields": {k: form.getlist(k) for k in ("who", "tag")},
            "files": {k: {"filename": f.filename, "type": f.content_type, "size": f.size, "text": f.text()}
                      for k in ("doc", "pic") if (f := form.file(k)) is not None},
            "type": request.header("content-type").split(";")[0],
        }

    return app


def test_files_are_sent_as_multipart_with_the_data_fields():
    with TestClient(upload_app()) as c:
        r = c.post("/up", data={"who": "me", "tag": ["a", "b"]}, files={"doc": ("r.txt", b"hello", "text/plain")})
        assert r.status_code == 200
        body = r.json()
        assert body["type"] == "multipart/form-data"
        assert body["fields"] == {"who": ["me"], "tag": ["a", "b"]}
        assert body["files"]["doc"] == {"filename": "r.txt", "type": "text/plain", "size": 5, "text": "hello"}


def test_a_file_value_can_be_bytes_a_pair_a_file_object_or_come_from_a_list():
    import io

    with TestClient(upload_app()) as c:
        r = c.post("/up", files={"doc": b"raw bytes", "pic": ("p.png", io.BytesIO(b"\x89PNG-ish"))})
        files = r.json()["files"]
        assert files["doc"]["size"] == 9 and files["doc"]["type"] == "application/octet-stream"
        assert files["pic"]["filename"] == "p.png" and files["pic"]["type"] == "image/png"  # guessed from the name
        r = c.post("/up", files=[("doc", ("a.txt", "text body")), ("doc", ("b.txt", b"x"))])
        assert r.json()["files"]["doc"]["filename"] in ("a.txt", "b.txt")


def test_a_filename_with_quotes_and_non_ascii_survives():
    with TestClient(upload_app()) as c:
        r = c.post("/up", files={"doc": ('café "x".txt', b"1", "text/plain")})
        assert r.json()["files"]["doc"]["filename"] == 'café "x".txt'


def test_files_cannot_be_combined_with_json_or_content():
    with TestClient(upload_app()) as c:
        with pytest.raises(ValueError):
            c.post("/up", files={"doc": b"x"}, json={"a": 1})
        with pytest.raises(ValueError):
            c.post("/up", files={"doc": b"x"}, content=b"y")
        with pytest.raises(ValueError):
            c.post("/up", files={"doc": b"x"}, data=b"y")  # data must be a dict next to files


# ---- websocket_connect ----

def ws_app():
    app = Server(openapi_url=None)

    @app.websocket("/ws")
    async def echo(websocket):
        await websocket.accept()
        async for message in websocket:
            if isinstance(message, str):
                await websocket.send_text(message)
            else:
                await websocket.send_bytes(message)

    @app.websocket("/proto")
    async def proto(websocket):
        await websocket.accept(subprotocol="chat")
        await websocket.send_text("hi")
        await websocket.close(1001, "later")

    @app.websocket("/cookie")
    async def cookie(websocket):
        await websocket.accept()
        await websocket.send_json({"cookie": websocket.headers.get("cookie"), "x": websocket.headers.get("x-token"),
                                   "other": websocket.headers.get("x-other"), "host": websocket.headers.get("host")})
        await websocket.close()

    @app.websocket("/refuse")
    async def refuse(websocket):
        await websocket.close(1008)

    @app.get("/login")
    def login():
        return "ok", 200, [httpp.set_cookie("sid", "abc")]

    return app


def test_websocket_connect_echoes_text_binary_and_json():
    with TestClient(ws_app()) as c:
        with c.websocket_connect("/ws") as ws:
            ws.send_text("héllo")
            assert ws.receive_text() == "héllo"
            ws.send_bytes(b"\x00\x01\xff")
            assert ws.receive_bytes() == b"\x00\x01\xff"
            ws.send_json({"a": [1, 2]})
            assert ws.receive_json() == {"a": [1, 2]}
            ws.send_json({"b": 1}, mode="binary")
            assert ws.receive_json(mode="binary") == {"b": 1}
            ws.send_text("m")
            assert ws.receive() == {"type": "websocket.receive", "text": "m"}


def test_websocket_connect_works_without_with_and_before_any_request():
    c = TestClient(ws_app())
    try:
        with c.websocket_connect("/ws") as ws:
            ws.send_text("x")
            assert ws.receive_text() == "x"
    finally:
        c.close()


def test_the_server_closing_raises_websocket_disconnect_with_its_code():
    with TestClient(ws_app()) as c:
        with c.websocket_connect("/proto", subprotocols=["chat"]) as ws:
            assert ws.accepted_subprotocol == "chat"
            assert ws.receive_text() == "hi"
            with pytest.raises(httpp.WebSocketDisconnect) as info:
                ws.receive_text()
            assert info.value.code == 1001 and info.value.reason == "later"


def test_a_refused_handshake_raises_websocket_disconnect():
    with TestClient(ws_app()) as c:
        with pytest.raises(httpp.WebSocketDisconnect):
            with c.websocket_connect("/refuse") as ws:
                ws.receive_text()
        with pytest.raises(httpp.WebSocketDisconnect):
            with c.websocket_connect("/no-such-route"):
                pass


def test_websocket_connect_sends_the_clients_cookies_headers_and_subprotocols():
    with TestClient(ws_app(), headers={"X-Token": "t1"}) as c:
        c.get("/login")
        with c.websocket_connect("/cookie", headers={"X-Other": "1"}) as ws:
            got = ws.receive_json()
        assert got == {"cookie": "sid=abc", "x": "t1", "other": "1", "host": "testserver"}
        with c.websocket_connect("/proto", subprotocols=["chat", "superchat"]) as ws:
            assert ws.accepted_subprotocol == "chat"


def test_the_client_closes_with_a_code_and_a_close_after_the_with_is_harmless():
    seen = []
    app = Server(openapi_url=None)

    @app.websocket("/ws")
    async def ws(websocket):
        await websocket.accept()
        try:
            await websocket.receive()
        except httpp.WebSocketDisconnect as exc:
            seen.append(exc.code)

    with TestClient(app) as c:
        with c.websocket_connect("/ws") as session:
            session.close(1001)
            session.close(1001)  # twice: nothing happens
        deadline = __import__("time").monotonic() + 5
        while not seen and __import__("time").monotonic() < deadline:
            __import__("time").sleep(0.01)
    assert seen == [1001]


def test_messages_sent_back_to_back_are_read_in_order():
    app = Server(openapi_url=None)

    @app.websocket("/ws")
    async def ws(websocket):
        await websocket.accept()
        await websocket.send_text("one")
        await websocket.send_text("two")

    with TestClient(app) as c:
        with c.websocket_connect("/ws") as session:
            assert session.receive_text() == "one"
            assert session.receive_text() == "two"
