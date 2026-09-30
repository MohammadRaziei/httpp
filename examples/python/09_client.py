"""HTTP client: Client(host, port).get(path) and Client.fetch(url).

Runs offline: it starts a small local server to talk to.
Swap in a real host/URL to use it against the internet (plain http only).

    python 09_client.py
"""
import threading

from httpp import Client, Server

# --- a local server to talk to (not part of the client API) -------------
app = Server()


@app.get("/hello")
def hello():
    return {"msg": "hi"}


port = app.bind_to_any_port("127.0.0.1")
threading.Thread(target=app.listen_after_bind, daemon=True).start()

# --- the client part ------------------------------------------------------
client = Client("127.0.0.1", port)
res = client.get("/hello")
print(res.status, res.ok)  # 200 True
print(res.body)  # body as str
print(res.header("content-type"))  # case-insensitive lookup, or None
print(res.headers)  # all headers, list of (name, value)

res = Client.fetch(f"http://127.0.0.1:{port}/hello")  # one call, no host/port splitting
print(res.status, res.body)

print(client.get("/missing").status)  # 404: no exception, check .ok / .status

app.stop()
