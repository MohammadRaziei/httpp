"""The httpp app of the benchmark suite. The same answers as suite_asgi.py and cpp/cpp_bench.cpp."""
import asyncio

import httpp

app = httpp.Server(openapi_url=None)


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
