import httpp

app = httpp.Server()
users = {1: "ada"}


@app.get("/users/{id}")
def user(id: int):
    if id not in users:
        raise httpp.HTTPException(404, "no such user")  # the client gets {"detail": "no such user"} and 404
    return {"id": id, "name": users[id]}


@app.get("/admin")
def admin():
    raise httpp.HTTPException(401, "no token", headers={"WWW-Authenticate": "Bearer"})


if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)
