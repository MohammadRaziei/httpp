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


@app.post("/upload")
def upload(request):  # a multipart/form-data body: text fields and files
    form = request.form()
    doc = form.file("doc")
    return "\n".join([
        f"title={form.field('title')}",
        f"name={doc.filename}",
        f"type={doc.content_type}",
        f"size={doc.size}",
    ])


if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)
