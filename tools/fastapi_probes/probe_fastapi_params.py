import json
from typing import Annotated, List, Optional
from fastapi import FastAPI, Query, Header, Cookie, Body, Depends, HTTPException, Path, Response
from fastapi.testclient import TestClient
from pydantic import BaseModel, Field

app = FastAPI()
class Item(BaseModel):
    name: str
    price: float = Field(gt=0)
    tags: List[str] = []

@app.get("/q")
def q(a: int, b: Optional[str] = None, c: List[int] = Query(default=[])): return {"a": a, "b": b, "c": c}
@app.get("/h")
def h(user_agent: str, x_token: Optional[str] = Header(None), session: Optional[str] = Cookie(None)): return {"ua": user_agent, "x": x_token, "s": session}
@app.post("/items")
def items(item: Item): return item
@app.post("/two")
def two(item: Item, qty: int = Body()): return {"q": qty}
@app.post("/emb")
def emb(item: Item = Body(embed=True)): return item
@app.get("/p/{pid}")
def p(pid: int, extra: str): return {"pid": pid}
@app.get("/he")
def he(): raise HTTPException(status_code=418, detail={"why": "teapot"}, headers={"X-A": "b"})
@app.get("/lst")
def lst(l: List[int]): return l
@app.post("/raw")
def raw(x: dict): return x
@app.get("/bad-ret", response_model=Item)
def badret(): return {"name": 1}
@app.get("/filt", response_model=Item, status_code=201)
def filt(): return {"name": "n", "price": 1, "tags": ["a"], "secret": "x"}

c = TestClient(app, raise_server_exceptions=False)
def show(label, r):
    try: body = r.json()
    except Exception: body = r.text
    print(f"{label:34} {r.status_code} {json.dumps(body)[:420]}")

show("q ok", c.get("/q?a=1&c=2&c=3"))
show("q missing a", c.get("/q"))
show("q bad a", c.get("/q?a=x&c=y"))
show("q repeated a", c.get("/q?a=1&a=2"))
show("q empty a", c.get("/q?a="))
show("h", c.get("/h", headers={"User-Agent":"ua1","X-Token":"t"}, cookies={"session":"s1"}))
show("h missing ua", c.get("/h", headers={"User-Agent":""}))
show("items ok", c.post("/items", json={"name":"n","price":"1.5"}))
show("items bad", c.post("/items", json={"name":1,"price":-1}))
show("items empty body", c.post("/items"))
show("items invalid json", c.post("/items", content='{"name": ', headers={"content-type":"application/json"}))
show("items text/plain", c.post("/items", content='{"name":"n","price":1}', headers={"content-type":"text/plain"}))
show("items list body", c.post("/items", json=[1]))
show("two ok", c.post("/two", json={"item":{"name":"n","price":1},"qty":"2"}))
show("two missing", c.post("/two", json={"item":{"name":"n","price":1}}))
show("two non-object", c.post("/two", json=[1]))
show("emb", c.post("/emb", json={"item":{"name":"n","price":1}}))
show("emb bad", c.post("/emb", json={"name":"n","price":1}))
show("path bad", c.get("/p/x"))
show("http exc", c.get("/he")); print(dict(c.get("/he").headers).get("x-a"))
show("lst", c.get("/lst"))
show("lst body", c.post("/lst"))
show("raw dict", c.post("/raw", json={"a":1}))
show("bad return", c.get("/bad-ret"))
show("filtered", c.get("/filt"))
show("404", c.get("/nope"))
show("405", c.post("/q"))
