"""The FastAPI side: the same routes as httpp_app.py (path int parsing and a JSON dict), served by uvicorn."""
from fastapi import FastAPI, Request

app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None)


@app.get("/")
async def hello():
    return "hello"


@app.get("/item/{item_id}")
async def item(item_id: int):
    return {"id": item_id, "ok": True}


@app.get("/agent")
async def agent(request: Request):
    return request.headers.get("user-agent", "")


for _i in range(300):
    def _make(i=_i):
        async def h(x: str):
            return {"i": i}
        return h
    app.get(f"/many/r{_i}/{{x}}")(_make())
