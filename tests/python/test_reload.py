"""httpp run --reload (ADR-0039): restart the app when a source file changes."""

import os
import signal
import socket
import subprocess
import sys
import time

import pytest

import httpp
from httpp import Server
from httpp import _reload

from test_run import child_env, free_port, wait_for

APP = '''
import os
from httpp import Server

app = Server(openapi_url=None)

@app.get("/v")
def v():
    return "{value}" + "|" + str(os.getpid())
'''


def get_v(port):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
        s.sendall(b"GET /v HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
        data = b""
        while chunk := s.recv(4096):
            data += chunk
    body = data.split(b"\r\n\r\n", 1)[1].decode()
    value, pid = body.split("|")
    return value, int(pid)


def write_app(path, value):
    path.write_text(APP.format(value=value))
    future = time.time() + write_app.bump  # a filesystem with coarse mtimes must still see a change
    write_app.bump += 2
    os.utime(path, (future, future))


write_app.bump = 0


def wait_value(port, value, seconds=20):
    end = time.monotonic() + seconds
    last = None
    while time.monotonic() < end:
        try:
            last = get_v(port)
            if last[0] == value:
                return last
        except (OSError, ValueError):
            pass
        time.sleep(0.1)
    raise AssertionError(f"server never answered {value!r} (last: {last})")


# ---------------------------------------------------------------- the watcher

def test_snapshot_sees_matching_files_and_skips_hidden_cache_and_venv_dirs(tmp_path):
    (tmp_path / "a.py").write_text("x")
    (tmp_path / "notes.txt").write_text("x")
    (tmp_path / "pkg").mkdir()
    (tmp_path / "pkg" / "b.py").write_text("x")
    for skipped in (".git", "__pycache__", "node_modules", ".hidden"):
        (tmp_path / skipped).mkdir()
        (tmp_path / skipped / "c.py").write_text("x")
    (tmp_path / "venv").mkdir()
    (tmp_path / "venv" / "pyvenv.cfg").write_text("home = x")
    (tmp_path / "venv" / "d.py").write_text("x")
    snap = _reload.snapshot([str(tmp_path)], ["*.py"])
    assert sorted(os.path.relpath(p, tmp_path) for p in snap) == ["a.py", os.path.join("pkg", "b.py")]
    assert sorted(os.path.relpath(p, tmp_path) for p in _reload.snapshot([str(tmp_path)], ["*.py", "*.txt"])) == [
        "a.py", "notes.txt", os.path.join("pkg", "b.py")]


def test_changed_names_edits_additions_and_removals(tmp_path):
    f = tmp_path / "a.py"
    f.write_text("1")
    before = _reload.snapshot([str(tmp_path)], ["*.py"])
    assert _reload.changed(before, _reload.snapshot([str(tmp_path)], ["*.py"])) is None
    f.write_text("22")  # size differs even if the mtime does not
    assert _reload.changed(before, _reload.snapshot([str(tmp_path)], ["*.py"])) == str(f)
    before = _reload.snapshot([str(tmp_path)], ["*.py"])
    g = tmp_path / "new.py"
    g.write_text("x")
    assert _reload.changed(before, _reload.snapshot([str(tmp_path)], ["*.py"])) == str(g)
    before = _reload.snapshot([str(tmp_path)], ["*.py"])
    g.unlink()
    assert _reload.changed(before, _reload.snapshot([str(tmp_path)], ["*.py"])) == str(g)


# ---------------------------------------------------------------- arguments

def test_run_refuses_reload_without_an_import_string_or_with_workers():
    with pytest.raises(TypeError):
        httpp.run(Server(openapi_url=None), reload=True)
    with pytest.raises(ValueError):
        httpp.run("myapp:app", reload=True, workers=2)
    with pytest.raises(ValueError):
        httpp.run("myapp:app", reload=True, reload_dirs=["/no/such/dir/for/httpp"])


# ---------------------------------------------------------------- the real thing

@pytest.mark.skipif(sys.platform == "win32", reason="SIGTERM")
def test_reload_restarts_on_change_survives_a_broken_file_and_stops_on_sigterm(tmp_path):
    app_file = tmp_path / "myapp.py"
    write_app(app_file, "1")
    port = free_port()
    proc = subprocess.Popen([sys.executable, "-m", "httpp", "run", "myapp:app", "--port", str(port), "--reload"],
                            cwd=tmp_path, env=child_env())
    try:
        first = wait_for(lambda: wait_value(port, "1"))
        write_app(app_file, "2")
        second = wait_value(port, "2")
        assert second[1] != first[1]  # a new process, not the old one patched

        app_file.write_text("this is not python (\n")  # a syntax error: no server, but the watcher lives on
        time.sleep(1.5)
        assert proc.poll() is None
        with pytest.raises(OSError):
            socket.create_connection(("127.0.0.1", port), timeout=1)
        write_app(app_file, "3")
        third = wait_value(port, "3")

        proc.send_signal(signal.SIGTERM)
        assert proc.wait(20) == 0
        with pytest.raises(OSError):
            socket.create_connection(("127.0.0.1", port), timeout=1)
        assert third[1] != proc.pid
    finally:
        if proc.poll() is None:
            proc.kill()


@pytest.mark.skipif(sys.platform == "win32", reason="SIGTERM")
def test_cli_reload_with_workers_is_an_error(tmp_path):
    write_app(tmp_path / "myapp.py", "1")
    r = subprocess.run([sys.executable, "-m", "httpp", "run", "myapp:app", "--reload", "--workers", "2", "--port", str(free_port())],
                       cwd=tmp_path, env=child_env(), capture_output=True, text=True, timeout=30)
    assert r.returncode == 1 and "reload" in r.stderr
