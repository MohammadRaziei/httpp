import httpp

app = httpp.Server()


@app.get("/hello/{name}")
def hello(name):
    return f"hello {name}"


if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)  # blocks; Ctrl+C stops it
