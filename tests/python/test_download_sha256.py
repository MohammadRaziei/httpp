"""Python bindings for httpp::sha256_hex/sha256_file/parse_sha256_checksum (in `httpp.utils`) and Download.sha256/sha256_url/
set_timeout/set_max_wait, mirroring tests/cpp/test_download.cpp."""

import hashlib
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Download, Server

BODY = b"the quick brown fox"
DIGEST = hashlib.sha256(BODY).hexdigest()


@contextmanager
def file_server(checksum_body="", checksum_status=200):
    app = Server()

    @app.get("/f.bin")
    def f():
        return BODY

    @app.get("/f.bin.sha256")
    def f_sha():
        return checksum_body, checksum_status

    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        yield f"http://127.0.0.1:{port}"
    finally:
        app.stop()
        th.join()


# ---- free functions -----------------------------------------------------------------------------

def test_sha256_hex_known_vectors():
    assert httpp.utils.sha256_hex(b"abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    assert httpp.utils.sha256_hex(b"") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    assert httpp.utils.sha256_hex(bytearray(BODY)) == DIGEST
    assert httpp.utils.sha256_hex(memoryview(BODY)) == DIGEST


def test_sha256_hex_agrees_with_hashlib_on_many_sizes():
    """The point of binding it: Python can check the C++ implementation against hashlib."""
    import os

    for n in list(range(0, 200)) + [255, 256, 257, 1000, 4096, 65535, 65536, 1_000_003]:
        data = os.urandom(n)
        assert httpp.utils.sha256_hex(data) == hashlib.sha256(data).hexdigest(), n


def test_the_helpers_live_in_utils_not_in_the_main_namespace():
    for name in ("sha256_hex", "sha256_file", "parse_sha256_checksum"):
        assert hasattr(httpp.utils, name) and not hasattr(httpp, name)


def test_sha256_hex_rejects_text():
    with pytest.raises(TypeError):
        httpp.utils.sha256_hex("abc")  # bytes only: encode explicitly


def test_sha256_file_matches_hashlib_and_a_missing_file_raises(tmp_path):
    big = tmp_path / "big.bin"
    big.write_bytes(bytes(i % 251 for i in range(3_000_000)))  # several read chunks
    assert httpp.utils.sha256_file(str(big)) == hashlib.sha256(big.read_bytes()).hexdigest()
    with pytest.raises(RuntimeError):
        httpp.utils.sha256_file(str(tmp_path / "nope.bin"))


def test_parse_sha256_checksum():
    assert httpp.utils.parse_sha256_checksum(DIGEST + "\n", "f.bin") == DIGEST
    assert httpp.utils.parse_sha256_checksum(DIGEST + "  f.bin\n", "f.bin") == DIGEST
    assert httpp.utils.parse_sha256_checksum(DIGEST + " *f.bin", "f.bin") == DIGEST
    assert httpp.utils.parse_sha256_checksum(DIGEST.upper(), "f.bin") == DIGEST  # returned in lowercase
    for bad in (DIGEST + "  other.bin", DIGEST + "\n" + DIGEST, DIGEST[:10], "<html>404</html>", ""):
        with pytest.raises(RuntimeError):
            httpp.utils.parse_sha256_checksum(bad, "f.bin")


# ---- Download ------------------------------------------------------------------------------------

def test_a_matching_digest_is_accepted_in_either_case(tmp_path):
    dest = tmp_path / "f.bin"
    with file_server() as base:
        res = Download(base + "/f.bin", str(dest)).disable_progress().sha256(DIGEST).run()
        assert res.ok and res.sha256 == DIGEST and dest.read_bytes() == BODY
        assert Download(base + "/f.bin", str(dest)).disable_progress().sha256(DIGEST.upper()).run().ok


def test_a_wrong_digest_fails_and_leaves_no_file(tmp_path):
    dest = tmp_path / "f.bin"
    with file_server() as base:
        res = Download(base + "/f.bin", str(dest)).disable_progress().sha256("0" * 64).run()
    assert not res.ok and DIGEST in res.error and not dest.exists()


def test_a_malformed_digest_fails_before_any_request(tmp_path):
    dest = tmp_path / "f.bin"
    res = Download("http://127.0.0.1:1/f.bin", str(dest)).disable_progress().sha256("not-hex").run()
    assert not res.ok and res.status == 0 and not dest.exists()


def test_sha256_url_default_and_explicit_checksum_file(tmp_path):
    dest = tmp_path / "f.bin"
    with file_server(DIGEST + "  f.bin\n") as base:
        res = Download(base + "/f.bin", str(dest)).disable_progress().sha256_url().run()
        assert res.ok and res.sha256 == DIGEST
        dest.unlink()
        res = Download(base + "/f.bin", str(dest)).disable_progress().sha256_url(base + "/f.bin.sha256").run()
        assert res.ok and res.sha256 == DIGEST


def test_sha256_url_with_an_error_page_downloads_nothing(tmp_path):
    dest = tmp_path / "f.bin"
    with file_server("<html>not found</html>", 404) as base:
        res = Download(base + "/f.bin", str(dest)).disable_progress().sha256_url().run()
    assert not res.ok and not dest.exists()


def test_result_sha256_is_empty_without_verification(tmp_path):
    with file_server() as base:
        res = Download(base + "/f.bin", str(tmp_path / "f.bin")).disable_progress().run()
    assert res.ok and res.sha256 == ""


def test_set_timeout_and_set_max_wait_are_chainable(tmp_path):
    dest = tmp_path / "f.bin"
    with file_server() as base:
        dl = Download(base + "/f.bin", str(dest)).disable_progress()
        assert dl.set_timeout(5) is dl
        assert dl.set_max_wait(0.5) is dl
        assert dl.set_max_wait(0) is dl  # <= 0 is ignored, like in C++
        assert dl.run().ok and dest.read_bytes() == BODY


def test_run_async_returns_the_sha256_too(tmp_path):
    import asyncio

    async def go(base):
        return await Download(base + "/f.bin", str(tmp_path / "f.bin")).disable_progress().sha256(DIGEST).run_async()

    with file_server() as base:
        assert asyncio.run(go(base)).sha256 == DIGEST
