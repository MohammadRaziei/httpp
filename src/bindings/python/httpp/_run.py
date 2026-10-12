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


def run(app, host="127.0.0.1", port=8000, *, workers=1, reuse_port=False, log_level=None):
    """Serve `app` (an httpp.Server, or an import string 'module:attr') until Ctrl+C or SIGTERM.

    Shutdown is graceful: in-flight work ends and the app's on_shutdown hooks run.
    workers > 1 starts that many processes sharing the port (Linux only), which is how a
    Python server gets past the GIL; it needs an import string, because each worker imports
    the app itself. reuse_port lets separate httpp processes share one port the same way.
    """
    from .server import Server

    if not isinstance(host, str) or not host:
        raise ValueError("host must be a non-empty string")
    if not (isinstance(port, int) and 1 <= port <= 65535):
        raise ValueError("port must be an integer from 1 to 65535")
    if not (isinstance(workers, int) and workers >= 1):
        raise ValueError("workers must be an integer >= 1")
    if log_level is not None:
        from . import set_log_level

        set_log_level(log_level)

    if workers > 1:
        if not isinstance(app, str):
            raise TypeError("workers > 1 needs an import string like 'main:app': each worker process imports the app itself")
        return _supervise(app, host, port, workers, log_level)

    if isinstance(app, str):
        app = load(app)
    if not isinstance(app, Server):
        raise TypeError(f"expected an httpp.Server, got {type(app).__name__} (ASGI/WSGI apps are not supported yet)")
    if reuse_port:
        app.set_reuse_port(True)
    if threading.current_thread() is threading.main_thread():
        signal.signal(signal.SIGTERM, signal.default_int_handler)  # SIGTERM stops like Ctrl+C: graceful
    _say(f"starting on http://{host}:{port} (pid {os.getpid()}, Ctrl+C to stop)")
    app.listen(host, port)  # returns after Ctrl+C / stop(); raises OSError if the port can't be bound


def _supervise(target, host, port, workers, log_level):
    """Start `workers` fresh `httpp run` processes on one port and keep them until a signal or a death.

    ponytail: a worker that dies stops the whole group (exit code 1) instead of being restarted;
    let systemd/docker restart it. Upgrade path: respawn with a back-off.
    """
    if not sys.platform.startswith("linux"):
        raise OSError("workers > 1 relies on SO_REUSEPORT load balancing, which only Linux provides")
    cmd = [sys.executable, "-m", "httpp", "run", target, "--host", host, "--port", str(port), "--reuse-port"]
    if log_level:
        cmd += ["--log-level", log_level]
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
