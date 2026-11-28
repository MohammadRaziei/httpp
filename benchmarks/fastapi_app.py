"""The FastAPI side: the same routes as httpp_app.py (path int parsing and a JSON dict), served by uvicorn."""
import os

from fastapi import FastAPI, Request
from fastapi.responses import FileResponse

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


# The parts "upload", "file" and "mw" of the suite (the same answers as suite_app.py and suite_asgi.py). BENCH_MW=1 installs the same two
# middleware, with the same settings, as suite_app.py does.
@app.post("/upload")
async def upload(request: Request):
    return str(len(await request.body()))


@app.get("/file")
async def send_the_file():
    return FileResponse(os.environ["BENCH_FILE"])


if os.environ.get("BENCH_MW") == "1":
    from starlette.middleware.cors import CORSMiddleware
    from starlette.middleware.trustedhost import TrustedHostMiddleware
    app.add_middleware(CORSMiddleware, allow_origins=["*"])
    app.add_middleware(TrustedHostMiddleware, allowed_hosts=["127.0.0.1"])


@app.get("/mw")
async def mw_hello():
    return "hello"
