"""request.form(), Form(), File() and UploadFile: the Python side of C++ request::form() (tests/cpp/test_form.cpp)."""

import threading
import time
from contextlib import contextmanager
from typing import Annotated

import pytest

from httpp import Client, File, Form, FormData, Server, UploadFile

CRLF = "\r\n"


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


def multipart(*parts, boundary="B"):
    """parts: (name, value) or (name, filename, content_type, bytes). Returns (body bytes, content type)."""
    out = b""
    for part in parts:
        if len(part) == 2:
            out += f"--{boundary}{CRLF}Content-Disposition: form-data; name=\"{part[0]}\"{CRLF}{CRLF}".encode() + part[1].encode() + CRLF.encode()
        else:
            name, filename, ctype, data = part
            out += (f"--{boundary}{CRLF}Content-Disposition: form-data; name=\"{name}\"; filename=\"{filename}\"{CRLF}"
                    f"Content-Type: {ctype}{CRLF}{CRLF}").encode() + data + CRLF.encode()
    out += f"--{boundary}--{CRLF}".encode()
    return out, f"multipart/form-data; boundary={boundary}"


def post(base, path, body, ctype):
    return Client(base).post(path, data=body, headers={"Content-Type": ctype})


def test_request_form_reads_fields_and_files():
    app = Server(openapi_url=None)

    @app.post("/up")
    def up(request):
        form = request.form()
        assert isinstance(form, FormData) and request.form() is form  # parsed once
        doc = form.file("doc")
        return {"who": form.field("who"), "tags": form.getlist("tag"), "missing": form.field("x", "dflt"),
                "file": [doc.filename, doc.content_type, doc.size, doc.text()], "none": form.file("nope") is None}

    body, ctype = multipart(("who", "me"), ("tag", "a"), ("tag", "b"), ("doc", "r.txt", "text/plain", b"hello"))
    with running(app) as base:
        r = post(base, "/up", body, ctype)
        assert r.json() == {"who": "me", "tags": ["a", "b"], "missing": "dflt",
                            "file": ["r.txt", "text/plain", 5, "hello"], "none": True}


def test_a_malformed_multipart_body_answers_400_through_request_form():
    app = Server(openapi_url=None)

    @app.post("/up")
    def up(request):
        return {"n": len(request.form().fields)}

    with running(app) as base:
        assert post(base, "/up", b"garbage", "multipart/form-data; boundary=B").status == 400
        assert post(base, "/up", b"a=1&b=2", "application/x-www-form-urlencoded").json() == {"n": 2}


def test_form_and_file_parameters_are_converted_and_checked():
    app = Server(openapi_url=None)

    @app.post("/p")
    async def p(name: str = Form(), age: int = Form(18), doc: UploadFile = File(), many: list[UploadFile] = File(None),
                raw: Annotated[bytes, File()] = b"", tags: list[str] = Form(None)):
        return {"name": name, "age": age, "doc": doc.filename, "many": [f.filename for f in many or []],
                "raw": len(raw), "tags": tags}

    body, ctype = multipart(("name", "n"), ("age", "30"), ("tags", "x"), ("tags", "y"),
                            ("doc", "a.bin", "application/octet-stream", b"\x00\x01"),
                            ("many", "m1", "text/plain", b"1"), ("many", "m2", "text/plain", b"2"),
                            ("raw", "r", "text/plain", b"12345"))
    with running(app) as base:
        r = post(base, "/p", body, ctype)
        assert r.status == 200, r.body
        assert r.json() == {"name": "n", "age": 30, "doc": "a.bin", "many": ["m1", "m2"], "raw": 5, "tags": ["x", "y"]}

        body, ctype = multipart(("name", "n"), ("doc", "a.bin", "text/plain", b"z"))
        assert post(base, "/p", body, ctype).json()["age"] == 18  # the default

        body, ctype = multipart(("age", "old"))  # bad int, missing name, missing file
        r = post(base, "/p", body, ctype)
        assert r.status == 422
        locs = sorted(tuple(e["loc"]) for e in r.json()["detail"])
        assert locs == [("body", "age"), ("body", "doc"), ("body", "name")]

        r = post(base, "/p", b"garbage", "multipart/form-data; boundary=B")
        assert r.status == 422 and r.json()["detail"][0]["type"] == "form_invalid"


def test_an_urlencoded_form_works_for_form_fields_and_a_sync_route():
    app = Server(openapi_url=None)

    @app.post("/login")
    def login(user: str = Form(), password: str = Form()):
        return {"user": user, "len": len(password)}

    with running(app) as base:
        r = post(base, "/login", b"user=ann&password=s%20ecret", "application/x-www-form-urlencoded")
        assert r.json() == {"user": "ann", "len": 7}


def test_the_openapi_document_describes_form_bodies():
    app = Server()

    @app.post("/p")
    def p(name: str = Form(), doc: UploadFile = File(), more: list[UploadFile] = File(None)):
        return "x"

    @app.post("/f")
    def f(n: int = Form(1)):
        return "x"

    paths = app.openapi()["paths"]
    body = paths["/p"]["post"]["requestBody"]["content"]["multipart/form-data"]["schema"]
    assert body["properties"]["doc"] == {"type": "string", "format": "binary", "title": "Doc"}
    assert body["properties"]["more"]["type"] == "array"
    assert body["properties"]["name"]["type"] == "string"
    assert sorted(body["required"]) == ["doc", "name"]
    assert "application/x-www-form-urlencoded" in paths["/f"]["post"]["requestBody"]["content"]
    assert "parameters" not in paths["/p"]["post"]


def test_an_uploadfile_can_be_saved(tmp_path):
    f = UploadFile("doc", "a.txt", "text/plain", b"abc")
    f.save(tmp_path / "out.bin")
    assert (tmp_path / "out.bin").read_bytes() == b"abc" and f.size == 3
