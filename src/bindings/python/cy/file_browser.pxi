# FileBrowser: the Python face of httpp::file_browser (ADR-0032). Included by httpp_python.pyx after server.pxi.

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

        FileBrowser("/srv/shared").title("Shared").theme(FileBrowser.Theme.DARK).accent("#2a7de1").mount(app, "/files")
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

    def __cinit__(self, root, *, title=None, theme=None, accent=None, search=None, show_hidden=None, show_dates=None, max_entries=None, view=None, sort=None, safe_mode=None, zip=None, max_zip_entries=None, powered_by=None):
        self._b = new cpp_file_browser(_fs(root))
        if title is not None: self.title(title)
        if theme is not None: self.theme(theme)
        if accent is not None: self.accent(accent)
        if search is not None: self.search(search)
        if show_hidden is not None: self.show_hidden(show_hidden)
        if show_dates is not None: self.show_dates(show_dates)
        if max_entries is not None: self.max_entries(max_entries)
        if view is not None: self.layout(view)
        if sort is not None: self.sort_by(sort)
        if safe_mode is not None: self.safe_mode(safe_mode)
        if zip is not None: self.zip(zip)
        if max_zip_entries is not None: self.max_zip_entries(max_zip_entries)
        if powered_by is not None: self.powered_by(powered_by)

    def __dealloc__(self):
        if self._b != NULL:
            del self._b

    def title(self, str title):
        """Shown in the page header and the tab (escaped, so any text is safe). Default "Files"."""
        self._b.title(title.encode("utf-8"))
        return self

    def theme(self, theme):
        """A FileBrowser.Theme member; the viewer can still switch it. Default SYSTEM."""
        fb_set_theme(self._b[0], _fb_member(theme, _Theme, "theme"))
        return self

    def accent(self, str colour):
        """"#rrggbb" (any case) or "" for the default colour. Default "". Raises ValueError for anything else."""
        self._b.accent(colour.encode("utf-8"))
        return self

    def search(self, bint on):
        """Show the filter box (it filters the folder that is open, in the browser). Default True."""
        self._b.search(on)
        return self

    def show_hidden(self, bint on):
        """False: names that start with '.' are not listed and answer 404 when asked for by name. Default False."""
        self._b.show_hidden(on)
        return self

    def show_dates(self, bint on):
        """False: no file or folder time anywhere. The page has no "Modified" column and cannot sort by date, the listing has no "mtime", the files
        carry no Last-Modified or ETag, and a ZIP stamps every entry 1980-01-01. Default True."""
        self._b.show_dates(on)
        return self

    def max_entries(self, count):
        """A folder with more entries is cut at this many and flagged "truncated". Must be above 0. Default 5000."""
        if count < 1:
            raise ValueError("max_entries must be above 0")
        self._b.max_entries(<size_t>count)
        return self

    def layout(self, view):
        """A FileBrowser.View member: the first view. Default LIST."""
        fb_set_view(self._b[0], _fb_member(view, _View, "view"))
        return self

    def sort_by(self, sort):
        """A FileBrowser.Sort member: the first sort. Default NAME."""
        fb_set_sort(self._b[0], _fb_member(sort, _Sort, "sort"))
        return self

    def safe_mode(self, bint on):
        """False (default): "Open raw" renders html and svg in the new tab and the dialog shows an svg as an image (html stays text there), all
        behind a Content-Security-Policy sandbox that stops every script and every request to another origin; xml, js and unknown types stay
        downloads. True: html and svg are only downloaded too, nothing that could run code is ever shown."""
        self._b.safe_mode(on)
        return self

    def zip(self, bint on):
        """True: the page can select files and folders and download them as one ZIP (`base/api/zip`). A folder goes in with everything in it
        (hidden names only with show_hidden; links that lead out of the root, and links to folders, are left out). Nothing is compressed.
        Default False: a request can make the server read a lot of disk, so turn it on where the viewers are trusted or behind a `before` hook."""
        self._b.zip(on)
        return self

    def powered_by(self, bint on):
        """True (default): a small "Powered by httpp" at the bottom of the page, a link to the project on GitHub. False: no credit line."""
        self._b.powered_by(on)
        return self

    def max_zip_entries(self, count):
        """The most entries (files and folders together) one ZIP may hold; a request for more answers 413 before any byte is sent.
        Must be above 0. Default 50000."""
        if count <= 0:
            raise ValueError("max_zip_entries must be above 0")
        self._b.max_zip_entries(<size_t>count)
        return self

    def mount(self, Server server, str base="/"):
        """Add the routes to `server` under `base` ("/files", or "/" for the whole server); runs entirely in C++ (no GIL). Returns `server`."""
        self._b.mount(server._srv[0], base.encode("utf-8"))
        return server
