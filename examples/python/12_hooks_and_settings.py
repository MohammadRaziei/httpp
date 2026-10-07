"""Server settings, hooks, error pages and exception handlers.

Runs offline: it serves itself, calls itself, and exits.

    python 12_hooks_and_settings.py
"""
import threading

import httpp
from httpp import Server

# Settings are keyword arguments; anything you leave out keeps its default.
app = Server(
    threads=4,  # worker threads for plain `def` handlers
    max_body=1_000_000,  # larger request bodies get a 413
    keep_alive_max=50,  # requests per connection
    handler_timeout=10,  # an `async def` handler running longer is cancelled -> 504
)


@app.on_startup
def starting():
    print("startup hook: about to listen")


@app.on_shutdown
def stopping():
    print("shutdown hook: server stopped")


@app.before_request
def require_key(request):
    # Return None to carry on, or any response to answer right here (the route is skipped).
    if request.path != "/health" and request.headers.get("x-key") != "secret":
        return {"error": "missing or wrong X-Key"}, 401


@app.after_request
def stamp(request, response):
    # Status and headers only; the body isn't passed (it may be a huge file or a stream).
    response.set_header("X-Served-By", "httpp")


@app.error_handler(404)
def not_found(request):
    # Only for errors the server makes itself (no such route, ...). A route that returns
    # ("gone", 404) on purpose keeps its own body.
    return {"error": "no such page", "path": request.path}


@app.exception_handler(ValueError)
def bad_value(request, exc):
    # Runs when a route raises. Without a handler: a plain 500 and the traceback on stderr.
    return {"error": str(exc)}, 400


@app.get("/health")
def health():
    return "ok"


@app.get("/double/{n}")
def double(n):
    return {"result": int(n) * 2}  # int("abc") raises ValueError -> bad_value


# --- run it, call it, stop it ----------------------------------------------------------
port = app.bind_to_any_port("127.0.0.1")
thread = threading.Thread(target=app.listen_after_bind, daemon=True)  # also runs the startup/shutdown hooks
thread.start()
base = f"http://127.0.0.1:{port}"
key = {"X-Key": "secret"}

for path, headers in [("/health", {}), ("/double/21", {}), ("/double/21", key), ("/double/abc", key),
                      ("/nowhere", key)]:
    res = httpp.get(base + path, headers=headers)
    print(f"{path:12} key={'yes' if headers else 'no ':3} -> {res.status} {res.body}  "
          f"X-Served-By={res.header('X-Served-By')}")

app.stop()
thread.join()
