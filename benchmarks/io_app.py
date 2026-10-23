"""Handlers that wait for I/O (10 ms), served by httpp (`def` and `async def`) and by bare ASGI (uvicorn)."""
import asyncio
import time

import httpp

app = httpp.Server()


@app.get("/")
def wait_sync():
    time.sleep(0.01)  # a blocking call: holds a pool thread
    return "ok"


@app.get("/async")
async def wait_async():
    await asyncio.sleep(0.01)  # a non-blocking wait: holds nothing
    return "ok"


async def asgi(scope, receive, send):
    if scope["type"] != "http":
        return
    await asyncio.sleep(0.01)
    await send({"type": "http.response.start", "status": 200, "headers": [(b"content-type", b"text/plain")]})
    await send({"type": "http.response.body", "body": b"ok"})
