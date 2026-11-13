"""Why does httpp.Client scale badly with threads on a free-threaded build? Linux only (reads /proc/self/task).

    python diag_sync_client.py --port 8000 --path / --threads 1,2,4,8,16,32
    python diag_sync_client.py --port 8000 --threads 32 --no-gc        # is the garbage collector the cause?

Every thread has a Client of its own and makes GET requests in a loop. For each thread count it prints requests per second, latency, and
the CPU time per request of the Python worker threads and of the other (C++) threads, which are the network engine. Run it on the GIL
build and on the free-threaded build and compare the tables; pin the process (taskset) the same way for both.
"""
import argparse
import gc
import json
import os
import sys
import threading
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


def run(threads, base, path, seconds, warmup):
    import httpp
    start = time.perf_counter() + warmup + 0.3
    stop = start + seconds
    workers = set()
    results = []
    ready = threading.Barrier(threads + 1)

    def worker():
        workers.add(threading.get_native_id())
        client = httpp.Client(base)
        lat, bad = [], 0
        ready.wait()
        while True:
            t0 = time.perf_counter()
            if t0 >= stop:
                break
            ok = client.get(path).status == 200
            if t0 >= start:
                lat.append(time.perf_counter() - t0)
                bad += not ok
        results.append((lat, bad))

    pool = [threading.Thread(target=worker) for _ in range(threads)]
    for t in pool:
        t.start()
    ready.wait()
    time.sleep(max(0.0, start - time.perf_counter()))
    before, t_before = thread_cpu(), time.perf_counter()
    time.sleep(max(0.0, stop - time.perf_counter()))
    after, t_after = thread_cpu(), time.perf_counter()
    for t in pool:
        t.join()
    lat = sorted(x for part, _ in results for x in part)
    bad = sum(b for _, b in results)
    n = len(lat)
    if not n:
        return None
    me = os.getpid()
    py_cpu = eng_cpu = 0.0
    for tid, cpu in after.items():
        used = cpu - before.get(tid, 0.0)
        if tid in workers:
            py_cpu += used
        elif tid != me:
            eng_cpu += used
    return {"threads": threads, "rps": n / (t_after - t_before), "p50_ms": lat[n // 2] * 1e3, "p99_ms": lat[min(n - 1, int(n * 0.99))] * 1e3,
            "errors": bad, "py_us_per_req": py_cpu * 1e6 / n, "engine_us_per_req": eng_cpu * 1e6 / n}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--path", default="/")
    ap.add_argument("--threads", default="1,2,4,8,16,32", help="comma separated thread counts")
    ap.add_argument("--seconds", type=float, default=4)
    ap.add_argument("--warmup", type=float, default=1)
    ap.add_argument("--no-gc", action="store_true", help="gc.disable() before the runs")
    args = ap.parse_args()
    if args.no_gc:
        gc.disable()
    gil = getattr(sys, "_is_gil_enabled", lambda: True)()
    print(f"python {sys.version.split()[0]}  gil={'on' if gil else 'off'}  gc={'off' if args.no_gc else 'on'}  cpus={len(os.sched_getaffinity(0))}")
    print(f"{'thr':>4} {'req/s':>9} {'p50 ms':>8} {'p99 ms':>8} {'py us/req':>10} {'engine us/req':>14}")
    rows = []
    for n in (int(x) for x in args.threads.split(",")):
        row = run(n, f"http://127.0.0.1:{args.port}", args.path, args.seconds, args.warmup)
        if row is None:
            print(f"{n:>4}  no requests finished")
            continue
        rows.append(row)
        print(f"{n:>4} {row['rps']:>9.0f} {row['p50_ms']:>8.2f} {row['p99_ms']:>8.2f} {row['py_us_per_req']:>10.1f} {row['engine_us_per_req']:>14.1f}"
              + (f"   ERRORS={row['errors']}" if row["errors"] else ""))
    print(json.dumps({"python": sys.version.split()[0], "gil": gil, "gc": not args.no_gc, "rows": rows}))


if __name__ == "__main__":
    main()
