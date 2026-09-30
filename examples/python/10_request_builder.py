"""curl-flavoured request builder: method, headers, body, content type.

Runs offline: it starts a small local server that echoes what it received.

    python 10_request_builder.py
"""
import threading

from httpp import Request, Server

# --- a local echo server (not part of the client API) -------------------
app = Server()


@app.route("/echo", methods=["POST", "PUT"])
def echo(request):
    return {
        "method": request.method,
        "token": request.headers.get("x-token"),
        "content_type": request.headers.get("content-type"),
        "body": request.text,
    }


port = app.bind_to_any_port("127.0.0.1")
threading.Thread(target=app.listen_after_bind, daemon=True).start()

# --- the request builder --------------------------------------------------
url = f"http://127.0.0.1:{port}/echo"

res = (
    Request(url)  # like: curl -X POST -H "X-Token: abc" -d '{"a": 1}' url
    .method("POST")
    .header("X-Token", "abc")
    .content_type("application/json")
    .data('{"a": 1}')
    .run()
)
print(res.status, res.body)

# every step is optional; without .content_type(), data is sent as form-urlencoded (like curl -d)
print(Request(url).method("PUT").data("plain text").run().body)

app.stop()
