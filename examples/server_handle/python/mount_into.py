import httpp

app = httpp.Server()
httpp.Directory("public").mount(app, "/static")  # `app` now serves /static


@app.get("/ping")
def ping():
    return "pong"


if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)
