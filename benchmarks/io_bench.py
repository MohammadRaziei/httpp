"""I/O-bound comparison: what `async def` is for. Usage: python io_bench.py <oha> [seconds]. Compare ratios inside one run only."""
import json
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OHA = sys.argv[1]
SECONDS = sys.argv[2] if len(sys.argv) > 2 else "5"
CASES = [  # (label, server, path)
    ("uvicorn async", "uvicorn", "/"),
    ("httpp def (blocking sleep)", "httpp", "/"),
    ("httpp async def", "httpp", "/async"),
]


def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def serve(kind, p):
    env = dict(os.environ, PYTHONPATH=HERE)
    cmd = ([sys.executable, "-m", "uvicorn", "io_app:asgi", "--port", str(p), "--log-level", "error", "--no-access-log"]
           if kind == "uvicorn" else [sys.executable, "-m", "httpp", "run", "io_app:app", "--port", str(p), "--log-level", "error"])
    proc = subprocess.Popen(cmd, env=env, cwd=HERE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", p), 0.1).close()
            return proc
        except OSError:
            time.sleep(0.1)
    proc.kill()
    raise RuntimeError(kind + " did not start")


def run(url, conc):
    out = subprocess.run([OHA, "-z", f"{SECONDS}s", "-c", str(conc), "--no-tui", "--output-format", "json", url], capture_output=True, check=True)
    d = json.loads(out.stdout)
    codes = d["statusCodeDistribution"]
    bad = sum(v for k, v in codes.items() if k != "200")
    return d["summary"]["requestsPerSec"], d["latencyPercentiles"]["p99"] * 1000, bad


print(f"{'':28}" + "".join(f"{c:>22}" for c in (32, 128, 512)) + "   (req/s, p99 ms, non-200)")
for label, kind, path in CASES:
    p = port()
    proc = serve(kind, p)
    try:
        run(f"http://127.0.0.1:{p}{path}", 32)  # warm-up
        cells = []
        for c in (32, 128, 512):
            r, p99, bad = run(f"http://127.0.0.1:{p}{path}", c)
            cells.append(f"{r:>9.0f} {p99:>6.0f} {bad:>4}")
        print(f"{label:28}" + "".join(f"{x:>22}" for x in cells))
    finally:
        proc.kill()
        proc.wait()
