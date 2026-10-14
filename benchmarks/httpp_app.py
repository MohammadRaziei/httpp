"""The httpp side of the benchmark: the same three routes written as `def` and as `async def`."""
import httpp

app = httpp.Server()


@app.get("/")
def hello():
    return "hello"


@app.get("/async")
async def hello_async():
    return "hello"


@app.get("/item/{item_id}")
def item(item_id: int):
    return {"id": item_id, "ok": True}


@app.get("/async/item/{item_id}")
async def item_async(item_id: int):
    return {"id": item_id, "ok": True}


@app.get("/agent")
def agent(request):
    return request.headers.get("user-agent", "")


@app.get("/async/agent")
async def agent_async(request):
    return request.headers.get("user-agent", "")


# Many routes: matching must not slow down with the number of routes. The benchmark asks for the last one.
def _add_many(n=300):
    for i in range(n):
        def sync_handler(i=i):
            return "hello"

        async def async_handler(i=i):
            return "hello"

        app.get(f"/many/r{i}/x")(sync_handler)
        app.get(f"/async/many/r{i}/x")(async_handler)


_add_many()
