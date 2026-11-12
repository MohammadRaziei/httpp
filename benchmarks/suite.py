#!/usr/bin/env python3
"""The benchmark suite: uvicorn, httpp (def / async def / await) on several Pythons and I/O thread counts, and
aiohttp, the C++ API, Drogon, and WebSocket echo servers, plus the HTTP clients (httpp, requests, httpx, aiohttp), all measured the same way. One command (the CMake target `bench`, see CMakeLists.txt), one output file to send back.

Method: variants run interleaved (round 1 of every variant, then round 2, ...), so a slow minute of the machine
hits all of them alike. The server and the load generator (`oha`) are pinned to separate CPUs. Server CPU time per
request comes from /proc (all threads). Numbers are only comparable with each other, inside one output file.

    python suite.py --pys 3.13=.venvs/py313/bin/python,3.14t=.venvs/py314t/bin/python --cpp bin/cpp_bench
"""
import argparse
import glob
import hashlib
import json
import os
import platform
import re
import resource
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
    lines.append(f"settings        conc={args.conc} seconds={args.seconds} rounds={args.rounds} warmup=1s ws_size={args.ws_size} idle_conns={args.idle_conns}")
    for label, py in args.pys:
        info = subprocess.run([py, "-c", "import sys,httpp;g=getattr(sys,'_is_gil_enabled',lambda:True)();"
                               "print(sys.version.split()[0],'GIL' if g else 'no-GIL (free-threaded)','httpp',httpp.__version__)"],
                              capture_output=True, text=True).stdout.strip()
        lines.append(f"python {label:<7} {info}")
        libs = subprocess.run([py, "-c", "from importlib.metadata import version as v\n"
                               "for n in ('uvicorn','uvloop','httptools','fastapi','starlette','aiohttp','httpx','requests','websockets','wsproto'):\n"
                               "    try: print(n, v(n), end='  ')\n"
                               "    except Exception: print(n, '-', end='  ')"], capture_output=True, text=True).stdout.strip()
        lines.append(f"       {'':<7} {libs}")
    cxx = shutil.which(os.environ.get("CXX", "g++"))
    if cxx:
        lines.append(f"c++ compiler    {subprocess.run([cxx, '--version'], capture_output=True, text=True).stdout.splitlines()[0]}  (CMake {os.environ.get('CMAKE_BUILD_TYPE', 'Release')} for the library and the bench programs)")
    lines.append(f"drogon          {args.drogon + '  (' + args.note + ')' if args.drogon else 'not built (HTTPP_BENCH_DROGON is OFF or uuid-dev is missing)'}")
    lines.append("uvicorn         one process, --no-access-log, uvloop when installed, httptools; the app is a bare ASGI function")
    lines.append("hello           a constant answer; json: a path parameter turned into an int and a JSON object back;")
    lines.append("cpu             sum of i*i for i<2000 in Python (about 60 us); the C++ server does the same sum in C++")
    lines.append("websocket        echo of a text message, closed loop: every connection sends the next one when the echo arrives (ws_load, no compression);")
    lines.append("                uvicorn uses its own websockets implementation; the websockets library runs on uvloop when installed")
    lines.append("idle            RSS of the server before and after N connections were opened and left idle (one run, not a median)")
    lines.append("rate            closed-loop runs measure queueing at saturation; the 'rate' tables hold latency at 50 percent of each maximum")
    return "\n".join(lines)


def hardware_info():
    """What the machine is, for the JSON file: CPU, memory, OS, laptop or not. No serial numbers and no host name."""
    lscpu = {}
    if shutil.which("lscpu"):
        for line in subprocess.run(["lscpu"], capture_output=True, text=True, env=dict(os.environ, LC_ALL="C")).stdout.splitlines():
            k, _, v = line.partition(":")
            lscpu[k.strip()] = v.strip()
    cpuinfo = read("/proc/cpuinfo")
    model = next((l.partition(":")[2].strip() for l in cpuinfo.splitlines() if l.startswith("model name")), "") or lscpu.get("Model name", "")
    pairs, phys = set(), ""
    for line in cpuinfo.splitlines():
        k, _, v = line.partition(":")
        if k.strip() == "physical id":
            phys = v.strip()
        elif k.strip() == "core id":
            pairs.add((phys, v.strip()))
    try:
        cores = int(lscpu["Socket(s)"]) * int(lscpu["Core(s) per socket"])
    except (KeyError, ValueError):
        cores = len(pairs) or None
    mem = {}
    for line in read("/proc/meminfo").splitlines():
        k, _, v = line.partition(":")
        if v.split():
            mem[k] = int(v.split()[0])  # kB
    osr = dict(l.split("=", 1) for l in read("/etc/os-release").splitlines() if "=" in l)
    fmax = read("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq")
    mhz = int(fmax) // 1000 if fmax.isdigit() else int(float(lscpu["CPU max MHz"])) if lscpu.get("CPU max MHz") else None
    power = glob.glob("/sys/class/power_supply/*")
    ac = [read(p + "/online") for p in power if os.path.basename(p).startswith(("AC", "ADP"))]
    vendor, product = read("/sys/class/dmi/id/sys_vendor"), read("/sys/class/dmi/id/product_name")
    info = {"cpu": model, "vendor": lscpu.get("Vendor ID", ""), "arch": platform.machine(), "sockets": int(lscpu["Socket(s)"]) if lscpu.get("Socket(s)", "").isdigit() else None,
            "physical_cores": cores, "logical_cpus": os.cpu_count(), "max_mhz": mhz, "l2_cache": lscpu.get("L2 cache", ""), "l3_cache": lscpu.get("L3 cache", ""),
            "memory_gib": round(mem["MemTotal"] / 1048576, 1) if "MemTotal" in mem else None, "swap_gib": round(mem.get("SwapTotal", 0) / 1048576, 1),
            "os": osr.get("PRETTY_NAME", "").strip('"'), "kernel": platform.release(), "wsl": "microsoft" in read("/proc/version").lower(),
            "hypervisor": lscpu.get("Hypervisor vendor", ""), "model": " ".join(x for x in (vendor, product) if x),
            "is_laptop": any(os.path.basename(p).startswith("BAT") for p in power), "on_battery": "0" in ac}
    return {k: v for k, v in info.items() if v not in ("", None)}


def meta_info(args, server_cpus, load_cpus):
    """The machine and the software of this run, for the JSON file (the docs site shows it next to every chart)."""
    gov = {read(f"/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_governor") for c in server_cpus}
    cxx = shutil.which(os.environ.get("CXX", "g++"))
    pythons = []
    probe = ("import sys, json\nfrom importlib.metadata import version as v\nout = {'version': sys.version.split()[0],"
             "'gil': bool(getattr(sys, '_is_gil_enabled', lambda: True)()), 'libs': {}, 'gil_after_import': {}}\n"
             "for n in %r:\n    try: out['libs'][n] = v(n)\n    except Exception: pass\nprint(json.dumps(out))")
    names = ("httpp", "uvicorn", "uvloop", "httptools", "fastapi", "starlette", "aiohttp", "httpx", "requests", "websockets")
    for label, py in args.pys:
        info = json.loads(subprocess.run([py, "-c", probe % (names,)], capture_output=True, text=True).stdout or "{}")
        for mod in ("httpp", "uvloop", "uvicorn", "aiohttp", "httpx", "requests"):  # a C extension without free-threading support turns the GIL back on
            out = subprocess.run([py, "-c", f"import sys, {mod}; print(bool(sys._is_gil_enabled()))"], capture_output=True, text=True).stdout.strip()
            if out:
                info.setdefault("gil_after_import", {})[mod] = out == "True"
        pythons.append(dict(info, label=label))
    return {"date": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "purpose": args.purpose, "hardware": hardware_info(),
            "server_cpus": cpu_ranges(server_cpus), "load_cpus": cpu_ranges(load_cpus),
            "governor": ",".join(sorted(g for g in gov if g)) or "?", "turbo_disabled": read("/sys/devices/system/cpu/intel_pstate/no_turbo", "?"),
            "tree": tree_id(), "compiler": subprocess.run([cxx, "--version"], capture_output=True, text=True).stdout.splitlines()[0] if cxx else "",
            "drogon": bool(args.drogon), "note": args.note, "pythons": pythons,
            "settings": {"conc": args.conc, "seconds": args.seconds, "rounds": args.rounds, "ws_size": args.ws_size, "idle_conns": args.idle_conns,
                         "client_conc": args.client_conc, "io_threads": args.io_threads, "only": args.only}}


class Variant:
    def __init__(self, group, name, path, cmd, env=None, conc=None, rate=False, kind="http"):
        self.group, self.name, self.path, self.cmd, self.env, self.conc, self.rate = group, name, path, cmd, env or {}, conc, rate
        self.kind = kind  # "http" (oha), "ws" (ws_load echo) or "idle" (memory with idle WebSocket connections)
        self.runs = []  # (req/s, us/req, p50 ms, p99 ms, errors, p99.9 ms)
        self.target = None  # kind "client": the Variant of the server the client talks to
        self.lib = ""  # kind "client": the library under test
        self.py = ""  # kind "client": the interpreter


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
        if subprocess.run([py, "-c", "import aiohttp"], capture_output=True).returncode == 0:
            loop = "uvloop" if import_ok else "asyncio"
            for path, what in (("/", "hello"), ("/cpu", "cpu"), ("/item/42", "json")):
                if what in args.only:
                    v.append(Variant(f"{what} (py {label})", f"aiohttp ({loop})", path, [py, "suite_aiohttp.py", "{port}"]))
        for n in args.io_threads:
            ht = lambda: [py, "-m", "httpp", "py", "run", "suite_app:app", "--port", "{port}", "--log-level", "error", "--io-threads", str(n)]
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
        ht = lambda: [py, "-m", "httpp", "py", "run", "suite_app:app", "--port", "{port}", "--log-level", "error", "--io-threads", "1"]
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
    if args.drogon and os.path.exists(args.drogon):
        for n in args.io_threads + [x for x in args.cpp_extra if x not in args.io_threads]:
            for path, what in (("/sync", "hello"), ("/cpu", "cpu")):
                if what in args.only:
                    v.append(Variant(f"{what} (C++)", f"Drogon, io_threads={n}", path, [args.drogon, "{port}", str(n)],
                                     rate=(what == "hello" and n in (1, args.io_threads[-1]))))
    v += ws_variants(args)
    v += client_variants(args)
    return v


def client_variants(args):
    """The HTTP clients, each against the same server (the C++ one when it is built: the fastest, so that it is the client that is measured)."""
    from client_worker import LIBS
    v = []
    if "client" not in args.only:
        return v
    if args.cpp and os.path.exists(args.cpp):
        target = Variant("client target", "httpp C++ route_io, io_threads=2", "/io", [args.cpp, "{port}", "2"])
    else:
        py = args.pys[0][1]
        target = Variant("client target", "uvicorn bare ASGI", "/", uvicorn_cmd(py) + ["--port", "{port}"])
    for label, py in args.pys:
        for lib, (mod, mode) in LIBS.items():
            if subprocess.run([py, "-c", f"import {mod}"], capture_output=True).returncode != 0:
                continue
            var = Variant(f"client GET, {args.client_conc} {'threads' if mode == 'sync' else 'tasks'} (py {label})", lib, target.path, [], kind="client")
            var.target, var.lib, var.py = target, lib, py
            v.append(var)
    return v


def ws_threads(args):
    """The I/O thread counts the Python WebSocket servers are run with: the first and 4 (or the last), to keep the run short."""
    return sorted({args.io_threads[0], 4 if 4 in args.io_threads else args.io_threads[-1]})


def ws_variants(args):
    v = []
    if "ws" not in args.only or not (args.ws_load and os.path.exists(args.ws_load)):
        return v
    what = f"websocket echo, {args.conc} connections, {args.ws_size} B"
    if args.cpp and os.path.exists(args.cpp):
        for n in args.io_threads:
            v.append(Variant(f"{what} (C++)", f"httpp C++, io_threads={n}", "/ws", [args.cpp, "{port}", str(n)], kind="ws"))
            if args.drogon and os.path.exists(args.drogon):
                v.append(Variant(f"{what} (C++)", f"Drogon, io_threads={n}", "/ws", [args.drogon, "{port}", str(n)], kind="ws"))
    for label, py in args.pys:
        g = f"{what} (py {label})"
        v.append(Variant(g, "uvicorn bare ASGI", "/ws", uvicorn_cmd(py) + ["--port", "{port}"], kind="ws"))
        v.append(Variant(g, "websockets library", "/ws", [py, "ws_websockets.py", "{port}"], kind="ws"))
        if subprocess.run([py, "-c", "import aiohttp"], capture_output=True).returncode == 0:
            v.append(Variant(g, "aiohttp", "/ws", [py, "suite_aiohttp.py", "{port}"], kind="ws"))
        for n in ws_threads(args):
            v.append(Variant(g, f"httpp async def, io_threads={n}", "/ws", httpp_cmd(py, n), kind="ws"))
    return v


def idle_variants(args):
    """One server of each kind, io_threads=1: how much memory it holds for N connections that do nothing."""
    v = []
    if "idle" not in args.only or not (args.ws_load and os.path.exists(args.ws_load)):
        return v
    g = "memory with idle WebSocket connections"
    if args.cpp and os.path.exists(args.cpp):
        v.append(Variant(g, "httpp C++", "/ws", [args.cpp, "{port}", "1"], kind="idle"))
    if args.drogon and os.path.exists(args.drogon):
        v.append(Variant(g, "Drogon", "/ws", [args.drogon, "{port}", "1"], kind="idle"))
    for label, py in args.pys:
        v.append(Variant(g, f"httpp async def (py {label})", "/ws", httpp_cmd(py, 1), kind="idle"))
        v.append(Variant(g, f"uvicorn bare ASGI (py {label})", "/ws", uvicorn_cmd(py) + ["--port", "{port}"], kind="idle"))
        v.append(Variant(g, f"websockets library (py {label})", "/ws", [py, "ws_websockets.py", "{port}"], kind="idle"))
        if subprocess.run([py, "-c", "import aiohttp"], capture_output=True).returncode == 0:
            v.append(Variant(g, f"aiohttp (py {label})", "/ws", [py, "suite_aiohttp.py", "{port}"], kind="idle"))
    return v


def uvicorn_cmd(py):
    cmd = [py, "-m", "uvicorn", "suite_asgi:app", "--log-level", "error", "--no-access-log"]
    if subprocess.run([py, "-c", "import uvloop"], capture_output=True).returncode == 0:
        cmd += ["--loop", "uvloop"]
    return cmd


def httpp_cmd(py, n):
    return [py, "-m", "httpp", "py", "run", "suite_app:app", "--port", "{port}", "--log-level", "error", "--io-threads", str(n)]


def ticks(pid):
    parts = read(f"/proc/{pid}/stat").rsplit(")", 1)[1].split()
    return int(parts[11]) + int(parts[12])


def oha(args, load_cpus, url, seconds, conc=None, rate=None):
    cmd = ["taskset", "-c", cpu_ranges(load_cpus)] if shutil.which("taskset") else []
    cmd += [args.oha] + (["-q", str(int(rate))] if rate else []) + ["-z", f"{seconds}s", "-c", str(conc or args.conc), "--no-tui", "--output-format", "json", url]
    return json.loads(subprocess.run(cmd, capture_output=True, check=True).stdout)


def _raise_nofile():
    """Run in the child before exec: the servers need one file descriptor per idle WebSocket connection."""
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (hard if hard != resource.RLIM_INFINITY else 1 << 20, hard))


class started:
    """A server process, pinned to the server CPUs, waited for until it accepts connections; stopped on exit."""

    def __init__(self, var, args, server_cpus):
        self.var, self.args, self.server_cpus = var, args, server_cpus

    def __enter__(self):
        self.port = free_port()
        cmd = [c.replace("{port}", str(self.port)) for c in self.var.cmd]
        if shutil.which("taskset"):
            cmd = ["taskset", "-c", cpu_ranges(self.server_cpus)] + cmd
        env = dict(os.environ, PYTHONPATH=HERE, **self.var.env)
        self.proc = proc = subprocess.Popen(cmd, cwd=HERE, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, preexec_fn=_raise_nofile)
        for _ in range(150):
            if proc.poll() is not None:
                raise RuntimeError(proc.stderr.read().decode()[-600:])
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.1).close()
                return self
            except OSError:
                time.sleep(0.1)
        self.__exit__()
        raise RuntimeError("did not start")

    def __exit__(self, *exc):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def measure_client(var, args, server_cpus, load_cpus):
    """One client run: the server is started, the client library makes requests for the measuring window in a process of its own."""
    with started(var.target, args, server_cpus) as srv:
        cmd = ["taskset", "-c", cpu_ranges(load_cpus)] if shutil.which("taskset") else []
        cmd += [var.py, "client_worker.py", "--lib", var.lib, "--port", str(srv.port), "--path", var.path, "--seconds", str(args.seconds),
                "--warmup", "1", "--conc", str(args.client_conc)]
        done = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True, timeout=args.seconds + 60)
        if done.returncode or not done.stdout.strip():
            raise RuntimeError((done.stderr.strip().splitlines() or ["client_worker failed"])[-1])
        d = json.loads(done.stdout.strip().splitlines()[-1])
        return (d["rps"], d["cpu_us_per_req"], d["p50_ms"], d["p99_ms"], d["errors"], d["p999_ms"])


def measure(var, args, server_cpus, load_cpus, rate=None):
    if var.kind == "ws":
        return measure_ws(var, args, server_cpus, load_cpus)
    if var.kind == "client":
        return measure_client(var, args, server_cpus, load_cpus)
    with started(var, args, server_cpus) as srv:
        proc, port = srv.proc, srv.port
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


def ws_load_cmd(args, load_cpus, port, *extra):
    cmd = ["taskset", "-c", cpu_ranges(load_cpus)] if shutil.which("taskset") else []
    return cmd + [args.ws_load, str(port), "--size", str(args.ws_size), "--threads", str(max(1, min(4, len(load_cpus))))] + list(extra)


def measure_ws(var, args, server_cpus, load_cpus):
    """One WebSocket echo run: ws_load prints a line when its measuring window opens (the server CPU time is read then) and a JSON
    summary when it closes."""
    with started(var, args, server_cpus) as srv:
        proc = srv.proc
        load = subprocess.Popen(ws_load_cmd(args, load_cpus, srv.port, "--conns", str(var.conc or args.conc), "--seconds", str(args.seconds),
                                            "--warmup", "1"), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        first = load.stdout.readline()
        t0 = ticks(proc.pid)
        rest = load.stdout.read()
        t1 = ticks(proc.pid)
        load.wait()
        err = load.stderr.read()
        if not first.strip().startswith("{\"started\""):
            raise RuntimeError(err.strip().splitlines()[-1] if err.strip() else "ws_load did not start measuring")
        data = json.loads(rest.strip().splitlines()[-1])
        bad = data["errors"] + data["failed"]
        if bad or load.returncode:
            rc = proc.poll()
            print(f"  ! {var.name}: {bad} errors ({data['connected']} connected), server {'is alive' if rc is None else f'exited with code {rc}'}; {err.strip()[-300:]}",
                  file=sys.stderr, flush=True)
        n = data["msgs_per_s"] * args.seconds
        return (data["msgs_per_s"], (t1 - t0) / os.sysconf("SC_CLK_TCK") * 1e6 / max(n, 1), data["p50_ms"], data["p99_ms"], bad, data["p999_ms"])


def rss_kib(pid):
    for line in read(f"/proc/{pid}/status").splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    return 0


def measure_idle(var, args, server_cpus, load_cpus, conns):
    """(connected, failed, RSS before in KiB, RSS after in KiB): the connections do nothing, ws_load holds them until its stdin closes."""
    with started(var, args, server_cpus) as srv:
        time.sleep(1.0)
        before = rss_kib(srv.proc.pid)
        load = subprocess.Popen(ws_load_cmd(args, load_cpus, srv.port, "--idle", "--conns", str(conns)),
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            line = load.stdout.readline()
            if not line:
                err = load.stderr.read().strip()
                raise RuntimeError(err.splitlines()[-1] if err else "ws_load failed")
            info = json.loads(line)
            time.sleep(1.0)
            after = rss_kib(srv.proc.pid)
        finally:
            load.stdin.close()
            try:
                load.wait(10)
            except subprocess.TimeoutExpired:
                load.kill()
        return info["connected"], info["failed"], before, after


def run_idle(args, server_cpus, load_cpus):
    """(text lines, [dict per server]) for the idle WebSocket memory table."""
    idle = idle_variants(args)
    data = []
    if not idle:
        return [], data
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    cap = (1 << 20) if hard == resource.RLIM_INFINITY else hard
    conns = max(100, min(args.idle_conns, cap - 300))
    lines = [f"=== MEMORY WITH {conns:,} IDLE WEBSOCKET CONNECTIONS (RSS of the server process, one run; io_threads=1) ===" + (
             f"  [asked for {args.idle_conns:,}: the open-file limit is {cap:,}]" if conns < args.idle_conns else ""), "",
             f"  {'server':<42}{'connected':>10}{'failed':>8}{'RSS before MiB':>16}{'RSS after MiB':>15}{'KiB / connection':>18}"]
    for var in idle:
        try:
            connected, bad, before, after = measure_idle(var, args, server_cpus, load_cpus, conns)
        except Exception as e:  # noqa: BLE001
            lines.append(f"  {var.name:<42}  FAILED: {str(e).strip().splitlines()[-1:] or ['failed']}")
            print(f"  FAILED idle {var.name}: {e}", flush=True)
            continue
        per = (after - before) / max(connected, 1)
        data.append({"server": var.name, "connected": connected, "failed": bad, "rss_before_kib": before, "rss_after_kib": after, "kib_per_connection": per})
        lines.append(f"  {var.name:<42}{connected:>10,}{bad:>8}{before / 1024:>16.1f}{after / 1024:>15.1f}{per:>18.2f}")
        print(f"idle {var.name:<40} {connected:,} connected, RSS {before / 1024:.1f} -> {after / 1024:.1f} MiB ({per:.2f} KiB each)", flush=True)
    lines.append("")
    return lines, data


def describe(var):
    """The structured form of a variant for the JSON file: suite, runtime, server family, I/O threads."""
    g = var.group
    suite = "ws" if g.startswith("websocket") else "latency" if g.startswith("latency") else "client" if g.startswith("client") else g.split(" (")[0]
    m = re.search(r"\(py ([^,)]+)", g)
    runtime = ("py" + m.group(1).replace(".", "")) if m else "cpp" if "(C++)" in g else ""
    if var.kind == "idle":
        m = re.search(r"\(py ([^,)]+)", var.name)
        runtime = ("py" + m.group(1).replace(".", "")) if m else "cpp"
    name = var.lib or var.name
    family = next((f for k, f in (("FastAPI", "fastapi"), ("uvicorn", "uvicorn"), ("aiohttp", "aiohttp"), ("Drogon", "drogon"), ("websockets library", "websockets"),
                                  ("httpp", "httpp")) if k in name), name)
    m = re.search(r"io_threads=(\d+)", name)
    return {"suite": suite, "runtime": runtime, "server": family, "variant": re.sub(r",? ?io_threads=\d+", "", name).strip(), "io_threads": int(m.group(1)) if m else None,
            "group": g}


def result_rows(variants, failed):
    rows = []
    for var in variants:
        row = describe(var)
        row["unit"] = "msg/s" if var.kind == "ws" else "req/s"
        if id(var) in failed:
            row["failed"] = failed[id(var)][0]
        elif var.runs:
            rps = [x[0] for x in var.runs]
            row.update({"rps": statistics.median(rps), "rps_sd_pct": (statistics.pstdev(rps) / statistics.mean(rps) * 100) if len(rps) > 1 and statistics.mean(rps) else 0.0,
                        "rps_min": min(rps), "rps_max": max(rps), "us_per_req": statistics.median(x[1] for x in var.runs),
                        "p50_ms": statistics.median(x[2] for x in var.runs), "p99_ms": statistics.median(x[3] for x in var.runs),
                        "p999_ms": statistics.median(x[5] for x in var.runs), "errors": sum(x[4] for x in var.runs),
                        "rounds": [{"rps": x[0], "us_per_req": x[1], "p50_ms": x[2], "p99_ms": x[3], "p999_ms": x[5], "errors": x[4]} for x in var.runs]})
        else:
            continue
        rows.append(row)
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pys", required=True, help="label=python,label=python (each with httpp and uvicorn installed)")
    ap.add_argument("--cpp", default="", help="path of the cpp_bench binary")
    ap.add_argument("--oha", default="oha")
    ap.add_argument("--ws-load", default="", help="path of the ws_load binary (the WebSocket load generator)")
    ap.add_argument("--drogon", default="", help="path of the drogon_bench binary")
    ap.add_argument("--note", default="", help="free text for the header (what was built)")
    ap.add_argument("--ws-size", type=int, default=64, help="bytes per WebSocket message")
    ap.add_argument("--idle-conns", type=int, default=10000, help="idle WebSocket connections for the memory table")
    ap.add_argument("--out-dir", default="", help="write <out-dir>/<timestamp>.txt (instead of --out)")
    ap.add_argument("--seconds", type=int, default=5)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--conc", type=int, default=64)
    ap.add_argument("--io-threads", default="1,2,4,8", help="I/O thread counts for httpp")
    ap.add_argument("--cpp-extra", default="", help="extra I/O thread counts only for the C++ server")
    ap.add_argument("--only", default="hello,json,cpu,latency,ws,idle,client",
                    help="hello, json, cpu, latency (one connection), ws (WebSocket echo), idle (memory with idle WebSockets), client (HTTP client libraries)")
    ap.add_argument("--server-cpus", default="", help="e.g. 0-7 (default: derived from the topology)")
    ap.add_argument("--load-cpus", default="", help="e.g. 8-11")
    ap.add_argument("--rate", action="store_true", help="also run the fixed-rate latency phase (needs hello in --only)")
    ap.add_argument("--out", default="")
    ap.add_argument("--client-conc", type=int, default=32, help="threads (sync clients) or tasks (async clients) of the client part")
    ap.add_argument("--purpose", default="published", choices=("published", "smoke"),
                    help="smoke: a test of the machinery; the docs site ignores it (and benchmarks/results/.gitignore keeps it out of git)")
    args = ap.parse_args()
    args.pys = [tuple(x.split("=", 1)) for x in args.pys.split(",") if x]
    args.io_threads = [int(x) for x in args.io_threads.split(",") if x]
    args.cpp_extra = [int(x) for x in args.cpp_extra.split(",") if x]
    args.only = args.only.split(",")
    sc, lc = default_split()
    server_cpus = parse_cpus(args.server_cpus) or sc
    load_cpus = parse_cpus(args.load_cpus) or lc

    head = sysinfo(args, server_cpus, load_cpus)
    meta = meta_info(args, server_cpus, load_cpus)
    print(head, flush=True)
    if args.out_dir and not args.out:
        os.makedirs(args.out_dir, exist_ok=True)
        stem = time.strftime("%Y%m%d-%H%M%S")
        args.out = os.path.join(args.out_dir, ("smoke-" if args.purpose == "smoke" else "") + stem + ".txt")
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
            print(f"round {r + 1}/{args.rounds}  {var.group:<22} {var.name:<40} {res[0]:>10,.0f} {'msg' if var.kind == 'ws' else 'req'}/s  {res[1]:6.1f} us/{'msg' if var.kind == 'ws' else 'req'}  "
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
        unit = "msg/s" if any(x.kind == "ws" for x in variants if x.group == g) else "req/s"
        lines.append(f"  {'variant':<42}{unit:>11}{'sd %':>7}{'[min-max]':>22}{('us/msg' if unit == 'msg/s' else 'us/req'):>9}{'p50 ms':>9}{'p99 ms':>9}{'errors':>8}")
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
    idle_lines, idle_data = run_idle(args, server_cpus, load_cpus)
    if idle_lines:
        lines += idle_lines
    text = "\n".join(lines)
    print(text)
    if args.out:
        with open(args.out, "w") as f:
            f.write(head + "\n" + text + "\n")
        print(f"written to {args.out}")
        meta["idle_conns_measured"] = idle_data[0]["connected"] if idle_data else 0
        doc = {"schema": 1, "meta": meta, "results": result_rows(variants, failed),
               "rated": [dict(describe(var), target_rps=target, p50_ms=res[2], p99_ms=res[3], p999_ms=res[5], errors=res[4]) for var, target, res in rated],
               "idle": idle_data}
        json_path = os.path.splitext(args.out)[0] + ".json"
        with open(json_path, "w") as f:
            json.dump(doc, f, indent=1)
            f.write("\n")
        print(f"written to {json_path}")


if __name__ == "__main__":
    sys.exit(main())
