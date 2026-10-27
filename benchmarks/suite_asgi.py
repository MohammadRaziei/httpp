"""A bare ASGI app for uvicorn: the floor for what an ASGI server can do. Same answers as suite_app.py."""


def _work():
    return sum(i * i for i in range(2000))


async def app(scope, receive, send):
    if scope["type"] != "http":
        return
    body = str(_work()).encode() if scope["path"] in ("/cpu", "/acpu") else b"hello"
    await send({"type": "http.response.start", "status": 200,
                "headers": [(b"content-type", b"text/plain"), (b"content-length", str(len(body)).encode())]})
    await send({"type": "http.response.body", "body": body})
