"""httpp.run / `httpp run`: import strings, graceful stop, workers sharing a port."""

import os
import signal
import socket
import subprocess
import sys
import time

import pytest

import httpp
from httpp import Server
from httpp._run import load

APP = '''
import os
from httpp import Server

app = Server()

@app.on_shutdown
def bye():
    open(os.environ["BYE_DIR"] + "/bye." + str(os.getpid()), "w").write("graceful")

@app.get("/pid")
def pid():
    return str(os.getpid())
'''


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def get_pid(port):
    """GET /pid on a fresh connection (so the kernel picks a worker each time)."""
    with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
        s.sendall(b"GET /pid HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        data = b""
        while chunk := s.recv(4096):
            data += chunk
    return int(data.split(b"\r\n\r\n", 1)[1])


@pytest.fixture
def app_dir(tmp_path):
    (tmp_path / "myapp.py").write_text(APP)
    return tmp_path


def child_env(**extra):
    """Environment for a child `python -m httpp`, which runs in another cwd: point it at THIS httpp."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(httpp.__file__)))
    return dict(os.environ, PYTHONPATH=root + os.pathsep + os.environ.get("PYTHONPATH", ""), **extra)


def start(app_dir, *args):
    env = child_env(BYE_DIR=str(app_dir))
    return subprocess.Popen([sys.executable, "-m", "httpp", "run", "myapp:app", *args], cwd=app_dir, env=env)


def wait_for(fn, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            return fn()
        except OSError:
            time.sleep(0.1)
    raise AssertionError("server did not come up")


def test_load_resolves_module_and_dotted_attribute(app_dir, monkeypatch):
    monkeypatch.syspath_prepend(str(app_dir))
    assert isinstance(load("myapp:app"), Server)
    assert load("myapp:app.get") == load("myapp:app").get or callable(load("myapp:app.get"))


@pytest.mark.parametrize("target", ["myapp", ":app", "myapp:", "", "nope_such_module:app", "myapp:missing"])
def test_load_refuses_bad_targets_with_a_short_message(app_dir, monkeypatch, target):
    monkeypatch.syspath_prepend(str(app_dir))
    with pytest.raises(ValueError):
        load(target)


def test_run_validates_its_arguments():
    app = Server()
    for bad in (dict(port=0), dict(port=70000), dict(port="80"), dict(host=""), dict(workers=0)):
        with pytest.raises(ValueError):
            httpp.run(app, **bad)
    with pytest.raises(TypeError):
        httpp.run(object())
    with pytest.raises(TypeError):  # workers need an import string
        httpp.run(app, workers=2)


@pytest.mark.skipif(sys.platform == "win32", reason="SIGTERM")
def test_cli_run_serves_and_stops_gracefully_on_sigterm(app_dir):
    port = free_port()
    proc = start(app_dir, "--port", str(port))
    try:
        pid = wait_for(lambda: get_pid(port))
        assert pid == proc.pid
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(15) == 0
        assert (app_dir / f"bye.{pid}").read_text() == "graceful"  # on_shutdown ran
    finally:
        if proc.poll() is None:
            proc.kill()


@pytest.mark.skipif(sys.platform == "win32", reason="SIGTERM")
@pytest.mark.parametrize("args, logged", [((), True), (("--log-level", "info"), True), (("--log-level", "warning"), False)])
def test_cli_run_logs_each_request_unless_the_level_is_raised(app_dir, args, logged):
    port = free_port()
    proc = subprocess.Popen([sys.executable, "-m", "httpp", "run", "myapp:app", "--port", str(port), *args],
                            cwd=app_dir, env=child_env(BYE_DIR=str(app_dir)), stderr=subprocess.PIPE, text=True)
    try:
        wait_for(lambda: get_pid(port))
        proc.send_signal(signal.SIGTERM)
        err = proc.communicate(timeout=15)[1]
    finally:
        if proc.poll() is None:
            proc.kill()
    assert (f"listening on http://127.0.0.1:{port}" in err) == logged
    assert ("GET /pid 200" in err) == logged


# A file that also starts itself at import time (no __main__ guard): `httpp run` must win.
SELF_STARTING = APP + '''
app.listen("127.0.0.1", 1)  # a port nobody can bind: if this ran, the server would die or sit on it
'''


@pytest.mark.parametrize("workers", [1, 2])
def test_cli_run_wins_over_a_listen_call_at_import_time(app_dir, workers):
    if workers > 1 and not sys.platform.startswith("linux"):
        pytest.skip("SO_REUSEPORT balancing is Linux")
    (app_dir / "myapp.py").write_text(SELF_STARTING)
    port = free_port()
    proc = subprocess.Popen([sys.executable, "-m", "httpp", "run", "myapp:app", "--port", str(port), "--workers", str(workers)],
                            cwd=app_dir, env=child_env(BYE_DIR=str(app_dir)), stderr=subprocess.PIPE, text=True)
    try:
        assert wait_for(lambda: get_pid(port))  # served on run's port, not the one in the file
        proc.send_signal(signal.SIGTERM)
        err = proc.communicate(timeout=15)[1]
        if sys.platform != "win32":  # there SIGTERM is a hard kill, so the exit code says nothing
            assert proc.returncode == 0
    finally:
        if proc.poll() is None:
            proc.kill()
    assert "ignoring app.listen('127.0.0.1', 1)" in err  # the person is told, not left guessing
    assert "if __name__" in err


def test_cli_run_reports_a_bad_target_and_exits_1(app_dir):
    r = subprocess.run([sys.executable, "-m", "httpp", "run", "myapp:missing"], cwd=app_dir, env=child_env(), capture_output=True, text=True)
    assert r.returncode == 1 and "no attribute 'missing'" in r.stderr and "Traceback" not in r.stderr


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="SO_REUSEPORT balancing is Linux")
def test_workers_share_one_port_and_all_stop_gracefully(app_dir):
    port = free_port()
    sup = start(app_dir, "--port", str(port), "--workers", "3")
    try:
        seen = set()

        def spread():
            for _ in range(40):
                seen.add(get_pid(port))
            if len(seen) < 3:
                raise OSError("not all workers are up yet")
            return seen

        wait_for(spread)
        assert sup.pid not in seen and len(seen) == 3
        sup.send_signal(signal.SIGTERM)
        assert sup.wait(20) == 0
        for pid in seen:
            assert (app_dir / f"bye.{pid}").read_text() == "graceful"
    finally:
        if sup.poll() is None:
            sup.kill()


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="SO_REUSEPORT balancing is Linux")
def test_a_dead_worker_stops_the_group_with_exit_1(app_dir):
    port = free_port()
    with socket.socket() as blocker:  # a plain listener on the port: every worker fails to bind
        blocker.bind(("127.0.0.1", port))
        blocker.listen()
        sup = start(app_dir, "--port", str(port), "--workers", "2")
        assert sup.wait(30) == 1


def test_free_threaded_build_keeps_the_gil_off_after_importing_httpp():
    """The extension declares itself free-threading compatible; otherwise a free-threaded
    interpreter silently turns the GIL back on at import and the wheel is pointless."""
    import sysconfig

    if not sysconfig.get_config_var("Py_GIL_DISABLED") or os.environ.get("PYTHON_GIL") == "1":
        pytest.skip("not a free-threaded interpreter, or the GIL was forced on")
    assert not sys._is_gil_enabled()


# ---- --workers auto ------------------------------------------------------------------------------


def test_cpu_count_honours_the_affinity_mask_and_a_container_cpu_limit(monkeypatch, tmp_path):
    from httpp import _run

    monkeypatch.setattr(os, "sched_getaffinity", lambda pid: set(range(8)), raising=False)
    monkeypatch.setattr(_run, "_CGROUP_V2_CPU_MAX", str(tmp_path / "missing"))
    monkeypatch.setattr(_run, "_CGROUP_V1_QUOTA", str(tmp_path / "missing"))
    monkeypatch.setattr(_run, "_CGROUP_V1_PERIOD", str(tmp_path / "missing"))
    assert _run._cpu_count() == 8  # no limit file: the affinity mask decides

    v2 = tmp_path / "cpu.max"
    monkeypatch.setattr(_run, "_CGROUP_V2_CPU_MAX", str(v2))
    v2.write_text("200000 100000\n")
    assert _run._cpu_count() == 2  # two CPUs of quota on an 8-CPU host
    v2.write_text("150000 100000\n")
    assert _run._cpu_count() == 1  # 1.5 CPUs: round down, so the quota does not throttle the workers
    v2.write_text("50000 100000\n")
    assert _run._cpu_count() == 1  # never below one
    v2.write_text("max 100000\n")
    assert _run._cpu_count() == 8  # "max" means no limit
    v2.write_text("garbage")
    assert _run._cpu_count() == 8  # unreadable: ignore it rather than crash

    v2.unlink()
    (tmp_path / "quota").write_text("400000\n")
    (tmp_path / "period").write_text("100000\n")
    monkeypatch.setattr(_run, "_CGROUP_V1_QUOTA", str(tmp_path / "quota"))
    monkeypatch.setattr(_run, "_CGROUP_V1_PERIOD", str(tmp_path / "period"))
    assert _run._cpu_count() == 4  # cgroup v1
    (tmp_path / "quota").write_text("-1\n")
    assert _run._cpu_count() == 8  # -1 means no limit


def test_cpu_count_without_sched_getaffinity(monkeypatch):
    from httpp import _run

    monkeypatch.delattr(os, "sched_getaffinity", raising=False)
    monkeypatch.setattr(os, "cpu_count", lambda: 6)
    monkeypatch.setattr(_run, "_cgroup_cpu_limit", lambda: None)
    assert _run._cpu_count() == 6
    monkeypatch.setattr(os, "cpu_count", lambda: None)
    assert _run._cpu_count() == 1


def test_resolve_workers(monkeypatch):
    from httpp import _run

    monkeypatch.setattr(_run, "_cpu_count", lambda: 4)
    monkeypatch.setattr(sys, "platform", "linux")
    assert _run._resolve_workers("auto") == 4
    assert _run._resolve_workers(3) == 3
    monkeypatch.setattr(sys, "platform", "darwin")
    assert _run._resolve_workers("auto") == 1  # processes sharing a port are Linux-only
    assert _run._resolve_workers(3) == 3  # an explicit number is the caller's call (run() then refuses it)
    for bad in (0, -1, "many", "AUTO ", 2.5, None):
        with pytest.raises(ValueError, match="workers"):
            _run._resolve_workers(bad)


def test_workers_auto_with_an_app_object_runs_one_process_and_serves(monkeypatch):
    from httpp import _run

    monkeypatch.setattr(sys, "platform", "linux")
    monkeypatch.setattr(_run, "_cpu_count", lambda: 4)  # would mean 4 processes, which an object cannot do
    app = Server()
    seen = []
    monkeypatch.setattr(app, "listen", lambda host, port: seen.append((host, port)))
    httpp.run(app, port=free_port(), workers="auto")
    assert len(seen) == 1


def test_cli_accepts_workers_auto_and_rejects_nonsense(app_dir):
    out = subprocess.run([sys.executable, "-m", "httpp", "run", "myapp:app", "--workers", "many"], cwd=app_dir,
                         env=child_env(BYE_DIR=str(app_dir)), capture_output=True, text=True, timeout=30)
    assert out.returncode == 2 and "workers" in out.stderr


@pytest.mark.skipif(sys.platform == "win32", reason="SIGTERM")
def test_cli_workers_auto_serves_and_stops_gracefully(app_dir):
    port = free_port()
    sup = start(app_dir, "--port", str(port), "--workers", "auto")
    try:
        assert wait_for(lambda: get_pid(port)) > 0
        sup.send_signal(signal.SIGTERM)
        assert sup.wait(20) == 0
    finally:
        if sup.poll() is None:
            sup.kill()
