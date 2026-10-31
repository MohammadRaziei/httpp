"""A bare ASGI app for uvicorn: the floor for what an ASGI server can do. Same answers as suite_app.py."""


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
    if path.startswith("/item/"):
        body = b'{"id":%d,"ok":true}' % int(path[6:])
        kind = b"application/json"
    else:
        body = str(_work()).encode() if path in ("/cpu", "/acpu") else b"hello"
        kind = b"text/plain"
    await send({"type": "http.response.start", "status": 200,
                "headers": [(b"content-type", kind), (b"content-length", str(len(body)).encode())]})
    await send({"type": "http.response.body", "body": body})
