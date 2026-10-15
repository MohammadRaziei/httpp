import json, warnings
warnings.filterwarnings("ignore")
from typing import Annotated, List, Optional
from fastapi import FastAPI, Depends, HTTPException, Header, Query, APIRouter, Response, BackgroundTasks
from fastapi.responses import JSONResponse, PlainTextResponse, RedirectResponse
from fastapi.testclient import TestClient
log = []
app = FastAPI()
def dep_a(q: int = 1):
    log.append("a-start"); yield q; log.append("a-end")
def dep_b(a: int = Depends(dep_a)):
    log.append("b"); return a + 1
async def dep_c(b: int = Depends(dep_b), a: int = Depends(dep_a)):
    log.append("c"); return (a, b)
@app.get("/d")
def d(c=Depends(dep_c), b=Depends(dep_b), x: int = Query(0)):
    log.append("handler"); return {"c": c, "b": b}
@app.get("/dfail")
def dfail(c=Depends(dep_c)):
    raise HTTPException(409, "conflict")
class Cls:
    def __init__(self, k: str = "kv"): self.k = k
@app.get("/cls")
def cls(o: Cls = Depends()): return {"k": o.k}
@app.get("/ann")
def ann(o: Annotated[Cls, Depends()], q: Annotated[int, Query(ge=1)] = 5): return {"k": o.k, "q": q}
@app.get("/resp")
def resp(response: Response):
    response.headers["x-one"] = "1"; response.status_code = 202; return {"ok": True}
@app.get("/json")
def js(): return JSONResponse({"a": 1}, status_code=203, headers={"x-j": "1"})
@app.get("/text")
def tx(): return PlainTextResponse("hi")
@app.get("/redir")
def rd(): return RedirectResponse("/text")
@app.get("/none")
def none(): return None
@app.get("/status", status_code=201)
def st(): return {"a": 1}
@app.get("/bg")
def bg(tasks: BackgroundTasks):
    tasks.add_task(lambda: log.append("bg")); return {"ok": 1}
r = APIRouter(prefix="/r", tags=["t"], dependencies=[Depends(lambda: log.append("router-dep"))])
@r.get("/x")
def rx(): return {"x": 1}
app.include_router(r)
app.dependency_overrides[dep_b] = lambda: 99
c = TestClient(app, raise_server_exceptions=False)
def show(label, r):
    try: body = r.json()
    except Exception: body = r.text
    print(f"{label:14} {r.status_code} {json.dumps(body)[:300]} {dict((k,v) for k,v in r.headers.items() if k.startswith('x-') or k=='content-type' or k=='location')}")
show("filtered", c.get("/filt")); show("404", c.get("/nope")); show("405", c.post("/d"))
show("d (override b)", c.get("/d?q=5&x=3")); print("log", log); log.clear()
app.dependency_overrides.clear()
show("d", c.get("/d?q=5")); print("log", log); log.clear()
show("dfail", c.get("/dfail")); print("log", log); log.clear()
show("d bad q", c.get("/d?q=z")); print("log", log); log.clear()
show("cls", c.get("/cls?k=zz")); show("ann", c.get("/ann?k=zz&q=0"))
show("resp", c.get("/resp")); show("json", c.get("/json")); show("text", c.get("/text")); show("redir", c.get("/redir", follow_redirects=False)); show("none", c.get("/none")); show("status", c.get("/status"))
show("bg", c.get("/bg")); print("log", log); log.clear()
show("router", c.get("/r/x")); print("log", log)
