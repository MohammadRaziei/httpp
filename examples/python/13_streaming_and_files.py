"""Streaming bodies, files from disk, redirects and cookies.

Runs offline: it serves itself, calls itself, and exits.

    python 13_streaming_and_files.py
"""
import asyncio
import tempfile
import threading
from pathlib import Path

import httpp
from httpp import Server

tmp = tempfile.TemporaryDirectory()
report = Path(tmp.name, "report.json")
report.write_text('{"rows": 3}')

app = Server()


@app.get("/countdown")
def countdown():
    # A generator is sent chunk by chunk (Transfer-Encoding: chunked); str or bytes pieces.
    def pieces():
        for n in (3, 2, 1):
            yield f"{n}...\n"
        yield b"liftoff\n"

    return pieces()


@app.get("/events")
async def events():
    # An async generator works too, and so does a status/headers tuple around it.
    async def pieces():
        for n in range(3):
            await asyncio.sleep(0.01)
            yield f"data: {n}\n\n"

    return pieces(), 200, {"Content-Type": "text/event-stream"}


@app.get("/report")
def send_report():
    # Streamed from disk with a known length; Content-Type guessed from the extension.
    # Never build the path from user input (path traversal): use serve_directory for that.
    return httpp.file(report)


@app.get("/old")
def old():
    return httpp.redirect("/report", 301)


@app.get("/login")
def login(request):
    print("cookies the client sent:", request.cookies)
    # Headers may be a list of pairs, so a name can repeat (several Set-Cookie).
    return "welcome", 200, [
        httpp.set_cookie("session", "abc123", max_age=3600, same_site="lax"),
        httpp.set_cookie("theme", "dark", http_only=False),
    ]


port = app.bind_to_any_port("127.0.0.1")
thread = threading.Thread(target=app.listen_after_bind, daemon=True)
thread.start()
base = f"http://127.0.0.1:{port}"

res = httpp.get(base + "/countdown")
print(repr(res.body), "| chunked:", res.header("Transfer-Encoding") == "chunked")
print(repr(httpp.get(base + "/events").body))
res = httpp.get(base + "/report")
print(res.body, "|", res.header("Content-Type"), "| length:", res.header("Content-Length"))
print(httpp.get(base + "/old", follow_redirects=True).body)
res = httpp.get(base + "/login", cookies={"seen": "1"})
for name, value in res.headers:
    if name.lower() == "set-cookie":
        print("Set-Cookie:", value)

app.stop()
thread.join()
tmp.cleanup()
