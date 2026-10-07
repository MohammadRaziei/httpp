"""Python parity for the C++ download builder, progress bar/range and URL helpers."""

import asyncio
import threading
import time
from contextlib import contextmanager

import pytest

import httpp
from httpp import Download, Server, URL, progress

SIZE = 2_000_000
CONTENT = bytes(i % 251 for i in range(SIZE))


@contextmanager
def file_server(tmp_path, *, log, extra=None):
    """Serves tmp_path/big.bin under /files (HEAD and Range work), logs every request seen.
    `extra(app)` may register more routes."""
    (tmp_path / "big.bin").write_bytes(CONTENT)
    app = Server()
    if extra:
        extra(app)

    @app.before_request
    def record(request):
        log.append((request.method, request.path, request.headers.get("range")))

    @app.get("/redirect")
    def redirect():
        return httpp.redirect("/files/big.bin")

    app.serve_directory("/files", str(tmp_path))
    port = app.bind_to_any_port("127.0.0.1")
    th = threading.Thread(target=app.listen_after_bind)
    th.start()
    time.sleep(0.05)
    try:
        yield f"http://127.0.0.1:{port}"
    finally:
        app.stop()
        th.join()


def dl(url, dest):
    return Download(url, str(dest)).disable_progress()


def gets(log):
    return [entry for entry in log if entry[0] == "GET" and entry[1].startswith("/files/")]


# --- Download --------------------------------------------------------------------------------


def test_a_fresh_download_is_complete_and_leaves_no_part_file(tmp_path):
    log = []
    out = tmp_path / "out" / "copy.bin"
    out.parent.mkdir()
    with file_server(tmp_path, log=log) as url:
        res = dl(f"{url}/files/big.bin", out).run()
    assert (res.ok, res.status, res.skipped, res.error) == (True, 200, False, "")
    assert out.read_bytes() == CONTENT
    assert not (out.parent / "copy.bin.part").exists()


def test_redirects_are_followed_by_default_and_can_be_turned_off(tmp_path):
    log = []
    out = tmp_path / "r.bin"
    with file_server(tmp_path, log=log) as url:
        res = dl(f"{url}/redirect", out).run()
        assert (res.ok, res.status) == (True, 200) and out.read_bytes() == CONTENT
        out.unlink()
        res = dl(f"{url}/redirect", out).follow_redirects(False).run()
        assert (res.ok, res.status) == (False, 302)
        assert not out.exists()  # a failed download leaves nothing behind
        out2 = tmp_path / "r2.bin"
        assert dl(f"{url}/redirect", out2).follow_redirects(True).run().ok


def test_resume_continues_a_part_file_with_a_range_request(tmp_path):
    log = []
    out = tmp_path / "resumed.bin"
    (tmp_path / "resumed.bin.part").write_bytes(CONTENT[:700_000])
    with file_server(tmp_path, log=log) as url:
        res = dl(f"{url}/files/big.bin", out).resume().run()
    assert (res.ok, res.skipped) == (True, False)
    assert out.read_bytes() == CONTENT  # byte for byte, no gap and no duplicate
    assert not (tmp_path / "resumed.bin.part").exists()
    assert [g[2] for g in gets(log)] == ["bytes=700000-"]


def test_resume_skips_a_complete_file_without_transferring_anything(tmp_path):
    log = []
    out = tmp_path / "done.bin"
    with file_server(tmp_path, log=log) as url:
        assert dl(f"{url}/files/big.bin", out).resume().run().skipped is False
        log.clear()
        res = dl(f"{url}/files/big.bin", out).resume().run()
    assert (res.ok, res.skipped, res.status) == (True, True, 200)
    assert gets(log) == []  # only the HEAD probe went out
    assert [e[0] for e in log] == ["HEAD"]
    assert out.read_bytes() == CONTENT


def test_force_redownloads_even_when_resume_would_skip(tmp_path):
    log = []
    out = tmp_path / "forced.bin"
    out.write_bytes(b"\0" * SIZE)  # right size, wrong content: resume alone would trust it
    with file_server(tmp_path, log=log) as url:
        assert dl(f"{url}/files/big.bin", out).resume().run().skipped is True
        assert out.read_bytes() != CONTENT
        log.clear()
        res = dl(f"{url}/files/big.bin", out).resume().force().run()
    assert (res.ok, res.skipped) == (True, False)
    assert out.read_bytes() == CONTENT
    assert [g[2] for g in gets(log)] == [None]  # from byte 0, no Range


def test_force_alone_changes_nothing_because_resume_is_what_reads_existing_files(tmp_path):
    log = []
    out = tmp_path / "f.bin"
    out.write_bytes(b"old")
    with file_server(tmp_path, log=log) as url:
        res = dl(f"{url}/files/big.bin", out).force().run()
    assert (res.ok, res.skipped) == (True, False)
    assert out.read_bytes() == CONTENT
    assert [e[0] for e in log] == ["GET"]  # no HEAD probe: resume is off


def test_failures_report_why_and_leave_no_file(tmp_path):
    log = []
    out = tmp_path / "nope.bin"
    with file_server(tmp_path, log=log) as url:
        res = dl(f"{url}/files/missing.bin", out).run()
        assert (res.ok, res.status) == (False, 404) and "404" in res.error
        res = dl(f"{url}/files/missing.bin", out).resume().run()
        assert (res.ok, res.skipped) == (False, False)
    assert not out.exists() and not (tmp_path / "nope.bin.part").exists()

    res = dl("not a url", out).run()
    assert (res.ok, res.status) == (False, 0) and "invalid" in res.error.lower()
    res = dl("http://127.0.0.1:1/x", out).run()  # nothing listens there
    assert (res.ok, res.status) == (False, 0) and res.error


def slow_route(app):
    @app.get("/slow")
    def slow():
        def pieces():
            for _ in range(8):  # about 0.4 s in total
                time.sleep(0.05)
                yield b"x" * 1000

        return pieces()


def test_run_async_does_not_block_the_event_loop_and_runs_downloads_concurrently(tmp_path):
    # The downloads must take long enough to be observable: on localhost a plain file is done in
    # a few ms, before a 5 ms ticker could even fire. A synchronous run() inside the coroutine
    # gives ticks == 0 and ~1.2 s here (checked by hand); run_async gives ~75 ticks and ~0.4 s.
    log = []
    with file_server(tmp_path, log=log, extra=slow_route) as url:

        async def main():
            ticks = 0

            async def ticker():
                nonlocal ticks
                while True:
                    await asyncio.sleep(0.005)
                    ticks += 1

            task = asyncio.ensure_future(ticker())
            t0 = time.monotonic()
            results = await asyncio.gather(
                *(dl(f"{url}/slow", tmp_path / f"a{i}.bin").run_async() for i in range(3)))
            took = time.monotonic() - t0
            task.cancel()
            return results, ticks, took

        results, ticks, took = asyncio.run(main())
    assert all(r.ok for r in results)
    assert all((tmp_path / f"a{i}.bin").stat().st_size == 8000 for i in range(3))
    assert ticks >= 15, f"the event loop was starved ({ticks} ticks)"
    assert took < 1.0, f"the downloads ran one after the other ({took:.2f}s)"


def test_run_async_needs_a_running_loop(tmp_path):
    with pytest.raises(RuntimeError):
        Download("http://127.0.0.1:1/x", str(tmp_path / "x")).run_async()


# --- progress --------------------------------------------------------------------------------


def test_bar_tracks_progress_clamps_and_draws_on_stderr_only(capfd):
    bar = progress.bar(10, "working")
    assert (bar.total, bar.current, bar.finished) == (10, 0, False)
    bar.update()
    bar.update(4)
    assert (bar.current, bar.finished) == (5, False)
    bar.set_progress(8)
    assert bar.current == 8
    bar.set_progress(99)  # clamped to the total, like the C++ bar
    assert (bar.current, bar.finished) == (10, True)
    bar.finish()
    out, err = capfd.readouterr()
    assert out == ""
    assert "working" in err and "100%" in err


def test_bar_refuses_negative_numbers():
    with pytest.raises(OverflowError):
        progress.bar(-1)
    bar = progress.bar(5)
    with pytest.raises(OverflowError):
        bar.update(-1)


def test_range_yields_every_index_and_advances_the_bar_once_per_step(capfd):
    r = progress.range(5, "steps")
    assert len(r) == 5
    seen = []
    for i in r:
        seen.append(i)
    assert seen == [0, 1, 2, 3, 4]
    out, err = capfd.readouterr()
    assert out == "" and "steps" in err and "100%" in err
    assert list(progress.range(0)) == []


def test_httpp_exposes_progress_and_url():
    assert httpp.progress is progress and httpp.URL is URL
    assert "URL" in httpp.__all__ and "progress" in httpp.__all__


# --- URL -------------------------------------------------------------------------------------


def test_url_parse_exposes_the_parts_and_never_raises():
    u = URL.parse("https://example.com:8443/a/b?x=1&y=2")
    assert (u.valid, u.scheme, u.host, u.port, u.path, u.query) == (
        True, "https", "example.com", 8443, "/a/b", "x=1&y=2")
    assert URL("http://example.com/p").port == 80
    assert URL("https://example.com/p").port == 443
    for bad in ("nonsense", "", "ftp://example.com/x"):
        assert URL.parse(bad).valid is False
    assert "example.com" in repr(u) and "invalid" in repr(URL("nonsense"))
    with pytest.raises(AttributeError):
        u.host = "other"  # read-only


def test_url_encode_and_decode_match_the_cpp_rules():
    assert URL.encode_component("a b/é~-._") == "a%20b%2F%C3%A9~-._"
    assert URL.encode_component("") == ""
    assert URL.decode_component("a%20b+c") == "a b+c"  # "+" stays "+"
    assert URL.decode_component("%41%42%43") == "ABC"
    assert URL.decode_component("%zz%4") == "%zz%4"  # malformed escapes pass through
    assert URL.decode_component("%ff") == "\ufffd"  # not UTF-8: replaced, never an exception
    for text in ("plain", "a b&c=d/é?#%", "日本語 ✓"):
        assert URL.decode_component(URL.encode_component(text)) == text


def test_url_build_query_encodes_names_and_values():
    assert URL.build_query([("a", "1"), ("b", "x y")]) == "a=1&b=x%20y"
    assert URL.build_query({"q": "a&b=c", "n": 2, "f": 1.5, "raw": b"z z"}) == (
        "q=a%26b%3Dc&n=2&f=1.5&raw=z%20z")
    assert URL.build_query({}) == ""
    assert URL.build_query([("k", "1"), ("k", "2")]) == "k=1&k=2"  # repeated names kept
    with pytest.raises(TypeError):
        URL.build_query({"a": None})
    with pytest.raises(TypeError):
        URL.build_query([("a", ["list"])])
