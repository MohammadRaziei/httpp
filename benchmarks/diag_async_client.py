"""Where does an AsyncClient run spend its CPU? Linux only (reads /proc/self/task).

    python diag_async_client.py --port 8000 --path / --conc 1,8,32,128

Start a server first (for example the C++ benchmark target or any HTTP/1.1 keep-alive server). For every concurrency level it prints
requests per second, latency, and the CPU time per request of each thread of this process: "loop" is the thread running asyncio
(the Python side), the others are C++ threads (the network engine). The thread that is close to 100% of one core is the bottleneck.
Run it once on the GIL build and once on the free-threaded build and compare the two tables.
"""
import argparse
import asyncio
import json
import os
import sys
import time


def thread_cpu():
    """tid -> CPU seconds (user + system) of every thread of this process."""
    hz = os.sysconf("SC_CLK_TCK")
    out = {}
    for tid in os.listdir("/proc/self/task"):
        try:
            with open(f"/proc/self/task/{tid}/stat") as f:
                text = f.read()
        except OSError:
            continue  # the thread ended
        rest = text[text.rindex(")") + 2:].split()
        out[int(tid)] = (int(rest[11]) + int(rest[12])) / hz
    return out


async def run(lib, base, path, conc, seconds, warmup):
    lat = []
    stop = time.perf_counter() + warmup + seconds
    start = time.perf_counter() + warmup
    snap = {}

    async def worker(call):
        while time.perf_counter() < stop:
            t0 = time.perf_counter()
            ok = await call()
            if t0 >= start:
                if not snap:
                    snap["cpu"] = thread_cpu()
                    snap["t"] = time.perf_counter()
                lat.append((time.perf_counter() - t0, ok))

    if lib == "httpp":
        import httpp
        async with httpp.AsyncClient(base, max_connections=conc) as c:
            async def call():
                return (await c.get(path)).status == 200
            await asyncio.gather(*(worker(call) for _ in range(conc)))
    else:
        import aiohttp
        async with aiohttp.ClientSession(connector=aiohttp.TCPConnector(limit=conc)) as s:
            async def call():
                async with s.get(base + path) as r:
                    await r.read()
                    return r.status == 200
            await asyncio.gather(*(worker(call) for _ in range(conc)))
    return lat, snap


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--path", default="/")
    ap.add_argument("--conc", default="1,8,32,128", help="comma separated concurrency levels")
    ap.add_argument("--seconds", type=float, default=4)
    ap.add_argument("--warmup", type=float, default=1)
    ap.add_argument("--lib", choices=["httpp", "aiohttp"], default="httpp")
    args = ap.parse_args()
    base = f"http://127.0.0.1:{args.port}"
    gil = getattr(sys, "_is_gil_enabled", lambda: True)()
    print(f"python {sys.version.split()[0]}  gil={'on' if gil else 'off'}  lib={args.lib}  cpus={len(os.sched_getaffinity(0))}")
    print(f"{'conc':>5} {'req/s':>9} {'p50 ms':>8} {'p99 ms':>8}   cpu us/req per thread (loop first)")
    rows = []
    for conc in (int(x) for x in args.conc.split(",")):
        lat, snap = asyncio.run(run(args.lib, base, args.path, conc, args.seconds, args.warmup))
        if not lat:
            print(f"{conc:>5}  no requests finished")
            continue
        after = thread_cpu()
        elapsed = time.perf_counter() - snap["t"]
        n = len(lat)
        per = {}
        for tid, cpu in after.items():
            used = cpu - snap["cpu"].get(tid, 0.0)
            if used > 0.005:
                per["loop" if tid == os.getpid() else f"t{tid}"] = used * 1e6 / n
        times = sorted(t for t, _ in lat)
        bad = sum(1 for _, ok in lat if not ok)
        row = {"conc": conc, "rps": n / elapsed, "p50_ms": times[n // 2] * 1e3, "p99_ms": times[min(n - 1, int(n * 0.99))] * 1e3,
               "errors": bad, "cpu_us_per_req": {k: round(v, 1) for k, v in sorted(per.items(), key=lambda kv: kv[0] != "loop")}}
        rows.append(row)
        cpu_text = "  ".join(f"{k}={v:.0f}" for k, v in row["cpu_us_per_req"].items())
        print(f"{conc:>5} {row['rps']:>9.0f} {row['p50_ms']:>8.2f} {row['p99_ms']:>8.2f}   {cpu_text}" + (f"   ERRORS={bad}" if bad else ""))
    print(json.dumps({"python": sys.version.split()[0], "gil": gil, "lib": args.lib, "rows": rows}))


if __name__ == "__main__":
    main()
