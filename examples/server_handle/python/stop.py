import httpp

app = httpp.Server()


@app.get("/quit")
def quit_server():
    app.shutdown(2)  # refuse new connections, let requests in progress finish, give up after 2 seconds
    return "bye"


if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)
