"""The native `httpp` command line (ADR-0041): server, browse (see also test_file_browser.py), download, curl, `py`, exit codes."""

import base64
import http.client
import json
import os
import socket
import ssl
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
from test_tls import certs  # noqa: F401  (fixture: a self-signed certificate, its key, and a key that does not belong to it)

posix_only = pytest.mark.skipif(os.name == "nt", reason="uses a script as the interpreter")


def same(text):
    """Paths as text, compared the way the file system does (Windows ignores case: venv\\Lib or venv\\lib, the same folder)."""
    return os.path.normcase(text)


def run(exe, *args, env=None, timeout=60, input=None):
    exe = [exe] if isinstance(exe, str) else list(exe)
    full_env = dict(os.environ)
    full_env.setdefault("HTTPP_PYTHON", sys.executable)  # what the launcher sets for the executable (which sits in site-packages, with no Python next to it)
    full_env.update(env or {})
    return subprocess.run([*exe, *args], capture_output=True, text=True, timeout=timeout, env=full_env, input=input)


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
        return json.dumps({"method": request.method, "body": request.body.decode(), "x": request.headers.get("x-test"), "ct": request.headers.get("content-type"), "auth": request.headers.get("authorization")})

    @app.get("/text")
    def text():
        return "plain body"

    @app.get("/old")
    def old():
        return httpp.redirect("/text")

    @app.get("/slow")
    def slow():
        time.sleep(2)
        return "late"

    return app


# --- the executable itself ---------------------------------------------------------------------


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


def test_server_serves_a_directory(httpp_exe, tmp_path, stop_process):
    (tmp_path / "hello.txt").write_text("hi there")
    port = free_port()
    proc = subprocess.Popen([*httpp_exe, "server", str(tmp_path), "--host", "127.0.0.1", "--port", str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        r = wait_for(f"http://127.0.0.1:{port}/hello.txt", proc)
        assert (r.status, r.text) == (200, "hi there")
        assert httpp.get(f"http://127.0.0.1:{port}/nope.txt").status == 404
    finally:
        stop_process(proc)


@pytest.mark.parametrize("command", ["server", "browse"])
def test_the_native_commands_log_the_address_and_each_request(httpp_exe, tmp_path, stop_process, command):
    port = free_port()
    proc = subprocess.Popen([*httpp_exe, command, str(tmp_path), "--host", "127.0.0.1", "--port", str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert wait_for(f"http://127.0.0.1:{port}/", proc).status != 0
        assert httpp.get(f"http://127.0.0.1:{port}/nope.txt").status == 404
    finally:
        _, err = stop_process(proc)
    assert f"listening on http://127.0.0.1:{port}" in err, err
    assert "GET /nope.txt 404" in err, err


# --- TLS (server and browse, the same options as `httpp run`) -----------------------------------


def _https_get(port, cafile, path="/"):
    conn = http.client.HTTPSConnection("127.0.0.1", port, context=ssl.create_default_context(cafile=cafile), timeout=5)
    try:
        conn.request("GET", path)
        r = conn.getresponse()
        return r.status, r.read()
    finally:
        conn.close()


def _wait_for_port(port, proc, attempts=100):
    for _ in range(attempts):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return
        except OSError:
            assert proc.poll() is None, proc.stderr.read() if proc.stderr else "exited"
            time.sleep(0.1)
    raise AssertionError("the server never listened")


@pytest.fixture
def encrypted_key(certs, tmp_path):  # noqa: F811
    """The test key again, encrypted with AES under the password 'pw'."""
    out = str(tmp_path / "encrypted_key.pem")
    subprocess.run(["openssl", "pkey", "-in", certs[1], "-aes256", "-passout", "pass:pw", "-out", out], check=True, capture_output=True)
    return out


@pytest.mark.parametrize("command", ["server", "browse"])
def test_the_native_commands_serve_https_when_given_a_certificate(httpp_exe, tmp_path, stop_process, certs, command):  # noqa: F811
    cert, key, _ = certs
    (tmp_path / "hello.txt").write_text("hi there")
    page = "/hello.txt" if command == "server" else "/"  # server has no folder page; browse answers / with one
    port = free_port()
    proc = subprocess.Popen([*httpp_exe, command, str(tmp_path), "--host", "127.0.0.1", "--port", str(port), "--ssl-certfile", cert, "--ssl-keyfile", key],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        _wait_for_port(port, proc)
        assert _https_get(port, cert, page)[0] == 200
        assert httpp.get(f"http://127.0.0.1:{port}{page}").status != 200  # one port serves TLS or plain HTTP, never both
    finally:
        out, err = stop_process(proc)
    assert f"https://127.0.0.1:{port}/" in out, out
    assert f"listening on https://127.0.0.1:{port}" in err, err


def test_server_over_https_serves_the_files(httpp_exe, tmp_path, stop_process, certs):  # noqa: F811
    cert, key, _ = certs
    (tmp_path / "hello.txt").write_text("hi there")
    port = free_port()
    proc = subprocess.Popen([*httpp_exe, "server", str(tmp_path), "--host", "127.0.0.1", "--port", str(port), "--ssl-certfile", cert, "--ssl-keyfile", key],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        _wait_for_port(port, proc)
        assert _https_get(port, cert, "/hello.txt") == (200, b"hi there")
        assert _https_get(port, cert, "/nope.txt")[0] == 404
    finally:
        stop_process(proc)


@pytest.mark.parametrize("how", ["argument", "environment"])
def test_an_encrypted_key_takes_its_password_from_an_argument_or_the_environment(httpp_exe, tmp_path, stop_process, certs, encrypted_key, how):  # noqa: F811
    cert = certs[0]
    (tmp_path / "hello.txt").write_text("hi there")
    port = free_port()
    flags = ["--ssl-keyfile-password", "pw"] if how == "argument" else []
    env = {**os.environ, **({"HTTPP_SSL_KEYFILE_PASSWORD": "pw"} if how == "environment" else {})}
    proc = subprocess.Popen([*httpp_exe, "server", str(tmp_path), "--host", "127.0.0.1", "--port", str(port), "--ssl-certfile", cert, "--ssl-keyfile", encrypted_key, *flags],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    try:
        _wait_for_port(port, proc)
        assert _https_get(port, cert, "/hello.txt") == (200, b"hi there")
    finally:
        stop_process(proc)


@pytest.mark.parametrize("command", ["server", "browse"])
def test_the_certificate_and_the_key_come_together(httpp_exe, tmp_path, certs, command):  # noqa: F811
    cert, key, _ = certs
    for flags, missing in ((["--ssl-certfile", cert], "--ssl-keyfile"), (["--ssl-keyfile", key], "--ssl-certfile")):
        r = run(httpp_exe, command, str(tmp_path), *flags)
        assert r.returncode == 2 and f"requires {missing}" in r.stderr, r.stderr


def test_a_missing_certificate_file_is_a_usage_error(httpp_exe, tmp_path, certs):  # noqa: F811
    r = run(httpp_exe, "server", str(tmp_path), "--ssl-certfile", str(tmp_path / "nope.pem"), "--ssl-keyfile", certs[1])
    assert r.returncode == 2 and "nope.pem" in r.stderr, r.stderr


@pytest.mark.parametrize("command", ["server", "browse"])
def test_a_key_that_does_not_belong_to_the_certificate_fails_naming_the_file(httpp_exe, tmp_path, certs, command):  # noqa: F811
    cert, _, other = certs
    r = run(httpp_exe, command, str(tmp_path), "--ssl-certfile", cert, "--ssl-keyfile", other)
    assert r.returncode == 1 and "does not match" in r.stderr and os.path.basename(other) in r.stderr, r.stderr


def test_an_encrypted_key_without_its_password_says_so_and_a_wrong_one_fails(httpp_exe, tmp_path, certs, encrypted_key):  # noqa: F811
    cert = certs[0]
    r = run(httpp_exe, "server", str(tmp_path), "--ssl-certfile", cert, "--ssl-keyfile", encrypted_key, env={"HTTPP_SSL_KEYFILE_PASSWORD": ""})
    assert r.returncode == 1 and "encrypted" in r.stderr and "no password" in r.stderr, r.stderr
    r = run(httpp_exe, "server", str(tmp_path), "--ssl-certfile", cert, "--ssl-keyfile", encrypted_key, "--ssl-keyfile-password", "wrong")
    assert r.returncode == 1 and "password" in r.stderr, r.stderr


@pytest.mark.parametrize("command", ["server", "browse"])
def test_the_native_commands_stay_quiet_with_log_level_off(httpp_exe, tmp_path, stop_process, command):
    port = free_port()
    proc = subprocess.Popen([*httpp_exe, command, str(tmp_path), "--host", "127.0.0.1", "--port", str(port), "--log-level", "off"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert wait_for(f"http://127.0.0.1:{port}/", proc).status != 0
        assert httpp.get(f"http://127.0.0.1:{port}/nope.txt").status == 404
    finally:
        _, err = stop_process(proc)
    assert "GET /nope.txt" not in err and "listening on" not in err, err


def test_a_bad_log_level_is_a_usage_error(httpp_exe):
    assert run(httpp_exe, "server", "--log-level", "loud").returncode == 2
    assert run(httpp_exe, "browse", "--log-level", "loud").returncode == 2


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
def _serve(httpp_exe, directory, *flags):
    port = free_port()
    proc = subprocess.Popen([*httpp_exe, "server", str(directory), "--host", "127.0.0.1", "--port", str(port), *flags],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return proc, f"http://127.0.0.1:{port}"


def test_server_hides_names_that_start_with_a_dot_unless_hidden_is_given(httpp_exe, tmp_path, stop_process):
    (tmp_path / "hello.txt").write_text("hi")
    (tmp_path / ".env").write_text("SECRET=1")
    (tmp_path / ".well-known").mkdir()
    (tmp_path / ".well-known" / "acme").write_text("token")
    proc, url = _serve(httpp_exe, tmp_path)
    try:
        assert wait_for(f"{url}/hello.txt", proc).status == 200
        assert httpp.get(f"{url}/.env").status == 404
        assert httpp.get(f"{url}/.well-known/acme").status == 404
    finally:
        stop_process(proc)
    proc, url = _serve(httpp_exe, tmp_path, "--hidden")
    try:
        assert wait_for(f"{url}/hello.txt", proc).status == 200
        assert httpp.get(f"{url}/.env").text == "SECRET=1"
        assert httpp.get(f"{url}/.well-known/acme").text == "token"
    finally:
        stop_process(proc)


def test_server_no_dates_sends_no_file_time(httpp_exe, tmp_path, stop_process):
    (tmp_path / "hello.txt").write_text("hi")
    for flags, validators in (((), True), (("--no-dates",), False)):
        proc, url = _serve(httpp_exe, tmp_path, *flags)
        try:
            r = wait_for(f"{url}/hello.txt", proc)
            assert r.status == 200
            assert (r.header("ETag") is not None) == validators and (r.header("Last-Modified") is not None) == validators
        finally:
            stop_process(proc)


@posix_only
@pytest.mark.parametrize("command", ["server", "browse"])
def test_the_banner_is_for_a_terminal_only(httpp_exe, tmp_path, command, stop_process):
    import pty
    import select

    art = "|_||_\\__|\\__| .__/ .__/"

    def started(extra, tty):
        port = free_port()
        args = [*httpp_exe, command, str(tmp_path), "--port", str(port), *extra]
        if tty:
            master, slave = pty.openpty()
            proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=slave, close_fds=True)
            os.close(slave)
        else:
            master, proc = None, subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for _ in range(100):  # wait until it listens
            if httpp.get(f"http://127.0.0.1:{port}/").status != 0:
                break
            time.sleep(0.1)
        text = ""
        if tty:
            while select.select([master], [], [], 0.3)[0]:
                chunk = os.read(master, 4096)
                if not chunk:
                    break
                text += chunk.decode(errors="replace")
            os.close(master)
        _, err = stop_process(proc)
        if tty:
            return text
        return err.decode(errors="replace") if isinstance(err, bytes) else err

    assert art in started([], tty=True)                        # a terminal gets it
    assert art not in started([], tty=False)                   # a pipe or a log file does not
    assert art not in started(["--no-banner"], tty=True)
    assert art not in started(["--log-level", "off"], tty=True)


@pytest.mark.parametrize("command", ["server", "browse"])
def test_ctrl_c_and_sigterm_stop_the_server_cleanly(httpp_exe, tmp_path, command):
    import signal

    for sig in (signal.SIGINT, signal.SIGTERM):
        port = free_port()
        proc = subprocess.Popen([*httpp_exe, command, str(tmp_path), "--host", "127.0.0.1", "--port", str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
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
    assert r.returncode == 0 and r.stdout == "plain body"


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


def test_curl_prints_the_body_byte_for_byte(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/text")
    assert r.returncode == 0 and r.stdout == "plain body"  # no newline added, like curl


def test_curl_i_prints_status_and_headers_before_the_body(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/text", "-i")
    assert r.returncode == 0, r.stderr
    out = r.stdout.replace("\r\n", "\n")  # subprocess text mode turns CRLF into LF on Windows only
    head, _, body = out.partition("\n\n")
    assert head.splitlines()[0] == "HTTP/1.1 200"
    assert "content-type" in head.lower()
    assert body == "plain body"


def test_curl_capital_i_sends_head_and_prints_only_the_headers(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/text", "-I")
    assert r.returncode == 0, r.stderr
    out = r.stdout.replace("\r\n", "\n")
    assert out.splitlines()[0].startswith("HTTP/1.1 200")
    assert "plain body" not in out
    assert out.endswith("\n\n")  # the empty line that ends the head


def test_curl_v_shows_the_exchange_on_stderr_and_keeps_stdout_clean(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/echo", "-v", "-H", "X-Test: seen")
    assert r.returncode == 0, r.stderr
    assert json.loads(r.stdout)["x"] == "seen"
    assert "> GET /echo" in r.stderr and "> X-Test: seen" in r.stderr
    assert "< HTTP/1.1 200" in r.stderr and "< content-type" in r.stderr.lower()


def test_curl_follows_redirects_only_with_l(httpp_exe, echo_app):
    with running(echo_app) as url:
        plain = run(httpp_exe, "curl", f"{url}/old", "-i")
        followed = run(httpp_exe, "curl", f"{url}/old", "-L")
        long_form = run(httpp_exe, "curl", f"{url}/old", "--location")
    assert plain.stdout.startswith("HTTP/1.1 302") and "location" in plain.stdout.lower()
    assert followed.returncode == 0 and followed.stdout == "plain body"
    assert long_form.stdout == "plain body"


def test_curl_max_time_stops_a_slow_server(httpp_exe, echo_app):
    with running(echo_app) as url:
        started = time.monotonic()
        r = run(httpp_exe, "curl", f"{url}/slow", "-m", "1")
        elapsed = time.monotonic() - started
    assert r.returncode == 1 and "status 0" in r.stderr
    assert elapsed < 2


def test_curl_max_time_must_be_a_number(httpp_exe):
    assert run(httpp_exe, "curl", "http://127.0.0.1:1/", "-m", "soon").returncode == 2


def test_curl_u_sends_basic_auth(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/echo", "-u", "alice:s3:cret")
        long_form = run(httpp_exe, "curl", f"{url}/echo", "--user", "alice:s3:cret")
    expected = "Basic " + base64.b64encode(b"alice:s3:cret").decode()  # only the first colon splits
    assert json.loads(r.stdout)["auth"] == expected
    assert json.loads(long_form.stdout)["auth"] == expected


def test_curl_u_without_a_password_is_an_error(httpp_exe):
    r = run(httpp_exe, "curl", "http://127.0.0.1:1/", "-u", "alice")
    assert r.returncode == 1 and "user:password" in r.stderr


def test_curl_i_with_o_writes_the_head_to_the_file_too(httpp_exe, echo_app, tmp_path):
    out = tmp_path / "with-head.txt"
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/text", "-i", "-o", str(out))
    assert r.returncode == 0 and r.stdout == ""
    assert out.read_text().startswith("HTTP/1.1 200") and out.read_text().endswith("plain body")


# --- py: the Python half -----------------------------------------------------------------------


def test_py_path_matches_the_package(httpp_exe):
    r = run(httpp_exe, "py", "path")
    assert r.returncode == 0, r.stderr
    assert [same(x) for x in r.stdout.splitlines()] == [same(f"include: {httpp.get_include_dir()}"), same(f"lib:     {httpp.get_lib_dir()}"), same(f"cmake:   {httpp.get_cmake_dir()}")]
    r = run(httpp_exe, "py", "path", "--cmake-dir")
    assert same(r.stdout) == same(f"{httpp.get_cmake_dir()}\n")
    r = run(httpp_exe, "py", "--include-dir")
    assert same(r.stdout) == same(f"{httpp.get_include_dir()}\n")


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


def test_py_runs_an_app(httpp_exe, tmp_path, stop_process):
    (tmp_path / "myapp.py").write_text('from httpp import Server\napp = Server(openapi_url=None)\n\n@app.get("/")\ndef index():\n    return "from py run"\n')
    port = free_port()
    proc = subprocess.Popen([*httpp_exe, "py", "run", "myapp:app", "--port", str(port), "--log-level", "warning"], cwd=tmp_path, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=dict(os.environ, HTTPP_PYTHON=sys.executable))
    try:
        r = wait_for(f"http://127.0.0.1:{port}/", proc, attempts=200)
        assert (r.status, r.text) == (200, "from py run")
    finally:
        stop_process(proc)


@posix_only
def test_py_passes_every_argument_unchanged(httpp_bin, tmp_path):
    """Spaces, quotes, empty strings, things that look like options, and --help all reach the interpreter as they were typed."""
    fake = tmp_path / "fakepython"
    fake.write_text(f"#!{sys.executable}\nimport json, sys\nprint(json.dumps(sys.argv[1:]))\nsys.exit(7)\n")
    fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
    args = ["run", "main:app", "two words", 'has"quote', "it's", "", "--port", "--help", "-h", "--version", "$HOME", "*", "ünï"]
    r = run(httpp_bin, "py", *args, env={"HTTPP_PYTHON": str(fake)})
    assert r.returncode == 7  # the interpreter's exit code is ours
    assert json.loads(r.stdout) == ["-m", "httpp", "py", *args]


@posix_only
def test_a_bad_httpp_python_is_reported(httpp_bin, tmp_path):
    r = run(httpp_bin, "py", "path", env={"HTTPP_PYTHON": str(tmp_path / "nope")})
    assert r.returncode == 1 and "HTTPP_PYTHON" in r.stderr


@posix_only
def test_without_a_python_the_error_says_what_to_do(httpp_bin, tmp_path):
    # the executable where it is installed (no Python next to it), an empty PATH and no HTTPP_PYTHON; run in place, so whatever the wheel
    # repair did to its library search path (auditwheel's httpp.libs) still applies
    empty = tmp_path / "empty"
    empty.mkdir()
    env = dict(os.environ, PATH=str(empty))
    env.pop("HTTPP_PYTHON", None)
    r = subprocess.run([httpp_bin, "py", "path"], capture_output=True, text=True, timeout=30, env=env)
    assert r.returncode == 1 and "need a Python" in r.stderr, (r.returncode, r.stdout, r.stderr)


# --- the launcher (httpp_cli.py) and `python -m httpp` -------------------------------------


def test_the_launcher_finds_the_executable_in_the_package():
    exe = httpp_cli.binary_path()
    sdk = Path(httpp.__file__).resolve().parent / "sdk"
    assert exe.parent == sdk / "bin"
    assert (sdk / "lib").is_dir()  # the executable's RPATH is ../lib


def test_the_package_ships_one_sdk_folder():
    """include, lib, bin and the CMake config sit under httpp/sdk, the same shape as an SDK install prefix."""
    pkg = Path(httpp.__file__).resolve().parent
    sdk = pkg / "sdk"
    assert Path(httpp.get_include_dir()).resolve() == sdk / "include"
    assert Path(httpp.get_lib_dir()).resolve() == sdk / "lib"
    assert (sdk / "include" / "httpp.h").is_file()
    assert (Path(httpp.get_cmake_dir()) / "httppConfig.cmake").is_file()
    assert Path(httpp.get_cmake_dir()).resolve().is_relative_to(sdk / "lib") if hasattr(Path, "is_relative_to") else True
    for old in ("include", "lib", "bin", "cmake"):
        assert not (pkg / old).exists(), f"httpp/{old} should now live in httpp/sdk/{old}"


def test_importing_the_launcher_does_not_import_the_package():
    """Why httpp_cli.py is next to the package and not in it: starting a native command must not load the compiled extension."""
    where = str(Path(httpp_cli.__file__).resolve().parent)  # -I ignores PYTHONPATH and the current directory, so the path is given here
    code = f"import sys; sys.path.insert(0, {where!r}); import httpp_cli; sys.exit(1 if 'httpp' in sys.modules else 0)"
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
def test_the_launcher_restores_a_lost_execute_bit(httpp_bin):
    exe = Path(httpp_bin)
    mode = exe.stat().st_mode
    try:
        exe.chmod(mode & ~(stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))
        assert not os.access(exe, os.X_OK)
        r = run([sys.executable, "-m", "httpp"], "--version")  # the launcher, started as `python -m httpp`; not run as root-with-bypass: access() above says it cannot be executed
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


def python_m(*args):
    return subprocess.run([sys.executable, "-m", "httpp", *args], capture_output=True, text=True, timeout=60)


def test_python_dash_m_httpp_is_the_httpp_command():
    """`python -m httpp ARGS` and `httpp ARGS` are the same program: same grammar, same exit codes."""
    r = python_m("--version")
    assert r.returncode == 0 and r.stdout.startswith("httpp "), r.stderr
    r = python_m("py", "path", "--cmake-dir")
    assert r.returncode == 0 and same(r.stdout) == same(f"{httpp.get_cmake_dir()}\n")
    r = python_m("py")
    assert r.returncode == 2 and "usage: httpp py" in r.stderr
    r = python_m("py", "run", "nomod:app")
    assert r.returncode == 1 and "nomod" in r.stderr


@pytest.mark.parametrize("old", ["run", "path"])
def test_python_dash_m_httpp_has_no_old_spelling_either(old):
    r = python_m(old)
    assert r.returncode == 2 and f"httpp py {old}" in r.stderr


def test_python_dash_m_httpp_and_the_executable_answer_alike(httpp_bin):
    for args in (["--version"], ["curl", "--help"], ["download"], ["bogus-command"]):
        a, b = python_m(*args), run(httpp_bin, *args)
        assert (a.returncode, a.stdout, a.stderr) == (b.returncode, b.stdout, b.stderr), args
