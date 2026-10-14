#!/usr/bin/env python3
"""Requests per second for httpp (`def` and `async def`) against uvicorn, measured with `oha`.

    python benchmarks/bench.py --oha /path/to/oha [--seconds 5] [--rounds 3] [--conc 32]
                               [--workers 1] [--only hello,item] [--targets httpp,httpp-async,uvicorn]

Server and load generator share this machine, so the numbers are only comparable with each other
(same box, same run). On one core they also fight for the CPU. Say so whenever you quote them.
"""
import argparse
import json
import os
import socket
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = {  # name -> (path for def / uvicorn, path for async def)
    "hello": ("/", "/async"),
    "item": ("/item/42", "/async/item/42"),
    "agent": ("/agent", "/async/agent"),
    "many": ("/many/r299/x", "/async/many/r299/x"),  # the last of 300 routes (uvicorn's bare app has no router)
}


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start(target, port, workers):
    inherited = [os.path.abspath(p) for p in os.environ.get("PYTHONPATH", "").split(os.pathsep) if p]
    env = dict(os.environ, PYTHONPATH=os.pathsep.join([HERE] + inherited))  # absolute: the child runs in HERE
    if target == "uvicorn":
        cmd = [sys.executable, "-m", "uvicorn", "asgi_app:app", "--port", str(port), "--log-level", "error",
               "--no-access-log", "--loop", "uvloop", "--http", "httptools", "--workers", str(workers)]
    else:
        cmd = [sys.executable, "-m", "httpp", "run", "httpp_app:app", "--port", str(port), "--log-level", "error",
               "--workers", str(workers)]
    proc = subprocess.Popen(cmd, env=env, cwd=HERE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(100):
        if proc.poll() is not None:
            raise RuntimeError(f"{target} exited: {proc.stderr.read().decode()[-500:]}")
        try:
            socket.create_connection(("127.0.0.1", port), 0.1).close()
            return proc
        except OSError:
            time.sleep(0.1)
    proc.kill()
    raise RuntimeError(f"{target} did not start")


def load(oha, port, path, seconds, conc):
    out = subprocess.run([oha, "-z", f"{seconds}s", "-c", str(conc), "--no-tui", "--output-format", "json",
                          f"http://127.0.0.1:{port}{path}"], capture_output=True, check=True)
    data = json.loads(out.stdout)
    ok = data["statusCodeDistribution"].get("200", 0)
    total = sum(data["statusCodeDistribution"].values())
    if ok != total:
        raise RuntimeError(f"{path}: only {ok} of {total} answers were 200")
    return data["summary"]["requestsPerSec"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--oha", default="oha")
    ap.add_argument("--seconds", type=int, default=5)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--conc", type=int, default=32)
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--only", default=",".join(SCENARIOS))
    ap.add_argument("--targets", default="uvicorn,httpp,httpp-async")
    args = ap.parse_args()

    rows = {}
    for target in args.targets.split(","):
        port = free_port()
        proc = start("uvicorn" if target == "uvicorn" else "httpp", port, args.workers)
        try:
            for name in args.only.split(","):
                path = SCENARIOS[name][1 if target == "httpp-async" else 0]
                load(args.oha, port, path, 1, args.conc)  # warm-up
                runs = [load(args.oha, port, path, args.seconds, args.conc) for _ in range(args.rounds)]
                rows.setdefault(name, {})[target] = (statistics.median(runs), min(runs), max(runs))
        finally:
            proc.terminate()
            proc.wait(10)
    targets = args.targets.split(",")
    print(f"\n{os.cpu_count()} CPU(s), workers={args.workers}, conc={args.conc}, {args.seconds}s x {args.rounds} (median [min-max] req/s)")
    print(f"{'scenario':<10}" + "".join(f"{t:>26}" for t in targets))
    for name, byt in rows.items():
        print(f"{name:<10}" + "".join(f"{byt[t][0]:>11,.0f} [{byt[t][1]:,.0f}-{byt[t][2]:,.0f}]".rjust(26) for t in targets))


if __name__ == "__main__":
    main()
