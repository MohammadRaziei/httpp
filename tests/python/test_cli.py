"""The native `httpp` command line (ADR-0041): server, browse (see also test_file_browser.py), download, curl, `py`, exit codes."""

import base64
import http.client
import http.server
import json
import os
import re
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


def run(exe, *args, env=None, timeout=60, input=None, cwd=None):
    exe = [exe] if isinstance(exe, str) else list(exe)
    full_env = dict(os.environ)
    full_env.setdefault("HTTPP_PYTHON", sys.executable)  # what the launcher sets for the executable (which sits in site-packages, with no Python next to it)
    full_env.update(env or {})
    return subprocess.run([*exe, *args], capture_output=True, text=True, timeout=timeout, env=full_env, input=input, cwd=cwd)


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
        r = httpp.Request(url, method="GET").run()
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
        return json.dumps({"method": request.method, "body": request.body.decode(), "x": request.headers.get("x-test"), "ct": request.headers.get("content-type"), "auth": request.headers.get("authorization"), "ua": request.headers.get("user-agent"), "ref": request.headers.get("referer"), "ae": request.headers.get("accept-encoding")})

    @app.get("/text")
    def text():
        return "plain body"

    @app.get("/files/report.txt")
    def report():
        return "report body"

    @app.get("/missing")
    def missing():
        return httpp.PlainTextResponse("no such page", status_code=404)  # an error status that has a body

    @app.get("/dl")
    def dl(response):
        response.add_header("Content-Disposition", 'attachment; filename="../evil/name.txt"')  # only the name is used
        return "payload"

    @app.post("/json-echo")
    def json_echo(request):
        return json.dumps({"ct": request.headers.get("content-type"), "accept": request.headers.get("accept"), "body": request.body.decode()})

    @app.put("/up/{name}")
    def upload(name, request):
        return f"{name}:{request.body.decode()}"

    calls = {"n": 0}

    @app.get("/flaky")
    def flaky():
        calls["n"] += 1  # 503 twice, then fine
        if calls["n"] <= 2:
            raise httpp.HTTPException(503)
        return "ok"

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
        assert httpp.Request(f"http://127.0.0.1:{port}/nope.txt", method="GET").run().status == 404
    finally:
        stop_process(proc)


@pytest.mark.parametrize("command", ["server", "browse"])
def test_the_native_commands_log_the_address_and_each_request(httpp_exe, tmp_path, stop_process, command):
    port = free_port()
    proc = subprocess.Popen([*httpp_exe, command, str(tmp_path), "--host", "127.0.0.1", "--port", str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert wait_for(f"http://127.0.0.1:{port}/", proc).status != 0
        assert httpp.Request(f"http://127.0.0.1:{port}/nope.txt", method="GET").run().status == 404
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
        assert httpp.Request(f"http://127.0.0.1:{port}{page}", method="GET").run().status != 200  # one port serves TLS or plain HTTP, never both
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
        assert httpp.Request(f"http://127.0.0.1:{port}/nope.txt", method="GET").run().status == 404
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
        assert httpp.Request(f"{url}/.env", method="GET").run().status == 404
        assert httpp.Request(f"{url}/.well-known/acme", method="GET").run().status == 404
    finally:
        stop_process(proc)
    proc, url = _serve(httpp_exe, tmp_path, "--hidden")
    try:
        assert wait_for(f"{url}/hello.txt", proc).status == 200
        assert httpp.Request(f"{url}/.env", method="GET").run().text == "SECRET=1"
        assert httpp.Request(f"{url}/.well-known/acme", method="GET").run().text == "token"
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
            if httpp.Request(f"http://127.0.0.1:{port}/", method="GET").run().status != 0:
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

@contextmanager
def endless_server():
    """An answer that never ends, sent in small chunks, and only to a curl User-Agent (what parrot.live does)."""
    stop = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def do_GET(self):
            if not self.headers.get("User-Agent", "").startswith("curl/"):
                self.send_response(302)
                self.send_header("Location", "/elsewhere")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            try:
                while not stop.is_set():
                    piece = b"frame\n"
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
                    self.wfile.flush()
                    time.sleep(0.01)
            except OSError:
                pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        yield f"127.0.0.1:{server.server_address[1]}"
    finally:
        stop.set()
        server.shutdown()
        server.server_close()
        thread.join()


def start(exe, *args):
    exe = [exe] if isinstance(exe, str) else list(exe)
    env = dict(os.environ)
    env.setdefault("HTTPP_PYTHON", sys.executable)
    return subprocess.Popen([*exe, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)



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


def test_curl_an_http_error_is_still_a_response_exit_0(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/nothing-here", "-i")
    assert r.returncode == 0, r.stderr  # like curl: without -f a 404 is just an answer
    assert r.stdout.startswith("HTTP/1.1 404")


def test_curl_f_fails_on_http_errors_with_22_and_prints_no_body(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/nothing-here", "-f")
        ok = run(httpp_exe, "curl", f"{url}/text", "--fail")
        redirect = run(httpp_exe, "curl", f"{url}/old", "-f")
    assert r.returncode == 22 and r.stdout == "" and "404" in r.stderr
    assert ok.returncode == 0 and ok.stdout == "plain body"
    assert redirect.returncode == 0  # -f only fails on 400 and above


def test_curl_f_does_not_write_the_output_file_on_an_http_error(httpp_exe, echo_app, tmp_path):
    out = tmp_path / "never.txt"
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/nothing-here", "-f", "-o", str(out))
    assert r.returncode == 22 and not out.exists()


def test_curl_a_refused_connection_exits_7_and_says_why(httpp_exe, tmp_path):
    out = tmp_path / "never.txt"
    r = run(httpp_exe, "curl", f"http://127.0.0.1:{free_port()}/", "-o", str(out))
    assert r.returncode == 7 and r.stderr.startswith("error:") and "status 0" not in r.stderr
    assert not out.exists()  # a transfer that never happened leaves no file behind


# --- curl and proxies (ADR-0067 and 0077) -----------------------------------------------------------------------

# An empty variable counts as unset, so these shield the tests from the proxy variables of the machine they run on.
NO_PROXY_VARIABLES = {name: "" for name in ("http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "no_proxy", "NO_PROXY")}


def curl_env(**variables):
    return {**NO_PROXY_VARIABLES, **variables}


@contextmanager
def recording_proxy():
    """An HTTP proxy that answers every GET with 'via proxy' and records what it was asked."""
    seen = []

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def do_GET(self):
            seen.append({"line": self.requestline, "authorization": self.headers.get("Proxy-Authorization")})
            body = b"via proxy"
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        yield f"127.0.0.1:{server.server_address[1]}", seen
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def test_curl_uses_http_proxy_from_the_environment(httpp_exe):
    with recording_proxy() as (proxy, seen):
        r = run(httpp_exe, "curl", "http://example.invalid/a?b=1", env=curl_env(http_proxy=f"http://{proxy}"))
    assert r.returncode == 0, r.stderr
    assert r.stdout == "via proxy"
    assert seen[0]["line"] == "GET http://example.invalid/a?b=1 HTTP/1.1"  # absolute form: it is a proxy request


def test_curl_reads_a_proxy_without_a_scheme_and_all_proxy_like_curl(httpp_exe):
    with recording_proxy() as (proxy, seen):
        bare = run(httpp_exe, "curl", "http://example.invalid/", env=curl_env(http_proxy=proxy))
        fallback = run(httpp_exe, "curl", "http://example.invalid/", env=curl_env(ALL_PROXY=proxy))
    assert (bare.returncode, bare.stdout) == (0, "via proxy")
    assert (fallback.returncode, fallback.stdout) == (0, "via proxy")
    assert len(seen) == 2


def test_curl_ignores_uppercase_HTTP_PROXY_like_curl(httpp_exe, echo_app):
    with recording_proxy() as (proxy, seen), running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/text", env=curl_env(HTTP_PROXY=f"http://{proxy}"))
    assert (r.returncode, r.stdout) == (0, "plain body")
    assert seen == []


def test_curl_x_replaces_the_environment_and_empty_x_turns_the_proxy_off(httpp_exe, echo_app):
    with recording_proxy() as (proxy, seen), running(echo_app) as url:
        dead = f"http://127.0.0.1:{free_port()}"
        via_x = run(httpp_exe, "curl", "-x", f"http://{proxy}", "http://example.invalid/", env=curl_env(http_proxy=dead))
        long_form = run(httpp_exe, "curl", "--proxy", proxy, "http://example.invalid/", env=curl_env(http_proxy=dead))
        off = run(httpp_exe, "curl", "-x", "", f"{url}/text", env=curl_env(http_proxy=f"http://{proxy}"))
    assert (via_x.returncode, via_x.stdout) == (0, "via proxy")
    assert (long_form.returncode, long_form.stdout) == (0, "via proxy")
    assert (off.returncode, off.stdout) == (0, "plain body")
    assert len(seen) == 2  # the two -x runs only


def test_curl_no_proxy_lists_keep_hosts_direct_and_noproxy_replaces_the_variable(httpp_exe, echo_app):
    with recording_proxy() as (proxy, seen), running(echo_app) as url:
        via = curl_env(http_proxy=f"http://{proxy}")
        excluded = run(httpp_exe, "curl", f"{url}/text", env={**via, "NO_PROXY": "localhost,127.0.0.0/8,::1"})
        starred = run(httpp_exe, "curl", f"{url}/text", "--noproxy", "*", env=via)
        replaced = run(httpp_exe, "curl", f"{url}/text", "--noproxy", "other.test", env={**via, "no_proxy": "127.0.0.1"})
        emptied = run(httpp_exe, "curl", f"{url}/text", "--noproxy", "", env={**via, "no_proxy": "127.0.0.1"})
        with_x = run(httpp_exe, "curl", "-x", f"http://{proxy}", "--noproxy", "127.0.0.1", f"{url}/text", env=curl_env())
    assert (excluded.returncode, excluded.stdout) == (0, "plain body")
    assert (starred.returncode, starred.stdout) == (0, "plain body")
    assert (replaced.returncode, replaced.stdout) == (0, "via proxy")  # --noproxy replaces no_proxy, so 127.0.0.1 is no longer excluded
    assert (emptied.returncode, emptied.stdout) == (0, "via proxy")
    assert (with_x.returncode, with_x.stdout) == (0, "plain body")  # the list applies to -x as well
    assert len(seen) == 2


def test_curl_v_names_the_proxy_without_its_login_and_sends_the_login(httpp_exe):
    with recording_proxy() as (proxy, seen):
        r = run(httpp_exe, "curl", "-v", "-x", f"http://user:secret@{proxy}", "http://example.invalid/")
    assert r.returncode == 0, r.stderr
    assert f"* Uses proxy http://{proxy}" in r.stderr
    assert "secret" not in r.stderr
    assert seen[0]["authorization"] == "Basic " + base64.b64encode(b"user:secret").decode()


def test_curl_an_unusable_proxy_exits_5_and_names_it(httpp_exe):
    for bad in ("ftp://127.0.0.1:21", "https://127.0.0.1:443"):
        r = run(httpp_exe, "curl", "-x", bad, "http://example.invalid/", env=curl_env())
        assert r.returncode == 5, (bad, r.stderr)
        assert r.stderr.startswith("error:") and bad in r.stderr
    env = run(httpp_exe, "curl", "http://example.invalid/", env=curl_env(http_proxy="ftp://127.0.0.1:21"))
    assert env.returncode == 5 and "ftp://127.0.0.1:21" in env.stderr


def test_curl_without_a_url_says_what_curl_says_and_exits_2(httpp_exe):
    hint = "httpp curl: try 'httpp curl --help' for more information\n"
    bare = run(httpp_exe, "curl")
    assert (bare.returncode, bare.stdout, bare.stderr) == (2, "", hint)
    flags_only = run(httpp_exe, "curl", "-i")
    assert (flags_only.returncode, flags_only.stdout) == (2, "")
    assert flags_only.stderr == "httpp curl: no URL specified\n" + hint
    assert run(httpp_exe, "curl", "--help").returncode == 0  # help needs no URL


def test_curl_a_bad_url_exits_3(httpp_exe):
    r = run(httpp_exe, "curl", "not a url")
    assert r.returncode == 3 and r.stderr.startswith("error:")


def test_curl_a_url_without_a_scheme_means_http(httpp_exe, echo_app):
    with running(echo_app) as url:
        bare = url[len("http://"):]  # 127.0.0.1:PORT, as typed in `curl host:port/path`
        r = run(httpp_exe, "curl", f"{bare}/text")
        upper = run(httpp_exe, "curl", f"HTTP://{bare}/text")
        verbose = run(httpp_exe, "curl", f"{bare}/text", "-v")
    assert r.returncode == 0, r.stderr
    assert r.stdout == "plain body"
    assert upper.returncode == 0 and upper.stdout == "plain body"  # a scheme that is there is left alone
    assert "> GET /text HTTP/1.1" in verbose.stderr


def test_curl_shows_an_endless_answer_as_it_arrives(httpp_exe):
    with endless_server() as host:
        proc = start(httpp_exe, "curl", host)  # no scheme, no -A: it must announce itself as curl to be answered at all
        watchdog = threading.Timer(20, proc.kill)
        watchdog.start()
        try:
            assert proc.stdout.read(12) == b"frame\nframe\n"  # the first pieces, while the answer is still going on
        finally:
            proc.terminate()
            proc.wait(timeout=10)
            watchdog.cancel()
            proc.stdout.close()
            proc.stderr.close()


@pytest.mark.skipif(sys.platform == "win32", reason="a closed pipe is a POSIX notion")
def test_curl_stops_with_23_when_the_reader_of_stdout_goes_away(httpp_exe):
    with endless_server() as host:
        proc = start(httpp_exe, "curl", host)
        watchdog = threading.Timer(20, proc.kill)
        watchdog.start()
        try:
            assert proc.stdout.read(6) == b"frame\n"
            proc.stdout.close()  # like `httpp curl ... | head -1`
            assert proc.wait(timeout=15) == 23
            assert b"standard output" in proc.stderr.read()
        finally:
            watchdog.cancel()
            proc.stderr.close()


def test_curl_a_big_answer_is_written_to_the_file_in_full(httpp_exe, tmp_path):
    body = "0123456789abcdef" * 65536  # 1 MiB
    app = Server(openapi_url=None)

    @app.get("/big")
    def big():
        return body

    out = tmp_path / "big.txt"
    with running(app) as url:
        r = run(httpp_exe, "curl", f"{url}/big", "-o", str(out), "-w", "%{size_download}")
    assert r.returncode == 0, r.stderr
    assert r.stdout == str(len(body))
    assert out.read_text() == body


def test_curl_version_names_httpp_and_the_curl_it_follows(httpp_exe):
    for flag in ("--version", "-V"):
        r = run(httpp_exe, "curl", flag)
        assert r.returncode == 0, r.stderr
        assert re.fullmatch(r"httpp curl \d+\.\d+\.\d+ compatible with curl \d+\.\d+\.\d+\s*", r.stdout), r.stdout


def test_curl_announces_itself_as_curl_unless_told_otherwise(httpp_exe, echo_app):
    with running(echo_app) as url:
        default = run(httpp_exe, "curl", f"{url}/echo")
        chosen = run(httpp_exe, "curl", f"{url}/echo", "-A", "probe/1")
    assert re.fullmatch(r"curl/\d+\.\d+\.\d+", json.loads(default.stdout)["ua"])
    assert json.loads(chosen.stdout)["ua"] == "probe/1"


def test_curl_a_url_with_a_space_is_rejected_and_never_sent(httpp_exe):
    r = run(httpp_exe, "curl", f"127.0.0.1:{free_port()}/a b", "-v")
    assert r.returncode == 3 and r.stderr.startswith("error:") and "> GET" not in r.stderr


def test_curl_a_bare_host_that_does_not_exist_is_not_a_bad_url(httpp_exe):
    r = run(httpp_exe, "curl", f"127.0.0.1:{free_port()}")
    assert r.returncode == 7  # refused, which proves it was read as http://127.0.0.1:PORT and not rejected as a URL


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
    assert r.returncode == 28 and r.stderr.startswith("error:")
    assert elapsed < 2


def test_curl_max_time_ends_an_endless_answer_that_never_stalls(httpp_exe):
    with endless_server() as host:
        started = time.monotonic()
        r = run(httpp_exe, "curl", host, "-m", "2")  # a frame every 10 ms: no single step is slow, only the total is
        elapsed = time.monotonic() - started
    assert r.returncode == 28 and "error:" in r.stderr
    assert re.search(r"^error: timed out after \d+ ms with [1-9]\d* bytes received$", r.stderr.strip())
    assert r.stdout.startswith("frame\n")  # what arrived before the limit was shown
    assert 1.5 < elapsed < 6


def test_curl_repeated_d_joins_the_values_with_an_ampersand(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", "-d", "a=1", "-d", "b=2", f"{url}/echo")  # the url may come last
        one = run(httpp_exe, "curl", f"{url}/echo", "--data", "x=y")
    assert json.loads(r.stdout)["body"] == "a=1&b=2" and json.loads(r.stdout)["method"] == "POST"
    assert json.loads(one.stdout)["body"] == "x=y"


def test_curl_A_and_e_set_the_user_agent_and_the_referer(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/echo", "-A", "probe/1", "-e", "http://from.example/")
        long_form = run(httpp_exe, "curl", f"{url}/echo", "--user-agent", "probe/2", "--referer", "http://x/")
        own = run(httpp_exe, "curl", f"{url}/echo", "-A", "probe/1", "-H", "User-Agent: mine")
        default = run(httpp_exe, "curl", f"{url}/echo")
    assert (json.loads(r.stdout)["ua"], json.loads(r.stdout)["ref"]) == ("probe/1", "http://from.example/")
    assert (json.loads(long_form.stdout)["ua"], json.loads(long_form.stdout)["ref"]) == ("probe/2", "http://x/")
    assert json.loads(own.stdout)["ua"] == "mine"  # an explicit -H wins over -A
    assert re.fullmatch(r"curl/\d+\.\d+\.\d+", json.loads(default.stdout)["ua"])  # the command presents itself as curl (ADR-0077)


def test_curl_max_redirs_bounds_the_hops_with_47(httpp_exe, echo_app):
    with running(echo_app) as url:
        enough = run(httpp_exe, "curl", f"{url}/old", "-L", "--max-redirs", "1")
        too_few = run(httpp_exe, "curl", f"{url}/old", "-L", "--max-redirs", "0")
        unused = run(httpp_exe, "curl", f"{url}/old", "--max-redirs", "0", "-i")
    assert enough.returncode == 0 and enough.stdout == "plain body"
    assert too_few.returncode == 47 and "too many redirects" in too_few.stderr
    assert unused.returncode == 0 and unused.stdout.startswith("HTTP/1.1 302")  # without -L it changes nothing


def test_curl_connect_timeout_does_not_cut_a_slow_answer(httpp_exe, echo_app):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/slow", "--connect-timeout", "1", "-m", "5")
    assert r.returncode == 0 and r.stdout == "late"


def test_curl_max_redirs_and_connect_timeout_must_be_numbers(httpp_exe):
    assert run(httpp_exe, "curl", "http://127.0.0.1:1/", "--max-redirs", "many").returncode == 2
    assert run(httpp_exe, "curl", "http://127.0.0.1:1/", "--max-redirs", "-2").returncode == 2
    assert run(httpp_exe, "curl", "http://127.0.0.1:1/", "--connect-timeout", "0").returncode == 2


def test_curl_s_hides_the_error_line_and_S_brings_it_back(httpp_exe):
    dead = f"http://127.0.0.1:{free_port()}/"
    quiet = run(httpp_exe, "curl", dead, "-s")
    loud = run(httpp_exe, "curl", dead, "-sS")
    assert quiet.returncode == 7 and quiet.stderr == ""
    assert loud.returncode == 7 and loud.stderr.startswith("error:")


def test_curl_w_prints_the_write_out_after_the_transfer(httpp_exe, echo_app, tmp_path):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/text", "-o", str(tmp_path / "o"), "-w", "%{http_code} %{size_download} 100%%\\n")
        missing = run(httpp_exe, "curl", f"{url}/nothing-here", "-s", "-o", str(tmp_path / "o2"), "-w", "%{http_code}")
    assert r.returncode == 0 and r.stdout == "200 10 100%\n"
    assert missing.stdout == "404"
    dead = run(httpp_exe, "curl", f"http://127.0.0.1:{free_port()}/", "-s", "-w", "%{http_code} %{exitcode} %{nope}")
    assert dead.returncode == 7 and dead.stdout == "000 7 %{nope}"  # an unknown variable stays as typed


def test_curl_w_reads_its_format_from_a_file(httpp_exe, echo_app, tmp_path):
    fmt = tmp_path / "fmt.txt"
    fmt.write_text("code=%{http_code};type=%{content_type}")
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/text", "-o", str(tmp_path / "o"), "-w", f"@{fmt}")
    assert r.returncode == 0 and r.stdout.startswith("code=200;type=text/plain")


def test_curl_capital_o_names_the_file_after_the_url(httpp_exe, echo_app, tmp_path):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/files/report.txt", "-O", cwd=str(tmp_path))
        bare = run(httpp_exe, "curl", f"{url}/", "--remote-name", cwd=str(tmp_path))
    assert r.returncode == 0 and r.stdout == ""
    assert (tmp_path / "report.txt").read_text() == "report body"
    assert bare.returncode == 23 and "file name" in bare.stderr


def test_curl_fail_with_body_exits_22_but_still_shows_and_saves_the_body(httpp_exe, echo_app, tmp_path):
    out = tmp_path / "error-page.txt"
    with running(echo_app) as url:
        shown = run(httpp_exe, "curl", f"{url}/missing", "--fail-with-body")
        saved = run(httpp_exe, "curl", f"{url}/missing", "--fail-with-body", "-o", str(out))
        ok = run(httpp_exe, "curl", f"{url}/text", "--fail-with-body")
        both = run(httpp_exe, "curl", f"{url}/text", "-f", "--fail-with-body")
    assert shown.returncode == 22 and shown.stdout == "no such page" and "404" in shown.stderr
    assert saved.returncode == 22 and out.read_text() == "no such page"
    assert ok.returncode == 0 and ok.stdout == "plain body"
    assert both.returncode == 2  # -f and --fail-with-body exclude each other


def test_curl_json_sends_the_body_with_both_json_headers(httpp_exe, echo_app, tmp_path):
    (tmp_path / "doc.json").write_text('{\n"a": 1\n}\n')
    with running(echo_app) as url:
        one = run(httpp_exe, "curl", f"{url}/json-echo", "--json", '{"a":1}')
        joined = run(httpp_exe, "curl", f"{url}/json-echo", "--json", '{"a":', "--json", "1}")
        from_file = run(httpp_exe, "curl", f"{url}/json-echo", "--json", f"@{tmp_path / 'doc.json'}")
        own_type = run(httpp_exe, "curl", f"{url}/json-echo", "--json", "{}", "-H", "Content-Type: application/vnd.x+json")
        mixed = run(httpp_exe, "curl", f"{url}/json-echo", "--json", "{}", "-d", "x")
    assert json.loads(one.stdout) == {"ct": "application/json", "accept": "application/json", "body": '{"a":1}'}
    assert json.loads(joined.stdout)["body"] == '{"a":1}'  # pieces are joined without a separator
    assert json.loads(from_file.stdout)["body"] == '{\n"a": 1\n}\n'  # unlike -d, the line breaks stay
    assert json.loads(own_type.stdout)["ct"] == "application/vnd.x+json"  # -H wins
    assert mixed.returncode == 1 and "--json" in mixed.stderr


def test_curl_J_names_the_file_after_content_disposition_and_never_replaces_one(httpp_exe, echo_app, tmp_path):
    with running(echo_app) as url:
        first = run(httpp_exe, "curl", f"{url}/dl", "-O", "-J", cwd=str(tmp_path))
        second = run(httpp_exe, "curl", f"{url}/dl", "-O", "-J", cwd=str(tmp_path))
        plain = run(httpp_exe, "curl", f"{url}/files/report.txt", "-O", "-J", cwd=str(tmp_path))  # no header: the URL's name
        without_O = run(httpp_exe, "curl", f"{url}/dl", "-J", cwd=str(tmp_path))  # -J only works with -O
    assert first.returncode == 0 and (tmp_path / "name.txt").read_text() == "payload"  # "../evil/" is cut off
    assert second.returncode == 23 and "Refusing to overwrite" in second.stderr
    assert plain.returncode == 0 and (tmp_path / "report.txt").read_text() == "report body"
    assert without_O.returncode == 0 and without_O.stdout == "payload"


def test_curl_output_dir_and_create_dirs(httpp_exe, echo_app, tmp_path):
    with running(echo_app) as url:
        named = run(httpp_exe, "curl", f"{url}/files/report.txt", "-O", "--output-dir", "out/deep", "--create-dirs", cwd=str(tmp_path))
        given = run(httpp_exe, "curl", f"{url}/text", "-o", "x.txt", "--output-dir", "out", cwd=str(tmp_path))
        nested = run(httpp_exe, "curl", f"{url}/text", "-o", "a/b/y.txt", "--create-dirs", cwd=str(tmp_path))
        missing = run(httpp_exe, "curl", f"{url}/text", "-o", "nodir/z.txt", cwd=str(tmp_path))
        absolute = run(httpp_exe, "curl", f"{url}/text", "-o", str(tmp_path / "abs.txt"), "--output-dir", "ignored", cwd=str(tmp_path))
    assert named.returncode == 0 and (tmp_path / "out/deep/report.txt").read_text() == "report body"
    assert given.returncode == 0 and (tmp_path / "out/x.txt").read_text() == "plain body"
    assert nested.returncode == 0 and (tmp_path / "a/b/y.txt").read_text() == "plain body"
    assert missing.returncode == 23 and not (tmp_path / "nodir").exists()  # without --create-dirs the folder is not made
    assert absolute.returncode == 0 and (tmp_path / "abs.txt").read_text() == "plain body"  # an absolute name stays where it is


def test_curl_k_and_cacert_open_an_https_server_with_a_self_signed_certificate(httpp_exe, tmp_path, stop_process, certs):  # noqa: F811
    cert, key, _ = certs
    (tmp_path / "hello.txt").write_text("hi there")
    port = free_port()
    proc = subprocess.Popen([*httpp_exe, "server", str(tmp_path), "--host", "127.0.0.1", "--port", str(port), "--ssl-certfile", cert, "--ssl-keyfile", key],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        _wait_for_port(port, proc)
        url = f"https://127.0.0.1:{port}/hello.txt"
        unchecked = run(httpp_exe, "curl", url)
        insecure = run(httpp_exe, "curl", "-k", url)
        long_form = run(httpp_exe, "curl", "--insecure", url)
        pinned = run(httpp_exe, "curl", "--cacert", cert, url)
        k_wins = run(httpp_exe, "curl", "-k", "--cacert", str(tmp_path / "no-such.pem"), url)
        missing = run(httpp_exe, "curl", "--cacert", str(tmp_path / "no-such.pem"), url)
    finally:
        stop_process(proc)
    assert unchecked.returncode == 35 and unchecked.stdout == ""  # a certificate nobody trusts
    assert insecure.returncode == 0 and insecure.stdout == "hi there"
    assert long_form.returncode == 0 and long_form.stdout == "hi there"
    assert pinned.returncode == 0 and pinned.stdout == "hi there"
    assert k_wins.returncode == 0 and k_wins.stdout == "hi there"
    assert missing.returncode == 77 and "certificate" in missing.stderr


def test_curl_T_uploads_a_file_with_put(httpp_exe, echo_app, tmp_path):
    payload = tmp_path / "payload.txt"
    payload.write_text("file content")
    with running(echo_app) as url:
        plain = run(httpp_exe, "curl", f"{url}/echo", "-T", str(payload))
        named = run(httpp_exe, "curl", f"{url}/up/", "--upload-file", str(payload))  # a trailing slash gets the file name
        stdin = run(httpp_exe, "curl", f"{url}/echo", "-T", "-", input="from stdin")
        missing = run(httpp_exe, "curl", f"{url}/echo", "-T", str(tmp_path / "nope"))
        both = run(httpp_exe, "curl", f"{url}/echo", "-T", str(payload), "-d", "a=1")
    assert (json.loads(plain.stdout)["method"], json.loads(plain.stdout)["body"]) == ("PUT", "file content")
    assert named.stdout == "payload.txt:file content"
    assert (json.loads(stdin.stdout)["method"], json.loads(stdin.stdout)["body"]) == ("PUT", "from stdin")
    assert missing.returncode == 26 and "cannot read" in missing.stderr
    assert both.returncode == 1 and "-T" in both.stderr


def test_curl_D_dumps_the_head_to_a_file_or_to_stdout(httpp_exe, echo_app, tmp_path):
    dump = tmp_path / "head.txt"
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/text", "-D", str(dump))
        to_stdout = run(httpp_exe, "curl", f"{url}/text", "--dump-header", "-")
    assert r.returncode == 0 and r.stdout == "plain body"
    head = dump.read_text().replace("\r\n", "\n")
    assert head.startswith("HTTP/1.1 200\n") and "content-type" in head.lower()
    assert to_stdout.stdout.replace("\r\n", "\n").startswith("HTTP/1.1 200\n")
    assert to_stdout.stdout.endswith("plain body")


def test_curl_d_reads_a_file_and_drops_its_line_breaks_like_curl(httpp_exe, echo_app, tmp_path):
    body = tmp_path / "body.txt"
    body.write_bytes(b"a=1\r\nb=2\n")
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/echo", "-d", f"@{body}")
        mixed = run(httpp_exe, "curl", f"{url}/echo", "-d", "x=1", "-d", f"@{body}")
        stdin = run(httpp_exe, "curl", f"{url}/echo", "-d", "@-", input="from=stdin\n")
        missing = run(httpp_exe, "curl", f"{url}/echo", "-d", f"@{tmp_path / 'nope'}")
    assert json.loads(r.stdout)["body"] == "a=1b=2" and json.loads(r.stdout)["method"] == "POST"
    assert json.loads(mixed.stdout)["body"] == "x=1&a=1b=2"
    assert json.loads(stdin.stdout)["body"] == "from=stdin"
    assert missing.returncode == 26 and "cannot read" in missing.stderr


def test_curl_compressed_is_accepted_and_the_streamed_answer_asks_for_identity(httpp_exe, echo_app):
    with running(echo_app) as url:
        plain = run(httpp_exe, "curl", f"{url}/echo")
        asked = run(httpp_exe, "curl", f"{url}/echo", "--compressed")
        own = run(httpp_exe, "curl", f"{url}/echo", "-H", "Accept-Encoding: gzip")
    assert asked.returncode == 0
    assert json.loads(plain.stdout)["ae"] == json.loads(asked.stdout)["ae"] == "identity"  # streamed as sent, so no coding is asked for
    assert json.loads(own.stdout)["ae"] == "gzip"  # an explicit -H wins


def test_curl_retry_tries_again_after_a_transient_status(httpp_exe, echo_app):
    with running(echo_app) as url:
        started = time.monotonic()
        r = run(httpp_exe, "curl", f"{url}/flaky", "--retry", "2", "--retry-delay", "1")
        elapsed = time.monotonic() - started
    assert r.returncode == 0 and r.stdout == "ok" and "retry" in r.stderr.lower()
    assert elapsed >= 2


def test_curl_retry_gives_up_after_the_given_number(httpp_exe, echo_app, tmp_path):
    with running(echo_app) as url:
        r = run(httpp_exe, "curl", f"{url}/flaky", "--retry", "1", "--retry-delay", "1", "-s", "-o", str(tmp_path / "o"), "-w", "%{http_code}")
    assert r.stdout == "503" and r.stderr == ""  # -s hides the retry warning too


def test_curl_retry_does_not_retry_a_refused_connection_unless_asked(httpp_exe):
    dead = f"http://127.0.0.1:{free_port()}/"
    started = time.monotonic()
    first = run(httpp_exe, "curl", dead, "--retry", "1", "--retry-delay", "1", "-s")
    fast = time.monotonic() - started
    started = time.monotonic()
    second = run(httpp_exe, "curl", dead, "--retry", "1", "--retry-delay", "1", "--retry-connrefused", "-s")
    slow = time.monotonic() - started
    assert first.returncode == 7 and fast < 1
    assert second.returncode == 7 and slow >= 1


def test_curl_fetches_several_urls_in_turn(httpp_exe, echo_app, tmp_path):
    with running(echo_app) as url:
        both = run(httpp_exe, "curl", "-H", "X-Test: shared", f"{url}/text", f"{url}/echo")
        files = run(httpp_exe, "curl", f"{url}/text", f"{url}/files/report.txt", "-o", "first.out", "-o", "second.out", cwd=str(tmp_path))
        named = run(httpp_exe, "curl", f"{url}/files/report.txt", f"{url}/text", "-O", cwd=str(tmp_path))
        dead = f"http://127.0.0.1:{free_port()}/"
        failing = run(httpp_exe, "curl", dead, f"{url}/text", "-s")
        counted = run(httpp_exe, "curl", f"{url}/text", f"{url}/text", "-w", "[%{http_code}]")
    assert both.stdout.startswith("plain body{") and json.loads(both.stdout[len("plain body"):])["x"] == "shared"
    assert files.returncode == 0 and files.stdout == ""
    assert (tmp_path / "first.out").read_text() == "plain body" and (tmp_path / "second.out").read_text() == "report body"
    assert (tmp_path / "report.txt").read_text() == "report body" and (tmp_path / "text").read_text() == "plain body"
    assert failing.returncode == 7 and failing.stdout == "plain body"  # the first failure is the exit code, the rest still run
    assert counted.stdout == "plain body[200]plain body[200]"


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
