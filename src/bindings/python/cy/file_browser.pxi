# FileBrowser: the Python face of httpp::file_browser (ADR-0065). Included by httpp_cy.pyx after server.pxi.

import enum as _enum


class _Theme(_enum.Enum):
    """httpp::file_browser::theme_mode (the numbers are checked against the C++ enum by a static_assert in cy/support.pxi)."""
    SYSTEM = 0
    LIGHT = 1
    DARK = 2


class _View(_enum.Enum):
    """httpp::file_browser::view_mode."""
    LIST = 0
    GRID = 1


class _Sort(_enum.Enum):
    """httpp::file_browser::sort_key."""
    NAME = 0
    SIZE = 1
    DATE = 2


cdef object _fb_member(object value, object enum_type, str what):
    if not isinstance(value, enum_type):
        raise TypeError("%s must be a FileBrowser.%s member, not %s" % (what, enum_type.__name__, type(value).__name__))
    return value.value


cdef class FileBrowser:
    """A read-only file browser for the folder `root`, mounted on a server with `mount(server, base)`.

        FileBrowser("/srv/shared").set_title("Shared").set_theme(FileBrowser.Theme.DARK).set_accent("#2a7de1").mount(app, "/files")
        FileBrowser("/srv/shared", title="Shared", theme=FileBrowser.Theme.DARK).mount(app, "/files")   # the same, with keywords

    The page, the routes and the security rules are those of httpp::file_browser (include/httpp/file_browser.hpp). A setter raises
    ValueError for a value it cannot use (an accent that is not #rrggbb, max_entries below 1) and then leaves the browser unchanged;
    `mount` raises ValueError for a bad base, a root that is not a folder, or the root of the file system. `root` is a str, bytes or
    os.PathLike, checked at mount time. One browser can be mounted more than once.
    """
    cdef cpp_file_browser* _b

    Theme = _Theme  # SYSTEM (follows the viewer's system setting), LIGHT, DARK
    View = _View    # LIST, GRID: the first view
    Sort = _Sort    # NAME, SIZE, DATE: the first sort

    def __cinit__(self, root, *, title=None, theme=None, accent=None, search=None, show_hidden=None, max_entries=None, view=None, sort=None):
        self._b = new cpp_file_browser(_fs(root))
        if title is not None: self.set_title(title)
        if theme is not None: self.set_theme(theme)
        if accent is not None: self.set_accent(accent)
        if search is not None: self.set_search(search)
        if show_hidden is not None: self.set_show_hidden(show_hidden)
        if max_entries is not None: self.set_max_entries(max_entries)
        if view is not None: self.set_view(view)
        if sort is not None: self.set_sort(sort)

    def __dealloc__(self):
        if self._b != NULL:
            del self._b

    def set_title(self, str title):
        """Shown in the page header and the tab (escaped, so any text is safe). Default "Files"."""
        self._b.set_title(title.encode("utf-8"))
        return self

    def set_theme(self, theme):
        """A FileBrowser.Theme member; the viewer can still switch it. Default SYSTEM."""
        fb_set_theme(self._b[0], _fb_member(theme, _Theme, "theme"))
        return self

    def set_accent(self, str colour):
        """"#rrggbb" (any case) or "" for the default colour. Default "". Raises ValueError for anything else."""
        self._b.set_accent(colour.encode("utf-8"))
        return self

    def set_search(self, bint on):
        """Show the filter box (it filters the folder that is open, in the browser). Default True."""
        self._b.set_search(on)
        return self

    def set_show_hidden(self, bint on):
        """False: names that start with '.' are not listed and answer 404 when asked for by name. Default False."""
        self._b.set_show_hidden(on)
        return self

    def set_max_entries(self, count):
        """A folder with more entries is cut at this many and flagged "truncated". Must be above 0. Default 5000."""
        if count < 1:
            raise ValueError("max_entries must be above 0")
        self._b.set_max_entries(<size_t>count)
        return self

    def set_view(self, view):
        """A FileBrowser.View member: the first view. Default LIST."""
        fb_set_view(self._b[0], _fb_member(view, _View, "view"))
        return self

    def set_sort(self, sort):
        """A FileBrowser.Sort member: the first sort. Default NAME."""
        fb_set_sort(self._b[0], _fb_member(sort, _Sort, "sort"))
        return self

    def mount(self, Server server, str base="/"):
        """Add the routes to `server` under `base` ("/files", or "/" for the whole server); runs entirely in C++ (no GIL)."""
        self._b.mount(server._srv[0], base.encode("utf-8"))
