"""The uvicorn side: bare ASGI (no framework), so the comparison is server against server."""
import json

_HELLO = [(b"content-type", b"text/plain; charset=utf-8")]
_JSON = [(b"content-type", b"application/json")]


async def app(scope, receive, send):
    if scope["type"] != "http":
        return
    path = scope["path"]
    if path.startswith("/item/"):
        body = json.dumps({"id": int(path[6:]), "ok": True}).encode()
        headers = _JSON
    elif path == "/agent":
        agent = dict(scope["headers"]).get(b"user-agent", b"")
        body, headers = agent, _HELLO
    else:
        body, headers = b"hello", _HELLO
    await send({"type": "http.response.start", "status": 200, "headers": headers})
    await send({"type": "http.response.body", "body": body})
