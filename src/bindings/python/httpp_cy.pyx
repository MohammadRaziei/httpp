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
from libcpp.vector cimport vector
from libcpp.pair cimport pair


cdef extern from "httpp/client.hpp" namespace "httpp":
    cdef cppclass response:
        int status
        string body
        vector[pair[string, string]] headers
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
        cbool skipped
        int status
        string error

    cdef cppclass cpp_download "httpp::download":
        cpp_download(string url, string dest_path) except +
        cpp_download& output(string dest_path) except +
        cpp_download& enable_progress(cbool enable) except +
        cpp_download& disable_progress() except +
        download_result run() except + nogil


cdef extern from "httpp/client.hpp" namespace "httpp::client":
    cdef cppclass cpp_curl_request "httpp::client::request":
        cpp_curl_request(string url) except +
        cpp_curl_request& method(string m) except +
        cpp_curl_request& header(string name, string value) except +
        cpp_curl_request& data(string body) except +
        cpp_curl_request& content_type(string type) except +
        response run() except + nogil


cdef class Response:
    """A response to an HTTP request made via Client.get() / Request.run()."""
    cdef int _status
    cdef bytes _body
    cdef list _headers  # list of (str, str) tuples, in server order

    def __init__(self, int status, bytes body, list headers=None):
        self._status = status
        self._body = body
        self._headers = headers or []

    @property
    def status(self):
        return self._status

    @property
    def body(self):
        return self._body.decode("utf-8", errors="replace")

    @property
    def ok(self):
        return 200 <= self._status < 300

    @property
    def headers(self):
        """All response headers, as a list of (name, value) tuples."""
        return list(self._headers)

    def header(self, str name):
        """Case-insensitive lookup of the first matching header, or None."""
        lname = name.lower()
        for k, v in self._headers:
            if k.lower() == lname:
                return v
        return None


cdef _make_response(response res):
    """Convert a C++ httpp::response into a Python Response, headers and all."""
    cdef list headers = [
        (h.first.decode("utf-8", errors="replace"), h.second.decode("utf-8", errors="replace"))
        for h in res.headers
    ]
    return Response(res.status, res.body, headers)


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
        return _make_response(res)

    @staticmethod
    def fetch(str full_url):
        """Parse `full_url` and GET it in one call — no manual URL parsing
        needed on the Python side either (see httpp::client::fetch)."""
        cdef response res = cpp_client.fetch(full_url.encode("utf-8"))
        return _make_response(res)


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
    """Result of Download.run()."""
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


cdef class Download:
    """Fluent builder over httpp::download:

        Download(url, path).run()
        Download(url).output(path).enable_progress().run()
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

    def run(self):
        cdef download_result res
        with nogil:
            res = self._dl.run()
        return DownloadResult(res.ok, res.status, res.error)


def download(str url, str dest_path, cbool show_progress=True):
    """Download `url` to `dest_path`, with a tqdm-like terminal progress
    bar unless show_progress=False. Shorthand for
    Download(url, dest_path).enable_progress(show_progress).run()."""
    return Download(url, dest_path).enable_progress(show_progress).run()

cdef class Request:
    """A small, curl-flavored fluent request builder — the common cases
    only (-X method, -H headers, -d data). NOT a libcurl-compatible shim;
    see Client for the plain request API this is built on.

        Request(url).method("PUT").header("X-Token", "abc").data("body").run()
    """
    cdef cpp_curl_request* _req

    def __init__(self, str url):
        self._req = new cpp_curl_request(url.encode("utf-8"))

    def __dealloc__(self):
        if self._req is not NULL:
            del self._req

    def method(self, str m):
        self._req.method(m.encode("utf-8"))
        return self

    def header(self, str name, str value):
        self._req.header(name.encode("utf-8"), value.encode("utf-8"))
        return self

    def data(self, str body):
        self._req.data(body.encode("utf-8"))
        return self

    def content_type(self, str type_):
        self._req.content_type(type_.encode("utf-8"))
        return self

    def run(self):
        cdef response res
        with nogil:
            res = self._req.run()
        return _make_response(res)
