"""run(): serve an httpp.Server until Ctrl+C / SIGTERM, on one process or several."""

import importlib
import os
import signal
import subprocess
import sys
import threading
import time

_GRACE = 10  # seconds a worker gets to finish after SIGTERM before it is killed
# Worker respawn (ADR-0044). Seconds to wait before restart number 1, 2, ... of a worker that keeps dying young; one more quick death
# after the last entry stops the whole group. A worker that lived at least _STABLE seconds starts the count over.
_RESPAWN_DELAYS = (0.0, 0.5, 1.0, 2.0, 4.0)
_STABLE = 30.0


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


def run(app, host="127.0.0.1", port=8000, *, workers=1, reuse_port=False, log_level=None, io_threads=None, shutdown_timeout=None,
        reload=False, reload_dirs=None, reload_patterns=None,
        ssl_certfile=None, ssl_keyfile=None, ssl_keyfile_password=None):
    """Serve `app` (an httpp.Server, or an import string 'module:attr') until Ctrl+C or SIGTERM.

    Shutdown is graceful: the port closes, requests in progress finish (they get "Connection: close"), and the app's on_shutdown
    hooks run. shutdown_timeout (seconds, default the app's `shutdown_timeout`, 10) is how long that may take before what is
    still open is cut; a second Ctrl+C cuts at once.
    ssl_certfile and ssl_keyfile (PEM files; ssl_keyfile_password if the key is encrypted) serve https and wss instead of http and ws
    (ADR-0040); both are needed, and the files are checked before anything starts.
    reload=True (development only; needs an import string and one worker) runs the app in a child process and restarts
    it when a file matching reload_patterns (default ["*.py"]) under reload_dirs (default: the current directory) changes.
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
    if shutdown_timeout is not None:
        if isinstance(shutdown_timeout, bool) or not isinstance(shutdown_timeout, (int, float)):
            raise TypeError("shutdown_timeout must be a number of seconds")
        if not (0 <= shutdown_timeout < 1e9):
            raise ValueError("shutdown_timeout must be a number of seconds from 0 up")
    if reload:
        if not isinstance(app, str):
            raise TypeError("reload needs an import string like 'main:app': the app is imported again in a new process")
        if workers != "auto" and workers != 1:
            raise ValueError("reload cannot be combined with workers")
        workers = 1
    if (ssl_certfile is None) != (ssl_keyfile is None):
        raise ValueError("ssl_certfile and ssl_keyfile go together")
    for name, value in (("ssl_certfile", ssl_certfile), ("ssl_keyfile", ssl_keyfile)):
        if value is not None and not isinstance(value, (str, os.PathLike)):
            raise TypeError(f"{name} must be a path")
    if ssl_keyfile_password is not None and not isinstance(ssl_keyfile_password, str):
        raise TypeError("ssl_keyfile_password must be a string")
    if ssl_certfile is not None:
        ssl_certfile, ssl_keyfile = os.fspath(ssl_certfile), os.fspath(ssl_keyfile)
        Server().set_tls(ssl_certfile, ssl_keyfile, ssl_keyfile_password)  # check the files now: a bad one fails before anything starts
    auto = workers == "auto"
    workers = _resolve_workers(workers)
    if auto and workers > 1 and not isinstance(app, str):
        _say("workers=auto needs an import string like 'main:app' to start several processes; running one")
        workers = 1
    from . import _log, set_log_level

    set_log_level(log_level or "info")  # like uvicorn: startup and one line per request; "warning" silences them
    _log.ensure_handler()  # with no logging configured by the app, print to stderr

    if reload:
        from . import _reload

        return _reload.serve(app, host, port, log_level, io_threads, shutdown_timeout, reload_dirs, reload_patterns, tls=_tls_args(ssl_certfile, ssl_keyfile, ssl_keyfile_password))

    if workers > 1:
        if not isinstance(app, str):
            raise TypeError("workers > 1 needs an import string like 'main:app': each worker process imports the app itself")
        return _supervise(app, host, port, workers, log_level, io_threads, shutdown_timeout, tls=_tls_args(ssl_certfile, ssl_keyfile, ssl_keyfile_password))

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
    if ssl_certfile is not None:
        app.set_tls(ssl_certfile, ssl_keyfile, ssl_keyfile_password)
    if reuse_port:
        app.set_reuse_port(True)
    if io_threads is not None:
        app.set_io_threads(io_threads)
    if shutdown_timeout is not None:
        app.shutdown_timeout = shutdown_timeout
    if threading.current_thread() is threading.main_thread():
        signal.signal(signal.SIGTERM, signal.default_int_handler)  # SIGTERM stops like Ctrl+C: graceful
    app.listen(host, port)  # logs "listening on ..." (info) once the port is really bound  # returns after Ctrl+C / stop(); raises OSError if the port can't be bound


def _tls_args(certfile, keyfile, password):
    """The command line options that give a child `httpp py run` the same TLS settings."""
    if certfile is None:
        return []
    args = ["--ssl-certfile", certfile, "--ssl-keyfile", keyfile]
    if password is not None:
        args += ["--ssl-keyfile-password", password]
    return args


def _describe_exit(code):
    if code < 0:
        try:
            return f"was killed by {signal.Signals(-code).name}"
        except ValueError:
            return f"was killed by signal {-code}"
    return f"exited with code {code}"


class _WorkerGroup:
    """The worker processes of one supervisor: starts them, notices deaths and restarts them with a back-off.

    `spawn()` returns a started process (anything with `pid`, `poll()` and `returncode`). The supervisor calls `poll()` every
    few hundred milliseconds; it returns False when a worker keeps dying right after it starts, which means the supervisor
    should stop the group: restarting for ever would only hide a broken app or a port that cannot be bound.
    """

    class _Slot:
        __slots__ = ("proc", "started", "streak", "restart_at")

        def __init__(self):
            self.proc = None
            self.started = 0.0
            self.streak = 0          # deaths in a row of a worker that lived less than _STABLE seconds
            self.restart_at = None   # set while the dead worker of this slot waits to be started again

    def __init__(self, spawn, count, clock=time.monotonic, say=None):
        self._spawn, self._clock, self._say = spawn, clock, say or _say
        self._slots = [self._Slot() for _ in range(count)]
        for slot in self._slots:
            self._start(slot)

    def _start(self, slot):
        slot.proc = self._spawn()
        slot.started = self._clock()
        slot.restart_at = None

    def procs(self):
        """Every process of the group, the dead one of a slot that waits for its restart included."""
        return [slot.proc for slot in self._slots if slot.proc is not None]

    def poll(self):
        now = self._clock()
        for slot in self._slots:
            if slot.restart_at is None:
                code = slot.proc.poll()
                if code is None:
                    continue
                if now - slot.started >= _STABLE:
                    slot.streak = 0
                slot.streak += 1
                what = f"worker {slot.proc.pid} {_describe_exit(code)}"
                if slot.streak > len(_RESPAWN_DELAYS):
                    self._say(f"{what}; it keeps dying right after it starts, stopping all")
                    return False
                delay = _RESPAWN_DELAYS[slot.streak - 1]
                slot.restart_at = now + delay
                self._say(f"{what}; restarting" + (f" in {delay:g}s" if delay else ""))
            if now >= slot.restart_at:
                self._start(slot)
        return True


def _parent_death_hook():
    """A `preexec_fn` that makes the kernel send SIGTERM to a worker when its supervisor dies, even by SIGKILL (Linux).

    Without it the workers of a killed supervisor keep the port for ever. None when libc has no prctl.
    """
    try:
        import ctypes

        prctl = ctypes.CDLL(None, use_errno=True).prctl
        prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
    except (ImportError, OSError, AttributeError):
        return None
    supervisor = os.getpid()

    def hook():  # runs in the child between fork and exec: nothing but plain calls here
        prctl(1, int(signal.SIGTERM), 0, 0, 0)  # PR_SET_PDEATHSIG
        if os.getppid() != supervisor:  # the supervisor died before the line above took effect
            os._exit(1)

    return hook


def _supervise(target, host, port, workers, log_level, io_threads=None, shutdown_timeout=None, tls=()):
    """Start `workers` fresh `httpp py run` processes on one port and keep them until a signal.

    A worker that dies is started again (back-off, see _WorkerGroup); one that keeps dying right after it starts stops the
    whole group with exit code 1. Workers also stop when this process is killed (see _parent_death_hook).
    """
    if not sys.platform.startswith("linux"):
        raise OSError("workers > 1 relies on SO_REUSEPORT load balancing, which only Linux provides")
    cmd = [sys.executable, "-m", "httpp", "py", "run", target, "--host", host, "--port", str(port), "--reuse-port"]
    if log_level:
        cmd += ["--log-level", log_level]
    if io_threads is not None:
        cmd += ["--io-threads", str(io_threads)]
    cmd += list(tls)
    grace = _GRACE if shutdown_timeout is None else shutdown_timeout
    if shutdown_timeout is not None:
        cmd += ["--shutdown-timeout", str(shutdown_timeout)]
    hook = _parent_death_hook()
    # Own session: Ctrl+C reaches only this supervisor, which then sends each worker exactly one SIGTERM.
    group = _WorkerGroup(lambda: subprocess.Popen(cmd, start_new_session=True, preexec_fn=hook), workers)
    stop = []
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.append(1))
    _say(f"{workers} workers on http://{host}:{port} (supervisor pid {os.getpid()}, Ctrl+C to stop)")
    code = 0
    try:
        while not stop:
            time.sleep(0.2)
            if not group.poll():
                code = 1
                break
    finally:
        procs = group.procs()
        for p in procs:
            if p.poll() is None:
                p.terminate()
        deadline = time.monotonic() + grace + 2  # the workers cut at `grace` themselves; this is only for a stuck one
        for p in procs:
            try:
                p.wait(max(0.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
    return code
