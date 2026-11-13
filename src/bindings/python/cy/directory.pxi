# Static files from a folder: the Python face of httpp::directory (include/httpp/server.hpp, ADR-0066).

cdef class Directory:
    """A folder served as static files, with every setting a method that returns the object (ADR-0066).

        Directory("./public").set_index(False).mount(app, "/static")
        Directory("./public", show_hidden=False, download_active=True).mount(app, "/static")   # the same, with keywords

    `Server.serve_directory(path, folder)` is `Directory(folder).mount(app, path)` with the defaults. Range, conditional requests and
    HEAD work, large files stream, and nothing outside the folder is ever served. `mount` raises ValueError when `base` does not start
    with "/". The folder is read when requests arrive. `root` is a str, bytes or os.PathLike.
    """
    cdef cpp_directory* _d

    def __cinit__(self, root, *, index=None, show_hidden=None, download_active=None):
        self._d = new cpp_directory(_fs(root))
        if index is not None: self.set_index(index)
        if show_hidden is not None: self.set_show_hidden(show_hidden)
        if download_active is not None: self.set_download_active(download_active)

    def __dealloc__(self):
        if self._d != NULL:
            del self._d

    def set_index(self, bint on):
        """A path that ends in "/" (or the mount itself) answers with index.html from that folder. Default True."""
        self._d.set_index(on)
        return self

    def set_show_hidden(self, bint on):
        """False: a path with a segment that starts with "." is a 404, whatever is on disk. Default True."""
        self._d.set_show_hidden(on)
        return self

    def set_download_active(self, bint on):
        """True: send nosniff, and make a file that is not plainly harmless to show (or asked for with ?download=1) a download. Default False."""
        self._d.set_download_active(on)
        return self

    def mount(self, Server server, str base="/"):
        """Add the folder to `server` under `base` ("/static", or "/" for the whole server); runs entirely in C++ (no GIL)."""
        try:
            self._d.mount(server._srv[0], base.encode("utf-8"))
        except (RuntimeError, ValueError) as e:
            raise ValueError(str(e)) from None
        return None
