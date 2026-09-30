"""Path parameters, Flask-style <name> or FastAPI-style {name}.

    python 02_path_params.py
    curl http://127.0.0.1:8000/hello/world
    curl http://127.0.0.1:8000/square/7
    curl http://127.0.0.1:8000/square/abc     # 422: not an int
"""
from httpp import Server

app = Server()


@app.get("/hello/{name}")  # "/hello/<name>" means the same thing
def hello(name):
    return f"hello {name}"


@app.get("/square/{n}")
def square(n: int):  # annotate as int/float and it is converted for you
    return {"n": n, "square": n * n}


app.listen("127.0.0.1", 8000)
