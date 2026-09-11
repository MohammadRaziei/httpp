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


cdef extern from "httpp/server.hpp" namespace "httpp":
    cdef cppclass cpp_server "httpp::server":
        cpp_server() except +
        void serve_directory(string mount_path, string local_dir) except +
        int bind_to_any_port(string host) except +
        void listen_after_bind() except + nogil
        void listen(string host, int port) except + nogil
        void stop() except +


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
