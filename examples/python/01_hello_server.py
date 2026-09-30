"""The smallest possible server.

    python 01_hello_server.py
    curl http://127.0.0.1:8000/
"""
from httpp import Server

app = Server()


@app.get("/")
def index():
    return "hello from httpp"


app.listen("127.0.0.1", 8000)  # blocks; Ctrl+C stops it
