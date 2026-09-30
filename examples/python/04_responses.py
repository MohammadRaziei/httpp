"""What a handler can return: str, bytes, dict/list (JSON), None,
optionally as (body, status) or (body, status, headers).

    python 04_responses.py
    curl -i http://127.0.0.1:8000/json
    curl -i http://127.0.0.1:8000/html
    curl -i http://127.0.0.1:8000/created
"""
from httpp import Server

app = Server()


@app.get("/text")
def text():
    return "plain text"  # text/plain


@app.get("/json")
def json_():
    return {"ok": True, "items": [1, 2, 3]}  # dict/list -> application/json


@app.get("/bytes")
def bytes_():
    return b"\x00\x01\x02"  # application/octet-stream


@app.get("/html")
def html():
    return "<h1>hi</h1>", 200, {"Content-Type": "text/html; charset=utf-8"}


@app.post("/created")
def created():
    return {"id": 1}, 201, {"Location": "/items/1", "X-Custom": "yes"}


@app.delete("/gone")
def gone():
    return None, 204  # empty body


app.listen("127.0.0.1", 8000)
