"""A bare ASGI app for uvicorn: the floor for what an ASGI server can do. Same answers as suite_app.py."""
import os


def _work():
    return sum(i * i for i in range(2000))


async def app(scope, receive, send):
    if scope["type"] == "websocket":  # an echo
        await receive()  # websocket.connect
        await send({"type": "websocket.accept"})
        while True:
            event = await receive()
            if event["type"] != "websocket.receive":
                return
            await send({"type": "websocket.send", "text": event.get("text"), "bytes": event.get("bytes")})
    if scope["type"] != "http":
        return
    path = scope["path"]
    if path == "/upload":  # reads the whole body and answers its size
        size = 0
        while True:
            event = await receive()
            size += len(event.get("body", b""))
            if not event.get("more_body"):
                break
        body, kind = str(size).encode(), b"text/plain"
    elif path == "/file":  # the file is read for every request, like a handler that sends a file would
        with open(os.environ["BENCH_FILE"], "rb") as f:
            body, kind = f.read(), b"application/octet-stream"
    elif path.startswith("/item/"):
        body = b'{"id":%d,"ok":true}' % int(path[6:])
        kind = b"application/json"
    else:
        body = str(_work()).encode() if path in ("/cpu", "/acpu") else b"hello"
        kind = b"text/plain"
    await send({"type": "http.response.start", "status": 200,
                "headers": [(b"content-type", kind), (b"content-length", str(len(body)).encode())]})
    await send({"type": "http.response.body", "body": body})
