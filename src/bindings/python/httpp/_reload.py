"""httpp py run --reload: restart the app when a source file changes (development only, ADR-0039).

A supervisor process polls the files, and runs the app as a child `httpp py run` process; on a change it stops the child
(a graceful stop with a short grace period) and starts a new one, so the new code is imported fresh. A child that
fails to start (a syntax error) or dies leaves the supervisor waiting for the next change.
"""

import fnmatch
import os
import signal
import subprocess
import sys
import threading
import time

_SKIP_DIRS = {"__pycache__", "node_modules"}
_POLL = 0.25  # seconds between scans
_CHILD_GRACE = 1.0  # seconds the old process gets to finish its requests when the code changes


def _say(message):
    print(f"httpp: {message}", file=sys.stderr, flush=True)


def snapshot(dirs, patterns):
    """{path: (mtime_ns, size)} of every file under `dirs` whose name matches one of `patterns`.

    Hidden directories, __pycache__, node_modules and virtual environments (a directory with pyvenv.cfg) are skipped.
    ponytail: polling with os.scandir, no dependency and the same on every OS; a few hundred files cost about a
    millisecond per scan. Upgrade path: inotify / ReadDirectoryChangesW for very large trees.
    """
    out = {}
    stack = list(dirs)
    while stack:
        top = stack.pop()
        try:
            entries = list(os.scandir(top))
        except OSError:
            continue
        if any(e.name == "pyvenv.cfg" for e in entries):
            continue
        for e in entries:
            try:
                if e.is_dir(follow_symlinks=False):
                    if not e.name.startswith(".") and e.name not in _SKIP_DIRS:
                        stack.append(e.path)
                elif any(fnmatch.fnmatch(e.name, p) for p in patterns):
                    st = e.stat()
                    out[e.path] = (st.st_mtime_ns, st.st_size)
            except OSError:
                continue  # vanished while scanning
    return out


def changed(before, after):
    """The first path (sorted) that was added, removed or modified between two snapshots, else None."""
    for path in sorted(set(before) | set(after)):
        if before.get(path) != after.get(path):
            return path
    return None


def _stop_child(child, grace):
    if child is None or child.poll() is not None:
        return
    child.terminate()  # SIGTERM: the child drains (graceful stop); TerminateProcess on Windows
    try:
        child.wait(grace + 2)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()


def serve(target, host, port, log_level=None, io_threads=None, shutdown_timeout=None, dirs=None, patterns=None, tls=()):
    """Run `target` ('module:attr') and restart it when a file under `dirs` (default: the current directory) changes."""
    dirs = [os.path.abspath(d) for d in (dirs or ["."])]
    for d in dirs:
        if not os.path.isdir(d):
            raise ValueError(f"reload directory not found: {d}")
    patterns = list(patterns or ["*.py"])
    grace = _CHILD_GRACE if shutdown_timeout is None else shutdown_timeout
    cmd = [sys.executable, "-m", "httpp", "py", "run", target, "--host", host, "--port", str(port), "--shutdown-timeout", str(grace)]
    if log_level:
        cmd += ["--log-level", log_level]
    if io_threads is not None:
        cmd += ["--io-threads", str(io_threads)]
    cmd += list(tls)

    def spawn():
        # Own session on POSIX: Ctrl+C reaches only this supervisor, which stops the child itself.
        return subprocess.Popen(cmd, start_new_session=(os.name == "posix"))

    stop = []
    if threading.current_thread() is threading.main_thread():
        for sig in (signal.SIGINT, signal.SIGTERM):
            signal.signal(sig, lambda *_: stop.append(1))
    _say(f"reloading on changes to {', '.join(patterns)} under {', '.join(dirs)} (supervisor pid {os.getpid()}, Ctrl+C to stop)")
    snap = snapshot(dirs, patterns)
    child = spawn()
    announced_dead = False
    try:
        while not stop:
            time.sleep(_POLL)
            now = snapshot(dirs, patterns)
            what = changed(snap, now)
            if what is not None:
                time.sleep(0.2)  # an editor often writes several files; take them as one change
                snap = snapshot(dirs, patterns)
                _say(f"change detected in {os.path.relpath(what)}; restarting")
                _stop_child(child, grace)
                child = spawn()
                announced_dead = False
            elif child.poll() is not None and not announced_dead:
                _say(f"the app exited with code {child.returncode}; waiting for a change to start it again")
                announced_dead = True
    finally:
        _stop_child(child, grace)
    return 0
