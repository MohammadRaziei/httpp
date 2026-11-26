"""Client uploads: Request.field, file, file_data, body_file, progress, `httpp curl -F` and a streamed -T (ADR-0113)."""

import json
import subprocess
import threading
import time
from contextlib import contextmanager

import pytest
from httpp import Request, Server


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
def app():
    app = Server()

    @app.post("/upload")
    def upload(request):
        form = request.form()
        return {
            "fields": {k: form.field(k) for k in ("title", "k")},
            "files": [
                {"field": f.field, "name": f.filename, "type": f.content_type, "size": f.size, "first": f.data[:1].decode(), "last": f.data[-1:].decode()}
                for f in form.files
            ],
        }

    @app.put("/store")
    def store(request):
        body = request.body
        return {"size": len(body), "type": request.headers.get("content-type"), "length": request.headers.get("content-length"),
                "first": body[:1].decode(), "last": body[-1:].decode()}

    return app


def make_file(path, size):
    data = bytearray(b"m" * size)
    if size:
        data[0:1] = b"a"
        data[-1:] = b"z"
    path.write_bytes(bytes(data))
    return str(path)


def test_fields_and_a_file_arrive_as_a_form(app, tmp_path):
    notes = make_file(tmp_path / "notes.txt", 10)
    with running(app) as url:
        res = Request(url + "/upload").field("title", "hi there").file("doc", notes).run()
    assert res.status == 200
    body = res.json()
    assert body["fields"]["title"] == "hi there"
    assert body["files"] == [{"field": "doc", "name": "notes.txt", "type": "text/plain", "size": 10, "first": "a", "last": "z"}]


def test_name_type_and_bytes_can_be_given(app, tmp_path):
    notes = make_file(tmp_path / "x.bin", 4)
    with running(app) as url:
        res = (
            Request(url + "/upload")
            .file("one", notes, filename="photo.png", content_type="image/png")
            .file_data("two", b"xyz", "notes.txt")
            .file_data("three", "q", "blob")
            .run()
        )
    files = {f["field"]: f for f in res.json()["files"]}
    assert (files["one"]["name"], files["one"]["type"], files["one"]["size"]) == ("photo.png", "image/png", 4)
    assert (files["two"]["name"], files["two"]["type"], files["two"]["size"]) == ("notes.txt", "text/plain", 3)
    assert (files["three"]["type"], files["three"]["size"]) == ("application/octet-stream", 1)


def test_keywords_build_the_same_request(app, tmp_path):
    notes = make_file(tmp_path / "notes.txt", 7)
    seen = []
    with running(app) as url:
        res = Request(url + "/upload", fields={"title": "t", "k": "v"}, files={"doc": (notes, "renamed.txt", "text/x-test")},
                      progress=lambda sent, total: seen.append((sent, total))).run()
    body = res.json()
    assert body["fields"] == {"title": "t", "k": "v"}
    assert (body["files"][0]["name"], body["files"][0]["type"]) == ("renamed.txt", "text/x-test")
    assert seen and seen[-1][0] == seen[-1][1]


def test_a_big_file_reports_progress_in_pieces(app, tmp_path):
    size = 300_000
    big = make_file(tmp_path / "big.bin", size)
    seen = []
    with running(app) as url:
        res = Request(url + "/upload").field("k", "v").file("big", big).progress(lambda sent, total: seen.append((sent, total))).run()
    assert res.json()["files"][0]["size"] == size
    sent = [s for s, _ in seen]
    assert len(seen) > 3 and sent == sorted(sent)
    assert seen[0][0] == 0 and seen[-1][0] == seen[-1][1] > size


def test_a_missing_file_is_an_error_in_the_response(app, tmp_path):
    with running(app) as url:
        res = Request(url + "/upload").file("doc", str(tmp_path / "nope.bin")).run()
    assert res.status == 0 and res.error == "file" and "nope.bin" in res.error_message


def test_mixing_kinds_of_body_is_an_error(app, tmp_path):
    notes = make_file(tmp_path / "n.txt", 2)
    with running(app) as url:
        a = Request(url + "/upload").data("x=1").field("k", "v").run()
        b = Request(url + "/upload").field("k", "v").body_file(notes).run()
    assert a.error == "file" and b.error == "file"


def test_body_file_puts_the_whole_file(app, tmp_path):
    whole = make_file(tmp_path / "whole.json", 250_000)
    with running(app) as url:
        res = Request(url + "/store").body_file(whole).run()
        typed = Request(url + "/store").body_file(whole).content_type("text/x-mine").run()
    body = res.json()
    assert (body["size"], body["type"], body["length"], body["first"], body["last"]) == (250_000, "application/json", "250000", "a", "z")
    assert typed.json()["type"] == "text/x-mine"


def test_a_progress_error_is_raised_after_the_upload(app, tmp_path):
    notes = make_file(tmp_path / "n.txt", 5)

    def boom(sent, total):
        raise RuntimeError("from progress")

    with running(app) as url:
        with pytest.raises(RuntimeError, match="from progress"):
            Request(url + "/upload").file("doc", notes).progress(boom).run()


def test_clone_carries_parts_and_progress(app):
    seen = []
    base = Request("http://127.0.0.1:1/x").field("k", "v").file_data("d", b"abc", "a.txt").progress(lambda s, t: seen.append(s))
    copy = base.clone()
    with running(app) as url:
        copy2 = Request(url + "/upload").field("title", "t").file_data("d", b"abc", "a.txt").progress(lambda s, t: seen.append(s)).clone()
        res = copy2.run()
    assert res.json()["files"][0]["size"] == 3 and seen
    assert copy is not base


def test_async_run_uploads(app, tmp_path):
    import asyncio

    notes = make_file(tmp_path / "n.txt", 20)

    async def go(url):
        return await Request(url + "/upload").field("title", "t").file("doc", notes).run_async()

    with running(app) as url:
        res = asyncio.run(go(url))
    assert res.json()["files"][0]["size"] == 20


# --- the command line ---

def curl(httpp_exe, *args, input=None):
    return subprocess.run([*httpp_exe, "curl", *args], capture_output=True, text=True, input=input, timeout=60)


def test_curl_F_sends_fields_and_files(httpp_exe, app, tmp_path):
    notes = make_file(tmp_path / "notes.txt", 10)
    text = tmp_path / "value.txt"
    text.write_text("from a file")
    with running(app) as url:
        res = curl(httpp_exe, f"{url}/upload", "-F", "title=hello", "-F", f"doc=@{notes};type=text/x-demo;filename=n.txt", "-F", f"k=<{text}")
        literal = curl(httpp_exe, f"{url}/upload", "--form-string", "title=@not-a-file", "-F", f"doc=@{notes}")
        missing = curl(httpp_exe, f"{url}/upload", "-F", f"doc=@{tmp_path / 'nope'}")
        bad = curl(httpp_exe, f"{url}/upload", "-F", "novalue")
        both = curl(httpp_exe, f"{url}/upload", "-F", "a=b", "-d", "x=1")
    body = json.loads(res.stdout)
    assert body["fields"] == {"title": "hello", "k": "from a file"}
    assert (body["files"][0]["name"], body["files"][0]["type"]) == ("n.txt", "text/x-demo")
    assert json.loads(literal.stdout)["fields"]["title"] == "@not-a-file"
    assert missing.returncode == 26 and "cannot read" in missing.stderr
    assert bad.returncode == 1 and "malformed" in bad.stderr
    assert both.returncode == 1 and "-F" in both.stderr


def test_curl_T_streams_a_big_file(httpp_exe, app, tmp_path):
    big = make_file(tmp_path / "big.dat", 400_000)
    with running(app) as url:
        res = curl(httpp_exe, f"{url}/store", "-T", big)
    body = json.loads(res.stdout)
    assert (body["size"], body["first"], body["last"], body["length"]) == (400_000, "a", "z", "400000")
