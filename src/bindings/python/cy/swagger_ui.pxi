# Swagger UI from the files compiled into libhttpp: the Python face of httpp::swagger_ui (include/httpp/swagger_ui.hpp, ADR-0066).

cdef class SwaggerUI:
    """Swagger UI served entirely in C++ (no GIL) from bytes compiled into the library.

        SwaggerUI().set_docs_path("/docs").set_openapi_url("/openapi.json").set_title("My API").mount(app)
        SwaggerUI(docs_path="/api/docs", title="My API").mount(app)   # the same style, with keywords

    You serve the OpenAPI document yourself (a Server with `docs_url` does both). `set_docs_path` raises ValueError unless the path starts
    with "/" and names more than the root, and then leaves the object unchanged.
    """
    cdef cpp_swagger_ui* _u

    def __cinit__(self, *, docs_path=None, openapi_url=None, title=None):
        self._u = new cpp_swagger_ui()
        if docs_path is not None: self.set_docs_path(docs_path)
        if openapi_url is not None: self.set_openapi_url(openapi_url)
        if title is not None: self.set_title(title)

    def __dealloc__(self):
        if self._u != NULL:
            del self._u

    def set_docs_path(self, str path):
        """Where the page lives; the three files are under it. Default "/docs"."""
        self._u.set_docs_path(path.encode("utf-8"))
        return self

    def set_openapi_url(self, str url):
        """Where the page loads the OpenAPI document from. Default "/openapi.json"."""
        self._u.set_openapi_url(url.encode("utf-8"))
        return self

    def set_title(self, str title):
        """Shown in the tab (escaped). Default "API"."""
        self._u.set_title(title.encode("utf-8"))
        return self

    def mount(self, Server server):
        """Add the page and its files to `server`."""
        self._u.mount(server._srv[0])
