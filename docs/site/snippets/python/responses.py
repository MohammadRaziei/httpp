from httpp import Server

app = Server()


@app.post("/items")
def create_item(request):
    # (body, status, headers): a dict body is sent as JSON
    return {"id": 1}, 201, {"Location": "/items/1"}


@app.get("/items/{item_id}")
def get_item(item_id):
    if item_id != "1":
        return {"error": "not found"}, 404
    return {"id": 1}


app.listen("127.0.0.1", 8000)
