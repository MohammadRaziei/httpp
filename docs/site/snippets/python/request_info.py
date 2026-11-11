import httpp

app = httpp.Server()


@app.post("/inspect")
def inspect(request):  # ask for `request` by name
    return "\n".join([
        f"{request.method} {request.path}",
        f"page={request.query.get('page', '')}",
        f"token={request.headers.get('x-token', '')}",
        f"body={request.text}",
    ])


if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)
