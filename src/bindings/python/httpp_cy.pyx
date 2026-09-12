# distutils: language = c++
# cython: language_level=3

"""
Cython bindings over the httpp C++ core (include/httpp.h). This is what
replaces libcurl on the Python side, for both making requests (Client) and
serving HTTP (Server) — the `httpp` CLI (`httpp server ...`, analogous to
`python -m http.server`) is a thin wrapper around Server.
"""

from libcpp.string cimport string
from libcpp cimport bool as cbool


cdef extern from "httpp/client.hpp" namespace "httpp":
    cdef cppclass response:
        int status
        string body
        cbool ok()

    cdef cppclass cpp_client "httpp::client":
        cpp_client(string host, int port) except +
        response get(string path) except +

        @staticmethod
        response fetch(string full_url) except +


cdef extern from "httpp/server.hpp" namespace "httpp":
    cdef cppclass cpp_server "httpp::server":
        cpp_server() except +
        void serve_directory(string mount_path, string local_dir) except +
        int bind_to_any_port(string host) except +
        void listen_after_bind() except + nogil
        void listen(string host, int port) except + nogil
        void stop() except +


cdef extern from "httpp/download.hpp" namespace "httpp":
    cdef cppclass download_result:
        cbool ok
        int status
        string error

    cdef cppclass cpp_download_file "httpp::download_file":
        cpp_download_file(string url) except +
        cpp_download_file& output(string dest_path) except +
        cpp_download_file& enable_progress(cbool enable) except +
        cpp_download_file& disable_progress() except +
        download_result run() except + nogil


cdef class Response:
    """A response to an HTTP request made via Client.get()."""
    cdef int _status
    cdef bytes _body

    def __init__(self, int status, bytes body):
        self._status = status
        self._body = body

    @property
    def status(self):
        return self._status

    @property
    def body(self):
        return self._body.decode("utf-8", errors="replace")

    @property
    def ok(self):
        return 200 <= self._status < 300


cdef class Client:
    """Minimal HTTP client. Replaces reaching for libcurl/requests for
    simple request/response use cases."""
    cdef cpp_client* _cli

    def __init__(self, str host, int port):
        self._cli = new cpp_client(host.encode("utf-8"), port)

    def __dealloc__(self):
        if self._cli is not NULL:
            del self._cli

    def get(self, str path):
        cdef response res = self._cli.get(path.encode("utf-8"))
        return Response(res.status, res.body)

    @staticmethod
    def fetch(str full_url):
        """Parse `full_url` and GET it in one call — no manual URL parsing
        needed on the Python side either (see httpp::client::fetch)."""
        cdef response res = cpp_client.fetch(full_url.encode("utf-8"))
        return Response(res.status, res.body)


cdef class Server:
    """Minimal HTTP server. Replaces spinning up cpp-httplib/libcurl
    yourself — this is also what `httpp server` (the CLI, similar to
    Python's own `python -m http.server`) is built on."""
    cdef cpp_server* _srv

    def __init__(self):
        self._srv = new cpp_server()

    def __dealloc__(self):
        if self._srv is not NULL:
            del self._srv

    def serve_directory(self, str mount_path, str local_dir):
        """Serve `local_dir` under `mount_path`, like `python -m http.server`."""
        self._srv.serve_directory(mount_path.encode("utf-8"), local_dir.encode("utf-8"))

    def bind_to_any_port(self, str host):
        return self._srv.bind_to_any_port(host.encode("utf-8"))

    def listen_after_bind(self):
        with nogil:
            self._srv.listen_after_bind()

    def listen(self, str host, int port):
        cdef string h = host.encode("utf-8")
        with nogil:
            self._srv.listen(h, port)

    def stop(self):
        self._srv.stop()


cdef class DownloadResult:
    """Result of DownloadFile.run()."""
    cdef cbool _ok
    cdef int _status
    cdef bytes _error

    def __init__(self, cbool ok, int status, bytes error):
        self._ok = ok
        self._status = status
        self._error = error

    @property
    def ok(self):
        return self._ok

    @property
    def status(self):
        return self._status

    @property
    def error(self):
        return self._error.decode("utf-8", errors="replace")


cdef class DownloadFile:
    """Fluent builder over httpp::download_file:

        DownloadFile(url).output(path).enable_progress().run()
    """
    cdef cpp_download_file* _dl

    def __init__(self, str url):
        self._dl = new cpp_download_file(url.encode("utf-8"))

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

    def run(self):
        cdef download_result res
        with nogil:
            res = self._dl.run()
        return DownloadResult(res.ok, res.status, res.error)


def download(str url, str dest_path, cbool show_progress=True):
    """Download `url` to `dest_path`, with a tqdm-like terminal progress
    bar unless show_progress=False. Shorthand for
    DownloadFile(url).output(dest_path).run()."""
    return DownloadFile(url).output(dest_path).enable_progress(show_progress).run()
