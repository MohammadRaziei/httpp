"""run(): serve an httpp.Server until Ctrl+C / SIGTERM, on one process or several."""

import importlib
import os
import signal
import subprocess
import sys
import threading
import time

_GRACE = 10  # seconds a worker gets to finish after SIGTERM before it is killed


def _say(message):
    sys.stderr.write(f"httpp: {message}\n")  # one write, so workers' lines don't interleave
    sys.stderr.flush()


def load(target):
    """'pkg.mod:attr' -> the object ('attr' may be dotted, e.g. 'main:services.app')."""
    module, sep, attr = target.partition(":")
    if not (sep and module and attr):
        raise ValueError(f"expected 'module:attribute' (e.g. 'main:app'), got {target!r}")
    if os.getcwd() not in sys.path:
        sys.path.insert(0, os.getcwd())  # like `python -m`: the app next to you is importable
    try:
        obj = importlib.import_module(module)
    except ModuleNotFoundError as e:
        if e.name == module.split(".")[0]:  # the module itself is missing, not something it imports
            raise ValueError(f"cannot import {module!r}: {e}") from None
        raise
    for part in attr.split("."):
        try:
            obj = getattr(obj, part)
        except AttributeError:
            raise ValueError(f"{module!r} has no attribute {attr!r}") from None
    return obj


_CGROUP_V2_CPU_MAX = "/sys/fs/cgroup/cpu.max"
_CGROUP_V1_QUOTA = "/sys/fs/cgroup/cpu/cpu.cfs_quota_us"
_CGROUP_V1_PERIOD = "/sys/fs/cgroup/cpu/cpu.cfs_period_us"


def _cgroup_cpu_limit():
    """The CPU quota of this container in whole CPUs (rounded down, at least 1), or None for no limit.
    A process may run on every CPU of the host and still be throttled to a fraction of them."""
    try:
        with open(_CGROUP_V2_CPU_MAX) as f:
            quota, period = f.read().split()[:2]
        if quota != "max":
            return max(1, int(quota) // int(period))
    except (OSError, ValueError, ZeroDivisionError):
        pass
    try:
        with open(_CGROUP_V1_QUOTA) as f:
            quota = int(f.read())
        with open(_CGROUP_V1_PERIOD) as f:
            period = int(f.read())
        if quota > 0 and period > 0:
            return max(1, quota // period)
    except (OSError, ValueError):
        pass
    return None


def _cpu_count():
    """How many CPUs this process can really use: its affinity mask, capped by a container CPU quota."""
    try:
        n = len(os.sched_getaffinity(0))
    except AttributeError:  # macOS, Windows
        n = os.cpu_count() or 1
    limit = _cgroup_cpu_limit()
    return max(1, min(n, limit) if limit else n)


def _resolve_workers(workers):
    """An int >= 1, or "auto" (one worker per usable CPU; one process where several can't share a port)."""
    if workers == "auto" and isinstance(workers, str):
        return _cpu_count() if sys.platform.startswith("linux") else 1
    if isinstance(workers, int) and not isinstance(workers, bool) and workers >= 1:
        return workers
    raise ValueError('workers must be an integer >= 1 or "auto"')


def run(app, host="127.0.0.1", port=8000, *, workers=1, reuse_port=False, log_level=None, io_threads=None):
    """Serve `app` (an httpp.Server, or an import string 'module:attr') until Ctrl+C or SIGTERM.

    Shutdown is graceful: in-flight work ends and the app's on_shutdown hooks run.
    workers > 1 starts that many processes sharing the port (Linux only), which is how a
    Python server gets past the GIL; it needs an import string, because each worker imports
    the app itself. log_level is "info" by default (one access line per request on stderr);
    "warning" keeps only problems, "off" nothing. workers="auto" means one per CPU this process can use (the affinity mask,
    capped by a container CPU quota): one process on a single CPU, off Linux, or when `app`
    is an object rather than an import string. reuse_port lets separate httpp processes
    share one port the same way. io_threads (default: what the app set, else 1) is the number of I/O threads
    sharing the port inside each process; on a free-threaded Python each also gets its own asyncio loop.
    """
    from .server import Server

    if not isinstance(host, str) or not host:
        raise ValueError("host must be a non-empty string")
    if not (isinstance(port, int) and 1 <= port <= 65535):
        raise ValueError("port must be an integer from 1 to 65535")
    auto = workers == "auto"
    workers = _resolve_workers(workers)
    if auto and workers > 1 and not isinstance(app, str):
        _say("workers=auto needs an import string like 'main:app' to start several processes; running one")
        workers = 1
    from . import _log, set_log_level

    set_log_level(log_level or "info")  # like uvicorn: startup and one line per request; "warning" silences them
    _log.ensure_handler()  # with no logging configured by the app, print to stderr

    if workers > 1:
        if not isinstance(app, str):
            raise TypeError("workers > 1 needs an import string like 'main:app': each worker process imports the app itself")
        return _supervise(app, host, port, workers, log_level, io_threads)

    if isinstance(app, str):
        # The file may also start itself (`app.listen(...)` / `app.run()` outside an `if __name__` guard);
        # while it imports, those calls do nothing, so this run's host, port and workers win.
        from . import server as _server

        _server._defer_listen = True
        try:
            app = load(app)
        finally:
            _server._defer_listen = False
    if not isinstance(app, Server):
        raise TypeError(f"expected an httpp.Server, got {type(app).__name__} (ASGI/WSGI apps are not supported yet)")
    if reuse_port:
        app.set_reuse_port(True)
    if io_threads is not None:
        app.set_io_threads(io_threads)
    if threading.current_thread() is threading.main_thread():
        signal.signal(signal.SIGTERM, signal.default_int_handler)  # SIGTERM stops like Ctrl+C: graceful
    app.listen(host, port)  # logs "listening on ..." (info) once the port is really bound  # returns after Ctrl+C / stop(); raises OSError if the port can't be bound


def _supervise(target, host, port, workers, log_level, io_threads=None):
    """Start `workers` fresh `httpp run` processes on one port and keep them until a signal or a death.

    ponytail: a worker that dies stops the whole group (exit code 1) instead of being restarted;
    let systemd/docker restart it. Upgrade path: respawn with a back-off.
    """
    if not sys.platform.startswith("linux"):
        raise OSError("workers > 1 relies on SO_REUSEPORT load balancing, which only Linux provides")
    cmd = [sys.executable, "-m", "httpp", "run", target, "--host", host, "--port", str(port), "--reuse-port"]
    if log_level:
        cmd += ["--log-level", log_level]
    if io_threads is not None:
        cmd += ["--io-threads", str(io_threads)]
    # Own session: Ctrl+C reaches only this supervisor, which then sends each worker exactly one SIGTERM.
    procs = [subprocess.Popen(cmd, start_new_session=True) for _ in range(workers)]
    stop = []
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.append(1))
    _say(f"{workers} workers on http://{host}:{port} (supervisor pid {os.getpid()}, Ctrl+C to stop)")
    code = 0
    try:
        while not stop:
            time.sleep(0.2)
            dead = [p for p in procs if p.poll() is not None]
            if dead:
                _say(f"worker {dead[0].pid} exited with code {dead[0].returncode}; stopping all")
                code = 1
                break
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        deadline = time.monotonic() + _GRACE
        for p in procs:
            try:
                p.wait(max(0.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
    return code
