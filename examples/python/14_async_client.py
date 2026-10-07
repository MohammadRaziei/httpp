"""AsyncClient, awaitable downloads and the server's handler timeout.

Runs offline: it serves itself, calls itself, and exits.

Be honest about what "async" means here: the client keeps a pool of keep-alive connections and
runs each blocking call on the event loop's thread pool. The loop is never blocked, but this is
not non-blocking socket I/O, so think tens of concurrent requests, not thousands.

    python 14_async_client.py
"""
import asyncio
import tempfile
import threading
from pathlib import Path

import httpp
from httpp import AsyncClient, Download, Server

app = Server(handler_timeout=0.5)  # an async handler gets half a second


@app.get("/square/{n}")
async def square(n: int):
    await asyncio.sleep(0.05)
    return {"n": n, "square": n * n}


@app.get("/slow")
async def slow():
    await asyncio.sleep(30)  # cancelled after 0.5 s; the client gets a 504
    return "never sent"


tmp = tempfile.TemporaryDirectory()
Path(tmp.name, "data.bin").write_bytes(b"x" * 1_000_000)
app.serve_directory("/files", tmp.name)

port = app.bind_to_any_port("127.0.0.1")
thread = threading.Thread(target=app.listen_after_bind, daemon=True)
thread.start()
base = f"http://127.0.0.1:{port}"


async def main():
    # 20 requests at once, over at most 8 connections: ~3 rounds of 0.05 s, not 20.
    async with AsyncClient(base, max_connections=8, timeout=5) as client:
        replies = await asyncio.gather(*(client.get(f"/square/{i}") for i in range(20)))
        print("squares:", [r.json()["square"] for r in replies][:6], "...")

        slow_reply = await client.get("/slow")
        print("slow handler ->", slow_reply.status, slow_reply.body)

    # Downloads can be awaited too; resume() skips a file that is already complete.
    outs = [Path(tmp.name, f"copy{i}.bin") for i in range(2)]
    results = await asyncio.gather(
        *(Download(f"{base}/files/data.bin", str(o)).disable_progress().run_async() for o in outs))
    print("downloads ok:", [r.ok for r in results], [o.stat().st_size for o in outs])
    again = await Download(f"{base}/files/data.bin", str(outs[0])).disable_progress().resume().run_async()
    print("second run skipped:", again.skipped)


asyncio.run(main())
app.stop()
thread.join()
tmp.cleanup()
