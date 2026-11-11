"""The httpp app of the benchmark suite. The same answers as suite_asgi.py and cpp/cpp_bench.cpp."""
import asyncio
import os

import httpp

# BENCH_INLINE=1 runs `async def` handlers that cannot suspend on the I/O thread (ADR-0023); off otherwise.
# BENCH_THREADS=n starts the worker pool at n threads (maximum 4 n); unset keeps the default.
_threads = int(os.environ.get("BENCH_THREADS", "0")) or None
app = httpp.Server(openapi_url=None, inline_async=os.environ.get("BENCH_INLINE") == "1", threads=_threads,
                   max_threads=_threads * 4 if _threads else None)
# BENCH_MAX=m and BENCH_GROW_MS=ms (with BENCH_THREADS) set the ceiling and the growth wait of the pool too (ADR-0059).
if os.environ.get("BENCH_GROW_MS"):
    app.set_thread_pool(_threads, int(os.environ["BENCH_MAX"]), grow_after_ms=float(os.environ["BENCH_GROW_MS"]))


@app.get("/sync")
def sync_hello():
    return "hello"


@app.get("/async")
async def async_hello():
    return "hello"


@app.get("/await")
async def await_hello():
    await asyncio.sleep(0)
    return "hello"


def _work():
    return sum(i * i for i in range(2000))


@app.get("/cpu")
def cpu():  # pure Python work: this is where a free-threaded interpreter can pay
    return str(_work())


@app.get("/acpu")
async def acpu():  # the same work inside an async handler (runs on a loop thread)
    return str(_work())


@app.get("/item/{item_id}")
def item(item_id: int):
    return {"id": item_id, "ok": True}


@app.get("/aitem/{item_id}")
async def aitem(item_id: int):
    return {"id": item_id, "ok": True}


@app.websocket("/ws")
async def ws_echo(websocket):  # an echo: the same as suite_asgi.py, ws_websockets.py and the C++ servers
    await websocket.accept()
    async for message in websocket:
        if isinstance(message, str):
            await websocket.send_text(message)
        else:
            await websocket.send_bytes(message)
