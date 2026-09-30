"""A tiny in-memory CRUD API: get/post/put/patch/delete and route(methods=[...]).

    python 05_http_methods.py
    curl -X POST http://127.0.0.1:8000/items -d '{"name": "apple"}'
    curl http://127.0.0.1:8000/items
    curl -X PATCH http://127.0.0.1:8000/items/1 -d '{"name": "pear"}'
    curl -X DELETE http://127.0.0.1:8000/items/1
"""
import itertools

from httpp import Server

app = Server()
items = {}
ids = itertools.count(1)  # next() is atomic; handlers run on several threads


@app.get("/items")
def list_items():
    return list(items.values())


@app.post("/items")
def create(request):
    item = {"id": next(ids), **request.json()}
    items[item["id"]] = item
    return item, 201


@app.get("/items/{id}")
def read(id: int):
    return items.get(id) or ({"error": "not found"}, 404)


@app.route("/items/{id}", methods=["PUT", "PATCH"])  # one handler, several methods
def update(id: int, request):
    if id not in items:
        return {"error": "not found"}, 404
    items[id] = {"id": id, **request.json()}
    return items[id]


@app.delete("/items/{id}")
def delete(id: int):
    return (None, 204) if items.pop(id, None) else ({"error": "not found"}, 404)


app.listen("127.0.0.1", 8000)
