"""One HTTP client library, one load run, one JSON line on stdout. Started by suite.py (the `client` part); it can be run by hand too.

    python client_worker.py --lib aiohttp --port 8000 --path / --seconds 5 --conc 32

Every library makes GET requests in a loop against one server for --warmup seconds (not counted) and then --seconds. Sync libraries get
--conc threads with a session each, async libraries --conc tasks on one session. The numbers: requests per second, latency
percentiles of the calls, CPU time of this whole process per request, and its peak RSS."""
import argparse
import asyncio
import json
import resource
import threading
import time

LIBS = {  # name -> (module that must import, sync or async)
    "httpp Client": ("httpp", "sync"),
    "requests": ("requests", "sync"),
    "httpx": ("httpx", "sync"),
    "httpp AsyncClient": ("httpp", "async"),
    "aiohttp": ("aiohttp", "async"),
    "httpx AsyncClient": ("httpx", "async"),
}


class Clock:
    """Counts only what finishes inside the measuring window."""

    def __init__(self, warmup, seconds):
        now = time.perf_counter()
        self.start, self.stop = now + warmup, now + warmup + seconds
        self.cpu0 = None

    def running(self):
        return time.perf_counter() < self.stop

    def inside(self, t0):
        """True when a call that began at t0 counts. The first one also takes the CPU time of the process: a sampler thread of its own
        can be starved for seconds by busy worker threads, the workers cannot."""
        if t0 < self.start:
            return False
        if self.cpu0 is None:
            self.cpu0 = time.process_time()
        return True


def sync_worker(lib, url, base, path, clock, out):
    lat, bad = [], 0
    if lib == "httpp Client":
        import httpp
        c = httpp.Client(base)
        call = lambda: c.get(path).status == 200  # noqa: E731
    elif lib == "requests":
        import requests
        s = requests.Session()
        call = lambda: s.get(url).status_code == 200  # noqa: E731
    else:
        import httpx
        c = httpx.Client()
        call = lambda: c.get(url).status_code == 200  # noqa: E731
    while clock.running():
        t0 = time.perf_counter()
        try:
            ok = call()
        except Exception:  # noqa: BLE001
            ok = False
        if clock.inside(t0):
            lat.append(time.perf_counter() - t0)
            bad += not ok
    out.append((lat, bad))


async def async_main(lib, url, base, path, clock, conc, out):
    lat, bad = [], 0

    async def loop(call):
        nonlocal bad
        while clock.running():
            t0 = time.perf_counter()
            try:
                ok = await call()
            except Exception:  # noqa: BLE001
                ok = False
            if clock.inside(t0):
                lat.append(time.perf_counter() - t0)
                bad += not ok

    if lib == "httpp AsyncClient":
        import httpp
        async with httpp.AsyncClient(base, max_connections=conc) as c:
            async def call():
                return (await c.get(path)).status == 200
            await asyncio.gather(*(loop(call) for _ in range(conc)))
    elif lib == "aiohttp":
        import aiohttp
        async with aiohttp.ClientSession(connector=aiohttp.TCPConnector(limit=conc)) as s:
            async def call():
                async with s.get(url) as r:
                    await r.read()
                    return r.status == 200
            await asyncio.gather(*(loop(call) for _ in range(conc)))
    else:
        import httpx
        async with httpx.AsyncClient(limits=httpx.Limits(max_connections=conc, max_keepalive_connections=conc)) as c:
            async def call():
                return (await c.get(url)).status_code == 200
            await asyncio.gather(*(loop(call) for _ in range(conc)))
    out.append((lat, bad))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lib", required=True, choices=sorted(LIBS))
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--path", default="/")
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--warmup", type=float, default=1)
    ap.add_argument("--conc", type=int, default=32)
    args = ap.parse_args()
    base = f"http://127.0.0.1:{args.port}"
    url = base + args.path
    out = []
    __import__(LIBS[args.lib][0])  # the import is not part of the warmup
    clock = Clock(args.warmup, args.seconds)
    if LIBS[args.lib][1] == "sync":
        threads = [threading.Thread(target=sync_worker, args=(args.lib, url, base, args.path, clock, out)) for _ in range(args.conc)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    else:
        asyncio.run(async_main(args.lib, url, base, args.path, clock, args.conc, out))
    cpu = time.process_time() - (clock.cpu0 if clock.cpu0 is not None else 0.0)
    lat = sorted(x for part, _ in out for x in part)
    bad = sum(b for _, b in out)
    n = len(lat)

    def pct(p):
        return lat[min(n - 1, int(n * p))] * 1000 if n else 0.0
    print(json.dumps({"lib": args.lib, "requests": n, "errors": bad, "rps": n / args.seconds, "p50_ms": pct(0.50), "p99_ms": pct(0.99),
                      "p999_ms": pct(0.999), "cpu_us_per_req": cpu * 1e6 / max(n, 1), "peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}))


if __name__ == "__main__":
    main()
