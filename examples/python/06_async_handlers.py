"""`async def` handlers work next to plain `def` ones, and they are truly
non-blocking: while a handler awaits, it holds no thread, so far more requests
than the server has threads can wait at the same time.

    python 06_async_handlers.py
    curl http://127.0.0.1:8000/sync
    curl http://127.0.0.1:8000/async

Fire many requests at /async at the same time (a load tool such as `hey`, `ab` or
`wrk`, or a thread pool): they overlap instead of queueing behind a limited number
of worker threads.
"""
import asyncio

from httpp import Server

app = Server()


@app.get("/sync")
def sync_handler():
    return "plain function"


@app.get("/async")
async def async_handler():
    # Await anything: async DB drivers, aiohttp, asyncio.gather, ...
    a, b = await asyncio.gather(fetch("a"), fetch("b"))
    return {"a": a, "b": b}


async def fetch(name):
    await asyncio.sleep(0.1)
    return f"result {name}"


app.listen("127.0.0.1", 8000)
