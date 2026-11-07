"""Server.mount_file_browser and `httpp browse`: the C++ browser through the Python API (the security matrix is in tests/cpp/test_file_browser.cpp)."""

import json
import os
import socket
import subprocess
import sys
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Server


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


@pytest.fixture
def tree(tmp_path):
    (tmp_path / "root" / "sub").mkdir(parents=True)
    (tmp_path / "root" / "a.txt").write_text("hello")
    (tmp_path / "root" / ".hidden.txt").write_text("x")
    (tmp_path / "root" / "sub" / "b.txt").write_text("bee")
    (tmp_path / "outside.txt").write_text("secret")
    return tmp_path / "root"


def test_lists_and_serves_files(tree):
    app = Server()
    app.mount_file_browser("/files", str(tree), title="My <files>")
    with running(app) as url:
        page = httpp.get(f"{url}/files")
        assert page.status == 200 and "My &lt;files&gt;" in page.text
        listing = json.loads(httpp.get(f"{url}/files/api/list").text)
        names = [e["name"] for e in listing["entries"]]
        assert names == ["sub", "a.txt"]  # folders first, dotfiles hidden
        assert httpp.get(f"{url}/files/raw/sub/b.txt").text == "bee"
        assert httpp.get(f"{url}/files/raw/.hidden.txt").status == 404
        assert httpp.get(f"{url}/files/raw/../outside.txt").status in (400, 404)
        assert httpp.get(f"{url}/files/raw/%2e%2e/outside.txt").status in (400, 404)


def test_options_are_applied(tree):
    app = Server()
    app.mount_file_browser("/", str(tree), show_hidden=True, max_entries=2)
    with running(app) as url:
        listing = json.loads(httpp.get(f"{url}/api/list").text)
        assert len(listing["entries"]) == 2 and listing["truncated"] is True and listing["limit"] == 2
        assert httpp.get(f"{url}/raw/.hidden.txt").status == 200


@pytest.mark.parametrize("kwargs", [{"theme": "purple"}, {"accent": "red"}, {"view": "tiles"}, {"sort": "color"}, {"max_entries": 0}])
def test_bad_options_raise_value_error(tree, kwargs):
    with pytest.raises(ValueError):
        Server().mount_file_browser("/files", str(tree), **kwargs)


def test_bad_base_and_root_raise_value_error(tree):
    with pytest.raises(ValueError):
        Server().mount_file_browser("files", str(tree))
    with pytest.raises(ValueError):
        Server().mount_file_browser("/files", str(tree / "a.txt"))
    with pytest.raises(ValueError):
        Server().mount_file_browser("/files", str(tree / "missing"))
    with pytest.raises(ValueError):
        Server().mount_file_browser("/files", os.path.abspath(os.sep))


def test_write_methods_are_refused(tree):
    app = Server()
    app.mount_file_browser("/files", str(tree))
    with running(app) as url:
        for method in ("POST", "PUT", "DELETE", "PATCH"):
            r = httpp.request(method, f"{url}/files/raw/a.txt")
            assert r.status in (404, 405)
        assert (tree / "a.txt").read_text() == "hello"


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def test_browse_command(tree, httpp_exe, stop_process):
    port = _free_port()
    proc = subprocess.Popen([*httpp_exe, "browse", str(tree), "--port", str(port), "--title", "Docs", "--hidden"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        url = f"http://127.0.0.1:{port}"
        for _ in range(100):  # a failed connection is a response with status 0, not an exception
            r = httpp.get(f"{url}/api/list")
            if r.status != 0:
                break
            time.sleep(0.1)
        assert r.status == 200, (r.status, r.body[:200], proc.poll())
        names = [e["name"] for e in json.loads(r.text)["entries"]]
        assert ".hidden.txt" in names and "a.txt" in names
        assert "Docs" in httpp.get(url).text
    finally:
        stop_process(proc)


def test_browse_command_refuses_a_missing_folder(tmp_path, httpp_exe):
    r = subprocess.run([*httpp_exe, "browse", str(tmp_path / "nope")], capture_output=True, text=True, timeout=30)
    assert r.returncode == 1 and "not a folder" in r.stderr


def test_browse_command_refuses_a_bad_accent(tree, httpp_exe):
    r = subprocess.run([*httpp_exe, "browse", str(tree), "--accent", "red"], capture_output=True, text=True, timeout=30)
    assert r.returncode == 1 and "error" in r.stderr
