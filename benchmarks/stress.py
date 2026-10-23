"""Mixed stress test: sync, async, streaming, failing and slow routes hit by many threads, some clients hang up early.
Usage: python stress.py [seconds] [threads]; STRESS_KINDS=s,a,... limits the kinds of request. Exit code 1 on any wrong answer or any traceback on the server's stderr."""
import asyncio
import http.client
import random
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = __import__("os").path.dirname(__import__("os").path.abspath(__file__))
SERVER = r'''
import asyncio, httpp
app = httpp.Server(handler_timeout=2)

@app.get("/s/{n}")
def s(n: int):
    return {"n": n}

@app.get("/a/{n}")
async def a(n: int):
    return {"n": n}

@app.get("/aw/{n}")
async def aw(n: int):
    await asyncio.sleep(0.002)
    return str(n)

@app.get("/stream/{n}")
def stream(n: int):
    for i in range(3):
        yield f"{n}:{i};".encode()

@app.get("/boom")
async def boom():
    raise RuntimeError("expected by the stress test")

@app.get("/sboom")
def sboom():
    raise RuntimeError("expected by the stress test")

@app.get("/slow")
async def slow():
    await asyncio.sleep(5)
    return "late"

@app.post("/echo")
async def echo(request):
    return request.body

if __name__ == "__main__":
    app.run("127.0.0.1", PORT)
'''


KINDS = (__import__("os").environ.get("STRESS_KINDS") or "s,a,aw,stream,boom,sboom,echo,hangup,slowhangup").split(",")


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 20
    nthreads = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    code = SERVER.replace("PORT", str(port))
    # stderr goes to a file: with a pipe nobody reads, the server blocks on its own log once the 64 KB buffer is full
    errfile = tempfile.TemporaryFile()
    proc = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.DEVNULL, stderr=errfile)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), 0.1).close()
            break
        except OSError:
            time.sleep(0.1)
    errors, counts = [], {"ok": 0, "hangup": 0}
    lock = threading.Lock()
    stop = time.time() + seconds

    def worker(seed):
        rnd = random.Random(seed)
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        while time.time() < stop:
            kind = rnd.choice(KINDS)
            n = rnd.randrange(10**6)
            try:
                if kind == "hangup" or kind == "slowhangup":
                    k = socket.create_connection(("127.0.0.1", port), 5)
                    path = "/slow" if kind == "slowhangup" else f"/aw/{n}"
                    k.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
                    k.close()  # gone before the answer
                    with lock:
                        counts["hangup"] += 1
                    continue
                if kind == "echo":
                    body = bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 3000)))
                    conn.request("POST", "/echo", body=body)
                    r = conn.getresponse()
                    got = r.read()
                    ok = r.status == 200 and got == body
                else:
                    conn.request("GET", {"boom": "/boom", "sboom": "/sboom"}.get(kind, f"/{kind}/{n}"))
                    r = conn.getresponse()
                    got = r.read()
                    ok = {"s": r.status == 200 and got == b'{"n": %d}' % n,
                          "a": r.status == 200 and got == b'{"n": %d}' % n,
                          "aw": r.status == 200 and got == str(n).encode(),
                          "stream": r.status == 200 and got == b"".join(b"%d:%d;" % (n, i) for i in range(3)),
                          "boom": r.status == 500, "sboom": r.status == 500}[kind]
                if not ok:
                    with lock:
                        errors.append((kind, n, r.status, got[:60]))
                else:
                    with lock:
                        counts["ok"] += 1
            except Exception as exc:  # a dropped keep-alive connection is reopened
                conn.close()
                conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                with lock:
                    errors.append((kind, n, "exception", repr(exc)[:80]))

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(nthreads)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    alive = proc.poll() is None
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    errfile.seek(0)
    err = errfile.read().decode(errors="replace")
    # the routes that fail on purpose print their own traceback: only unexpected ones count
    lines = [l for l in err.splitlines() if l.strip()]
    unexpected = [l for l in lines if ("Error" in l or "Exception" in l) and "expected by the stress test" not in l
                  and not l.startswith(("Traceback", " "))]
    print(f"python {sys.version.split()[0]} gil={getattr(sys, '_is_gil_enabled', lambda: True)()} "
          f"ok={counts['ok']} hangups={counts['hangup']} wrong={len(errors)} server_alive={alive} unexpected_stderr={len(unexpected)}")
    for e in errors[:10]:
        print("  wrong:", e)
    for l in unexpected[:10]:
        print("  stderr:", l)
    sys.exit(1 if errors or not alive or unexpected else 0)


if __name__ == "__main__":
    main()
