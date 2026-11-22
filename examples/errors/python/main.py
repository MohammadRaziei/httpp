import httpp

app = httpp.Server()
users = {1: "ada"}


@app.get("/users/{id}")
def user(id: int):
    if id not in users:
        return {"error": "no such user"}, 404  # your own answer: it is sent as it is
    return {"id": id, "name": users[id]}


@app.get("/parse")
def parse():
    raise ValueError("not a number")  # a raise ends in the exception handler below


@app.get("/boom")
def boom():
    raise RuntimeError("secret internals")  # nobody handles this: the client sees a plain 500


@app.exception_handler(ValueError)
def bad_value(request, exc):
    return {"error": str(exc)}, 422


@app.error_handler(404)
def not_found(request):  # the server's own 404, for a path that no route has
    return {"error": "not found"}


if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)
