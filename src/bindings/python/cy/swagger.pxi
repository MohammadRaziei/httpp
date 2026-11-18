# Swagger UI from the files compiled into libhttpp: the Python face of httpp::swagger (include/httpp/swagger.hpp, ADR-0066, ADR-0082).

cdef class Swagger:
    """Swagger UI served entirely in C++ (no GIL) from bytes compiled into the library.

        Swagger("/openapi.json").set_title("My API").mount(app, "/docs")
        Swagger("/openapi.json", title="My API").mount(app)   # the same, with a keyword; base defaults to "/docs"

    `openapi_url` is where the page loads the OpenAPI document from. It is required: you serve that document yourself (a Server with
    `docs_url` does both), and it is used as it is. `mount` raises ValueError when `base` does not start with "/" or names only the
    root, and the constructor raises ValueError for an empty `openapi_url`. The same object can be mounted more than once.
    """
    cdef cpp_swagger* _u

    def __cinit__(self, str openapi_url, *, title=None):
        self._u = new cpp_swagger(openapi_url.encode("utf-8"))
        if title is not None: self.set_title(title)

    def __dealloc__(self):
        if self._u != NULL:
            del self._u

    def set_title(self, str title):
        """Shown in the tab (escaped). Default "API"."""
        self._u.set_title(title.encode("utf-8"))
        return self

    def mount(self, Server server, str base="/docs"):
        """Add the page and its files to `server`: the page at `base`, the files under it."""
        self._u.mount(server._srv[0], base.encode("utf-8"))
