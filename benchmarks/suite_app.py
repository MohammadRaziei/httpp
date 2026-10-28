"""The httpp app of the benchmark suite. The same answers as suite_asgi.py and cpp/cpp_bench.cpp."""
import asyncio
import os

import httpp

# BENCH_INLINE=1 runs `async def` handlers that cannot suspend on the I/O thread (ADR-0023); off otherwise.
# BENCH_THREADS=n starts the worker pool at n threads (maximum 4 n); unset keeps the default.
_threads = int(os.environ.get("BENCH_THREADS", "0")) or None
app = httpp.Server(openapi_url=None, inline_async=os.environ.get("BENCH_INLINE") == "1", threads=_threads,
                   max_threads=_threads * 4 if _threads else None)


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
