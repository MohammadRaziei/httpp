"""Tests for the httpp Python bindings (Client + Server)."""

import time
import threading


def test_server_serves_a_directory(tmp_path):
    from httpp.httpp_cy import Server, Client

    (tmp_path / "index.html").write_text("hello from httpp")

    srv = Server()
    srv.serve_directory("/", str(tmp_path))
    port = srv.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=srv.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        cli = Client("127.0.0.1", port)
        res = cli.get("/index.html")
        assert res.status == 200
        assert res.body == "hello from httpp"
    finally:
        srv.stop()
        th.join()


def test_a_served_file_has_validators_and_answers_a_conditional_request_with_304(tmp_path):
    from httpp.httpp_cy import Server, Client

    (tmp_path / "a.txt").write_text("hello world")

    srv = Server()
    srv.serve_directory("/", str(tmp_path))
    port = srv.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=srv.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        cli = Client("127.0.0.1", port)
        first = cli.get("/a.txt")
        assert first.status == 200
        names = {k.lower(): v for k, v in first.headers}
        etag = names["etag"]
        assert etag.startswith('"') and "last-modified" in names
        again = cli.get("/a.txt", headers={"If-None-Match": etag})
        assert again.status == 304 and again.body == ""
        assert cli.get("/a.txt", headers={"If-None-Match": '"stale"'}).status == 200
        part = cli.get("/a.txt", headers={"Range": "bytes=6-10", "If-Range": '"stale"'})
        assert part.status == 200 and part.body == "hello world"
    finally:
        srv.stop()
        th.join()


def test_client_returns_404_for_missing_path():
    from httpp.httpp_cy import Server, Client

    srv = Server()
    port = srv.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=srv.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        cli = Client("127.0.0.1", port)
        res = cli.get("/nope")
        assert res.status == 404
        assert not res.ok
    finally:
        srv.stop()
        th.join()


def test_client_fetch_parses_full_url(tmp_path):
    from httpp.httpp_cy import Server, Client

    (tmp_path / "index.html").write_text("fetched via full url")

    srv = Server()
    srv.serve_directory("/", str(tmp_path))
    port = srv.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=srv.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        res = Client.fetch(f"http://127.0.0.1:{port}/index.html")
        assert res.ok
        assert res.body == "fetched via full url"
    finally:
        srv.stop()
        th.join()


def test_download_progress_bar_goes_to_stderr_not_stdout(tmp_path):
    """Progress is diagnostics: stdout must stay clean for piping."""
    import subprocess
    import sys
    import textwrap

    (tmp_path / "f.bin").write_bytes(b"x" * 2_000_000)
    code = textwrap.dedent(f"""
        import threading
        from httpp import Server, download
        app = Server()
        app.serve_directory("/f", {str(tmp_path)!r})
        port = app.bind_to_any_port("127.0.0.1")
        threading.Thread(target=app.listen_after_bind, daemon=True).start()
        r = download(f"http://127.0.0.1:{{port}}/f/f.bin", {str(tmp_path / "out.bin")!r})
        print("ok", r.ok)
        app.stop()
    """)
    res = subprocess.run([sys.executable, "-c", code], capture_output=True, timeout=60)
    assert res.returncode == 0, res.stderr.decode(errors="replace")
    assert res.stdout.decode().strip() == "ok True"  # nothing but our own print
    assert b"downloading" in res.stderr
