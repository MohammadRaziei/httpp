"""Errors: return a 4xx yourself, or let an exception become a generic 500.

    python 08_error_handling.py
    curl -i http://127.0.0.1:8000/users/1
    curl -i http://127.0.0.1:8000/users/99    # 404 you returned
    curl -i http://127.0.0.1:8000/boom        # 500; the traceback shows here, not in the response
    curl -i http://127.0.0.1:8000/nope        # 404 from the server itself
"""
from httpp import Server

app = Server()
users = {1: "ada"}


@app.get("/users/{id}")
def user(id: int):
    if id not in users:
        return {"error": "user not found"}, 404
    return {"id": id, "name": users[id]}


@app.get("/boom")
def boom():
    raise RuntimeError("secret internals")  # client only sees "Internal Server Error"


try:
    app.listen("127.0.0.1", 8000)
except OSError as e:  # e.g. the port is already taken
    raise SystemExit(f"could not start: {e}")
