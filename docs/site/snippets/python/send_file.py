import httpp

app = httpp.Server()


@app.get("/report")
def report():
    # (file, status, headers): the file is streamed from disk
    return httpp.file("report.txt"), 200, {"Content-Disposition": 'attachment; filename="report.txt"'}


if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)
