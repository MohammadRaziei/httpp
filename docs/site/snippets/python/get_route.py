from httpp import Server

app = Server()


@app.get("/hello/{name}")
def hello(name):
    return f"hello {name}"


app.listen("127.0.0.1", 8000)  # blocks; Ctrl+C stops it
