#!/usr/bin/env python3
"""The benchmark suite: uvicorn, httpp (def / async def / await) on several Pythons and I/O thread counts, and
the C++ API, all measured the same way. One command (`make bench`), one output file to send back.

Method: variants run interleaved (round 1 of every variant, then round 2, ...), so a slow minute of the machine
hits all of them alike. The server and the load generator (`oha`) are pinned to separate CPUs. Server CPU time per
request comes from /proc (all threads). Numbers are only comparable with each other, inside one output file.

    python suite.py --pys 3.13=.venvs/py313/bin/python,3.14t=.venvs/py314t/bin/python --cpp bin/cpp_bench
"""
import argparse
import hashlib
import json
import os
import platform
import shutil
import socket
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def cpu_ranges(cpus):
    """[0,1,2,3,8] -> '0-3,8' (taskset syntax)."""
    cpus, out = sorted(cpus), []
    i = 0
    while i < len(cpus):
        j = i
        while j + 1 < len(cpus) and cpus[j + 1] == cpus[j] + 1:
            j += 1
        out.append(str(cpus[i]) if i == j else f"{cpus[i]}-{cpus[j]}")
        i = j + 1
    return ",".join(out)


def parse_cpus(text):
    out = []
    for part in text.split(","):
        if "-" in part:
            a, b = part.split("-")
            out += range(int(a), int(b) + 1)
        elif part:
            out.append(int(part))
    return out


def default_split():
    """(server cpus, load cpus), split on whole cores. The server gets about two thirds of the performance cores (the ones
    with a hyper-thread sibling), the load generator the rest and every efficiency core, because it is the load
    generator that runs out first at hundreds of thousands of requests per second. A core is never shared: a split in the
    middle of a hyper-thread pair (server 0-8, load 9-11) makes the two fight for one core."""
    allowed = sorted(os.sched_getaffinity(0))
    cores = {}
    for c in allowed:
        sib = read(f"/sys/devices/system/cpu/cpu{c}/topology/thread_siblings_list")
        if "," in sib or "-" in sib:
            cores.setdefault(sib, []).append(c)
    ordered = sorted(cores.values())
    if len(ordered) < 2:
        pool = [c for g in ordered for c in g] if ordered else allowed
        if len(pool) < 3:
            return pool, pool  # nothing to separate
        k = max(1, len(pool) // 4)
        return pool[:-k], pool[-k:] + [c for c in allowed if c not in pool]
    k = max(1, len(ordered) // 3)
    server = [c for g in ordered[:-k] for c in g]
    return server, [c for c in allowed if c not in server]


def tree_id():
    """A short hash of the sources under test, so a result file says which code produced it."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    h = hashlib.sha1()
    for sub in ("include", "src"):
        for d, _, files in sorted(os.walk(os.path.join(root, sub))):
            for f in sorted(files):
                if f.endswith((".hpp", ".cpp", ".h", ".c", ".py", ".pyx", ".pxd")):
                    with open(os.path.join(d, f), "rb") as fh:
                        h.update(f.encode() + fh.read())
    return h.hexdigest()[:10]


def sysinfo(args, server_cpus, load_cpus):
    lines = [f"date            {time.strftime('%Y-%m-%d %H:%M:%S %z')}",
             f"kernel          {platform.release()}",
             f"cpu             {next((l.split(':', 1)[1].strip() for l in read('/proc/cpuinfo').splitlines() if l.startswith('model name')), '?')}",
             f"logical cpus    {os.cpu_count()}",
             f"server cpus     {cpu_ranges(server_cpus)}   load generator cpus  {cpu_ranges(load_cpus)}"]
    gov = {read(f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_governor") for c in server_cpus}
    lines.append(f"governor        {', '.join(sorted(g for g in gov if g)) or '?'}")
    lines.append(f"turbo disabled  {read('/sys/devices/system/cpu/intel_pstate/no_turbo', '?')}")
    on_battery = any(read(p) == "0" for p in
                     [f"/sys/class/power_supply/{d}/online" for d in os.listdir("/sys/class/power_supply")]
                     if os.path.basename(os.path.dirname(p)).startswith(("AC", "ADP")))
    lines.append(f"on battery      {'YES (results will be low and noisy)' if on_battery else 'no / unknown'}")
    lines.append(f"tree            {tree_id()}")
    lines.append(f"settings        conc={args.conc} seconds={args.seconds} rounds={args.rounds} warmup=1s")
    for label, py in args.pys:
        info = subprocess.run([py, "-c", "import sys,httpp;g=getattr(sys,'_is_gil_enabled',lambda:True)();"
                               "print(sys.version.split()[0],'GIL' if g else 'no-GIL (free-threaded)','httpp',httpp.__version__)"],
                              capture_output=True, text=True).stdout.strip()
        lines.append(f"python {label:<7} {info}")
        libs = subprocess.run([py, "-c", "from importlib.metadata import version as v\n"
                               "for n in ('uvicorn','uvloop','httptools','fastapi','starlette'):\n"
                               "    try: print(n, v(n), end='  ')\n"
                               "    except Exception: print(n, '-', end='  ')"], capture_output=True, text=True).stdout.strip()
        lines.append(f"       {'':<7} {libs}")
    cxx = shutil.which(os.environ.get("CXX", "g++"))
    if cxx:
        lines.append(f"c++ compiler    {subprocess.run([cxx, '--version'], capture_output=True, text=True).stdout.splitlines()[0]}  (library: CMake Release, bench: -O2)")
    lines.append("uvicorn         one process, --no-access-log, uvloop when installed, httptools; the app is a bare ASGI function")
    lines.append("hello           a constant answer; json: a path parameter turned into an int and a JSON object back;")
    lines.append("cpu             sum of i*i for i<2000 in Python (about 60 us); the C++ server does the same sum in C++")
    lines.append("rate            closed-loop runs measure queueing at saturation; the 'rate' tables hold latency at 50 percent of each maximum")
    return "\n".join(lines)


class Variant:
    def __init__(self, group, name, path, cmd, env=None, conc=None, rate=False):
        self.group, self.name, self.path, self.cmd, self.env, self.conc, self.rate = group, name, path, cmd, env or {}, conc, rate
        self.runs = []  # (req/s, us/req, p50 ms, p99 ms, errors)


def _gil_free(py):
    out = subprocess.run([py, "-c", "import sys;print(not getattr(sys,'_is_gil_enabled',lambda:True)())"], capture_output=True, text=True).stdout
    return out.strip() == "True"


def build_variants(args):
    v = []
    for label, py in args.pys:
        uv = [py, "-m", "uvicorn", "suite_asgi:app", "--log-level", "error", "--no-access-log"]
        try:
            import_ok = subprocess.run([py, "-c", "import uvloop"], capture_output=True).returncode == 0
        except OSError:
            import_ok = False
        if import_ok:
            uv += ["--loop", "uvloop"]
        for path, what in (("/", "hello"), ("/cpu", "cpu"), ("/item/42", "json")):
            if what in args.only:
                v.append(Variant(f"{what} (py {label})", "uvicorn bare ASGI", path, uv + ["--port", "{port}"], rate=(what == "hello")))
        if subprocess.run([py, "-c", "import fastapi"], capture_output=True).returncode == 0:
            fa = [py, "-m", "uvicorn", "fastapi_app:app", "--log-level", "error", "--no-access-log", "--port", "{port}"] + (["--loop", "uvloop"] if import_ok else [])
            for path, what in (("/", "hello"), ("/item/42", "json")):
                if what in args.only:
                    v.append(Variant(f"{what} (py {label})", "FastAPI + uvicorn", path, fa))
        for n in args.io_threads:
            ht = lambda: [py, "-m", "httpp", "run", "suite_app:app", "--port", "{port}", "--log-level", "error", "--io-threads", str(n)]
            for path, name, what in (("/sync", "httpp def", "hello"), ("/async", "httpp async def", "hello"),
                                     ("/await", "httpp async + await", "hello"), ("/item/42", "httpp def JSON", "json"),
                                     ("/aitem/42", "httpp async def JSON", "json"), ("/cpu", "httpp def CPU-bound", "cpu"),
                                     ("/acpu", "httpp async def CPU-bound", "cpu")):
                if what in args.only:
                    v.append(Variant(f"{what} (py {label})", f"{name}, io_threads={n}", path, ht(),
                                     rate=(what == "hello" and name in ("httpp def", "httpp async def") and n in (1, args.io_threads[-1]))))
            if "cpu" in args.only and n in (args.io_threads[0], 4):
                # The size of the worker pool matters for CPU-bound handlers: under a GIL the smallest is best, without
                # one the question is how fast the default pool reaches the size the machine can use (ADR-0025, addendum 4).
                for t in ((1, 4, 16) if not _gil_free(py) else (2, 4, 8)):
                    v.append(Variant(f"cpu (py {label})", f"httpp def CPU-bound, pool={t}, io_threads={n}", "/cpu", ht(), {"BENCH_THREADS": str(t)}))
            if "hello" in args.only:
                v.append(Variant(f"hello (py {label})", f"httpp async def INLINE, io_threads={n}", "/async", ht(), {"BENCH_INLINE": "1"}))
    if "latency" in args.only and args.pys:
        label, py = args.pys[0]
        ht = lambda: [py, "-m", "httpp", "run", "suite_app:app", "--port", "{port}", "--log-level", "error", "--io-threads", "1"]
        g = f"latency, 1 connection (py {label}, io_threads=1)"
        v.append(Variant(g, "httpp def", "/sync", ht(), conc=1))
        v.append(Variant(g, "httpp async def", "/async", ht(), conc=1))
        v.append(Variant(g, "httpp async def INLINE", "/async", ht(), {"BENCH_INLINE": "1"}, conc=1))
        v.append(Variant(g, "httpp async + await", "/await", ht(), conc=1))
        if args.cpp and os.path.exists(args.cpp):
            v.append(Variant(g, "C++ route_io", "/io", [args.cpp, "{port}", "1"], conc=1))
    if args.cpp and os.path.exists(args.cpp):
        for n in args.io_threads + [x for x in args.cpp_extra if x not in args.io_threads]:
            for path, name, what in (("/sync", "C++ route (worker pool)", "hello"), ("/io", "C++ route_io (inline)", "hello"),
                                     ("/cpu", "C++ route CPU-bound", "cpu")):
                if what in args.only:
                    v.append(Variant(f"{what} (C++)", f"{name}, io_threads={n}", path, [args.cpp, "{port}", str(n)],
                                     rate=(what == "hello" and "route_io" in name and n in (1, args.io_threads[-1]))))
    return v


def ticks(pid):
    parts = read(f"/proc/{pid}/stat").rsplit(")", 1)[1].split()
    return int(parts[11]) + int(parts[12])


def oha(args, load_cpus, url, seconds, conc=None, rate=None):
    cmd = ["taskset", "-c", cpu_ranges(load_cpus)] if shutil.which("taskset") else []
    cmd += [args.oha] + (["-q", str(int(rate))] if rate else []) + ["-z", f"{seconds}s", "-c", str(conc or args.conc), "--no-tui", "--output-format", "json", url]
    return json.loads(subprocess.run(cmd, capture_output=True, check=True).stdout)


def measure(var, args, server_cpus, load_cpus, rate=None):
    port = free_port()
    cmd = [c.replace("{port}", str(port)) for c in var.cmd]
    if shutil.which("taskset"):
        cmd = ["taskset", "-c", cpu_ranges(server_cpus)] + cmd
    env = dict(os.environ, PYTHONPATH=HERE, **var.env)
    proc = subprocess.Popen(cmd, cwd=HERE, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        for _ in range(150):
            if proc.poll() is not None:
                raise RuntimeError(proc.stderr.read().decode()[-600:])
            try:
                socket.create_connection(("127.0.0.1", port), 0.1).close()
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError("did not start")
        url = f"http://127.0.0.1:{port}{var.path}"
        oha(args, load_cpus, url, 1, var.conc)
        t0 = ticks(proc.pid)
        data = oha(args, load_cpus, url, args.seconds, var.conc, rate)
        t1 = ticks(proc.pid)
        dist = data["statusCodeDistribution"]
        ok = dist.get("200", 0)
        # oha aborts the requests still in flight when the time is up ("aborted due to deadline"): not an error of the server
        bad = sum(dist.values()) - ok + sum(n for k, n in data.get("errorDistribution", {}).items() if "deadline" not in k)
        pct = data.get("latencyPercentiles", {})
        if bad:  # say what happened: a dead server shows up as hundreds of thousands of refused connections
            rc = proc.poll()
            print(f"  ! {var.name}: {bad} errors, server {'is alive' if rc is None else f'exited with code {rc}'}; {data.get('errorDistribution', {})}",
                  file=sys.stderr, flush=True)
            if rc is not None:
                print(proc.stderr.read().decode(errors="replace")[-1500:], file=sys.stderr, flush=True)
        return (ok / args.seconds, (t1 - t0) / os.sysconf("SC_CLK_TCK") * 1e6 / max(ok, 1),
                (pct.get("p50") or 0) * 1000, (pct.get("p99") or 0) * 1000, bad, (pct.get("p99.9") or 0) * 1000)
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pys", required=True, help="label=python,label=python (each with httpp and uvicorn installed)")
    ap.add_argument("--cpp", default="", help="path of the cpp_bench binary")
    ap.add_argument("--oha", default="oha")
    ap.add_argument("--seconds", type=int, default=5)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--conc", type=int, default=64)
    ap.add_argument("--io-threads", default="1,2,4,8", help="I/O thread counts for httpp")
    ap.add_argument("--cpp-extra", default="", help="extra I/O thread counts only for the C++ server")
    ap.add_argument("--only", default="hello,json,cpu,latency", help="hello, json, cpu, latency (one connection)")
    ap.add_argument("--server-cpus", default="", help="e.g. 0-7 (default: derived from the topology)")
    ap.add_argument("--load-cpus", default="", help="e.g. 8-11")
    ap.add_argument("--rate", action="store_true", help="also run the fixed-rate latency phase (needs hello in --only)")
    ap.add_argument("--out", default="")
    args = ap.parse_args()
    args.pys = [tuple(x.split("=", 1)) for x in args.pys.split(",") if x]
    args.io_threads = [int(x) for x in args.io_threads.split(",") if x]
    args.cpp_extra = [int(x) for x in args.cpp_extra.split(",") if x]
    args.only = args.only.split(",")
    sc, lc = default_split()
    server_cpus = parse_cpus(args.server_cpus) or sc
    load_cpus = parse_cpus(args.load_cpus) or lc

    head = sysinfo(args, server_cpus, load_cpus)
    print(head, flush=True)
    variants = build_variants(args)
    failed = {}
    for r in range(args.rounds):
        for var in variants:
            if id(var) in failed:
                continue
            try:
                res = measure(var, args, server_cpus, load_cpus)
            except Exception as e:  # one broken variant must not lose the whole run
                failed[id(var)] = str(e).strip().splitlines()[-1:] or ["failed"]
                print(f"  FAILED {var.group} / {var.name}: {failed[id(var)][0]}", flush=True)
                continue
            var.runs.append(res)
            print(f"round {r + 1}/{args.rounds}  {var.group:<22} {var.name:<40} {res[0]:>10,.0f} req/s  {res[1]:6.1f} us/req  "
                  f"p50 {res[2]:6.2f} ms  p99 {res[3]:6.2f} ms  errors {res[4]}", flush=True)

    # Latency at a fixed request rate: half of what each variant reached in the closed-loop runs. At saturation the
    # latency of a closed loop is only queueing; here it is what a user would see at a comfortable load.
    rated = []
    if "rate" in args.only or args.rate:
        for var in variants:
            if var.rate and var.runs and id(var) not in failed:
                target = statistics.median(x[0] for x in var.runs) * 0.5
                try:
                    res = measure(var, args, server_cpus, load_cpus, rate=target)
                except Exception as e:
                    print(f"  FAILED rate {var.group} / {var.name}: {e}", flush=True)
                    continue
                rated.append((var, target, res))
                print(f"rate {var.group:<22} {var.name:<40} {target:>9,.0f} req/s target  p50 {res[2]:6.2f}  p99 {res[3]:6.2f}  p99.9 {res[5]:6.2f} ms", flush=True)

    lines = ["", "=== RESULTS (median of rounds; us/req = server CPU time per request, all threads; sd = standard deviation of the rounds) ===", ""]
    groups = []
    for var in variants:
        if var.group not in groups:
            groups.append(var.group)
    for g in groups:
        lines.append(g)
        lines.append(f"  {'variant':<42}{'req/s':>11}{'sd %':>7}{'[min-max]':>22}{'us/req':>9}{'p50 ms':>9}{'p99 ms':>9}{'errors':>8}")
        for var in (x for x in variants if x.group == g):
            if id(var) in failed:
                lines.append(f"  {var.name:<42}  FAILED: {failed[id(var)][0]}")
                continue
            rps = [x[0] for x in var.runs]
            lines.append(f"  {var.name:<42}{statistics.median(rps):>11,.0f}{(statistics.pstdev(rps) / statistics.mean(rps) * 100 if len(rps) > 1 else 0):>7.1f}{f'[{min(rps):,.0f}-{max(rps):,.0f}]':>22}"
                         f"{statistics.median(x[1] for x in var.runs):>9.1f}{statistics.median(x[2] for x in var.runs):>9.2f}"
                         f"{statistics.median(x[3] for x in var.runs):>9.2f}{sum(x[4] for x in var.runs):>8}")
        lines.append("")
    if rated:
        lines += ["=== LATENCY AT HALF OF EACH VARIANT'S MAXIMUM RATE (one run; not a closed loop, so not queueing at saturation) ===", "",
                  f"  {'variant':<52}{'target req/s':>13}{'p50 ms':>9}{'p99 ms':>9}{'p99.9 ms':>10}{'errors':>8}"]
        for var, target, res in rated:
            lines.append(f"  {var.group + ' / ' + var.name:<52}{target:>13,.0f}{res[2]:>9.2f}{res[3]:>9.2f}{res[5]:>10.2f}{res[4]:>8}")
        lines.append("")
    text = "\n".join(lines)
    print(text)
    if args.out:
        with open(args.out, "w") as f:
            f.write(head + "\n" + text + "\n")
        print(f"written to {args.out}")


if __name__ == "__main__":
    sys.exit(main())
