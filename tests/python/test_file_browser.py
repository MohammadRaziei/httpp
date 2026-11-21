"""httpp.FileBrowser and `httpp browse`: the C++ browser through the Python API (the security matrix is in tests/cpp/test_file_browser.cpp)."""

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
from httpp import FileBrowser, Server


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
    FileBrowser(tree).title("My <files>").mount(app, "/files")
    with running(app) as url:
        page = httpp.Request(f"{url}/files", method="GET").run()
        assert page.status == 200 and "My &lt;files&gt;" in page.text
        listing = json.loads(httpp.Request(f"{url}/files/api/list", method="GET").run().text)
        names = [e["name"] for e in listing["entries"]]
        assert names == ["sub", "a.txt"]  # folders first, dotfiles hidden
        assert httpp.Request(f"{url}/files/raw/sub/b.txt", method="GET").run().text == "bee"
        assert httpp.Request(f"{url}/files/raw/.hidden.txt", method="GET").run().status == 404
        assert httpp.Request(f"{url}/files/raw/../outside.txt", method="GET").run().status in (400, 404)
        assert httpp.Request(f"{url}/files/raw/%2e%2e/outside.txt", method="GET").run().status in (400, 404)


def test_options_are_applied(tree):
    app = Server()
    FileBrowser(tree, show_hidden=True, max_entries=2).mount(app)
    with running(app) as url:
        listing = json.loads(httpp.Request(f"{url}/api/list", method="GET").run().text)
        assert len(listing["entries"]) == 2 and listing["truncated"] is True and listing["limit"] == 2
        assert httpp.Request(f"{url}/raw/.hidden.txt", method="GET").run().status == 200


def test_the_enums_and_the_keywords_reach_the_page(tree):
    app = Server()
    (FileBrowser(tree, accent="#12ab34", search=False)
        .theme(FileBrowser.Theme.DARK).layout(FileBrowser.View.GRID).sort_by(FileBrowser.Sort.DATE).mount(app, "/files"))
    with running(app) as url:
        page = httpp.Request(f"{url}/files", method="GET").run().text
        for fragment in ('"theme":"dark"', '"view":"grid"', '"sort":"date"', '"accent":"#12ab34"', '"search":false'):
            assert fragment in page


def test_the_defaults_are_those_of_the_cpp_class(tree):
    app = Server()
    FileBrowser(tree).mount(app, "/files")
    with running(app) as url:
        page = httpp.Request(f"{url}/files", method="GET").run().text
        for fragment in ('"theme":"auto"', '"view":"list"', '"sort":"name"', '"accent":""', '"search":true', '"limit":5000', '"powered":true', '"zip":false', '"dates":true'):
            assert fragment in page


def test_html_and_svg_are_shown_behind_a_sandbox_by_default_and_safe_mode_makes_them_downloads(tree):
    (tree / "p.html").write_text("<script>1</script><p>hi</p>")
    (tree / "x.svg").write_text("<svg xmlns='http://www.w3.org/2000/svg'/>")
    (tree / "x.xml").write_text("<a/>")
    default, loose, safe, keyword = Server(), Server(), Server(), Server()
    FileBrowser(tree).mount(default, "/files")
    fb = FileBrowser(tree)
    assert fb.safe_mode(False) is fb
    fb.mount(loose, "/files")
    fb = FileBrowser(tree)
    assert fb.safe_mode(True) is fb
    fb.mount(safe, "/files")
    FileBrowser(tree, safe_mode=True).mount(keyword, "/files")
    for server in (default, loose):
        with running(server) as url:
            assert '"safe":false' in httpp.Request(f"{url}/files", method="GET").run().text
            for name in ("p.html", "x.svg"):
                r = httpp.Request(f"{url}/files/raw/{name}", method="GET").run()
                assert r.header("Content-Disposition") is None  # shown, not downloaded
                csp = r.header("Content-Security-Policy")
                assert "sandbox;" in csp and "allow-" not in csp and "default-src 'none'" in csp
                assert "attachment" in httpp.Request(f"{url}/files/raw/{name}?download=1", method="GET").run().header("Content-Disposition")
            assert "attachment" in httpp.Request(f"{url}/files/raw/x.xml", method="GET").run().header("Content-Disposition")
    for server in (safe, keyword):
        with running(server) as url:
            assert '"safe":true' in httpp.Request(f"{url}/files", method="GET").run().text
            for name in ("p.html", "x.svg", "x.xml"):
                r = httpp.Request(f"{url}/files/raw/{name}", method="GET").run()
                assert "attachment" in r.header("Content-Disposition") and "sandbox" not in (r.header("Content-Security-Policy") or "")


def _zip_dates(url, query):
    import io
    import urllib.request
    import zipfile

    with urllib.request.urlopen(f"{url}/files/api/zip?{query}") as r:
        return {i.filename: i.date_time for i in zipfile.ZipFile(io.BytesIO(r.read())).infolist()}


def test_show_dates_false_shows_and_sends_no_file_time(tree):
    app, dated = Server(), Server()
    fb = FileBrowser(tree, zip=True)
    assert fb.show_dates(False) is fb
    fb.mount(app, "/files")
    FileBrowser(tree, zip=True).mount(dated, "/files")
    with running(dated) as url:
        assert '"dates":true' in httpp.Request(f"{url}/files", method="GET").run().text
        assert '"mtime":' in httpp.Request(f"{url}/files/api/list", method="GET").run().text
        assert httpp.Request(f"{url}/files/raw/a.txt", method="GET").run().header("Last-Modified") is not None
        assert _zip_dates(url, "path=/&name=a.txt")["a.txt"][:3] != (1980, 1, 1)
    with running(app) as url:
        assert '"dates":false' in httpp.Request(f"{url}/files", method="GET").run().text
        listing = httpp.Request(f"{url}/files/api/list", method="GET").run().text
        assert "a.txt" in listing and "mtime" not in listing
        raw = httpp.Request(f"{url}/files/raw/a.txt", method="GET").run()
        assert raw.status == 200 and raw.header("ETag") is None and raw.header("Last-Modified") is None
        assert set(_zip_dates(url, "path=/").values()) == {(1980, 1, 1, 0, 0, 0)}


def test_the_show_dates_keyword_is_the_same_as_the_setter(tree):
    app = Server()
    FileBrowser(tree, show_dates=False).mount(app, "/files")
    with running(app) as url:
        assert '"dates":false' in httpp.Request(f"{url}/files", method="GET").run().text


def test_the_setters_chain_and_one_browser_mounts_twice(tree):
    fb = FileBrowser(tree)
    assert fb.title("x") is fb and fb.theme(FileBrowser.Theme.LIGHT) is fb and fb.max_entries(3) is fb
    first, second = Server(), Server()
    fb.mount(first, "/a")
    fb.mount(second, "/b")
    with running(first) as url:
        assert httpp.Request(f"{url}/a/api/list", method="GET").run().status == 200
    with running(second) as url:
        assert httpp.Request(f"{url}/b/api/list", method="GET").run().status == 200


@pytest.mark.parametrize("call", [
    lambda fb: fb.accent("red"),
    lambda fb: fb.accent("#12345"),
    lambda fb: fb.max_entries(0),
    lambda fb: fb.max_entries(-1),
])
def test_a_bad_value_raises_value_error_at_the_setter_and_changes_nothing(tree, call):
    fb = FileBrowser(tree)
    with pytest.raises(ValueError):
        call(fb)
    app = Server()
    fb.mount(app, "/files")
    with running(app) as url:
        page = httpp.Request(f"{url}/files", method="GET").run().text
        assert '"accent":""' in page and '"limit":5000' in page


@pytest.mark.parametrize("name,value", [("theme", "dark"), ("layout", "grid"), ("sort_by", "date"),
                                        ("theme", FileBrowser.View.GRID), ("layout", FileBrowser.Sort.NAME), ("sort_by", 1)])
def test_an_enum_setter_takes_only_its_own_enum(tree, name, value):
    with pytest.raises(TypeError):
        getattr(FileBrowser(tree), name)(value)


@pytest.mark.parametrize("keyword,value", [("theme", "dark"), ("view", FileBrowser.Sort.NAME), ("accent", "red"), ("max_entries", 0)])
def test_the_constructor_keywords_are_checked_like_the_setters(tree, keyword, value):
    with pytest.raises((TypeError, ValueError)):
        FileBrowser(tree, **{keyword: value})


def test_bad_base_and_root_raise_value_error_when_mounting(tree):
    with pytest.raises(ValueError):
        FileBrowser(tree).mount(Server(), "files")
    for root in (tree / "a.txt", tree / "missing", os.path.abspath(os.sep)):
        fb = FileBrowser(root)  # the root is checked when mounting, not here
        with pytest.raises(ValueError):
            fb.mount(Server(), "/files")


def test_mount_file_browser_is_gone_from_server():
    assert not hasattr(Server, "mount_file_browser")  # one way to do it, as in C++ (ADR-0032)


def test_write_methods_are_refused(tree):
    app = Server()
    FileBrowser(tree).mount(app, "/files")
    with running(app) as url:
        for method in ("POST", "PUT", "DELETE", "PATCH"):
            r = httpp.Request(f"{url}/files/raw/a.txt", method=method).run()
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
            r = httpp.Request(f"{url}/api/list", method="GET").run()
            if r.status != 0:
                break
            time.sleep(0.1)
        assert r.status == 200, (r.status, r.body[:200], proc.poll())
        names = [e["name"] for e in json.loads(r.text)["entries"]]
        assert ".hidden.txt" in names and "a.txt" in names
        assert "Docs" in httpp.Request(url, method="GET").run().text
    finally:
        stop_process(proc)


def test_browse_command_refuses_a_missing_folder(tmp_path, httpp_exe):
    r = subprocess.run([*httpp_exe, "browse", str(tmp_path / "nope")], capture_output=True, text=True, timeout=30)
    assert r.returncode == 1 and "not a folder" in r.stderr


def test_browse_command_refuses_a_bad_accent(tree, httpp_exe):
    r = subprocess.run([*httpp_exe, "browse", str(tree), "--accent", "red"], capture_output=True, text=True, timeout=30)
    assert r.returncode == 1 and "error" in r.stderr


@pytest.mark.parametrize("flag, shown", [(None, True), ("--safe", False)])
def test_browse_command_shows_html_by_default_and_safe_makes_it_a_download(tree, httpp_exe, stop_process, flag, shown):
    (tree / "p.html").write_text("<p>hi</p>")
    port = _free_port()
    proc = subprocess.Popen([*httpp_exe, "browse", str(tree), "--port", str(port), *([flag] if flag else [])], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        url = f"http://127.0.0.1:{port}"
        for _ in range(100):
            r = httpp.Request(f"{url}/raw/p.html", method="GET").run()
            if r.status != 0:
                break
            time.sleep(0.1)
        assert r.status == 200, (r.status, proc.poll())
        if shown:
            assert r.header("Content-Disposition") is None and "sandbox;" in r.header("Content-Security-Policy")
        else:
            assert "attachment" in r.header("Content-Disposition")
        assert f'"safe":{"false" if shown else "true"}' in httpp.Request(url, method="GET").run().text
    finally:
        stop_process(proc)


# ---- ZIP download (ADR-0032): the archive is read back with zipfile, the strictest reader that is at hand ----

def _zip_tree(tree):
    (tree / "فارسی").mkdir()
    (tree / "فارسی" / "آهنگ من.mp3").write_bytes(bytes(range(256)) * 600)  # 153600 bytes: more than one stored block
    (tree / "sub" / "empty").mkdir()
    (tree / "empty.txt").write_bytes(b"")
    return tree


def _archive(body):
    import io
    import zipfile
    z = zipfile.ZipFile(io.BytesIO(body))
    assert z.testzip() is None
    return z


def test_zip_is_off_by_default_and_set_zip_turns_it_on(tree):
    app = Server()
    FileBrowser(tree).mount(app, "/files")
    on = Server()
    FileBrowser(tree, zip=True).mount(on, "/files")
    with running(app) as url:
        assert httpp.Request(f"{url}/files/api/zip?path=/sub", method="GET").run().status == 404
        assert '"zip":false' in httpp.Request(f"{url}/files", method="GET").run().text
    with running(on) as url:
        assert httpp.Request(f"{url}/files/api/zip?path=/sub", method="GET").run().status == 200
        assert '"zip":true' in httpp.Request(f"{url}/files", method="GET").run().text


def test_zip_of_a_folder_and_of_chosen_names_reads_back_with_zipfile(tree):
    _zip_tree(tree)
    app = Server()
    FileBrowser(tree).zip(True).title("Music").mount(app, "/files")
    with running(app) as url:
        whole = httpp.Request(f"{url}/files/api/zip?path=/", method="GET").run()
        assert whole.status == 200 and whole.header("Content-Type") == "application/zip"
        assert "Music.zip" in whole.header("Content-Disposition")
        z = _archive(whole.content)
        assert z.namelist() == ["Music/", "Music/sub/", "Music/sub/b.txt", "Music/sub/empty/", "Music/a.txt", "Music/empty.txt", "Music/فارسی/", "Music/فارسی/آهنگ من.mp3"] or \
            sorted(z.namelist()) == sorted(["Music/", "Music/sub/", "Music/sub/b.txt", "Music/sub/empty/", "Music/a.txt", "Music/empty.txt", "Music/فارسی/", "Music/فارسی/آهنگ من.mp3"])
        assert z.read("Music/فارسی/آهنگ من.mp3") == bytes(range(256)) * 600
        assert z.read("Music/a.txt") == b"hello" and z.read("Music/empty.txt") == b""
        assert ".hidden.txt" not in " ".join(z.namelist())  # hidden names stay out

        # chosen names by POST (a form, repeated "name"), a Persian folder name, and the file name that goes with it
        form = httpp.Request(f"{url}/files/api/zip", form=[("path", "/"), ("name", "فارسی")], method="POST").run()
        assert form.status == 200
        assert "filename*=UTF-8''%D9%81%D8%A7%D8%B1%D8%B3%DB%8C.zip" in form.header("Content-Disposition")
        assert _archive(form.content).namelist() == ["فارسی/", "فارسی/آهنگ من.mp3"]

        two = httpp.Request(f"{url}/files/api/zip", form=[("path", "/"), ("name", "a.txt"), ("name", "sub")], method="POST").run()
        assert _archive(two.content).namelist() == ["a.txt", "sub/", "sub/b.txt", "sub/empty/"]


def test_zip_refusals_and_the_entry_limit(tree):
    app = Server()
    FileBrowser(tree).zip(True).max_zip_entries(2).mount(app, "/files")
    with running(app) as url:
        assert httpp.Request(f"{url}/files/api/zip?path=/../x", method="GET").run().status == 400
        assert httpp.Request(f"{url}/files/api/zip?path=/&name=..", method="GET").run().status == 400
        assert httpp.Request(f"{url}/files/api/zip?path=/&name=.hidden.txt", method="GET").run().status == 404
        assert httpp.Request(f"{url}/files/api/zip?path=/&name=nope", method="GET").run().status == 404
        too_many = httpp.Request(f"{url}/files/api/zip?path=/", method="GET").run()
        assert too_many.status == 413 and "too many files" in too_many.text
        assert httpp.Request(f"{url}/files/api/zip?path=/&name=a.txt", method="GET").run().status == 200


def test_the_credit_line_is_on_by_default_and_powered_by_false_turns_it_off(tree):
    app = Server()
    FileBrowser(tree).mount(app, "/on")
    FileBrowser(tree, powered_by=False).mount(app, "/off")
    with running(app) as url:
        on, off = httpp.Request(f"{url}/on", method="GET").run().text, httpp.Request(f"{url}/off", method="GET").run().text
        assert '"powered":true' in on and "https://github.com/mohammadraziei/httpp" in on
        assert '"powered":false' in off


def test_the_zip_setters_chain_and_refuse_a_bad_value(tree):
    fb = FileBrowser(tree)
    assert fb.powered_by(False) is fb and fb.zip(True) is fb and fb.max_zip_entries(10) is fb
    with pytest.raises(ValueError):
        fb.max_zip_entries(0)
    with pytest.raises(ValueError):
        FileBrowser(tree, max_zip_entries=0)


def test_browse_command_no_powered_by_hides_the_credit(tree, httpp_exe, stop_process):
    port = _free_port()
    proc = subprocess.Popen([*httpp_exe, "browse", str(tree), "--port", str(port), "--no-powered-by"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        url = f"http://127.0.0.1:{port}"
        for _ in range(100):
            r = httpp.Request(url, method="GET").run()
            if r.status != 0:
                break
            time.sleep(0.1)
        assert r.status == 200 and '"powered":false' in r.text, (r.status, proc.poll())
    finally:
        stop_process(proc)


@pytest.mark.parametrize("flag, zip_on", [(None, True), ("--no-zip", False)])
def test_browse_command_offers_zip_and_no_zip_turns_it_off(tree, httpp_exe, stop_process, flag, zip_on):
    port = _free_port()
    proc = subprocess.Popen([*httpp_exe, "browse", str(tree), "--port", str(port), *([flag] if flag else [])], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        url = f"http://127.0.0.1:{port}"
        for _ in range(100):
            r = httpp.Request(f"{url}/api/zip?path=/sub", method="GET").run()
            if r.status != 0:
                break
            time.sleep(0.1)
        assert (r.status == 200) == zip_on, (r.status, proc.poll())
        if zip_on:
            assert _archive(r.content).namelist() == ["sub/", "sub/b.txt"]
        assert f'"zip":{"true" if zip_on else "false"}' in httpp.Request(url, method="GET").run().text
    finally:
        stop_process(proc)
