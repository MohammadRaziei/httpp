"""The native `httpp` command line (ADR-0041): server, browse (see also test_file_browser.py), download, curl, `py`, exit codes."""

import json
import os
import socket
import stat
import subprocess
import sys
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Server

import httpp_cli
from pathlib import Path

posix_only = pytest.mark.skipif(os.name == "nt", reason="uses a script as the interpreter")


def run(exe, *args, env=None, timeout=60, input=None):
    full_env = dict(os.environ)
    full_env.setdefault("HTTPP_PYTHON", sys.executable)  # what the launcher sets for the executable (which sits in site-packages, with no Python next to it)
    full_env.update(env or {})
    return subprocess.run([exe, *args], capture_output=True, text=True, timeout=timeout, env=full_env, input=input)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@contextmanager
def running(app):
    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        yield f"http://127.0.0.1:{port}"
    finally:
        app.stop()
        th.join()


def wait_for(url, proc, attempts=100):
    for _ in range(attempts):  # a failed connection is a response with status 0, not an exception
        r = httpp.get(url)
        if r.status != 0:
            return r
        assert proc.poll() is None, proc.stderr.read() if proc.stderr else "exited"
        time.sleep(0.1)
    return r


@pytest.fixture
def echo_app():
    app = Server(openapi_url=None)

    @app.route("/echo", ("GET", "POST", "PUT", "DELETE"))
    def echo(request):
        return json.dumps({"method": request.method, "body": request.body.decode(), "x": request.headers.get("x-test"), "ct": request.headers.get("content-type")})

    @app.get("/text")
    def text():
        return "plain body"

    return app


# --- the executable itself ---------------------------------------------------------------------


def test_version(httpp_exe):
    r = run(httpp_exe, "--version")
    assert r.returncode == 0
    assert r.stdout.strip() == f"httpp {httpp.__version__}", r.stdout


def test_help_lists_the_native_commands_and_py(httpp_exe):
    r = run(httpp_exe, "--help")
    assert r.returncode == 0
    for word in ("server", "browse", "download", "curl", "py", "run", "path"):
        assert word in r.stdout, word


@pytest.mark.parametrize("command", ["server", "browse", "download", "curl"])
def test_every_native_command_has_help(httpp_exe, command):
    r = run(httpp_exe, command, "--help")
    assert r.returncode == 0 and command in r.stdout


def test_an_unknown_command_is_a_usage_error(httpp_exe):
    r = run(httpp_exe, "bogus")
    assert r.returncode == 2 and r.stderr


def test_a_bad_option_value_is_a_usage_error(httpp_exe):
    r = run(httpp_exe, "server", "--port", "notanumber")
    assert r.returncode == 2
    r = run(httpp_exe, "browse", "--theme", "purple")
    assert r.returncode == 2


def test_no_arguments_prints_help_and_fails(httpp_exe):
    r = run(httpp_exe)
    assert r.returncode == 1 and "server" in r.stdout


@pytest.mark.parametrize("old", ["run", "path"])
def test_run_and_path_point_to_py(httpp_exe, old):
    r = run(httpp_exe, old, "--whatever")
    assert r.returncode == 2
    assert f"httpp py {old}" in r.stderr


# --- server ------------------------------------------------------------------------------------


def test_server_serves_a_directory(httpp_exe, tmp_path):
    (tmp_path / "hello.txt").write_text("hi there")
    port = free_port()
    proc = subprocess.Popen([httpp_exe, "server", str(tmp_path), "--host", "127.0.0.1", "--port", str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        r = wait_for(f"http://127.0.0.1:{port}/hello.txt", proc)
        assert (r.status, r.text) == (200, "hi there")
        assert httpp.get(f"http://127.0.0.1:{port}/nope.txt").status == 404
    finally:
        proc.terminate()
        proc.communicate(timeout=20)


def test_server_refuses_a_missing_folder(httpp_exe, tmp_path):
    r = run(httpp_exe, "server", str(tmp_path / "nope"))
    assert r.returncode == 1 and "not a folder" in r.stderr


def test_server_reports_a_port_that_is_taken(httpp_exe, tmp_path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        r = run(httpp_exe, "server", str(tmp_path), "--host", "127.0.0.1", "--port", str(s.getsockname()[1]))
    assert r.returncode == 1 and "cannot listen" in r.stderr


@posix_only
@pytest.mark.parametrize("command", ["server", "browse"])
def test_ctrl_c_and_sigterm_stop_the_server_cleanly(httpp_exe, tmp_path, command):
    import signal

    for sig in (signal.SIGINT, signal.SIGTERM):
        port = free_port()
        proc = subprocess.Popen([httpp_exe, command, str(tmp_path), "--host", "127.0.0.1", "--port", str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            assert wait_for(f"http://127.0.0.1:{port}/", proc).status != 0
            proc.send_signal(sig)
            assert proc.wait(timeout=20) == 0
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.communicate()


# --- download ----------------------------------------------------------------------------------


def test_download_to_a_file(httpp_exe, tmp_path):
    (tmp_path / "src").mkdir()
    payload = bytes(range(256)) * 400
    (tmp_path / "src" / "f.bin").write_bytes(payload)
    app = Server(openapi_url=None)
    app.serve_directory("/files", str(tmp_path / "src"))
    with running(app) as url:
        out = tmp_path / "copy.bin"
        r = run(httpp_exe, "download", f"{url}/files/f.bin", "-o", str(out), "-q")
        assert r.returncode == 0, r.stderr
        assert out.read_bytes() == payload
        assert not (tmp_path / "copy.bin.part").exists()


def test_download_to_stdout(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "download", f"{url}/text")
    assert r.returncode == 0 and r.stdout == "plain body\n"


def test_download_failure_exits_1(httpp_exe, echo_app, tmp_path):
    with running(echo_app) as url:
        r = run(httpp_exe, "download", f"{url}/does-not-exist")
        assert r.returncode == 1 and "status 404" in r.stderr
        r = run(httpp_exe, "download", f"{url}/does-not-exist", "-o", str(tmp_path / "x"), "-q")
        assert r.returncode == 1 and r.stderr.startswith("error:")
    assert not (tmp_path / "x").exists()


def test_download_needs_a_url(httpp_exe):
    assert run(httpp_exe, "download").returncode == 2


# --- curl --------------------------------------------------------------------------------------


def test_curl_get(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/echo")
    assert r.returncode == 0
    assert json.loads(r.stdout)["method"] == "GET"


def test_curl_data_defaults_to_post_and_headers_are_sent(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/echo", "-d", "a=1&b=two words", "-H", "X-Test:  hello ")
    assert r.returncode == 0, r.stderr
    got = json.loads(r.stdout)
    assert got["method"] == "POST" and got["body"] == "a=1&b=two words" and got["x"] == "hello"
    assert got["ct"] == "application/x-www-form-urlencoded"


def test_curl_explicit_method(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/echo", "-X", "PUT", "-d", "")
    assert r.returncode == 0, r.stderr
    got = json.loads(r.stdout)
    assert got["method"] == "PUT" and got["body"] == ""


def test_curl_repeated_headers_and_long_options(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/echo", "--request", "DELETE", "--header", "X-Test: one", "--header", "X-Other: two")
    assert r.returncode == 0, r.stderr
    got = json.loads(r.stdout)
    assert got["method"] == "DELETE" and got["x"] == "one"


def test_curl_output_file(httpp_exe, echo_app, tmp_path):
    out = tmp_path / "body.txt"
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/text", "-o", str(out))
    assert r.returncode == 0 and r.stdout == ""
    assert out.read_text() == "plain body"


def test_curl_malformed_header_exits_1(httpp_exe):
    r = run(httpp_exe, "curl", "http://127.0.0.1:1/", "-H", "NoColonHere")
    assert r.returncode == 1 and "malformed header" in r.stderr


def test_curl_a_failed_request_exits_1(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/nothing-here")
    assert r.returncode == 1 and "status 404" in r.stderr
    r = run(httpp_exe, "curl", f"http://127.0.0.1:{free_port()}/")
    assert r.returncode == 1 and "status 0" in r.stderr


# --- py: the Python half -----------------------------------------------------------------------


def test_py_path_matches_the_package(httpp_exe):
    r = run(httpp_exe, "py", "path")
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines() == [f"include: {httpp.get_include_dir()}", f"lib:     {httpp.get_lib_dir()}", f"cmake:   {httpp.get_cmake_dir()}"]
    r = run(httpp_exe, "py", "path", "--cmake-dir")
    assert r.stdout == f"{httpp.get_cmake_dir()}\n"
    r = run(httpp_exe, "py", "--include-dir")
    assert r.stdout == f"{httpp.get_include_dir()}\n"


def test_py_help_is_the_python_help(httpp_exe):
    r = run(httpp_exe, "py", "--help")
    assert r.returncode == 0 and "usage: httpp py" in r.stdout
    r = run(httpp_exe, "py", "run", "--help")
    assert r.returncode == 0 and "--reload" in r.stdout and "--ssl-certfile" in r.stdout and "--shutdown-timeout" in r.stdout


def test_py_forwards_the_exit_code(httpp_exe, tmp_path):
    r = run(httpp_exe, "py", "run", "no_such_module:app", "--port", str(free_port()))
    assert r.returncode == 1 and "httpp: error" in r.stderr
    r = run(httpp_exe, "py", "run", "main:app", "--workers", "many")
    assert r.returncode == 2


def test_py_without_a_command_is_a_usage_error(httpp_exe):
    r = run(httpp_exe, "py")
    assert r.returncode == 2 and "usage: httpp py" in r.stderr


def test_py_runs_an_app(httpp_exe, tmp_path):
    (tmp_path / "myapp.py").write_text('from httpp import Server\napp = Server(openapi_url=None)\n\n@app.get("/")\ndef index():\n    return "from py run"\n')
    port = free_port()
    proc = subprocess.Popen([httpp_exe, "py", "run", "myapp:app", "--port", str(port), "--log-level", "warning"], cwd=tmp_path, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=dict(os.environ, HTTPP_PYTHON=sys.executable))
    try:
        r = wait_for(f"http://127.0.0.1:{port}/", proc, attempts=200)
        assert (r.status, r.text) == (200, "from py run")
    finally:
        proc.terminate()
        proc.communicate(timeout=30)


@posix_only
def test_py_passes_every_argument_unchanged(httpp_bin, tmp_path):
    """Spaces, quotes, empty strings, things that look like options, and --help all reach the interpreter as they were typed."""
    fake = tmp_path / "fakepython"
    fake.write_text(f"#!{sys.executable}\nimport json, sys\nprint(json.dumps(sys.argv[1:]))\nsys.exit(7)\n")
    fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
    args = ["run", "main:app", "two words", 'has"quote', "it's", "", "--port", "--help", "-h", "--version", "$HOME", "*", "ünï"]
    r = run(httpp_bin, "py", *args, env={"HTTPP_PYTHON": str(fake)})
    assert r.returncode == 7  # the interpreter's exit code is ours
    assert json.loads(r.stdout) == ["-m", "httpp", *args]


@posix_only
def test_a_bad_httpp_python_is_reported(httpp_bin, tmp_path):
    r = run(httpp_bin, "py", "path", env={"HTTPP_PYTHON": str(tmp_path / "nope")})
    assert r.returncode == 1 and "HTTPP_PYTHON" in r.stderr


@posix_only
def test_without_a_python_the_error_says_what_to_do(httpp_bin, tmp_path):
    empty = tmp_path / "empty"
    empty.mkdir()
    # a copy of the executable in a folder without a Python next to it, and an empty PATH
    copy = tmp_path / "httpp"
    copy.write_bytes(open(httpp_bin, "rb").read())
    copy.chmod(0o755)
    env = dict(os.environ, PATH=str(empty), LD_LIBRARY_PATH=httpp.get_lib_dir(), DYLD_LIBRARY_PATH=httpp.get_lib_dir())
    env.pop("HTTPP_PYTHON", None)
    r = subprocess.run([str(copy), "py", "path"], capture_output=True, text=True, timeout=30, env=env)
    assert r.returncode == 1 and "need a Python" in r.stderr, (r.returncode, r.stdout, r.stderr)


# --- the launcher (httpp_cli.py) and `python -m httpp` -------------------------------------


def test_the_launcher_finds_the_executable_in_the_package():
    exe = httpp_cli.binary_path()
    assert exe.parent.name == "bin" and exe.parent.parent == Path(httpp.__file__).resolve().parent
    assert exe.parent.parent.joinpath("lib").is_dir()  # the executable's RPATH is ../lib


def test_importing_the_launcher_does_not_import_the_package():
    """Why httpp_cli.py is next to the package and not in it: starting a native command must not load the compiled extension."""
    code = "import sys, httpp_cli; sys.exit(1 if 'httpp' in sys.modules else 0)"
    assert subprocess.run([sys.executable, "-I", "-c", code], timeout=60).returncode == 0


def test_the_launcher_answers_run_and_path_without_py(capsys):
    assert httpp_cli.main(["run", "main:app"]) == 2
    assert "httpp py run" in capsys.readouterr().err
    assert httpp_cli.main(["path"]) == 2
    assert httpp_cli.main(["py"]) == 2
    assert "usage: httpp py" in capsys.readouterr().err


def test_the_launcher_runs_py_commands_in_process(capsys):
    assert httpp_cli.main(["py", "path", "--cmake-dir"]) == 0
    assert capsys.readouterr().out == f"{httpp.get_cmake_dir()}\n"


def test_the_launcher_reports_a_missing_executable(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(httpp_cli, "binary_path", lambda: tmp_path / "bin" / "httpp")
    assert httpp_cli.run_native(["--version"]) == 1
    assert "missing" in capsys.readouterr().err


@posix_only
def test_the_launcher_restores_a_lost_execute_bit(httpp_command, httpp_bin):
    exe = Path(httpp_bin)
    mode = exe.stat().st_mode
    try:
        exe.chmod(mode & ~(stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))
        assert not os.access(exe, os.X_OK)
        r = run(httpp_command, "--version")  # not run as root-with-bypass: access() above says it cannot be executed
        assert r.returncode == 0 and r.stdout.startswith("httpp "), r.stderr
        assert os.access(exe, os.X_OK)
    finally:
        exe.chmod(mode)


@posix_only
def test_the_launcher_says_when_it_cannot_make_it_executable(monkeypatch, tmp_path, capsys):
    exe = tmp_path / "bin" / "httpp"
    exe.parent.mkdir()
    exe.write_bytes(b"x")
    exe.chmod(0o644)
    monkeypatch.setattr(httpp_cli, "binary_path", lambda: exe)

    def refuse(self, mode):
        raise PermissionError("read-only file system")

    monkeypatch.setattr(Path, "chmod", refuse)
    assert httpp_cli.run_native([]) == 1
    assert "cannot be made" in capsys.readouterr().err


@pytest.mark.parametrize("native", ["server", "browse", "download", "curl"])
def test_python_dash_m_httpp_runs_the_native_commands_too(native):
    r = subprocess.run([sys.executable, "-m", "httpp", native, "--help"], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0 and native in r.stdout


def test_python_dash_m_httpp_keeps_run_and_path():
    r = subprocess.run([sys.executable, "-m", "httpp", "path", "--cmake-dir"], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0 and r.stdout == f"{httpp.get_cmake_dir()}\n"
