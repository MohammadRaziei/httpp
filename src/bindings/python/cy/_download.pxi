# Download, SHA-256 helpers, Bar. Included by httpp_cy.pyx (textual include: order matters, see the list there).

cdef class DownloadResult:
    """Result of Download.run(): ok, status (0 if no response), error (text, empty when ok),
    skipped (True when resume() found the complete file and made no transfer; ok is True too),
    sha256 (the digest the file was checked against; empty unless sha256()/sha256_url() was used)."""
    cdef cbool _ok
    cdef cbool _skipped
    cdef int _status
    cdef bytes _error
    cdef bytes _sha256

    def __init__(self, cbool ok, int status, bytes error, cbool skipped=False, bytes sha256=b""):
        self._ok = ok
        self._status = status
        self._error = error
        self._skipped = skipped
        self._sha256 = sha256

    @property
    def sha256(self):
        return self._sha256.decode("ascii")

    @property
    def skipped(self):
        return self._skipped

    @property
    def ok(self):
        return self._ok

    @property
    def status(self):
        return self._status

    @property
    def error(self):
        return self._error.decode("utf-8", errors="replace")


cdef class Download:
    """Fluent builder over httpp::download:

        Download(url, path).run()
        Download(url).output(path).enable_progress().run()
        Download(url, path).resume().run()      # wget -c: skip a complete file, continue a .part
        await Download(url, path).run_async()   # doesn't block the event loop

    follow_redirects (on by default; off = a 3xx is the final answer), resume (off by default;
    needs a server that answers HEAD and Range, else it quietly degrades to a fresh download),
    force (with resume: ignore existing files and start from byte 0).
    """
    cdef cpp_download* _dl

    def __init__(self, str url, str dest_path=""):
        self._dl = new cpp_download(url.encode("utf-8"), dest_path.encode("utf-8"))

    def __dealloc__(self):
        if self._dl is not NULL:
            del self._dl

    def output(self, str dest_path):
        self._dl.output(dest_path.encode("utf-8"))
        return self

    def enable_progress(self, cbool enable=True):
        self._dl.enable_progress(enable)
        return self

    def disable_progress(self):
        self._dl.disable_progress()
        return self

    def follow_redirects(self, cbool enable=True):
        self._dl.follow_redirects(enable)
        return self

    def resume(self, cbool enable=True):
        self._dl.resume(enable)
        return self

    def force(self, cbool enable=True):
        self._dl.force(enable)
        return self

    def sha256(self, str hex):
        """Verify the finished file against this SHA-256 (64 hex digits, any case); on a mismatch the
        file is deleted and the result is not ok."""
        self._dl.sha256(hex.encode("utf-8"))
        return self

    def sha256_url(self, str checksum_url=""):
        """Read the expected SHA-256 from a checksum file (as `sha256sum` writes it) fetched before the
        download; empty means `<download url>.sha256`. Overrides sha256()."""
        self._dl.sha256_url(checksum_url.encode("utf-8"))
        return self

    def set_timeout(self, long seconds):
        """Seconds for connecting, reading and writing; 0 or less keeps the defaults."""
        self._dl.set_timeout(seconds)
        return self

    def set_max_wait(self, double seconds):
        """Seconds a lock that is not being updated is waited for before it is taken over (default 2);
        values <= 0 are ignored. (C++ takes milliseconds; Python uses seconds, like set_timeout.)"""
        if not seconds < 1e9:  # also catches nan and inf
            raise ValueError("max_wait is out of range")
        self._dl.set_max_wait(cpp_milliseconds(<long long>(seconds * 1000.0)))
        return self

    def run(self):
        cdef download_result res
        with nogil:
            res = self._dl.run()
        return DownloadResult(res.ok, res.status, res.error, res.skipped, res.sha256)

    def run_async(self):
        """Run the download without blocking the event loop; `await` the DownloadResult. Like
        Request.run_async, the blocking call runs on the loop's default thread-pool executor."""
        import asyncio
        return asyncio.get_running_loop().run_in_executor(None, self.run)


def sha256_hex(data):
    """SHA-256 of a bytes-like object as 64 lowercase hex characters (C++: httpp::sha256_hex).
    Text must be encoded first (a str raises TypeError)."""
    # ponytail: no typed memoryview here. Cython's memoryview needs Py_buffer, which the stable ABI
    # only has from Python 3.11, and the abi3 wheel targets 3.9 (pyproject.toml, py-api = "cp39").
    # bytes is read in place; bytearray / memoryview / array.array are copied once. Upgrade path:
    # raise py-api to cp311 and go back to `const unsigned char[::1] data not None` (no copy).
    cdef bytes raw
    if type(data) is bytes:
        raw = data
    else:
        raw = memoryview(data).tobytes()  # TypeError for str, int, list and other non-buffers
    cdef size_t n = len(raw)
    cdef const char* p = raw
    cdef string res
    with nogil:
        res = cpp_sha256_hex(p, n)
    return res.decode("ascii")


def sha256_file(path):
    """SHA-256 of a whole file, read in chunks. RuntimeError if it cannot be opened or read
    (C++: httpp::sha256_file)."""
    import os
    cdef string p = os.fspath(path).encode("utf-8")
    cdef string res
    with nogil:
        res = cpp_sha256_file(p)
    return res.decode("ascii")


def parse_sha256_checksum(str text, str file_name):
    """Digest (lowercase) from the text of a checksum file: one line `<64 hex>` optionally followed by
    `*`/spaces and a file name that must equal file_name. RuntimeError for anything else
    (C++: httpp::parse_sha256_checksum)."""
    return cpp_parse_sha256_checksum(text.encode("utf-8"), file_name.encode("utf-8")).decode("ascii")


cdef class Bar:
    """tqdm-like terminal progress bar with a known total, drawn on stderr (so piping a program's
    output stays clean). Also available as httpp.progress.bar:

        bar = httpp.progress.bar(100, "working")
        bar.update()            # advance by 1 (or update(n))
        bar.set_progress(50)    # absolute position, clamped to total
        bar.finish()
    """
    cdef cpp_bar* _bar

    def __init__(self, size_t total, str description=""):
        self._bar = new cpp_bar(total, description.encode("utf-8"))

    def __dealloc__(self):
        if self._bar is not NULL:
            del self._bar

    def update(self, size_t n=1):
        self._bar.update(n)

    def set_progress(self, size_t current):
        self._bar.set_progress(current)

    def finish(self):
        self._bar.finish()

    @property
    def total(self):
        return self._bar.total()

    @property
    def current(self):
        return self._bar.current()

    @property
    def finished(self):
        return self._bar.is_finished()


cdef bytes _query_text(object value):
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, bytes):
        return value
    if isinstance(value, (int, float)):
        return str(value).encode("utf-8")
    raise TypeError(f"a query name or value must be str, bytes, int or float, not {type(value).__name__}")
